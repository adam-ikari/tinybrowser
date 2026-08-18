#include "js_engine.h"
#include "tb.h"
#include "view.h"
#include "browser_internal.h"
#include <quickjs.h>
#include <string.h>
#include <stdlib.h>

/* 内置 JS: dom.js + parser.js + render.js (由 cmake 生成) */
#include "js_builtins.inc"

/* 引擎实例:内部封装 QuickJS runtime/context + host 指针 */
typedef struct js_timer js_timer;
struct js_timer {
  int tid;
  uint64_t deadline;  /* host clock 单调毫秒 */
  JSValue fn;         /* 强引用 */
  js_timer *next;
};

typedef struct {
  JSRuntime *rt;
  JSContext *ctx;
  tb_browser *host;  /* NULL = 无 host(console/timer 是 no-op) */
  js_timer *timers;
  int next_tid;
  /* 脚本执行超时(js_exec_ms_limit)的中断状态 */
  const tb_clock *isr_clock;
  uint64_t isr_start_ms;
  uint32_t isr_limit_ms;
} js_handle;

/* ---- interrupt handler:js_exec_ms_limit 超时 → 中断当前 JS 执行 ---- */
static int js_interrupt_handler(JSRuntime *rt, void *opaque) {
  (void)rt;
  js_handle *h = (js_handle *)opaque;
  if (!h || h->isr_limit_ms == 0) return 0;
  return h->isr_clock->now_ms(h->isr_clock) - h->isr_start_ms >= h->isr_limit_ms;
}

/* 每次 JS 执行前重置超时窗口 */
static void js_isr_reset(js_handle *h) {
  if (h && h->isr_clock) h->isr_start_ms = h->isr_clock->now_ms(h->isr_clock);
}

/* ---- helper: JSValue → malloc'd C string ---- */
static char *js_to_cstring(JSContext *ctx, JSValue v) {
  if (JS_IsException(v)) {
    JSValue exc = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, exc);
    size_t len = strlen(msg);
    char *buf = malloc(len + 1);
    memcpy(buf, msg, len + 1);
    JS_FreeCString(ctx, msg);
    JS_FreeValue(ctx, exc);
    return buf;
  }
  const char *s = JS_ToCString(ctx, v);
  if (!s) return strdup("");
  size_t len = strlen(s);
  char *buf = malloc(len + 1);
  memcpy(buf, s, len + 1);
  JS_FreeCString(ctx, s);
  return buf;
}

/* ---- __tb_console(level, msg) bridge ---- */
static JSValue js___tb_console(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
  js_handle *h = JS_GetContextOpaque(ctx);
  if (!h || !h->host) return JS_UNDEFINED;

  const char *level = JS_ToCString(ctx, argv[0]);
  const char *msg = JS_ToCString(ctx, argv[1]);
  if (h->host->cfg.on_console) {
    h->host->cfg.on_console(h->host, level, msg, h->host->cfg.ud);
  }
  JS_FreeCString(ctx, level);
  JS_FreeCString(ctx, msg);
  return JS_UNDEFINED;
}

/* ---- __tb_timer_set(delay_ms, fn) bridge:返回 tid(int) ---- */
static JSValue js___tb_timer_set(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  js_handle *h = JS_GetContextOpaque(ctx);
  if (!h) return JS_UNDEFINED;

  int32_t delay = 0;
  JS_ToInt32(ctx, &delay, argv[0]);

  js_timer *t = calloc(1, sizeof(js_timer));
  if (!t) return JS_EXCEPTION;
  t->tid = h->next_tid++;
  if (h->host && h->host->cfg.clock) {
    t->deadline = h->host->cfg.clock->now_ms(h->host->cfg.clock) + delay;
  } else {
    t->deadline = tb_clock_real.now_ms(&tb_clock_real) + delay;
  }
  t->fn = JS_DupValue(ctx, argv[1]);
  t->next = h->timers;
  h->timers = t;
  return JS_NewInt32(ctx, t->tid);
}

