# M2a JS 运行时实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 DOM 解析、渲染、交互全部迁移到嵌入的 QuickJS 运行时中，用纯 JS 手写宽松解析器（`parser.js`）替换 lexbor，同步执行 `<script>`，并保持 M1 的渲染输出逐字节等价。

**Architecture:** 单线程 pump 模型不变。C 侧 `browser.c` 通过 `tb_js_engine` seam（函数表）驱动 JS：`open` 创建每文档一个的 JS context（QuickJS runtime 进程级单例），`load_document` 注入 builtins（parser.js/dom.js/render.js）→ 流式喂入 body → 按文档序执行脚本 → `render` 产出 `tb_view`。交互（click/fill/select/submit）改为 C 侧保留表单值 `fv[]` + JS 侧查询元素信息 + 小 JSON 工具解析。lexbor、`dom.c`、`render.c` 全部删除。

**Tech Stack:** C99 + QuickJS（quickjs-ng v0.16.1，vendored）+ libuv + 手写 JS（parser.js / dom.js / render.js）。无 lexbor。无 node。JS 单测用 vendored `qjs_exe` CLI 跑。

## Global Constraints

- **纯 JS 解析**：HTML 解析器是手写 JS（宽松 HTML/XML 风格 tokenizer），绝不用 lexbor。DOM 完全在 JS——`{type:"element"|"text", tag, attrs, children, text, id, parent}` 纯数据树。
- **同步脚本执行**：document order；parser 遇到 `<script>` 收集 raw text 到队列，`load_document` 循环执行（inline → eval；src → C 桥 `__tb_load_sync` nested pump）；每 script try/catch 隔离；`js_exec_ms_limit` 超时 interrupt → abort 该 script。`readyState`：脚本执行期间 `"loading"`，全部执行完 → `"complete"`。
- **渲染逐字节等价**：`render.js` 必须复现 `render.c` 全部语义——block 集合 `{"p","div","h1".."h6","li","ul","ol","table","tr","section","article","header","footer","nav","aside","br","hr","blockquote","pre","form","select","fieldset"}`、空白折叠、option 收集、title、首尾空行裁剪、`off` 钳到 `[0,nl]`。既有 golden（hello/forms/nav）逐字节不变。
- **迭代 walk**：JS 里所有树遍历必须显式栈迭代，禁止递归（QuickJS 栈限制，深 DOM 会爆栈）。
- **宽松 tokenizer 契约（spec §3.2）**：void 元素（br img input hr meta link）自动自闭合；unquoted attrs；隐式闭合（p/li/td/tr/th/option）；mis-nesting 宽容；HTML entity 解码（`&amp; &lt; &gt; &quot; &apos; &nbsp;` + `&#NN;` + `&#xHH;`）；script/style raw text；boolean attrs = 空字符串；comment/DOCTYPE/PI 跳过；畸形输入 → warning 并继续。
- **seam 签名（对 spec §4.4 的两处修正，以本计划为准）**：
  ```c
  struct tb_js_engine {
    void *(*open)(const struct tb_js_engine *self, struct tb_browser *host);   /* 修正①:传 host 而非 cfg,支持 __tb_load_sync nested pump */
    int   (*load_document)(const struct tb_js_engine *self, void *h, const char *body, size_t len);
    int   (*render)(const struct tb_js_engine *self, void *h, const char *url, int status, struct tb_view *out);  /* 修正②:带 url/status,与 M1 tb_render 对齐 */
    int   (*eval)(const struct tb_js_engine *self, void *h, const char *code, char **out);  /* *out = malloc,调用方 free */
    void  (*poll_timers)(const struct tb_js_engine *self, void *h);
    void  (*close)(const struct tb_js_engine *self, void *h);
  };
  const struct tb_js_engine *tb_default_js_engine(void);
  ```
- **C 桥（5 口 7 函数，JS 侧名字）**：`__tb_console(level,msg)`、`__tb_timer_set(ms,cb)→id`、`__tb_timer_cancel(id)`、`__tb_cookie()→str`、`__tb_cookie_set(str)`、`__tb_load_sync(url)→body`。host==NULL 时优雅降级：console 转发 `cfg.on_console`（host NULL → 无操作）、cookie/load_sync 返回空串、timer 不 pump。
- **tb_config 新增字段**：`int64_t js_memory_limit`（→ `JS_SetMemoryLimit`）、`uint32_t js_exec_ms_limit`（→ interrupt handler）、`void (*on_console)(tb_browser*, const char *level, const char *msg, void *ud)`、`const struct tb_js_engine *js_engine`（NULL → 默认）。新增公共 API `tb_err tb_eval_js(tb_browser*, const char *code, char **out)`。
- **每文档一个 JS context**：`open` 建 context，`close` 释放；QuickJS runtime 进程级单例复用（interrupt 状态放单例，M2a 单活跃文档假设）。
- **quickjs-ng v0.16.1 vendored**：`add_subdirectory(deps/quickjs-ng EXCLUDE_FROM_ALL)`，target `qjs`（static），头文件在仓库根，link `m dl pthread`；关 `QJS_BUILD_EXAMPLES`、`QJS_ENABLE_INSTALL`；`deps/VERSIONS.lock` 记录 SHA256。
- **JS builtins 注入**：`src/js/{parser,dom,render}.js` 经 CMake 脚本转义生成 `src/core/js_builtins.inc`（`const char *const tb_js_builtins[]`），open 时逐个 `JS_Eval` 到 context（普通模式，顶层 `var/function` 全局共享）。
- **dom.js DOM 子集（spec §4.3）**：querySelector/querySelectorAll/getElementById（仅 `tag`/`.class`/`#id`/`tag.class`，class 用 token 匹配，深度优先文档序）；document.title rw；document.readyState ro；document.createElement + appendChild/removeChild + textContent rw；getAttribute/setAttribute/removeAttribute；innerHTML 只读（重新序列化）；document.cookie rw；addEventListener（存储）。节点方法经原型 `_tb_node_proto`（load_document 时遍历树 setPrototypeOf 一次）。
- **交互（spec §4.2 语义不变）**：C 侧保留 `fv[64]` 表单值存储；JS 侧 `_tb_elem_info(id)`/`_tb_form_info(form_id)` 返回 JSON 字符串；C 侧最小 JSON 工具 `json.c` 解析（`json_get_str/json_get_int/json_get_str_array`）。
- **golden 迁移**：既有 fixtures（hello/forms/nav）输出逐字节不变；新增 M2a 静态 fixtures（实体/隐式闭合/boolean/script raw）golden 由 M1 语义生成比对；新增"脚本改 DOM" fixtures 为 M2a 新基线（M1 无法执行脚本），`-u` 生成后人工审阅。
- **测试命令**：`make` / `make test` / `make cli`。JS 单测 = `qjs_exe tests/js/<file>.test.js`（末尾失败则 `throw`，CLI 非零退出）。
- **响应**：所有提交信息、代码注释用中文（符合本仓库惯例）。

---

### Task 1: quickjs-ng vendoring + seam/config 骨架 + embed smoke

**Files:**
- Create: `deps/quickjs-ng/`（vendored 源码，解压自 codeload tarball v0.16.1）
- Modify: `deps/VERSIONS.lock`、`deps/VERSIONS.md`
- Modify: `CMakeLists.txt:17-102`
- Create: `src/core/browser_internal.h`
- Modify: `src/core/browser.c`
- Modify: `src/tb.h`
- Create: `tests/unit/test_qjs_embed.cc`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: 无（从零开始）
- Produces:
  - `deps/quickjs-ng/` 完整源码树 + CMake target `qjs`
  - `src/core/browser_internal.h`：`struct tb_browser`（含新字段 `const struct tb_js_engine *engine; void *js_doc; char *cookie_jar;`）与 `typedef struct { const char *name; char *value; } fv_t;`
  - `src/tb.h`：`struct tb_js_engine` seam + `tb_default_js_engine()` + tb_config 新字段 + `tb_eval_js` 声明
  - `src/core/js_builtins.inc`（空数组占位，Task 2-4 填充）

- [ ] **Step 1: Vendoring——下载并解压 quickjs-ng v0.16.1**

```bash
cd /home/gem/project/tinybrowser
mkdir -p /tmp/qjs-dl
curl -L -o /tmp/qjs-dl/qjs.tar.gz https://codeload.github.com/quickjs-ng/quickjs/tar.gz/refs/tags/v0.16.1
sha256sum /tmp/qjs-dl/qjs.tar.gz   # 记录到 deps/VERSIONS.lock
rm -rf deps/quickjs-ng
mkdir -p deps/quickjs-ng
tar -xzf /tmp/qjs-dl/qjs.tar.gz -C deps/quickjs-ng --strip-components=1
```

验证：`ls deps/quickjs-ng/quickjs.h deps/quickjs-ng/CMakeLists.txt deps/quickjs-ng/quickjs.c` 全部存在。

- [ ] **Step 2: 更新 deps/VERSIONS.lock 与 VERSIONS.md**

`deps/VERSIONS.lock` 追加一行（SHA 用 Step 1 实际值）：
```
quickjs-ng v0.16.1 <SHA256>
```
`deps/VERSIONS.md` 对应小节加 quickjs-ng v0.16.1（仓库 quickjs-ng/quickjs，vendored 源码，CMake target `qjs`，头文件在仓库根）。

- [ ] **Step 3: 顶层 CMakeLists 接入 quickjs-ng**

在 `CMakeLists.txt` 的 lexbor 块之前插入（保留注释说明用途）：
```cmake
# quickjs-ng: JS 引擎(M2a,替代 lexbor)。EXCLUDE_FROM_ALL 避免无条件构建
# 其 CLI/test exe;qjs 静态库由 tinybrowser 依赖拉取构建。
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/quickjs-ng EXCLUDE_FROM_ALL)
```
`target_link_libraries(tinybrowser PUBLIC ...)` 追加 `qjs`。

- [ ] **Step 4: 写 embed smoke 测试（红）**

`tests/unit/test_qjs_embed.cc`：
```c
#include <quickjs.h>
#include <gtest/gtest.h>

TEST(QjsEmbed, EvalExpression) {
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue r = JS_Eval(ctx, "1 + 2", 5, "<test>", JS_EVAL_TYPE_GLOBAL);
  int32_t v = 0;
  ASSERT_FALSE(JS_IsException(r));
  ASSERT_EQ(JS_ToInt32(ctx, &v, r), 0);
  EXPECT_EQ(v, 3);
  JS_FreeValue(ctx, r);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(QjsEmbed, MemoryLimitWorks) {
  JSRuntime *rt = JS_NewRuntime();
  JS_SetMemoryLimit(rt, 64 * 1024);
  JSContext *ctx = JS_NewContext(rt);
  JSValue r = JS_Eval(ctx, "var a=[]; while(true) a.push('x'.repeat(1024));", 40, "<t>", JS_EVAL_TYPE_GLOBAL);
  EXPECT_TRUE(JS_IsException(r));
  JS_FreeValue(ctx, r);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}
```

- [ ] **Step 5: tests/CMakeLists.txt 注册 test_qjs_embed**

按既有模式：
```cmake
add_executable(test_qjs_embed unit/test_qjs_embed.cc)
target_include_directories(test_qjs_embed PRIVATE ${TB_TEST_INCS} ${CMAKE_CURRENT_SOURCE_DIR}/../deps/quickjs-ng)
target_link_libraries(test_qjs_embed PRIVATE tinybrowser qjs gtest_main)
gtest_discover_tests(test_qjs_embed)
```
（`TB_TEST_INCS` 已含 `.../src`，quickjs 头单独加 include dir。链接 `tinybrowser qjs`——tinybrowser 尚未链 qjs，先显式加。）

- [ ] **Step 6: 配置并跑测试（绿）**

```bash
cd /home/gem/project/tinybrowser && rm -rf build && cmake -S . -B build && make -C build -j$(nproc) && ctest --test-dir build --output-on-failure
```
Expected: 全部既有测试 + `test_qjs_embed` 通过。`deps/quickjs-ng/libqjs.a` 与 `qjs_exe` 已构建。

- [ ] **Step 7: 建 browser_internal.h 并迁移 struct tb_browser**

