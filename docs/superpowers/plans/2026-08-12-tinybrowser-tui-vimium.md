# Tinybrowser M1 TUI 重做(vimium 式交互)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 M1 TUI 交互从四模式重做为五模式 vimium 式——f 键呼出多字母 hint 覆盖正文直达可点元素,并加鼠标滚轮滚动与左键命中。

**Architecture:** 三个独立可测单元,按依赖顺序推进:(1) render 层给 `struct tb_elem` 加 `size_t off`(元素文本在 `v->text` 的字节偏移),裁剪后平移;(2) 新增纯函数模块 `src/core/hint.c`(hint 键位分配 + off↔(row,col) 转换),零依赖可单测;(3) `tb_cli.c` 加 `MODE_HINT` 状态机 + hint 覆盖渲染 + 鼠标处理,复用现有 `tb_click`/`tb_fill`/`tb_select`。golden 快照保持零改动。

**Tech Stack:** C99(libtinybrowser.a)、termbox2(TUI)、lexbor(DOM)、CMake + gtest(测试)。

## Global Constraints

- `struct tb_elem` 仅新增 `size_t off` 字段,不新增任何公共 API 函数。
- `tb_view_dump` **不打印** off → hello/nav/forms 三个 golden 文件必须字节零改动(以 golden_runner 通过为准)。
- hint 分配:N ≤ 26 单字母 `keys[i]`;N > 26 全两字母 `keys[i%26] + keys[i/26]`;无单/双混入;覆盖 N ≤ 676。
- HINT 模式鼠标左键忽略;滚轮在 BROWSE/HINT 均生效。
- 元素 text 跨多行时 off 指向首行起点;命中只看首行起点,不做多行范围追踪。
- 无可点元素按 f 不进入 HINT;hint 无匹配按键忽略;空 `v->text` 路径安全返回。
- 测试模式沿用现有:每测试一个 executable + `TB_TEST_INCS` + `gtest_discover_tests`;golden 经 `add_test(NAME golden ...)`。
- 构建命令:`make`(配置+构建)、`make test`(配置+构建+ctest)、`make cli`(只建 tb)。

---

### Task 1:render 层 off 字段

给元素坐标打地基:`struct tb_elem` 加 `off`;`add_elem` 记录写入缓冲时的文本长度;`tb_render` 首尾空行裁剪后把全部 elems 的 off 平移;`tb_view_clone` 拷贝 off;dump 不打印 off(golden 零改动)。用 `test_render_coord.cc` 证明每个可点元素 `off` 精确指向其文本起点。

**Files:**
- Modify: `src/tb.h:102-110`(struct tb_elem)
- Modify: `src/core/render.c:63-84`(add_elem)、`src/core/render.c:171-181`(tb_render 裁剪)
- Modify: `src/core/view.c:37-66`(tb_view_clone)
- Test: `tests/unit/test_render_coord.cc`(新建)
- Modify: `tests/CMakeLists.txt`(末尾追加 test_render_coord 条目)

**Interfaces:**
- Produces:`struct tb_elem.off`(size_t)语义 = 元素文本在 `tb_view_text(v)` 中的起始字节偏移;裁剪后仍有效。`tb_view_elem` 浅拷贝自然携带。

- [ ] **Step 1:写失败测试 `tests/unit/test_render_coord.cc`**

