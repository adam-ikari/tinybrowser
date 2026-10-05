/* JS 引擎 = qzjs(嵌入式 QuickJS-ng 运行时)。
 *
 * 与旧实现的关键差别:宿主**不再**直接持有 JSContext/JSValue。qzjs 的主权原则
 * (include/qzjs/qzjs.h:130)规定库从不执行宿主代码,JS 跑在库自有线程 + 自有 uv
 * loop 上。宿主与 JS 的唯一通道是:
 *   入站 qz_control(rt, cmd)   —— 命令排队,qzjs 线程在事件循环安全点执行
 *   出站 qz_recv_message(...)  —— per-rt FIFO 邮箱,取回执(JSON)
 *
 * 同步语义由「发命令 + 阻塞收回执」拼出来,不需要宿主回调:
 *   {"op":"eval","correl":"N","script":"..."}   → JS_Eval,回执带 result/error
 *   {"op":"inspect","correl":"N","expr":"..."}   → 求值 + JSON.stringify,回执带 json
 * 两者都由 qzjs 在**它自己的线程上**执行(control.c:998 qz_control_dispatch),
 * 所以 JSContext 的访问是合法的。回执经 JS_JSONStringify(control.c:120),即回执
 * 本身是一段 JSON 文本。
 */
#include "js_engine.h"
#include "tb.h"
#include "view.h"
#include "browser_internal.h"
#include "url.h"
#include <qzjs/qzjs.h>
#include <cJSON.h>
#include <string.h>
#include <stdlib.h>

/* 内置 JS: dom.js + parser.js + render.js (由 cmake 生成) */
#include "js_builtins.inc"

/* 无 js_exec_ms_limit 时的回执等待上限。与控制面 ctl_extract 的缺省
 * timeout_ms(5000)对齐:回执条目超期会被判 TIMEOUT 并从表里摘掉,宿主再等也
 * 等不到,只会白等一个 timeout。 */
#define JS_CTL_DEFAULT_TIMEOUT_MS 5000

typedef struct {
  qz_t *rt;
  tb_browser *host;      /* NULL = 无 host(console 只入队,不派发) */
  uint32_t correl_seq;
  uint32_t limit_ms;     /* js_exec_ms_limit;0 = 用 JS_CTL_DEFAULT_TIMEOUT_MS */
} js_handle;

/* ===================================================================
 * JSON helpers
 * =================================================================== */

/* 把任意字节转成可嵌入 JSON 字符串字面量的转义形式(不含外层引号)。
 * 控制字符走 \u00XX,UTF-8 多字节序列原样透传(合法 JSON)。 */
static char *json_escape(const char *s, size_t len) {
  static const char hex[] = "0123456789abcdef";
  /* 最坏情况:每个字节都成 \u00XX(6 字节) */
  char *out = (char *)malloc(len * 6 + 1);
  if (!out) return NULL;
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  out[o++] = '\\'; out[o++] = '"';  break;
      case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
      case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
      case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
      case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
      case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
      case '\t': out[o++] = '\\'; out[o++] = 't';  break;
      default:
        if (c < 0x20) {
          out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
          out[o++] = hex[(c >> 4) & 0xF]; out[o++] = hex[c & 0xF];
        } else {
          out[o++] = (char)c;
        }
    }
  }
  out[o] = '\0';
  return out;
}

/* JSON 字符串字面量(含外层引号)。 */
static char *json_string(const char *s, size_t len) {
  char *esc = json_escape(s, len);
  if (!esc) return NULL;
  size_t n = strlen(esc);
  char *out = (char *)malloc(n + 3);
  if (!out) { free(esc); return NULL; }
  out[0] = '"';
  memcpy(out + 1, esc, n);
  out[n + 1] = '"';
  out[n + 2] = '\0';
  free(esc);
  return out;
}

/* cJSON 值 → malloc'd 字符串。字符串取其内容,其余(number/bool/object/…)
 * 取其 JSON 文本 —— 与旧实现 JS_ToCString 的观感一致。 */
static char *jsonval_to_cstr(const cJSON *v) {
  if (!v) return strdup("");
  if (cJSON_IsString(v)) return strdup(v->valuestring ? v->valuestring : "");
  char *s = cJSON_PrintUnformatted(v);
  char *r = strdup(s ? s : "");
  cJSON_free(s);
  return r;
}

static int now_ms(const js_handle *h) {
  if (h->host && h->host->cfg.clock) return (int)h->host->cfg.clock->now_ms(h->host->cfg.clock);
  return (int)tb_clock_real.now_ms(&tb_clock_real);
}

/* ===================================================================
 * console 桥:JS postMessage → 宿主 cfg.on_console
 * =================================================================== */