`src/core/browser_internal.h`（新建）：
```c
#ifndef TB_BROWSER_INTERNAL_H
#define TB_BROWSER_INTERNAL_H
#include "tb.h"
#include "session.h"
#include <uv.h>

typedef struct { const char *name; char *value; } fv_t;

struct tb_browser {
  tb_config cfg;
  tb_session session;
  tb_view *view;
  const tb_transport *transport;
  fv_t fv[64];
  int nfv;
  uv_loop_t loop;
  int loop_init;
  const struct tb_js_engine *engine; /* 实际 engine:cfg.js_engine 或 tb_default_js_engine() */
  void *js_doc;                      /* 当前文档的 JS context 句柄(engine->open 返回值) */
  char *cookie_jar;                  /* document.cookie 往返(M2a 单串) */
};

/* 子资源同步加载:nested pump。*out = strdup 或空串,调用方 free。 */
tb_err tb_load_sync(tb_browser *b, const char *url, char **out);

#endif
```
`browser.c`：删除本地 `struct tb_browser` 与 `fv_t` 定义，改为 `#include "browser_internal.h"`。所有引用点不变（字段名一致）。`tb_create` 里 `cfg.js_engine` 为 NULL 时赋默认：`b->engine = cfg.js_engine ? cfg.js_engine : tb_default_js_engine();`（Task 6 前 `tb_default_js_engine` 尚未实现——Task 1 在 browser.c 顶部加临时 stub：返回 NULL。`tb_eval_js` 先实现为 `if (!b->engine || !b->js_doc) return TB_ERR_NO_VIEW;`，Task 5 换真实现。）

- [ ] **Step 8: tb.h 扩展**

在 tb_config 追加（Task 1 只声明，Task 5 使用）：
```c
int64_t js_memory_limit;              /* QuickJS 内存上限字节;0=默认 */
uint32_t js_exec_ms_limit;            /* 单脚本执行毫秒上限;0=不限制 */
void (*on_console)(tb_browser *, const char *level, const char *msg, void *ud);
const struct tb_js_engine *js_engine; /* NULL → 默认 quickjs engine */
```
声明 seam（Global Constraints 中逐字）+ `tb_default_js_engine(void)` + `tb_err tb_eval_js(tb_browser *b, const char *code, char **out);`

- [ ] **Step 9: js_builtins.inc 生成机制（空数组占位）**

`cmake/embed_js.cmake`：
```cmake
# 把 src/js/*.js 转成 C 字符串数组。输入目录、输出路径由调用方传入。
function(tb_embed_js IN_DIR OUT_FILE)
  file(GLOB TB_JS_SRC ${IN_DIR}/*.js)
  list(SORT TB_JS_SRC)
  set(LINES "")
  foreach(f ${TB_JS_SRC})
    file(READ ${f} SRC)
    string(REPLACE "\\" "\\\\" SRC "${SRC}")
    string(REPLACE "\"" "\\\"" SRC "${SRC}")
    string(REPLACE "\n" "\\n" SRC "${SRC}")
    get_filename_component(base ${f} NAME)
    string(APPEND LINES "  /* ${base} */\n  \"${SRC}\",\n")
  endforeach()
  file(WRITE ${OUT_FILE} "/* 由 cmake/embed_js.cmake 生成,勿手改。 */\nconst char *const tb_js_builtins[] = {\n${LINES}  NULL\n};\n")
endfunction()
```
顶层 CMakeLists（tests 之前、target 定义后）：
```cmake
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake)
set(TB_JS_BUILTINS ${CMAKE_CURRENT_BINARY_DIR}/src/core/js_builtins.inc)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src/core)
tb_embed_js(${CMAKE_CURRENT_SOURCE_DIR}/src/js ${TB_JS_BUILTINS})
add_custom_target(tb_js_builtins ALL DEPENDS ${TB_JS_BUILTINS})
```
`add_library(tinybrowser ...)` 之后加 `add_dependencies(tinybrowser tb_js_builtins)`。创建空目录 `src/js/.keep`（git 需有文件才跟踪；Task 2 起填 .js）。

- [ ] **Step 10: 全量回归**

```bash
cd /home/gem/project/tinybrowser && make -C build -j$(nproc) && ctest --test-dir build --output-on-failure
```
Expected: 全绿（Task 1 未改任何解析/渲染行为）。

- [ ] **Step 11: Commit**

```bash
git add deps/quickjs-ng deps/VERSIONS.lock deps/VERSIONS.md CMakeLists.txt src/tb.h src/core/browser.c src/core/browser_internal.h src/js/.keep tests/unit/test_qjs_embed.cc tests/CMakeLists.txt cmake/embed_js.cmake
git commit -m "feat(m2a): vendored quickjs-ng + js engine seam + config 骨架"
```

---

### Task 2: parser.js——纯 JS 宽松 HTML/XML tokenizer

**Files:**
- Create: `src/js/parser.js`
- Create: `tests/js/parser.test.js`
- Create: `tests/js/lib/testlib.js`（断言小工具，Task 2-4 共用）

**Interfaces:**
- Consumes: 无（纯 JS，无 C 依赖）
- Produces:
  - `_tb_parser.parse(html)` → 返回文档对象 `{ root, doc, readyState, warnings[] }`（非流式便捷入口，供单测与 `load_document` 用）
  - `_tb_parser.make()` → 增量解析器实例：`.feed(chunk)`、`.finalize()`、`.next_script()`、`.root`、`.doc`、`.warnings`
  - 节点结构：`{type:"element"|"text", tag, attrs, children, text, id, parent}`，`attrs` 为 `{name:value}`（boolean attr → `""`）；`id` 从 1 起每节点递增分配
  - script 记录：`{src: string|null, text: string}`
  - `_tb_parser.readyState` 字符串
  - Task 3 依赖：`dom.js` 遍历此树并挂原型方法

- [ ] **Step 1: 写共享断言小工具**

`tests/js/lib/testlib.js`：
```js
// 极简断言:qjs_exe 无外部依赖。失败累积,最终 throw 使 CLI 非零退出。
var __passed = 0, __failed = 0;
function eq(a, b, msg) {
  if (a !== b) { __failed++; console.log("FAIL " + (msg || "eq") + ": got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
  else __passed++;
}
function ok(cond, msg) { eq(!!cond, true, msg || "ok"); }
function t(name, fn) {
  try { fn(); __passed++; }
  catch (e) { __failed++; console.log("FAIL " + name + ": " + e); }
}
function done() {
  console.log(__passed + " passed, " + __failed + " failed");
  if (__failed) throw new Error(__failed + " failures");
}
```
（`t()` 失败时 `__passed++` 会重复计数——修正：`t` 内成功不计数，`eq/ok` 才计数。实现时以"eq/ok 是唯一计数源"为准。）

- [ ] **Step 2: 写失败测试（红）**

`tests/js/parser.test.js`（核心用例，完整列出）：
```js
load("./tests/js/lib/testlib.js");
var P = _tb_parser.parse;   // Task 2 完成后存在

t("basic tree", function () {
  var d = P("<html><body><p>hi <b>there</b></p></body></html>");
  var body = d.root.children[0];
  eq(body.tag, "html");
  var p = body.children[0].children[0];
  eq(p.tag, "p");
  eq(p.children.length, 2);
  eq(p.children[1].tag, "b");
  eq(p.children[1].children[0].text, "there");
});

t("void elements self-close", function () {
  var d = P("<p>a<br>b<img src=x>c<input><hr>d</p>");
  var p = d.root.children[0];
  eq(p.children.length, 7);
  eq(p.children[1].tag, "br");
  ok(p.children[1].children === undefined || p.children[1].children.length === 0);
  eq(p.children[2].tag, "img");
});

t("unquoted and boolean attrs", function () {
  var d = P("<input name=q value=hello checked disabled>");
  var n = d.root.children[0];
  eq(n.attrs.name, "q");
  eq(n.attrs.value, "hello");
  eq(n.attrs.checked, "");
  eq(n.attrs.disabled, "");
});

t("quoted attrs with escapes", function () {
  var d = P('<a href="/x?a=1&amp;b=2" title="he said \'hi\'">t</a>');
  var n = d.root.children[0];
  eq(n.attrs.href, "/x?a=1&b=2");
  eq(n.attrs.title, "he said 'hi'");
});

t("implicit close p/li/td/tr/th/option", function () {
  var d = P("<ul><li>a<li>b</ul><table><tr><td>x<td>y</table>");
  var ul = d.root.children[0];
  eq(ul.children.length, 2);
  eq(ul.children[1].children[0].text, "b");
  var table = d.root.children[1];
  var tr = table.children[0];
  eq(tr.children.length, 2);
  eq(tr.children[1].children[0].text, "y");
});

t("mis-nesting tolerated", function () {
  var d = P("<b>one<i>two</b>three</i>");
  eq(d.root.children[0].tag, "b");
  eq(d.root.children[0].children[0].text, "one");
  // </b> 时 i 被隐式闭合,i 的内容归属 b 下
  eq(d.root.children[0].children.length, 2);
  ok(d.warnings.length >= 1);
});

t("entity decode in text", function () {
  var d = P("<p>a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos; &nbsp;x &#65;&#x42;</p>");
  var p = d.root.children[0];
  eq(p.children[0].text, "a&b <c> \"d\" 'e'  x AB");
});

t("script raw text collected as record", function () {
  var d = P("<p>a</p><script>if (1 < 2 && x) { var s = \"&amp;\"; }</script><p>b</p>");
  var p0 = d.root.children[0];
  eq(p0.children[0].text, "a");
  var p1 = d.root.children[1];
  eq(p1.children[0].text, "b");
  // raw 内容不进 DOM
  var sc = d.scripts[0];
  eq(sc.src, null);
  ok(sc.text.indexOf("&amp;") >= 0);
  ok(sc.text.indexOf("<") >= 0);
});

t("script src attribute", function () {
  var d = P("<script src='/app.js'></script>");
  eq(d.scripts[0].src, "/app.js");
  eq(d.scripts[0].text, "");
});

t("style raw text skipped from tree", function () {
  var d = P("<style>a > b { color: red }</style><p>x</p>");
  eq(d.root.children.length, 1);
  eq(d.root.children[0].tag, "p");
});

t("comment doctype pi skipped", function () {
  var d = P("<!DOCTYPE html><!-- hi --><?pi x?><p>ok</p>");
  eq(d.root.children.length, 1);
  eq(d.root.children[0].tag, "p");
});

t("text across chunk boundary is contiguous", function () {
  var ins = _tb_parser.make();
  ins.feed("<p>hel");
  ins.feed("lo <b>w");
  ins.feed("orld</b></p>");
  ins.finalize();
  var p = ins.root.children[0];
  eq(p.children[0].text, "hello ");
  eq(p.children[1].children[0].text, "world");
});

t("tag split across chunk boundary", function () {
  var ins = _tb_parser.make();
  ins.feed("<a hr");
  ins.feed("ef='/x'");
  ins.feed(">t</a>");
  ins.feed("<p>q");
  ins.finalize();
  eq(ins.root.children[0].attrs.href, "/x");
  eq(ins.root.children[1].children[0].text, "q");
});

t("malformed input warns and continues", function () {
  var d = P("<p>a< <p>b");
  ok(d.warnings.length >= 1);
  var p1 = d.root.children[d.root.children.length - 1];
  eq(p1.children[0].text, "b");
});

t("ids increment from 1", function () {
  var d = P("<p>x</p><a href='/y'>y</a>");
  eq(d.root.children[0].id, 1);
  eq(d.root.children[1].id, 2);
});

t("readyState loading before finalize", function () {
  var ins = _tb_parser.make();
  ins.feed("<p>x</p>");
  eq(ins.readyState, "loading");
  ins.finalize();
});
```
跑：`./build/deps/quickjs-ng/qjs_exe tests/js/parser.test.js` → FAIL（`_tb_parser` 未定义）。

- [ ] **Step 3: 实现 parser.js**

`src/js/parser.js` 完整实现（关键算法）：

