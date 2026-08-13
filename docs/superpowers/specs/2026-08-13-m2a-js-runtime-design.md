# tinybrowser — M2a JS 运行时(DOM in JS)设计文档

- 日期:2026-08-13
- 状态:草案(待 review)
- 语言:中文
- 定位:承接 `2026-08-11-tinybrowser-design.md` 的 §9(JS Runtime)、§10(QuickJS)与 §17(里程碑 M2);**取代**此前已批准的"lexbor C parse → 转换进 JS"方向,改为纯 JS 解析。前置:2026-08-12 TUI 已就绪。

---

## 1. 目标与非目标

### 1.1 目标

- **兑现"跑在 JS runtime 里的浏览器"**:DOM 完全在 JS——HTML 用手写 JS 宽松解析器(流式接口)解析为 JS 对象树;C 侧不再有 DOM 表示。
- **脚本同步执行**:document order;parser 遇 `<script>` 阻塞执行后再继续 tokenize;inline 直接 eval,`src` 走同步桥。
- **DOM 子集 + 交互**:脚本可查询/修改 DOM;`tb_click/fill/select/submit` 经 JS 查询落点。
- **渲染迁移到 JS**:`render.c` 的 walk 语义(空白折叠、block 换行、元素 off/id、select options、title)以 JS **迭代**遍历等价实现,golden 逐字节验证。
- **QuickJS 嵌入**:每 document 一个 context,与 document 同生共死。
- **C 桥面更薄**(比原计划):无 node bridge 双向通道;只有五类口——console、timer(set/cancel)、cookie(读/写)、load_sync、eval_js(§4.1 共 7 个函数)。

### 1.2 非目标

- 不是完整 HTML5 规范解析器:tree construction 简化——宽松 XML-style tokenizer + 近似 HTML 容错(契约见 §3.2)。
- 不做完整 CSS 引擎:selector 仅 `tag` / `.class` / `#id` / `tag.class`。
- 不做 fetch / EventSource / WebSocket / URL / TextEncoder 等 web platform 面(M3+ 经 C 桥补)。
- 不区分 defer/async:M2a 全部同步执行。
- 不做 layout/style;脚本修改 DOM 后的**重渲染线**归 M2b(本里程碑最终 render 在 load complete 后一次性做)。

---

## 2. 架构总览与数据流

### 2.1 分层

```
frontend (tb TUI)
   ↓  public C API (src/tb.h)
core (browser.c / session.c / view.c)
   ↓  js_engine seam (tb_js_engine)
JS context (per-document, QuickJS)
   ├── parser.js   — 宽松 tokenizer(流式接口)
   ├── dom.js      — DOM 子集(查询/构造/属性)
   └── render.js   — 文本视图 walk(迭代)
   ↓  5 个 C 桥
C bridges: __tb_console / __tb_timer_set·cancel / __tb_cookie / __tb_load_sync
```

与 M1 的分层图相比,唯一的替换:原 `dom.h/dom.c(lexbor)` 与 `render.c` 整体下沉进 JS context。transport / clock / js_engine 三个 seam 并列在 `tb_config`(§15 原设计)。

### 2.2 加载数据流

```
1. tb_navigate → session.nav_start → transport->open(req)
2. on_body    → nav_ctx.body 缓冲(append,不改线)
3. on_done    → 内容分类(content_type / attachment)
4. TB_CONTENT_RENDER:
   a. 释放旧 document(旧 JS context + 旧 tb_view + timer 队列)
   b. 创建新 JS context(QuickJS runtime 复用,context 新建)
   c. 注入 C 桥 + 内建 JS(parser.js / dom.js / render.js;M2a 无 web platform polyfill,见 §1.2)
   d. tb_load_document(body) →
        feed(一次性) → tokenize → 对象树 →
        同步执行脚本(§2.3)→ readyState = "complete"
   e. tb_render_js() → { text, elems[], title, url, status }
        → C 侧 tb_view_from_js 填充 tb_view
   f. session.nav_done / on_title / on_view_changed(与 M1 相同)
5. 非渲染内容 → 同 M1:tb_view_new + is_not_renderable
```

