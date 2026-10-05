#include "tb.h"
#include "browser_internal.h"
#include "content.h"
#include "curl_transport.h"
#include "session.h"
#include "url.h"
#include "cookie.h"
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

/* ---- 导航上下文:把传输回调桥接到浏览器状态 ----
 *
 * 生命周期分成两段,分界线是「传输回调内」与「pump 顶层」:
 *
 *   传输回调(on_headers/on_body/on_done)只搬数据、登记,不做任何引擎工作。
 *   on_done 把 nav_ctx 挂进 b->nav_ready 队列就返回。
 *   引擎侧的一切(load_document / render,以及 <script src> 触发的
 *   tb_load_sync 子资源抓取)由 tb_pump 在循环顶层调 nav_commit 做。
 *
 * 为什么必须这样分:nav_on_done 原本直接在传输回调里跑引擎。传输回调是由
 * curl_transport 的 check_multi_info 触发的,而它坐在 curl_multi_info_read
 * 的 while 循环里 —— 也就是 curl_multi_socket_action 的栈上。于是
 * <script src> 的抓取会在 curl 自己的循环内部调 curl_multi_add_handle /
 * uv_run,重入 curl multi handle。这是 UB,而且表现为段错误或静默的状态错乱,
 * 极难归因。
 *
 * 「pending 计数在 commit 时才递减」是配套的:若在 on_done 里就 nav_done,
 * tb_session_idle 会变真,tb_pump 可能在 commit 之前就返回,调用方拿到的是
 * 上一张视图。让 pending 一直挂到 commit 完成,pump 才可能判空闲。
 */
