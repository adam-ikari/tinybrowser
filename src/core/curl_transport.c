#include "curl_transport.h"
#include <curl/curl.h>
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tb_curl_op {
  struct tb_curl_transport *t;
  CURL *easy;
  tb_transport_req req;
  char *body; size_t blen, bcap;
  int status;
  char *ct;
  int attachment;
  char *final_url;
  char errbuf[CURL_ERROR_SIZE];
} tb_curl_op;

typedef struct tb_curl_transport {
  tb_transport iface;
  uv_loop_t *loop;
  CURLM *multi;
  uv_timer_t timer;
  const char *ua;
  const char *ca_bundle;
  int running;   /* curl_multi_socket_action 的运行句柄计数 */
} tb_curl_transport;

typedef struct { tb_curl_transport *t; curl_socket_t fd; uv_poll_t p; } tb_curl_sock;

static void check_multi_info(tb_curl_transport *t);

static void uv_sock_cb(uv_poll_t *p, int status, int events) {
  (void)status;
  tb_curl_sock *cs = (tb_curl_sock *)p->data;
  tb_curl_transport *t = cs->t;   /* 先取:action 可能完成传输并 REMOVE 掉 cs */
  int ev = 0;
  if (events & UV_READABLE) ev |= CURL_CSELECT_IN;
  if (events & UV_WRITABLE) ev |= CURL_CSELECT_OUT;
  curl_multi_socket_action(t->multi, cs->fd, ev, &t->running);
  check_multi_info(t);
}

/* uv_poll_stop 不会把句柄移出 loop 的 poll_handles 队列,直接 free 会留下悬垂节点;
   必须 uv_close(内部摘链)+ 在 close 回调里释放。 */
static void on_sock_close(uv_handle_t *h) {
  tb_curl_sock *cs = (tb_curl_sock *)h->data;
  free(cs);
}

static int curl_socket_cb(CURL *easy, curl_socket_t s, int what, void *userp, void *socketp) {
  (void)easy;
  tb_curl_transport *t = (tb_curl_transport *)userp;
  if (what == CURL_POLL_REMOVE) {
    if (socketp) {
      tb_curl_sock *cs = (tb_curl_sock *)socketp;
      uv_poll_stop(&cs->p);
      uv_close((uv_handle_t *)&cs->p, on_sock_close);
    }
    curl_multi_assign(t->multi, s, NULL);
    return 0;
  }
  tb_curl_sock *cs = socketp ? (tb_curl_sock *)socketp : calloc(1, sizeof *cs);
  if (!socketp) {
    cs->t = t; cs->fd = s;
    cs->p.data = cs;
    uv_poll_init_socket(t->loop, &cs->p, (uv_os_sock_t)s);
    curl_multi_assign(t->multi, s, cs);
  }
  int ev = 0;
  if (what & CURL_POLL_IN) ev |= UV_READABLE;
  if (what & CURL_POLL_OUT) ev |= UV_WRITABLE;
  uv_poll_start(&cs->p, ev, uv_sock_cb);
  return 0;
}

static void timer_uv_cb(uv_timer_t *h) {
  tb_curl_transport *t = (tb_curl_transport *)h->data;
  int running = 0;
  curl_multi_socket_action(t->multi, CURL_SOCKET_TIMEOUT, 0, &running);
  check_multi_info(t);
}

static int curl_timer_cb(CURLM *m, long timeout_ms, void *userp) {
  (void)m;
  tb_curl_transport *t = (tb_curl_transport *)userp;
  if (timeout_ms < 0) { uv_timer_stop(&t->timer); return 0; }
  if (timeout_ms == 0) timeout_ms = 1;
  uv_timer_start(&t->timer, timer_uv_cb, (uint64_t)timeout_ms, 0);
  return 0;
}

static int header_has_token(const char *v, size_t vlen, const char *tok) {
  size_t tl = strlen(tok);
  for (size_t i = 0; i + tl <= vlen; i++)
    if (strncasecmp(v + i, tok, tl) == 0) return 1;
  return 0;
}