```cpp
#include "dom.h"
#include "render.h"
#include "tb.h"
#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <string>
#include <string.h>

static std::string slurp(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static tb_view *render_fixture(const char *name) {
  std::string dir = TB_TEST_SRC_DIR + std::string("tests/goldens/fixtures/");
  std::string html = slurp(dir + name + ".html");
  tb_dom *d = tb_dom_parse(html.data(), html.size());
  tb_view *v = tb_render(d, ("http://example.test/" + std::string(name)).c_str(), 200);
  tb_dom_free(d);
  return v;
}

static bool is_clickable(const char *type) {
  return strcmp(type, "link") == 0 || strcmp(type, "button") == 0 ||
         strcmp(type, "input") == 0 || strcmp(type, "select") == 0;
}

TEST(RenderCoord, OffPointsAtElemTextStart) {
  for (const char *name : {"hello", "nav", "forms"}) {
    tb_view *v = render_fixture(name);
    const char *text = tb_view_text(v);
    ASSERT_NE(text, nullptr);
    for (int i = 0; i < tb_view_nelems(v); i++) {
      struct tb_elem e{};
      ASSERT_EQ(tb_view_elem(v, i, &e), 0);
      if (!is_clickable(e.type)) continue;
      ASSERT_NE(e.text, nullptr);
      size_t len = strlen(e.text);
      ASSERT_LE(e.off, strlen(text)) << name << " elem " << i;
      ASSERT_LE(e.off + len, strlen(text)) << name << " elem " << i;
      // 更强断言(优于 strstr):off 处前缀必须恰好等于该元素文本
      EXPECT_EQ(strncmp(text + e.off, e.text, len), 0) << name << " elem " << i;
    }
    tb_view_free(v);
  }
}
```

- [ ] **Step 2:构建并确认测试失败**

Run: `make test 2>&1 | tail -20`
Expected: 编译失败,`struct tb_elem` 没有成员 `off`(test_render_coord 编不过)。这是预期的 fail。

- [ ] **Step 3:实现 off 字段 + 平移**

`src/tb.h` struct tb_elem 加字段(放在 `text` 之后):

```c
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
```

`src/core/render.c` `add_elem`(memset 后、设置字段前)记录当前已写文本长度——此时元素文本尚未 push,`c->len` 即元素文本起点:

```c
static void add_elem(ctx_t *c, const char *type, tb_node *n) {
  c->elems = realloc(c->elems, (size_t)(c->nelems + 1) * sizeof(struct tb_elem));
  struct tb_elem *e = &c->elems[c->nelems++];
  memset(e, 0, sizeof *e);
  e->off = c->len;
  e->id = tb_dom_id(c->dom, n);
  ...
```

`src/core/render.c` `tb_render` 尾部裁剪处,平移全部 elems 的 off(前导换行都位于首个内容之前,所以所有 `off >= start`,直接减不越界):

```c
  if (start) {
    memmove(text, text + start, end - start + 1);
    for (int i = 0; i < c.nelems; i++) c.elems[i].off -= start;
  }
```

`src/core/view.c` `tb_view_clone` 逐字段拷贝处加一行:

```c
      t->id = s->id;
      t->type = s->type;  /* type 是字符串字面量,不拥有 */
      t->off = s->off;
      t->text = dup(s->text);
```

`tests/CMakeLists.txt` 末尾追加:

```cmake
add_executable(test_render_coord unit/test_render_coord.cc)
target_include_directories(test_render_coord PRIVATE ${TB_TEST_INCS})
target_compile_definitions(test_render_coord PRIVATE TB_TEST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")
target_link_libraries(test_render_coord PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_render_coord)
```

- [ ] **Step 4:构建 + 跑新测试 + golden 全绿**

Run: `make test 2>&1 | tail -25`
Expected: test_render_coord 通过(3 个 fixture、全部可点元素 off 精确);`golden` 测试 OK,hello/nav/forms 三个 golden 零改动(off 未泄漏进 dump)。

- [ ] **Step 5:提交**

```bash
git add src/tb.h src/core/render.c src/core/view.c tests/unit/test_render_coord.cc tests/CMakeLists.txt
git commit -m "feat(render): record element text offset for hint hit-testing"
```

---

### Task 2:hint 纯函数模块

新增 `src/core/hint.c`/`hint.h`,含两个纯函数族:`tb_hint_keys`(vimium 键位分配,自洽无冲突)+ `tb_off_to_pos`/`tb_pos_to_off`(字节偏移↔行列)。零依赖,单测直接可测。接线进 tinybrowser 源列表与 test_hint 测试目标。