**判断①(流式落点,显式):** parser 接口是**真流式**(增量状态机 `feed(chunk)`,§3.1);但 M2a 接线先缓冲 body、load complete 后一次性 feed。理由:`on_body` 在 poll 同步栈内触发,若在其中 feed,遇 `<script>` 需 nested pump,形成传输回调栈内的 reentrancy,复杂且易错;且文本浏览器 render 在 load complete 后进行,首屏渐进收益≈0。未来若做首屏渐进渲染,只需把 `on_body` 接线从 append 改为 feed——parser 接口与 seam 均不改。

### 2.3 脚本执行位置与计时器

- 插入点 = M1 `nav_on_done`(browser.c:83 的 `b->view = tb_render(...)`)之前:parse + 脚本执行在 view 生成前完成。
- 脚本里 `__tb_timer_set` 的 timer 进入 per-document timer 队列;**不**立即执行;由 `tb_pump` 每轮检查到期,在宿主线程触发回调(§4.1)。因此文档加载期间注册的 timer 会在后续 pump 中执行。
- 加载完成后的重渲染线(脚本改 DOM → view 更新)M2b 实现;M2a 中脚本在加载期执行,其最终 DOM 状态一次性进入 render。

### 2.4 迁移边界(C 侧删改)

| 删除 | 替换 |
|---|---|
| `src/core/dom.h` / `dom.c`(`tb_dom_*` lexbor 封装) | 删除;DOM 全在 JS |
| `deps/lexbor` | 从 deps/ 移除,VERSIONS.lock 更新 |
| `src/core/render.c` 的 walk 逻辑 | 迁移为 `render.js`(迭代),golden 验证等价 |
| `browser.c` 的 `find_node_by_id` / `node_in_subtree` / `find_containing_form` / `collect_form_pairs` | 改为 JS context 内查询(`querySelector` 或 render 输出 elems) |

保留不变:M1 的 `view.c / session.c / url.c / content.c / curl_transport.c / tb.h` 交互 API 签名。

---

## 3. parser.js:宽松 HTML tokenizer(流式)

### 3.1 接口(真流式)

```js
var _tb_parser = {
  feed(chunk) { /* 增量消费;内部保存 position + 未闭合 token 缓冲 */ },
  finalize()  { /* 冲刷末尾文本 */ },
  root, doc, readyState, errors, warnings
};
```

- `feed()` 不要求完整输入:跨 chunk 的 tag / attr / 注释 / script raw text 都续接。
- `finalize()` 处理 EOF(未闭合 tag → 记录 warning 并关闭)。
- 接口真流式;M2a 接线一次性 `feed`(判断①,§2.2)。

### 3.2 宽松规则(契约)

- **void 元素自动自闭合**:`br img input hr meta link`(M1 render 已按此处理,契约一致)。
- **unquoted attributes**:`a href=/x class=y` 接受;值可被空白 / `>` 终止。
- **隐式闭合**:省略闭合标签的 start tag 先关闭未闭合的同名元素——`<p>一<p>二` → 两个兄弟 `p`;`<li>`、`<td>` 同理(仅限小集合:p li td tr th option)。
- **mis-nesting 宽容关闭**:`<b><i>x</b>y` → 不抛错,按最近未闭合元素尽力配对;记录 warning。
- **HTML entity 解码**:`&amp; &lt; &gt; &quot; &apos; &nbsp;` + `&#NN;`(数字) + `&#xHH;`;未知实体保留字面。
- **script/style raw text**:内容不作解析,原样进 text(含 `<` 与 `&`);以 `</script` / `</style` 终止。
- **boolean 属性 = 空串值**:`<input disabled>` → `attrs.disabled === ""`。
- **comment / DOCTYPE / PI 跳过**。
- **畸形输入**:记录 `warnings` + console warning,继续(不 abort;错误处理 §6)。
- **节点形态**:
  ```js
  { type:"element", tag, attrs:{}, children:[], text, id, parent }
  { type:"text",    text, id, parent }
  ```
  `id` parse 期间递增分配(M1 `tb_dom_id` 语义);root 为 `{ type:"root", children:[], id:0 }`。

### 3.3 同步脚本执行

- parse 到 `<script>` → 收集 raw text → parser 暂停 → 执行:
  - **inline** → 该 document context 内 eval;语法错误 → console + warning,继续。
  - **src** → C 桥 `__tb_load_sync(url)`(nested pump,§4.2)→ eval;失败 → warning,继续。
- 每 script 独立 try/catch 隔离;`js_exec_ms_limit` interrupt 超时 → abort **该 script**(已执行副作用保留),继续 parse。

---

## 4. C 桥、DOM 子集与 JS 引擎接线

