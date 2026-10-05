#include "tb.h"
#include "view.h"
#include "browser_internal.h"
#include "js_engine.h"
#include "fakes.h"
#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>
#include <thread>
#include <chrono>

/* ---- seam basics: open / eval / close ---- */

TEST(JsEngine, DefaultEngineOpenEvalClose) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  ASSERT_NE(eng, nullptr);
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  ASSERT_EQ(eng->eval(eng, h, "1 + 1", &out), 0);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "2");
  free(out);
  eng->close(eng, h);
}

TEST(JsEngine, EvalReturnsString) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  char *out = nullptr;
  ASSERT_EQ(eng->eval(eng, h, "'hello'", &out), 0);
  EXPECT_STREQ(out, "hello");
  free(out);
  eng->close(eng, h);
}

TEST(JsEngine, EvalErrorReturnsNonZero) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  char *out = nullptr;
  EXPECT_NE(eng->eval(eng, h, "throw new Error('boom')", &out), 0);
  ASSERT_NE(out, nullptr);
  /* message should contain the error text */
  EXPECT_TRUE(strstr(out, "boom") != nullptr);
  free(out);
  eng->close(eng, h);
}

/* ---- console bridge: host=NULL → no crash ---- */

static int g_console_calls;
static char g_console_msg[256];

static void test_on_console(tb_browser *b, const char *level, const char *msg, void *ud) {
  (void)b; (void)level; (void)ud;
  g_console_calls++;
  strncpy(g_console_msg, msg, sizeof g_console_msg - 1);
  g_console_msg[sizeof g_console_msg - 1] = '\0';
}

TEST(JsEngine, ConsoleBridgeWithoutHost) {
  /* host=NULL → __tb_console is a no-op, no crash */
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  char *out = nullptr;
  eng->eval(eng, h, "__tb_console('log', 'hi')", &out);
  free(out);
  eng->close(eng, h);
}

/* console 桥:旧实现是宿主 C 函数 __tb_console 直调 cfg.on_console。
 * 现在 qzjs 从不回调宿主,console 输出走 postMessage 进邮箱,由宿主在控制面
 * 往返途中(或 poll_timers)派发。所以 console.warn 之后紧接着的 eval 就应该
 * 已经把消息带回来了 —— 不需要额外 pump。 */
TEST(JsEngine, ConsoleBridgeWithHost) {
  g_console_calls = 0;
  g_console_msg[0] = '\0';
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "t";
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  cfg.on_console = test_on_console;
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);
  /* open engine with host so on_console is wired */
  const struct tb_js_engine *eng = b->engine;
  void *h = eng->open(eng, b);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  eng->eval(eng, h, "console.warn('test msg')", &out);
  free(out);
  EXPECT_EQ(g_console_calls, 1);
  EXPECT_STREQ(g_console_msg, "test msg");
  eng->close(eng, h);
  tb_destroy(b);
}

/* console 消息若先于回执到达邮箱,派发顺序不能把 correl 对错帧:
 * 一次 eval 里先 console 再返回值,两条都得正确归位。 */
TEST(JsEngine, ConsoleMessageDoesNotCorruptReceipt) {
  g_console_calls = 0;
  g_console_msg[0] = '\0';
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "t";
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  cfg.on_console = test_on_console;
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);
  const struct tb_js_engine *eng = b->engine;
  void *h = eng->open(eng, b);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  ASSERT_EQ(eng->eval(eng, h, "console.log('a'); 'the-result'", &out), 0);
  EXPECT_STREQ(out, "the-result");   /* 回执没被 console 帧串味 */
  EXPECT_EQ(g_console_calls, 1);
  EXPECT_STREQ(g_console_msg, "a");
  free(out);
  eng->close(eng, h);
  tb_destroy(b);
}

/* ---- tb_eval_js over browser, before any document is loaded ----
 * 语义变更(M2a 引擎驱动):引擎现在由 tb_create 就建立并持有,不再等到首次
 * 导航。所以「没有文档」不再意味着「没有引擎」——
 *   - 纯 JS 表达式可以正常求值;
 *   - 依赖 document 的操作仍会失败,但因为没有文档(document._root 为 null),
 *     而不是因为引擎不存在。
 * tb_config.js_memory_limit 之类旧旋钮同理不再适用(见 brain 记录)。 */

