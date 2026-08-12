#include "view.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *p = malloc(n + 1);
  if (p) memcpy(p, s, n + 1);
  return p;
}

tb_view *tb_view_new(void) {
  return calloc(1, sizeof(tb_view));
}

void tb_view_free(tb_view *v) {
  if (!v) return;
  free(v->url);
  free(v->title);
  free(v->text);
  free(v->not_renderable_type);
  for (int i = 0; i < v->nelems; i++) {
    struct tb_elem *e = &v->elems[i];
    free((void *)e->text);
    free((void *)e->href);
    free((void *)e->name);
    free((void *)e->value);
    for (int j = 0; j < e->noptions; j++) free((void *)e->options[j]);
    free((void *)e->options);
  }
  free(v->elems);
  free(v);
}

tb_view *tb_view_clone(const tb_view *v) {
  if (!v) return NULL;
  tb_view *c = tb_view_new();
  c->url = dup(v->url);
  c->title = dup(v->title);
  c->status = v->status;
  c->text = dup(v->text);
  c->is_not_renderable = v->is_not_renderable;
  c->not_renderable_type = dup(v->not_renderable_type);
  c->nelems = v->nelems;
  if (v->nelems) {
    c->elems = calloc((size_t)v->nelems, sizeof(struct tb_elem));
    for (int i = 0; i < v->nelems; i++) {
      const struct tb_elem *s = &v->elems[i];
      struct tb_elem *t = &c->elems[i];
      t->id = s->id;
      t->type = s->type;  /* type 是字符串字面量,不拥有 */
      t->off = s->off;
      t->text = dup(s->text);
      t->href = dup(s->href);
      t->name = dup(s->name);
      t->value = dup(s->value);
      if (s->noptions) {
        t->options = calloc((size_t)s->noptions, sizeof(char *));
        for (int j = 0; j < s->noptions; j++) t->options[j] = dup(s->options[j]);
        t->noptions = s->noptions;
      }
    }
  }
  return c;
}

char *tb_view_dump(const tb_view *v) {
  size_t cap = 1024, len = 0;
  char *out = malloc(cap);
  if (!out) return NULL;
  out[0] = '\0';
#define APPEND(...) do { \
    int need = snprintf(NULL, 0, __VA_ARGS__); \
    if (need > 0 && len + (size_t)need + 1 > cap) { \
      cap = (len + (size_t)need + 1) * 2; \
      out = realloc(out, cap); \
      if (!out) return NULL; \
    } \
    if (need > 0) { snprintf(out + len, cap - len, __VA_ARGS__); len += (size_t)need; } \
  } while (0)
  APPEND("URL: %s\n", v ? (v->url ? v->url : "") : "");
  APPEND("TITLE: %s\n", v ? (v->title ? v->title : "") : "");
  APPEND("STATUS: %d\n", v ? v->status : 0);
  APPEND("TEXT:\n%s\n", v ? (v->text ? v->text : "") : "");
  APPEND("---\nELEMS:\n");
  if (v) {
    for (int i = 0; i < v->nelems; i++) {
      const struct tb_elem *e = &v->elems[i];
      APPEND("id=%d type=%s text=\"%s\" href=\"%s\" name=\"%s\" value=\"%s\"",
             e->id, e->type ? e->type : "", e->text ? e->text : "",
             e->href ? e->href : "", e->name ? e->name : "", e->value ? e->value : "");
      if (e->noptions) {
        APPEND(" options=\"");
        for (int j = 0; j < e->noptions; j++) APPEND("%s%s", j ? "|" : "", e->options[j] ? e->options[j] : "");
        APPEND("\"");
      }
      APPEND("\n");
    }
  }
#undef APPEND
  return out;
}

/* ---- 公共访问器(Task 1 tb.h 声明) ---- */

const char *tb_view_url(const tb_view *v) { return v ? v->url : NULL; }
const char *tb_view_title(const tb_view *v) { return v ? v->title : NULL; }
int tb_view_status(const tb_view *v) { return v ? v->status : 0; }
const char *tb_view_text(const tb_view *v) { return v ? v->text : NULL; }
int tb_view_nelems(const tb_view *v) { return v ? v->nelems : 0; }
int tb_view_not_renderable(const tb_view *v) { return v ? v->is_not_renderable : 0; }
const char *tb_view_not_renderable_type(const tb_view *v) { return v ? v->not_renderable_type : NULL; }

int tb_view_elem(const tb_view *v, int i, struct tb_elem *out) {
  if (!v || i < 0 || i >= v->nelems || !out) return -1;
  *out = v->elems[i];   /* 浅拷贝:指针指向 view 内部,view 存活期内有效 */
  return 0;
}
