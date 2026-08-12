#include "tb.h"
#include "content.h"
#include "curl_transport.h"
#include "dom.h"
#include "render.h"
#include "session.h"
#include "url.h"
#include "view.h"
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int id; char *value; } fv_t;

struct tb_browser {
  tb_config cfg;
  tb_session session;
  tb_dom *dom;
  tb_view *view;
  const tb_transport *transport;
  fv_t fv[64];          /* 表单值(由 tb_fill/tb_select 写入),M1 固定槽 */
  int nfv;
  uv_loop_t loop;
  int loop_init;        /* 1 = 默认 curl transport + 自有 uv loop */
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

/* replace=1 时不再压栈(back/forward/reload 已由 session 管理栈) */
static tb_err do_navigate(tb_browser *b, const char *url, const char *method,
                          const char *body, const char *content_type, int replace) {
  if (!b || !url) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!replace) tb_session_nav_start(&b->session, url);
  tb_session_touch_net(&b->session);

  nav_ctx *c = calloc(1, sizeof *c);
  c->b = b;
  c->url = strdup(url);

  tb_transport_req req = {0};
  req.method = method;
  /* 请求 URL 借用 nav_ctx 的副本:调用方可能立刻释放参数(如 click 的 resolve 结果),
     而传输层在 op 存活期间持有该指针。 */
  req.url = c->url;
  req.body = body;
  req.content_type = content_type;
  req.on_headers = nav_on_headers;
  req.on_body = nav_on_body;
  req.on_done = nav_on_done;
  req.ud = c;
  b->transport->open(b->transport, &req);
  tb_err ok = { 0, "" };
  return ok;
}

tb_err tb_navigate(tb_browser *b, const char *url) {
  return do_navigate(b, url, "GET", NULL, NULL, 0);
}

/* ---- 表单值存储 ---- */
static fv_t *find_fv(tb_browser *b, int id, int create) {
  for (int i = 0; i < b->nfv; i++) if (b->fv[i].id == id) return &b->fv[i];
  if (create && b->nfv < 64) {
    b->fv[b->nfv].id = id;
    b->fv[b->nfv].value = NULL;
    return &b->fv[b->nfv++];
  }
  return NULL;
}

/* ---- DOM 遍历辅助 ---- */
static tb_node *find_node_by_id(tb_browser *b, int id, const char **tag_out) {
  tb_node *root = tb_dom_root(b->dom);
  tb_node *found = NULL;
  struct { tb_node *n; } stack[4096]; int sp = 0;
  stack[sp++].n = root;
  while (sp) {
    tb_node *n = stack[--sp].n;
    if (!n) continue;
    if (tb_dom_is_element(n) && tb_dom_id(b->dom, n) == id) {
      found = n;
      if (tag_out) *tag_out = tb_dom_tag(n);
      break;
    }
    for (tb_node *ch = tb_dom_first_child(n); ch; ch = tb_dom_next_sibling(ch))
      if (sp < 4096) stack[sp++].n = ch;
  }
  return found;
}

static int node_in_subtree(tb_node *root, tb_node *target) {
  struct { tb_node *n; } st[4096]; int sp = 0;
  for (tb_node *ch = tb_dom_first_child(root); ch; ch = tb_dom_next_sibling(ch))
    if (sp < 4096) st[sp++].n = ch;
  while (sp) {
    tb_node *y = st[--sp].n;
    if (y == target) return 1;
    for (tb_node *ch = tb_dom_first_child(y); ch; ch = tb_dom_next_sibling(ch))
      if (sp < 4096) st[sp++].n = ch;
  }
  return 0;
}

static tb_node *find_containing_form(tb_browser *b, tb_node *n) {
  struct { tb_node *n; } st[4096]; int sp = 0;
  st[sp++].n = tb_dom_root(b->dom);
  while (sp) {
    tb_node *x = st[--sp].n;
    if (!x) continue;
    if (tb_dom_is_element(x) && strcmp(tb_dom_tag(x), "form") == 0 &&
        node_in_subtree(x, n))
      return x;
    for (tb_node *ch = tb_dom_first_child(x); ch; ch = tb_dom_next_sibling(ch))
      if (sp < 4096) st[sp++].n = ch;
  }
  return NULL;
}

