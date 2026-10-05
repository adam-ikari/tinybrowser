#ifndef TB_BROWSER_INTERNAL_H
#define TB_BROWSER_INTERNAL_H
#include "tb.h"
#include "session.h"
#include <uv.h>


struct tb_browser {
  tb_config cfg;
  tb_session session;
  tb_view *view;
  const tb_transport *transport;
  uv_loop_t loop;
  int loop_init;        /* 1 = 默认 curl transport + 自有 uv loop */
  const struct tb_js_engine *engine; /* 实际 engine:cfg.js_engine 或 tb_default_js_engine() */
  void *js_doc;         /* 引擎句柄(engine->open 返回);tb_destroy 负责 close */
  char *cookie_jar;     /* document.cookie 往返(M2a 单串) */
  /* 传输已完成、等待在 pump 顶层提交引擎工作的导航队列(单链表)。
     见 browser.c nav_ctx 的注释:传输回调只登记,不跑引擎。 */
  struct nav_ctx *nav_ready, *nav_ready_tail;
  /* 已 open 但 on_done 尚未触发的导航。tb_destroy 要回收它们:调用方
     完全可能在导航途中直接销毁浏览器,那时没人会替 on_done 收尾。 */
  struct nav_ctx *nav_inflight;
};

/* 子资源同步加载:nested pump。*out = strdup 或空串,调用方 free。 */
tb_err tb_load_sync(tb_browser *b, const char *url, char **out);

#endif
