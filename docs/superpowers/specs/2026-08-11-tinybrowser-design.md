# tinybrowser — 可嵌入的 Agent 文本浏览器 设计文档

- 日期:2026-08-11
- 状态:设计定稿,待评审
- 语言/标准:C99(`-std=c99 -Wall -Wextra`)
- 定位:一个 **可方便集成到其他程序**、**面向 AI agent**(文本观察 + 动作空间)、**Web1.0 式可交互文本** 的浏览器内核(类 Lynx),不追求视觉效果。

---

## 1. 目标与非目标

### 目标
1. **可嵌入**:以静态库 `libtinybrowser.a` + 一组纯 C API(`browser_*`)提供,宿主完全掌控线程与生命周期。
2. **Agent 浏览器**:输出是**结构化文本观察**(标题/URL/状态/正文文本/带稳定 ID 的交互元素表);输入是一组**动作命令**(goto/click/fill/submit/…)。
3. **单线程、零全局状态**:见 §5。
4. **指纹与主流浏览器一致**:UA 与浏览器指纹对齐 Chrome(JS 侧 + HTTP 侧)。
5. **零系统依赖**:所有依赖源码 vendored(§14),不装任何系统包;**不使用 OpenSSL**。
6. **可测试的代码**:seam、纯核、确定性、失败注入(§15)。

### 非目标(明确不做)
- ❌ 视觉效果 / 图形渲染 / 截图。文本观察是唯一输出形态。
- ❌ 文件下载(非文本内容不落盘,只报告"不可渲染")。
- ❌ 依赖 OpenSSL / BoringSSL。
- ❌ 自动通过人机验证(CAPTCHA):只做**检测 + 策略 hook**,不做自动破解(§12)。
- ❌ 反向"消费 CDP 去驱动 Chrome":只做**对外提供 CDP 服务端**(§13.3)。
- ❌ TLS 1.0/1.1(默认关闭;mbedtls 3.x 原生仅 1.2/1.3,与 Chrome 一致)。
- ❌ HTTP/3(延后,§6.4,可选构建项)。
- ❌ 多线程共享实例、多实例共享线程。

---

## 2. 架构总览

```
                    ┌────────── 前端(frontends)──────────┐
                    │  tb (TUI/termbox2) │ tb-mcp (stdio  │
                    │  JSON-RPC) │ tb-cdp (WS server)     │
                    └──────────────────┬──────────────────┘
                                       │ 纯 C API (browser_*)
                    ┌──────────────────▼──────────────────┐
                    │  core: session / text-render /      │
                    │        interaction / js-runtime /   │
                    │        dom / network / fingerprint / │
                    │        challenges                   │
                    └──────────────────┬──────────────────┘
                                       │ 依赖(全部 vendored)
                    ┌──────────────────▼──────────────────┐
                    │  libuv(事件循环) · libcurl(multi)    │
                    │  · lexbor(HTML5+DOM) · quickjs-ng    │
                    │  (JS) · mbedtls(TLS) · nghttp2(H2)   │
                    └─────────────────────────────────────┘
```

- **单进程、单线程、单事件循环**。唯一驱动是宿主调用的 `tb_pump()`(§5)。
- 前端只是引擎能力的**不同序列化**:tb(tty 人用)、tb-mcp(agent 通过 stdio JSON-RPC 用)、tb-cdp(现有 CDP 工具链用)。三者共享同一 core。

---

## 3. 组件分解

每个组件:职责 / 接口 / 依赖,边界清晰、可独立理解与测试。

### 3.1 core/network — 网络层
- **职责**:libcurl-multi 异步传输、cookie、重定向、gzip、TLS(经 mbedtls)、内容类型分发。
- **接口**:内部 `tb_transport`(见 §15 seam);对外暴露导航/加载事件给 session。
- **依赖**:libuv、libcurl、mbedtls、(nghttp2)。
- **内容类型分发**(无下载):
  - `text/html`、`text/plain`、`application/xhtml+xml` 等 → 解析渲染;
  - 其他(`image/*`、`application/pdf`、`application/octet-stream`…)→ **不解析不落盘**,观察中报告「不可渲染:content-type=…,URL=…」,不保留 body。

