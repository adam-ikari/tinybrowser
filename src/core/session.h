#ifndef TB_SESSION_H
#define TB_SESSION_H
#include "tb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tb_session {
  char **back_url, **back_title; int nback, cap_back;
  char **fwd_url, **fwd_title; int nfwd, cap_fwd;
  char *cur_url, *cur_title;
  int cur_status;
  int pending;
  uint64_t last_net_ms;
  uint32_t idle_grace_ms;
  const tb_clock *clock;
} tb_session;

void tb_session_init(tb_session *s, const tb_clock *clock, uint32_t grace_ms);
void tb_session_free(tb_session *s);

/* 开始一次导航:当前页压入后退栈、清空前进栈、记新 URL、pending++ */
void tb_session_nav_start(tb_session *s, const char *url);
/* 导航完成:更新标题/状态、pending-- */
void tb_session_nav_done(tb_session *s, const char *url, const char *title, int status);
void tb_session_nav_fail(tb_session *s);          /* pending-- */
void tb_session_touch_net(tb_session *s);         /* 记录网络活动时刻 */
int  tb_session_idle(const tb_session *s);        /* pending==0 且 距上次网络 ≥ grace */

void tb_session_back(tb_session *s);
void tb_session_forward(tb_session *s);
const char *tb_session_url(const tb_session *s);
const char *tb_session_title(const tb_session *s);
int tb_session_status(const tb_session *s);
int tb_session_can_back(const tb_session *s);
int tb_session_can_fwd(const tb_session *s);

#ifdef __cplusplus
}
#endif
#endif
