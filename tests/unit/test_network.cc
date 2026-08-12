#include "tb.h"
#include "http_server.h"
#include <gtest/gtest.h>
#include <string>

TEST(Network, FetchRealPageEndToEnd) {
  HttpServer srv({
    {"/", {200, "text/html", "<title>Net</title><p>hello network</p>"}},
    {"/plain", {200, "text/plain", "just text"}},
    {"/bin", {200, "application/octet-stream", "\x00\x01\x02binary"}},
    {"/redirect", {302, "text/html", ""}},
    {"/target", {200, "text/html", "<title>Redirected</title>"}},
  });
  tb_config cfg{};
  cfg.idle_grace_ms = 300;
  cfg.user_agent = "tb-test";
  tb_browser *b = tb_create(&cfg);        // 无显式 transport → 默认 curl
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Net");
  EXPECT_STREQ(tb_view_text(v), "hello network");
  tb_view_free(v);

  // 纯文本
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/plain").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  tb_observe(b, &v);
  EXPECT_EQ(tb_view_not_renderable(v), 0);
  EXPECT_STREQ(tb_view_text(v), "just text");
  tb_view_free(v);

  // 二进制 → 不可渲染,不解析
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/bin").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  tb_observe(b, &v);
  EXPECT_EQ(tb_view_not_renderable(v), 1);
  EXPECT_STREQ(tb_view_not_renderable_type(v), "application/octet-stream");
  tb_view_free(v);

  // 重定向跟随,最终 URL 是 /target
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/redirect").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  tb_observe(b, &v);
  EXPECT_STREQ(tb_view_title(v), "Redirected");
  EXPECT_EQ(tb_view_status(v), 200);
  tb_view_free(v);

  tb_destroy(b);
}
