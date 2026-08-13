#ifndef TB_BROWSER_INTERNAL_H
#define TB_BROWSER_INTERNAL_H
#include "tb.h"
#include "dom.h"
#include "session.h"
#include <uv.h>

typedef struct { int id; char *value; } fv_t;

struct tb_browser {
  tb_config cfg;
  tb_session session;
  tb_dom *dom;
  tb_view *view;
  const tb_transport *transport;
  fv_t fv[64];          /* 表单值(由 tb_fill/tb_select 写入),M1 固定槽 */
  int nfv;
  uv_loop_t loop;
  int loop_init;        /* 1 = 默认 curl transport + 自有 uv loop */
  const struct tb_js_engine *engine; /* 实际 engine:cfg.js_engine 或 tb_default_js_engine() */
  void *js_doc;                      /* 当前文档的 JS context 句柄(engine->open 返回值) */
  char *cookie_jar;                  /* document.cookie 往返(M2a 单串) */
};

/* 子资源同步加载:nested pump。*out = strdup 或空串,调用方 free。 */
tb_err tb_load_sync(tb_browser *b, const char *url, char **out);

#endif
