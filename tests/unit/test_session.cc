#include "session.h"
#include "fakes.h"
#include <gtest/gtest.h>

TEST(Session, IdleRequiresGraceAfterNet) {
  fake_clock fc; fake_clock_init(&fc);
  tb_session s; tb_session_init(&s, &fc.base, 300);
  tb_session_touch_net(&s);
  EXPECT_FALSE(tb_session_idle(&s));     // 距网络事件 0ms
  fc.now += 299;
  EXPECT_FALSE(tb_session_idle(&s));
  fc.now += 1;
  EXPECT_TRUE(tb_session_idle(&s));
  tb_session_free(&s);
}

TEST(Session, PendingBlocksIdle) {
  fake_clock fc; fake_clock_init(&fc);
  tb_session s; tb_session_init(&s, &fc.base, 0);
  tb_session_nav_start(&s, "http://a");
  fc.now += 1000;
  EXPECT_FALSE(tb_session_idle(&s));
  tb_session_nav_done(&s, "http://a", "A", 200);
  fc.now += 1;
  EXPECT_TRUE(tb_session_idle(&s));
  tb_session_free(&s);
}

TEST(Session, HistoryBackForwardAndClearFwd) {
  fake_clock fc; fake_clock_init(&fc);
  tb_session s; tb_session_init(&s, &fc.base, 0);
  tb_session_nav_start(&s, "http://a");
  tb_session_nav_done(&s, "http://a", "A", 200);
  tb_session_nav_start(&s, "http://b");
  tb_session_nav_done(&s, "http://b", "B", 200);
  EXPECT_TRUE(tb_session_can_back(&s));
  EXPECT_FALSE(tb_session_can_fwd(&s));
  tb_session_back(&s);
  tb_session_nav_done(&s, "http://a", "A", 200);
  EXPECT_STREQ(tb_session_url(&s), "http://a");
  EXPECT_TRUE(tb_session_can_fwd(&s));
  // 新导航清空前进栈
  tb_session_nav_start(&s, "http://c");
  EXPECT_FALSE(tb_session_can_fwd(&s));
  tb_session_free(&s);
}
