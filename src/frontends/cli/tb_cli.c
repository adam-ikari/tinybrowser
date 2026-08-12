#define TB_IMPL
#define _XOPEN_SOURCE 700  /* glibc 需要它才在 <wchar.h> 声明 wcwidth(termbox2 使用) */
#define _DEFAULT_SOURCE
#include "tb.h"          /* 先于 termbox2:避免其 #define TB_OK 污染 tb.h 同名枚举 */
#include "termbox2.h"
#include "hint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 布局:
   顶栏: URL  status  title
   中部: 可滚动文本视图(正文)
   底栏: 元素列表(0-based 索引  type text)  + 模式行
   模式: BROWSE(浏览) HINT(f 键 vimium 提示) CMD(: 命令/URL) SEARCH(/ 页内搜索) INPUT(填充输入框)
   按键(BROWSE): ↑↓/jk 滚动  g/G 首/尾  PgUp/PgDn 翻页  Home/End 首/尾
                 Space 翻页  Tab 下移焦点  0-9 数字直达  Enter 激活(link/button/input)
                 ←→ 切换 select 选项  i 填充 input  f 呼出 hint  : 命令  / 搜索
                 q/Ctrl+Q/Ctrl+C 退出
   HINT: a-z 激活元素(两字母分两步)  Backspace 回退  Esc 退出
   命令(:): q/quit 退出  b/back 后退  f/forward 前进  r/reload 重载,其余视为 URL */

enum { MODE_BROWSE, MODE_CMD, MODE_SEARCH, MODE_INPUT, MODE_HINT };

static int scroll_top = 0, focus = -1;   /* focus: -1=正文,>=0=元素索引 */
static tb_browser *g_b;
static tb_view *g_v;
static int mode = MODE_BROWSE;
static char prompt_buf[256];
static int prompt_len;
static char search_str[128];
static int search_str_len;
static int hint_n;            /* 进入 HINT 时的可点元素总数 */
static int hint_elems[676];   /* 可点元素在 g_v 中的索引(序号 k → hint = tb_hint_keys(hint_n,k)) */
static char hint_prefix[3] = {0};   /* 已输前缀(最长 2,显式零初始化) */
static int hint_prefix_len;

static void enter_mode(int m);
static void run_command(void);

static const char *mode_name(void) {
  switch (mode) {
    case MODE_CMD:    return "CMD";
    case MODE_SEARCH: return "SEARCH";
    case MODE_INPUT:  return "INPUT";
    case MODE_HINT:   return "HINT";
    default:          return "BROWSE";
  }
}

static void prompt_reset(void) {
  prompt_buf[0] = '\0';
  prompt_len = 0;
}

static void enter_mode(int m) {
  mode = m;
  prompt_reset();
}

/* 重新观察:等待传输空闲 → 释放旧视图 → 取新视图 */
static void refresh_view(void) {
  tb_wait_idle(g_b, 30000);
  tb_view_free(g_v);
  g_v = NULL;
  tb_observe(g_b, &g_v);
  scroll_top = 0;
  search_str_len = 0;
  search_str[0] = '\0';
  if (mode == MODE_HINT) enter_mode(MODE_BROWSE);
}

static void navigate(const char *url) {
  tb_navigate(g_b, url);
  refresh_view();
}

/* 将用户输入补全为 URL(无 scheme 时假定 http) */
static void navigate_input(const char *s) {
  char buf[512];
  if (strstr(s, "://")) snprintf(buf, sizeof buf, "%s", s);
  else snprintf(buf, sizeof buf, "http://%s", s);
  navigate(buf);
}

static int count_lines(const char *text) {
  if (!text || !*text) return 0;
  int n = 1;
  for (const char *p = text; *p; p++) if (*p == '\n') n++;
  return n;
}

static int body_line_count(void) {
  return count_lines(g_v ? tb_view_text(g_v) : "");
}

/* 从 start_line 起第一个包含 needle 的行;找不到返回 -1 */
static int find_line(int start_line, const char *needle) {
  const char *text = g_v ? tb_view_text(g_v) : "";
  int line = 0;
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (line >= start_line) {
      char buf[512];
      size_t cplen = len < sizeof buf - 1 ? len : sizeof buf - 1;
      memcpy(buf, p, cplen); buf[cplen] = '\0';
      if (strstr(buf, needle)) return line;
    }
    line++;
    if (!nl) break;
    p = nl + 1;
  }
  return -1;
}

