#ifndef TB_VIEW_H
#define TB_VIEW_H
#include "tb.h"

#ifdef __cplusplus
extern "C" {
#endif

/* struct tb_elem comes from tb.h (public API). */
struct tb_view {
  char *url;
  char *title;
  int status;
  char *text;
  struct tb_elem *elems;
  int nelems;
  int is_not_renderable;
  char *not_renderable_type;
};

tb_view *tb_view_new(void);
void tb_view_free(tb_view *v);
tb_view *tb_view_clone(const tb_view *v);
char *tb_view_dump(const tb_view *v);   /* golden/TUI 用,malloc */

#ifdef __cplusplus
}
#endif
#endif
