#include "tb.h"
#include "content.h"
#include "dom.h"
#include "render.h"
#include "session.h"
#include "view.h"
#include <stdlib.h>
#include <string.h>

struct tb_browser {
  tb_config cfg;
  tb_session session;
  tb_dom *dom;
  tb_view *view;
  const tb_transport *transport;
};

/* ---- 导航上下文:把传输回调桥接到浏览器状态 ---- */
typedef struct {
  tb_browser *b;
  char *url;            /* 请求 URL */
  char *final_url;
  int status;
  char *content_type;
  int attachment;
  char *body; size_t body_len, body_cap;
  int done;
} nav_ctx;

static void nav_on_headers(void *ud, int status, const char *ct,
                           int attachment, const char *final_url) {
  nav_ctx *c = ud;
  c->status = status;
  free(c->content_type);
  c->content_type = ct ? strdup(ct) : NULL;
  c->attachment = attachment;
  free(c->final_url);
  c->final_url = final_url ? strdup(final_url) : NULL;
}

static void nav_on_body(void *ud, const char *data, size_t len) {
  nav_ctx *c = ud;
  if (c->body_cap < c->body_len + len + 1) {
    c->body_cap = (c->body_len + len + 1) * 2;
    c->body = realloc(c->body, c->body_cap);
  }
  memcpy(c->body + c->body_len, data, len);
  c->body_len += len;
  c->body[c->body_len] = '\0';
}

static void nav_on_done(void *ud, tb_err err) {
  nav_ctx *c = ud;
  tb_browser *b = c->b;
  c->done = 1;
  const char *effective = c->final_url ? c->final_url : c->url;
  if (err.code != 0) {
    tb_session_nav_fail(&b->session);
    if (b->cfg.on_error) b->cfg.on_error(b, err, c->url, b->cfg.ud);
    goto out;
  }
  tb_content_kind kind = tb_content_classify(c->content_type, c->attachment);
  if (kind == TB_CONTENT_RENDER) {
    tb_dom_free(b->dom);
    b->dom = tb_dom_parse(c->body, c->body_len);
    if (!b->dom) {
      tb_session_nav_fail(&b->session);
      tb_err pe = { TB_ERR_PARSE, "HTML parse failed" };
      if (b->cfg.on_error) b->cfg.on_error(b, pe, effective, b->cfg.ud);
      goto out;
    }
    tb_view_free(b->view);
    b->view = tb_render(b->dom, effective, c->status);
  } else {
    tb_dom_free(b->dom);
    b->dom = NULL;
    tb_view_free(b->view);
    b->view = tb_view_new();
    b->view->url = strdup(effective);
    b->view->status = c->status;
    b->view->is_not_renderable = 1;
    b->view->not_renderable_type = strdup(c->content_type ? c->content_type : "unknown");
  }
  tb_session_nav_done(&b->session, effective, tb_view_title(b->view), c->status);
  tb_session_touch_net(&b->session);
  if (b->cfg.on_title) b->cfg.on_title(b, tb_view_title(b->view), b->cfg.ud);
  if (b->cfg.on_view_changed) b->cfg.on_view_changed(b, b->cfg.ud);
out:
  free(c->url); free(c->final_url); free(c->content_type);
  free(c->body); free(c);
}

static tb_err do_navigate(tb_browser *b, const char *url, const char *method,
                          const char *body, const char *content_type) {
  if (!b || !url) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  tb_session_nav_start(&b->session, url);
  tb_session_touch_net(&b->session);

  nav_ctx *c = calloc(1, sizeof *c);
  c->b = b;
  c->url = strdup(url);

  tb_transport_req req = {0};
  req.method = method;
  req.url = url;
  req.body = body;
  req.on_headers = nav_on_headers;
  req.on_body = nav_on_body;
  req.on_done = nav_on_done;
  req.ud = c;
  (void)content_type;
  b->transport->open(b->transport, &req);
  tb_err ok = { 0, "" };
  return ok;
}

tb_err tb_navigate(tb_browser *b, const char *url) {
  return do_navigate(b, url, "GET", NULL, NULL);
}

/* ---- 生命周期 ---- */
tb_browser *tb_create(const tb_config *cfg) {
  tb_browser *b = calloc(1, sizeof *b);
  if (cfg) b->cfg = *cfg;
  if (!b->cfg.user_agent) b->cfg.user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
  if (!b->cfg.clock) b->cfg.clock = &tb_clock_real;
  if (!b->cfg.transport) {
    /* 真实 curl transport 在 Task 12 注入;此任务测试必须显式传 fake。
       生产路径由 Task 12 的 default_transport() 填充。 */
    tb_err e = { TB_ERR_ARG, "no transport configured (Task 12 wires default)" };
    free(b);
    return NULL;
  }
  b->transport = b->cfg.transport;
  tb_session_init(&b->session, b->cfg.clock, b->cfg.idle_grace_ms);
  return b;
}

void tb_destroy(tb_browser *b) {
  if (!b) return;
  tb_session_free(&b->session);
  tb_dom_free(b->dom);
  tb_view_free(b->view);
  free(b);
}

/* ---- 驱动 ---- */
int tb_pump(tb_browser *b, uint32_t timeout_ms) {
  if (!b) return 1;
  const tb_clock *clk = b->cfg.clock;
  uint64_t deadline = timeout_ms ? clk->now_ms(clk) + timeout_ms : 0;
  for (;;) {
    b->transport->poll(b->transport);
    if (tb_session_idle(&b->session)) return 0;
    if (deadline && clk->now_ms(clk) >= deadline) return 1;
  }
}

int tb_wait_idle(tb_browser *b, uint32_t timeout_ms) {
  return tb_pump(b, timeout_ms);
}

/* ---- 观察 ---- */
tb_err tb_observe(tb_browser *b, tb_view **out) {
  if (!b || !out) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!b->view) { tb_err e = { TB_ERR_NO_VIEW, "no view yet" }; return e; }
  *out = tb_view_clone(b->view);
  tb_err ok = { 0, "" };
  return ok;
}

/* ---- 占位(Task 11 填充真实现)---- */
tb_err tb_back(tb_browser *b) { (void)b; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
tb_err tb_forward(tb_browser *b) { (void)b; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
tb_err tb_reload(tb_browser *b) { (void)b; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
tb_err tb_click(tb_browser *b, int id) { (void)b; (void)id; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
tb_err tb_fill(tb_browser *b, int id, const char *v) { (void)b; (void)id; (void)v; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
tb_err tb_select(tb_browser *b, int id, const char *o) { (void)b; (void)id; (void)o; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
tb_err tb_submit(tb_browser *b, int form_id) { (void)b; (void)form_id; tb_err e = { TB_ERR_ARG, "TODO Task 11" }; return e; }
void tb_free(void *p) { free(p); }