/* ---- __tb_timer_cancel(tid) ---- */
static JSValue js___tb_timer_cancel(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv) {
  js_handle *h = JS_GetContextOpaque(ctx);
  if (!h) return JS_UNDEFINED;
  int32_t tid = 0;
  JS_ToInt32(ctx, &tid, argv[0]);
  js_timer **pp = &h->timers;
  while (*pp) {
    js_timer *t = *pp;
    if (t->tid == tid) {
      *pp = t->next;
      JS_FreeValue(ctx, t->fn);
      free(t);
      return JS_NewBool(ctx, 1);
    }
    pp = &t->next;
  }
  return JS_NewBool(ctx, 0);
}

/* ---- __tb_load_sync(url) bridge (nested pump) ---- */
static JSValue js___tb_load_sync(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  js_handle *h = JS_GetContextOpaque(ctx);
  if (!h || !h->host) {
    return JS_ThrowTypeError(ctx, "__tb_load_sync: no host");
  }
  const char *url = JS_ToCString(ctx, argv[0]);
  char *body = NULL;
  tb_err err = tb_load_sync(h->host, url, &body);
  JS_FreeCString(ctx, url);
  if (err.code != 0) {
    free(body);
    return JS_ThrowTypeError(ctx, "load failed: %s", err.msg);
  }
  JSValue ret = JS_NewString(ctx, body ? body : "");
  free(body);
  return ret;
}

/* ---- __tb_eval_js(code) bridge (over browser) ---- */
static JSValue js___tb_eval_js(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
  js_handle *h = JS_GetContextOpaque(ctx);
  if (!h || !h->host) {
    return JS_ThrowTypeError(ctx, "__tb_eval_js: no host");
  }
  const char *code = JS_ToCString(ctx, argv[0]);
  char *out = NULL;
  tb_err err = tb_eval_js(h->host, code, &out);
  JS_FreeCString(ctx, code);
  if (err.code != 0) {
    free(out);
    return JS_ThrowTypeError(ctx, "eval failed: %s", err.msg);
  }
  JSValue ret = JS_NewString(ctx, out ? out : "");
  free(out);
  return ret;
}

/* ---- __tb_view_dump(id) bridge ---- */
static JSValue js___tb_view_dump(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
  js_handle *h = JS_GetContextOpaque(ctx);
  if (!h || !h->host || !h->host->view) {
    return JS_NewString(ctx, "");
  }
  /* 从 argv[0] 取视图 id (当前忽略,返回整个 view dump) */
  char *dump = tb_view_dump(h->host->view);
  JSValue ret = JS_NewString(ctx, dump ? dump : "");
  free(dump);
  return ret;
}

/* ---- 文档加载器(JS):parse → attach 原型 → 同步执行 scripts → readyState=complete ----
   作为全局函数 _tb_load_document 在 open 时注入;load_document 用 JS_Call 传入 body
   (JS_NewStringLen,不做字符串拼接/转义)。 */
static const char *TB_LOADER_SRC =
  "var _tb_load_document = function (body) {"
  "  var p = _tb_parser.make();"
  "  p.feed(body);"
  "  p.finalize();"
  "  document._tb_attach(p.root);"
  "  _tb_parser._readyState = 'loading';"
  "  var s;"
  "  while ((s = p.next_script()) !== null) {"
  "    var code = null;"
  "    try { code = s.src ? __tb_load_sync(s.src) : s.text; } catch (e) {}"
  "    if (!code) continue;"
  "    try { (0, eval)(code); }"
  "    catch (e) {"
  "      __tb_console('warn', 'script error: ' + (e && e.message ? e.message : String(e)));"
  "    }"
  "  }"
  "  _tb_parser._readyState = 'complete';"
  "  return p.root;"
  "};";

