# tinybrowser M1 TUI 重做:vimium 式交互

日期:2026-08-12
状态:已批准(用户 "实施")
范围:M1 TUI(frontend `src/frontends/cli/tb_cli.c`)+ render 层最小扩展(`src/core/render.c` / `src/core/view.c` / `src/tb.h`)。完成后回归测试全绿,再规划 M2。

## 背景

M1 TUI 已交付四模式状态机(BROWSE/CMD/SEARCH/INPUT),但用户判定其交互"VIM式操作和方向键导航结合的方式"是错误的——缺少 vimium 式 hint 直达与鼠标支持,交互仍受限于底栏选项。本 spec 重做交互层为 vimium 式:**vi 键位 + 方向键保留,f 键呼出多字母 hint 覆盖正文,鼠标点击/滚轮支持**。

## 1. 交互模型:五模式

| 模式 | 进入 | 键位 |
|---|---|---|
| BROWSE | 初始 | vi:j/k/g/G/空格/PgUp/PgDn 滚动;方向键:↑↓滚动、←→select 循环;**f 进 HINT**;Tab/数字聚焦底栏;i 填充;Enter 激活焦点;`:` 命令;`/` 搜索;q/Ctrl+Q 退出 |
| HINT | BROWSE 按 f | a-z 直接激活元素;Esc 退出;Backspace 回退一级 |
| CMD `:` | `:` | 不变(现有 `run_command`) |
| SEARCH `/` | `/` | 不变(现有 `search_key`) |
| INPUT `>` | `i`/Enter 于 input | 不变(现有 `input_key`/`submit_fill`) |

`MODE_HINT` 为新增模式枚举。CMD/SEARCH/INPUT 三个模式逻辑零改动。

## 2. 元素坐标(render 层)

- `struct tb_elem`(src/tb.h)加字段 `size_t off` —— 元素文本在 `v->text` 中的起始字节偏移。
- `render.c` 的 `add_elem` 记录 `c->len` 为 `off`(此时 `c->len` 即已写文本长度)。
- **首尾空行裁剪平移**:`tb_render` 末尾裁 `start` 个前导 `\n` 并 `memmove` 文本;裁剪后遍历所有 elems,`off -= start`。
- `view.c`:
  - `tb_view_clone` 拷贝 `off`。
  - `tb_view_dump` **不打印** `off` → golden 文件零改动。
  - `tb_view_free` 无需特殊处理(off 是值)。
- 命中判定只用元素文本起始位置(见 §5),不追踪文本结束。

## 3. hint 键位分配算法(纯函数,自洽无冲突)

输入:可点元素数组(link/button/input/select,顺序即 render 的 elems 顺序),数量 N。
输出:每元素 hint 字符串。

- N ≤ 26:`hint[i] = "abcdefghijklmnopqrstuvwxyz"[i]`(单字母)。
- N > 26:全两字母 `hint[i] = keys[i%26] + keys[i/26]`。
  - 无单字母混入 → 首字母输入不会被任何单字母劫持,无歧义。
  - 首字母 `keys[i%26]` 定位一组(≤26 个),次字母 `keys[i/26]` 区分;N ≤ 676 覆盖 Web1.0 场景。

算法封装为独立函数(如 `tb_cli` 内静态函数,或 `src/core/hint.c` + 单元测试直接可测)。**推荐独立源文件** `src/core/hint.c`/`hint.h`,纯数据无依赖,便于单测。

## 4. HINT 模式交互

- 进入(按 f):计算全部可点元素 hint;候选集 = 全部可点元素;已输前缀 `hint_prefix` 为空。
- 渲染:对每个候选元素,将其 hint 字符串叠加画在元素文本起始处,`TB_YELLOW` 前景,覆盖原字符(列号由 §5 off→行列给出)。若 hint 长度 2 且已输首字母,只画剩余字母。
- 按键:
  - a-z:候选 = hint 以 `hint_prefix + c` 开头的元素。
    - 若某元素 hint 恰好 == `hint_prefix + c` 且唯一 → 激活(见下)。
    - 否则若候选非空 → `hint_prefix += c`,重画候选的剩余字母。
    - 否则忽略。
  - Backspace:`hint_prefix` 截短一字符(非空时),重画。
  - Esc:退出回 BROWSE。
  - 其余按键忽略。