static void js_drain_one(js_handle *h, char *json) {
  if (!json) return;
  cJSON *root = cJSON_Parse(json);
  if (root) {
    /* level/msg 嵌在 __tb_console 对象**里面**,不是在顶层 —— 从 root 上取
     * 只会拿到 NULL,静默丢掉整条 console 消息。 */
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "__tb_console");
    const cJSON *lv = c ? cJSON_GetObjectItemCaseSensitive(c, "level") : NULL;
    const cJSON *mg = c ? cJSON_GetObjectItemCaseSensitive(c, "msg") : NULL;
    if (cJSON_IsObject(c) && cJSON_IsString(lv) && cJSON_IsString(mg) && h->host &&
        h->host->cfg.on_console) {
      h->host->cfg.on_console(h->host, lv->valuestring, mg->valuestring,
                              h->host->cfg.ud);
    } else {
      /* document.cookie 写入:JS 发原值 + 当前页 URL,宿主按 URL 判定
         domain/path 缺省值。HttpOnly 覆盖不了 —— JS 压根看不到那些。 */
      const cJSON *ck = cJSON_GetObjectItemCaseSensitive(root, "__tb_cookie_set");
      if (cJSON_IsObject(ck) && h->host) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(ck, "value");
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(ck, "url");
        if (cJSON_IsString(v) && v->valuestring && cJSON_IsString(u) && u->valuestring)
          tb_cookie_jar_set(h->host->cookies, v->valuestring, u->valuestring);
      } else {
        /* 导航指令:JS 侧 location.href=/assign()/history.back() 发过来的。
         * 走同一条 mailbox —— 与 console 同源,qzjs 从不回调宿主。 */
        const cJSON *nv = cJSON_GetObjectItemCaseSensitive(root, "__tb_nav");
        if (cJSON_IsObject(nv) && h->host) {
          const cJSON *act = cJSON_GetObjectItemCaseSensitive(nv, "action");
          const cJSON *url = cJSON_GetObjectItemCaseSensitive(nv, "url");
          const char *a = cJSON_IsString(act) ? act->valuestring : "navigate";
          if (strcmp(a, "reload") == 0) {
            tb_reload(h->host);
          } else if (strcmp(a, "back") == 0) {
            tb_back(h->host);
          } else if (strcmp(a, "forward") == 0) {
            tb_forward(h->host);
          } else if (cJSON_IsString(url) && url->valuestring && url->valuestring[0]) {
            /* 相对引用按当前文档 URL 解析(浏览器语义):
               location.href='/next' 是最常见的写法,直接丢给 curl 会因缺 host
               而失败。session 的 cur_url 此刻就是当前页。 */
            const char *base = tb_session_url(&h->host->session);
            char *abs = tb_url_resolve(base, url->valuestring);
            if (abs) {
              tb_navigate(h->host, abs);
              free(abs);
            } else {
              tb_navigate(h->host, url->valuestring);
            }
          }
        }
      }
    }
    cJSON_Delete(root);
  }
  qz_free_message(json);
}

/* ===================================================================
 * 控制面往返:发命令 → 阻塞等回执(途中把 console 消息派发掉)
 * =================================================================== */

/* interrupt 命令(op:"interrupt" 是 fire-and-forget,无 correl 也照收,故不需要
 * 登记回执条目)。长度一律用 strlen —— 之前硬编码成 20,而字面量只有 18 字节,
 * qz_control_sink 里的 memcpy 会多读 2 字节越过字符串字面量(ASAN:
 * global-buffer-overflow)。长度算错不改变功能,但确实是越界读,别再写死。 */
static const char TB_INTERRUPT_CMD[] = "{\"op\":\"interrupt\"}";

/* 成功返回 malloc'd 回执 JSON(调用者 free);失败返回 NULL。
 * 非回执的邮箱消息(console)就地消费后继续等。 */