/* ---- 内置函数注册 ---- */
static const JSCFunctionListEntry js_global_funcs[] = {
  JS_CFUNC_DEF("__tb_console", 2, js___tb_console),
  JS_CFUNC_DEF("__tb_timer_set", 2, js___tb_timer_set),
  JS_CFUNC_DEF("__tb_timer_cancel", 1, js___tb_timer_cancel),
  JS_CFUNC_DEF("__tb_load_sync", 1, js___tb_load_sync),
  JS_CFUNC_DEF("__tb_eval_js", 1, js___tb_eval_js),
  JS_CFUNC_DEF("__tb_view_dump", 1, js___tb_view_dump),
};

/* ---- engine impl: open ---- */
static void *js_engine_open(const struct tb_js_engine *self, tb_browser *host) {
  (void)self;
  js_handle *h = calloc(1, sizeof(js_handle));
  if (!h) return NULL;

  /* 从 host 获取内存限制;默认 4MB */
  size_t mem_limit = 4 * 1024 * 1024;
  if (host && host->cfg.js_memory_limit > 0) {
    mem_limit = host->cfg.js_memory_limit;
  }

  h->rt = JS_NewRuntime();
  if (!h->rt) { free(h); return NULL; }
  JS_SetMaxStackSize(h->rt, 1024 * 1024);  /* 1MB 栈 */
  /* 注意:JS_SetMemoryLimit 在 builtins 注入之后再设(见下方)——
     引擎自身 builtins(dom.js/parser.js/render.js ~27KB)不应受用户
     配置的文档内存限制约束;限制约束文档加载期(脚本)与运行期。 */

  /* js_exec_ms_limit 超时中断:opaque 传 js_handle,执行前 js_isr_reset 重置窗口 */
  h->isr_clock = (host && host->cfg.clock) ? host->cfg.clock : &tb_clock_real;
  h->isr_limit_ms = host ? host->cfg.js_exec_ms_limit : 0;
  JS_SetInterruptHandler(h->rt, js_interrupt_handler, h);

  h->ctx = JS_NewContext(h->rt);
  if (!h->ctx) { JS_FreeRuntime(h->rt); free(h); return NULL; }

  h->host = host;
  JS_SetContextOpaque(h->ctx, h);

  /* 注册全局桥函数 */
  JSValue global_obj = JS_GetGlobalObject(h->ctx);
  JS_SetPropertyFunctionList(h->ctx, global_obj, js_global_funcs,
                             sizeof(js_global_funcs) / sizeof(js_global_funcs[0]));
  JS_FreeValue(h->ctx, global_obj);

  /* 加载内置 JS (dom.js + parser.js + render.js) */
  for (int i = 0; tb_js_builtins[i] != NULL; i++) {
    JSValue val = JS_Eval(h->ctx, tb_js_builtins[i], strlen(tb_js_builtins[i]),
                          "<builtins>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val)) {
      JSValue exc = JS_GetException(h->ctx);
      const char *msg = JS_ToCString(h->ctx, exc);
      fprintf(stderr, "[js_engine] builtins load error: %s\n", msg);
      JS_FreeCString(h->ctx, msg);
      JS_FreeValue(h->ctx, exc);
    }
    JS_FreeValue(h->ctx, val);
  }

  /* 注入文档加载器 _tb_load_document(body):parse + attach + 同步执行脚本 */
  JSValue loader = JS_Eval(h->ctx, TB_LOADER_SRC, strlen(TB_LOADER_SRC),
                           "<loader>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(loader)) {
    JSValue exc = JS_GetException(h->ctx);
    const char *msg = JS_ToCString(h->ctx, exc);
    fprintf(stderr, "[js_engine] loader load error: %s\n", msg);
    JS_FreeCString(h->ctx, msg);
    JS_FreeValue(h->ctx, exc);
  }
  JS_FreeValue(h->ctx, loader);

  /* 引擎注入完成,收紧到用户配置的内存上限(默认 4MB)。
     文档加载期(load_document 的脚本)与运行期以此限制;超限 → JS exception。 */
  JS_SetMemoryLimit(h->rt, mem_limit);

  return h;
}