static size_t header_cb(char *buf, size_t sz, size_t n, void *ud) {
  tb_curl_op *op = (tb_curl_op *)ud;
  size_t len = sz * n;
  char *line = buf;
  if (len >= 5 && strncmp(line, "HTTP/", 5) == 0) {
    /* HTTP/1.1 200 OK */
    char *sp = strchr(line + 8, ' ');
    op->status = sp ? atoi(sp + 1) : 0;
    return len;
  }
  char *colon = (char *)memchr(line, ':', len);
  if (!colon) return len;
  size_t nl = (size_t)(colon - line);
  /* 值取冒号后跳过空白到行尾 */
  char *v = colon + 1;
  while ((size_t)(v - line) < len && (*v == ' ' || *v == '\t')) v++;
  size_t vlen = len - (size_t)(v - line);
  while (vlen && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n')) vlen--;
  if (nl == 12 && strncasecmp(line, "content-type", 12) == 0) {
    free(op->ct);
    op->ct = (char *)malloc(vlen + 1);
    memcpy(op->ct, v, vlen); op->ct[vlen] = '\0';
  } else if (nl == 20 && strncasecmp(line, "content-disposition", 20) == 0) {
    if (header_has_token(v, vlen, "attachment")) op->attachment = 1;
  }
  return len;
}

static size_t write_cb(char *ptr, size_t sz, size_t n, void *ud) {
  tb_curl_op *op = (tb_curl_op *)ud;
  size_t len = sz * n;
  if (op->bcap < op->blen + len + 1) {
    op->bcap = (op->blen + len + 1) * 2;
    op->body = (char *)realloc(op->body, op->bcap);
  }
  memcpy(op->body + op->blen, ptr, len);
  op->blen += len;
  op->body[op->blen] = '\0';
  return len;
}

static void check_multi_info(tb_curl_transport *t) {
  CURLMsg *msg;
  int left;
  while ((msg = curl_multi_info_read(t->multi, &left))) {
    if (msg->msg != CURLMSG_DONE) continue;
    CURL *easy = msg->easy_handle;
    tb_curl_op *op = NULL;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &op);
    if (!op) { curl_easy_cleanup(easy); continue; }
    CURLcode rc = msg->data.result;
    long st = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &st);
    char *eff = NULL;
    curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL, &eff);
    op->status = op->status ? op->status : (int)st;
    free(op->final_url);
    op->final_url = eff ? strdup(eff) : NULL;
    curl_multi_remove_handle(t->multi, easy);
    curl_easy_cleanup(easy);
    /* 派发(完成点统一触发,见 tb.h 约定) */
    op->req.on_headers(op->req.ud, op->status, op->ct, op->attachment, op->final_url);
    if (op->blen) op->req.on_body(op->req.ud, op->body, op->blen);
    tb_err e = { 0, "" };
    if (rc != CURLE_OK) {
      e.code = TB_ERR_NET;
      snprintf(e.msg, sizeof e.msg, "%s", op->errbuf[0] ? op->errbuf : curl_easy_strerror(rc));
    }
    op->req.on_done(op->req.ud, e);
    free(op->body); free(op->ct); free(op->final_url); free(op);
  }
}

static void *curl_open(const tb_transport *self, const tb_transport_req *req) {
  tb_curl_transport *t = (tb_curl_transport *)self;
  tb_curl_op *op = calloc(1, sizeof *op);
  op->t = t;
  op->req = *req;
  op->easy = curl_easy_init();
  curl_easy_setopt(op->easy, CURLOPT_PRIVATE, op);
  curl_easy_setopt(op->easy, CURLOPT_URL, req->url);
  curl_easy_setopt(op->easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(op->easy, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(op->easy, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(op->easy, CURLOPT_USERAGENT, t->ua);
  curl_easy_setopt(op->easy, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(op->easy, CURLOPT_HEADERDATA, op);
  curl_easy_setopt(op->easy, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(op->easy, CURLOPT_WRITEDATA, op);
  curl_easy_setopt(op->easy, CURLOPT_ERRORBUFFER, op->errbuf);
  curl_easy_setopt(op->easy, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  if (t->ca_bundle) curl_easy_setopt(op->easy, CURLOPT_CAINFO, t->ca_bundle);
  if (strcmp(req->method, "POST") == 0) {
    /* COPYPOSTFIELDS 自行拷贝,避免依赖 req->body 生命周期 */
    curl_easy_setopt(op->easy, CURLOPT_POST, 1L);
    curl_easy_setopt(op->easy, CURLOPT_COPYPOSTFIELDS, req->body ? req->body : "");
  }
  curl_multi_add_handle(t->multi, op->easy);
  return op;
}

static void curl_cancel(const tb_transport *self, void *opv) {
  tb_curl_transport *t = (tb_curl_transport *)self;
  tb_curl_op *op = (tb_curl_op *)opv;
  if (op) {
    curl_multi_remove_handle(t->multi, op->easy);
    curl_easy_cleanup(op->easy);
    free(op->body); free(op->ct); free(op->final_url); free(op);
  }
}

static void curl_poll(const tb_transport *self) { (void)self; /* uv 驱动 */ }

tb_transport *tb_curl_transport_create(uv_loop_t *loop, const tb_config *cfg) {
  tb_curl_transport *t = calloc(1, sizeof *t);
  t->iface.open = curl_open;
  t->iface.cancel = curl_cancel;
  t->iface.poll = curl_poll;
  t->loop = loop;
  t->ua = cfg->user_agent;
  t->ca_bundle = cfg->ca_bundle_path;
  t->multi = curl_multi_init();
  curl_multi_setopt(t->multi, CURLMOPT_SOCKETFUNCTION, curl_socket_cb);
  curl_multi_setopt(t->multi, CURLMOPT_SOCKETDATA, t);
  curl_multi_setopt(t->multi, CURLMOPT_TIMERFUNCTION, curl_timer_cb);
  curl_multi_setopt(t->multi, CURLMOPT_TIMERDATA, t);
  uv_timer_init(loop, &t->timer);
  t->timer.data = t;
  return &t->iface;
}
