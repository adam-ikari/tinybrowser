#ifndef TB_URL_H
#define TB_URL_H
#ifdef __cplusplus
extern "C" {
#endif
/* 相对 URL 解析(base 必须绝对 URL;ref 可为相对路径/query/fragment/绝对)。
   返回 malloc 的新字符串,调用方 tb_free。失败(非法 base/ref)返回 NULL。 */
char *tb_url_resolve(const char *base, const char *ref);
/* 表单 urlencode:除 unreserved 外全部 %XX 编码。返回 malloc。 */
char *tb_url_encode(const char *s);
#ifdef __cplusplus
}
#endif
#endif
