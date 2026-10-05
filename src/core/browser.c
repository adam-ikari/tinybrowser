#include "tb.h"
#include "browser_internal.h"
#include "content.h"
#include "curl_transport.h"
#include "session.h"
#include "url.h"
#include "view.h"
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "js_engine.h"

/* Task 5: tb_default_js_engine 现在由 js_engine.c 提供真实实现，
   这里不再需要 stub。但 tb.h 声明在 js_engine.h 中，
   browser.c 通过 js_engine.h 拿到它。 */

/* Task 5: tb_eval_js 真实现 */
tb_err tb_eval_js(tb_browser *b, const char *code, char **out) {
  if (!b || !b->engine || !b->js_doc) {
    tb_err e = { TB_ERR_NO_VIEW, "no document" };
    return e;
  }
  int rc = b->engine->eval(b->engine, b->js_doc, code, out);
  if (rc != 0) {
    tb_err e = { TB_ERR_PARSE, "eval failed" };
    return e;
  }
  tb_err ok = { 0, "" };
  return ok;
}

/* ---- 子资源同步获取:nested pump (Task 5) ---- */

/* transport 回调上下文 (tb_load_sync 内部用) */
typedef struct {
  int done;
  int failed;
  char *body;
  size_t blen, bcap;
} sync_ctx;

static void sync_on_headers(void *ud, int status, const char *ct,
                            int attachment, const char *final_url) {
  (void)ud; (void)status; (void)ct; (void)attachment; (void)final_url;
}

static void sync_on_body(void *ud, const char *data, size_t len) {
  sync_ctx *c = (sync_ctx *)ud;
  if (c->bcap < c->blen + len + 1) {
    c->bcap = (c->blen + len + 1) * 2;
    c->body = realloc(c->body, c->bcap);
  }
  memcpy(c->body + c->blen, data, len);
  c->blen += len;
  c->body[c->blen] = '\0';
}

static void sync_on_done(void *ud, tb_err err) {
  sync_ctx *c = (sync_ctx *)ud;
  c->done = 1;
  if (err.code != 0) { c->failed = 1; }
}