static void redraw(void) {
  tb_clear();
  int w = tb_width(), h = tb_height();
  const char *url = g_v ? tb_view_url(g_v) : "";
  const char *title = g_v ? tb_view_title(g_v) : "";
  tb_printf(0, 0, 0, 0, "%-*s", w - 1, url);
  tb_printf(0, 1, 0, 0, "status=%d  title=%s  [%s]", g_v ? tb_view_status(g_v) : 0,
            title, mode_name());

  int nelems = g_v ? tb_view_nelems(g_v) : 0;
  int elem_rows = nelems < 5 ? nelems : 5;
  int body_bottom = h - 3 - elem_rows;
  if (body_bottom < 3) body_bottom = 3;

  /* 正文(可滚动) */
  const char *text = g_v ? tb_view_text(g_v) : "(no page)";
  int total = count_lines(text);
  int visible = body_bottom - 2 + 1;
  int max_top = total > visible ? total - visible : 0;
  if (scroll_top > max_top) scroll_top = max_top;
  if (scroll_top < 0) scroll_top = 0;

  int y = 2, line = 0;
  const char *p = text;
  while (*p && y <= body_bottom) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (line >= scroll_top) {
      char buf[512];
      size_t cplen = len < sizeof buf - 1 ? len : sizeof buf - 1;
      memcpy(buf, p, cplen); buf[cplen] = '\0';
      uint32_t fg = 0;
      if (search_str_len > 0 && strstr(buf, search_str)) fg = TB_CYAN;
      tb_printf(0, y, fg, 0, "%-*s", w - 1, buf);
      y++;
    }
    line++;
    if (!nl) break;
    p = nl + 1;
  }

  /* HINT 覆盖:在可点元素文本起点叠加提示键位 */
  if (mode == MODE_HINT && text != NULL) {
    for (int k = 0; k < hint_n; k++) {
      char h[4] = {0};
      size_t hn = tb_hint_keys((size_t)hint_n, (size_t)k, h, sizeof h);
      if ((int)hn <= hint_prefix_len) continue;              /* 前缀已覆盖全部(将激活) */
      if (strncmp(h, hint_prefix, (size_t)hint_prefix_len) != 0) continue;  /* 非当前候选 */
      struct tb_elem e;
      if (tb_view_elem(g_v, hint_elems[k], &e)) continue;
      int erow, ecol;
      tb_off_to_pos(text, e.off, &erow, &ecol);
      if (erow < scroll_top || erow > scroll_top + visible - 1) continue;   /* 可见区外 */
      int sy = 2 + (erow - scroll_top);
      int rem = (int)hn - hint_prefix_len;
      for (int j = 0; j < rem && ecol + hint_prefix_len + j < w; j++)
        tb_set_cell(ecol + hint_prefix_len + j, sy, h[hint_prefix_len + j], TB_YELLOW, 0);
    }
  }

  /* 底栏元素(0-based 索引) */
  if (focus >= nelems) focus = nelems ? nelems - 1 : -1;
  for (int i = 0; i < nelems && i < elem_rows; i++) {
    struct tb_elem e;
    if (tb_view_elem(g_v, i, &e)) break;
    int hl = (i == focus);
    if (hl) tb_set_cell(0, y + i, '>', TB_YELLOW, 0);
    tb_printf(hl ? 1 : 0, y + i, hl ? TB_YELLOW : 0, 0,
              "%d:%s %s", i, e.type, e.text ? e.text : "");
  }

  /* 模式行 */
  if (mode == MODE_BROWSE) {
    tb_printf(0, h - 1, TB_GREEN, 0,
              "q退出  :命令  /搜索  j/k滚动  Tab/数字选元素  Enter激活  f提示");
  } else if (mode == MODE_HINT) {
    tb_printf(0, h - 1, TB_GREEN, 0, "hint: %s_  (Esc退出  Backspace回退)", hint_prefix);
  } else {
    char mprefix = (mode == MODE_SEARCH) ? '/' : (mode == MODE_INPUT) ? '>' : ':';
    tb_printf(0, h - 1, TB_GREEN, 0, "%c%s_", mprefix, prompt_buf);
    tb_set_cursor((int)strlen(prompt_buf) + 2, h - 1);
  }
  tb_present();
}

/* ---- 交互动作 ---- */

