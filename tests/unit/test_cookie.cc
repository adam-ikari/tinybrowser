/* Cookie jar 单元测试(纯 C 层,不碰网络不碰 JS)。
 *
 * cookie 是本轮新写的全部实现里最容易静默出错的部分:匹配规则错一点不会崩,
 * 只会「cookie 悄悄不生效」或「发到不该去的域」。所以逐条规则钉死。
 */
#include "cookie.h"
#include "tb.h"
#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>

namespace {

/* 设一条 cookie,断言它在某 URL 上可见(header 视角 = 含 HttpOnly)。 */
void ExpectHeader(tb_cookie_jar *j, const char *url, const char *want) {
  char *got = tb_cookie_jar_header(j, url);
  EXPECT_STREQ(got ? got : "(null)", want) << "at " << url;
  free(got);
}

/* 断言 JS 视角(document.cookie)可见 —— 应排除 HttpOnly。 */
void ExpectVisible(tb_cookie_jar *j, const char *url, const char *want) {
  char *got = tb_cookie_jar_visible(j, url);
  EXPECT_STREQ(got ? got : "(null)", want) << "at " << url;
  free(got);
}

}  // namespace

TEST(Cookie, BasicSetAndGet) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=1", "http://example.test/");
  tb_cookie_jar_set(j, "b=2", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "a=1; b=2");
  ExpectHeader(j, "http://example.test/other/page", "a=1; b=2");
  EXPECT_EQ(tb_cookie_jar_count(j), 2);
  tb_cookie_jar_free(j);
}

/* 值的 '=' 必须保留:Base64 token 里满是 '='。 */
TEST(Cookie, ValueMayContainEquals) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "tok=YWJjPT0=", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "tok=YWJjPT0=");
  tb_cookie_jar_free(j);
}

/* 没有 '=' 的不是 cookie,静默丢弃(与浏览器一致)。 */
TEST(Cookie, MalformedIsIgnored) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "justaname", "http://example.test/");
  tb_cookie_jar_set(j, "", "http://example.test/");
  tb_cookie_jar_set(j, "=novalue", "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(j), 0);
  ExpectHeader(j, "http://example.test/", "");
  tb_cookie_jar_free(j);
}

/* 同 (name, domain, path) 应替换而非累加 —— 否则每次刷新页面积累一个同名 cookie。 */
TEST(Cookie, SameNameSameDomainPathReplaces) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "sid=old", "http://example.test/");
  tb_cookie_jar_set(j, "sid=new", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "sid=new");
  EXPECT_EQ(tb_cookie_jar_count(j), 1);
  tb_cookie_jar_free(j);
}

/* 同名但不同 path 是两个 cookie(RFC 6265 5.3)。 */
TEST(Cookie, SameNameDifferentPathCoexist) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "sid=root; Path=/", "http://example.test/");
  tb_cookie_jar_set(j, "sid=deep; Path=/admin", "http://example.test/admin");
  ExpectHeader(j, "http://example.test/", "sid=root");
  ExpectHeader(j, "http://example.test/admin/x", "sid=root; sid=deep");
  EXPECT_EQ(tb_cookie_jar_count(j), 2);
  tb_cookie_jar_free(j);
}

/* Path 前缀必须在路径分量边界上:/foo 不该匹配 /foobar。 */
TEST(Cookie, PathPrefixRespectsComponentBoundary) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=1; Path=/foo", "http://example.test/foo");
  ExpectHeader(j, "http://example.test/foo", "a=1");
  ExpectHeader(j, "http://example.test/foo/bar", "a=1");
  ExpectHeader(j, "http://example.test/foobar", "");   /* ← 关键:不是 /foo 的子路径 */
  tb_cookie_jar_free(j);
}

/* Path 缺省 = 请求 path 的目录部分。 */
TEST(Cookie, PathDefaultsToRequestDirectory) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "d=1", "http://example.test/a/b/page.html");
  ExpectHeader(j, "http://example.test/a/b/page.html", "d=1");
  ExpectHeader(j, "http://example.test/a/b/other", "d=1");
  ExpectHeader(j, "http://example.test/a/x", "");       /* 出了目录 */
  tb_cookie_jar_free(j);
}

/* Domain 缺省 = 请求 host(精确匹配,不跨子域)。 */
TEST(Cookie, DefaultDomainIsExactHost) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "h=1", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "h=1");
  ExpectHeader(j, "http://sub.example.test/", "");      /* 缺省域不跨子域 */
  tb_cookie_jar_free(j);
}