/* ---- 交互 ---- */
tb_err tb_click(tb_browser *b, int id) {
  if (!b) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  const char *tag = NULL;
  tb_node *n = find_node_by_id(b, id, &tag);
  if (!n) { tb_err e = { TB_ERR_NO_ELEM, "no such element" }; return e; }
  if (strcmp(tag, "a") == 0) {
    const char *href = tb_dom_attr(n, "href");
    if (!href) { tb_err e = { TB_ERR_ARG, "link has no href" }; return e; }
    char *target = tb_url_resolve(tb_session_url(&b->session), href);
    if (!target) { tb_err e = { TB_ERR_ARG, "bad href" }; return e; }
    tb_err r = do_navigate(b, target, "GET", NULL, NULL, 0);
    tb_free(target);
    return r;
  }
  if (strcmp(tag, "button") == 0) {
    tb_node *form = find_containing_form(b, n);
    if (!form) { tb_err e = { TB_ERR_NO_ELEM, "button not in form" }; return e; }
    return tb_submit(b, tb_dom_id(b->dom, form));
  }
  tb_err e = { TB_ERR_ARG, "element not clickable" };
  return e;
}

tb_err tb_fill(tb_browser *b, int id, const char *value) {
  if (!b || !value) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  const char *tag = NULL;
  tb_node *n = find_node_by_id(b, id, &tag);
  if (!n) { tb_err e = { TB_ERR_NO_ELEM, "no such element" }; return e; }
  if (strcmp(tag, "input") != 0) { tb_err e = { TB_ERR_ARG, "not an input" }; return e; }
  fv_t *f = find_fv(b, id, 1);
  free(f->value);
  f->value = strdup(value);
  tb_err ok = { 0, "" };
  return ok;
}

tb_err tb_select(tb_browser *b, int id, const char *option) {
  if (!b || !option) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  const char *tag = NULL;
  tb_node *n = find_node_by_id(b, id, &tag);
  if (!n) { tb_err e = { TB_ERR_NO_ELEM, "no such element" }; return e; }
  if (strcmp(tag, "select") != 0) { tb_err e = { TB_ERR_ARG, "not a select" }; return e; }
  /* 校验 option 存在 */
  int okv = 0;
  for (tb_node *ch = tb_dom_first_child(n); ch; ch = tb_dom_next_sibling(ch)) {
    if (tb_dom_is_element(ch) && strcmp(tb_dom_tag(ch), "option") == 0) {
      const char *txt = tb_dom_text(tb_dom_first_child(ch));
      if (txt && strcmp(txt, option) == 0) { okv = 1; break; }
    }
  }
  if (!okv) { tb_err e = { TB_ERR_ARG, "no such option" }; return e; }
  fv_t *f = find_fv(b, id, 1);
  free(f->value);
  f->value = strdup(option);
  tb_err ok = { 0, "" };
  return ok;
}

/* 把 parent 的子节点按文档序压入栈(先压最后一个,使第一个在栈顶) */
static void push_children_doc_order(tb_node *parent, struct { tb_node *n; } *st, int *sp) {
  tb_node *kids[4096]; int nk = 0;
  for (tb_node *ch = tb_dom_first_child(parent); ch && nk < 4096; ch = tb_dom_next_sibling(ch))
    kids[nk++] = ch;
  while (nk > 0 && *sp < 4096) st[(*sp)++].n = kids[--nk];
}

/* 收集 form 内所有命名控件 name=value(已 urlencode),返回 query 片段 */
static char *collect_form_pairs(tb_browser *b, tb_node *form) {
  size_t cap = 256, len = 0;
  char *out = malloc(cap);
  out[0] = '\0';
  struct { tb_node *n; } st[4096]; int sp = 0;
  push_children_doc_order(form, st, &sp);
  while (sp) {
    tb_node *n = st[--sp].n;
    if (tb_dom_is_element(n)) {
      const char *tag = tb_dom_tag(n);
      const char *name = tb_dom_attr(n, "name");
      if (name && (strcmp(tag, "input") == 0 || strcmp(tag, "select") == 0)) {
        fv_t *f = find_fv(b, tb_dom_id(b->dom, n), 0);
        const char *val = f ? f->value : tb_dom_attr(n, "value");
        if (!val) val = "";
        char *en = tb_url_encode(name);
        char *ev = tb_url_encode(val);
        int need = (int)strlen(en) + (int)strlen(ev) + 8;
        if (len + (size_t)need > cap) { cap = (len + (size_t)need) * 2; out = realloc(out, cap); }
        if (len) out[len++] = '&';
        strcpy(out + len, en); len += strlen(en);
        out[len++] = '=';
        strcpy(out + len, ev); len += strlen(ev);
        free(en); free(ev);
      }
    }
    push_children_doc_order(n, st, &sp);
  }
  out[len] = '\0';
  return out;
}