**Files:**
- Create: `src/core/hint.h`、`src/core/hint.c`
- Modify: `CMakeLists.txt:71-81`(tinybrowser 源列表加 hint.c)
- Test: `tests/unit/test_hint.cc`(新建)
- Modify: `tests/CMakeLists.txt`(末尾追加 test_hint 条目)

**Interfaces:**
- Consumes: 无(纯标准库)。
- Produces(Task 3/4 依赖,签名必须一致):
  - `size_t tb_hint_keys(size_t n, size_t i, char *dst, size_t cap)` — 第 i 个可点元素(共 n 个)的 hint 键位写入 dst;返回字符数(1 或 2),`i >= n || cap < 2` 返回 0。
  - `void tb_off_to_pos(const char *text, size_t off, int *row, int *col)` — 字节偏移→(行,列),均 0 起;text 可 NULL(输出 0,0)。
  - `size_t tb_pos_to_off(const char *text, int row, int col)` — (行,列)→字节偏移;行/列越界钳制到文本末尾;text 可 NULL(返回 0)。

- [ ] **Step 1:写失败测试 `tests/unit/test_hint.cc`**

```cpp
#include "hint.h"
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <string.h>

TEST(HintKeys, SingleLetterUpTo26) {
  char dst[4] = {0};
  for (size_t i = 0; i < 26; i++) {
    ASSERT_EQ(tb_hint_keys(26, i, dst, sizeof dst), 1u);
    EXPECT_EQ(dst[0], (char)('a' + i));
  }
  EXPECT_EQ(tb_hint_keys(26, 26, dst, sizeof dst), 0u);  /* 越界 */
}

TEST(HintKeys, SingleElement) {
  char dst[4] = {0};
  ASSERT_EQ(tb_hint_keys(1, 0, dst, sizeof dst), 1u);
  EXPECT_EQ(dst[0], 'a');
}

TEST(HintKeys, HundredElementsAllTwoLetter) {
  char dst[4] = {0};
  for (size_t i = 0; i < 100; i++)
    EXPECT_EQ(tb_hint_keys(100, i, dst, sizeof dst), 2u);
}

TEST(HintKeys, TwoLettersAbove26) {
  char dst[4] = {0};
  ASSERT_EQ(tb_hint_keys(27, 0, dst, sizeof dst), 2u);
  EXPECT_EQ(dst[0], 'a'); EXPECT_EQ(dst[1], 'a');
  ASSERT_EQ(tb_hint_keys(27, 26, dst, sizeof dst), 2u);
  EXPECT_EQ(dst[0], 'a'); EXPECT_EQ(dst[1], 'b');   /* i=26 → i%26=0 → 'a', i/26=1 → 'b' */
}

TEST(HintKeys, AllUniqueUpTo676) {
  std::set<std::string> seen;
  for (size_t i = 0; i < 676; i++) {
    char dst[4] = {0};
    ASSERT_EQ(tb_hint_keys(676, i, dst, sizeof dst), 2u);
    seen.insert(std::string(dst, 2));
  }
  EXPECT_EQ(seen.size(), 676u);   /* 全部 hint 唯一 */
}

TEST(Coord, OffToPosBasics) {
  int r = -1, c = -1;
  tb_off_to_pos("ab\ncd", 0, &r, &c); EXPECT_EQ(r, 0); EXPECT_EQ(c, 0);
  tb_off_to_pos("ab\ncd", 2, &r, &c); EXPECT_EQ(r, 0); EXPECT_EQ(c, 2);
  tb_off_to_pos("ab\ncd", 3, &r, &c); EXPECT_EQ(r, 1); EXPECT_EQ(c, 0);
  tb_off_to_pos("ab\ncd", 5, &r, &c); EXPECT_EQ(r, 1); EXPECT_EQ(c, 2);
}

TEST(Coord, PosToOffBasics) {
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 0, 0), 0u);
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 0, 2), 2u);
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 1, 0), 3u);
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 1, 2), 5u);
}

TEST(Coord, RoundTrip) {
  const char *text = "line one\nline two\nline three\nlast";
  for (size_t off = 0; off <= strlen(text); off++) {
    if (off < strlen(text) && text[off] == '\n') continue;  /* 换行符无唯一往返(映射到下行起点) */
    int r, c;
    tb_off_to_pos(text, off, &r, &c);
    EXPECT_EQ(tb_pos_to_off(text, r, c), off);
  }
}

TEST(Coord, EdgeCases) {
  int r, c;
  tb_off_to_pos("", 0, &r, &c);       EXPECT_EQ(r, 0); EXPECT_EQ(c, 0);
  tb_off_to_pos(NULL, 0, &r, &c);     EXPECT_EQ(r, 0); EXPECT_EQ(c, 0);
  EXPECT_EQ(tb_pos_to_off(NULL, 0, 0), 0u);
  EXPECT_EQ(tb_pos_to_off("abc", 5, 0), 3u);    /* row 越界 → 末尾 */
  EXPECT_EQ(tb_pos_to_off("abc", 0, 99), 3u);   /* col 越界 → 行尾 */
}
```

