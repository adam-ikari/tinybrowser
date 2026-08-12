#ifndef TB_RENDER_H
#define TB_RENDER_H
#include "dom.h"
#include "view.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 纯函数:DOM → 文本视图。无 I/O、无时钟。 */
tb_view *tb_render(tb_dom *dom, const char *url, int status);

#ifdef __cplusplus
}
#endif
#endif