typedef struct nav_ctx {
  struct nav_ctx *next;
  tb_browser *b;
  char *url;            /* 请求 URL */
  char *final_url;
  int status;
  char *content_type;
  int attachment;
  char *body; size_t body_len, body_cap;
  char *cookie_header;   /* 请求时发出的 Cookie 头;传输层持有其指针,故归本 ctx */
  int done;
  tb_err err;           /* on_done 带来的传输层结果 */
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

/* Set-Cookie 落在 on_headers 之后派发(传输层的约定),此时 final_url 已就绪,
   cookie 的 domain/path 归属可以按最终 URL 判定。 */
static void nav_on_set_cookie(void *ud, const char *value) {
  nav_ctx *c = ud;
  const char *effective = c->final_url ? c->final_url : c->url;
  tb_cookie_jar_set(c->b->cookies, value, effective);
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

/* 从「在飞」链表摘掉自己。摘不掉说明链表已被 tb_destroy 清空(不该发生:
   传输层此刻仍持有 ud 指针,浏览器却已没了 —— 那是调用方的生命周期错误)。 */
static void nav_unlink_inflight(nav_ctx *c) {
  tb_browser *b = c->b;
  nav_ctx **pp = &b->nav_inflight;
  while (*pp) {
    if (*pp == c) { *pp = c->next; return; }
    pp = &(*pp)->next;
  }
}

static void nav_on_done(void *ud, tb_err err) {
  nav_ctx *c = ud;
  c->done = 1;
  c->err = err;
  tb_browser *b = c->b;
  nav_unlink_inflight(c);
  /* 登记即返回:引擎工作一律留给 nav_commit(pump 顶层)。见 nav_ctx 的注释。 */
  c->next = NULL;
  if (b->nav_ready_tail) b->nav_ready_tail->next = c;
  else b->nav_ready = c;
  b->nav_ready_tail = c;
}

/* ---- 提交:在 pump 顶层做引擎工作 ---- */

/* 通知渲染失败的公共尾巴(错误码 + 消息)。 */
static void nav_fail(tb_browser *b, int code, const char *msg,
                     const char *url) {
  tb_session_nav_fail(&b->session);
  if (b->cfg.on_error) {
    tb_err e = { 0, "" };
    e.code = code;
    snprintf(e.msg, sizeof e.msg, "%s", msg ? msg : "");
    b->cfg.on_error(b, e, url, b->cfg.ud);
  }
}

static void nav_commit_one(nav_ctx *c) {
  tb_browser *b = c->b;
  const char *effective = c->final_url ? c->final_url : c->url;
  if (c->err.code != 0) {
    nav_fail(b, c->err.code, c->err.msg, c->url);
    goto done;
  }
  tb_content_kind kind = tb_content_classify(c->content_type, c->attachment);
  if (kind == TB_CONTENT_RENDER) {
    /* 引擎驱动(M2a):qzjs 解析 HTML、跑脚本、产出视图。不再有 C 侧 DOM 树——
     * 交互(click/fill/select/submit)改为经控制面在 JS 自己的树上查询。
     * load_document 内部可能同步抓 <script src> 子资源(tb_load_sync),
     * 它自己会 pump 传输 —— 这正是必须待在 pump 顶层的原因。 */
    if (b->engine->load_document(b->engine, b->js_doc, c->body, c->body_len, effective) != 0) {
      nav_fail(b, TB_ERR_PARSE, "document load failed", effective);
      goto done;
    }
    tb_view_free(b->view);
    b->view = tb_view_new();
    if (b->engine->render(b->engine, b->js_doc, effective, c->status, b->view) != 0) {
      nav_fail(b, TB_ERR_PARSE, "render failed", effective);
      goto done;
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
  /* pending_url 必须是本结构自有的副本 —— effective 指向 c->url/c->final_url,
     而 nav_ctx 在本函数末尾就 free 了。定时器重渲要长期读它。 */
  free((void *)b->pending_url);
  b->pending_url = strdup(effective);
  /* 指纹置 NULL = 「待重新采样」。下次 pump 会只记基线而不重渲,否则这次导航
     会额外多触发一轮 on_view_changed/on_title(见 nav_refresh_view 首采分支)。 */
  free(b->dom_fingerprint);
  b->dom_fingerprint = NULL;
  if (b->cfg.on_title) b->cfg.on_title(b, tb_view_title(b->view), b->cfg.ud);
  if (b->cfg.on_view_changed) b->cfg.on_view_changed(b, b->cfg.ud);
done:
  free(c->url); free(c->final_url); free(c->content_type);
  free(c->body); free(c->cookie_header); free(c);
}

/* ---- 定时器改动后的视图刷新 ----
 *
 * qzjs 的定时器在它自有 loop 上跑,回调改的是 JS 那棵树;宿主视图是导航时
 * 渲染的那张,不会自己变。真实页面大量依赖这点(轮询状态、动画、倒计时),
 * 不刷新等于定时器白写。
 *
 * 做法是**变更检测 + 惰性重渲**:让 JS 侧算一个 DOM 指纹(文本长度 + 节点
 * 数 + title),宿主每次 pump 比一次,变了才重新 render。
 *
 * 为什么不在定时器回调里直接重渲:qzjs 从不回调宿主,回调跑在它的线程上,
 * 没有办法在那里驱动宿主。只能在 pump 侧轮询。
 *
 * 为什么用指纹而不是「总是重渲」:重渲要过一趟控制面(JS_Eval + JSON 往返),
 * 每次 pump 都做会给空闲页面带来无谓开销。指纹很便宜(一次小 eval),
 * 且只在真变了时才付重渲的钱。
 */
static void nav_refresh_view(tb_browser *b) {
  if (!b->js_doc || !b->engine || !b->view) return;
  if (!b->pending_url) return;              /* 还没加载过文档 */
  /* 不可渲染内容(图片/二进制等)没有 DOM 可重渲。少了这一句,二进制响应
     会在下一次 pump 被引擎重渲成一张空视图,丢掉 not_renderable 标记 ——
     tb_view_not_renderable() 随之失效。 */
  if (b->view->is_not_renderable) return;

  char *fp = NULL;
  /* 指纹取文本长度 + 元素数 + title:足以覆盖绝大多数 DOM 变化,
     又不至于长到把每次 pump 拖慢。 */
  static const char FP_EXPR[] =
      "String(__tb_doc ? (function(){ var n = 0, t = 0, st = [__tb_doc];"
      " while (st.length) { var x = st.pop();"
      "   if (x.type === 'text') t += x.text.length; else n++;"
      "   for (var i = x.children.length - 1; i >= 0; i--) st.push(x.children[i]); }"
      " return n + ':' + t + ':' + document.title; })() : '')";
  if (b->engine->eval(b->engine, b->js_doc, FP_EXPR, &fp) != 0 || !fp) {
    free(fp);
    return;
  }
  if (!b->dom_fingerprint) {
    /* 首次采样(刚导航完):只记基线,不重渲。
       若在这里也重渲,每次导航都会多触发一次 on_view_changed/on_title ——
       导航本身已经渲染过一次了。 */
    b->dom_fingerprint = fp;
    return;
  }
  if (strcmp(b->dom_fingerprint, fp) == 0) {
    free(fp);                              /* 没变,省掉重渲 */
    return;
  }

  /* 顺序要紧:**先重渲、成功后再落指纹**。
   * 反过来(先记指纹再渲染)的话,一旦 render 失败,指纹已经是新状态,
   * 后续每次 pump 都判定「没变」→ 自动刷新被永久关闭,旧视图一直留着,
   * 而且不报任何错。渲染失败(控制面偶发失败、JS 侧遍历抛异常等)本该
   * 在下一次 pump 重试,前提是失败时保留旧指纹。 */
  tb_view *nv = tb_view_new();
  if (!nv) { free(fp); return; }
  if (b->engine->render(b->engine, b->js_doc, b->pending_url,
                        b->view->status, nv) == 0) {
    free(b->dom_fingerprint);
    b->dom_fingerprint = fp;               /* fp 归本字段 */
    tb_view_free(b->view);
    b->view = nv;
    if (b->cfg.on_title) b->cfg.on_title(b, tb_view_title(b->view), b->cfg.ud);
    if (b->cfg.on_view_changed) b->cfg.on_view_changed(b, b->cfg.ud);
  } else {
    free(fp);              /* 保留旧指纹 → 下次 pump 重试 */
    tb_view_free(nv);      /* 渲染失败:保留旧视图 */
  }
}

/* 排空就绪队列。返回提交的条数(供 tb_pump 判是否需要继续转)。 */
static int nav_commit_ready(tb_browser *b) {
  int n = 0;
  while (b->nav_ready) {
    nav_ctx *c = b->nav_ready;
    b->nav_ready = c->next;
    if (!b->nav_ready) b->nav_ready_tail = NULL;
    c->next = NULL;
    nav_commit_one(c);
    n++;
  }
  return n;
}

/* replace=1 时不再压栈(back/forward/reload 已由 session 管理栈) */
static tb_err do_navigate(tb_browser *b, const char *url, const char *method,
                          const char *body, const char *content_type, int replace) {
  if (!b || !url) { tb_err e = { TB_ERR_ARG, "bad args" }; return e; }
  if (!replace) tb_session_nav_start(&b->session, url);
  tb_session_touch_net(&b->session);

  nav_ctx *c = calloc(1, sizeof *c);
  if (!c) { tb_err e = { TB_ERR_ARG, "out of memory" }; return e; }
  c->b = b;
  c->url = strdup(url);
  /* 先挂上「在飞」链表:若 open 之后浏览器就被销毁(调用方没 pump 就
     tb_destroy),on_done 永远不会触发,这份 nav_ctx(含整页 body)就成了
     泄漏。ASAN 在 DestroyDuringNavigationFreesPendingCtx 里抓到过。
     挂链必须早于 open —— open 可能同步派发回调。 */
  c->next = b->nav_inflight;
  b->nav_inflight = c;

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
  req.on_set_cookie = nav_on_set_cookie;
  req.ud = c;
  /* Cookie 头:按请求 URL 从 jar 里取(域/路径/Secure/HttpOnly 过滤在 jar 内做)。
     请求 URL 存在 nav_ctx 里,传输层在 op 存活期间持有该指针 —— 与
     req.url 借用同一份,无额外生命周期问题。 */
  c->cookie_header = tb_cookie_jar_header(b->cookies, url);
  req.cookie = c->cookie_header;
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
  b->cookies = tb_cookie_jar_new();
  if (!b->cookies) {
    if (b->js_doc && b->engine) b->engine->close(b->engine, b->js_doc);
    if (b->loop_init) {
      if (b->transport && b->transport->destroy) b->transport->destroy(b->transport);
      uv_loop_close(&b->loop);
    }
    tb_session_free(&b->session);
    free(b);
    return NULL;
  }
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
  /* 回收所有 nav_ctx:在飞的和已就绪待提交的。
   两者都是自己 malloc 的,传输层不负责回收(它只管自己的 op)。调用方可能
   在导航途中直接 tb_destroy,那时 on_done 永远不会触发 —— 不排空就是泄漏,
   且带着整页 body。ASAN 在 DestroyDuringNavigationFreesPendingCtx 里抓到过。 */
  while (b->nav_ready) {
    nav_ctx *c = b->nav_ready;
    b->nav_ready = c->next;
    free(c->url); free(c->final_url); free(c->content_type);
    free(c->body); free(c->cookie_header); free(c);
  }
  b->nav_ready_tail = NULL;
  while (b->nav_inflight) {
    nav_ctx *c = b->nav_inflight;
    b->nav_inflight = c->next;
    free(c->url); free(c->final_url); free(c->content_type);
    free(c->body); free(c->cookie_header); free(c);
  }

  tb_session_free(&b->session);
  tb_view_free(b->view);
  tb_cookie_jar_free(b->cookies);
  free((void *)b->pending_url);
  free(b->dom_fingerprint);
  free(b);
}

/* ---- 驱动 ---- */
int tb_pump(tb_browser *b, uint32_t timeout_ms) {
  if (!b) return 1;
  const tb_clock *clk = b->cfg.clock;
  uint64_t deadline = timeout_ms ? clk->now_ms(clk) + timeout_ms : 0;
  for (;;) {
    /* 顺序要紧,四步依次:
       1. 推进 uv loop + 传输 —— 完成的导航进 nav_ready 队列(纯搬运)。
       2. 排空 JS 邮箱 —— console 帧与「导航指令」在此取出。**这一步原先
          根本不存在**:poll_timers 从没被调用过,而 qzjs 只在控制面往返时
          才顺带取邮箱。于是页面脚本里的 location.href='/x' 发出的指令永远
          没人执行,console 帧也会一直堆着。qzjs 从不回调宿主,邮箱是它唯一
          的出站通道,不排它等于把 JS 的对外输出全堵死。
       3. 在顶层提交引擎工作。提交过程自身会 pump 传输抓子资源,所以必须
          排在 transport->poll 之后、且不在任何回调栈内。
       4. 判空闲。 */
    if (b->loop_init) uv_run(&b->loop, UV_RUN_NOWAIT);
    b->transport->poll(b->transport);
    if (b->js_doc && b->engine && b->engine->poll_timers)
      b->engine->poll_timers(b->engine, b->js_doc);
    nav_commit_ready(b);
    nav_refresh_view(b);
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