### 3.2 core/html — 解析与 DOM
- **职责**:HTML5 容错解析 → DOM 树 + CSS 选择器能力。
- **接口**:`tb_dom_*`(get document / query selector / iterate)。
- **依赖**:lexbor(HTML5 parser + DOM + selectors,纯 C)。

### 3.3 core/render — 文本渲染(纯函数)
- **职责**:DOM → 结构化文本视图(§7)。
- **接口**:`tb_view tb_render(const tb_dom*)`;**无 I/O、无时钟**,可被 golden 快照直接断言。
- **依赖**:仅 DOM。

### 3.4 core/session — 会话与状态机(纯逻辑)
- **职责**:历史栈(back/forward)、当前 document、挂起请求计数、导航状态机、元素 ID 分配。
- **接口**:状态查询/驱动原语;**无 I/O**,可直接驱动断言。
- **依赖**:无(操作由上层注入)。
- **元素 ID 规则**:ID 绑定 **DOM 节点身份**(插入时分配,节点存活期间跨重渲染不变),不随渲染顺序漂移。

### 3.5 core/js — JS 运行时
- **职责**:QuickJS(quickjs-ng)上下文管理 + Web 平台 API 绑定 + 任务调度。
- **接口**:`tb_eval_js(code, out)`;内部 fetch/timers/console/EventSource/WebSocket 绑定。
- **依赖**:quickjs-ng、network(reachout)、render(观察)。
- **生命周期**:一个 document 一个 JS 上下文,随 document 生灭,无跨文档状态,无全局 ctx。

### 3.6 core/fingerprint — 指纹模块
- **职责**:单一配置源,同时驱动 **JS 侧**(navigator/window 属性 shim)与 **HTTP 侧**(默认请求头),见 §11。
- **接口**:`tb_fingerprint_apply(config, js_ctx)` / `tb_fingerprint_headers(config)`。
- **依赖**:quickjs(应用 shim)、network(注入头)。

### 3.7 core/challenges — 挑战检测与策略
- **职责**:检测人机验证/反爬页面,触发策略 hook(§12)。
- **接口**:`on_challenge` 回调 + 策略枚举(retry-with-backoff / abort-with-error / queue-for-human)。
- **依赖**:DOM(检测标记)、session。

### 3.8 frontends — 三个前端
- `frontends/cli/` **tb**:termbox2 TUI,分页展示文本视图 + 交互元素列表 + 输入处理(§13.1)。
- `frontends/mcp/` **tb-mcp**:stdio JSON-RPC,把动作空间映射为 MCP 工具(§13.2)。
- `frontends/cdp/` **tb-cdp**:WebSocket server,对外提供 CDP 子集(§13.3)。

---

## 4. 数据流

1. 宿主调 `tb_navigate(url)` → session 记历史 → network 发请求(libcurl-multi)。
2. 响应头到达 → **内容类型分发**:可渲染 → body 交给 lexbor 解析成 DOM;不可渲染 → 记录"不可渲染"状态,丢弃 body。
3. DOM 就绪 → 建 JS 上下文(仅 M2+)→ 渲染纯函数产出文本视图。
4. 宿主在回调(`on_view_changed`)或下次 `tb_observe` 拿到视图。
5. 宿主调 `tb_click(id)` / `tb_fill(id, v)` / `tb_submit(form_id)` → interaction 层映射到 DOM 动作 → 可能触发 JS 或新导航。
6. 一切推进都发生在 `tb_pump()` 内(§5);`tb_wait_idle()` 是"pump 到空闲"的便捷封装。

**空闲判定(wait_idle)**:无在途 HTTP 请求 **且** 在 `idle_grace_ms`(默认 300ms)内无新网络请求;JS 定时器仅在其触发网络请求时才阻塞。

---

## 5. 线程模型与状态规则(硬约束)