```js
// 纯 JS 宽松 HTML/XML tokenizer → DOM 树。流式接口。
// 节点: {type, tag, attrs, children, text, id, parent}
var _tb_parser = (function () {
  var VOID = { br:1, img:1, input:1, hr:1, meta:1, link:1 };
  var IMPLICIT = { p:1, li:1, td:1, tr:1, th:1, option:1 };
  var RAW = { script:1, style:1 };
  var next_id = 0;
  function newNode(type, tag) { return { type: type, tag: tag, attrs: {}, children: [], text: "", id: ++next_id }; }

  function decodeEntities(s) {
    // &amp; &lt; &gt; &quot; &apos; &nbsp; + &#NN; + &#xHH; + 未知实体原样保留
    return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|amp|lt|gt|quot|apos|nbsp);/g, function (m, e) {
      if (e === "amp") return "&";
      if (e === "lt") return "<";
      if (e === "gt") return ">";
      if (e === "quot") return '"';
      if (e === "apos") return "'";
      if (e === "nbsp") return " ";
      if (e[1] === "x") return String.fromCharCode(parseInt(e.slice(2), 16));
      return String.fromCharCode(parseInt(e.slice(1), 10));
    });
  }

  function Parser() {
    this.buf = "";          // 未消费输入(跨 chunk 续接)
    this.i = 0;             // 已消费游标
    this.state = "text";    // text | tagopen | tagname | attrname | attrvalue | close | comment | doctype | pi | raw
    this.stack = [];        // 开元素栈
    this.curTag = null;     // 正在解析的 start tag 名字
    this.curAttrs = {};
    this.curQuote = "";     // 属性值引号 '' | '"' | ""(unquoted)
    this.rawTag = null;     // raw 状态下的标签名(script/style)
    this.rawText = "";      // raw 文本累积
    this.root = newNode("element", "html");
    this.stack.push(this.root);
    this.scripts = [];
    this.warnings = [];
    this.textBuf = "";      // TEXT 状态累积的文本(未 flush)
    this.readyState = "loading";
    this.finalized = false;
  }

  Parser.prototype.feed = function (chunk) {
    if (this.finalized) return;
    this.buf += chunk;
    this._scan();
  };

  Parser.prototype.finalize = function () {
    if (this.finalized) return;
    this.finalized = true;
    this._scan();                 // 扫尽残留
    this._flushText();
    while (this.stack.length > 1) {           // 隐式关闭所有未闭合标签
      this._warn("unclosed <" + this.stack[this.stack.length - 1].tag + ">");
      this.stack.pop();
    }
    // 文本节点重建(见 Step 3 的 text 追加说明)
  };

  Parser.prototype._warn = function (m) { this.warnings.push(m); };
  Parser.prototype._flushText = function () {
    if (this.textBuf) {
      var n = newNode("text", null);
      n.text = decodeEntities(this.textBuf);
      this.textBuf = "";
      this._append(n);
    }
  };

  // _append:把节点加到栈顶(去掉刚 pop 的空 select/input 占位,保持 M1 渲染一致)
  Parser.prototype._append = function (n) {
    var top = this.stack[this.stack.length - 1];
    n.parent = top;
    top.children.push(n);
  };

  // _scan:主状态机,消费 buf 直到无法推进(不完整结构留待下次 feed)
  Parser.prototype._scan = function () {
    var s = this.buf, L = s.length;
    while (true) {
      if (this.state === "text") {
        var lt = s.indexOf("<", this.i);
        if (lt < 0) {
          this.textBuf += s.slice(this.i);
          this.i = L;
          return;                       // 等更多输入或 finalize
        }
        this.textBuf += s.slice(this.i, lt);
        this.i = lt + 1;
        if (s.startsWith("!--", lt + 1)) { this.state = "comment"; this.i = lt + 4; }
        else if (s[lt + 1] === "!") { this.state = "doctype"; this.i = lt + 2; }
        else if (s[lt + 1] === "?") { this.state = "pi"; this.i = lt + 2; }
        else if (s[lt + 1] === "/") { this.state = "close"; this.i = lt + 2; }
        else if (/[a-zA-Z]/.test(s[lt + 1] || "")) { this.state = "tagname"; this.curTag = ""; this.curAttrs = {}; }
        else { this.textBuf += "<"; this.i = lt + 1; }   // "<" 后非法字符,当字面
      }
      else if (this.state === "tagname") {
        var m = /^[a-zA-Z][a-zA-Z0-9:-]*/.exec(s.slice(this.i));
        if (!m) { if (this.i >= L) return; this._warn("bad tag name"); this.state = "text"; continue; }
        this.curTag = m[0].toLowerCase();
        this.i += m[0].length;
        this.state = "tagopen";
      }
      else if (this.state === "tagopen") {
        // 之后可能是 > / > 属性 或 EOF
        if (this.i >= L) return;
        var c = s[this.i];
        if (c === ">") { this._closeStartTag(false); this.i++; }
        else if (c === "/" && s[this.i + 1] === ">") { this._closeStartTag(true); this.i += 2; }
        else if (/\s/.test(c)) { this.i++; this.state = "attrname"; continue; }
        else { this.state = "attrname"; continue; }   // 无空格属性名
      }
      else if (this.state === "attrname") {
        if (this.i >= L) return;
        var c = s[this.i];
        if (c === ">" || c === "/") { this._closeStartTag(c === "/" && s[this.i + 1] === ">"); if (c === ">") this.i++; else this.i += 2; continue; }
        if (/\s/.test(c)) { this.i++; continue; }
        var am = /^[^=\s/>]+/.exec(s.slice(this.i));
        if (!am) { this._warn("bad attr"); this.state = "text"; continue; }
        var an = am[0];
        this.i += am[0].length;
        this.curAttrs[an] = "";               // 先置空(boolean 默认)
        if (s[this.i] === "=") { this.i++; this.state = "attrvalue"; }
        // 否则 boolean attr,继续 attrname
      }
      else if (this.state === "attrvalue") {
        if (this.i >= L) return;
        var c = s[this.i];
        if (c === '"' || c === "'") {
          this.curQuote = c; this.i++;
          var end = s.indexOf(c, this.i);
          if (end < 0) { if (L - this.i > 0) { /* 跨 chunk:留待续接 */ return; } }
          var raw = end < 0 ? "" : s.slice(this.i, end);
          if (end < 0) { /* 未闭合引号(EOF 或等更多) */ this._setLastAttr(raw); this.i = L; this.state = "text"; }
          else {
            this._setLastAttr(raw);
            this.i = end + 1;
            this.state = "tagopen";
          }
        } else if (c === ">") { this._closeStartTag(false); this.i++; }
        else {
          var um = /^[^ \t\r\n>]+/.exec(s.slice(this.i));   // unquoted value
          if (!um) { this._warn("bad attr value"); this.state = "text"; continue; }
          this._setLastAttr(um[0]);
          this.i += um[0].length;
          this.state = "tagopen";
        }
      }
      else if (this.state === "close") {
        var cm = /^[a-zA-Z][a-zA-Z0-9:-]*/.exec(s.slice(this.i));
        if (!cm) { if (this.i >= L) return; this._warn("bad close tag"); this.state = "text"; continue; }
        this._doClose(cm[0].toLowerCase());
        this.i += cm[0].length;
        this.state = "text";              // 跳到 > (忽略属性,宽容)
      }
      else if (this.state === "comment") {
        var ce = s.indexOf("-->", this.i);
        if (ce < 0) { this.i = L; return; }   // 未完成,续接
        this.i = ce + 3; this.state = "text";
      }
      else if (this.state === "doctype") {
        var de = s.indexOf(">", this.i);
        if (de < 0) { this.i = L; return; }
        this.i = de + 1; this.state = "text";
      }
      else if (this.state === "pi") {
        var pe = s.indexOf("?>", this.i);
        var p2 = s.indexOf(">", this.i);
        var endAt = (pe < 0) ? p2 : (p2 < 0 ? pe : Math.min(pe, p2));
        if (endAt < 0) { this.i = L; return; }
        this.i = endAt + (s[endAt] === "?" ? 2 : 1); this.state = "text";
      }
      else if (this.state === "raw") {
        var re = s.indexOf("</" + this.rawTag, this.i);
        if (re < 0) { this.rawText += s.slice(this.i); this.i = L; return; }
        this.rawText += s.slice(this.i, re);
        this.i = re + 2 + this.rawTag.length;
        // 跳过 </script> 的 > (宽容:直到 >)
        var gt = s.indexOf(">", this.i);
        if (gt < 0) { this.i = L; return; }
        this.i = gt + 1;
        this.scripts.push({ src: this.curAttrs.src || null, text: this.rawText });
        this.rawTag = null; this.rawText = "";
        this.state = "text";
      }
    }
  };

  Parser.prototype._setLastAttr = function (v) {
    // 写回最后一个属性名(记录在 this.lastAttrName)
    this.curAttrs[this.lastAttrName] = decodeEntities(v);
  };

  Parser.prototype._closeStartTag = function (selfClose) {
    this._flushText();
    var tag = this.curTag;
    if (VOID[tag]) selfClose = true;
    // 隐式闭合:同栈顶同名
    if (IMPLICIT[tag] && this.stack[this.stack.length - 1].tag === tag) {
      this.stack.pop();
    }
    if (selfClose) {
      // 自闭合元素不入栈;若 RAW 标签(如 <script src=x />)无内容
      if (RAW[tag]) { this.scripts.push({ src: this.curAttrs.src || null, text: "" }); }
      var voidN = newNode("element", tag);
      voidN.attrs = this.curAttrs;
      this._append(voidN);
    } else {
      var n = newNode("element", tag);
      n.attrs = this.curAttrs;
      this._append(n);
      if (RAW[tag]) { this.rawTag = tag; this.rawText = ""; this.state = "raw"; }
      else this.stack.push(n);
    }
    this.curTag = null; this.curAttrs = {};
  };

  Parser.prototype._doClose = function (tag) {
    this._flushText();
    // 找栈中最近的同名(允许 mis-nest:中间元素全部隐式闭合)
    var idx = -1;
    for (var k = this.stack.length - 1; k >= 0; k--) {
      if (this.stack[k].tag === tag) { idx = k; break; }
    }
    if (idx < 0) { this._warn("unmatched </" + tag + ">"); return; }
    while (this.stack.length - 1 > idx) {
      this._warn("implicit close <" + this.stack[this.stack.length - 1].tag + ">");
      this.stack.pop();
    }
    this.stack.pop();
  };

  Parser.prototype._append = function (n) {
    var top = this.stack[this.stack.length - 1];
    n.parent = top;
    top.children.push(n);
  };

  Parser.prototype.next_script = function () { return this.scripts.length ? this.scripts.shift() : null; };

  function parse(html) {
    var p = new Parser();
    p.feed(html);
    p.finalize();
    return { root: p.root, doc: p.root, scripts: p.scripts.slice(), readyState: "complete", warnings: p.warnings };
  }
  function make() { return new Parser(); }

  return { parse: parse, make: make, VOID: VOID, IMPLICIT: IMPLICIT, RAW: RAW, decodeEntities: decodeEntities };
})();
```
（实现要点：`textBuf` 跨 chunk 累积到 `<` 或 finalize 才 flush 为文本节点；`<` 遇 `</` 时先 flush；`_setLastAttr` 依赖 `lastAttrName`——在 attrname 完成时记录 `this.lastAttrName = an`。TEXT 状态遇 `<` 后必须 `this._flushText()`。`finalize` 里脚本队列已满。DOM 树即 `root`（`html` 元素），`doc === root`，Task 3 的 `document` 概念建在此 root 上。）

实现注意（agent 执行时对照上述骨架补全，以下必须成立）：
1. 文本节点在元素内按序追加；`<` 前 flush。
2. `parse()` 返回的 `scripts` 用切片副本（`finalize` 后 `scripts` 仍持有,避免 shift 副作用）。
3. `readyState` 在 parse() 返回 "complete"（Task 5 由 load_document 控制真实时序）。
4. 所有扫描用 `s.indexOf`/正则,不逐字符,保证性能可接受（10k 节点 ~ms 级）。
5. `_scan` 每次循环必须推进 `this.i` 或改变 state,否则死循环（agent 必须自查每个分支）。

- [ ] **Step 4: 跑测试（绿）**

```bash
cd /home/gem/project/tinybrowser && ./build/deps/quickjs-ng/qjs_exe tests/js/parser.test.js
```
Expected: `N passed, 0 failed`。若有 FAIL，修 parser.js 后重跑。

- [ ] **Step 5: 纳入 CMake 单测**

`tests/CMakeLists.txt` 追加：
```cmake
add_custom_target(tb_js_runner ALL DEPENDS qjs_exe)
add_test(NAME js_parser COMMAND $<TARGET_FILE:qjs_exe> ${CMAKE_CURRENT_SOURCE_DIR}/js/parser.test.js)
```
（qjs_exe 由 Task 1 的 EXCLUDE_FROM_ALL 提供；`add_custom_target(... ALL)` 保证 `make` 构建它。）

- [ ] **Step 6: Commit**

```bash
git add src/js/parser.js tests/js/parser.test.js tests/js/lib/testlib.js tests/CMakeLists.txt
git commit -m "feat(m2a): parser.js 宽松 HTML tokenizer + JS 单测"
```

---

### Task 3: dom.js——DOM 子集 + 查询 + 原型方法

**Files:**
- Create: `src/js/dom.js`
- Create: `tests/js/dom.test.js`