tb_err tb_load_sync(tb_browser *b, const char *url, char **out) {
  if (!b || !url || !out) {
    if (out) *out = strdup("");
    tb_err e = { TB_ERR_ARG, "bad args" };
    return e;
  }
  sync_ctx ctx; memset(&ctx, 0, sizeof ctx);

  tb_transport_req req; memset(&req, 0, sizeof req);
  req.method = "GET";
  req.url = url;
  req.on_headers = sync_on_headers;
  req.on_body    = sync_on_body;
  req.on_done    = sync_on_done;
  req.ud         = &ctx;

  void *op = b->transport->open(b->transport, &req);
  if (!op) { *out = strdup(""); tb_err e = { TB_ERR_NET, "open failed" }; return e; }

  const tb_clock *clk = b->cfg.clock;
  uint64_t t0 = clk->now_ms(clk);
  uint32_t timeout = b->cfg.nav_timeout_ms ? b->cfg.nav_timeout_ms : 30000;

  while (!ctx.done && !ctx.failed) {
    if (b->loop_init) uv_run(&b->loop, UV_RUN_NOWAIT);
    b->transport->poll(b->transport);
    if (clk->now_ms(clk) - t0 > timeout) break;
  }
  /* 只有还在飞的 op 才 cancel。传输层的所有权约定是「on_done 一触发,传输层
   * 就把 op 收走并释放」(curl_transport.c check_multi_info 在派发 on_done 之后
   * 立即 free(op))。此前的代码无条件 cancel,于是拿着已释放的 op 去
   * curl_multi_remove_handle —— use-after-free,真 curl 传输下必崩
   * (<script src> 才会走到这条路径,所以单测全绿、集成一跑就 segfault)。
   *
   * fake transport 掩盖了它:fake_cancel 在 op 链表里找不到就静默返回,所以
   * 单元测试永远看不到这处崩溃。别把「fake 不炸」当契约。 */
  if (!ctx.done && !ctx.failed) b->transport->cancel(b->transport, op);

  *out = ctx.body ? ctx.body : strdup("");
  return (ctx.done && !ctx.failed) ? (tb_err){0, ""} : (tb_err){TB_ERR_TIMEOUT, "timeout"};
}

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
    /* 引擎驱动(M2a):qzjs 解析 HTML、跑脚本、产出视图。不再有 C 侧 DOM 树——
     * 交互(click/fill/select/submit)改为经控制面在 JS 自己的树上查询。 */
    if (b->engine->load_document(b->engine, b->js_doc, c->body, c->body_len, effective) != 0) {
      tb_session_nav_fail(&b->session);
      tb_err pe = { TB_ERR_PARSE, "document load failed" };
      if (b->cfg.on_error) b->cfg.on_error(b, pe, effective, b->cfg.ud);
      goto out;
    }
    tb_view_free(b->view);
    b->view = tb_view_new();
    if (b->engine->render(b->engine, b->js_doc, effective, c->status, b->view) != 0) {
      tb_session_nav_fail(&b->session);
      tb_err pe = { TB_ERR_PARSE, "render failed" };
      if (b->cfg.on_error) b->cfg.on_error(b, pe, effective, b->cfg.ud);
      goto out;
    }
  } else {
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

/* 表单值的宿主侧存储(fv 表)已删:tb_fill/tb_select 现在写进 JS 的 __tb_fv,
 * 提交时由 _tb_form_pairs 读出,不再需要宿主把值搬一遍。 */

/* ---- 引擎查询辅助 ----
 * 交互不再走 C DOM 树(find_node_by_id / collect_form_pairs 已删):节点查找、
 * form 结构、控件枚举全部在 JS 侧,宿主经控制面 eval 取回 JSON。
 * 下面的 js_* 把「发表达式 → 拿 JSON 文本」这件事收在一处。 */

/* 从一段 JSON 文本里取顶层字符串字段的值(已反转义),写入 out。
 * 只处理我们自己的 _tb_elem_info / _tb_form_info 输出:形状固定、值都是短
 * 字符串或数字。嵌套对象/数组不解析(那些交给 JS 侧判断)。
 * 字段不存在 → out[0]=0,返回 0。 */
static int js_json_field(const char *json, const char *key, char *out, size_t cap) {
  if (out && cap) out[0] = '\0';
  if (!json || !key || !out || !cap) return 0;
  char pat[64];
  snprintf(pat, sizeof pat, "\"%s\":", key);
  const char *p = strstr(json, pat);
  if (!p) return 0;
  p += strlen(pat);
  while (*p == ' ') p++;
  if (*p != '"') {
    /* 数字 / true / false / null:原样拷贝到分隔符 */
    size_t o = 0;
    while (*p && *p != ',' && *p != '}' && o < cap - 1) out[o++] = *p++;
    out[o] = '\0';
    return 1;
  }
  p++;
  size_t o = 0;
  while (*p && o < cap - 1) {
    if (*p == '\\' && p[1]) {
      p++;
      switch (*p) {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        default: out[o++] = *p; break;   /* \uXXXX 退化为原字节 */
      }
      p++;
      continue;
    }
    if (*p == '"') break;
    out[o++] = *p++;
  }
  out[o] = '\0';
  return 1;
}

/* 同上,取整数字段。 */
static int js_json_int_field(const char *json, const char *key, int *out) {
  char buf[32];
  if (!js_json_field(json, key, buf, sizeof buf)) return 0;
  if (!buf[0]) return 0;
  *out = atoi(buf);
  return 1;
}

/* 把字符串转义成可嵌入 JS 双引号字面量的形式(含外层引号)。
 * 放不下返回 0。 */
static int js_escape_into(char *out, size_t cap, const char *s) {
  size_t o = 0;
  if (cap < 3) return 0;
  out[o++] = '"';
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    char esc[8];
    const char *rep = esc;
    size_t replen = 1;
    switch (*p) {
      case '"':  rep = "\\\""; replen = 2; break;
      case '\\': rep = "\\\\"; replen = 2; break;
      case '\n': rep = "\\n";  replen = 2; break;
      case '\r': rep = "\\r";  replen = 2; break;
      case '\t': rep = "\\t";  replen = 2; break;
      default:
        if (*p < 0x20) {
          snprintf(esc, sizeof esc, "\\u%04x", *p);
          replen = 6;
        } else {
          esc[0] = (char)*p;
          esc[1] = '\0';
          replen = 1;
        }
    }
    if (o + replen + 2 > cap) return 0;
    memcpy(out + o, rep, replen);
    o += replen;
  }
  out[o++] = '"';
  out[o] = '\0';
  return 1;
}