tb_err tb_submit(tb_browser *b, int form_id) {
  if (!b) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  const char *tag = NULL;
  tb_node *form = find_node_by_id(b, form_id, &tag);
  if (!form || strcmp(tag, "form") != 0) { tb_err e = { TB_ERR_NO_ELEM, "no such form" }; return e; }
  const char *action = tb_dom_attr(form, "action");
  const char *base = tb_session_url(&b->session);
  char *target = tb_url_resolve(base ? base : "http://localhost/", action ? action : "");
  if (!target) { tb_err e = { TB_ERR_ARG, "bad action" }; return e; }
  char *pairs = collect_form_pairs(b, form);
  const char *method = tb_dom_attr(form, "method");
  int is_post = method && strcmp(method, "post") == 0;
  tb_err r;
  if (is_post) {
    r = do_navigate(b, target, "POST", pairs, "application/x-www-form-urlencoded", 0);
  } else {
    /* GET:追加 query */
    size_t n = strlen(target) + strlen(pairs) + 2;
    char *withq = malloc(n);
    snprintf(withq, n, "%s?%s", target, pairs);
    r = do_navigate(b, withq, "GET", NULL, NULL, 0);
    tb_free(withq);
  }
  tb_free(target);
  tb_free(pairs);
  return r;
}

/* ---- 历史导航 ---- */
tb_err tb_back(tb_browser *b) {
  if (!b) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!tb_session_can_back(&b->session)) { tb_err e = { TB_ERR_ARG, "no history" }; return e; }
  tb_session_back(&b->session);
  return do_navigate(b, tb_session_url(&b->session), "GET", NULL, NULL, 1);
}
tb_err tb_forward(tb_browser *b) {
  if (!b) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!tb_session_can_fwd(&b->session)) { tb_err e = { TB_ERR_ARG, "no history" }; return e; }
  tb_session_forward(&b->session);
  return do_navigate(b, tb_session_url(&b->session), "GET", NULL, NULL, 1);
}
tb_err tb_reload(tb_browser *b) {
  if (!b || !tb_session_url(&b->session)) { tb_err e = { TB_ERR_ARG, "no current page" }; return e; }
  return do_navigate(b, tb_session_url(&b->session), "GET", NULL, NULL, 1);
}

/* ---- 生命周期 ---- */
tb_browser *tb_create(const tb_config *cfg) {
  tb_browser *b = calloc(1, sizeof *b);
  if (cfg) b->cfg = *cfg;
  if (!b->cfg.user_agent) b->cfg.user_agent = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
  if (!b->cfg.clock) b->cfg.clock = &tb_clock_real;
  if (!b->cfg.transport) {
    b->loop_init = uv_loop_init(&b->loop) == 0;
    if (b->loop_init) {
      b->cfg.transport = tb_curl_transport_create(&b->loop, &b->cfg);
    } else {
      tb_err e = { TB_ERR_NET, "uv_loop_init failed" };
      free(b);
      return NULL;
    }
  }
  b->transport = b->cfg.transport;
  tb_session_init(&b->session, b->cfg.clock, b->cfg.idle_grace_ms);
  return b;
}

void tb_destroy(tb_browser *b) {
  if (!b) return;
  if (b->loop_init) uv_loop_close(&b->loop);
  tb_session_free(&b->session);
  tb_dom_free(b->dom);
  tb_view_free(b->view);
  for (int i = 0; i < b->nfv; i++) free(b->fv[i].value);
  free(b);
}

/* ---- 驱动 ---- */
int tb_pump(tb_browser *b, uint32_t timeout_ms) {
  if (!b) return 1;
  const tb_clock *clk = b->cfg.clock;
  uint64_t deadline = timeout_ms ? clk->now_ms(clk) + timeout_ms : 0;
  for (;;) {
    if (b->loop_init) uv_run(&b->loop, UV_RUN_NOWAIT);
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

void tb_free(void *p) { free(p); }