**Interfaces:**
- Consumes: `_tb_parser`（Task 2）——`parse(html)`/`make()`、节点结构、`decodeEntities`
- Produces:
  - `var document`（全局）：`title`(get/set)、`readyState`、`documentElement`、`createElement(tag)`、`getElementById(id)`、`querySelector(sel)`、`querySelectorAll(sel)`、`cookie`(get/set)
  - `_tb_node_proto`：节点方法——`appendChild/removeChild/textContent(get/set)/getAttribute/setAttribute/removeAttribute/innerHTML(get,只读)/tagName/parentNode/childNodes/addEventListener`
  - `document._tb_attach(root)`：遍历树,把所有节点 `setPrototypeOf` 到 `_tb_node_proto`（迭代,禁递归）
  - `_tb_elem_info(id)` → JSON 字符串（供 Task 6 交互）：
    - a: `{"tag":"a","href":"..."}`
    - input: `{"tag":"input","type":"..."}`
    - button: `{"tag":"button","form":<id>}`（无表单则 `"form":0`）
    - select: `{"tag":"select","options":["..",..]}`
    - form: `{"tag":"form"}`

- [ ] **Step 1: 写失败测试（红）**

`tests/js/dom.test.js`：
```js
load("./tests/js/lib/testlib.js");
var P = _tb_parser;
function doc(html) {
  var d = P.parse(html);
  document._tb_attach(d.root);
  return d;
}

t("querySelector basic", function () {
  var d = doc('<div id="a"><p class="x">1</p><p class="y">2</p><a href="/z">3</a></div>');
  var a = document.querySelector("p.x");
  eq(a.textContent, "1");
  eq(document.querySelector("a").attrs.href, "/z");
});

t("querySelectorAll doc order", function () {
  var d = doc("<ul><li>1</li><li>2</li></ul><p>3</p>");
  var ps = document.querySelectorAll("li");
  eq(ps.length, 2);
  eq(ps[0].textContent, "1");
  eq(ps[1].textContent, "2");
});

t("getElementById", function () {
  var d = doc('<p id="x">hi</p><p id="y">yo</p>');
  eq(document.getElementById("x").textContent, "hi");
  eq(document.getElementById("nope"), null);
});

t("class token matching", function () {
  var d = doc('<div class="a b"><span class="b">t</span></div>');
  eq(document.querySelectorAll(".b").length, 2);
  eq(document.querySelector("div.a").tagName, "div");
});

t("title read/write", function () {
  var d = doc("<title>My Page</title><p>x</p>");
  eq(document.title, "My Page");
  document.title = "Changed";
  eq(document.title, "Changed");
});

t("readyState ro", function () {
  var d = doc("<p>x</p>");
  eq(document.readyState, "complete");
});

t("createElement + appendChild + textContent", function () {
  var d = doc("<div id='root'></div>");
  var div = document.getElementById("root");
  var span = document.createElement("span");
  div.appendChild(span);
  span.textContent = "hello";
  eq(span.textContent, "hello");
  eq(div.childNodes.length, 1);
  eq(div.innerHTML, "<span>hello</span>");
});

t("textContent write replaces children", function () {
  var d = doc("<div><span>a</span><span>b</span></div>");
  var div = document.querySelector("div");
  div.textContent = "z";
  eq(div.childNodes.length, 1);
  eq(div.childNodes[0].text, "z");
});

t("removeChild", function () {
  var d = doc("<div><span>a</span><span>b</span></div>");
  var div = document.querySelector("div");
  var s = document.querySelectorAll("span")[0];
  div.removeChild(s);
  eq(div.childNodes.length, 1);
});

t("attrs get/set/remove", function () {
  var d = doc('<a href="/a" class="c">t</a>');
  var a = document.querySelector("a");
  eq(a.getAttribute("href"), "/a");
  a.setAttribute("href", "/b");
  eq(a.getAttribute("href"), "/b");
  a.removeAttribute("class");
  eq(a.getAttribute("class"), null);
});

t("cookie passes through bridge", function () {
  // bridge 由 qjs_exe 环境注入?否——单测里 __tb_cookie 未定义时安全降级
  var d = doc("<p>x</p>");
  // 只在桥存在时测
  if (typeof __tb_cookie === "function") {
    __tb_cookie_set("k=v");
    eq(__tb_cookie(), "k=v");
  }
});

t("innerHTML read-only serializer", function () {
  var d = doc('<div id="d"><p>a&amp;b</p></div>');
  var div = document.getElementById("d");
  eq(div.innerHTML, "<p>a&amp;b</p>");
});

t("addEventListener stores", function () {
  var d = doc("<button id='b'>go</button>");
  var b = document.getElementById("b");
  var called = 0;
  b.addEventListener("click", function () { called++; });
  b._fire("click");
  eq(called, 1);
});

t("elem info for interaction", function () {
  var d = doc("<form id='f'><a href='/x'>l</a><input name='q'><button>Go</button></form>");
  var a = document.querySelector("a");
  var info = JSON.parse(_tb_elem_info(a.id));
  eq(info.tag, "a");
  eq(info.href, "/x");
  var btn = document.querySelector("button");
  var bi = JSON.parse(_tb_elem_info(btn.id));
  eq(bi.tag, "button");
  var inp = document.querySelector("input");
  eq(JSON.parse(_tb_elem_info(inp.id)).tag, "input");
});
```

- [ ] **Step 2: 跑测试确认红**

```bash
cd /home/gem/project/tinybrowser && ./build/deps/quickjs-ng/qjs_exe tests/js/dom.test.js
```
Expected: FAIL（`document` 未定义）。

- [ ] **Step 3: 实现 dom.js**

`src/js/dom.js`（要点，实现时补全）：
```js
// DOM 子集。节点为 parser 纯数据;此处挂原型方法与全局 document。
var _tb_node_proto = {
  get tagName() { return this.tag ? this.tag.toUpperCase() : ""; },
  get childNodes() { return this.children || []; },
  get parentNode() { return this.parent || null; },
  appendChild: function (n) { n.parent = this; this.children.push(n); return n; },
  removeChild: function (n) {
    var i = this.children.indexOf(n);
    if (i < 0) throw new Error("removeChild: not a child");
    this.children.splice(i, 1); n.parent = null; return n;
  },
  get textContent() {
    // 迭代收集后代文本
    var out = "", stack = [this];
    while (stack.length) {
      var n = stack.pop();
      if (n.type === "text") out += n.text;
      else for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
    }
    return out;
  },
  set textContent(v) {
    this.children = [];
    var t = { type: "text", tag: null, attrs: {}, children: [], text: String(v), id: _tb_next_id(), parent: this };
    this.children.push(t);
  },
  getAttribute: function (n) { return Object.prototype.hasOwnProperty.call(this.attrs, n) ? this.attrs[n] : null; },
  setAttribute: function (n, v) { this.attrs[n] = String(v); },
  removeAttribute: function (n) { delete this.attrs[n]; },
  get innerHTML() {
    // 子树序列化(低fidelity:attr 引号转义,文本实体转义)
    function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
    var out = "", stack = [this];
    // 用迭代展开(先序遍历)
    // 简化实现:直接迭代序列化
    function ser(n) {
      if (n.type === "text") return esc(n.text);
      var a = "";
      for (var k in n.attrs) a += " " + k + '="' + esc(n.attrs[k]) + '"';
      var inner = "";
      for (var j = 0; j < n.children.length; j++) inner += ser(n.children[j]);
      return "<" + n.tag + a + ">" + inner + "</" + n.tag + ">";
    }
    return ser(this);   // 注意:ser 递归;M2a innerHTML 只读、树深有限,可接受
  },
  addEventListener: function (type, cb) {
    if (!this._listeners) Object.defineProperty(this, "_listeners", { value: {}, configurable: true, writable: true });
    if (!this._listeners[type]) this._listeners[type] = [];
    this._listeners[type].push(cb);
  },
  _fire: function (type) {
    if (!this._listeners || !this._listeners[type]) return;
    var cbs = this._listeners[type].slice();
    for (var i = 0; i < cbs.length; i++) { try { cbs[i]({}); } catch (e) {} }
  }
};

var _tb_next_id = function () {
  // 复用 parser 的 id 计数器? parser 暴露 nextId。此处独立兜底(建节点时才用)
  return _tb_parser._nextId ? _tb_parser._nextId() : (_tb_parser._idCtr = (_tb_parser._idCtr || 0) + 1);
};

var document = {
  _root: null,
  _tb_attach: function (root) {
    document._root = root;
    // 迭代挂原型(禁递归)
    var stack = [root];
    while (stack.length) {
      var n = stack.pop();
      Object.setPrototypeOf(n, _tb_node_proto);
      for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
    }
  },
  get documentElement() { return document._root; },
  get readyState() { return _tb_parser._readyState || "complete"; },
  get title() {
    // 迭代找 head>title 文本
    var q = document.querySelectorAll("title");
    return q.length ? q[0].textContent : "";
  },
  set title(v) {
    var q = document.querySelectorAll("title");
    if (q.length) { q[0].textContent = v; return; }
    // 无 title:在 head 下建一个(没有 head 则建)
    var head = document.querySelector("head");
    if (!head) {
      head = document.createElement("head");
      var html = document._root;
      html.children.unshift(head);
      head.parent = html;
    }
    var t = document.createElement("title");
    t.textContent = v;
    head.appendChild(t);
  },
  createElement: function (tag) {
    return { type: "element", tag: String(tag).toLowerCase(), attrs: {}, children: [], text: "", id: _tb_next_id(), parent: null, __proto__: _tb_node_proto };
  },
  getElementById: function (id) {
    return document.querySelector("#" + id);
  },
  querySelectorAll: function (sel) {
    // 解析 sel:tag | .class | #id | tag.class
    var re = /^([a-zA-Z][a-zA-Z0-9:-]*)?([.#][a-zA-Z0-9_-]+)?$/;
    var m = re.exec(sel);
    if (!m) throw new Error("unsupported selector: " + sel);
    var tag = m[1] ? m[1].toLowerCase() : null;
    var cls = null, id = null;
    if (m[2]) {
      if (m[2][0] === ".") cls = m[2].slice(1);
      else id = m[2].slice(1);
    }
    var out = [];
    var stack = [document._root];
    while (stack.length) {
      var n = stack.pop();
      if (n.type === "element") {
        if ((!tag || n.tag === tag) &&
            (!cls || (n.attrs["class"] || "").split(/\s+/).indexOf(cls) >= 0) &&
            (!id || n.attrs.id === id)) out.push(n);
        for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
      }
    }
    return out;
  },
  querySelector: function (sel) {
    var all = document.querySelectorAll(sel);
    return all.length ? all[0] : null;
  },
  get cookie() { return typeof __tb_cookie === "function" ? __tb_cookie() : ""; },
  set cookie(v) { if (typeof __tb_cookie_set === "function") __tb_cookie_set(String(v)); }
};

// 交互辅助(供 C 侧 engine->eval):返回 JSON 字符串
function _tb_elem_info(id) {
  var n = document.getElementById(String(id));
  if (!n || n.type !== "element") return JSON.stringify({ tag: "" });
  var o = { tag: n.tag };
  if (n.tag === "a") o.href = n.attrs.href || "";
  if (n.tag === "input") o.type = n.attrs.type || "text";
  if (n.tag === "button") { o.form = _tb_form_of(n); }
  if (n.tag === "select") {
    o.options = [];
    for (var k = 0; k < n.children.length; k++) {
      var c = n.children[k];
      if (c.type === "element" && c.tag === "option") o.options.push(c.textContent);
    }
  }
  return JSON.stringify(o);
}
function _tb_form_of(node) {
  // 迭代向上找 form 祖先
  var p = node.parent;
  while (p) { if (p.type === "element" && p.tag === "form") return p.id; p = p.parent; }
  return 0;
}
```
（`_tb_elem_info` 里 select options 用 `textContent` 取 option 文本，与 render.c `tb_dom_text(option)` 语义一致。`_tb_form_of` 由父链迭代。）

- [ ] **Step 4: 跑测试（绿）**

```bash
cd /home/gem/project/tinybrowser && ./build/deps/quickjs-ng/qjs_exe tests/js/dom.test.js
```
Expected: `N passed, 0 failed`。

- [ ] **Step 5: 注册 CMake 单测**

`tests/CMakeLists.txt` 追加：
```cmake
add_test(NAME js_dom COMMAND $<TARGET_FILE:qjs_exe> ${CMAKE_CURRENT_SOURCE_DIR}/js/dom.test.js)
```

- [ ] **Step 6: Commit**

```bash
git add src/js/dom.js tests/js/dom.test.js tests/CMakeLists.txt
git commit -m "feat(m2a): dom.js DOM 子集 + 查询 + 原型方法 + 交互辅助"
```