TEST(JsEngine, EvalJsOverBrowserNoDoc) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "t";
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);

  /* 引擎已就绪 → 纯表达式可求值 */
  char *out = nullptr;
  EXPECT_EQ(tb_eval_js(b, "1 + 2", &out).code, 0);
  EXPECT_STREQ(out, "3");
  free(out);

  /* 但确实还没有文档:引擎活着,文档是空的 */
  out = nullptr;
  EXPECT_EQ(tb_eval_js(b, "String(document._root === null)", &out).code, 0);
  EXPECT_STREQ(out, "true");
  free(out);

  /* 无文档时 render 必须拒绝,而不是渲染出一个空壳视图 */
  tb_view *v = tb_view_new();
  EXPECT_EQ(b->engine->render(b->engine, b->js_doc, "http://x/", 200, v), -1);
  tb_view_free(v);

  /* JS 语法错误照常如实上报 */
  out = nullptr;
  EXPECT_EQ(tb_eval_js(b, "this is ( not js", &out).code, TB_ERR_PARSE);
  free(out);

  tb_destroy(b);
}

/* ---- load_document + render: simple HTML ---- */

TEST(JsEngine, LoadAndRenderSimple) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);

  const char *html = "<title>Test</title><p>Hello world</p>";
  ASSERT_EQ(eng->load_document(eng, h, html, strlen(html)), 0);

  tb_view *v = tb_view_new();
  ASSERT_EQ(eng->render(eng, h, "http://test/", 200, v), 0);
  EXPECT_STREQ(v->title, "Test");
  ASSERT_NE(v->text, nullptr);
  EXPECT_TRUE(strstr(v->text, "Hello world") != nullptr);
  tb_view_free(v);
  eng->close(eng, h);
}

/* ---- load_document with inline script ---- */

TEST(JsEngine, ScriptExecutesInline) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);

  const char *html = "<title>S</title><script>document.title = 'Changed';</script><p>ok</p>";
  ASSERT_EQ(eng->load_document(eng, h, html, strlen(html)), 0);

  tb_view *v = tb_view_new();
  ASSERT_EQ(eng->render(eng, h, "http://x/", 200, v), 0);
  EXPECT_STREQ(v->title, "Changed");
  tb_view_free(v);
  eng->close(eng, h);
}

/* ---- script error isolation: page still renders after script throw ---- */

TEST(JsEngine, ScriptErrorIsolated) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);

  const char *html = "<title>E</title><script>throw new Error('boom')</script><p>still ok</p>";
  ASSERT_EQ(eng->load_document(eng, h, html, strlen(html)), 0);

  tb_view *v = tb_view_new();
  ASSERT_EQ(eng->render(eng, h, "http://x/", 200, v), 0);
  EXPECT_STREQ(v->title, "E");
  EXPECT_TRUE(strstr(v->text, "still ok") != nullptr);
  tb_view_free(v);
  eng->close(eng, h);
}

/* ---- timers ----
 * 旧实现里 setTimeout 是宿主 C 桥(__tb_timer_set),宿主自己跑链表。
 * 现在定时器归 qzjs polyfill(它有自有 event loop),qzjs 又从不回调宿主,
 * 所以宿主侧不再有 timer 链表——这里验的是 polyfill 的定时器确实可用,
 * 且没有 host 时引擎照样能开、能注册、能取消。 */

TEST(JsEngine, SetTimeoutIsProvidedByPolyfill) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  /* 注册 + 取消,确认句柄可用且不抛 */
  ASSERT_EQ(eng->eval(eng, h,
      "var tid = setTimeout(function(){}, 100000);"
      "clearTimeout(tid);"
      "typeof tid", &out), 0);
  EXPECT_STREQ(out, "number");
  free(out);
  eng->poll_timers(eng, h);
  eng->close(eng, h);
}

/* 定时器回调跑在 qzjs 自有线程上,轮询几次后应已执行。 */
TEST(JsEngine, TimerCallbackFires) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  ASSERT_EQ(eng->eval(eng, h,
      "globalThis.__fired = false;"
      "setTimeout(function(){ __fired = true; }, 1);"
      "typeof __fired", &out), 0);
  free(out);

  for (int i = 0; i < 200; i++) {
    eng->poll_timers(eng, h);
    out = nullptr;
    if (eng->eval(eng, h, "String(__fired)", &out) == 0 && out &&
        strcmp(out, "true") == 0) {
      free(out);
      eng->close(eng, h);
      SUCCEED();
      return;
    }
    free(out);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  eng->close(eng, h);
  FAIL() << "timer callback did not fire within 1s";
}

/* ---- OOM ----
 * qzjs 不暴露 JS_SetMemoryLimit(无内存上限旋钮),所以这里不再是「配置限额触发
 * OOM」,而是「引擎自身在堆耗尽时抛 JS 异常,并如实以 eval 失败回传」。 */

TEST(JsEngine, OutOfMemorySurfacesAsError) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  tb_browser *b = tb_create(&cfg);
  void *h = eng->open(eng, b);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  int rc = eng->eval(eng, h,
    "var a=[]; while(true) a.push('x'.repeat(1024));",
    &out);
  EXPECT_NE(rc, 0);
  free(out);
  eng->close(eng, h);
  tb_destroy(b);
}