/* ---- engine impl: eval ---- */
static int js_engine_eval(const struct tb_js_engine *self, void *handle,
                          const char *code, char **out) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->ctx) return -1;

  js_isr_reset(h);  /* 单次 eval 的执行时间预算从本次开始计时 */
  JSValue result = JS_Eval(h->ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
  int is_exc = JS_IsException(result);
  *out = js_to_cstring(h->ctx, result);  /* 异常时返回异常消息字符串 */
  JS_FreeValue(h->ctx, result);
  return is_exc ? -1 : 0;
}

/* ---- engine impl: close ---- */
static void js_engine_close(const struct tb_js_engine *self, void *handle) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h) return;
  if (h->ctx) {
    js_timer *t = h->timers;
    while (t) {
      js_timer *nx = t->next;
      JS_FreeValue(h->ctx, t->fn);
      free(t);
      t = nx;
    }
  }
  if (h->ctx) JS_FreeContext(h->ctx);
  if (h->rt) JS_FreeRuntime(h->rt);
  free(h);
}

/* ---- engine impl: load_document ---- */
static int js_engine_load_document(const struct tb_js_engine *self, void *handle,
                                   const char *body, size_t len) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->ctx) return -1;

  /* 调用全局 _tb_load_document(body):parse + attach + 脚本执行(见 TB_LOADER_SRC) */
  js_isr_reset(h);  /* 文档解析+脚本执行共享 js_exec_ms_limit 预算 */
  JSValue global = JS_GetGlobalObject(h->ctx);
  JSValue fn = JS_GetPropertyStr(h->ctx, global, "_tb_load_document");
  JS_FreeValue(h->ctx, global);
  if (JS_IsUndefined(fn) || JS_IsException(fn)) {
    JS_FreeValue(h->ctx, fn);
    return -1;
  }

  JSValue js_body = JS_NewStringLen(h->ctx, body, len);
  JSValue args[] = { js_body };
  JSValue result = JS_Call(h->ctx, fn, JS_UNDEFINED, 1, args);
  JS_FreeValue(h->ctx, fn);
  JS_FreeValue(h->ctx, js_body);

  if (JS_IsException(result)) {
    JSValue exc = JS_GetException(h->ctx);
    const char *msg = JS_ToCString(h->ctx, exc);
    fprintf(stderr, "[js_engine] load error: %s\n", msg);
    JS_FreeCString(h->ctx, msg);
    JS_FreeValue(h->ctx, exc);
    JS_FreeValue(h->ctx, result);
    return -1;
  }

  /* 保存树根到全局 _tb_current_doc。
     注意:quickjs-ng 的 JS_SetPropertyStr 取走 val 的“所有权”——
     内部 set_value 只释放旧值、不复制新值,设置成功后调用者不得再
     JS_FreeValue(result)(否则属性悬空,GC 时 gc_decref_child 断言崩溃)。
     已在 deps/quickjs-ng 上实测:SetProperty 后再 FreeValue(val) 段错误。 */
  JSValue global2 = JS_GetGlobalObject(h->ctx);
  JS_SetPropertyStr(h->ctx, global2, "_tb_current_doc", result);
  JS_FreeValue(h->ctx, global2);
  return 0;
}

