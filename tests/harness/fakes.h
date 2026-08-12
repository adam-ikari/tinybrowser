#ifndef TB_FAKES_H
#define TB_FAKES_H
#include "tb.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ---- fake clock ---- */
typedef struct {
  tb_clock base;
  uint64_t now;
} fake_clock;
static uint64_t fake_now_ms(const tb_clock *self) {
  return ((fake_clock *)self)->now;
}
static void fake_clock_init(fake_clock *fc) {
  fc->base.now_ms = fake_now_ms;
  fc->now = 1000000u;  /* 远离 0,便于断言相对时间 */
}

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

static void fake_op_fire(fake_transport *ft, fake_op *op) {
  fake_resp *r = op->resp;
  r->fires++;
  if (r->err_code != 0) {
    tb_err e = { r->err_code, "" };
    strncpy(e.msg, r->err_msg ? r->err_msg : "fake error", sizeof e.msg - 1);
    if (op->req.on_headers) op->req.on_headers(op->req.ud, 0, NULL, 0, NULL);
    if (op->req.on_body) op->req.on_body(op->req.ud, "", 0);
    if (op->req.on_done) op->req.on_done(op->req.ud, e);
    return;
  }
  if (op->req.on_headers)
    op->req.on_headers(op->req.ud, r->status, r->content_type, r->attachment,
                       op->req.url);
  size_t len = r->body ? strlen(r->body) : 0;
  if (op->req.on_body) op->req.on_body(op->req.ud, r->body ? r->body : "", len);
  tb_err ok = { 0, "" };
  if (op->req.on_done) op->req.on_done(op->req.ud, ok);
}

static void *fake_open(const tb_transport *self, const tb_transport_req *req) {
  fake_transport *ft = (fake_transport *)self;
  fake_resp *match = NULL;
  for (int i = 0; i < ft->nresponses; i++) {
    if (ft->responses[i].url && req->url &&
        strncmp(ft->responses[i].url, req->url, strlen(ft->responses[i].url)) == 0) {
      match = &ft->responses[i];
      break;
    }
  }
  fake_op *op = (fake_op *)calloc(1, sizeof(fake_op));
  op->req = *req;
  op->resp = match;
  op->opened_at = ft->clock->now;
  op->next = ft->ops;
  ft->ops = op;
  return op;
}

static void fake_cancel(const tb_transport *self, void *op) {
  (void)self;
  if (!op) return;
  fake_op **pp = &((fake_transport *)self)->ops;
  while (*pp) {
    if (*pp == op) { *pp = (*pp)->next; free(op); return; }
    pp = &(*pp)->next;
  }
}

static void fake_poll(const tb_transport *self) {
  fake_transport *ft = (fake_transport *)self;
  ft->poll_calls++;
  ft->clock->now += 1;   /* 每次 poll 推进 1ms:让 grace/超时 逻辑在 pump 循环中收敛 */
  fake_op *op = ft->ops;
  while (op) {
    fake_op *next = op->next;
    if (op->resp && ft->clock->now >= op->opened_at + op->resp->delay_ms) {
      fake_op_fire(ft, op);
      /* 从链表移除并释放 */
      fake_op **pp = &ft->ops;
      while (*pp && *pp != op) pp = &(*pp)->next;
      if (*pp) { *pp = op->next; free(op); }
    }
    op = next;
  }
}

static void fake_transport_init(fake_transport *ft, fake_clock *fc) {
  ft->base.open = fake_open;
  ft->base.cancel = fake_cancel;
  ft->base.poll = fake_poll;
  ft->clock = fc;
  ft->responses = NULL; ft->nresponses = 0;
  ft->ops = NULL;
  ft->poll_calls = 0;
}

#endif /* TB_FAKES_H */