/* 激活元素(link/button 导航;input 进 INPUT;select 聚焦回 BROWSE) */
static void activate_elem(int idx) {
  if (idx < 0 || !g_v) return;
  struct tb_elem e;
  if (tb_view_elem(g_v, idx, &e)) return;
  if (strcmp(e.type, "link") == 0 || strcmp(e.type, "button") == 0) {
    tb_click(g_b, e.id);             /* 内部做相对 href 解析;button 自动提交所属表单 */
    refresh_view();                  /* refresh_view 内部退出 HINT */
  } else if (strcmp(e.type, "input") == 0) {
    focus = idx;
    enter_mode(MODE_INPUT);
  } else if (strcmp(e.type, "select") == 0) {
    focus = idx;
    enter_mode(MODE_BROWSE);
  }
}

static void activate_focused(void) {
  activate_elem(focus);
}

/* 收集全部可点元素;无可点元素则不进入 HINT */
static void enter_hint(void) {
  if (!g_v) return;
  int n = tb_view_nelems(g_v);
  hint_n = 0;
  for (int i = 0; i < n && hint_n < 676; i++) {
    struct tb_elem e;
    if (tb_view_elem(g_v, i, &e)) continue;
    if (strcmp(e.type, "link") == 0 || strcmp(e.type, "button") == 0 ||
        strcmp(e.type, "input") == 0 || strcmp(e.type, "select") == 0)
      hint_elems[hint_n++] = i;
  }
  if (hint_n == 0) return;   /* 无可点元素,无操作 */
  hint_prefix_len = 0;
  hint_prefix[0] = '\0';   /* 清残留前缀,避免模式行显示陈旧字符 */
  mode = MODE_HINT;
}

/* select 切换选项;dir=±1 下一个/上一个(循环) */
static void cycle_select(int dir) {
  if (focus < 0 || !g_v) return;
  struct tb_elem e;
  if (tb_view_elem(g_v, focus, &e)) return;
  if (strcmp(e.type, "select") != 0 || e.noptions <= 0) return;
  int idx = 0;
  if (e.value && e.options) {
    for (int i = 0; i < e.noptions; i++)
      if (e.options[i] && strcmp(e.options[i], e.value) == 0) { idx = i; break; }
  }
  idx = (idx + dir + e.noptions) % e.noptions;
  tb_select(g_b, e.id, e.options[idx]);
  refresh_view();
}

static void submit_fill(void) {
  if (focus < 0 || !g_v) return;
  struct tb_elem e;
  if (tb_view_elem(g_v, focus, &e)) return;
  tb_fill(g_b, e.id, prompt_buf);
  refresh_view();
  enter_mode(MODE_BROWSE);
}

/* 执行 CMD 模式输入:命令或 URL(先拷贝,prompt_reset 会清空 prompt_buf) */
static void run_command(void) {
  char s[256];
  snprintf(s, sizeof s, "%s", prompt_buf);
  enter_mode(MODE_BROWSE);
  if (strcmp(s, "q") == 0 || strcmp(s, "quit") == 0) { exit(0); }
  if (strcmp(s, "b") == 0 || strcmp(s, "back") == 0) { tb_back(g_b); refresh_view(); return; }
  if (strcmp(s, "f") == 0 || strcmp(s, "forward") == 0) { tb_forward(g_b); refresh_view(); return; }
  if (strcmp(s, "r") == 0 || strcmp(s, "reload") == 0) { tb_reload(g_b); refresh_view(); return; }
  if (*s) navigate_input(s);
}

/* 搜索:跳到下一个匹配行 */
static void next_search_match(void) {
  if (search_str_len == 0) return;
  int line = find_line(scroll_top + 1, search_str);
  if (line < 0) line = find_line(0, search_str);
  if (line >= 0) {
    scroll_top = line;
    int visible = tb_height() - 3 - 5 + 1;   /* 与 redraw 的可见行数近似对齐 */
    if (visible > 1) scroll_top = line;
  }
}

/* ---- 按键分发 ---- */