static char *js_ctl(js_handle *h, const char *cmd, int timeout_ms) {
  if (!h || !h->rt || !cmd) return NULL;
  if (qz_control(h->rt, cmd, strlen(cmd)) != 0) return NULL;

  int waited = 0;
  for (;;) {
    int slice = timeout_ms - waited;
    if (timeout_ms >= 0 && slice <= 0) {
      /* 收据窗口用尽:投递 interrupt(无 correl 的 fire-and-forget 形态),
       * 让引擎在下一个指令边界停下,并等它把异常回执吐出来。 */
      qz_control(h->rt, TB_INTERRUPT_CMD, strlen(TB_INTERRUPT_CMD));
      return NULL;
    }
    char *json = NULL;
    size_t len = 0;
    /* 剩余时间留 1ms 余量,免得最后一轮刚好踩到 qz 侧的超时判定。 */
    int t = (timeout_ms < 0) ? -1 : (slice > 1 ? slice - 1 : 1);
    int r = qz_recv_message(h->rt, &json, &len, t);
    if (r == 1) {  /* 超时 */
      if (timeout_ms < 0) continue;
      qz_control(h->rt, TB_INTERRUPT_CMD, strlen(TB_INTERRUPT_CMD));
      return NULL;
    }
    if (r < 0 || !json) return NULL;

    int t0 = now_ms(h);
    cJSON *root = cJSON_Parse(json);
    int is_receipt = 0;
    if (root) {
      const cJSON *ctl = cJSON_GetObjectItemCaseSensitive(root, "ctl");
      const cJSON *co = cJSON_GetObjectItemCaseSensitive(root, "correl");
      /* correl 是回执唯一的配对依据,只认带 ctl:true + correl 的帧。 */
      is_receipt = cJSON_IsTrue(ctl) && cJSON_IsString(co) && co->valuestring &&
                   strstr(cmd, co->valuestring) != NULL;
    }
    if (is_receipt) {
      cJSON_Delete(root);
      return json;  /* 调用者负责 qz_free_message */
    }
    cJSON_Delete(root);
    js_drain_one(h, json);
    waited += now_ms(h) - t0;
  }
}

static int js_ctl_timeout(const js_handle *h) {
  return h->limit_ms > 0 ? (int)h->limit_ms : JS_CTL_DEFAULT_TIMEOUT_MS;
}

/* op:"eval" —— 求值并把结果转成字符串。*out = malloc'd 字符串。
 * 返回 0 = 成功;-1 = JS 异常 / 控制面失败(两种情况 *out 都填错误串)。 */
static int js_eval(js_handle *h, const char *code, size_t len, char **out) {
  *out = NULL;
  char *jstr = json_string(code, len);
  if (!jstr) { *out = strdup("qzjs: out of memory"); return -1; }
  char correl[24];
  snprintf(correl, sizeof correl, "\"%u\"", ++h->correl_seq);

  size_t need = strlen(jstr) + 128;
  char *cmd = (char *)malloc(need);
  snprintf(cmd, need,
           "{\"op\":\"eval\",\"correl\":%s,\"timeout_ms\":%d,\"script\":%s}",
           correl, js_ctl_timeout(h), jstr);
  free(jstr);

  char *receipt = js_ctl(h, cmd, js_ctl_timeout(h));
  free(cmd);
  if (!receipt) {
    *out = strdup("qzjs: control round-trip failed");
    return -1;
  }

  int rc = 0;
  cJSON *root = cJSON_Parse(receipt);
  const cJSON *ok = root ? cJSON_GetObjectItemCaseSensitive(root, "ok") : NULL;
  if (cJSON_IsTrue(ok)) {
    *out = jsonval_to_cstr(root ? cJSON_GetObjectItemCaseSensitive(root, "result") : NULL);
  } else {
    const cJSON *e = root ? cJSON_GetObjectItemCaseSensitive(root, "error") : NULL;
    *out = jsonval_to_cstr(e);
    rc = -1;
  }
  cJSON_Delete(root);
  qz_free_message(receipt);
  return rc;
}

/* op:"inspect" —— 求值 + JSON.stringify,回执的 json 字段是该 JSON 文本。 */
static char *js_inspect(js_handle *h, const char *expr) {
  char *jstr = json_string(expr, strlen(expr));
  if (!jstr) return NULL;
  char correl[24];
  snprintf(correl, sizeof correl, "\"%u\"", ++h->correl_seq);

  size_t need = strlen(jstr) + 128;
  char *cmd = (char *)malloc(need);
  snprintf(cmd, need,
           "{\"op\":\"inspect\",\"correl\":%s,\"timeout_ms\":%d,\"expr\":%s}",
           correl, js_ctl_timeout(h), jstr);
  free(jstr);

  char *receipt = js_ctl(h, cmd, js_ctl_timeout(h));
  free(cmd);
  if (!receipt) return NULL;

  char *out = NULL;
  cJSON *root = cJSON_Parse(receipt);
  const cJSON *j = root ? cJSON_GetObjectItemCaseSensitive(root, "json") : NULL;
  if (cJSON_IsString(j) && j->valuestring) out = strdup(j->valuestring);
  cJSON_Delete(root);
  qz_free_message(receipt);
  return out;
}

/* ===================================================================
 * JS 侧引导:console 桥 + 两段式文档加载器
 * =================================================================== */