/* _tb_form_pairs 的输出 {"ok":true,"pairs":[{id,name,value}...]} → urlencoded
 * query 片段。数组元素在 JS 侧已是固定顺序,这里按 "pairs": 之后逐个对象取
 * name/value。取不到就跳过该对(宁可少字段,不产出坏的 query)。 */
static char *js_pairs_to_query(const char *json) {
  size_t cap = 256, len = 0;
  char *out = malloc(cap);
  if (!out) return NULL;
  out[0] = '\0';
  const char *p = json ? strstr(json, "\"pairs\":") : NULL;
  if (!p) return out;
  p += 8;
  for (;;) {
    const char *ob = strchr(p, '{');
    if (!ob) break;
    const char *oe = strchr(ob, '}');
    if (!oe) break;
    /* 截出一个对象字面量,交给 js_json_field 取字段 */
    size_t n = (size_t)(oe - ob) + 1;
    char *obj = malloc(n + 1);
    if (!obj) break;
    memcpy(obj, ob, n);
    obj[n] = '\0';
    char name[512] = {0}, value[2048] = {0};
    if (js_json_field(obj, "name", name, sizeof name) && name[0] &&
        js_json_field(obj, "value", value, sizeof value)) {
      char *en = tb_url_encode(name);
      char *ev = tb_url_encode(value);
      if (en && ev) {
        size_t need = strlen(en) + strlen(ev) + 4;
        if (len + need > cap) { cap = (len + need) * 2; out = realloc(out, cap); }
        if (len) out[len++] = '&';
        strcpy(out + len, en); len += strlen(en);
        out[len++] = '=';
        strcpy(out + len, ev); len += strlen(ev);
      }
      free(en); free(ev);
    }
    free(obj);
    p = oe + 1;
  }
  out[len] = '\0';
  return out;
}

/* 在引擎里求值一个返回 JSON 字符串的表达式,结果 strdup 给调用方。
 * 失败返回 NULL。 */
static char *js_query(tb_browser *b, const char *expr) {
  if (!b || !b->js_doc || !b->engine) return NULL;
  char *out = NULL;
  if (b->engine->eval(b->engine, b->js_doc, expr, &out) != 0) {
    free(out);
    return NULL;
  }
  return out;   /* 可能是 "null" —— 调用方按需判 */
}

/* 求值一个返回 boolean 的表达式;成功返回 0/1,失败返回 -1。 */
static int js_query_bool(tb_browser *b, const char *expr) {
  char *r = js_query(b, expr);
  if (!r) return -1;
  int v = (strcmp(r, "true") == 0) ? 1 : 0;
  free(r);
  return v;
}

/* ---- 交互 ---- */
tb_err tb_click(tb_browser *b, int id) {
  if (!b) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!b->js_doc) { tb_err e = { TB_ERR_NO_VIEW, "no document" }; return e; }
  char expr[128];
  /* _tb_elem_info 返回 {tag, href?, type?, form?, options?} 的 JSON 串 */
  snprintf(expr, sizeof expr, "_tb_elem_info(%d)", id);
  char *info = js_query(b, expr);
  if (!info) { tb_err e = { TB_ERR_NO_ELEM, "element query failed" }; return e; }

  /* 从 JSON 里取字段。这里不引 cJSON:tb_elem_info 的输出是我们自己写的,
   * 形状固定,用轻量取键即可(值里的转义由 JSON 保证,此处只取短字段)。 */
  char tag[32] = {0};
  char href[1024] = {0};
  char itype[32] = {0};
  int form_id = 0;
  js_json_field(info, "tag", tag, sizeof tag);
  if (strcmp(tag, "a") == 0) {
    js_json_field(info, "href", href, sizeof href);
    if (href[0] == '\0') { free(info); tb_err e = { TB_ERR_ARG, "link has no href" }; return e; }
    char *target = tb_url_resolve(tb_session_url(&b->session), href);
    free(info);
    if (!target) { tb_err e = { TB_ERR_ARG, "bad href" }; return e; }
    tb_err r = do_navigate(b, target, "GET", NULL, NULL, 0);
    tb_free(target);
    return r;
  }
  int is_button_submit = 0;
  if (strcmp(tag, "button") == 0) {
    is_button_submit = 1;
  } else if (strcmp(tag, "input") == 0) {
    js_json_field(info, "type", itype, sizeof itype);
    if (strcmp(itype, "submit") == 0 || strcmp(itype, "button") == 0 ||
        strcmp(itype, "reset") == 0) is_button_submit = 1;
  }
  if (is_button_submit) {
    /* form id:button 由 _tb_form_of 给出;input 由 JS 补查 */
    if (!js_json_int_field(info, "form", &form_id)) {
      snprintf(expr, sizeof expr, "String(_tb_form_of(_tb_node_by_id(%d)) || 0)", id);
      char *fs = js_query(b, expr);
      form_id = fs ? atoi(fs) : 0;
      free(fs);
    }
    free(info);
    if (form_id <= 0) { tb_err e = { TB_ERR_NO_ELEM, "button not in form" }; return e; }
    return tb_submit(b, form_id);
  }
  free(info);
  tb_err e = { TB_ERR_ARG, "element not clickable" };
  return e;
}

