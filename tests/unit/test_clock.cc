#include "tb.h"
#include "fakes.h"
#include <gtest/gtest.h>

TEST(Clock, RealAdvances) {
  uint64_t a = tb_clock_real.now_ms(&tb_clock_real);
  uint64_t b = tb_clock_real.now_ms(&tb_clock_real);
  // 两次调用间隔极小,但不能断言相等(可能同毫秒);断言单调不减。
  EXPECT_GE(b, a);
}

TEST(Clock, FakeIsDeterministic) {
  fake_clock fc;
  fake_clock_init(&fc);
  EXPECT_EQ(fc.now, 1000000u);
  fake_clock *p = &fc;
  fc.now += 7;
  EXPECT_EQ(p->now, 1000007u);
}

TEST(Transport, FakeDeliversInOrderAndDeferred) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);

  fake_resp r = {};
  r.url = "http://x/"; r.status = 200; r.content_type = "text/html";
  r.body = "<p>hello</p>"; r.delay_ms = 5;
  ft.responses = &r; ft.nresponses = 1;

  struct S { int headers, body, done; const char *ct; } s = {};
  tb_transport_req req = {};
  req.method = "GET"; req.url = "http://x/page";
  req.on_headers = [](void *ud, int status, const char *ct, int attachment, const char *final_url) {
    auto *d = (S *)ud; (void)status; (void)attachment; (void)final_url;
    d->headers++; d->ct = ct;
  };
  req.on_body = [](void *ud, const char *data, size_t len) {
    auto *d = (S *)ud; (void)data; (void)len; d->body++;
  };
  req.on_done = [](void *ud, tb_err err) { auto *d = (S *)ud; d->done++; EXPECT_EQ(err.code, 0); };
  req.ud = &s;
  void *op = ft.base.open(&ft.base, &req);

  // fake_poll 每次推进 1ms:open 于 t=1000000,delay=5 → t=1000005 到期。
  ft.base.poll(&ft.base);          // now=1000001
  EXPECT_EQ(s.done, 0);            // 5ms 未到
  for (int i = 0; i < 3; i++) ft.base.poll(&ft.base);  // now=1000004,仍未到
  EXPECT_EQ(s.done, 0);
  ft.base.poll(&ft.base);          // now=1000005,到期→一次 poll 派发
  EXPECT_EQ(s.done, 1);
  EXPECT_EQ(s.headers, 1);
  EXPECT_EQ(s.body, 1);
  ASSERT_NE(s.ct, nullptr);
  EXPECT_STREQ(s.ct, "text/html");

  ft.base.cancel(&ft.base, op);
}

TEST(Transport, FakeErrorInjection) {
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  fake_resp r = {};
  r.url = "http://x/"; r.err_code = 1; r.err_msg = "boom";
  ft.responses = &r; ft.nresponses = 1;

  struct S { int done; tb_err got; } s = {};
  tb_transport_req req = {};
  req.method = "GET"; req.url = "http://x/";
  req.on_done = [](void *ud, tb_err err) { auto *d = (S *)ud; d->done++; d->got = err; };
  req.ud = &s;
  ft.base.open(&ft.base, &req);
  fc.now += 1;
  ft.base.poll(&ft.base);
  EXPECT_EQ(s.done, 1);
  EXPECT_EQ(s.got.code, 1);
  EXPECT_STREQ(s.got.msg, "boom");
}