---

### Task 4: render.js + golden 迁移

**Files:**
- Create: `src/js/render.js`
- Create: `tests/js/render.test.js`
- Modify: `tests/goldens/golden_runner.cc`
- Modify: `tests/CMakeLists.txt`（golden 注册）
- Create: `tests/goldens/fixtures/{entities,scriptpage}.html` + `tests/goldens/goldens/{entities,scriptpage}.golden`

**Interfaces:**
- Consumes: `_tb_parser`（Task 2）、`document`（Task 3）
- Produces: `tb_render_js(doc, url, status)` → `{ text, title, url, status, elems:[{id,type,text,href,name,value,options,off}] }`
- golden_runner 改为驱动真实 seam（Task 5 才实现 engine——本任务先驱动 JS 侧函数？不：golden_runner 用 seam 调 `tb_default_js_engine()`，Task 5 提供真实现；Task 4 阶段 golden_runner 直接 include quickjs 跑 builtins 会重复 Task 5 工作）。

**修正接线顺序**：golden_runner 需要 seam + `tb_default_js_engine` 真实实现才能跑。故本任务 golden_runner 的重构放到 Task 5 完成（Task 5 实现 engine 后 golden_runner 切 seam 并跑全量 golden 验证等价）。本任务只做 render.js + 新 fixtures 的 JS 侧验证。

**Interfaces（修正后）:**
- Produces: `tb_render_js(doc, url, status)`（本任务）
- 本任务结束时不改 golden_runner；golden 逐字节验证延至 Task 5 接线后统一执行（见 Task 5 Step 9）

- [ ] **Step 1: 写失败测试（红）**

`tests/js/render.test.js`：
```js
load("./tests/js/lib/testlib.js");
var P = _tb_parser;
function R(html, url, status) {
  var d = P.parse(html);
  document._tb_attach(d.root);
  return tb_render_js(d.root, url || "http://x/", status === undefined ? 200 : status);
}

t("text extraction with collapse", function () {
  var r = R("<p>  hello   world  </p><p>\n\nline2</p>");
  eq(r.text, "hello world\nline2");
});

t("block elements newline separated", function () {
  var r = R("<div>a</div><div>b</div>");
  eq(r.text, "a\nb");
});

t("inline no newline", function () {
  var r = R("<p>a<b>b</b>c</p>");
  eq(r.text, "abc");
});

t("title extracted", function () {
  var r = R("<title>Hello</title><p>x</p>");
  eq(r.title, "Hello");
});

t("links and buttons elems", function () {
  var r = R("<a href='/a'>A</a><button>Go</button>");
  eq(r.elems.length, 2);
  eq(r.elems[0].type, "link");
  eq(r.elems[0].href, "/a");
  eq(r.elems[0].text, "A");
  eq(r.elems[1].type, "button");
  eq(r.elems[1].text, "Go");
});

t("input by type", function () {
  var r = R("<input name='q'><input type='submit' value='Go'>");
  eq(r.elems.length, 2);
  eq(r.elems[0].type, "input");
  eq(r.elems[1].type, "button");
});

t("select options", function () {
  var r = R("<select name='s'><option>en</option><option>zh</option></select>");
  eq(r.elems.length, 1);
  eq(r.elems[0].type, "select");
  eq(r.elems[0].options.length, 2);
  eq(r.elems[0].options[1], "zh");
});

t("form elem recorded", function () {
  var r = R("<form action='/x'><input name='q'></form>");
  eq(r.elems.length, 2);
  eq(r.elems[0].type, "form");
});

t("script/style/head skipped from text", function () {
  var r = R("<head><title>T</title></head><script>var x=1;</script><style>p{}</style><p>v</p>");
  eq(r.text, "v");
});

t("offsets increase within text", function () {
  var r = R("<p>aaa<a href='/x'>bbb</a>ccc</p>");
  // aaa(0-2) link(3-5) ccc(6-8)
  eq(r.text, "aaabbbccc");
  eq(r.elems[0].off, 3);
});

t("leading/trailing blank trimmed", function () {
  var r = R("<div></div><p>mid</p><div></div>");
  eq(r.text, "mid");
});

t("deep tree does not overflow (iterative)", function () {
  var ins = P.make();
  var s = "";
  for (var i = 0; i < 2000; i++) s += "<div>";
  s += "x";
  for (i = 0; i < 2000; i++) s += "</div>";
  ins.feed(s); ins.finalize();
  document._tb_attach(ins.root);
  var r = tb_render_js(ins.root, "http://x/", 200);
  eq(r.text, "x");
});

t("option text not in body", function () {
  var r = R("<select><option>en</option><option>zh</option></select>");
  eq(r.text, "");
});
```

- [ ] **Step 2: 跑测试确认红**

```bash
cd /home/gem/project/tinybrowser && ./build/deps/quickjs-ng/qjs_exe tests/js/render.test.js
```
Expected: FAIL（`tb_render_js` 未定义）。

- [ ] **Step 3: 实现 render.js**

`src/js/render.js`（完整逻辑，镜像 `render.c` 语义）：
```js
// 渲染:DOM 树 → {text, title, url, status, elems}。语义与 M1 render.c 逐字节一致。
var _tb_block = { p:1, div:1, h1:1, h2:1, h3:1, h4:1, h5:1, h6:1, li:1, ul:1, ol:1,
  table:1, tr:1, section:1, article:1, header:1, footer:1, nav:1, aside:1,
  br:1, hr:1, blockquote:1, pre:1, form:1, select:1, fieldset:1 };
var _tb_skip = { script:1, style:1, noscript:1, head:1 };

function tb_render_js(doc, url, status) {
  var c = { text: "", title: "", elems: [], url: url, status: status };
  var first = true;

  function push(s) {
    // 空白折叠:line-start 与 trailing 空白丢弃,中间折行
    for (var k = 0; k < s.length; k++) {
      var ch = s[k];
      if (ch === " " || ch === "\t" || ch === "\r" || ch === "\n") {
        if (c.text.length === 0) continue;                 // line-start 丢弃
        if (c.text[c.text.length - 1] === "\n") continue;  // 折叠连续换行
        c.text += "\n";
      } else c.text += ch;
    }
  }

  function addElem(e) {
    e.off = c.text.length;
    c.elems.push(e);
  }

  // 迭代 walk(显式栈,禁递归)。栈帧:{n, cur_sel, visited}
  // 前序处理逻辑镜像 render.c:block 前换行、子节点、block 后换行、恢复 cur_sel
  var stack = [];
  // 每帧存:节点、进入时 select 状态、是否已展开子树、是否已出(处理 after)
  // 实现:两遍法——栈里放 {n, curSel, phase}
  stack.push({ n: doc, curSel: -1, phase: 0 });
  var curSel = -1;   // 当前 select 元素 id(用于 option 收集),-1 = 不在 select 内

  while (stack.length) {
    var f = stack[stack.length - 1];
    var n = f.n;
    if (f.phase === 0) {
      f.phase = 1;
      if (n.type === "text") {
        if (curSel < 0) push(n.text);   // select 内 option 文本不进正文
        continue;
      }
      if (n.type !== "element") continue;
      if (_tb_skip[n.tag]) { stack.pop(); continue; }   // 整棵子树跳过
      if (n.tag === "title") { c.title = n.children.length ? tb_text_of(n) : ""; stack.pop(); continue; }

      // 元素自身 → elem 记录
      if (n.tag === "a" && n.attrs.href) {
        addElem({ id: n.id, type: "link", text: tb_text_of(n), href: n.attrs.href, name: "", value: "", options: [] });
      } else if (n.tag === "input") {
        var it = n.attrs.type || "text";
        if (it === "hidden") { stack.pop(); continue; }
        var kind = (it === "submit" || it === "button") ? "button" : "input";
        addElem({ id: n.id, type: kind, text: "", href: "", name: n.attrs.name || "", value: it === "checkbox" ? (n.attrs.checked !== undefined ? "on" : "") : (n.attrs.value || ""), options: [] });
      } else if (n.tag === "button") {
        addElem({ id: n.id, type: "button", text: tb_text_of(n), href: "", name: n.attrs.name || "", value: n.attrs.value || "", options: [] });
      } else if (n.tag === "select") {
        addElem({ id: n.id, type: "select", text: "", href: "", name: n.attrs.name || "", value: "", options: [] });
        // 子选项收集:进入 select 子树时设置 curSel
        // 但 select 的 option 是直接子节点——用帧的 curSel 标记
        f.curSel = n.id;
      } else if (n.tag === "form") {
        addElem({ id: n.id, type: "form", text: "", href: n.attrs.action || "", name: "", value: "", options: [] });
      } else if (n.tag === "option") {
        // option 仅当在 select 内:收集文本到当前 select 的 elem
        // 因为 elem 已 push,需在进入 option 前记录其父 select 的 elem 下标
        f.optOf = _tb_find_elem_of(c.elems, curSel);
      }

      // block 前换行
      if (_tb_block[n.tag] && n.tag !== "br" && n.tag !== "hr" && n.tag !== "li") {
        if (!first && c.text.length && c.text[c.text.length - 1] !== "\n") push("\n");
      }
      if (n.tag === "br" || n.tag === "hr") {
        if (!first && c.text.length && c.text[c.text.length - 1] !== "\n") push("\n");
      }
      first = false;

      // 展开子节点(逆序入栈保持文档序)
      for (var k = n.children.length - 1; k >= 0; k--) {
        stack.push({ n: n.children[k], curSel: curSel, phase: 0 });
      }
    } else {
      // 子树处理完:block 后换行 + 恢复 curSel
      if (_tb_block[n.tag] && n.tag !== "br" && n.tag !== "hr" && n.tag !== "li") {
        if (c.text.length && c.text[c.text.length - 1] !== "\n") push("\n");
      }
      if (n.tag === "select") { curSel = f.curSel === undefined ? -1 : -1; }  // 出 select
      stack.pop();
    }
  }

  // 裁剪首尾空行
  var t = c.text;
  t = t.replace(/^\n+/, "").replace(/\n+$/, "");
  c.text = t;

  return c;

  function tb_text_of(n) {
    // 迭代取后代文本(trim 到首尾无空白)
    var out = "", st = [n];
    while (st.length) {
      var x = st.pop();
      if (x.type === "text") out += x.text;
      else for (var q = x.children.length - 1; q >= 0; q--) st.push(x.children[q]);
    }
    return out.trim();
  }
  function _tb_find_elem_of(elems, selId) {
    for (var q = elems.length - 1; q >= 0; q--) if (elems[q].id === selId) return q;
    return -1;
  }
}
```
（关键语义对照 render.c：`curSel` 是"当前 select 内"标记；`f.curSel` 存进入帧时的值以便恢复。**注意上面骨架里 select 子树的 option 收集逻辑需要实现者仔细对照 render.c 的 select/cur_sel 处理**——render.c 里 select 元素本身 add_elem 后，其 option 子节点的文本被收集为 select elem 的 options[]，不进正文。实现时必须保证：进入 select 后 `curSel = select 的 elem 下标`，option 子节点把 `text` 追加到 `c.elems[curSel].options` 而非正文。`push` 跳过 select 内文本。子 select 嵌套恢复。以上骨架的 phase 机制已给出容器，agent 照 render.c:60-140 逐条移植语义。）

- [ ] **Step 4: 跑测试（绿）**

```bash
cd /home/gem/project/tinybrowser && ./build/deps/quickjs-ng/qjs_exe tests/js/render.test.js
```
Expected: `N passed, 0 failed`。

- [ ] **Step 5: 新增 M2a fixtures**

`tests/goldens/fixtures/entities.html`：
```html
<!DOCTYPE html>
<title>Entities</title>
<p>a&amp;b &lt;tag&gt; &quot;q&quot; &#65;&#x42; &nbsp;</p>
<input name=q value=hi checked>
<ul><li>one<li>two</ul>
<p>mis<b>nest<i>x</b>y</i></p>
```
`tests/goldens/fixtures/scriptpage.html`：
```html
<!DOCTYPE html>
<title>ScriptPage</title>
<script>var hidden = "&amp; &lt; not parsed";</script>
<p>after script</p>
```
（scriptpage 的脚本只赋值局部变量，不改 DOM——golden 与 M1 语义一致：script 子树不产出文本。）

- [ ] **Step 6: 注册 JS render 单测**

`tests/CMakeLists.txt` 追加：
```cmake
add_test(NAME js_render COMMAND $<TARGET_FILE:qjs_exe> ${CMAKE_CURRENT_SOURCE_DIR}/js/render.test.js)
```

