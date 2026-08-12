#ifndef TB_FAKES_H
#define TB_FAKES_H
#include "tb.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- fake clock ---- */
typedef struct {
  tb_clock base;
  uint64_t now;
} fake_clock;
uint64_t fake_now_ms(const tb_clock *self);
void fake_clock_init(fake_clock *fc);

/* ---- fake transport ---- */
typedef struct {
  char *url;          /* 匹配前缀 */
  int status;
  const char *content_type;
  int attachment;
  const char *body;
  int err_code;       /* 非 0 -> on_done 报错 */
  const char *err_msg;
  uint64_t delay_ms;  /* 从 open 起延时多少毫秒后才派发 */
  int fires;
} fake_resp;

typedef struct fake_op {
  tb_transport_req req;
  fake_resp *resp;
  uint64_t opened_at;
  struct fake_op *next;
} fake_op;

typedef struct {
  tb_transport base;
  fake_clock *clock;        /* 共享同一个 fake clock,保证时间一致 */
  fake_resp *responses; int nresponses;   /* 按顺序匹配第一条前缀命中 */
  fake_op *ops;
  int poll_calls;
} fake_transport;

void *fake_open(const tb_transport *self, const tb_transport_req *req);
void fake_cancel(const tb_transport *self, void *op);
void fake_poll(const tb_transport *self);
void fake_transport_init(fake_transport *ft, fake_clock *fc);

#ifdef __cplusplus
}
#endif
#endif /* TB_FAKES_H */