static void browse_key(struct tb_event *ev) {
  int n = g_v ? tb_view_nelems(g_v) : 0;
  if (ev->key == TB_KEY_CTRL_Q || ev->key == TB_KEY_CTRL_C) exit(0);
  else if (ev->ch == 'q') exit(0);
  else if (ev->ch == ':') enter_mode(MODE_CMD);
  else if (ev->ch == 'f') enter_hint();
  else if (ev->ch == '/') enter_mode(MODE_SEARCH);
  else if (ev->ch == 'j' || ev->key == TB_KEY_ARROW_DOWN) scroll_top++;
  else if (ev->ch == 'k' || ev->key == TB_KEY_ARROW_UP) scroll_top = scroll_top > 0 ? scroll_top - 1 : 0;
  else if (ev->ch == 'g') scroll_top = 0;
  else if (ev->ch == 'G') scroll_top = 0x7fffffff;
  else if (ev->ch == ' ') scroll_top += tb_height() - 8;
  else if (ev->key == TB_KEY_PGDN) scroll_top += tb_height() - 8;
  else if (ev->key == TB_KEY_PGUP) scroll_top -= tb_height() - 8;
  else if (ev->key == TB_KEY_HOME) scroll_top = 0;
  else if (ev->key == TB_KEY_END) scroll_top = 0x7fffffff;
  else if (ev->key == TB_KEY_TAB) focus = n > 0 ? (focus + 1) % n : -1;
  else if (ev->ch >= '0' && ev->ch <= '9') focus = ev->ch - '0';
  else if (ev->ch == 'i') {
    if (focus >= 0 && n > 0) {
      struct tb_elem e;
      if (tb_view_elem(g_v, focus, &e) == 0 && strcmp(e.type, "input") == 0)
        enter_mode(MODE_INPUT);
    }
  }
  else if (ev->key == TB_KEY_ENTER) activate_focused();
  else if (ev->ch == 'h' || ev->key == TB_KEY_ARROW_LEFT) cycle_select(-1);
  else if (ev->ch == 'l' || ev->key == TB_KEY_ARROW_RIGHT) cycle_select(1);
}

/* ---- HINT 模式 ---- */

static void hint_key(struct tb_event *ev) {
  if (ev->key == TB_KEY_ESC) { enter_mode(MODE_BROWSE); return; }
  if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
    if (hint_prefix_len > 0) hint_prefix_len--;
    hint_prefix[hint_prefix_len] = '\0';   /* 截短后补终止符,模式行不显示陈旧字符 */
    return;
  }
  if (ev->ch < 'a' || ev->ch > 'z') return;
  char np[4]; int len = 0;
  if (hint_prefix_len > 0) np[len++] = hint_prefix[0];
  if (hint_prefix_len > 1) np[len++] = hint_prefix[1];
  np[len++] = (char)ev->ch;
  np[len] = '\0';
  /* 唯一精确匹配(hint == 输入) → 激活 */
  for (int k = 0; k < hint_n; k++) {
    char h[4] = {0};
    size_t hn = tb_hint_keys((size_t)hint_n, (size_t)k, h, sizeof h);
    if ((int)hn == len && strncmp(h, np, (size_t)len) == 0) { activate_elem(hint_elems[k]); return; }
  }
  /* 无前缀延伸候选 → 忽略 */
  int any = 0;
  for (int k = 0; k < hint_n; k++) {
    char h[4] = {0};
    size_t hn = tb_hint_keys((size_t)hint_n, (size_t)k, h, sizeof h);
    if ((int)hn >= len && strncmp(h, np, (size_t)len) == 0) { any = 1; break; }
  }
  if (!any) return;
  hint_prefix[0] = np[0];
  hint_prefix[1] = np[1];
  hint_prefix_len = len;
}

static void cmd_key(struct tb_event *ev) {
  if (ev->key == TB_KEY_ESC) enter_mode(MODE_BROWSE);
  else if (ev->key == TB_KEY_ENTER) run_command();
  else if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
    if (prompt_len > 0) prompt_buf[--prompt_len] = '\0';
  }
  else if (ev->ch >= 0x20 && ev->ch < 0x7f) {
    if (prompt_len < (int)sizeof prompt_buf - 1) {
      prompt_buf[prompt_len++] = (char)ev->ch;
      prompt_buf[prompt_len] = '\0';
    }
  }
}

static void search_key(struct tb_event *ev) {
  if (ev->key == TB_KEY_ESC) { enter_mode(MODE_BROWSE); }
  else if (ev->key == TB_KEY_ENTER) { next_search_match(); }
  else if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
    if (prompt_len > 0) prompt_buf[--prompt_len] = '\0';
  }
  else if (ev->ch >= 0x20 && ev->ch < 0x7f) {
    if (prompt_len < (int)sizeof prompt_buf - 1) {
      prompt_buf[prompt_len++] = (char)ev->ch;
      prompt_buf[prompt_len] = '\0';
    }
  }
  search_str_len = prompt_len;
  memcpy(search_str, prompt_buf, (size_t)prompt_len + 1);
  if (search_str_len > 0) {
    int line = find_line(scroll_top, search_str);
    if (line >= 0) scroll_top = line;
  }
}