/* ---- engine impl: render ---- */
static int js_engine_render(const struct tb_js_engine *self, void *handle,
                            const char *url, int status, struct tb_view *out) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->ctx || !out) return -1;

  /* 获取 _tb_current_doc */
  JSValue global = JS_GetGlobalObject(h->ctx);
  JSValue doc = JS_GetPropertyStr(h->ctx, global, "_tb_current_doc");
  JS_FreeValue(h->ctx, global);

  if (JS_IsUndefined(doc)) return -1;

  /* 调用 tb_render_js(doc, url, status) */
  JSValue fn = JS_Eval(h->ctx, "tb_render_js", 11, "<render>", JS_EVAL_TYPE_GLOBAL);
  if (JS_IsUndefined(fn) || JS_IsException(fn)) {
    JS_FreeValue(h->ctx, fn);
    JS_FreeValue(h->ctx, doc);
    return -1;
  }
  JSValue js_url = JS_NewString(h->ctx, url ? url : "");
  JSValue js_status = JS_NewInt32(h->ctx, status);
  JSValue args[] = { doc, js_url, js_status };
  JSValue result = JS_Call(h->ctx, fn, JS_UNDEFINED, 3, args);
  JS_FreeValue(h->ctx, fn);
  JS_FreeValue(h->ctx, js_url);
  JS_FreeValue(h->ctx, js_status);
  JS_FreeValue(h->ctx, doc);

  if (JS_IsException(result)) {
    JSValue exc = JS_GetException(h->ctx);
    const char *msg = JS_ToCString(h->ctx, exc);
    fprintf(stderr, "[js_engine] render error: %s\n", msg);
    JS_FreeCString(h->ctx, msg);
    JS_FreeValue(h->ctx, exc);
    JS_FreeValue(h->ctx, result);
    return -1;
  }

  /* 从结果对象提取 text, title, status, elems */
  JSValue js_text = JS_GetPropertyStr(h->ctx, result, "text");
  JSValue js_title = JS_GetPropertyStr(h->ctx, result, "title");
  JSValue js_st = JS_GetPropertyStr(h->ctx, result, "status");
  JSValue js_elems = JS_GetPropertyStr(h->ctx, result, "elems");

  const char *text = JS_ToCString(h->ctx, js_text);
  const char *title = JS_ToCString(h->ctx, js_title);
  int32_t st = 0;
  JS_ToInt32(h->ctx, &st, js_st);

  free(out->text);
  free(out->title);
  free(out->url);
  out->text = strdup(text ? text : "");
  out->title = strdup(title ? title : "");
  out->url = strdup(url ? url : "");   /* seam 签名直接带 url 参数,勿依赖 JS 侧 */
  out->status = st;

  /* 解析 elems 数组 */
  uint32_t nelems = 0;
  JSValue js_len = JS_GetPropertyStr(h->ctx, js_elems, "length");
  JS_ToUint32(h->ctx, &nelems, js_len);
  JS_FreeValue(h->ctx, js_len);

  free(out->elems);
  out->nelems = 0;
  if (nelems > 0) {
    out->elems = calloc(nelems, sizeof(struct tb_elem));
    for (uint32_t i = 0; i < nelems; i++) {
      JSValue je = JS_GetPropertyUint32(h->ctx, js_elems, i);
      JSValue je_id = JS_GetPropertyStr(h->ctx, je, "id");
      JSValue je_type = JS_GetPropertyStr(h->ctx, je, "type");
      JSValue je_text = JS_GetPropertyStr(h->ctx, je, "text");
      JSValue je_href = JS_GetPropertyStr(h->ctx, je, "href");
      JSValue je_name = JS_GetPropertyStr(h->ctx, je, "name");
      JSValue je_value = JS_GetPropertyStr(h->ctx, je, "value");
      JSValue je_off = JS_GetPropertyStr(h->ctx, je, "off");
      /* select 的 options 数组(render.js 输出;M1 tb_render 等价物) */
      JSValue je_options = JS_GetPropertyStr(h->ctx, je, "options");

      int32_t eid = 0, eoff = 0;
      JS_ToInt32(h->ctx, &eid, je_id);
      JS_ToInt32(h->ctx, &eoff, je_off);
      const char *etype = JS_ToCString(h->ctx, je_type);
      const char *etext = JS_ToCString(h->ctx, je_text);
      const char *ehref = JS_ToCString(h->ctx, je_href);
      const char *ename = JS_ToCString(h->ctx, je_name);
      const char *evalue = JS_ToCString(h->ctx, je_value);

      struct tb_elem *e = &out->elems[out->nelems];
      e->id = eid;
      e->type = strdup(etype ? etype : "");
      e->text = strdup(etext ? etext : "");
      e->href = strdup(ehref ? ehref : "");
      e->name = strdup(ename ? ename : "");
      e->value = strdup(evalue ? evalue : "");
      e->off = eoff;

      /* 展开 options 数组到 e->options / e->noptions(M1 语义:select 的选项) */
      uint32_t nopts = 0, opt_len = 0;
      e->noptions = 0;
      e->options = NULL;
      if (JS_IsArray(je_options)) {
        JSValue jol = JS_GetPropertyStr(h->ctx, je_options, "length");
        JS_ToUint32(h->ctx, &opt_len, jol);
        JS_FreeValue(h->ctx, jol);
        if (opt_len > 0) {
          e->options = calloc(opt_len, sizeof(char *));
          for (uint32_t o = 0; o < opt_len; o++) {
            JSValue jo = JS_GetPropertyUint32(h->ctx, je_options, o);
            const char *os = JS_ToCString(h->ctx, jo);
            e->options[e->noptions++] = strdup(os ? os : "");
            JS_FreeCString(h->ctx, os);
            JS_FreeValue(h->ctx, jo);
          }
        }
      }
      out->nelems++;

      JS_FreeCString(h->ctx, etype);
      JS_FreeCString(h->ctx, etext);
      JS_FreeCString(h->ctx, ehref);
      JS_FreeCString(h->ctx, ename);
      JS_FreeCString(h->ctx, evalue);
      /* 必须先释放从 je 派生的子引用,再释放 je 本身,
         否则 je 引用计数归零被回收后,子引用指向已回收值,
         触发 quickjs gc_decref_child 断言(JS_REF_COUNT(p) > 0)。 */
      JS_FreeValue(h->ctx, je_id);
      JS_FreeValue(h->ctx, je_type);
      JS_FreeValue(h->ctx, je_text);
      JS_FreeValue(h->ctx, je_href);
      JS_FreeValue(h->ctx, je_name);
      JS_FreeValue(h->ctx, je_value);
      JS_FreeValue(h->ctx, je_off);
      JS_FreeValue(h->ctx, je_options);
      JS_FreeValue(h->ctx, je);
    }
  }

  JS_FreeCString(h->ctx, text);
  JS_FreeCString(h->ctx, title);
  JS_FreeValue(h->ctx, js_text);
  JS_FreeValue(h->ctx, js_title);
  JS_FreeValue(h->ctx, js_st);
  JS_FreeValue(h->ctx, js_elems);
  JS_FreeValue(h->ctx, result);
  return 0;
}