/* Domain=a.com 应匹配 sub.a.com,但不匹配 b.com。 */
TEST(Cookie, ExplicitDomainMatchesSubdomains) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "d=1; Domain=example.test", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "d=1");
  ExpectHeader(j, "http://sub.example.test/", "d=1");
  ExpectHeader(j, "http://deep.sub.example.test/", "d=1");
  ExpectHeader(j, "http://evil.test/", "");
  tb_cookie_jar_free(j);
}

/* 前导 '.' 的 Domain 语义等同无前导(规范化后都按后缀匹配)。 */
TEST(Cookie, LeadingDotInDomainIsIgnored) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "d=1; Domain=.example.test", "http://example.test/");
  ExpectHeader(j, "http://sub.example.test/", "d=1");
  tb_cookie_jar_free(j);
}

/* Domain 是别的站 → 整条拒绝。这是防跨站泄漏的关键一条。 */
TEST(Cookie, ForeignDomainIsRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "evil=1; Domain=other.test", "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(j), 0);
  ExpectHeader(j, "http://example.test/", "");
  /* 兄弟域之间的伪装:example.test 不能给 notexample.test 设 cookie */
  tb_cookie_jar_set(j, "e2=1; Domain=notexample.test", "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(j), 0);
  tb_cookie_jar_free(j);
}

/* Secure:只在 https 上发。 */
TEST(Cookie, SecureOnlyOverHttps) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "s=1; Secure", "https://example.test/");
  ExpectHeader(j, "https://example.test/", "s=1");
  ExpectHeader(j, "http://example.test/", "");
  tb_cookie_jar_free(j);
}

/* HttpOnly:HTTP 上照发,但 document.cookie 读不到。 */
TEST(Cookie, HttpOnlyHiddenFromScript) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "h=1; HttpOnly", "http://example.test/");
  tb_cookie_jar_set(j, "v=2", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "h=1; v=2");
  ExpectVisible(j, "http://example.test/", "v=2");   /* JS 只看得到 v */
  tb_cookie_jar_free(j);
}

/* Max-Age<=0 即删除。 */
TEST(Cookie, MaxAgeZeroDeletes) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "x=1", "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(j), 1);
  tb_cookie_jar_set(j, "x=1; Max-Age=0", "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(j), 0);
  tb_cookie_jar_free(j);
}

/* 已过期的 Expires 不该被收下(过去的日期 → 立即删除语义)。 */
TEST(Cookie, PastExpiresIsDropped) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "old=1; Expires=Wed, 01 Jan 2020 00:00:00 GMT",
                    "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(j), 0);
  /* 未来的日期要留下,并在本机时区无关(内部按 UTC 计算) */
  tb_cookie_jar_set(j, "new=1; Expires=Wed, 01 Jan 2099 00:00:00 GMT",
                    "http://example.test/");
  ExpectHeader(j, "http://example.test/", "new=1");
  tb_cookie_jar_free(j);
}

/* 未来 Max-Age 留下,且不会被后续读取误删。 */
TEST(Cookie, FutureMaxAgeSurvives) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "k=1; Max-Age=3600", "http://example.test/");
  for (int i = 0; i < 3; i++) ExpectHeader(j, "http://example.test/", "k=1");
  EXPECT_EQ(tb_cookie_jar_count(j), 1);
  tb_cookie_jar_free(j);
}

/* 不带属性的裸 cookie 是 session cookie:进程存活期内一直有效。 */
TEST(Cookie, SessionCookieHasNoExpiry) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "sess=1", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "sess=1");
  ExpectHeader(j, "http://example.test/", "sess=1");   /* 再取一次仍在 */
  tb_cookie_jar_free(j);
}

/* 属性里夹空格、大小写混写都要认。 */
TEST(Cookie, AttributeWhitespaceAndCaseInsensitive) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=1;   domain=example.test ;  path=/x ;  secure",
                    "http://example.test/x");
  ExpectHeader(j, "https://sub.example.test/x/y", "a=1");
  ExpectHeader(j, "http://sub.example.test/x/y", "");   /* Secure 不发 http */
  tb_cookie_jar_free(j);
}