/* console 桥:qzjs 永不回调宿主,所以 console 输出走 postMessage 进邮箱,
 * 由宿主 poll_timers / 控制面往返途中派发到 cfg.on_console。
 *
 * 两段式加载器:qzjs 没有同步宿主回调,所以 <script src> 无法像旧实现那样
 * 在 JS 里 __tb_load_sync 回来。改成宿主先批量抓取:
 *   __tb_begin_load(body)  → 解析 + 挂原型,把待跑脚本存下,返回 src 列表(JSON)
 *   __tb_finish_load(map)  → 按 map 补齐 src 正文,逐个隔离执行,置 readyState
 * 解析只做一次(begin 里就把脚本全收进 __tb_pending)。
 */
static const char *TB_BOOT_SRC =
  "var __tb_fmt = function (a) {"
  "  var out = [];"
  "  for (var i = 0; i < a.length; i++) {"
  "    var v = a[i];"
  "    if (typeof v === 'string') out.push(v);"
  "    else { try { out.push(JSON.stringify(v)); } catch (e) { out.push(String(v)); } }"
  "  }"
  "  return out.join(' ');"
  "};"
  "var __tb_console = {};"
  "['log', 'info', 'warn', 'error', 'debug'].forEach(function (lv) {"
  "  __tb_console[lv] = function () {"
  "    var m = __tb_fmt(arguments);"
  "    try { postMessage({ __tb_console: { level: lv, msg: m } }); } catch (e) {}"
  "    if (lv === 'error' || lv === 'warn') {"
  "      try { if (globalThis.__tb_stderr) globalThis.__tb_stderr(lv, m); } catch (e) {}"
  "    }"
  "  };"
  "});"
  "try { globalThis.console = __tb_console; } catch (e) {}"
  /* location / history / navigator。
     qzjs 从不执行宿主代码,所以这里没有「宿主函数」可用。分成两个方向:
     - **读**(url / 历史长度 / UA):宿主在每次导航提交后用 eval 把最新值推给
       __tb_env,JS 侧只读本地对象。查询不往返控制面,故 location.href 随手可读。
     - **写**(location.href = x / assign / reload / back / forward):JS 往
       mailbox postMessage,宿主在 poll_timers 里取出并执行。这是唯一可行方向,
       因为写操作天然是异步的 —— 导航要等网络。 */
  "var __tb_env = { url: '', hlen: 0, can_back: false, can_fwd: false, ua: '',"
                       " cookie: '' };"
  /* document.cookie 的写侧。读侧在 __tb_env.cookie(宿主推的,已排除 HttpOnly)。
     setcookie 的第一个参数是当前页 URL —— 宿主据此判定 domain/path 缺省值。 */
  "var __tb_set_cookie = function (v) {"
  "  try { postMessage({ __tb_cookie_set: { value: v, url: __tb_env.url } }); } catch (e) {}"
  "};"
  "var __tb_send_nav = function (action, url) {"
  "  try { postMessage({ __tb_nav: { action: action, url: url || '' } }); } catch (e) {}"
  "};"
  "var __tb_location = {"
  "  get href() { return __tb_env.url; },"
  "  set href(v) { __tb_send_nav('navigate', String(v)); },"
  "  get origin() { var m = this.href.match(/^[a-z]+:\\/\\/[^\\/]+/); return m ? m[0] : ''; },"
  "  get protocol() { return this.href.split(':')[0] + ':'; },"
  "  get host() { var m = this.href.match(/^[a-z]+:\\/\\/([^\\/?#]+)/); return m ? m[1] : ''; },"
  "  get hostname() { var h = this.host.split(':'); return h[0]; },"
  "  get port() { var h = this.host.split(':'); return h.length > 1 ? h[1] : ''; },"
  "  get pathname() {"
  "    var s = this.href.replace(/^[a-z]+:\\/\\/[^\\/?#]+/, '');"
  "    var q = s.indexOf('?'); if (q >= 0) s = s.slice(0, q);"
  "    var f = s.indexOf('#'); if (f >= 0) s = s.slice(0, f);"
  "    return s || '/';"
  "  },"
  "  get search() {"
  "    var s = this.href; var q = s.indexOf('?');"
  "    if (q < 0) return '';"
  "    var f = s.indexOf('#');"
  "    return f > q ? s.slice(q, f) : s.slice(q);"
  "  },"
  "  get hash() { var f = this.href.indexOf('#'); return f < 0 ? '' : this.href.slice(f); },"
  "  reload: function () { __tb_send_nav('reload', ''); },"
  "  assign: function (u) { __tb_send_nav('navigate', String(u)); },"
  "  replace: function (u) { __tb_send_nav('navigate', String(u)); },"
  "  toString: function () { return this.href; }"
  "};"
  "var __tb_history = {"
  "  get length() { return __tb_env.hlen; },"
  "  back: function () { __tb_send_nav('back', ''); },"
  "  forward: function () { __tb_send_nav('forward', ''); },"
  "  go: function (n) {"
  "    n = Number(n) | 0;"
  "    if (n < 0) { for (var i = 0; i < -n; i++) this.back(); }"
  "    else if (n > 0) { for (var i = 0; i < n; i++) this.forward(); }"
  "  },"
  "  get state() { return null; }"
  "};"
  /* navigator:qzjs 的 polyfill 已经建了一个 navigator,并且把 userAgent 设成
     'qzjs/1.0 (WinterTC)'。那在独立运行时是对的(标识运行时),但页面是跑在
     **浏览器**里的 —— 必须覆盖成浏览器 UA,否则服务端的 UA 嗅探会把它判成
     非浏览器(qzjs 那套 WinterTC 的 UA 嗅探恰好「像」浏览器,后果更隐蔽)。
     故此处**无条件覆盖** userAgent,但不整体替换 navigator 对象,保留 qzjs
     提供的其余属性。 */
  "try {"
  "  var __tb_nav = globalThis.navigator;"
  "  if (!__tb_nav) { __tb_nav = {}; globalThis.navigator = __tb_nav; }"
  "  try { __tb_nav.userAgent = __tb_env.ua || __tb_nav.userAgent || ''; } catch (e) {}"
  "  try { if (!__tb_nav.platform) __tb_nav.platform = 'Linux x86_64'; } catch (e) {}"
  "} catch (e) {}"
  "try { globalThis.location = __tb_location; } catch (e) {}"
  "try { globalThis.history = __tb_history; } catch (e) {}"
  /* window = globalThis 的别名。qzjs 提供了 self(=== globalThis)但没有 window,
     而 window.x = v / window.location 是网页脚本最常见的写法 —— 缺了它,
     一大批真实页面在第一行就 ReferenceError。别名指向同一对象,不是拷贝。 */
  "try { if (typeof window === 'undefined') globalThis.window = globalThis; } catch (e) {}"
  "var __tb_pending = [];"
  "var __tb_root = null;"
  "var __tb_doc = null;"
  /* 阶段一:解析 + 收集脚本。返回 src 数组(值,不是 JSON 文本——宿主用
   * op:"inspect" 取回,inspect 自带 JSON.stringify,这里再 stringify 会双编码)。 */
  "var __tb_begin_load = function (body) {"
  "  var p = _tb_parser.make();"
  "  p.feed(body);"
  "  p.finalize();"
  "  document._tb_attach(p.root);"
  "  _tb_parser._readyState = 'loading';"
  "  __tb_pending = [];"
  "  var srcs = [], s;"
  "  while ((s = p.next_script()) !== null) {"
  "    __tb_pending.push({ src: s.src || null, text: s.src ? null : s.text });"
  "    if (s.src) srcs.push(s.src);"
  "  }"
  "  __tb_root = p.root;"
  "  return srcs;"
  "};"
  /* 阶段二:宿主已把 src 正文放进 map,逐个隔离执行。 */
  "var __tb_finish_load = function (map) {"
  "  for (var i = 0; i < __tb_pending.length; i++) {"
  "    var it = __tb_pending[i];"
  "    var code = it.src ? (Object.prototype.hasOwnProperty.call(map, it.src) ? map[it.src] : null)"
  "                     : it.text;"
  "    if (!code) continue;"
  "    try { (0, eval)(code); }"
  "    catch (e) {"
  "      __tb_console.warn('script error: ' + (e && e.message ? e.message : String(e)));"
  "    }"
  "  }"
  "  __tb_pending = [];"
  "  _tb_parser._readyState = 'complete';"
  "  __tb_doc = __tb_root;"
  "  return true;"
  "};";

