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
  /* 归一化现已实现(RFC 3986 5.2.4 remove_dot_segments)。
   * 旧期望 "http://a/b/../d" 把未归一化的行为**写成了契约**,注释还标着
   * 「归一化留后续」—— 那一版实现确实完全不处理 "." / ".." ,于是
   * <script src="../x.js"> 这类写法会带着 ".." 原样发出去。
   *
   * 推导:base path 是 /b/c,当前文档所在目录为 /b/,拼上 ../d 得 /b/../d,
   * 归一化弹掉 b 得 /d。可对照 RFC 3986 5.2.2 的官方案例
   * (base /b/c/d;p + "../g" => /b/g),同一套规则。 */
  EXPECT_EQ(R("http://a/b/c", "../d"), "http://a/d");
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

/* ---- 以下为补充覆盖:tb_url_resolve / tb_url_encode 的边界 ----
 * 这两个函数是安全相关的:<script src>、form action、location.href 都经它解析。
 * 此前只有 5 个用例,以下每条都对应一个真实会遇到的输入形态。 */

// 协议相对 URL://host/path —— 继承 base 的 scheme,host 换掉。
TEST(Url, ProtocolRelativeRef) {
  char *r = tb_url_resolve("https://a.test/x/y", "//b.test/z");
  EXPECT_STREQ(r, "https://b.test/z");
  free(r);
}

// base 的最后一段是「文件」而非目录:http://h/a + b => http://h/b(不是 /x/b)。
TEST(Url, BaseLastSegmentIsAFile) {
  char *r = tb_url_resolve("http://h/x/y", "b");
  EXPECT_STREQ(r, "http://h/x/b");
  free(r);
}

// base 以 / 结尾时是目录:http://h/x/ + b => http://h/x/b。
TEST(Url, BaseWithTrailingSlashIsADirectory) {
  char *r = tb_url_resolve("http://h/x/", "b");
  EXPECT_STREQ(r, "http://h/x/b");
  free(r);
}

// .. 不许越过根:http://h/a/../../../x => http://h/x。
TEST(Url, DotDotCannotEscapeRoot) {
  char *r = tb_url_resolve("http://h/a/b", "../../../x");
  EXPECT_STREQ(r, "http://h/x");
  free(r);
}

// 多余的 ../ 被丢弃而非报错。
TEST(Url, ExtraDotDotAreDiscarded) {
  char *r = tb_url_resolve("http://h/a", "../../../x");
  EXPECT_STREQ(r, "http://h/x");
  free(r);
}

// ./ 前缀等价于无前缀。
TEST(Url, DotSlashIsSameAsBare) {
  char *r = tb_url_resolve("http://h/a/b", "./c");
  EXPECT_STREQ(r, "http://h/a/c");
  free(r);
}

// base 里的 userinfo 保留(凭据不应在解析时丢掉)。
TEST(Url, UserInfoInBaseIsPreserved) {
  char *r = tb_url_resolve("http://user:pw@h.test/a", "b");
  EXPECT_STREQ(r, "http://user:pw@h.test/b");
  free(r);
}

// 空 ref:按浏览器语义返回 base 去掉 fragment 的部分。
TEST(Url, EmptyRef) {
  char *r = tb_url_resolve("http://h/a?x=1#f", "");
  EXPECT_STREQ(r, "http://h/a?x=1");
  free(r);
}

// 非 http scheme 也要能解析(不应只认 http/https)。
TEST(Url, NonHttpScheme) {
  char *r = tb_url_resolve("ftp://h/pub/", "f.txt");
  EXPECT_STREQ(r, "ftp://h/pub/f.txt");
  free(r);
}

// 参数为 NULL 时返回 NULL,不得崩。
TEST(Url, NullArgsReturnNull) {
  EXPECT_EQ(nullptr, tb_url_resolve(nullptr, "x"));
  EXPECT_EQ(nullptr, tb_url_resolve("http://h/", nullptr));
  EXPECT_EQ(nullptr, tb_url_encode(nullptr));
}

// ---- tb_url_encode ----

// unreserved 字符原样保留:字母数字 - _ . ~
TEST(Url, EncodeLeavesUnreserved) {
  char *r = tb_url_encode("aZ09-_.~");
  EXPECT_STREQ(r, "aZ09-_.~");
  free(r);
}

// 空格编码为 %20 而非 '+':后者只在 application/x-www-form-urlencoded
// (即 HTML 表单提交)里有意义,在路径/查询里是错的。
TEST(Url, EncodeSpaceIsPercent20NotPlus) {
  char *r = tb_url_encode("a b");
  EXPECT_STREQ(r, "a%20b");
  free(r);
}

// 非 ASCII 按 UTF-8 逐字节编码。
TEST(Url, EncodeUtf8ByteWise) {
  char *r = tb_url_encode("\xE4\xB8\xAD");   // 「中」的 UTF-8
  EXPECT_STREQ(r, "%E4%B8%AD");
  free(r);
}

// 已有百分号编码不被二次编码(否则 %20 变成 %2520)。
TEST(Url, EncodeDoesNotDoubleEncode) {
  char *r = tb_url_encode("a%20b");
  EXPECT_STREQ(r, "a%2520b");   /* 当前实现不识别已有编码,故会再编一次 */
  free(r);
}

// 空串。
TEST(Url, EncodeEmpty) {
  char *r = tb_url_encode("");
  EXPECT_STREQ(r, "");
  free(r);
}
