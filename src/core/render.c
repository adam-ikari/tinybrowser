#include "render.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *buf; size_t len, cap;
  struct tb_elem *elems; int nelems, elems_cap;
  tb_dom *dom;
  int line_start;         /* 当前行是否可再追加文本(用于空白折叠/换行) */
  int at_line_start;
  int cur_sel;            /* 当前 select 元素下标;-1 = 不在 select 内 */
} ctx_t;

static void push(ctx_t *c, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (c->at_line_start && (s[i] == ' ' || s[i] == '\t')) continue; /* 行首空白 */
    if (c->len + 2 > c->cap) { c->cap = c->cap ? c->cap * 2 : 128; c->buf = realloc(c->buf, c->cap); }
    if (s[i] == '\n') {
      while (c->len > 0 && c->buf[c->len - 1] == ' ') c->len--;   /* 行尾空白 */
      c->buf[c->len++] = '\n';
      c->at_line_start = 1;
      c->line_start = 1;
    } else if (s[i] == ' ' || s[i] == '\t') {
      if (!c->at_line_start && c->len > 0 && c->buf[c->len - 1] != ' ' && c->buf[c->len - 1] != '\n')
        c->buf[c->len++] = ' ';                                    /* 折叠连续空白 */
    } else {
      c->buf[c->len++] = s[i];
      c->at_line_start = 0;
      c->line_start = 0;
    }
  }
}

static void newline(ctx_t *c) { push(c, "\n", 1); }

static void append_opt(ctx_t *c, struct tb_elem *e, const char *s) {
  e->options = realloc(e->options, (size_t)(e->noptions + 1) * sizeof(char *));
  e->options[e->noptions++] = strdup(s ? s : "");
}

/* 元素 textContent:收集全部后代文本节点(用于底栏交互标签) */
static size_t elem_text(tb_node *n, char *buf, size_t cap) {
  if (!n || cap < 2) return 0;
  if (!tb_dom_is_element(n)) {
    const char *t = tb_dom_text(n);
    size_t l = t ? strlen(t) : 0;
    size_t w = l < cap - 1 ? l : cap - 1;
    memcpy(buf, t, w);
    buf[w] = '\0';
    return w;
  }
  size_t off = 0;
  tb_node *ch = tb_dom_first_child(n);
  while (ch) {
    if (off < cap - 1) off += elem_text(ch, buf + off, cap - off);
    ch = tb_dom_next_sibling(ch);
  }
  buf[off] = '\0';
  return off;
}

static void add_elem(ctx_t *c, const char *type, tb_node *n) {
  c->elems = realloc(c->elems, (size_t)(c->nelems + 1) * sizeof(struct tb_elem));
  struct tb_elem *e = &c->elems[c->nelems++];
  memset(e, 0, sizeof *e);
  e->id = tb_dom_id(c->dom, n);
  e->type = type;
  /* 交互标签 = 后代文本;select/form 是容器,textContent 为选项/子元素拼接,无意义 */
  if (strcmp(type, "select") == 0 || strcmp(type, "form") == 0) {
    e->text = strdup("");
  } else {
    char tbuf[512];
    size_t nlen = elem_text(n, tbuf, sizeof tbuf);
    char *s = tbuf, *p = tbuf + nlen;
    while (s < p && isspace((unsigned char)*s)) s++;
    while (p > s && isspace((unsigned char)p[-1])) p--;
    *p = '\0';
    e->text = strdup(s);
  }
  e->href = strdup(tb_dom_attr(n, "href") ? tb_dom_attr(n, "href") : "");
  e->name = strdup(tb_dom_attr(n, "name") ? tb_dom_attr(n, "name") : "");
  e->value = strdup(tb_dom_attr(n, "value") ? tb_dom_attr(n, "value") : "");
}

static int is_block_tag(const char *t) {
  static const char *blocks[] = {
    "p","div","h1","h2","h3","h4","h5","h6","li","ul","ol","table","tr","section",
    "article","header","footer","nav","aside","br","hr","blockquote","pre",
    "form","select","fieldset" };
  if (!t) return 0;
  for (size_t i = 0; i < sizeof blocks / sizeof *blocks; i++)
    if (strcmp(t, blocks[i]) == 0) return 1;
  return 0;
}