static void *js_engine_open(const struct tb_js_engine *self, tb_browser *host) {
  (void)self;
  js_handle *h = (js_handle *)calloc(1, sizeof(js_handle));
  if (!h) return NULL;
  h->host = host;
  h->limit_ms = host ? host->cfg.js_exec_ms_limit : 0;

  /* 引导脚本 = 内置 JS(dom/parser/render) + 桥与加载器 */
  size_t cap = strlen(TB_BOOT_SRC) + 1;
  for (int i = 0; tb_js_builtins[i] != NULL; i++) cap += strlen(tb_js_builtins[i]) + 1;
  char *boot = (char *)malloc(cap);
  if (!boot) { free(h); return NULL; }
  size_t o = 0;
  for (int i = 0; tb_js_builtins[i] != NULL; i++) {
    size_t n = strlen(tb_js_builtins[i]);
    memcpy(boot + o, tb_js_builtins[i], n);
    o += n;
    boot[o++] = '\n';
  }
  memcpy(boot + o, TB_BOOT_SRC, strlen(TB_BOOT_SRC));
  o += strlen(TB_BOOT_SRC);
  boot[o] = '\0';

  qz_config_t cfg;
  qz_config_init(&cfg);
  cfg.control_plane = QZ_CONTROL_IN_PROC;  /* 默认 OFF 会让 qz_control 恒 -1 */
  /* 跑的是第三方网页脚本:strict 挡住 fs 限根逃逸、processSpawn 与 env 注入。
   * sandbox_root / env_allowlist 留 NULL → strict 下拒绝一切 fs、不注入 env。 */
  cfg.strict_mode = 1;
  cfg.initial_script = boot;

  h->rt = qz_create(&cfg);
  free(boot);
  if (!h->rt) {
    fprintf(stderr, "[js_engine] qz_create failed (abi %u)\n", qz_abi_version());
    free(h);
    return NULL;
  }
  return h;
}

