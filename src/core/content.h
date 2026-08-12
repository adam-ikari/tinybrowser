#ifndef TB_CONTENT_H
#define TB_CONTENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TB_CONTENT_RENDER,
  TB_CONTENT_NOT_RENDERABLE
} tb_content_kind;

/* 规范化:去参数、去空白、转小写。返回指向内部静态缓冲的指针(每次调用覆盖)。 */
const char *tb_content_normalize(const char *content_type);
tb_content_kind tb_content_classify(const char *content_type, int attachment);

#ifdef __cplusplus
}
#endif

#endif