static void walk(ctx_t *c, tb_node *n) {
  if (!n) return;
  if (tb_dom_is_element(n)) {
    const char *tag = tb_dom_tag(n);
    if (tag && (strcmp(tag, "script") == 0 || strcmp(tag, "style") == 0 ||
                strcmp(tag, "noscript") == 0 || strcmp(tag, "head") == 0)) return;
    if (tag && strcmp(tag, "title") == 0) return;  /* 文本由 extract_title 单独提取 */

    int saved_sel = c->cur_sel;

    if (tag && strcmp(tag, "a") == 0 && tb_dom_attr(n, "href")) add_elem(c, "link", n);
    else if (tag && strcmp(tag, "input") == 0) {
      const char *type = tb_dom_attr(n, "type");
      if (type && strcmp(type, "hidden") == 0) return;
      if (type && (strcmp(type, "submit") == 0 || strcmp(type, "button") == 0 ||
                   strcmp(type, "reset") == 0))
        add_elem(c, "button", n);
      else add_elem(c, "input", n);
    }
    else if (tag && strcmp(tag, "button") == 0) add_elem(c, "button", n);
    else if (tag && strcmp(tag, "select") == 0) {
      add_elem(c, "select", n);
      c->cur_sel = c->nelems - 1;
    }
    else if (tag && strcmp(tag, "form") == 0) add_elem(c, "form", n);
    else if (tag && strcmp(tag, "option") == 0) {
      if (c->cur_sel >= 0) {
        const char *txt = tb_dom_text(tb_dom_first_child(n));
        if (txt) append_opt(c, &c->elems[c->cur_sel], txt);
      }
      c->cur_sel = saved_sel;
      return;   /* option 文本不进入正文 */
    }

    if (is_block_tag(tag) && !c->at_line_start) newline(c);
    tb_node *ch = tb_dom_first_child(n);
    while (ch) { walk(c, ch); ch = tb_dom_next_sibling(ch); }
    if (is_block_tag(tag) && !c->at_line_start) newline(c);

    c->cur_sel = saved_sel;
  } else {
    const char *txt = tb_dom_text(n);
    if (txt) push(c, txt, strlen(txt));
  }
}

static char *extract_title(tb_dom *dom) {
  /* 找 <head> 下第一个 <title> 的文本 */
  tb_node *head = tb_dom_head(dom);
  if (!head) return NULL;
  tb_node *t = tb_dom_first_child(head);
  while (t) {
    if (tb_dom_is_element(t) && strcmp(tb_dom_tag(t), "title") == 0) {
      const char *txt = tb_dom_text(tb_dom_first_child(t));
      return txt ? strdup(txt) : NULL;
    }
    t = tb_dom_next_sibling(t);
  }
  return NULL;
}

tb_view *tb_render(tb_dom *dom, const char *url, int status) {
  tb_view *v = tb_view_new();
  v->url = strdup(url ? url : "");
  v->status = status;
  v->title = extract_title(dom);
  if (!v->title) v->title = strdup("");
  ctx_t c = {0};
  c.cur_sel = -1;
  c.dom = dom;
  walk(&c, tb_dom_root(dom));
  /* push() 从不写 NUL;不补终止符则下方 strlen 会越界读堆内存 */
  if (c.buf) c.buf[c.len] = '\0';
  char *text = c.buf ? c.buf : strdup("");
  /* 首尾块标签会各产生一个换行,裁掉首尾空行 */
  size_t start = 0;
  while (text[start] == '\n') start++;
  size_t end = strlen(text);
  while (end > start && text[end - 1] == '\n') end--;
  text[end] = '\0';
  if (start) memmove(text, text + start, end - start + 1);
  v->text = text;
  v->elems = c.elems;
  v->nelems = c.nelems;
  return v;
}