static int js_engine_eval(const struct tb_js_engine *self, void *handle,
                          const char *code, char **out) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->rt || !code || !out) return -1;
  return js_eval(h, code, strlen(code), out);
}

static void js_engine_close(const struct tb_js_engine *self, void *handle) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h) return;
  if (h->rt) qz_destroy(h->rt);
  free(h);
}

/* ---- 把宿主的浏览器环境推给 JS(location/history/navigator 的读侧) ----
 *
 * qzjs 从不回调宿主,所以读侧不能「问宿主」。做法是宿主主动推:把最新 URL /
 * 历史长度 / UA 用 op:"eval" 写进 __tb_env,JS 侧只读本地变量。读
 * (location.href)因此没有控制面往返开销 —— 真实页面脚本动辄读几十次。
 */
/* 表达式模板单独提出来,长度用 strlen(FMT) 算 —— 不再用「+160」这种魔数。
 * 那个魔数曾经不够:格式串本身已超 160 字节,snprintf 静默截断出一条残缺表达式,
 * eval 报错,而返回值当时被忽略,于是表现为「location 全是空值」这种
 * 完全指不到根因的现象。与本文件早前 `js_eval(..., 24, ...)` 少一个字符
 * 是同一类错误,同一个坑踩了两次。 */
#define TB_PUSH_ENV_FMT \
  "__tb_env.url = %s; __tb_env.ua = %s;" \
  "__tb_env.hlen = %d; __tb_env.can_back = %d; __tb_env.can_fwd = %d;" \
  /* qzjs polyfill 把 userAgent 设成 'qzjs/1.0 (WinterTC)'。页面跑在浏览器里, */ \
  /* 必须覆盖成浏览器 UA,否则服务端的 UA 嗅探会误判。boot 期 __tb_env.ua 还是 */ \
  /* 空的,所以每次 push 都重设一遍。 */ \
  "try { if (globalThis.navigator) navigator.userAgent = __tb_env.ua; } catch (e) {}" \
  "1"

static int js_push_env(js_handle *h, const char *url) {
  tb_browser *b = h->host;
  if (!b || !h->rt || !url) return 0;
  char *ju = json_string(url, strlen(url));
  const char *ua = b->cfg.user_agent ? b->cfg.user_agent : "";
  char *jua = json_string(ua, strlen(ua));
  if (!ju || !jua) { free(ju); free(jua); return -1; }

  /* history.length 含后退栈 + 前进栈(浏览器行为亦然),故两个栈长度相加。 */
  int hlen = tb_session_nback(&b->session) + tb_session_nfwd(&b->session);
  /* +1 收尾 NUL,+32 给三个 %d 的最坏位数留余量。 */
  size_t need = strlen(ju) + strlen(jua) + strlen(TB_PUSH_ENV_FMT) + 32 + 1;
  char *expr = (char *)malloc(need);
  if (!expr) { free(ju); free(jua); return -1; }
  snprintf(expr, need, TB_PUSH_ENV_FMT,
           ju, jua, hlen, tb_session_can_back(&b->session),
           tb_session_can_fwd(&b->session));
  free(ju); free(jua);

  /* document.cookie 的读侧也在这里推。此刻 jar 里可能已含**上一页**设的
     cookie —— 正确:cookie 在文档解析前就该可见(与真实浏览器一致)。 */
  char *jc = tb_cookie_jar_visible(b->cookies, url);
  if (jc) {
    char *jcs = json_string(jc, strlen(jc));
    free(jc);
    if (jcs) {
      /* 按 jcs(转义后)的长度算,不是 jc —— cookie 值里可能有引号/反斜杠,
         转义后更长,拿原长度算会再次截断。 */
      size_t n2 = strlen(expr) + strlen(jcs) + strlen("; __tb_env.cookie = ") + 1;
      char *expr2 = (char *)malloc(n2);
      if (expr2) {
        snprintf(expr2, n2, "%s; __tb_env.cookie = %s", expr, jcs);
        free(expr);
        expr = expr2;
      }
      free(jcs);
    }
  }

  char *out = NULL;
  int rc = js_eval(h, expr, strlen(expr), &out);
  if (rc != 0)
    fprintf(stderr, "[js_engine] push env failed: %s\n", out ? out : "?");
  free(expr);
  free(out);
  return rc;
}

