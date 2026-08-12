#include "tb.h"
#include "fakes.h"
#include <gtest/gtest.h>
#include <string.h>

/* 返回预填充的静态 cfg:调用方在 tb_create 之前设置回调与 ud,
   因为 tb_create 会按值拷贝 cfg。 */
static tb_config *init_cfg(fake_transport *ft, fake_clock *fc) {
  static tb_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "test-agent";
  cfg.idle_grace_ms = 300;
  cfg.clock = &fc->base;
  cfg.transport = &ft->base;
  return &cfg;
}

struct Sink {
  int view_changed = 0, titles = 0, errors = 0;
  char last_title[256] = {0};
  static void on_view(tb_browser *, void *ud) { ((Sink*)ud)->view_changed++; }
  static void on_title(tb_browser *, const char *t, void *ud) {
    Sink *s = (Sink*)ud; s->titles++; strncpy(s->last_title, t, sizeof s->last_title - 1);
  }
  static void on_error(tb_browser *, tb_err, const char *, void *ud) { ((Sink*)ud)->errors++; }
};

TEST(Browser, NavigateObserveFullFlow) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  fake_resp r = {};
  r.url = "http://x/"; r.status = 200; r.content_type = "text/html";
  r.body = "<title>Hi</title><a href='/about'>About</a>";
  ft.responses = &r; ft.nresponses = 1;

  Sink sink;
  tb_config *cfg = init_cfg(&ft, &fc);
  cfg->on_view_changed = Sink::on_view; cfg->on_title = Sink::on_title;
  cfg->ud = &sink;
  tb_browser *b = tb_create(cfg);

  ASSERT_EQ(tb_navigate(b, "http://x/").code, 0);
  EXPECT_EQ(tb_wait_idle(b, 5000), 0);         // 空闲返回 0
  EXPECT_EQ(sink.titles, 1);
  EXPECT_STREQ(sink.last_title, "Hi");
  EXPECT_GE(sink.view_changed, 1);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Hi");
  EXPECT_STREQ(tb_view_text(v), "About");
  ASSERT_EQ(tb_view_nelems(v), 1);
  struct tb_elem e{};
  tb_view_elem(v, 0, &e);
  EXPECT_STREQ(e.href, "/about");
  tb_view_free(v);
  tb_destroy(b);
}

TEST(Browser, NotRenderableContent) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  fake_resp r = {};
  r.url = "http://x/"; r.status = 200; r.content_type = "application/pdf";
  r.body = "%PDF-1.4 junk";
  ft.responses = &r; ft.nresponses = 1;

  tb_config *cfg = init_cfg(&ft, &fc);
  tb_browser *b = tb_create(cfg);
  ASSERT_EQ(tb_navigate(b, "http://x/doc.pdf").code, 0);
  EXPECT_EQ(tb_wait_idle(b, 1000), 0);
  tb_view *v = nullptr;
  tb_observe(b, &v);
  ASSERT_EQ(tb_view_not_renderable(v), 1);
  EXPECT_STREQ(tb_view_not_renderable_type(v), "application/pdf");
  tb_view_free(v);
  tb_destroy(b);
}

TEST(Browser, NetworkErrorSurface) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  fake_resp r = {};
  r.url = "http://x/"; r.err_code = 1; r.err_msg = "connection refused";
  ft.responses = &r; ft.nresponses = 1;

  Sink sink;
  tb_config *cfg = init_cfg(&ft, &fc);
  cfg->on_error = Sink::on_error; cfg->ud = &sink;
  tb_browser *b = tb_create(cfg);
  ASSERT_EQ(tb_navigate(b, "http://x/").code, 0);
  EXPECT_EQ(tb_wait_idle(b, 1000), 0);
  EXPECT_EQ(sink.errors, 1);
  tb_destroy(b);
}

TEST(Browser, WaitIdleHonorsGrace) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  fake_resp r = {};
  r.url = "http://x/"; r.status = 200; r.content_type = "text/html";
  r.body = "<p>ok</p>";
  ft.responses = &r; ft.nresponses = 1;

  tb_config *cfg = init_cfg(&ft, &fc);
  tb_browser *b = tb_create(cfg);
  tb_navigate(b, "http://x/");
  // fake poll 每次 +1ms;300ms grace 后仍空闲 → 需多次 pump
  EXPECT_EQ(tb_wait_idle(b, 1000), 0);
  tb_destroy(b);
}
