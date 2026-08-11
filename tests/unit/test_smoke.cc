#include "tb.h"
#include <gtest/gtest.h>

TEST(Smoke, CreateDestroy) {
  tb_browser *b = tb_create(nullptr);
  ASSERT_NE(b, nullptr);
  tb_destroy(b);
}

TEST(Smoke, HeaderIsCppSafe) {
  // 编译期验证 extern "C" 包裹有效(gtest 是 C++ 编译单元)。
  tb_config cfg{};
  cfg.user_agent = "test";
  tb_browser *b = tb_create(&cfg);
  tb_destroy(b);
}