/* 两段式:begin → 宿主抓 src → finish */
static int js_engine_load_document(const struct tb_js_engine *self, void *handle,
                                   const char *body, size_t len, const char *base_url) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->rt) return -1;

  /* 环境必须先于文档推给 JS:脚本执行时 location 得已是本页 URL,否则读到的是
     上一页(session 的 cur_url 此刻尚未 nav_done)。 */
  if (h->host) js_push_env(h, base_url);

  /* 阶段一:__tb_begin_load(body) → src 列表(JSON 字符串) */
  char *jbody = json_string(body ? body : "", len);
  if (!jbody) return -1;
  size_t need = strlen(jbody) + 64;
  char *expr = (char *)malloc(need);
  snprintf(expr, need, "__tb_begin_load(%s)", jbody);
  free(jbody);

  char *srcs_json = js_inspect(h, expr);
  free(expr);
  if (!srcs_json) {
    fprintf(stderr, "[js_engine] load: begin failed\n");
    return -1;
  }

  /* 阶段二:宿主把每个 <script src> 同步抓下来(复用既有 tb_load_sync),
   * 结果攒进一个 cJSON object,最后序列化交给 __tb_finish_load。
   *
   * 此前这里是字符串手术:map 从 "{}" 起手,每个条目 sprintf("%s,%s:%s",...)
   * 往后追加,最后 map[strlen-1] = '}'。两处都是错的:
   *   - 追加位置在 '{' 之后而不是之前,结果是 {},"k":"v" —— 花括号在左,
   *     后面拖着一串悬空键值,根本不是合法 object;
   *   - map[strlen-1] 想替换「尾 ','」,但此刻末尾是值的收尾引号,替换后把
   *     引号吃掉,JSON 一样烂。实测外部脚本永远抓不到(页面仍渲染,因为
   *     非法对象字面量被 finish_load 当成普通实参报错跳过,页面正文照跑)。
   * 改用 cJSON 拼,序列化交给 cJSON_PrintUnformatted,转义/括号一律不手写。
   *
   * src 相对路径按 base_url 解析(HTML 语义:<script src> 相对文档 URL)。
   * 拿不到 base(直接调 seam 的测试)时按绝对 URL 处理,解析失败就跳过该条。
   *
   * srcs_json 是 js_inspect 里 strdup 出来的堆串,归本函数 free。 */
  cJSON *mapobj = cJSON_CreateObject();
  cJSON *srcs = cJSON_Parse(srcs_json);
  if (srcs && cJSON_IsArray(srcs)) {
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, srcs) {
      if (!cJSON_IsString(it) || !it->valuestring) continue;
      const char *ref = it->valuestring;
      char *abs = tb_url_resolve(base_url, ref);
      if (!abs) continue;                    /* 解析不了:不猜,跳过这条 */
      char *code = NULL;
      tb_err err = h->host ? tb_load_sync(h->host, abs, &code) : (tb_err){0};
      if (err.code == 0 && code) cJSON_AddStringToObject(mapobj, ref, code);
      free(code);
      free(abs);
    }
  }
  cJSON_Delete(srcs);
  free(srcs_json);
  char *map = cJSON_PrintUnformatted(mapobj);
  cJSON_Delete(mapobj);
  if (!map) return -1;

  /* 阶段三:__tb_finish_load(map) → 跑脚本 + 置 readyState */
  size_t n2 = strlen(map) + 64;
  char *expr2 = (char *)malloc(n2);
  snprintf(expr2, n2, "__tb_finish_load(%s)", map);
  free(map);

  char *res = NULL;
  int rc = js_eval(h, expr2, strlen(expr2), &res);
  free(expr2);
  free(res);
  if (rc != 0) {
    fprintf(stderr, "[js_engine] load: finish failed\n");
    return -1;
  }
  return 0;
}

