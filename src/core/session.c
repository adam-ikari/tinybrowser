#include "session.h"
#include <stdlib.h>
#include <string.h>

void tb_session_init(tb_session *s, const tb_clock *clock, uint32_t grace_ms) {
  memset(s, 0, sizeof *s);
  s->clock = clock;
  s->idle_grace_ms = grace_ms ? grace_ms : 300;
}

void tb_session_free(tb_session *s) {
  for (int i = 0; i < s->nback; i++) { free(s->back_url[i]); free(s->back_title[i]); }
  for (int i = 0; i < s->nfwd; i++)  { free(s->fwd_url[i]);  free(s->fwd_title[i]); }
  free(s->back_url); free(s->back_title);
  free(s->fwd_url);  free(s->fwd_title);
  free(s->cur_url);  free(s->cur_title);
}

static void push(char ***arr, char ***titles, int *n, int *cap, const char *url, const char *title) {
  if (*n == *cap) { *cap = *cap ? *cap * 2 : 16;
    *arr = realloc(*arr, (size_t)*cap * sizeof(char *));
    *titles = realloc(*titles, (size_t)*cap * sizeof(char *)); }
  (*arr)[*n] = strdup(url ? url : "");
  (*titles)[*n] = strdup(title ? title : "");
  (*n)++;
}

void tb_session_nav_start(tb_session *s, const char *url) {
  if (s->cur_url) push(&s->back_url, &s->back_title, &s->nback, &s->cap_back, s->cur_url, s->cur_title);
  for (int i = 0; i < s->nfwd; i++) { free(s->fwd_url[i]); free(s->fwd_title[i]); }
  s->nfwd = 0;
  free(s->cur_url);
  s->cur_url = strdup(url ? url : "");
  s->pending++;
}

void tb_session_nav_done(tb_session *s, const char *url, const char *title, int status) {
  if (url) { free(s->cur_url); s->cur_url = strdup(url); }
  free(s->cur_title);
  s->cur_title = strdup(title ? title : "");
  s->cur_status = status;
  if (s->pending > 0) s->pending--;
}

void tb_session_nav_fail(tb_session *s) { if (s->pending > 0) s->pending--; }

void tb_session_touch_net(tb_session *s) {
  if (s->clock) s->last_net_ms = s->clock->now_ms(s->clock);
}

int tb_session_idle(const tb_session *s) {
  if (s->pending > 0) return 0;
  if (!s->clock) return 1;
  uint64_t now = s->clock->now_ms(s->clock);
  return now - s->last_net_ms >= s->idle_grace_ms;
}

void tb_session_back(tb_session *s) {
  if (!s->nback) return;
  char *u = s->back_url[s->nback - 1], *t = s->back_title[s->nback - 1];
  s->nback--;
  push(&s->fwd_url, &s->fwd_title, &s->nfwd, &s->cap_fwd, s->cur_url, s->cur_title);
  free(s->cur_url); s->cur_url = u;
  free(s->cur_title); s->cur_title = t;
  s->pending++;
}

void tb_session_forward(tb_session *s) {
  if (!s->nfwd) return;
  char *u = s->fwd_url[s->nfwd - 1], *t = s->fwd_title[s->nfwd - 1];
  s->nfwd--;
  push(&s->back_url, &s->back_title, &s->nback, &s->cap_back, s->cur_url, s->cur_title);
  free(s->cur_url); s->cur_url = u;
  free(s->cur_title); s->cur_title = t;
  s->pending++;
}

const char *tb_session_url(const tb_session *s) { return s->cur_url; }
const char *tb_session_title(const tb_session *s) { return s->cur_title; }
int tb_session_status(const tb_session *s) { return s->cur_status; }
int tb_session_can_back(const tb_session *s) { return s->nback > 0; }
int tb_session_can_fwd(const tb_session *s) { return s->nfwd > 0; }