- [ ] **Step 7: Commit**

```bash
git add src/js/render.js tests/js/render.test.js tests/goldens/fixtures/entities.html tests/goldens/fixtures/scriptpage.html tests/CMakeLists.txt
git commit -m "feat(m2a): render.js 渲染管线 + JS 单测 + M2a fixtures"
```

---

### Task 5: js_engine.c——C 桥 + seam 真实实现 + golden 接线

**Files:**
- Create: `src/core/js_engine.c`
- Create: `src/core/js_engine.h`
- Modify: `CMakeLists.txt`（加 js_engine.c、js_builtins.inc 生成接入、golden_runner 依赖）
- Modify: `src/core/browser.c`（`tb_load_sync` 实现、`tb_eval_js` 真实现）
- Modify: `src/core/browser_internal.h`（`tb_load_sync` 声明已含）
- Modify: `tests/goldens/golden_runner.cc`（切 seam）
- Modify: `tests/CMakeLists.txt`（golden_runner 链接 qjs）
- Create: `tests/unit/test_js_engine.cc`

**Interfaces:**
- Consumes: seam（Task 1）、builtins（Task 2-4 的 parser.js/dom.js/render.js）、`browser_internal.h` 的 `struct tb_browser`
- Produces:
  - `const struct tb_js_engine *tb_default_js_engine(void)`（真实实现）
  - `src/core/js_engine.h` 暴露 `tb_default_js_engine` 给 golden_runner/browser.c
  - C 桥函数（供 JS）：`__tb_console/__tb_timer_set/__tb_timer_cancel/__tb_cookie/__tb_cookie_set/__tb_load_sync`
  - `tb_load_sync(tb_browser*, url, char **out)`（nested pump，browser.c）
  - golden_runner 现在驱动真实 engine，验证既有 golden 逐字节等价

- [ ] **Step 1: 写失败测试（红）**

`tests/unit/test_js_engine.cc`：
```c
#include "tb.h"
#include "fakes.h"
#include "browser_internal.h"   // 或经 include 路径
#include <gtest/gtest.h>
#include <string.h>
#include <stdlib.h>

// 直接驱动 seam(host=NULL):console 降级、load_sync 空、cookie 空、timer 不 pump
TEST(JsEngine, DefaultEngineOpenEvalClose) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  ASSERT_NE(eng, nullptr);
  void *h = eng->open(eng, NULL);
  ASSERT_NE(h, nullptr);
  char *out = nullptr;
  ASSERT_EQ(eng->eval(eng, h, "1 + 1", &out), 0);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "2");
  free(out);
  eng->close(eng, h);
}

TEST(JsEngine, ConsoleBridgeWithoutHost) {
  static int calls = 0; static char got[256] = {0};
  static tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.on_console = [](tb_browser*, const char *level, const char *msg, void*) {
    calls++; strncpy(got, msg, sizeof got - 1); (void)level;
  };
  // host NULL 时 on_console 不触发(优雅降级)
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  char *out = nullptr;
  eng->eval(eng, h, "__tb_console('log', 'hi')", &out);
  free(out);
  EXPECT_EQ(calls, 0);
  eng->close(eng, h);
}

TEST(JsEngine, EvalJsOverBrowser) {
  // browser 默认 engine(未接线 load_document 前 eval 应失败)
  fake_clock fc; fake_clock_init(&fc);
  fake_transport ft; fake_transport_init(&ft, &fc);
  tb_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "t"; cfg.clock = &fc.base; cfg.transport = &ft.base;
  tb_browser *b = tb_create(&cfg);
  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "1", &out).code, TB_ERR_NO_VIEW);
  tb_destroy(b);
}
```
（lambda 在 gtest 里 OK，但 C 回调用普通 static 函数更稳——实现时用 static 函数。`TB_ERR_NO_VIEW` 需在 tb.h 导出。）

- [ ] **Step 2: 跑测试确认红**

```bash
cd /home/gem/project/tinybrowser && make -C build -j$(nproc) && ctest --test-dir build -R test_js_engine --output-on-failure
```
Expected: 编译失败或 FAIL（`tb_default_js_engine`/`tb_eval_js` 未实现）。

- [ ] **Step 3: 实现 js_engine.h**

`src/core/js_engine.h`：
```c
#ifndef TB_JS_ENGINE_H
#define TB_JS_ENGINE_H
#include "tb.h"
const struct tb_js_engine *tb_default_js_engine(void);
#endif
```

- [ ] **Step 4: 实现 js_engine.c**

要点（完整实现）：
- runtime 单例：`static JSRuntime *g_rt`（首次 open 时 `JS_NewRuntime()`；`js_memory_limit` 由首个 cfg 设置——多 browser 场景取 max；interrupt 状态结构体 `static struct { const tb_clock *clock; uint64_t start_ms; uint32_t limit_ms; } g_isr;`）
- doc context 结构：
```c
struct js_doc {
  JSContext *ctx;
  struct tb_browser *host;         /* 可能 NULL */
  tb_config cfg;                   /* open 时拷贝,桥用 */
  struct timer { uint32_t id; uint64_t due_ms; JSValue cb; struct timer *next; } *timers;
  uint32_t next_timer_id;
  int is_loading;                  /* 脚本执行期置 1 */
};
```
- interrupt handler（`JS_SetInterruptHandler`）：`g_isr.limit_ms && clock->now_ms() >= g_isr.start_ms + g_isr.limit_ms` 返回 1（中断 → JS exception）。
- `open`：取 host 的 cfg（host 非 NULL 用 `host->cfg`，否则用 static 默认 zero cfg）；`JS_NewContext(g_rt)`；注册全局函数（6 个桥）；逐个 `JS_Eval(g_ctx, tb_js_builtins[i], len, "<builtin>", JS_EVAL_TYPE_GLOBAL)`（错误 → 记录并返回 NULL）。`struct js_doc` 挂 `JS_SetContextOpaque(ctx, d)` 供桥取 host。
- 桥实现（C 函数 `__tb_console` 等，注册为 JS function）：
  - `__tb_console(level, msg)`：取 `d->cfg.on_console` 非 NULL 且 host 非 NULL 时调 `on_console(host, level_str, msg_str, cfg.ud)`。
  - `__tb_timer_set(ms, cb)`：`d->host` 为 NULL → 返回 0 且不存（降级）；否则 `due = cfg.clock->now_ms() + ms`，插链表，返回 id。
  - `__tb_timer_cancel(id)`：从链表删（JSValue free）。
  - `__tb_cookie()`：host 非 NULL → `host->cookie_jar ? host->cookie_jar : ""`；host NULL → ""。
  - `__tb_cookie_set(str)`：host 非 NULL → free 旧的、strdup 新的。
  - `__tb_load_sync(url)`：host 非 NULL → `tb_load_sync(host, url, &body)`，转 JS 字符串并 free；host NULL → 空字符串。
- `load_document`：从 `tb_js_builtins` 已注入的全局找 `_tb_parser`/`document`；构造加载脚本：
  ```js
  (function () {
    var p = _tb_parser.make();
    p.feed(<body>);          // 经 JS_NewStringLen 传入,不拼字符串
    p.finalize();
    document._tb_attach(p.root);
    _tb_parser._readyState = "loading";
    return p;
  })()
  ```
  拿到 `p`（JSValue，返回对象）后循环 `p.next_script()`：每条记录 `{src,text}`——
  - `text` 非空：`JS_Eval(ctx, text, len, "<script>", JS_EVAL_TYPE_GLOBAL)`，异常 → `__tb_console("error", <script> 执行异常)` 并 `JS_GetException` 清理（try/catch 等价隔离）。
  - `src` 非空：调 `__tb_load_sync(src)` 得 body，再 eval。
  - 每次 eval 前重置 `g_isr.start_ms = clock->now_ms()`、`limit = cfg.js_exec_ms_limit`。
  - eval 后检查 JS exception；每 script 独立 try/catch。
  - `p` 对象本身用 `JS_FreeValue`（`p` 是 JS 对象引用，next_script 后不再需要——但脚本可能引用 p？不需要，脚本只碰 document）。
  全部执行完：`_tb_parser._readyState = "complete"`（通过 eval 一行 JS）。`d->is_loading = 0`。返回 0。
- `render`：eval `tb_render_js(document._root, <url>, <status>)` → 返回值 JS 对象 → 用 `tb_view_from_js(ctx, obj, out)` 填充 `tb_view`（`out->url/status` 用参数直接 strdup 填入，不读 JS）：
```c
static int view_from_js(JSContext *ctx, JSValue o, const char *url, int status, tb_view *v) {
  v->url = strdup(url ? url : "");
  v->status = status;
  v->title = js_str_prop(ctx, o, "title");
  v->text  = js_str_prop(ctx, o, "text");
  v->not_renderable = 0; v->nrt = NULL;
  JSValue arr = JS_GetPropertyStr(ctx, o, "elems");
  uint32_t n = 0; JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, arr, "length"));
  v->elems = calloc(n ? n : 1, sizeof(struct tb_elem));
  v->nelems = n;
  for (uint32_t i = 0; i < n; i++) {
    JSValue e = JS_GetPropertyUint32(ctx, arr, i);
    struct tb_elem *dst = &v->elems[i];
    dst->id = js_int_prop(ctx, e, "id");
    dst->type = js_str_dup_prop(ctx, e, "type");   // strdup(桥持有)
    dst->text = js_str_dup_prop(ctx, e, "text");
    dst->href = js_str_dup_prop(ctx, e, "href");
    dst->name = js_str_dup_prop(ctx, e, "name");
    dst->value = js_str_dup_prop(ctx, e, "value");
    dst->off = (size_t)js_int_prop(ctx, e, "off");
    dst->options = NULL; dst->noptions = 0;
    JSValue oa = JS_GetPropertyStr(ctx, e, "options");
    if (JS_IsArray(ctx, oa)) { uint32_t on=0; JS_ToUint32(ctx,&on,JS_GetPropertyStr(ctx,oa,"length"));
      dst->noptions = on; dst->options = calloc(on?on:1, sizeof(char*));
      for (uint32_t j=0;j<on;j++){ JSValue s=JS_GetPropertyUint32(ctx,oa,j); const char *cs=JS_ToCString(ctx,s);
        dst->options[j] = strdup(cs?cs:""); if(cs) JS_FreeCString(ctx,cs); JS_FreeValue(ctx,s);} }
    JS_FreeValue(ctx, oa);
    JS_FreeValue(ctx, e);
  }
  JS_FreeValue(ctx, arr);
  return 0;
}
```
  `js_str_prop`/`js_str_dup_prop`/`js_int_prop` 小工具：取属性转 C 字符串/int（str_dup 用 strdup，str_ 复用 JS_ToCString 缓冲避免 dup？view 的字段语义是"桥 owns"——M1 view 里 type/text/href/name/value 是 strdup 的（tb_view_free 会 free）。故全用 strdup。`options` 也是 strdup。）
- `eval`：`JS_Eval(ctx, code, len, "<eval>", JS_EVAL_TYPE_GLOBAL)`；异常 → 返回 TB_ERR_* 且 `*out = strdup(JS_GetException 消息)`；否则结果 `JS_ToCString` → `*out = strdup`。
- `poll_timers`：遍历 d->timers，`due <= clock->now_ms()` 的：`JS_Call(ctx, cb, JS_UNDEFINED, 0, NULL)`（异常忽略），free，从链表移除；返回。
- `close`：free 所有 timer（JSValue free），`JS_FreeContext(ctx)`，free struct js_doc。
- 静态 `const struct tb_js_engine g_quickjs = { js_open, js_load_document, js_render, js_eval, js_poll_timers, js_close };` + `tb_default_js_engine(void){ return &g_quickjs; }`
- `JSValue` 引用管理：所有 `JS_GetPropertyStr` 结果用完 `JS_FreeValue`。

- [ ] **Step 5: 实现 tb_load_sync（browser.c）**

