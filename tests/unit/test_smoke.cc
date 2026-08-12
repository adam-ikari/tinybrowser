#include "tb.h"
#include "fakes.h"
#include <gtest/gtest.h>

TEST(Smoke, CreateDestroy) {
  // Task10: tb_create 需要 transport(无则返回 NULL)。
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg{};
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);
  tb_destroy(b);
}

TEST(Smoke, HeaderIsCppSafe) {
  // 编译期验证 extern "C" 包裹有效(gtest 是 C++ 编译单元)。
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg{};
  cfg.user_agent = "test";
  cfg.clock = &fc.base;
  cfg.transport = &ft.base;
  tb_browser *b = tb_create(&cfg);
  tb_destroy(b);
}
