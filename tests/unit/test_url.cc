#include "url.h"
#include "tb.h"
#include <gtest/gtest.h>

static std::string R(const char *b, const char *r) {
  char *s = tb_url_resolve(b, r);
  EXPECT_NE(s, nullptr);
  std::string out = s ? s : "";
  tb_free(s);
  return out;
}

TEST(Url, AbsoluteRefPassthrough) {
  EXPECT_EQ(R("http://a/b", "http://c/d"), "http://c/d");
  EXPECT_EQ(R("http://a/b", "https://x/y?z=1"), "https://x/y?z=1");
}

TEST(Url, AbsolutePath) {
  EXPECT_EQ(R("http://a/b/c", "/d"), "http://a/d");
  EXPECT_EQ(R("http://a/b/c", "/d/e?q=1"), "http://a/d/e?q=1");
}

TEST(Url, RelativePath) {
  EXPECT_EQ(R("http://a/b/c", "d"), "http://a/b/d");
  EXPECT_EQ(R("http://a/b/c", "../d"), "http://a/b/../d");  // 归一化留后续
}

TEST(Url, QueryAndFragmentOnly) {
  EXPECT_EQ(R("http://a/b", "?x=1"), "http://a/b?x=1");
  EXPECT_EQ(R("http://a/b", "#frag"), "http://a/b#frag");
}

TEST(Url, Encode) {
  char *e = tb_url_encode("a b&c=d/e");
  EXPECT_STREQ(e, "a%20b%26c%3Dd%2Fe");
  tb_free(e);
  char *e2 = tb_url_encode("simple");
  EXPECT_STREQ(e2, "simple");
  tb_free(e2);
}
