#include "tb.h"
#include <stdlib.h>
#include <string.h>

struct tb_browser {
  tb_config cfg;
  int initialized;
};

tb_browser *tb_create(const tb_config *cfg) {
  tb_browser *b = calloc(1, sizeof *b);
  if (cfg) b->cfg = *cfg;
  b->initialized = 1;
  return b;
}
void tb_destroy(tb_browser *b) { if (b) free(b); }
void tb_free(void *p) { free(p); }

/* 其余函数本任务只给桩,返回 TB_ERR_ARG;Task 9-11 逐个替换为真实现。 */
#define STUB2(f) tb_err f(tb_browser *b, int id) { (void)b; (void)id; tb_err e = { TB_ERR_ARG, "" }; return e; }
tb_err tb_navigate(tb_browser *b, const char *url) { (void)b; (void)url; tb_err e = { TB_ERR_ARG, "" }; return e; }
tb_err tb_back(tb_browser *b) { (void)b; tb_err e = { TB_ERR_ARG, "" }; return e; }
tb_err tb_forward(tb_browser *b) { (void)b; tb_err e = { TB_ERR_ARG, "" }; return e; }
tb_err tb_reload(tb_browser *b) { (void)b; tb_err e = { TB_ERR_ARG, "" }; return e; }
tb_err tb_observe(tb_browser *b, tb_view **out) { (void)b; (void)out; tb_err e = { TB_ERR_ARG, "" }; return e; }
void tb_view_free(tb_view *v) { (void)v; }
STUB2(tb_click)
STUB2(tb_submit)
tb_err tb_fill(tb_browser *b, int id, const char *value) { (void)b; (void)id; (void)value; tb_err e = { TB_ERR_ARG, "" }; return e; }
tb_err tb_select(tb_browser *b, int id, const char *option) { (void)b; (void)id; (void)option; tb_err e = { TB_ERR_ARG, "" }; return e; }
int tb_wait_idle(tb_browser *b, uint32_t t) { (void)b; (void)t; return 0; }
int tb_pump(tb_browser *b, uint32_t t) { (void)b; (void)t; return 0; }