/* 多个 cookie 同时命中时的顺序:按插入序。 */
TEST(Cookie, MultipleCookiesKeepInsertionOrder) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "c=3", "http://example.test/");
  tb_cookie_jar_set(j, "a=1", "http://example.test/");
  tb_cookie_jar_set(j, "b=2", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "c=3; a=1; b=2");
  tb_cookie_jar_free(j);
}

/* 空 jar 与 NULL 都要能安全调用 —— browser 里 jar 可能尚未分配。 */
TEST(Cookie, NullJarIsSafe) {
  ExpectHeader(nullptr, "http://example.test/", "");
  tb_cookie_jar_set(nullptr, "a=1", "http://example.test/");
  EXPECT_EQ(tb_cookie_jar_count(nullptr), 0);
  tb_cookie_jar_free(nullptr);   /* 不得崩 */
}

/* http://example.com 不该收到 example.test 的 cookie(后缀边界)。 */
TEST(Cookie, NoSuffixBoundaryLeak) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "x=1; Domain=example.test", "http://example.test/");
  ExpectHeader(j, "http://notexample.test/", "");     /* 'otexample.test' ≠ 后缀 */
  ExpectHeader(j, "http://example.test.evil.com/", "");
  tb_cookie_jar_free(j);
}
/* 属性名按精确匹配,不按前缀 —— 否则凭空的属性会被采纳。 */
TEST(Cookie, AttributeNameIsExactNotPrefix) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  /* pathological 不是 path:若按前缀匹配,会被当成 Path=ological=x */
  tb_cookie_jar_set(j, "a=1; pathological=x", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "a=1");
  /* domainfoo 不是 domain:不该把 host 换成 domainfoo */
  tb_cookie_jar_set(j, "b=2; domainfoo=x", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "a=1; b=2");
  EXPECT_EQ(tb_cookie_jar_count(j), 2);
  tb_cookie_jar_free(j);
}

/* 重复属性按后者胜(与浏览器一致)。 */
TEST(Cookie, RepeatedAttributeLastWins) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=1; Path=/; Path=/deep", "http://example.test/deep/x");
  ExpectHeader(j, "http://example.test/deep/x", "a=1");
  ExpectHeader(j, "http://example.test/other", "");
  tb_cookie_jar_free(j);
}

/* ---- 端口:domain 匹配不看端口(RFC 6265) ----
 *
 * 这些用例盯的是一个具体的回归:url_split 曾把 "localhost:8080" 整个当成 host,
 * 于是
 *   1. 带 Domain= 的 cookie 只要 URL 带端口就整条被丢弃(开发服务器上
 *      Domain= 全部设不进去,且无任何错误提示);
 *   2. 同 host 不同端口之间不命中。
 * host-only 当时"能用"是巧合 —— 端口被嵌进 domain 字符串、两边恰好相等。 */

TEST(Cookie, DomainAttributeWorksWhenUrlHasPort) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  /* 设的时候就能读到,不能是空 */
  tb_cookie_jar_set(j, "s=1; Domain=localhost", "http://localhost:8080/");
  ExpectHeader(j, "http://localhost:8080/", "s=1");
  tb_cookie_jar_free(j);
}

TEST(Cookie, DomainMatchIgnoresPort) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "s=1; Domain=localhost", "http://localhost:8080/");
  ExpectHeader(j, "http://localhost:9090/", "s=1");   /* 跨端口应命中 */
  ExpectHeader(j, "https://localhost/", "s=1");      /* 跨协议仍命中 */
  ExpectHeader(j, "http://example.test/", "");        /* 别的 host 不命中 */
  tb_cookie_jar_free(j);
}

TEST(Cookie, HostOnlyCookieSpansPorts) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "s=1", "http://127.0.0.1:8080/");
  /* cookie 不按端口隔离,故 host-only 也应跨端口 */
  ExpectHeader(j, "http://127.0.0.1:9090/x", "s=1");
  ExpectHeader(j, "http://127.0.0.2:8080/", "");      /* 换 IP 就不命中 */
  tb_cookie_jar_free(j);
}

TEST(Cookie, IpLiteralHostWithPort) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "s=1; Domain=127.0.0.1", "http://127.0.0.1:8080/");
  ExpectHeader(j, "http://127.0.0.1:9090/", "s=1");
  tb_cookie_jar_free(j);
}