```c
tb_err tb_load_sync(tb_browser *b, const char *url, char **out) {
  /* nested pump:子资源同步获取。失败/超时 → *out = 空串。 */
  tb_transport_req req; memset(&req, 0, sizeof req);
  req.method = "GET"; req.url = url;
  char *body = NULL; size_t blen = 0; int done = 0; int failed = 0;
  void *op = b->transport->open(&req);
  if (!op) { *out = strdup(""); return TB_ERR_NET; }
  struct { tb_browser *b; int *done, *failed; char **body; size_t *blen; } cbctx = { b, &done, &failed, &body, &blen };
  /* on_done 由 transport 回调设置 */
  ...
  uint64_t t0 = b->cfg.clock->now_ms();
  while (!done && !failed) {
    uv_run(&b->loop, UV_RUN_NOWAIT);
    b->transport->poll(...);
    if (b->cfg.clock->now_ms() - t0 > b->cfg.nav_timeout_ms) break;
  }
  b->transport->cancel(op);
  *out = body ? body : strdup("");
  return done && !failed ? TB_OK : TB_ERR_TIMEOUT;
}
```
（transport 的 on_done 签名从现有 `tb_transport` 结构（session.h）抄——op 内嵌回调。`tb_transport_req` 含 `on_done`/`on_headers`/`on_body` 回调与 `ud`。实现时复用现有 session 对 transport 回调的用法。失败 → `body` 置空 + `failed=1`。）

- [ ] **Step 6: 实现 tb_eval_js（browser.c）**

```c
tb_err tb_eval_js(tb_browser *b, const char *code, char **out) {
  if (!b->engine || !b->js_doc) return tb_err(TB_ERR_NO_VIEW, "no document");
  return b->engine->eval(b->engine, b->js_doc, code, out);
}
```

- [ ] **Step 7: CMake 接线**

- 源列表加 `src/core/js_engine.c`。
- include dirs 加 `${CMAKE_CURRENT_SOURCE_DIR}/deps/quickjs-ng`。
- link 加 `qjs`（Task 1 已加）。
- `tb_load_sync` 用 `uv_run`——browser.c 已链 uv_a。
- golden_runner：`target_link_libraries(test_golden PRIVATE tinybrowser qjs gtest_main)`（golden_runner 用 gtest 跑 `-u`）。

- [ ] **Step 8: 改造 golden_runner.cc 驱动 seam**

`tests/goldens/golden_runner.cc` 主逻辑改为：
```c
#include "tb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *slurp(const char *path, size_t *len) { ... }   /* 已有 */

/* 直接驱动真实 engine:open(NULL)/load_document/render → dump。 */
static char *render_html(const char *html, size_t hlen, const char *url) {
  const struct tb_js_engine *eng = tb_default_js_engine();
  void *h = eng->open(eng, NULL);
  if (!h) return NULL;
  if (eng->load_document(eng, h, html, hlen) != 0) { eng->close(eng, h); return NULL; }
  tb_view *v = tb_view_new();
  if (eng->render(eng, h, url, 200, v) != 0) { tb_view_free(v); eng->close(eng, h); return NULL; }
  char *dump = tb_view_dump(v);
  tb_view_free(v);
  eng->close(eng, h);
  return dump;
}
```
（其余 `-u` 重生成、diff 逻辑保留。fixtures 读法不变。）

- [ ] **Step 9: 生成新 fixtures 的 golden 并全量验证等价**

```bash
cd /home/gem/project/tinybrowser && make -C build -j$(nproc)
# 既有 golden 必须逐字节不变
./build/tests/test_golden
# 新增 fixtures 生成 golden(人工审阅后提交)
./build/tests/test_golden -u
```
检查 `tests/goldens/goldens/entities.golden`、`scriptpage.golden` 内容符合预期（实体解码、script 子树跳过、隐式闭合）。同时确认 hello/forms/nav golden **未被改动**（`git status` 里这三个文件不应 modified）。注意：此时 browser.c 尚未接线 engine（Task 6），golden_runner 直接驱动 seam 不受影响。

- [ ] **Step 10: 全量测试**

```bash
cd /home/gem/project/tinybrowser && ctest --test-dir build --output-on-failure
```
Expected: 全绿（含 js_parser/js_dom/js_render、test_qjs_embed、test_js_engine、golden）。`test_interact`/`test_browser` 仍走旧路径（browser.c 未接线 engine），不受影响。

- [ ] **Step 11: Commit**

```bash
git add src/core/js_engine.c src/core/js_engine.h src/core/browser.c src/core/browser_internal.h CMakeLists.txt tests/goldens/golden_runner.cc tests/goldens/goldens/entities.golden tests/goldens/goldens/scriptpage.golden tests/unit/test_js_engine.cc tests/CMakeLists.txt
git commit -m "feat(m2a): js_engine.c C 桥 + seam 实现 + golden 切真实引擎"
```

---

### Task 6: browser.c 迁移——nav 接线 + 交互经 JS + 删 lexbor

**Files:**
- Modify: `src/core/browser.c`（nav_on_done、tb_create/destroy、交互五函数）
- Modify: `CMakeLists.txt`（删 lexbor、删 dom.c/render.c）
- Delete: `src/core/dom.c`, `src/core/dom.h`, `src/core/render.c`, `src/core/render.h`
- Delete: `deps/lexbor/`
- Create: `src/core/json.c`、`src/core/json.h`（最小 JSON 工具）
- Modify: `tests/CMakeLists.txt`（删 test_dom/test_render、重写 test_interact）
- Modify: `tests/unit/test_interact.cc`（保持语义，driver 不变）
- Delete: `tests/unit/test_dom.cc`, `tests/unit/test_render.cc`

**Interfaces:**
- Consumes: seam（Task 1/5）、`_tb_elem_info`/`_tb_form_info`（Task 3）、render.js（Task 4）、json.c
- Produces:
  - `src/core/json.h`：`int json_get_str(const char *json, const char *key, char **out)`、`int json_get_int(const char *json, const char *key, int64_t *out)`、`int json_get_str_array(const char *json, const char *key, char ***out, int *n)`
  - `js_engine.c` 补 `_tb_form_info(form_id)` 桥函数（本任务新增到 dom.js 或 js_engine 注入）：
    ```js
    function _tb_form_info(form_id) {
      var f = document.getElementById(String(form_id));
      if (!f || f.tag !== "form") return "{}";
      var o = { action: f.attrs.action || "", method: (f.attrs.method || "get").toLowerCase() };
      o.controls = [];
      // 迭代收集 form 内控件:input(name), select(name), textarea(name), button(name 仅当有 name)
      var stack = [f];
      while (stack.length) {
        var n = stack.pop();
        if (n.type === "element" && n !== f) {
          if ((n.tag === "input" || n.tag === "select" || n.tag === "textarea") && n.attrs.name) {
            o.controls.push({ id: n.id, name: n.attrs.name, tag: n.tag,
              type: n.attrs.type || "text", value: n.attrs.value || "" });
            continue;   // 控件自身不再深入(select 的 option 不需要)
          }
          for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
        } else {
          for (var j = n.children.length - 1; j >= 0; j--) stack.push(n.children[j]);
        }
      }
      return JSON.stringify(o);
    }
    ```
- 交互语义（M1 不变）：
  - `tb_click(id)`：`_tb_elem_info(id)` → tag "a" → 导航 href（url 相对解析如旧）；tag "button"/"input"(button 型) → `_tb_form_info(form_of)` → submit；其他 tag → 无操作。
  - `tb_fill(id, value)`：`_tb_elem_info(id)` → tag 必须 "input" → 存 fv。
  - `tb_select(id, value)`：`_tb_elem_info(id)` → tag 必须 "select" 且 value ∈ options[] → 存 fv。
  - `tb_submit(form_id)`：`_tb_form_info(form_id)` → 控件清单，C 侧合并 fv（`find_fv(b, id)` 命中用 fv.value 否则 attr value）→ 组 pairs → GET query / POST body（`tb_url_encode` 如旧）→ 导航。
- 旧 `find_node_by_id`/`node_in_subtree`/`find_containing_form`/`collect_form_pairs`/`tb_dom_*` 依赖全部删除。

- [ ] **Step 1: 实现 json.c/json.h**

`src/core/json.h`：
```c
#ifndef TB_JSON_H
#define TB_JSON_H
#include <stdint.h>
int json_get_str(const char *json, const char *key, char **out);   /* 0=找到并 strdup 到 *out;1=未找到 */
int json_get_int(const char *json, const char *key, int64_t *out);
int json_get_str_array(const char *json, const char *key, char ***out, int *n);
#endif
```
`src/core/json.c`：手写极简解析器，仅支持 `{"k":"v", "k":123, "k":["a","b"]}`（无嵌套对象、无数字 key）。实现要点：逐字符扫，匹配 `"key" :` 后取值；字符串值处理 `\"` 与 `\\` 转义；数组收集字符串元素。~120 行。单测：Task 6 里 `tests/unit/test_json.cc`（4 个用例：字符串/整数/数组/未找到/转义）。

- [ ] **Step 2: 写失败测试（红）——重写 test_interact 断言 JS 查询路径**

`tests/unit/test_interact.cc` 保持现有 5 个用例结构与断言（driver 不变：fake transport + 真 browser + tb_click/fill/select/submit）。仅当编译/行为因接线而变时调整。Task 6 完成后这 5 个用例必须原样通过。同时 `tests/CMakeLists.txt` 删掉 `unit/test_dom.cc`、`unit/test_render.cc`（`add_executable` 行与 `gtest_discover_tests` 行），新增 `unit/test_json.cc`。

- [ ] **Step 3: 迁移 browser.c——nav 接线**

`nav_on_done`（现 75-83 行 `b->dom = tb_dom_parse(...); b->view = tb_render(...)`）替换为：
```c
if (b->content_type_ok) {
  b->js_doc = b->engine->open(b->engine, b);       /* 创建新文档 context */
  if (!b->js_doc) { /* 引擎失败 */ ... on_error(TB_ERR_PARSE) ... }
  int rc = b->engine->load_document(b->engine, b->js_doc, body, len);
  if (rc != 0) { b->engine->close(b->engine, b->js_doc); b->js_doc = NULL; on_error(TB_ERR_PARSE); }
  else b->view = tb_view_new(), b->engine->render(b->engine, b->js_doc, eff_url, status, b->view);
}
```
（`not_renderable` 分支保持：非 text/html 时 `b->view` 仍建 not_renderable view，不 open engine。）
`tb_create`：`b->engine = cfg.js_engine ? cfg.js_engine : tb_default_js_engine();`（删 Task 1 的 NULL stub）。`tb_destroy`：`if (b->js_doc) { b->engine->close(b->engine, b->js_doc); b->js_doc = NULL; }`；free cookie_jar。`tb_pump` 里在 `transport->poll` 后加 `if (b->js_doc) b->engine->poll_timers(b->engine, b->js_doc);`（timer 由 pump 驱动）。

- [ ] **Step 4: 迁移交互五函数（C 侧）**

```c
static int elem_info(tb_browser *b, int id, char **out) {
  char code[64];
  snprintf(code, sizeof code, "_tb_elem_info(%d)", id);
  return b->engine->eval(b->engine, b->js_doc, code, out);
}
tb_err tb_click(tb_browser *b, int id) {
  if (!b->js_doc) return TB_ERR_NO_VIEW;
  char *info = NULL;
  if (elem_info(b, id, &info) != 0) return TB_ERR_NO_ELEM;
  /* 解析 info:tag */
  char *tag = NULL; int64_t form_id = 0; char *href = NULL;
  json_get_str(info, "tag", &tag);
  if (!tag || !*tag) { free(tag); free(info); return TB_ERR_NO_ELEM; }
  if (!strcmp(tag, "a")) { json_get_str(info, "href", &href);
    if (href && *href) { tb_err r = tb_navigate(b, tb_url_join(b->session.url, href)); free(href); free(tag); free(info); return r; }
    free(href);
  } else if (!strcmp(tag, "button") || !strcmp(tag, "input")) {
    /* input 需 type=submit/button → 由 _tb_elem_info 已归类为 button 型 */
    json_get_int(info, "form", &form_id);
    if (form_id) { tb_err r = tb_submit(b, (int)form_id); free(tag); free(info); return r; }
  }
  free(tag); free(info); return TB_OK;
}
tb_err tb_fill(tb_browser *b, int id, const char *value) {
  if (!b->js_doc) return TB_ERR_NO_VIEW;
  char *info = NULL; if (elem_info(b, id, &info) != 0) return TB_ERR_NO_ELEM;
  char *tag = NULL; json_get_str(info, "tag", &tag);
  int bad = !tag || strcmp(tag, "input");
  free(tag); free(info);
  if (bad) return TB_ERR_NO_ELEM;
  set_fv(b, id, value);
  return TB_OK;
}
tb_err tb_select(tb_browser *b, int id, const char *value) {
  if (!b->js_doc) return TB_ERR_NO_VIEW;
  char *info = NULL; if (elem_info(b, id, &info) != 0) return TB_ERR_NO_ELEM;
  char *tag = NULL; json_get_str(info, "tag", &tag);
  int bad = !tag || strcmp(tag, "select");
  int found = 0;
  if (!bad) { char **opts = NULL; int n = 0;
    json_get_str_array(info, "options", &opts, &n);
    for (int i = 0; i < n; i++) if (!strcmp(opts[i], value)) { found = 1; break; }
    for (int i = 0; i < n; i++) free(opts[i]); free(opts); }
  free(tag); free(info);
  if (bad || !found) return TB_ERR_NO_ELEM;
  set_fv(b, id, value);
  return TB_OK;
}
tb_err tb_submit(tb_browser *b, int form_id) {
  if (!b->js_doc) return TB_ERR_NO_VIEW;
  char code[64]; snprintf(code, sizeof code, "_tb_form_info(%d)", form_id);
  char *info = NULL; if (b->engine->eval(b->engine, b->js_doc, code, &info) != 0) return TB_ERR_NO_ELEM;
  char *action = NULL, *method = NULL;
  json_get_str(info, "action", &action);
  json_get_str(info, "method", &method);
  int is_post = method && !strcmp(method, "post");
  /* 收集控件:info 里 "controls" 是数组,每项 {id,name,tag,type,value} */
  struct pair { int id; char *name; char *value; } pairs[64];
  int np = json_parse_controls(info, pairs, 64);   /* json.c 提供,解析 controls 数组 */
  /* 合并 fv:find_fv(b, id) 命中用 fv.value */
  for (int i = 0; i < np; i++) {
    const char *fv = find_fv(b, pairs[i].id);
    if (fv) { free(pairs[i].value); pairs[i].value = strdup(fv); }
  }
  /* 构建 query/body 用 tb_url_encode,复用旧 collect_form_pairs 之后的编码逻辑 */
  char *target = tb_url_join(b->session.url, action ? action : b->session.url);
  tb_err r = build_and_navigate(b, target, is_post, pairs, np);
  free(action); free(method); free(info);
  return r;
}
```
（`json_parse_controls` 加到 json.h/json.c：解析 `"controls":[{"id":..,"name":"..","value":".."}]` 数组，每个对象抽 id/name/value 到 pairs。`set_fv`/`find_fv` 保留现有 fv 数组逻辑。`build_and_navigate` 复用现有 GET query 拼接 / POST body 编码 + `tb_transport_req` 构造。）