- [ ] **Step 2:构建并确认测试失败**

Run: `make test 2>&1 | tail -20`
Expected: 编译失败,`hint.h` 不存在(test_hint 编不过)。这是预期的 fail。

- [ ] **Step 3:实现 hint.h / hint.c + CMake 接线**

Create `src/core/hint.h`:

```c
#ifndef TB_CORE_HINT_H
#define TB_CORE_HINT_H
#include <stddef.h>

/* vimium 式 hint 键位分配。n = 可点元素总数,i = 元素序号(0-based)。
   n<=26:单字母 keys[i];n>26:全两字母 keys[i%26] + keys[i/26](N<=676 全唯一)。
   返回写入 dst 的字符数(1 或 2);i>=n 或 cap<2 返回 0。dst 需至少 2 字节。 */
size_t tb_hint_keys(size_t n, size_t i, char *dst, size_t cap);

/* 字节偏移 → (行,列),均 0 起。text 可 NULL。 */
void tb_off_to_pos(const char *text, size_t off, int *row, int *col);

/* (行,列) → 字节偏移;越界钳制到文本末尾。text 可 NULL。 */
size_t tb_pos_to_off(const char *text, int row, int col);

#endif
```

Create `src/core/hint.c`:

```c
#include "hint.h"

static const char KEYS[] = "abcdefghijklmnopqrstuvwxyz";

size_t tb_hint_keys(size_t n, size_t i, char *dst, size_t cap) {
  if (i >= n || cap < 2) return 0;
  if (n <= 26) {
    dst[0] = KEYS[i];
    return 1;
  }
  dst[0] = KEYS[i % 26];
  dst[1] = KEYS[i / 26];
  return 2;
}

void tb_off_to_pos(const char *text, size_t off, int *row, int *col) {
  int r = 0, c = 0;
  if (text) {
    for (size_t i = 0; i < off && text[i] != '\0'; i++) {
      if (text[i] == '\n') { r++; c = 0; }
      else c++;
    }
  }
  if (row) *row = r;
  if (col) *col = c;
}

size_t tb_pos_to_off(const char *text, int row, int col) {
  if (!text) return 0;
  size_t off = 0;
  int r = 0;
  while (*text) {
    if (r == row) break;
    if (*text++ == '\n') r++;
    off++;
  }
  /* text 现指向第 row 行起点(或已到末尾) */
  int c = 0;
  while (*text && *text != '\n' && c < col) { text++; off++; c++; }
  return off;
}
```

`CMakeLists.txt` tinybrowser 源列表,在 `src/core/render.c` 后加 `src/core/hint.c`:

```cmake
add_library(tinybrowser STATIC
  src/core/browser.c
  src/core/clock.c
  src/core/content.c
  src/core/url.c
  src/core/dom.c
  src/core/view.c
  src/core/render.c
  src/core/hint.c
  src/core/session.c
  src/core/curl_transport.c
)
```

`tests/CMakeLists.txt` 末尾追加:

```cmake
add_executable(test_hint unit/test_hint.cc)
target_include_directories(test_hint PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_hint PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_hint)
```

- [ ] **Step 4:构建 + 跑测试确认通过**

Run: `make test 2>&1 | tail -25`
Expected: test_hint 全部通过;其余测试与 golden 不受影响。

- [ ] **Step 5:提交**

```bash
git add src/core/hint.h src/core/hint.c CMakeLists.txt tests/unit/test_hint.cc tests/CMakeLists.txt
git commit -m "feat(core): vimium hint key assignment and off/pos helpers"
```

---

### Task 3:TUI HINT 模式(键盘)

`tb_cli.c` 加 `MODE_HINT`:f 进入(无可点元素则无操作)、hint 覆盖正文渲染(TB_YELLOW)、a-z 前缀过滤与唯一精确匹配即激活、Backspace 回退、Esc 退出、导航后自动退出、resize 保持。激活逻辑抽成 `activate_elem(idx)` 供 HINT 与 BROWSE 复用。无自动测试可写(TUI),以手动冒烟验证。

**Files:**
- Modify: `src/frontends/cli/tb_cli.c`(枚举、状态、redraw、browse_key、main loop、顶注)
- Modify: `CMakeLists.txt:109-113`(tb target 加 src/core include)

**Interfaces:**
- Consumes:Task 1 的 `struct tb_elem.off`;Task 2 的 `tb_hint_keys`/`tb_off_to_pos`。
- Produces:Task 4 复用的 `activate_elem(int idx)` 静态函数;`MODE_HINT` 枚举值。

- [ ] **Step 1:先完成 CMake include 接线**

`CMakeLists.txt` tb 目标加 `src/core` 头路径(测试已含,此处仅 tb 需要):

```cmake
if(TB_BUILD_CLI AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/cli/tb_cli.c)
  add_executable(tb src/frontends/cli/tb_cli.c)
  target_link_libraries(tb PRIVATE tinybrowser)
  target_include_directories(tb PRIVATE deps/termbox2 ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
endif()
```

Run: `make cli`
Expected: 构建成功(tb_cli.c 尚未用 hint.h,无行为变化)。

- [ ] **Step 2:改 `tb_cli.c` 头注释 + 枚举 + 状态**

头注释的模式行改为五模式说明:

```c
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
```

枚举与状态(顶注下方):

```c
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
static char hint_prefix[3];   /* 已输前缀(最长 2) */
static int hint_prefix_len;
```

`mode_name()` 加 HINT 分支:

```c
static const char *mode_name(void) {
  switch (mode) {
    case MODE_CMD:    return "CMD";
    case MODE_SEARCH: return "SEARCH";
    case MODE_INPUT:  return "INPUT";
    case MODE_HINT:   return "HINT";
    default:          return "BROWSE";
  }
}
```

- [ ] **Step 3:新增 enter_hint / activate_elem,并让 refresh_view 退出 HINT**

在 `refresh_view` 定义里,末尾追加退出 HINT(新导航后候选失效):

```c
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
```

把现有 `activate_focused` 重构为按索引的 `activate_elem`,并新增 `enter_hint`(放在 `activate_focused` 原位置):

```c
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
  mode = MODE_HINT;
}
```

`browse_key` 加 f 分支(放在 `:` 分支附近):

```c
  else if (ev->ch == ':') enter_mode(MODE_CMD);
  else if (ev->ch == 'f') enter_hint();
  else if (ev->ch == '/') enter_mode(MODE_SEARCH);
```

- [ ] **Step 4:新增 hint_key 按键处理**

