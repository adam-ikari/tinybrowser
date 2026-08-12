#ifndef TB_H
#define TB_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_browser tb_browser;

typedef struct tb_err {
  int code;              /* 0 = OK */
  char msg[128];
} tb_err;

/* 错误码 */
enum {
  TB_OK = 0,
  TB_ERR_ARG,        /* 非法参数 */
  TB_ERR_NET,        /* 网络/传输失败 */
  TB_ERR_PARSE,      /* HTML 解析失败 */
  TB_ERR_TIMEOUT,    /* 导航/等待超时 */
  TB_ERR_NO_ELEM,    /* 元素 ID 不存在 */
  TB_ERR_NO_VIEW     /* 尚无视图 */
};

/* ---- seam 接口 ---- */

typedef struct tb_transport tb_transport;

typedef struct tb_transport_req {
  const char *method;       /* "GET" | "POST" */
  const char *url;
  const char *body;         /* POST body,NULL 表示空 */
  const char *content_type; /* 请求 Content-Type(表单 urlencoded 等),可 NULL */
  /* 响应回调:全部在宿主线程、poll 期间同步触发。
     本设计约定:on_headers/on_body/on_done 在同一瞬间(传输完成时)依次触发,
     M1 实现统一在完成点派发。 */
  void (*on_headers)(void *ud, int status, const char *content_type,
                     int attachment, const char *final_url);
  void (*on_body)(void *ud, const char *data, size_t len);
  void (*on_done)(void *ud, tb_err err);
  void *ud;
} tb_transport_req;

struct tb_transport {
  void *(*open)(const tb_transport *self, const tb_transport_req *req); /* 返回 op 句柄 */
  void (*cancel)(const tb_transport *self, void *op);
  void (*poll)(const tb_transport *self);   /* 推进传输;回调在内部触发 */
};

typedef struct tb_clock {
  uint64_t (*now_ms)(const struct tb_clock *self);
} tb_clock;

extern const tb_clock tb_clock_real;

/* ---- 公共 API ---- */

typedef void (*tb_on_idle)(tb_browser *, void *ud);
typedef void (*tb_on_view_changed)(tb_browser *, void *ud);
typedef void (*tb_on_error)(tb_browser *, tb_err, const char *detail, void *ud);
typedef void (*tb_on_title)(tb_browser *, const char *title, void *ud);

typedef struct tb_config {
  const char *user_agent;      /* NULL → 内置默认 */
  const char *ca_bundle_path;  /* NULL → 不指定 CA(curl 默认) */
  uint32_t idle_grace_ms;      /* 默认 300 */
  uint32_t nav_timeout_ms;     /* 默认 30000 */
  tb_on_idle on_idle;
  tb_on_view_changed on_view_changed;
  tb_on_error on_error;
  tb_on_title on_title;
  const tb_transport *transport; /* NULL → 真实 curl 传输 */
  const tb_clock *clock;         /* NULL → tb_clock_real */
  void *ud;
} tb_config;

/* 视图(不透明;访问器见下) */
typedef struct tb_view tb_view;

tb_browser *tb_create(const tb_config *cfg);
void        tb_destroy(tb_browser *b);

tb_err tb_navigate(tb_browser *b, const char *url);
tb_err tb_back(tb_browser *b);
tb_err tb_forward(tb_browser *b);
tb_err tb_reload(tb_browser *b);

tb_err tb_observe(tb_browser *b, tb_view **out);   /* *out = 深拷贝,调用方 tb_view_free */
void   tb_view_free(tb_view *v);

tb_err tb_click(tb_browser *b, int id);
tb_err tb_fill(tb_browser *b, int id, const char *value);
tb_err tb_select(tb_browser *b, int id, const char *option);
tb_err tb_submit(tb_browser *b, int form_id);

int tb_wait_idle(tb_browser *b, uint32_t timeout_ms);  /* 0=空闲,1=超时未空闲 */
int tb_pump(tb_browser *b, uint32_t timeout_ms);       /* 0=空闲,1=仍忙 */
void tb_free(void *p);

struct tb_elem {
  int id;
  const char *type;   /* "link"|"button"|"input"|"select"|"form" */
  const char *text;
  size_t off;         /* 元素文本在 v->text 中的起始字节偏移(vimium hint/鼠标命中用) */
  const char *href;   /* link */
  const char *name;   /* input/select */
  const char *value;  /* input 当前值 / select 已选项 */
  const char **options; int noptions;  /* select */
};

/* 视图访问器 */
const char *tb_view_url(const tb_view *v);
const char *tb_view_title(const tb_view *v);
int         tb_view_status(const tb_view *v);
const char *tb_view_text(const tb_view *v);
int         tb_view_nelems(const tb_view *v);
int         tb_view_elem(const tb_view *v, int i, struct tb_elem *out); /* 0=成功 */
int         tb_view_not_renderable(const tb_view *v);  /* 1=不可渲染 */
const char *tb_view_not_renderable_type(const tb_view *v);

#ifdef __cplusplus
}
#endif
#endif /* TB_H */
