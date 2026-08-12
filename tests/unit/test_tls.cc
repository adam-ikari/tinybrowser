#include "tb.h"
#include "tls_server.h"
#include <gtest/gtest.h>

// 测试从 build 目录运行(cwd 不在源树),证书路径由 CMake 注入的绝对源目录拼接。
#ifndef TB_TEST_SRC_DIR
#error "TB_TEST_SRC_DIR must be defined (absolute tests/ source dir)"
#endif
#define CERT_PATH(name) TB_TEST_SRC_DIR "certs/" name

TEST(Tls, HttpsFetchWithPinnedCA) {
  TlsServer srv(CERT_PATH("server.crt"), CERT_PATH("server.key"));
  tb_config cfg{};
  cfg.user_agent = "tb-tls-test";
  cfg.idle_grace_ms = 300;
  cfg.ca_bundle_path = CERT_PATH("ca.crt");
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 8000), 0);
  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_EQ(tb_view_not_renderable(v), 0);
  EXPECT_STREQ(tb_view_title(v), "TLS OK");
  EXPECT_STREQ(tb_view_text(v), "secure");
  tb_view_free(v);
  tb_destroy(b);
}

TEST(Tls, RejectsUntrustedCA) {
  TlsServer srv(CERT_PATH("server.crt"), CERT_PATH("server.key"));
  tb_config cfg{};
  cfg.user_agent = "tb-tls-test";
  cfg.idle_grace_ms = 300;
  // 不给 CA:验证失败 → 导航报错。断言宽容(未空闲/无视图/不可渲染任一即可)。
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  int r = tb_wait_idle(b, 8000);
  tb_view *v = nullptr;
  tb_observe(b, &v);
  EXPECT_TRUE(r == 1 || v == nullptr || tb_view_not_renderable(v));
  if (v) tb_view_free(v);
  tb_destroy(b);
}