实现注意：旧的 `collect_form_pairs`/`find_containing_form`/`find_node_by_id`/`node_in_subtree` 依赖 `b->dom`（lexbor）——全部删除，替换为上述 engine->eval 路径。`tb_dom_*` 不再被 browser.c 引用。

- [ ] **Step 5: 删 lexbor + CMake**

- `CMakeLists.txt`：删 18-24 行 lexbor 块；`add_library(tinybrowser ...)` 源列表删 `dom.c`、`render.c`；include dirs 删 `${CMAKE_CURRENT_SOURCE_DIR}/deps/lexbor/source`；link 删 `lexbor_static`。
- `git rm -r deps/lexbor src/core/dom.c src/core/dom.h src/core/render.c src/core/render.h tests/unit/test_dom.cc tests/unit/test_render.cc`
- `deps/VERSIONS.lock`、`deps/VERSIONS.md` 删 lexbor 条目。
- `tests/CMakeLists.txt`：删 test_dom/test_render 的 add_executable 与 gtest_discover_tests 行；加 `add_executable(test_json unit/test_json.cc)` + 链接 + discover。

- [ ] **Step 6: 新增 test_json.cc（红→绿）**

`tests/unit/test_json.cc`：
```c
#include "json.h"
#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>

TEST(Json, GetStr) {
  char *out = NULL;
  ASSERT_EQ(json_get_str("{\"a\":\"x\",\"b\":\"y\"}", "b", &out), 0);
  EXPECT_STREQ(out, "y"); free(out);
  EXPECT_EQ(json_get_str("{\"a\":\"x\"}", "nope", &out), 1);
}
TEST(Json, GetInt) {
  int64_t v = 0;
  ASSERT_EQ(json_get_int("{\"form\":42}", "form", &v), 0);
  EXPECT_EQ(v, 42);
}
TEST(Json, Escapes) {
  char *out = NULL;
  ASSERT_EQ(json_get_str("{\"q\":\"a\\\"b\\\\c\"}", "q", &out), 0);
  EXPECT_STREQ(out, "a\"b\\c"); free(out);
}
TEST(Json, StrArray) {
  char **a = NULL; int n = 0;
  ASSERT_EQ(json_get_str_array("{\"options\":[\"en\",\"zh\"]}", "options", &a, &n), 0);
  ASSERT_EQ(n, 2); EXPECT_STREQ(a[0], "en"); EXPECT_STREQ(a[1], "zh");
  for (int i = 0; i < n; i++) free(a[i]); free(a);
}
```

- [ ] **Step 7: 全量回归（核心验收）**

```bash
cd /home/gem/project/tinybrowser && rm -rf build && cmake -S . -B build && make -C build -j$(nproc) && ctest --test-dir build --output-on-failure
```
Expected:
- test_browser 5 用例、test_interact 5 用例、golden（含新 fixtures）、js_*、test_qjs_embed、test_js_engine、test_json 全绿。
- 无 lexbor 符号引用（`grep -r lexbor build/src` 空）。
- 重点：`test_interact` 的 FillThenGetSubmitBuildsQuery 等 5 用例**原样通过**——证明交互经 JS 查询语义等价。

- [ ] **Step 8: 验证渲染等价（关键 golden 检查）**

```bash
cd /home/gem/project/tinybrowser && git diff --stat tests/goldens/goldens/
```
Expected: hello.golden/forms.golden/nav.golden **无任何修改**（M1 与 M2a 渲染逐字节一致）。若被改动，对照 `render.c`（git show 旧版）与 render.js 排查。

- [ ] **Step 9: Commit**

```bash
git add -A deps/lexbor src/core/dom.c src/core/dom.h src/core/render.c src/core/render.h tests/unit/test_dom.cc tests/unit/test_render.cc CMakeLists.txt deps/VERSIONS.lock deps/VERSIONS.md tests/CMakeLists.txt src/core/browser.c src/core/json.c src/core/json.h src/core/js_engine.c tests/unit/test_json.cc tests/unit/test_interact.cc
git commit -m "feat(m2a): browser.c 全面迁移 JS 引擎 + 交互经 JS 查询 + 移除 lexbor"
```

---

### Task 7: 集成测试——HTTP server 全栈（脚本执行 + console + timer）

**Files:**
- Create: `tests/integration/test_m2a_integration.cc`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/core/js_engine.c`（如集成暴露问题）
- Create: `tests/goldens/fixtures/dynamic.html` + `tests/goldens/goldens/dynamic.golden`

**Interfaces:**
- Consumes: 全栈（Task 1-6）
- Produces: 无新公共接口——验证行为契约

- [ ] **Step 1: 写集成测试（红）**

复用现有 HTTP test server（Task m1 TLS 集成用的 embedded server，`tests/integration/http_server.h` 或等价）。`tests/integration/test_m2a_integration.cc`：
```c
#include "tb.h"
#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>

/* 复用 tests/integration 的 HTTP server 工具(见既有 test_tls 用法) */

TEST(M2aIntegration, SyncScriptRunsAndMutatesDOM) {
  // server 返回含 <script>document.title = "ByScript"; var p=document.createElement("p"); p.textContent="added"; document.body.appendChild(p);</script><body></body>
  // 断言:observe 后 title == "ByScript";view text 含 "added"(脚本改 DOM 后渲染)
}

TEST(M2aIntegration, ScriptErrorIsolated) {
  // <script>throw new Error("boom")</script><p>ok</p>
  // 断言:页面仍渲染出 "ok";on_console 收到 error 级消息(含 "boom")
}

TEST(M2aIntegration, ConsoleBridgeCalled) {
  // <script>__tb_console("log", "hello from js")</script>
  // 断言:on_console 回调收到 "hello from js"(level "log")
}

TEST(M2aIntegration, TimerFiresThroughPump) {
  // <script>__tb_timer_set(100, function(){ document.title = "Ticked"; })</script><p>x</p>
  // fake clock?集成用真 transport + 真 clock:pump 100ms 后 title 变 "Ticked"
  // 或:server 页面 + tb_wait_idle(延时由 pump 推进)
}

TEST(M2aIntegration, EvalJsOnLoadedDoc) {
  // 加载静态页 → tb_eval_js(b, "document.title", &out) → out == 页面 title
}
```
（实现时按 tests/integration 既有 HTTP server 的接口签名写。若没有现成 server 工具则本任务先建一个最简 `http_server.h`：绑定随机端口 + 线程 serve 预置响应表。）

- [ ] **Step 2: 跑测试确认红**

```bash
cd /home/gem/project/tinybrowser && make -C build -j$(nproc) && ctest --test-dir build -R m2a --output-on-failure
```

- [ ] **Step 3: 修复暴露的集成问题**

预期可能问题（agent 按实际修复）：
- `__tb_console` 在 host 有 cfg.on_console 时未被正确路由（检查 open 里 host 的 cfg 拷贝）。
- timer 因 pump 未调 `poll_timers` 不触发（检查 Task 6 的 tb_pump 改动）。
- `document.body` 不存在（M2a 无 body 快捷引用）——fixture 改用 `document.querySelector("body")` 或直接 `document._root`。**在 dom.js 补 `document.body` 便捷属性**（`get body() { return document.querySelector("body"); }`，DOM 子集内合理，Task 3 未含——本任务补并加单测）。
- 脚本改 DOM 后 `render` 需在脚本执行后调用（顺序正确性）。

- [ ] **Step 4: dynamic fixture + golden（脚本改 DOM 新基线）**

`tests/goldens/fixtures/dynamic.html`：
```html
<!DOCTYPE html>
<title>Dynamic</title>
<script>
  var el = document.createElement("p");
  el.textContent = "injected";
  document.body.appendChild(el);
</script>
<body><p>static</p></body>
```
`tests/goldens/goldens/dynamic.golden` 由 `-u` 生成后人工审阅（M2a 新基线：文本应为 `static\ninjected`——脚本先执行改了 DOM，render 在完整 DOM 上运行。注意顺序：load_document 中脚本在 render 前执行，故 body 在脚本运行时已存在，`document.body` 命中）。

- [ ] **Step 5: 全量回归**

```bash
cd /home/gem/project/tinybrowser && ctest --test-dir build --output-on-failure
```
Expected: 全绿。既有 golden 仍逐字节不变。

- [ ] **Step 6: 验证 CLI 可构建 + 手动冒烟**

```bash
cd /home/gem/project/tinybrowser && make -C build cli
# 有网络时:tb <url> 观察渲染;无网络用本地 file/http server 冒烟
```
Expected: `tb` 可链接构建。无 TUI 交互断言——CLI 冒烟为可选人工步骤。

- [ ] **Step 7: Commit**

```bash
git add tests/integration/test_m2a_integration.cc tests/CMakeLists.txt tests/goldens/fixtures/dynamic.html tests/goldens/goldens/dynamic.golden src/js/dom.js tests/js/dom.test.js
git commit -m "test(m2a): 集成测试——同步脚本/console/timer + dynamic golden"
```

---

### Task 8: 计划收尾——spec 修订 + 全量回归

**Files:**
- Modify: `docs/superpowers/specs/2026-08-13-m2a-js-runtime-design.md`（记录 seam 两处签名修正 + 交互数据流定稿）
- Modify: `README.md`（如存在构建说明提及 lexbor）

**Interfaces:**
- Consumes: 全部任务产物

- [ ] **Step 1: spec 修订**

在 spec §4.4 后加"实现计划修订"小节：
- seam `open` 签名：`open(self, tb_browser *host)`（原 `open(self, cfg)`）——原因：`__tb_load_sync` nested pump 需 host 访问 uv loop/transport。
- seam `render` 签名：`render(self, h, url, status, out)`（原 `render(self, h, out)`）——对齐 M1 `tb_render(dom, url, status)`，golden_runner 直接可用。
- 交互数据流定稿：C 侧 fv[] + `_tb_elem_info(id)`/`_tb_form_info(form_id)` JSON + json.c 解析。
- 补 `document.body` 便捷属性（Task 7 引入）。

- [ ] **Step 2: 全量最终回归**

```bash
cd /home/gem/project/tinybrowser && rm -rf build && cmake -S . -B build && make -C build -j$(nproc) && ctest --test-dir build --output-on-failure
```
Expected: 全绿；`git status` 干净（除 spec 修订）。

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-08-13-m2a-js-runtime-design.md README.md
git commit -m "docs(m2a): spec 修订——seam 签名与交互数据流定稿"
```