### 4.1 桥面(per-document context 注入的全局函数)

| 桥 | 签名 | 行为 |
|---|---|---|
| `__tb_console` | `(level, msg)` | 转发 `tb_config.on_console`;level: `log\|warn\|error` |
| `__tb_timer_set` | `(ms, cb)` → id | per-document timer 队列,宿主线程 pump 到期触发 |
| `__tb_timer_cancel` | `(id)` | 取消 |
| `__tb_cookie` | `()` → str | host cookie jar 读(字符串往返) |
| `__tb_cookie_set` | `(str)` | jar 写 |
| `__tb_load_sync` | `(url)` → body\|"" | 同步子资源加载(§4.2) |
| 宿主 API | `tb_eval_js(b, code, &out)` | 宿主侧执行 JS 的入口(测试/调试) |

**无 node bridge**——不暴露 JS 对象到 C、也不让 C 直接读写 JS 属性;交互全部经 render 输出 + 查询 API(§4.3)。

### 4.2 判断②:script src 同步 fetch

- `__tb_load_sync` 内部:发起新 transport request(nested op),nested pump(复用 `tb_pump` 的循环体:uv_run NOWAIT + transport->poll + 到期 timer 派发)直到该 op `on_done`。
- 失败 → 返回空串 + warning,**不阻塞导航本身**(文档继续 parse,该脚本视为无内容)。
- 仅文档加载期间可用;加载完成后的 JS 无同步 fetch 路径(M2a 也不提供异步 fetch)。

### 4.3 DOM 子集(判断③)

- **查询**:`querySelector` / `querySelectorAll` / `getElementById`;selector 仅 `tag` / `.class` / `#id` / `tag.class`(文档序,深度优先)。
- **文档级**:`tb_doc.title`(rw,回写 `<title>`)、`tb_doc.readyState`(ro:脚本执行期间 "loading",全部脚本执行完 → "complete")、`tb_doc.documentElement`。
- **构造**:`document.createElement(tag)` + `node.appendChild` / `node.removeChild` + `node.textContent`(rw)。**脚本修改自然进入最终 render**(render 在加载期脚本全部执行后做)。
- **属性**:`getAttribute` / `setAttribute` / `removeAttribute`;`element.tagName` / `attrs` / `children` / `parent` / `id` / `textContent`。
- **`innerHTML` 只读**(re-serialize 子树)。
- **`document.cookie`** rw(桥到 host jar)。
- **`addEventListener(type, cb)`**:M2a 存储 + 触发基础设施(deliver 到宿主 pump),完整事件系统归 M3。
- **无**:style / classList / dataset / client* / 布局相关。

### 4.4 tb_config 扩展与 js_engine seam

`src/tb.h` 新增:

```c
int64_t js_memory_limit;               /* JS_SetMemoryLimit 字节;0 = 引擎默认 */
uint32_t js_exec_ms_limit;             /* JS_SetInterruptHandler,每 script 毫秒;0 = 无限 */
void (*on_console)(tb_browser *, const char *level, const char *msg, void *ud);
const struct tb_js_engine *js_engine;  /* seam;NULL = 默认 QuickJS-ng */
tb_err tb_eval_js(tb_browser *, const char *code, char **out); /* *out = tb_free */
```

`tb_js_engine` seam(仿 `tb_transport` 形态):

```c
struct tb_js_engine {
  /* open:建 context;load_document:喂 body + 执行脚本;render:产出 {text,elems};*/
  void *(*open)(const struct tb_js_engine *self, const struct tb_config *cfg);
  int   (*load_document)(const struct tb_js_engine *self, void *h, const char *body, size_t len);
  int   (*render)(const struct tb_js_engine *self, void *h, struct tb_view *out);
  int   (*eval)(const struct tb_js_engine *self, void *h, const char *code, char **out);
  void  (*poll_timers)(const struct tb_js_engine *self, void *h);  /* tb_pump 每轮调用 */
  void  (*close)(const struct tb_js_engine *self, void *h);
};
```

- 每 document 一个 JS context;导航创建,导航离开释放(含 timer 队列清理)。QuickJS **runtime** 进程级单例复用(browser 实例持有),context 按 document 新建。
- quickjs-ng 构建沿用现有 CMake(build/qjs);**lexbor 从 deps/ 移除**。

---

## 5. 渲染迁移(render.js)

### 5.1 语义等价目标(M1 `render.c` 契约)