static void input_key(struct tb_event *ev) {
  if (ev->key == TB_KEY_ESC) enter_mode(MODE_BROWSE);
  else if (ev->key == TB_KEY_ENTER) submit_fill();
  else if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
    if (prompt_len > 0) prompt_buf[--prompt_len] = '\0';
  }
  else if (ev->ch >= 0x20 && ev->ch < 0x7f) {
    if (prompt_len < (int)sizeof prompt_buf - 1) {
      prompt_buf[prompt_len++] = (char)ev->ch;
      prompt_buf[prompt_len] = '\0';
    }
  }
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: tb <url>\n"); return 2; }
  if (tb_init() != 0) { fprintf(stderr, "termbox init failed\n"); return 1; }
  tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE);
  tb_config cfg = {0};
  cfg.idle_grace_ms = 300;
  cfg.user_agent = "tb/0.1";
  g_b = tb_create(&cfg);
  if (!g_b) { tb_shutdown(); return 1; }
  tb_navigate(g_b, argv[1]);
  refresh_view();

  int running = 1;
  while (running) {
    redraw();
    struct tb_event ev;
    int pr = tb_poll_event(&ev);
    if (pr == TB_ERR_POLL) continue;   /* SIGWINCH 中断 poll,重试;下次返回 resize 事件 */
    if (pr < 0) { fprintf(stderr, "\nPOLLERR %d\n", pr); break; }
    switch (ev.type) {
      case TB_EVENT_KEY:
        if (mode == MODE_CMD) cmd_key(&ev);
        else if (mode == MODE_SEARCH) search_key(&ev);
        else if (mode == MODE_INPUT) input_key(&ev);
        else if (mode == MODE_HINT) hint_key(&ev);
        else browse_key(&ev);
        break;
      case TB_EVENT_MOUSE:
        if ((ev.key == TB_KEY_MOUSE_WHEEL_UP || ev.key == TB_KEY_MOUSE_WHEEL_DOWN) &&
            (mode == MODE_BROWSE || mode == MODE_HINT)) {
          scroll_top += (ev.key == TB_KEY_MOUSE_WHEEL_DOWN) ? 1 : -1;   /* redraw 内钳制 */
        } else if (ev.key == TB_KEY_MOUSE_LEFT && mode == MODE_BROWSE && g_v) {
          /* 点击底栏元素区 → 选中;再点已选中项 → 激活。
             元素列表浮动在正文之后(与 redraw 相同公式):
             elem_top = 2 + clamp(正文总行数 - scroll_top, 0, visible)。 */
          int nelems = tb_view_nelems(g_v);
          int elem_rows = nelems < 5 ? nelems : 5;
          int hgt = tb_height();
          int body_bottom = hgt - 3 - elem_rows;
          if (body_bottom < 3) body_bottom = 3;
          const char *text = tb_view_text(g_v);
          int total = count_lines(text);
          int visible = body_bottom - 2 + 1;
          int rendered = total - scroll_top;
          if (rendered < 0) rendered = 0;
          if (rendered > visible) rendered = visible;
          int elem_top = 2 + rendered;
          if (ev.y >= elem_top && ev.y < elem_top + elem_rows) {
            int idx = ev.y - elem_top;
            if (idx == focus) activate_focused();
            else focus = idx;
          } else if (ev.y >= 2 && ev.y < elem_top) {
            /* 正文点击命中:屏幕行列 → 文档行列 → 字节偏移 */
            int row = scroll_top + (ev.y - 2);
            size_t pos = tb_pos_to_off(text, row, ev.x);
            int hit = -1;
            for (int i = 0; i < nelems; i++) {
              struct tb_elem e;
              if (tb_view_elem(g_v, i, &e)) continue;
              const char *t = e.type;
              if (strcmp(t, "link") != 0 && strcmp(t, "button") != 0 &&
                  strcmp(t, "input") != 0) continue;
              int erow;
              tb_off_to_pos(text, e.off, &erow, NULL);
              if (erow != row) continue;
              size_t elen = e.text ? strlen(e.text) : 0;
              if (e.off <= pos && pos < e.off + elen) { hit = i; break; }
            }
            if (hit >= 0) activate_elem(hit);
          }
        }
        break;
      case TB_EVENT_RESIZE:
        break;
    }
  }
  tb_view_free(g_v);
  tb_destroy(g_b);
  tb_shutdown();
  return 0;
}
