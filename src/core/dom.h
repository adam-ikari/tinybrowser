#ifndef TB_DOM_H
#define TB_DOM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_dom tb_dom;
typedef struct tb_node tb_node;  /* opaque handle = lxb_dom_node_t* */

tb_dom *tb_dom_parse(const char *html, size_t len);
void tb_dom_free(tb_dom *d);

tb_node *tb_dom_root(const tb_dom *d);
tb_node *tb_dom_head(const tb_dom *d);
tb_node *tb_dom_first_child(tb_node *n);
tb_node *tb_dom_next_sibling(tb_node *n);

int tb_dom_is_element(tb_node *n);
const char *tb_dom_tag(tb_node *n);
const char *tb_dom_attr(tb_node *n, const char *name);
const char *tb_dom_text(tb_node *n);

int tb_dom_id(tb_dom *d, tb_node *n);

#ifdef __cplusplus
}
#endif

#endif
