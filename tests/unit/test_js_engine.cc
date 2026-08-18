#include "tb.h"
#include "view.h"
#include "browser_internal.h"
#include "js_engine.h"
#include "fakes.h"
#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>

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

TEST(JsEngine, ConsoleBridgeWithHost) {
  g_console_calls = 0;
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
  eng->eval(eng, h, "__tb_console('warn', 'test msg')", &out);
  free(out);
  EXPECT_EQ(g_console_calls, 1);
  EXPECT_STREQ(g_console_msg, "test msg");
  eng->close(eng, h);
  tb_destroy(b);
}

/* ---- tb_eval_js over browser (no document loaded) ---- */

TEST(JsEngine, EvalJsOverBrowserNoDoc) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "t";
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  tb_browser *b = tb_create(&cfg);
  char *out = nullptr;
  EXPECT_EQ(tb_eval_js(b, "1", &out).code, TB_ERR_NO_VIEW);
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

/* ---- timer set/cancel (host=NULL path, no-op) ---- */

TEST(JsEngine, TimerNoOpWithoutHost) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  char *out = nullptr;
  eng->eval(eng, h, "var tid = __tb_timer_set(100, function(){}); tid", &out);
  /* should return 0 (no-op) */
  EXPECT_STREQ(out, "0");
  free(out);
  eng->poll_timers(eng, h);
  eng->close(eng, h);
}

/* ---- memory limit ---- */

TEST(JsEngine, MemoryLimitTriggersOOM) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  /* open with a restrictive memory limit */
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  cfg.js_memory_limit = 64 * 1024;
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