/* node_in_subtree / find_containing_form 已删:form 归属改由 JS 侧
 * _tb_form_of 沿 parent 链求出(它与渲染 id 同域,无需 C 树)。 */

/* ---- 交互 ---- */
tb_err tb_fill(tb_browser *b, int id, const char *value) {
  if (!b || !value) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!b->js_doc) { tb_err e = { TB_ERR_NO_VIEW, "no document" }; return e; }
  char expr[128];
  snprintf(expr, sizeof expr, "_tb_elem_info(%d)", id);
  char *info = js_query(b, expr);
  if (!info) { tb_err e = { TB_ERR_NO_ELEM, "element query failed" }; return e; }
  char tag[32] = {0};
  js_json_field(info, "tag", tag, sizeof tag);
  if (strcmp(tag, "input") != 0) {
    free(info);
    tb_err e = { TB_ERR_ARG, "not an input" };
    return e;
  }
  free(info);
  /* 值写进 JS 侧 __tb_fv;提交时由 _tb_form_pairs 读出。 */
  char esc[2048];
  if (!js_escape_into(esc, sizeof esc, value)) {
    tb_err e = { TB_ERR_ARG, "value too long" };
    return e;
  }
  snprintf(expr, sizeof expr, "String(_tb_set_value(%d, %s))", id, esc);
  int rc = js_query_bool(b, expr);
  if (rc < 0) { tb_err e = { TB_ERR_PARSE, "set value failed" }; return e; }
  tb_err ok = { 0, "" };
  return ok;
}

tb_err tb_select(tb_browser *b, int id, const char *option) {
  if (!b || !option) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!b->js_doc) { tb_err e = { TB_ERR_NO_VIEW, "no document" }; return e; }
  char expr[128];
  snprintf(expr, sizeof expr, "_tb_elem_info(%d)", id);
  char *info = js_query(b, expr);
  if (!info) { tb_err e = { TB_ERR_NO_ELEM, "element query failed" }; return e; }
  char tag[32] = {0};
  js_json_field(info, "tag", tag, sizeof tag);
  if (strcmp(tag, "select") != 0) {
    free(info);
    tb_err e = { TB_ERR_ARG, "not a select" };
    return e;
  }
  free(info);

  /* option 成员判定放在 JS 侧(_tb_has_option):option 可能被 optgroup 包住,
   * 需要遍历子树,把它写成宿主侧的表达式既难读又容易撑爆表达式缓冲。 */
  char qopt[1024];
  if (!js_escape_into(qopt, sizeof qopt, option)) {
    tb_err e = { TB_ERR_ARG, "option too long" };
    return e;
  }
  snprintf(expr, sizeof expr, "String(_tb_has_option(%d, %s))", id, qopt);
  int found = js_query_bool(b, expr);
  if (found <= 0) { tb_err e = { TB_ERR_ARG, "no such option" }; return e; }

  char esc[2048];
  if (!js_escape_into(esc, sizeof esc, option)) {
    tb_err e = { TB_ERR_ARG, "option too long" };
    return e;
  }
  snprintf(expr, sizeof expr, "String(_tb_set_value(%d, %s))", id, esc);
  int rc = js_query_bool(b, expr);
  if (rc < 0) { tb_err e = { TB_ERR_PARSE, "set value failed" }; return e; }
  tb_err ok = { 0, "" };
  return ok;
}