walk 语义逐条保留,golden 验证:

- 跳过 `script / style / noscript / head / title` 的文本(`title` 文本由 extract_title 单独取)。
- `a[href]` → link;`input[type=hidden]` 跳过;`input[type=submit|button|reset]` → button;其他 input → input;`button` → button;`select` → select 并收集 option 文本;`form` → form;**`option` 文本不进正文**(仅入 select.options)。
- block 标签前后补换行(集合同 `render.c is_block_tag`);空白折叠:行首空白、连续空白、行尾空白、折叠规则逐条对齐。
- 元素 `off` = 当前已写缓冲长度(文本 push 前);`id` = parse 期分配的 id。
- `title`:head 下第一个 `title` 文本。
- 首尾空行裁剪;`off` 钳制到 `[0, nl]`(空/全空白页面时 M1 的 size_t 下溢防护)。

### 5.2 迭代遍历(硬性约束)

- JS walk **必须迭代**(显式栈),不得递归——QuickJS 栈限制,深 DOM 递归触发 RangeError stack overflow(已实测:10k 深度递归崩;迭代 ~0.35µs/node,10k 节点 walk ≈ 3.5ms)。
- `render.js` 输出:
  ```js
  { text, elems:[{id,type,text,href,name,value,options:[],off}], title, url, status }
  ```
  C 侧 `tb_view_from_js` 逐字段填充 `tb_view`(字符串 strdup,options 数组展开为 `e->options`)。

### 5.3 golden 等价

- 既有 fixtures(`hello.html` / `forms.html` / `nav.html`)golden **逐字节一致**(M1 生成文本 ↔ render.js 输出)。
- 新增 JS 侧 fixtures:实体解码、隐式闭合、boolean attr、script raw text、脚本改 DOM 后 render。

---

## 6. 错误处理

| 场景 | 处理 | 返回/上报 |
|---|---|---|
| parse 畸形(未闭合 tag / mis-nest) | 记录 + console warning,继续 | 不失败 |
| script 语法错误 | console + warning,继续 | 不失败 |
| script 超时(`js_exec_ms_limit`) | abort 该 script,继续 parse | 不失败 |
| script src 加载失败 | `__tb_load_sync` 返回空,继续 | 不失败 |
| 内存超限(`js_memory_limit`) | 该 context 判定失败,文档失败 | `TB_ERR_PARSE` |
| `tb_eval_js` 宿主调用失败 | 返回错误码 | `TB_ERR_PARSE`(或新码) |
| 空 / 全空白 body | render 空文本,off 钳 0 | 正常 |

与 M1 的 `TB_ERR_PARSE` 语义一致:仅"DOM 不可构建"才 fail(目前 M1 中 parse 永不失败,lexbor 宽容;JS 侧同样宽容,内存超限是唯一硬失败路径)。

---

## 7. 测试策略

| 层 | 测什么 | 手段 |
|---|---|---|
| parser 单元 | tokenizer 契约(宽松/实体/raw/隐式闭合/boolean/mis-nest/畸形) | JS 单测:quickjs 执行 parser.js,断言树结构 |
| DOM 子集 | querySelector / 构造 / 属性 rw / cookie / title | JS 单测 |
| render 等价 | render.js ↔ M1 golden | 既有 fixtures 逐字节 |
| 桥面 | console / cookie / timer / load_sync | gtest + fake transport + 真 QuickJS |
| seam | js_engine 可注入 dummy | gtest:注入 dummy 断言不崩溃、文档不执行 |
| 集成 | 全栈(QuickJS+curl+uv+mbedtls) | 内嵌 HTTP server:脚本导航 / 改 DOM / 表单 |

## 8. 里程碑与迁移边界

- **M2a(本 spec)**:DOM in JS、parser.js、dom.js、render.js(golden 等价)、脚本同步执行、5 口 C 桥、`tb_config` 扩展、删除 lexbor 与 dom.c。
- **M2b**:重渲染线(脚本改 DOM 后 view 更新)、表单状态迁 JS、cookie jar 持久化、异步 fetch。
- **M2c**:事件系统交付、加载/进度状态、错误页。
- **实现顺序**(计划细分):① parser.js + 单测 → ② dom.js 子集 + 单测 → ③ render.js + golden 等价 → ④ 桥面 + `tb_config` → ⑤ `browser.c` 接线 + 删 lexbor → ⑥ 集成测试。
