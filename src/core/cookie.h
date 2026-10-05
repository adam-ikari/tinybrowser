/* Cookie jar:内存实现,够浏览器用。
 *
 * 覆盖 RFC 6265 里真正影响行为的部分,刻意不做的也列在下面 —— 宁可少做,
 * 也不要做一个「看起来对但语义错」的实现。
 *
 * 做的:
 *   - name=value 解析(首个 '=' 分隔,值内 '=' 保留)
 *   - Domain 属性:缺省 = 请求 host;带前导 '.' 按后缀匹配;否则精确匹配
 *   - Path 属性:缺省 = 请求 path 的目录部分;按路径前缀匹配(边界正确)
 *   - Expires(Max-Age 优先):过期即删。**不持久化** —— 进程内有效
 *   - Secure:只在 https 请求上发送
 *   - HttpOnly:照常发给 HTTP,但 document.cookie **读不到**(JS 不可见)
 *   - 同名 cookie 的替换:按 (name, domain, path) 三元组,新值覆盖旧值
 *
 * 不做的(以及为什么):
 *   - **不持久化**:不落盘。重启浏览器丢 cookie。tinybrowser 无 profile 目录概念。
 *   - **不做 public suffix 列表**:eTLD+1 判断缺席,故 `Domain=com` 这类
 *     跨站泄漏在实现上防不住。真实的浏览器必须防(这是 cookie 安全的核心),
 *     本实现在注释里标为已知缺口,不做半套的伪装。
 *   - **不处理 SameSite**:无跨站上下文概念(我们是单进程本地浏览器)。
 *   - **不做 Max-Age 的负数以外的复杂优先级**:Max-Age > Expires,已覆盖。
 *   - **不 URL-decode cookie 值**:多数实现原样透传。
 */
#ifndef TB_COOKIE_H
#define TB_COOKIE_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_cookie_jar tb_cookie_jar;

/* 空 jar(NULL)与空 jar 等价:所有 API 都容忍 NULL,便于 tb_browser 里
   cookie_jar 尚未分配时直接调用。 */
tb_cookie_jar *tb_cookie_jar_new(void);
void tb_cookie_jar_free(tb_cookie_jar *j);

/* 存一条 Set-Cookie 原值。req_url 用于缺省 domain/path 的推导。
   解析失败(没有 '=' 或为空)静默忽略 —— 与浏览器一致(坏 cookie 被丢弃)。 */
void tb_cookie_jar_set(tb_cookie_jar *j, const char *set_cookie_value,
                       const char *req_url);

/* 取出适用于 req_url 的 Cookie 头值("a=1; b=2")。
   *out = malloc'd 串,调用方 free;无匹配时返回 strdup("")。 */
char *tb_cookie_jar_header(tb_cookie_jar *j, const char *req_url);

/* 取出 req_url 上 JS 可见(即非 HttpOnly)的 cookie,同样格式。
   document.cookie 读的就是它。 */
char *tb_cookie_jar_visible(tb_cookie_jar *j, const char *req_url);

/* 条数(测试/诊断用)。 */
int tb_cookie_jar_count(tb_cookie_jar *j);

#ifdef __cplusplus
}
#endif

#endif /* TB_COOKIE_H */