1. **唯一驱动 = `tb_pump()`**。不存在后台线程;不设 `pump_mode` 配置(单一模型)。回调在宿主线程内**同步**触发,零锁、零消息队列。
2. **一实例一线程**:`tb_browser` 不跨线程使用;每个线程至多一个实例;实例间不共享任何状态。API 无需线程安全,全树无 mutex。
3. **零全局可变状态**:uv loop、curl multi、quickjs ctx、DOM、历史栈、fingerprint profile、challenge 状态……全部挂在 `tb_browser*` 下。禁止任何 `static` 可变状态;只允许 `const` 静态表(指纹头清单、content-type 表之类)。
4. **可复入**:同一线程内多个实例串行可用(各自独立),无共享单例。
5. 验证手段:编译期/审查规则 + 测试中并发创建多实例断言隔离。

---

## 6. 网络与协议

### 6.1 传输
- libcurl-multi 异步;cookie 走 curl cookie jar;重定向跟随并记录最终 URL;gzip 解压。
- **TLS:mbedtls 是唯一后端,不使用 OpenSSL**。libcurl 以 `--with-mbedtls` 构建;构建时禁止 `USE_OPENSSL`(配置到 OpenSSL 则 configure 硬失败)。

### 6.2 协议演进(见里程碑 §17)
| 能力 | 方案 | 阶段 |
|---|---|---|
| HTTP/1.1 | curl 原生 | M1 |
| TLS 1.2 / 1.3 | mbedtls 3.x 原生;**TLS 1.1 默认关** | M1 |
| SSE | JS `EventSource` 实现:长连接 + 流式写回调 + 事件循环驱动 | M2 |
| WebSocket ws/wss | libcurl 8.x 自带 WS API(`curl_ws_*`),同栈同 cookie,wss 走 mbedtls | M2 |
| HTTP/2 | vendored **nghttp2** + curl `--with-nghttp2` + mbedtls ALPN(h2 over TLS) | M2 |
| gRPC | 浏览器语境 = **gRPC-Web**:fetch + `application/grpc-web` content-type 即覆盖,原生 gRPC(trailers)浏览器侧不存在 | 免费 |
| HTTP/3 | **延后**(QUIC 需要 QUIC-TLS,mbedtls 不支持;留作 M2 之后的可选构建项,届时在 GnuTLS/BoringSSL 中定夺) | 延后 |

### 6.3 内容类型分发
见 §3.1。仅文本类渲染;非文本报告不可渲染,不落盘。

---

## 7. 文本视图与 Agent 观察

`tb_observe` 输出结构化快照(JSON 兼容):

```json
{
  "url": "https://example.com/a",
  "title": "A",
  "status": 200,
  "text": "…线性化正文…",
  "elements": [
    { "id": 3, "type": "link",     "text": "About", "href": "/about" },
    { "id": 4, "type": "input",    "name": "q", "value": "" },
    { "id": 5, "type": "button",   "text": "Go" },
    { "id": 6, "type": "select",   "name": "lang", "options": ["en","zh"] }
  ],
  "not_renderable": null,
  "console": []
}
```

- `elements[].id`:稳定 ID,绑定 DOM 节点身份(§3.4),agent 凭 ID 动作。
- `not_renderable`:非文本内容时报告 `{"content_type": "...", "url": "..."}`。
- 渲染纯函数决定一切,可 golden 快照比对(§16)。

---

## 8. 动作空间(命令集)

| 命令 | C API |
|---|---|
| 导航/历史 | `tb_navigate` / `tb_back` / `tb_forward` / `tb_reload` |
| 观察 | `tb_observe` |
| 交互 | `tb_click(id)` / `tb_fill(id, value)` / `tb_select(id, option)` / `tb_submit(form_id)` |
| JS | `tb_eval_js(code, out)`(malloc,`tb_free`) |
| 等待/驱动 | `tb_wait_idle(timeout_ms)` / `tb_pump(timeout_ms)` |
| 生命周期 | `tb_create` / `tb_destroy` |

---

## 9. C API 草案