static int js_engine_render(const struct tb_js_engine *self, void *handle,
                            const char *url, int status, struct tb_view *out) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->rt || !out) return -1;

  /* __tb_doc 若为 null 说明没加载过文档。用 eval(op:"eval" 不 stringify,
   * 原样拿布尔);inspect 会把 true 变成 JSON 文本 "true",再被回执编码成
   * "\"true\"",直接 strcmp 会误判成未加载。 */
  static const char TB_HAS_DOC_EXPR[] = "String(__tb_doc !== null)";
  char *has = NULL;
  if (js_eval(h, TB_HAS_DOC_EXPR, sizeof(TB_HAS_DOC_EXPR) - 1, &has) != 0 || !has) {
    free(has);
    return -1;
  }
  int loaded = strcmp(has, "true") == 0;
  free(has);
  if (!loaded) return -1;

  /* tb_render_js(doc, url, status) → JSON.stringify → 宿主填 tb_view。
   * 走 inspect 而非 eval:inspect 自带 JSON.stringify,省掉在 JS 侧拼
   * JSON.stringify 的那层包装。 */
  char *jurl = json_string(url ? url : "", url ? strlen(url) : 0);
  if (!jurl) return -1;
  size_t need = strlen(jurl) + 96;
  char *expr = (char *)malloc(need);
  snprintf(expr, need, "tb_render_js(__tb_doc, %s, %d)", jurl, status);
  free(jurl);

  char *json = js_inspect(h, expr);
  free(expr);
  if (!json) {
    fprintf(stderr, "[js_engine] render failed\n");
    return -1;
  }

  cJSON *root = cJSON_Parse(json);
  free(json);
  if (!root) return -1;

  const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "text");
  const cJSON *ti = cJSON_GetObjectItemCaseSensitive(root, "title");
  const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "status");
  const cJSON *el = cJSON_GetObjectItemCaseSensitive(root, "elems");

  free(out->text);
  free(out->title);
  free(out->url);
  out->text = strdup(cJSON_IsString(t) && t->valuestring ? t->valuestring : "");
  out->title = strdup(cJSON_IsString(ti) && ti->valuestring ? ti->valuestring : "");
  out->url = strdup(url ? url : "");
  out->status = cJSON_IsNumber(st) ? st->valueint : 0;

  free(out->elems);
  out->elems = NULL;
  out->nelems = 0;
  if (cJSON_IsArray(el)) {
    int n = cJSON_GetArraySize(el);
    if (n > 0) {
      out->elems = (struct tb_elem *)calloc((size_t)n, sizeof(struct tb_elem));
      for (int i = 0; i < n; i++) {
        const cJSON *e = cJSON_GetArrayItem(el, i);
        struct tb_elem *o = &out->elems[out->nelems];
        const cJSON *f;
        f = cJSON_GetObjectItemCaseSensitive(e, "id");
        o->id = cJSON_IsNumber(f) ? f->valueint : 0;
        f = cJSON_GetObjectItemCaseSensitive(e, "off");
        o->off = cJSON_IsNumber(f) ? f->valueint : 0;
        f = cJSON_GetObjectItemCaseSensitive(e, "type");
        o->type = strdup(cJSON_IsString(f) && f->valuestring ? f->valuestring : "");
        f = cJSON_GetObjectItemCaseSensitive(e, "text");
        o->text = strdup(cJSON_IsString(f) && f->valuestring ? f->valuestring : "");
        f = cJSON_GetObjectItemCaseSensitive(e, "href");
        o->href = strdup(cJSON_IsString(f) && f->valuestring ? f->valuestring : "");
        f = cJSON_GetObjectItemCaseSensitive(e, "name");
        o->name = strdup(cJSON_IsString(f) && f->valuestring ? f->valuestring : "");
        f = cJSON_GetObjectItemCaseSensitive(e, "value");
        o->value = strdup(cJSON_IsString(f) && f->valuestring ? f->valuestring : "");
        /* select 的 options 数组 */
        f = cJSON_GetObjectItemCaseSensitive(e, "options");
        if (cJSON_IsArray(f)) {
          int no = cJSON_GetArraySize(f);
          if (no > 0) {
            o->options = (const char **)calloc((size_t)no, sizeof(char *));
            for (int k = 0; k < no; k++) {
              const cJSON *o1 = cJSON_GetArrayItem(f, k);
              o->options[o->noptions++] =
                  strdup(cJSON_IsString(o1) && o1->valuestring ? o1->valuestring : "");
            }
          }
        }
        out->nelems++;
      }
    }
  }
  cJSON_Delete(root);
  return 0;
}

/* qzjs 在自有线程上跑自己的 loop,定时器由它自己触发。宿主这侧只需把邮箱里
 * 积压的 console 帧派发出去(否则 on_console 要等到下一次控制面往返才触发)。 */
static void js_engine_poll_timers(const struct tb_js_engine *self, void *handle) {
  (void)self;
  js_handle *h = (js_handle *)handle;
  if (!h || !h->rt) return;
  for (;;) {
    char *json = NULL;
    size_t len = 0;
    if (qz_recv_message(h->rt, &json, &len, 0) != 0 || !json) break;
    js_drain_one(h, json);
  }
}

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