在 `browse_key` 之后加:

```c
/* ---- HINT 模式 ---- */

static void hint_key(struct tb_event *ev) {
  if (ev->key == TB_KEY_ESC) { enter_mode(MODE_BROWSE); return; }
  if (ev->key == TB_KEY_BACKSPACE || ev->key == TB_KEY_BACKSPACE2) {
    if (hint_prefix_len > 0) hint_prefix_len--;
    return;
  }
  if (ev->ch < 'a' || ev->ch > 'z') return;
  char np[3]; int len = 0;
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
```

- [ ] **Step 5:redraw 画 hint 覆盖 + 模式行 + main loop 分发**

`redraw` 中,正文循环结束(底栏之前)插入 hint 覆盖(仅当前候选:前缀匹配者画剩余字母,未匹配者不画;可见区外跳过):

```c
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
```

模式行分支改造(原 `if (mode == MODE_BROWSE) ... else ...`):

```c
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
```

main loop 按键分发加 HINT 分支:

```c
      case TB_EVENT_KEY:
        if (mode == MODE_CMD) cmd_key(&ev);
        else if (mode == MODE_SEARCH) search_key(&ev);
        else if (mode == MODE_INPUT) input_key(&ev);
        else if (mode == MODE_HINT) hint_key(&ev);
        else browse_key(&ev);
        break;
```

- [ ] **Step 6:构建 + 手动冒烟(键盘路径)**

Run: `make cli`

冒烟(真实终端,需本地服务器 + 页面):
```bash
# 起本地服务器,页面含 40 个链接(覆盖单字母+两字母两档)
python3 - <<'EOF' >/tmp/tb_hint.html
links = "".join(f'<a href="http://example.com/{i}">link{i}</a> ' for i in range(40))
print(f"<html><body><h1>Hint smoke</h1><p>{links}</p></body></html>")
EOF
python3 -m http.server 8000 --directory /tmp &   # 后台
./build/tb http://127.0.0.1:8000/tb_hint.html
```
Expected:
- 按 `f`:正文每个链接文本起点出现黄色 hint(前 26 个单字母 a–z,其余两字母 aa/ba/ca…),模式行显示 `hint: _`。
- 按两字母 hint 首字母(如 `a`):只剩 `a*` 组候选显示剩余字母,按次字母即导航(URL 变化,回 BROWSE)。
- 按单字母 hint(如 `z`)一次即导航。
- `Backspace` 回退前缀,Esc 退出回 BROWSE。
- 无链接页面验证:`printf '<html><body><p>plain text, no links</p></body></html>' > /tmp/tb_plain.html`,开一个新 tab 加载 `http://127.0.0.1:8000/tb_plain.html`,按 `f` 应无操作(不进入 HINT)。

- [ ] **Step 7:提交**

```bash
git add src/frontends/cli/tb_cli.c CMakeLists.txt
git commit -m "feat(tui): vimium-style hint mode (f key)"
```

---

### Task 4:TUI 鼠标支持

滚轮在 BROWSE/HINT 滚动;左键在 BROWSE 命中正文可点元素(off→行列逆命中)并激活,底栏鼠标逻辑保留;HINT 左键忽略。

**Files:**
- Modify: `src/frontends/cli/tb_cli.c:341-353`(main loop TB_EVENT_MOUSE 分支)

**Interfaces:**
- Consumes:Task 2 `tb_pos_to_off`/`tb_off_to_pos`;Task 3 `activate_elem`。
- Produces:无新函数(全部改 main loop 鼠标分支)。

- [ ] **Step 1:改 main loop 鼠标分支**

现有 `TB_EVENT_MOUSE` 分支(仅底栏左键)替换为:

```c
      case TB_EVENT_MOUSE:
        if ((ev->key == TB_KEY_MOUSE_WHEEL_UP || ev->key == TB_KEY_MOUSE_WHEEL_DOWN) &&
            (mode == MODE_BROWSE || mode == MODE_HINT)) {
          scroll_top += (ev->key == TB_KEY_MOUSE_WHEEL_DOWN) ? 1 : -1;   /* redraw 内钳制 */
        } else if (ev->key == TB_KEY_MOUSE_LEFT && mode == MODE_BROWSE && g_v) {
          /* 点击底栏元素区 → 选中;再点已选中项 → 激活(现有逻辑保留) */
          int nelems = tb_view_nelems(g_v);
          int elem_rows = nelems < 5 ? nelems : 5;
          int elem_top = tb_height() - 3 - elem_rows;
          if (ev->y >= elem_top && ev->y < elem_top + elem_rows) {
            int idx = ev->y - elem_top;
            if (idx == focus) activate_focused();
            else focus = idx;
          } else if (ev->y >= 2 && ev->y <= elem_top - 1) {
            /* 正文点击命中:屏幕行列 → 文档行列 → 字节偏移 */
            int row = scroll_top + (ev->y - 2);
            const char *text = tb_view_text(g_v);
            size_t pos = tb_pos_to_off(text, row, ev->x);
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
```

注:`elem_top - 1` 即正文区底(`body_bottom`);顶栏两行(0–1)不在正文区,自然排除。select 不含在正文命中里(无 text),只能经 hint/focus 交互,与 spec §5 一致。

- [ ] **Step 2:构建 + 手动冒烟(鼠标路径)**

Run: `make cli`
Run: `./build/tb http://127.0.0.1:8000/tb_hint.html`(沿用 Task 3 的本地服务器)
Expected:
- 滚轮下滑 → 正文上滚;滚轮上滑 → 下滚;HINT 模式滚轮同样生效。
- 左键点正文链接文本 → 直接导航;点空白处无操作。
- 左键点底栏元素 → 选中(高亮),再点已选中 → 激活;行为与旧版一致。
- HINT 模式左键正文 → 忽略(不导航、不激活)。

- [ ] **Step 3:提交**

```bash
git add src/frontends/cli/tb_cli.c
git commit -m "feat(tui): mouse wheel scroll and left-click hit-testing"
```

---

### Task 5:全量回归 + 冒烟清单

收尾:全测试跑通、golden 零改动复核、手动冒烟走查一遍 spec §7 的完整清单。无新代码逻辑。

**Files:**
- Modify: 无(仅验证)

- [ ] **Step 1:全量测试**

Run: `make test 2>&1 | tail -30`
Expected: 全部测试通过(含 test_hint、test_render_coord、golden 3 fixtures OK);无 MISMATCH。

- [ ] **Step 2:确认 golden 未变**

Run: `git status --porcelain tests/goldens/`
Expected: 空输出(goldens/ 目录无改动,off 未泄漏进 dump)。

- [ ] **Step 3:完整 TUI 冒烟(spec §7)**

Run: `./build/tb http://127.0.0.1:8000/tb_hint.html`(真实终端)
Expected(逐项核对):
- `f` → hint 覆盖出现(黄字);`a-z` 单字母一次激活;两字母分两步激活。
- `Esc` 退出 HINT、`Backspace` 回退前缀。
- 激活后回 BROWSE 且新视图无残留 hint。
- 导航(激活链接)后自动退出 HINT。
- 无链接页面按 `f` 无操作。
- resize(拖终端窗口)后 HINT 状态与前缀保持,下轮 redraw 正确重画。
- 滚轮滚动、左键正文命中、左键底栏选中/激活、HINT 左键忽略。
- 旧路径回归:`:` 命令、`/` 搜索、`i` 填充、Tab/数字焦点、Enter 激活、`←→` 切 select、`q` 退出。

- [ ] **Step 4:提交(如无任何改动则跳过)**

Run: `git status --porcelain`
Expected: 工作区干净(若冒烟发现 bug,先修再回到 Step 1,修复单独提交)。