```c
typedef struct tb_browser tb_browser;
typedef struct tb_view tb_view;   // §7 快照(内存由 tb_observe 分配,tb_free)
typedef struct tb_err { int code; char msg[128]; } tb_err;

typedef struct tb_config {
  const char *user_agent;          // 默认:与主流 Chrome 对齐(§11)
  int64_t     js_memory_limit;     // bytes
  uint32_t    js_exec_ms_limit;    // 单任务预算
  uint32_t    idle_grace_ms;       // wait_idle 宽限,默认 300
  uint32_t    nav_timeout_ms;
  // 回调(均在线程内、tb_pump 期间同步触发)
  void (*on_idle)(tb_browser*, void *ud);
  void (*on_view_changed)(tb_browser*, void *ud);
  void (*on_console)(tb_browser*, const char *level, const char *msg, void *ud);
  void (*on_error)(tb_browser*, tb_err, const char *detail, void *ud);
  void (*on_title)(tb_browser*, const char *title, void *ud);
  void (*on_challenge)(tb_browser*, const char *kind, tb_challenge_policy*, void *ud);
  void *ud;
  // 测试 seam(默认均指向真实实现,§15)
  const struct tb_transport *transport;  // 默认 curl 实现
  const struct tb_clock    *clock;       // 默认真实时钟
  const struct tb_js_engine *js_engine;  // 默认 quickjs 实现
} tb_config;

tb_browser *tb_create(const tb_config*);
void        tb_destroy(tb_browser*);
tb_err tb_navigate(tb_browser*, const char *url);
tb_err tb_observe(tb_browser*, tb_view *out);      // 分配,tb_free
tb_err tb_click(tb_browser*, int id);
tb_err tb_fill(tb_browser*, int id, const char *value);
tb_err tb_select(tb_browser*, int id, const char *option);
tb_err tb_submit(tb_browser*, int form_id);
tb_err tb_back(tb_browser*); tb_err tb_forward(tb_browser*); tb_err tb_reload(tb_browser*);
tb_err tb_eval_js(tb_browser*, const char *code, char **out); // malloc,tb_free
int    tb_wait_idle(tb_browser*, uint32_t timeout_ms);
int    tb_pump(tb_browser*, uint32_t timeout_ms);
void   tb_free(void*);
```

- **C99**:库 target 用 `-std=c99 -Wall -Wextra`。
- **`extern "C"` 守卫**:`tb.h` 全量包裹,保证 C++ 宿主与 gtest 可链接。
- 头部只暴露上述 API;内部状态全部在 `tb_browser*`(私有实现,无全局)。

---

## 10. JS 运行时(QuickJS)

- **引擎**:quickjs-ng(vendored,活跃维护 fork);每 document 一个 context,与 document 同生共死。
- **Web 平台 API 面**(M2):`fetch`、`EventSource`(SSE)、`WebSocket`(ws/wss)、`setTimeout/setInterval`、`console.*`、DOM 操作子集。
- **约束**:JS 定时器仅当触发网络请求时阻塞 wait_idle(§4);单任务执行预算 `js_exec_ms_limit`;内存上限 `js_memory_limit`。
- 通过 `tb_js_engine` seam 可注入哑实现(§15)。

---

## 11. 指纹模块(tb_fingerprint)

**单一配置源**,同时驱动 JS 侧与 HTTP 侧,保证两面一致。

- **L1(JS 侧,v1 实现)**:`navigator.userAgent/platform/language/languages/hardwareConcurrency/deviceMemory/maxTouchPoints/plugins/userAgentData`、`webdriver=false`、`window.outerWidth/Height`、`screen.width/height/colorDepth/pixelDepth` — 全部对齐目标 Chrome 版本。
- **L2(HTTP 侧,v1 实现)**:`sec-ch-ua`、`sec-ch-ua-platform`、`sec-ch-ua-mobile`、`sec-ch-ua-full-version-list`、`sec-fetch-dest/mode/site/user`、`Accept`、`Accept-Language`、`Accept-Encoding`、`Priority`、`upgrade-insecure-requests`、`Cache-Control`。
- **L3(明确延后)**:TLS 层指纹(JA3/JA4,需 BoringSSL/curl-impersonate 路线)、canvas/WebGL/AudioContext。与"不用 OpenSSL"共同决定其延后;不承诺具体时间。
- **UA 默认值**:对齐主流 Chrome(如 `Chrome/126.x` 形态),由配置覆盖。

---

## 12. 人机验证:检测 + 策略,不是破解