/* push_children_doc_order / collect_form_pairs 已删:控件枚举与取值改由
 * JS 侧 _tb_form_pairs 完成(它能直接看到 __tb_fv 里 tb_fill/tb_select 写的值,
 * 不需要宿主把 C 侧 fv 表再搬一遍)。 */

tb_err tb_submit(tb_browser *b, int form_id) {
  if (!b) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!b->js_doc) { tb_err e = { TB_ERR_NO_VIEW, "no document" }; return e; }

  /* form 的 action/method 来自 JS 侧 _tb_form_info */
  char expr[128];
  snprintf(expr, sizeof expr, "_tb_form_info(%d)", form_id);
  char *finfo = js_query(b, expr);
  if (!finfo) { tb_err e = { TB_ERR_NO_ELEM, "form query failed" }; return e; }
  char ok[16] = {0}, action[1024] = {0}, method[16] = {0};
  js_json_field(finfo, "ok", ok, sizeof ok);
  if (strcmp(ok, "true") != 0) {
    free(finfo);
    tb_err e = { TB_ERR_NO_ELEM, "no such form" };
    return e;
  }
  js_json_field(finfo, "action", action, sizeof action);
  js_json_field(finfo, "method", method, sizeof method);
  free(finfo);

  const char *base = tb_session_url(&b->session);
  char *target = tb_url_resolve(base ? base : "http://localhost/", action[0] ? action : "");
  if (!target) { tb_err e = { TB_ERR_ARG, "bad action" }; return e; }

  /* 控件对由 JS 侧 _tb_form_pairs 收集(已按文档序、已合并 tb_fill/tb_select 的值) */
  snprintf(expr, sizeof expr, "_tb_form_pairs(%d)", form_id);
  char *pj = js_query(b, expr);
  char *pairs = pj ? js_pairs_to_query(pj) : strdup("");
  free(pj);
  if (!pairs) pairs = strdup("");

  int is_post = strcmp(method, "post") == 0;
  tb_err r;
  if (is_post) {
    r = do_navigate(b, target, "POST", pairs, "application/x-www-form-urlencoded", 0);
  } else {
    size_t n = strlen(target) + strlen(pairs) + 3;
    char *withq = malloc(n);
    /* pairs 为空时不要留一个裸 "?" */
    if (pairs[0]) snprintf(withq, n, "%s?%s", target, pairs);
    else snprintf(withq, n, "%s", target);
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
  b->engine = b->cfg.js_engine ? b->cfg.js_engine : tb_default_js_engine();
  /* 引擎在 create 时就起来(而非首次导航时):tb_fill/tb_select/tb_submit 直接
   * 经控制面向 JS 树查询,若 js_doc 只在 nav_on_done 里建,这些 API 在导航前
   * (以及渲染失败后)就没有可问的对象。qzjs 的 runtime 拥有自己的线程,
   * 提前建立也让首次导航少一次冷启动。 */
  b->js_doc = b->engine->open(b->engine, b);
  if (!b->js_doc) {
    tb_err e = { TB_ERR_PARSE, "JS engine failed to start" };
    /* 失败路径同样要收拾自己建出来的 transport,否则 curl multi 泄漏。
     * 顺序同 tb_destroy:transport 的 destroy 要先跑 loop。 */
    if (b->loop_init) {
      if (b->transport && b->transport->destroy) b->transport->destroy(b->transport);
      uv_loop_close(&b->loop);
    }
    free(b);
    return NULL;
  }
  tb_session_init(&b->session, b->cfg.clock, b->cfg.idle_grace_ms);
  return b;
}

void tb_destroy(tb_browser *b) {
  if (!b) return;
  /* 引擎持有的 qzjs runtime(自有线程 + loop)必须显式关闭,否则线程泄漏。 */
  if (b->js_doc && b->engine) b->engine->close(b->engine, b->js_doc);
  /* loop_init 同时意味着「默认 curl transport 是 tb_create 自己建的」
   * (见 tb_create),所以只有这条路径才归我们销毁 —— 调用方注入的
   * cfg.transport 生命周期归调用方,browser 不碰。
   * 顺序:transport 的 destroy 内部要跑一次 loop 才能完成 libuv 的异步
   * close,必须排在 uv_loop_close 之前。 */
  if (b->loop_init) {
    if (b->transport && b->transport->destroy) b->transport->destroy(b->transport);
    uv_loop_close(&b->loop);
  }
  tb_session_free(&b->session);
  tb_view_free(b->view);
  free(b->cookie_jar);
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