/* ---- engine impl: poll_timers ---- */
static void js_engine_poll_timers(const struct tb_js_engine *self, void *handle) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->ctx || !h->host) return;

  uint64_t now = h->host->cfg.clock
                   ? h->host->cfg.clock->now_ms(h->host->cfg.clock)
                   : tb_clock_real.now_ms(&tb_clock_real);

  js_timer *t = h->timers;
  while (t) {
    js_timer *nx = t->next;
    if (t->deadline <= now) {
      JSValue fn = JS_DupValue(h->ctx, t->fn);
      /* 从链表中摘除 */
      js_timer **pp = &h->timers;
      while (*pp != t) pp = &(*pp)->next;
      *pp = t->next;
      JS_FreeValue(h->ctx, t->fn);
      free(t);
      JSValue r = JS_Call(h->ctx, fn, JS_UNDEFINED, 0, NULL);
      if (JS_IsException(r)) {
        JSValue exc = JS_GetException(h->ctx);
        const char *msg = JS_ToCString(h->ctx, exc);
        fprintf(stderr, "[js_engine] timer cb error: %s\n", msg);
        JS_FreeCString(h->ctx, msg);
        JS_FreeValue(h->ctx, exc);
      }
      JS_FreeValue(h->ctx, r);
      JS_FreeValue(h->ctx, fn);
    }
    t = nx;
  }
}

/* ---- 引擎 vtable ---- */
static const struct tb_js_engine impl = {
  .open = js_engine_open,
  .eval = js_engine_eval,
  .close = js_engine_close,
  .load_document = js_engine_load_document,
  .render = js_engine_render,
  .poll_timers = js_engine_poll_timers,
};

const struct tb_js_engine *tb_default_js_engine(void) {
  return &impl;
}