- **检测**(`on_challenge`):DOM/响应头启发式 — recaptcha/hcaptcha/turnstile 标记、`cf-chl`、403+验证页正文、"Are you a human" 等。
- **策略 hook**(`tb_challenge_policy`):`retry-with-backoff` / `abort-with-error` / `queue-for-human`(宿主自行接入人工或外部系统)。
- **明确边界**:不做自动解答。现代验证码是风险评分的 ML 系统,无确定性答案;本架构是文本内核、QuickJS 非 V8,无法真实伪造深度环境探测。此边界如实声明,不因"必须无人"而承诺不可达之事(§18)。

---

## 13. 前端

### 13.1 tb(TUI)— termbox2
- 依赖 **termbox2**(vendored 单头文件,C99 友好,零依赖,不引入 terminfo 运行时依赖)。
- 职责:文本视图分页滚动 + 元素列表 + 键盘/鼠标输入;ANSI 输出。
- 说明:文本视图本身由 core 渲染,termbox2 只提供原始单元格 + 输入 + 缩放 + UTF-8(CJK 双宽)。

### 13.2 tb-mcp(stdio JSON-RPC)
- 首个集成层(M3)。把动作空间映射为 MCP 工具:goto/back/forward/reload/observe/click/fill/submit/select/eval_js/wait_idle。
- 线程模型:主线程读一行 JSON-RPC → 分发 → `tb_pump`;单实例。

### 13.3 tb-cdp(WebSocket server,CDP 子集)
- 定位:对外提供 CDP 服务端,让 Puppeteer/Playwright 等现有 CDP 工具链驱动本引擎(M5)。
- 传输:libuv 监听;HTTP `Upgrade: websocket` → WS 服务端(vendored **wslay**,RFC6455,纯 C;wss 走 mbedtls server 模式);普通 GET 走 `/json/list`(Chrome 风格目标发现页)。
- **CDP 子集**(对齐现有能力,单会话):
  | CDP 域 | 引擎映射 |
  |---|---|
  | `Page.navigate/reload/history` | tb_navigate/back/forward/reload |
  | `Runtime.evaluate` | tb_eval_js + `consoleAPICalled`/`exceptionThrown` |
  | `DOM.getDocument/querySelector` | lexbor DOM |
  | `DOMSnapshot.captureSnapshot` | 文本视图(tb_observe) |
  | `Accessibility.getFullAXTree` | 交互元素表(稳定 ID 的 AX 树) |
  | `Input.insertText/dispatchKeyEvent` | tb_fill/click/submit |
  | `Network.*` | 网络事件 + cookie |
  | `Emulation.setUserAgentOverride` | tb_fingerprint profile |
  | `Fetch`(请求拦截) | 可选,M4+ |
- **明确排除**:`Screenshot`、`Tracing`、`Performance`、`SystemInfo`;不做"全量 CDP schema"承诺 — 支持清单固定、如实声明。

---

## 14. 构建与依赖

### 14.1 依赖(git submodule,vendored 源码编译,零系统包,不用 OpenSSL)

| 子模块 | 用途 | 语言 |
|---|---|---|
| `deps/quickjs-ng` | JS 引擎 | 纯 C |
| `deps/lexbor` | HTML5 parser + DOM + selectors | 纯 C |
| `deps/libuv` | 事件循环 | 纯 C |
| `deps/libcurl` | 异步网络(multi),`--with-mbedtls` | 纯 C |
| `deps/mbedtls` | **唯一** TLS 后端(curl 链它) | 纯 C |
| `deps/nghttp2` | HTTP/2(M2) | 纯 C |
| `deps/termbox2` | tb TUI(单头文件) | C |
| `deps/wslay` | CDP 的 WS 服务端(M5) | 纯 C |
| `deps/googletest` | 单元测试框架(测试专用) | C++ |

### 14.2 构建系统
- **CMake** 实际驱动;顶层 **Makefile** 命令入口:

```
make init    # git submodule update --init --recursive
make         # = cmake configure + build(全部产物)
make test    # ctest(全部测试)
make mcp / cli / cdp   # 只 build 对应前端
make clean
```

- **C/C++ 边界**:库 target 坚持 C99;测试文件是 C++(gtest),include C 头文件(经 `extern "C"`);CMake 对测试 target 用 C++14。
- **产物**:`libtinybrowser.a`、`tb`、`tb-mcp`、`tb-cdp`、各测试二进制。
- **禁止项**:构建脚本若探测到 curl 配置指向 OpenSSL → configure 硬失败(`USE_OPENSSL=OFF` / `USE_MBEDTLS=ON` 强制)。

