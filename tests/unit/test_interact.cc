#include "tb.h"
#include "fakes.h"
#include <gtest/gtest.h>
#include <string.h>
#include <vector>
#include <string>

// 拦截 fake transport 的 open,记录请求方法/URL/body
struct ReqLog { std::string method, url, body; };
static std::vector<ReqLog> g_log;
static fake_transport *g_ft;   // wrap transport 委托的真实 fake

static void *log_open(const tb_transport *self, const tb_transport_req *req) {
  (void)self;
  ReqLog l; l.method = req->method; l.url = req->url;
  l.body = req->body ? req->body : "";
  g_log.push_back(l);
  return g_ft->base.open(&g_ft->base, req);
}
static void log_cancel(const tb_transport *self, void *op) {
  (void)self;
  g_ft->base.cancel(&g_ft->base, op);
}
static void log_poll(const tb_transport *self) {
  (void)self;
  g_ft->base.poll(&g_ft->base);
}
/* destroy 留 NULL:wrap 借用 fake_transport 内嵌的 base,而 fake 不持有
 * 需释放的堆资源(tb_destroy 只销毁 tb_create 自建的 transport,注入的这
 * 个不在其列)。写成具名字段而非位置初始化,新增槽位时不会被静默错位。 */
static tb_transport wrap = {
  .open = log_open,
  .cancel = log_cancel,
  .poll = log_poll,
  .destroy = NULL,
};

static tb_browser *setup(fake_transport *ft, fake_clock *fc, const char *html) {
  static fake_resp r;
  r = {}; r.url = "http://x/"; r.status = 200; r.content_type = "text/html";
  r.body = html;
  ft->responses = &r; ft->nresponses = 1;
  g_ft = ft;
  g_log.clear();
  static tb_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "t"; cfg.idle_grace_ms = 300;
  cfg.clock = &fc->base; cfg.transport = &wrap;
  return tb_create(&cfg);
}

TEST(Interact, ClickLinkNavigates) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_browser *b = setup(&ft, &fc,
    "<title>T</title><a href='/about'>About</a>");
  ASSERT_EQ(tb_navigate(b, "http://x/").code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  tb_view *v; tb_observe(b, &v);
  struct tb_elem e{}; tb_view_elem(v, 0, &e);
  int link_id = e.id;
  tb_view_free(v);
  ASSERT_EQ(tb_click(b, link_id).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  ASSERT_GE(g_log.size(), 2u);
  EXPECT_STREQ(g_log.back().url.c_str(), "http://x/about");
  tb_destroy(b);
}

TEST(Interact, FillThenGetSubmitBuildsQuery) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_browser *b = setup(&ft, &fc,
    "<form action='/search'><input name='q'><input name='lang' value='en'>"
    "<button>Go</button></form>");
  ASSERT_EQ(tb_navigate(b, "http://x/").code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  tb_view *v; tb_observe(b, &v);
  struct tb_elem e{};
  tb_view_elem(v, 1, &e);   // input q
  int qid = e.id;
  tb_view_elem(v, 3, &e);   // button
  int bid = e.id;
  tb_view_free(v);

  ASSERT_EQ(tb_fill(b, qid, "hello world").code, 0);
  ASSERT_EQ(tb_click(b, bid).code, 0);   // button → submit form
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  ASSERT_GE(g_log.size(), 2u);
  const ReqLog &r = g_log.back();
  EXPECT_STREQ(r.method.c_str(), "GET");
  EXPECT_STREQ(r.url.c_str(), "http://x/search?q=hello%20world&lang=en");
  tb_destroy(b);
}

TEST(Interact, PostSubmitSendsBody) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_browser *b = setup(&ft, &fc,
    "<form action='/s' method='post'><input name='a' value='1'>"
    "<input name='b'><button>Go</button></form>");
  ASSERT_EQ(tb_navigate(b, "http://x/").code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  tb_view *v; tb_observe(b, &v);
  struct tb_elem e{};
  tb_view_elem(v, 1, &e);   // input a
  int aid = e.id;
  tb_view_elem(v, 3, &e);   // button
  int bid = e.id;
  tb_view_free(v);
  ASSERT_EQ(tb_fill(b, aid, "7").code, 0);
  ASSERT_EQ(tb_click(b, bid).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  const ReqLog &r = g_log.back();
  EXPECT_STREQ(r.method.c_str(), "POST");
  EXPECT_STREQ(r.body.c_str(), "a=7&b=");
  tb_destroy(b);
}

TEST(Interact, SelectThenSubmitIncludesOption) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_browser *b = setup(&ft, &fc,
    "<form action='/s' method='post'><select name='lang'>"
    "<option>en</option><option>zh</option></select><button>Go</button></form>");
  ASSERT_EQ(tb_navigate(b, "http://x/").code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  tb_view *v; tb_observe(b, &v);
  struct tb_elem e{};
  tb_view_elem(v, 1, &e);  // select
  int sel_id = e.id;
  tb_view_elem(v, 2, &e);  // button
  int bid = e.id;
  tb_view_free(v);
  ASSERT_EQ(tb_select(b, sel_id, "zh").code, 0);
  ASSERT_EQ(tb_click(b, bid).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  const ReqLog &r = g_log.back();
  EXPECT_STREQ(r.method.c_str(), "POST");
  EXPECT_NE(r.body.find("lang=zh"), std::string::npos);
  tb_destroy(b);
}

TEST(Interact, BackForwardHistory) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_browser *b = setup(&ft, &fc, "<a href='/2'>2</a>");
  ASSERT_EQ(tb_navigate(b, "http://x/1").code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  // 点链接到 /2
  tb_view *v; tb_observe(b, &v);
  struct tb_elem e{}; tb_view_elem(v, 0, &e);
  tb_view_free(v);
  ASSERT_EQ(tb_click(b, e.id).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  EXPECT_STREQ(g_log.back().url.c_str(), "http://x/2");
  ASSERT_EQ(tb_back(b).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  EXPECT_STREQ(g_log.back().url.c_str(), "http://x/1");
  ASSERT_EQ(tb_forward(b).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 1000), 0);
  EXPECT_STREQ(g_log.back().url.c_str(), "http://x/2");
  tb_destroy(b);
}