/* IPv6 字面量:方括号内的冒号不能当端口剥掉。 */
TEST(Cookie, IPv6LiteralHostKeepsBrackets) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "s=1; Domain=[::1]", "http://[::1]:8080/");
  ExpectHeader(j, "http://[::1]:8080/", "s=1");
  ExpectHeader(j, "http://[::1]:9090/", "s=1");       /* 跨端口 */
  ExpectHeader(j, "http://127.0.0.1:8080/", "");      /* 不同 host 不命中 */
  tb_cookie_jar_free(j);
}

/* 端口段非数字时**不得**剥,否则会把 host 截坏(把 example.test:abc 变成
 * example.test,等于让 cookie 发往一个不同的 host)。
 * 正确取向是 fail closed:host 保持 "example.test:abc",与 example.test 不匹配,
 * 结果是 cookie 不发送 —— 宁可不发,也不误发。 */
TEST(Cookie, NonNumericPortFailsClosed) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "s=1", "http://example.test/");
  ExpectHeader(j, "http://example.test/x", "s=1");
  /* 合法 URL 不会出现这种端口;真出现时按 fail closed 处理 */
  ExpectHeader(j, "http://example.test:abc/", "");
  tb_cookie_jar_free(j);
}

/* ---- CRLF / 控制字符注入(安全) ----
 *
 * cookie 值由**我们自己的代码**拼进 `Cookie:` 请求头。若 name/value 能带
 * CRLF,一个能设置 cookie 的源(被入侵的服务器把用户输入回显进 Set-Cookie
 * 即可)就能往该域的每个后续请求注入任意头行。是否真被拆成多行取决于
 * transport 实现 —— 不能把安全性外包给 transport,换实现就崩。
 * 浏览器也是直接拒收这类 cookie(RFC 6265 cookie-octet 排除 CTL)。 */

TEST(Cookie, CrLfInValueIsRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=x\r\nX-Evil: injected", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "");     /* 整条拒收,不部分保留 */
  tb_cookie_jar_free(j);
}

TEST(Cookie, BareCrOrLfIsRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=x\rEvil", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "");
  tb_cookie_jar_set(j, "b=x\nEvil", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "");
  tb_cookie_jar_free(j);
}

TEST(Cookie, CrLfInNameIsRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a\r\nX-Evil: 1=v", "http://example.test/");
  ExpectHeader(j, "http://example.test/", "");
  tb_cookie_jar_free(j);
}

/* Domain 属性值同样要校验:`Domain=a\r\nX: 1` 是完全合法的后缀,
 * 靠 domain 后缀检查兜不住。 */
TEST(Cookie, CrLfInDomainAttributeIsRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=1; Domain=example.test\r\nX-Evil: 1",
                    "http://example.test/");
  ExpectHeader(j, "http://example.test/", "");
  tb_cookie_jar_free(j);
}

/* DEL(0x7F)按 cookie-octet 也排除。
 *
 * 值里的**嵌入 NUL 测不了**:API 收const char*,NUL 在到达 jar 之前就把字符串
 * 截断了,jar 看到的与 "a=x" 无异 —— 这是 C 字符串接口的属性,不是 jar 的缺陷。
 * 所以此处只断言 DEL;NUL 的等价风险(「校验通过但实际更短」)在 strnlen 风格的
 * 接口下才会出现,当前接口下不可达。 */
TEST(Cookie, DelIsRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "b=x\x7f", "http://example.test/");
  EXPECT_EQ(0, tb_cookie_jar_count(j));
  tb_cookie_jar_free(j);
}

/* 分隔符与空白按 cookie-octet 也排除(否则拼出的头会有歧义)。 */
TEST(Cookie, SeparatorsAndSpaceInValueAreRejected) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "a=x y", "http://example.test/");
  EXPECT_EQ(0, tb_cookie_jar_count(j));
  tb_cookie_jar_set(j, "b=x,y", "http://example.test/");
  EXPECT_EQ(0, tb_cookie_jar_count(j));
  tb_cookie_jar_free(j);
}

/* 正常 cookie 不受影响 —— 防止上面的校验误伤。 */
TEST(Cookie, OrdinaryCookieStillWorks) {
  tb_cookie_jar *j = tb_cookie_jar_new();
  tb_cookie_jar_set(j, "sid=YWJjPT0=; Path=/; Domain=example.test",
                    "https://example.test/");
  ExpectHeader(j, "https://example.test/x", "sid=YWJjPT0=");
  tb_cookie_jar_free(j);
}
