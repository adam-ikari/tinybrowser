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
  char *cookies;      /* 全部 Set-Cookie 原值,\n 分隔 */
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

/* 在 header 值里找**完整 token**(非子串)。
 *
 * 原实现是纯子串匹配,于是
 *   Content-Disposition: inline; filename="my-attachment-file.html"
 * 会因为 filename 里含 "attachment" 而被误判成附件 ——
 * 端到端测试 BrowserApi.AttachmentTokenIsNotMatchedAsSubstring 抓到。
 * 按 RFC 6266,disposition-type 是值里的第一个 token,必须整体匹配。
 * token 边界:串首/串尾,或前后是分隔符(空白与 , ; =)。 */
static int header_has_token(const char *v, size_t vlen, const char *tok) {
  size_t tl = strlen(tok);
  for (size_t i = 0; i + tl <= vlen; i++) {
    if (strncasecmp(v + i, tok, tl) != 0) continue;
    /* 左边界 */
    if (i > 0) {
      char c = v[i - 1];
      if (c != ' ' && c != '\t' && c != ',' && c != ';' && c != '=') continue;
    }
    /* 右边界 */
    if (i + tl < vlen) {
      char c = v[i + tl];
      if (c != ' ' && c != '\t' && c != ',' && c != ';' && c != '=') continue;
    }
    return 1;
  }
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
  } else if (nl == 19 && strncasecmp(line, "content-disposition", 19) == 0) {
    /* 19 不是 20:"content-disposition" 是 19 个字符。此前写成 20,于是这条
     * 分支**永远不成立** —— Content-Disposition 从未被检出过,带
     * attachment 的下载被当成普通页面渲染(端到端测试
     * BrowserApi.ContentDispositionAttachmentIsNotRendered 抓到)。
     * 同族的 "content-type"(12)与 "set-cookie"(10)长度是对的,只有这条错。 */
    if (header_has_token(v, vlen, "attachment")) op->attachment = 1;
  } else if (nl == 10 && strncasecmp(line, "set-cookie", 10) == 0 &&
             op->req.on_set_cookie) {
    /* Set-Cookie 逐条派发。不能在 header_cb 里直接调宿主回调 —— 那是 curl
       内部栈,且 host 名 (cookie 的 domain/path 归属)要等 final_url 才知道。
       故先攒着,到完成点、拿到 final_url 之后再派发(见 check_multi_info)。 */
    char *val = (char *)malloc(vlen + 1);
    if (val) {
      memcpy(val, v, vlen);
      val[vlen] = '\0';
      /* 追加:多条 Set-Cookie 用 \n 分隔。
       * val 在两个分支里都必须被消费或释放 —— 早先只在「首个 cookie」分支
       * 把它挂到 op->cookies,追加分支拷完就丢弃,于是第二条及以后的
       * Set-Cookie 每次漏一份(ASAN 实测 18 bytes,CookieRoundTrips 发两条
       * cookie 时抓到)。realloc 失败时同样要释放,否则静默漏。 */
      if (op->cookies) {
        size_t old = strlen(op->cookies);
        char *bigger = (char *)realloc(op->cookies, old + vlen + 2);
        if (bigger) {
          bigger[old] = '\n';
          memcpy(bigger + old + 1, val, vlen + 1);
          op->cookies = bigger;
          free(val);
        } else {
          free(val);   /* 保留已攒的 cookies,丢弃这一条 */
        }
      } else {
        op->cookies = val;
      }
    }
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
    /* Set-Cookie 在 on_headers 之后派发:cookie 的 domain/path 归属要靠
       final_url 判定,宿主必须先看到 final_url。 */
    if (op->cookies && op->req.on_set_cookie) {
      const char *p = op->cookies;
      while (*p) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n) {
          char *one = (char *)malloc(n + 1);
          if (one) {
            memcpy(one, p, n);
            one[n] = '\0';
            op->req.on_set_cookie(op->req.ud, one);
            free(one);
          }
        }
        if (!nl) break;
        p = nl + 1;
      }
    }
    if (op->blen) op->req.on_body(op->req.ud, op->body, op->blen);
    tb_err e = { 0, "" };
    if (rc != CURLE_OK) {
      e.code = TB_ERR_NET;
      snprintf(e.msg, sizeof e.msg, "%s", op->errbuf[0] ? op->errbuf : curl_easy_strerror(rc));
    }
    op->req.on_done(op->req.ud, e);
    free(op->body); free(op->ct); free(op->final_url); free(op->cookies); free(op);
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
  /* Cookie 头:browser 侧拼好传进来。CURLOPT_COOKIE(而非 COOKIEFILE)——
     我们自己的 jar 在内存里,不走 curl 的 cookie engine(那需要落盘/回调)。 */
  if (req->cookie && req->cookie[0])
    curl_easy_setopt(op->easy, CURLOPT_COOKIE, req->cookie);
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
    free(op->body); free(op->ct); free(op->final_url); free(op->cookies); free(op);
  }
}

static void curl_poll(const tb_transport *self) { (void)self; /* uv 驱动 */ }

/* timer 的 close 回调:uv_close 是异步的,而 timer 句柄内嵌在 t 里,
   所以 t 必须活到回调跑完。真正释放放在这里。 */
static void on_timer_closed(uv_handle_t *h) {
  tb_curl_transport *t = (tb_curl_transport *)h->data;
  curl_multi_cleanup(t->multi);
  free(t);
}

/* tb_destroy 调用(仅限 tb_create 自建的默认 transport)。
 *
 * curl_multi_init() 会建一个内部连接池 easy handle,此前没有任何地方调
 * curl_multi_cleanup() —— 长跑进程、或反复 create/destroy 的测试会持续
 * 累积(ASAN/Valgrind: 每次 tb_create 泄漏 multi + 连接池)。
 *
 * uv_timer_t 内嵌在 t 中,而 uv_close 异步,所以顺序是:
 *   uv_timer_stop → curl_multi_cleanup(同步释放 curl 侧) → uv_close(摘链)
 *   → uv_run(NOWAIT) 让 close 回调跑掉 → 回调里 free(t)。
 * 直接 free(t) 会把仍挂在 loop 上的 uv 句柄留成悬垂节点。
 *
 * 已知边界:此刻若还有 in-flight 请求,tb_curl_op 包装层会泄漏
 * (curl_multi_cleanup 会清掉 easy handle,但包装层是我们 malloc 的、
 * 传输层没有在飞 op 链表可枚举)。正常流程下 destroy 发生在导航完成之后,
 * 不存在在飞请求。 */
static void curl_destroy(const tb_transport *self) {
  tb_curl_transport *t = (tb_curl_transport *)self;
  if (!t) return;
  if (t->timer.data && !uv_is_closing((uv_handle_t *)&t->timer)) {
    uv_timer_stop(&t->timer);
    uv_close((uv_handle_t *)&t->timer, on_timer_closed);
    if (t->loop) uv_run(t->loop, UV_RUN_NOWAIT);   /* 让 close 回调跑完 */
    return;
  }
  /* 没有 loop 或句柄已关闭:直接收尾。 */
  if (t->multi) curl_multi_cleanup(t->multi);
  free(t);
}

tb_transport *tb_curl_transport_create(uv_loop_t *loop, const tb_config *cfg) {
  tb_curl_transport *t = calloc(1, sizeof *t);
  t->iface.open = curl_open;
  t->iface.cancel = curl_cancel;
  t->iface.poll = curl_poll;
  t->iface.destroy = curl_destroy;
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