- 激活动作(与 BROWSE 的 `activate_focused` 同逻辑):
  - link/button → `tb_click` + `refresh_view`,退出 HINT。
  - input → `focus` 置为该元素,退出 HINT,进入 MODE_INPUT。
  - select → `focus` 置为该元素,退出 HINT(←→ 或 vi 键位切换选项)。
- 新导航后(`refresh_view`)自动退出 HINT(候选基于旧视图失效)。
- resize:保持 HINT 状态与 `hint_prefix`,下轮 redraw 用新尺寸重画。

## 5. off→行列 与 鼠标命中

- `off_to_pos(const char *text, size_t off)` → (row, col):扫描文本数 `\n`,row 为换行数,col 为行内字节偏移。TUI 侧实现,单元测试覆盖。
- HINT 叠加定位:元素 `off` → (row, col);屏幕行 = `row - scroll_top`;col 为列。若元素在可见区外(row 超出 [scroll_top, scroll_top+visible])则跳过(不画 hint)。
- 鼠标滚轮:BROWSE/HINT 下 WHEEL_UP/DOWN → scroll_top 增减(同 vi 滚动)。
- 鼠标左键(仅 BROWSE):
  - 点底栏元素区:现有逻辑保留。
  - 点正文:点击 (x,y) → 行列 → 字节偏移(逆 off→pos,需 `pos_to_off`);找 `off` 在其行起始于该行、且 `off <= 点击偏移 < off + strlen(e.text)` 的元素 → 按 §4 激活动作处理;无命中忽略。
  - HINT 模式鼠标左键:忽略(避免歧义)。
- `pos_to_off(text, row, col)`:扫描到目标行,`off = 行起始 + col`。

## 6. 公共 API 与兼容

- 仅 `struct tb_elem` 加 `size_t off` 字段。`tb_view_elem` 浅拷贝自然带上;对外无 break。
- 无新增公共函数(TUI 内部可全用现有 API + off)。
- `tb_view_dump` 不打印 off → hello/nav/forms 三个 golden 文件零改动(以测试通过为准验证)。

## 7. 测试

- 单元(新增 `tests/unit/hint_test.cpp`):分配算法 N=1/26/27/100;断言:单字母区、双字母区、首字母组≤26、N≤676 内全部 hint 唯一、长度正确。
- 单元(新增,或并入 hint/pos 测试):`off_to_pos`/`pos_to_off` 往返一致;跨行、空文本、行尾边界。
- 单元(新增 `tests/unit/render_coord_test.cpp`):对 hello.html/nav.html/forms.html fixture,断言每个可点元素 `off` 指向其文本在 `v->text` 中的真实位置(用 `strstr(v->text, e->text)` 校验起始)。
- golden:现有 41 测试全绿,确认 off 未泄漏进 dump。
- TUI 冒烟:手测 f→hint→激活、多字母链路、Esc/Backspace、resize、鼠标点击/滚轮。

## 8. 边界与错误处理

- 无可点元素按 f:不进入 HINT(可视为无操作)。
- 元素 text 跨多行:off 指向首行起始,命中只看首行起始位置。
- HINT 中 resize:保持状态重画。
- hint 无匹配按键:忽略,不抖动。
- 空 `v->text`/`g_v` 为 NULL:`off_to_pos`/渲染路径全部安全返回。

## 非目标

- 不做 JS 运行时(M2)、不做图形/桌面渲染、不改浏览器核心 API 语义、不动 transport/session/dom 层。
- 不为 hint 命中引入多行文本范围追踪。
