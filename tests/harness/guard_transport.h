#ifndef TB_GUARD_TRANSPORT_H
#define TB_GUARD_TRANSPORT_H
#include "tb.h"
#include <string.h>

/* 一个自包含的 transport,用于结构性断言「引擎工作不发生在传输回调栈内」。
 *
 * 背景:M2a 之前 nav_on_done 直接在传输回调里跑 load_document/render,而
 * <script src> 的抓取(tb_load_sync)会同步调 transport->open/poll。对 curl
 * 传输而言,此刻正坐在 curl_multi_info_read 的循环里 —— 在 curl 自己的栈上
 * 重入 curl_multi_add_handle,是 UB。
 *
 * 「测试没崩」证明不了这件事修好了:UB 的表现随版本、时序、栈布局变化,历史上
 * 它就曾以「页面照常渲染,只是脚本没跑」的形式静默存在。所以这里直接测量:
 * 回调执行期间若有人再调 open/poll,记为违规。
 *
 * 极简实现:open 立刻完成(下一个 poll 时同步派发),不排队、不延时。
 * 够用来观测「回调内是否重入」,不承担时序测试的职责。
 */
class guard_transport {
 public:
  tb_transport base{};
  /* 当前是否在传输回调栈内。 */
  int in_callback = 0;
  /* 回调期间发生的 open/poll 次数 —— 必须恒为 0。 */
  int reentrant_calls = 0;
  /* 累计派发的回调数,用来确认这条路径确实被走到(否则 reentrant_calls==0
     可能只是因为压根没触发子资源抓取,断言就没有说服力)。 */
  int callback_count = 0;

  /* open 时要返回给这个请求的内容。 */
  const char *body = nullptr;
  int status = 200;
  const char *content_type = "text/html";

  void init() {
    base.open = &wrap_open;
    base.cancel = &wrap_cancel;
    base.poll = &wrap_poll;
    base.destroy = nullptr;
  }

 private:
  static guard_transport *self(const tb_transport *t) {
    return const_cast<guard_transport *>(
        reinterpret_cast<const guard_transport *>(t));
  }

  /* 单个在飞请求:本 transport 不支持并发导航,够了。 */
  struct op { tb_transport_req req; bool active = false; };
  op op_{};

  static void *wrap_open(const tb_transport *t, const tb_transport_req *req) {
    guard_transport *g = self(t);
    if (g->in_callback) g->reentrant_calls++;
    g->op_.req = *req;
    g->op_.active = true;
    return &g->op_;
  }

  static void wrap_cancel(const tb_transport *t, void *opv) {
    guard_transport *g = self(t);
    if (g->in_callback) g->reentrant_calls++;
    if (opv) static_cast<op *>(opv)->active = false;
  }

  /* poll 同步派发回调,派发期间 in_callback 置位。 */
  static void wrap_poll(const tb_transport *t) {
    guard_transport *g = self(t);
    if (g->in_callback) g->reentrant_calls++;
    if (!g->op_.active) return;

    tb_transport_req req = g->op_.req;
    const char *body = g->body ? g->body : "";
    int status = g->status;
    const char *ct = g->content_type;

    g->op_.active = false;   /* 完成即收走:对齐 curl 的所有权语义 */
    g->callback_count++;
    g->in_callback++;
    if (req.on_headers) req.on_headers(req.ud, status, ct, 0, nullptr);
    if (req.on_body) req.on_body(req.ud, body, strlen(body));
    tb_err ok{0, ""};
    if (req.on_done) req.on_done(req.ud, ok);
    g->in_callback--;
  }
};

#endif /* TB_GUARD_TRANSPORT_H */