---

## 15. 可测性设计(一等原则)

1. **接缝(seam)优先**:transport / clock / js_engine 三个接口第一天就在 `tb_config` 里(默认真实实现)。测试注入 `fake_transport`(固定响应/错误码/延迟)、`fake_clock`(手动推进,不 sleep)、哑 JS。
2. **纯核分离**:渲染(DOM→视图)与会话状态机无 I/O、无时钟,直接断言;只有 network/js 经 seam 注入。
3. **确定性**:元素 ID 绑定节点身份;零全局可变状态(§5);golden 快照可复现。
4. **失败注入**:fake transport 产生 4xx/5xx/超时/乱序/断连,本地测 retry/backoff、导航超时、挑战检测。

---

## 16. 测试策略

| 层 | 测什么 | 手段 |
|---|---|---|
| 单元 | 状态机、渲染纯函数、指纹头、内容分发、ID 稳定性、失败注入 | gtest + seam(无 I/O) |
| 快照 | lexbor 解析真实 HTML → 文本视图 | golden 字节比对 |
| 集成 | 全栈(libuv+curl+quickjs+mbedtls) | 内嵌测试 HTTP server |
| 契约 | tb-mcp JSON-RPC 往返 | stdio 模拟 |
| CDP | 真实 CDP 客户端驱动引擎 | puppeteer-core 连 tb-cdp smoke(证明兼容性非自说自话) |
| 并发/隔离 | 多实例同线程隔离 | 并发创建实例断言 |

全部 headless,CI 友好。

---

## 17. 里程碑

- **M1 文本内核(无 JS)**:网络 + TLS1.2/1.3 + lexbor + 渲染 + 会话/历史 + 动作空间(除 eval_js)+ 内容分发 + gtest 骨架 + tb(TUI)。
- **M2 JS 运行时**:quickjs-ng 集成、`tb_eval_js`、console、定时器、`fetch`、SSE(EventSource)、WebSocket(ws/wss)、HTTP/2;指纹 L1/L2。
- **M3 MCP server**:tb-mcp(stdio JSON-RPC),动作空间全量映射。
- **M4 硬化**:挑战检测 + 策略 hook、跨页 ID 稳定性、失败注入全量、性能(渲染/内存)、CI 全绿。
- **M5 CDP 前端**:tb-cdp(WS + wslay + CDP 子集)、puppeteer-core smoke。HTTP/3 在此阶段之后作为可选构建项另行评估。

每阶段结束时:该阶段测试全绿 + golden 快照更新。

---

## 18. 安全与伦理边界

- **不做**自动通过人机验证:检测 + 策略 hook + 人工/外部接入口(§12)。
- **不做**反检测武器化:指纹一致性用于减少误报与正常自动化,不承诺绕过 ML 风控。
- 诚实声明:现代 CAPTCHA 无确定性解法;QuickJS ≠ V8,深度环境探测无法完全伪造;TLS 指纹(JA3/JA4)维护成本极高 → 均延后或不做。
- 所有"对齐主流浏览器"的指纹行为以**公开 UA/头**为基准,不依赖隐蔽漏洞。

---

## 19. 风险与开放问题

1. **HTTP/3**:QUIC-TLS 与"不用 OpenSSL"的最终取舍(GnuTLS vs BoringSSL)留待 M5 后评估;届时 mbedtls 管 HTTP/1.1+2 不变。
2. **CDP 子集漂移**:支持清单固定、如实声明;Puppeteer 等客户端对部分方法有前置调用(如 `Target.setDiscoverTargets`)需在 M5 兼容性测试中补齐。
3. **lexbor 与 quickjs 版本钉住**:submodule 钉 commit,升版走独立 PR。
4. **fingerprint 时效**:Chrome 版本迭代导致默认 profile 需随版本更新;profile 以配置为中心,升级即换配置。
5. **mbedtls 证书库**:系统 CA 不可用时需 vendored CA bundle(与"零系统依赖"一致的内部决策点)。
