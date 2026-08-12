#include "dom.h"
#include <lexbor/html/interfaces/document.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interface.h>
#include <stdlib.h>
#include <string.h>

struct tb_dom {
    lxb_html_document_t *doc;
    struct idmap {
        const lxb_dom_node_t *node;
        int id;
    } *ids;
    size_t nids, cap_ids;
    int next_id;
};

tb_dom *tb_dom_parse(const char *html, size_t len) {
    lxb_html_document_t *doc = lxb_html_document_create();
    if (!doc) return NULL;
    if (lxb_html_document_parse(doc, (const lxb_char_t *)html, len) != LXB_STATUS_OK) {
        lxb_html_document_destroy(doc);
        return NULL;
    }
    tb_dom *d = calloc(1, sizeof *d);
    if (!d) {
        lxb_html_document_destroy(doc);
        return NULL;
    }
    d->doc = doc;
    d->next_id = 1;
    return d;
}

void tb_dom_free(tb_dom *d) {
    if (!d) return;
    lxb_html_document_destroy(d->doc);
    free(d->ids);
    free(d);
}

tb_node *tb_dom_root(const tb_dom *d) {
    lxb_html_body_element_t *body = lxb_html_document_body_element(d->doc);
    if (body) return (tb_node *)body;
    return (tb_node *)lxb_dom_document_root(lxb_dom_interface_document(d->doc));
}

tb_node *tb_dom_first_child(tb_node *n) {
    if (!n) return NULL;
    return (tb_node *)lxb_dom_node_first_child((lxb_dom_node_t *)n);
}

tb_node *tb_dom_next_sibling(tb_node *n) {
    if (!n) return NULL;
    return (tb_node *)lxb_dom_node_next((lxb_dom_node_t *)n);
}

int tb_dom_is_element(tb_node *n) {
    if (!n) return 0;
    return ((lxb_dom_node_t *)n)->type == LXB_DOM_NODE_TYPE_ELEMENT;
}

const char *tb_dom_tag(tb_node *n) {
    if (!tb_dom_is_element(n)) return NULL;
    lxb_dom_element_t *e = (lxb_dom_element_t *)n;
    return (const char *)lxb_dom_element_local_name(e, NULL);
}

const char *tb_dom_attr(tb_node *n, const char *name) {
    if (!tb_dom_is_element(n)) return NULL;
    lxb_dom_element_t *e = (lxb_dom_element_t *)n;
    lxb_dom_attr_t *a = lxb_dom_element_attr_by_name(
        e, (const lxb_char_t *)name, strlen(name));
    if (!a) return NULL;
    size_t len;
    const lxb_char_t *val = lxb_dom_attr_value(a, &len);
    if (!val || len == 0) return NULL;
    return (const char *)val;
}

const char *tb_dom_text(tb_node *n) {
    if (!n) return NULL;
    lxb_dom_node_t *node = (lxb_dom_node_t *)n;
    if (node->type != LXB_DOM_NODE_TYPE_TEXT) return NULL;
    return (const char *)lxb_dom_node_text_content(node, NULL);
}

int tb_dom_id(tb_dom *d, tb_node *n) {
    if (!d || !n) return 0;
    const lxb_dom_node_t *node = (const lxb_dom_node_t *)n;
    for (size_t i = 0; i < d->nids; i++)
        if (d->ids[i].node == node) return d->ids[i].id;
    if (d->nids == d->cap_ids) {
        size_t nc = d->cap_ids ? d->cap_ids * 2 : 64;
        struct idmap *ni = realloc(d->ids, nc * sizeof *ni);
        if (!ni) return 0;
        d->ids = ni;
        d->cap_ids = nc;
    }
    d->ids[d->nids].node = node;
    d->ids[d->nids].id = d->next_id++;
    return d->ids[d->nids++].id;
}
