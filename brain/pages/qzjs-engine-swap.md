---
id: qzjs-engine-swap
title: "JS 引擎迁移到 qzjs 子模块：架构与集成定案"
category: decision
status: active
tags: [js, qzjs, quickjs, engine, submodule]
created: "2026-10-04T14:05:26"
updated: "2026-10-06T09:54:13"
---

<!-- compiled_truth -->
---
id: qzjs-engine-swap
title: "JS 引擎迁移到 qzjs 子模块：架构与集成定案"
category: decision
status: active
tags: [js, qzjs, quickjs, engine, submodule]
created: "2026-10-04T14:05:26"
updated: "2026-10-06T04:27:03"
---

<!-- compiled_truth -->
## 结论:已落地并全绿(149/149;ASAN+UBSAN+leak 全新构建 149/149 零泄漏)

## 定位:qzjs 不是 QuickJS 的 drop-in,而是「邮箱 + 事件循环」运行时
- `adam-ikari/qzjs` = *Embeddable QuickJS-ng runtime with PAL*。JS 跑在**库自有线程 + 自有 uv loop** 上。
- 主权铁律(`include/qzjs/qzjs.h:130`):**qzjs 从不执行宿主代码**。公共 API 无函数指针字段,宿主↔库的
  全部通讯只有一条通道:入站 `qz_post_message` / `qz_control`,出站 per-rt FIFO 邮箱
  `qz_recv_message` / `qz_free_message`(唤醒 fd `qz_message_fd`,eventfd)。
- 因此「宿主直接拿到 `JSContext*` 同步 eval」这条路**默认不存在**——JSContext 归 qzjs 线程独占。

## 定案一:子模块(从自己 GitHub 克隆)
- `deps/qzjs` → `https://github.com/adam-ikari/qzjs.git`,现钉在 master `2020a598`(2026-10-06 更新,8 个提交:PR #8~#15 全部合入)。
- shallow clone(`--depth 1`)。qzjs 自身还有 8 个嵌套子模块;只初始化 4 个:
  `quickjs-ng` / `libuv` / `miniz` / `mbedtls`。
- **故意不初始化**:`wamr`(体量最大)、`wasm3`、`lz4`(仅 `QZ_POLYFILL_MODE=compressed` 需要,默认 `rodata`)、`googletest`。
- ⚠️ **`deps/qzjs/deps/mbedtls` 还有第三层嵌套子模块 `framework`(mbedtls-framework),必须初始化**。
- ⚠️ qzjs 走 squash merge,故本地跟踪的旧 SHA 不是 master 的祖先。**核对办法是逐项查内容,不能只看 SHA**。
- ⚠️ **`/home/gem/project/qzos/qzjs` 是同一上游的另一份 checkout**,它的本地提交多半未推送。
  **以 GitHub master 为准**,否则会把只存在于本地的改动误当成上游已有能力。

## 定案二:进程模型 = THREAD(单进程)
- `QZ_PROCESS_MODEL=THREAD`。ISOLATED 要随包部署 `qzjs-rt` 可执行档,对 TUI 浏览器是不必要的负担。
- 代价:拿不到跨进程 liveness ping。用 `qz_ping_if_available()` 探测,THREAD 下恒返回
  `QZ_PING_UNAVAILABLE(-2)`——**不谎报健康**。

## 定案三:同步 eval 走控制面 `op:"eval"`,不需要 `QZ_EXTRA_SOURCES`(推翻早先建议)
- 链路:`qz_control(rt, cmd)` → msgq → qzjs 线程 `qz_wake_cb` → `flags==QZ_MSG_FLAG_CONTROL`
  → `qz_control_dispatch` → `ctl_eval` **在 qzjs 线程上** `JS_Eval` → 回执入邮箱。
- 命令 `{"op":"eval","correl":"<id>","timeout_ms":T,"script":"<code>"}`;回执
  `{"ctl":true,"correl":"<id>","ok":1,"result":<val>}`;异常时 `ok:0` + `error` + `code`。
- `op:"inspect"` = 求值 + `JS_JSONStringify` → 回执 `json` 字段。**用它取代逐字段搬 `tb_view`。**
- `src/control.c` 是**无条件**编译的,只有 `control_endpoint.c`(LOCAL 档 uv_pipe 端点)是 ISOLATED-only
  → **THREAD 下控制面可用**。必须 `cfg.control_plane = QZ_CONTROL_IN_PROC`(默认 OFF 恒 -1)。
- `correl` 是回执**唯一**配对依据,缺/空/非字符串会被硬拒。唯一豁免是 `op:"interrupt"`。

## 定案四:删掉 tinybrowser 自带的 quickjs-ng / libuv / mbedtls
- 目标名冲突是硬阻塞:qzjs 产出与 tinybrowser **同名**的 target(`qjs`/`uv_a`/`mbedtls`+`mbedx509`+`mbedcrypto`)。
- qzjs 还要给它的 quickjs-ng 打 5 个补丁(C99 atomics / drain-jobs / bc-reader-hardening /
  debugger / libuv-c99-atomics),是工作树改动 → quickjs-ng **必须**来自 qzjs 子模块。
- 连带:libcurl 的 ExternalProject 改指 `deps/qzjs/deps/mbedtls`;`test_tls` 的 include 也改。

## 落地后的接口变更
- `tb_config.js_memory_limit` **已删除**(qzjs 无 `JS_SetMemoryLimit` 对应旋钮,用户确认接受失效)。
  OOM 现在表现为引擎自身抛 JS 异常 → eval 回执 `ok:0`。
- `js_exec_ms_limit` 语义改为「控制面回执窗口」,默认 5000ms,超时投递 `op:"interrupt"`。

## qzjs 更新到 `2020a598`(2026-10-06)
- `478e6e7` → `2020a598`,8 个提交(PR #8~#15)。**对我们真正相关的只有两条。**
- ✅ **issue #5(无 CA 注入点)已被解决**:新增 `qz_add_ca_pem(rt, pem)`。PEM **追加**在系统 CA
  之后 —— 只增加信任根,**绝不替换或削弱系统信任**;校验本身不变(恒 `VERIFY_REQUIRED` +
  主机名校验),**刻意不提供跳过校验的开关**(那会扩大攻击面)。
  - ⚠️ **THREAD 下直接生效;ISOLATED 下静默无效**(写的是父进程 rt,真正握手的是子进程),
    必须改走 `QZ_CA_FILE`。我们是 THREAD,不受影响。这是该 API 最值得记住的一条。
  - 符号已编进我们的 `libqzjs.a`,但**目前用不上**:tinybrowser 传输层是 libcurl 而非
    qzjs 的 http,页面 JS 也没有 fetch。接线 fetch 时这条才有意义。
- `qz_wait_idle` 的 worker 修复(#14)对我们**无影响**:worker 有独立 uv_loop,其内异步工作
  对父不可见 → wait_idle 提前 teardown。我们既不用 worker 也不调 `qz_wait_idle`。
- ⚠️ `qz_io_stream_ops_t` 的 `on_end` 多了第三参 `error_msg`(C 层失败原因透传到 JS,
  此前全被压成同一个 `error_status`)。**我们没接线 http_ops,故不受影响**——
  但接线 fetch 时这是接口变化,别按旧签名写。
- **我提的 PR #2 评审点(`qz_compile_js()` 里的 `$<TARGET_FILE:qjsc>` 交叉构建陷阱)仍在**,
  但上游在 #8 里把 `QZ_QJSC_HOST` / `QZ_LZ4_HOST` 两个出口**整个撤掉**了,理由见其
  `brain/pages/platform-support.md`:范围已定为 **Linux-only**(2026-10-03 拍板)——
  `qz_message_fd()` 用 Linux 特有的 eventfd、控制面用 `AF_UNIX`+`SO_PEERCRED`、io_uring
  须显式禁用,三条约束已让交叉构建没有真实消费者,为一个已声明不支持的场景留 54 行
  构建分支是净负债。
  **结论:上游选的是「不支持」而非「修好」。既无受支持的配置能走到那个陷阱,我的评审意见
  就此作废,不再重提。** 这是「范围决定压倒构建灵活性」的一个实例,值得记住的处理方式。

## 构建要点(踩过的坑)
- qzjs 自己的测试套件需要:`-DQZ_BUILD_TESTS=ON -DQZ_PROCESS_MODEL=THREAD -DQZ_PROFILE=minimal
  -DQZ_PROFILE_LAST=minimal -DQZ_WITH_TLS=ON -DQZ_WITH_WAMR=OFF`。**单设 `QZ_WITH_WAMR=OFF` 会被
  qzjs 自己的 profile-switch 块静默覆盖。**
- `gh pr list`(GraphQL)会超时;用 `gh api repos/adam-ikari/qzjs/...`(REST)。

## 实现期踩到的 bug(都是「测试看着像引擎坏了,其实是自己写错」)
(略)

## 子资源抓取(`<script src>`)—— 集成测试挖出的四个 bug
(略)

## 导航生命周期:两段式(定稿)
- `nav_commit_ready` 的引擎工作必须在 **`tb_pump` 顶层**,绝不能放进传输回调(那是 curl 重入 UB)。
- `tb_pump` 四步固定顺序:① 轮询传输 → ② `poll_timers` → ③ 排空 JS 邮箱(nav 指令 + cookie 写)→ ④ 提交就绪导航。

## UB 类修复:必须证明测试能变红
- 方法固定为 `tools/verify_fixes.sh`:备份 → 逐条破坏 → 构建 → 跑指定测试 → 断言「真的红了」→ 恢复。
  **「仍然全绿」本身就是结论**,说明那条测试没钉住修复,要么补测试要么删掉不可观察的逻辑。

## 测试替身比被测物更宽容 = 盲区(方法论教训)
(略)

## 测试分层(定稿)
(略)

## ⚠️ boot 脚本是 C 字符串拼接:字面量里不能有 `//`(已加门禁)
- `TB_BOOT_SRC` 是一长串相邻 C 字符串字面量,**C 拼接它们时不换行**。于是字面量内部的一个
  `//` 行注释会把**后面的字面量整段吃掉** —— C 编译器毫无察觉,boot 脚本却少了一行,
  表现为 `qz_create failed (abi 1)`,而 abi 版本照常打印,**完全看不出原因**。
- 本轮就被这个坑了一次:三处 `//` 注释吃掉相邻语句,`qz_create` 直接失败,排查方向一度跑偏
  (怀疑引擎 ABI、怀疑自己新增的 MIME 表里有裸 `/` 键 —— 后者其实是误判,键本来已加引号)。
- **门禁:`tools/check_boot_js.py`** —— 从 js_engine.c 抽出拼接后的 boot 源码与 `src/js/*.js`
  一起交给 `node --check`,并额外警告字面量内的 `//`。新增 boot 代码后必须跑它。

## HTML 解析器的两个隐患类别(本轮挖出并修完)
- **`src/js/parser.js` 的 chunk 边界不变性此前完全不成立。** `feed()` 是流式接口、切点由网络决定,
  但原文有四处「needle 横跨 chunk 就永远失配」:
  1. `raw`(script/style)最严重 —— `</script` 一旦跨切点,整个 `<script>` 连同其后**所有**正文
     被当成脚本内容,元素直接消失。喂 25 字节的 chunk 恰好不踩到,换个切点就中招。
  2. `comment` —— 切在 `--` 与 `>` 之间时注释再也不会闭合,余下整篇文档被吞进注释。
  3. `text` —— 缓冲区恰好断在 `<` 之后时把 `<` 当字面,`<title>` 被切成 `<` + `title>` 后整个元素
     连同正文变成纯文本。修的时候还踩了一步:先 `i = lt + 1` 再 return,下个 chunk 从 `lt+1` 接着扫,
     那个 `<` 被永久跳过 —— **比不修更隐蔽**,游标必须留在 `lt`。
  4. `pi` —— **修不了也没必要修**:PI 内容整段被丢弃(既不入 DOM 也不记 warnings),
     「少扫末尾一个字节」不可能改变任何可观察输出。留尾部的写法是假的,已删,并在代码里
     写明「将来若改成保留 PI 文本,必须同时补上」。这类**不可观察的防御性代码同样是负债**。
- **修法统一为「needle 找不到时,末尾 needle.length-1 字节不消费,等下次 feed」。**
- 测试:`tests/js/parser.test.js` 里做**穷举切点不变性** —— 20 份文档 × 每个字节偏移,
  外加三刀切(小文档),要求 DOM / scripts / warnings 与整段解析**逐字节一致**。约 3400 条断言。
  这类性质测试比逐个手写用例强:它不会漏掉你没想的切点。

## `<script>` 不一定是脚本(本轮修,已在生产链路生效)
- parser 只负责把 `type`(缺失时回退到过时别名 `language`)记在脚本记录上;
  **「算不算脚本」是加载器的策略**(`js_engine.c` 的 `__tb_is_js_script`,按 JavaScript MIME essence 白名单)。
- 修前的两个后果:① 内容被 eval —— 结构化数据的 JSON 抛 `expecting ';'`,而 `<style>` 的 CSS
  更是**每个带样式的页面**都拿到一条 `script error: ... is not defined` 假报错;
  ② 带 `src` 的数据块会**真的发起一次网络请求**去抓一个根本不是脚本的 URL(唯一有外部副作用的一条)。
- 连带:`RAW` 含 `script` 与 `style`,此前两者都塞进 `scripts`,现只收 `script`。

## 其余 HTML 规范偏差(已修)
- **数值实体按码点解释**,此前用 `String.fromCharCode` → `&#x1F600;` 被截成 U+F600(私有区码位),
  渲染成无意义字形。改 `fromCodePoint`,并把 NUL / 代理区 / 越界映射到 U+FFFD(规范要求)。
  ⚠️ 写这条时我把原来 `&nbsp;` 那个**不可见的 U+00A0 字节**改成了普通空格 —— 教训:
  **解析器源码里不要放裸的非 ASCII 字面量,一律用 `\uXXXX` 转义。**
- **属性名一律小写**(规范在 tokenizer 就做了)。标签名早已小写,属性名漏了。而 `dom.js` 取值
  一律用小写键(`n.attrs.href` / `.class` / `.id`)→ `<a HREF=/x>` 的 href 是空、**链接是死的**。
- **重复属性保留第一个**(规范丢弃后者),此前后者覆盖前者。
- **raw 闭合标签名后必须紧跟分隔符**(空白 / `/` / `>`)。此前没有这个判断,
  `</scriptfoo>` 会闭合 script,把后面的正文吃成 HTML。
- **文档在 raw 元素中途结束时,那段内容仍是一段完整脚本**(EOF 结束 raw 文本)。
  此前直接丢弃 —— 下载被截断时脚本就此消失。

## 现状与遗留
- ✅ 导航与交互**已完全**改由 qzjs 引擎驱动。交互经控制面在 JS 自己的树上查询。
- ✅ `window` / `location` / `history` / `navigator.userAgent` 已补齐;`document.cookie` 是真 jar。
- ✅ `tb_pump` 会排空 JS 邮箱(此前从不调 `poll_timers`,等于 nav 指令永远没人执行)。
- ✅ 定时器改 DOM 后视图自动刷新(JS 侧 DOM 指纹比对)。
- ✅ **坏 UTF-8 替换成 U+FFFD**(`json_escape`)。此前一个坏字节会让整个 eval 失败,
  整页连正文都渲染不出来(`json_string` 违背了自己「产出可安全嵌入 JS 字面量的字节」的职责)。
- ⚠️ **chunk 切分缺陷目前是潜在的,不是活的**:`__tb_begin_load` 走的是 `p.feed(body)` 一次喂全量,
  今天没有生产路径能踩到。但 `feed()` 是 parser 的公开 API 且它本就是流式 tokenizer,
  性质测试已把不变量钉住,改成真分块喂也不会再退化。
- cookie jar **刻意不做**的部分(已知缺口,非遗漏):无完整 public suffix 列表、无 SameSite、不持久化。
  真正缺的是「拒绝裸 TLD」这一条(仅当请求 host 本身就是 com 时才被接受)。
- ⚠️ **cookie 的 domain 匹配不看端口**。`strip_port` 要处理 IPv6 方括号与「端口段非数字 ⇒ fail closed」。
- ⚠️ **qzjs 不回调宿主 ⇒「宿主函数」不可用**。读侧靠 `op:"eval"` 推给 `__tb_env`;
  写侧靠 JS `postMessage` 进邮箱、宿主 `poll_timers` 取出执行。新增宿主能力必须在这两个方向里选。
- ⚠️ **qzjs 的 polyfill 会预设 `navigator.userAgent='qzjs/1.0 (WinterTC)'`**,必须覆盖成浏览器 UA,
  但不要整体替换 `navigator` 对象。
- ⚠️ `<meta charset>` **被解析但未被使用**。GBK/Latin-1 页面按 UTF-8 处理,需要转码表,
  不在 M2a 范围内。当前只是不崩(坏字节替换成 U+FFFD),不是正确解码。
- ⚠️ `<base href>` **被解析进树但未用于相对 URL 解析**。相对 href/src 仍按文档 URL 解析。
  `dom.js` 的选择器只支持 `tag` / `.class` / `#id` / `tag.class`,不支持属性选择器。
- `tb_url_encode` 仍会对已有的 `%XX` 二次编码(有测试钉住当前行为,尚未修)。


## Timeline

- time: 2026-10-04T14:05:26
  kind: decision
  summary: "Created this page: JS 引擎迁移到 qzjs 子模块：架构与集成定案"
  source: "M2a 引擎替换决策"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T14:06:02
  kind: decision
  summary: "qzjs 作为唯一 JS 引擎：子模块 + THREAD 模型 + 控制面 eval 桥"
  source: "M2a 引擎替换决策"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T14:06:11
  kind: reversal
  summary: "推翻「必须用 QZ_EXTRA_SOURCES 才能同步 eval」：qzjs 控制面原生已有 op:\"eval\"/\"inspect\"，走公开 API 即可且不违反主权原则，QZ_EXTRA_SOURCES 方案作废"
  source: "深读 src/control.c 后"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T15:40:50
  kind: decision
  summary: "迁移完成:控制面 eval 桥已落地,附踩坑与构建要点"
  source: "实现 + 74 测试全绿 + 全新 build 目录验证"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T16:25:45
  kind: reversal
  summary: "修正上一条结论:「minimal 之后单独打开 QZ_WITH_TLS 即可」是错的 —— 那只改了 cache,已生成的构建规则不回溯,实际编出 QZ_WITH_TLS=0(假开关,HTTPS 整段被 #if 掉)。正确做法:预置 QZ_PROFILE_LAST 跳过 profile-switch,且所有 QZ_WITH_* 必须在 add_subdirectory 之前落 cache"
  source: "开启 TLS 时实测 flags.make 发现"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:31:49
  kind: decision
  summary: "集成测试挖出子资源抓取的 4 个 bug(UAF/坏 JSON/相对 src/缺 window)与构建系统陈旧产物缺陷"
  source: "端到端集成测试 7 用例 + ASAN 全新构建 82/82"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:32:17
  kind: reversal
  summary: "「导航已完全由 qzjs 驱动、遗留仅 console 堆积」是错的 —— <script src> 那条路径从未真正跑过:tb_load_sync 有 use-after-free(fake transport 静默吸收,单测全绿是假信号)、map 拼出的是非法 JSON、相对 src 从不解析、window 全局缺失。四者叠在同一条代码路径上,真 curl 下首个 <script src> 即 segfault"
  source: "tests/integration/test_m2a_integration.cc 端到端复现并逐个定位"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:32:25
  kind: reversal
  summary: "「minimal 之后单独打开 QZ_WITH_TLS 即可」是错的 —— 那只改了 cache,已生成的构建规则不回溯,实际编出 QZ_WITH_TLS=0(假开关,HTTPS 整段被 #if 掉)。正确做法:预置 QZ_PROFILE_LAST 跳过 profile-switch,且所有 QZ_WITH_* 必须在 add_subdirectory 之前落 cache"
  source: "开启 TLS 时实测 flags.make 发现"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:32:36
  kind: decision
  summary: "确立测试分层:unit 直驱 engine seam 不碰网络,integration 走完整导航路径(真 HTTP+真 qzjs+真 curl),js/ 纯 JS 单测,goldens/ 逐字节。并立下门禁:凡是测试替身比被测物更宽容的地方,单测全绿都是假信号"
  source: "写 M2a 集成测试时的分工与教训"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:02:08
  kind: decision
  summary: "reentrancy 根治:传输回调与引擎工作两段分离。on_headers/on_body/on_done 只搬数据登记,nav_ctx 挂进 b->nav_ready;load_document/render(含 <script src> 子资源抓取)一律由 tb_pump 在循环顶层调 nav_commit_ready 做。配套:pending 计数在 commit 时才递减(否则 tb_session_idle 提前变真,pump 可能在 commit 前返回,调用方拿到上一张视图);tb_destroy 排空 nav_ready + nav_inflight"
  source: "根因是 curl_multi_add_handle 在 curl_multi_info_read 循环栈内被调用,属 UB"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:02:16
  kind: reversal
  summary: "推翻「reentrancy 是次要遗留、修掉 UAF 即可」:它是 UB —— curl_multi_add_handle 在 curl_multi_info_read 循环栈内被调用,表现随版本/时序/栈布局变化,不是稳定段错误。且「测试没崩」根本证明不了修好了:同一个 bug 上一轮就曾以「页面照常渲染、只是脚本没跑」的形式静默存在"
  source: "修完 UAF 后写结构性断言,并回退修复验证测试能变红"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:04:24
  kind: decision
  summary: "reentrancy 已根治(两段式)+ 导航中销毁泄漏已修 + UB 类修复的验收标准(必须证明测试能变红)"
  source: "commit 71c6f69;ASAN 84/84 零泄漏;回退修复验证测试变红"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:27:43
  kind: decision
  summary: "提交前复核:补回改写 compiled truth 时丢失的「qzos 那份 checkout 未推送、勿当基准」结论"
  source: "提交前逐条核对 compiled truth 与 HEAD 的差异"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T23:46:06
  kind: decision
  summary: "浏览器 API 补齐完成(cookie/location/history/navigator + tb_pump 排空邮箱 + 定时器驱动重渲),并确立「qzjs 无宿主回调 ⇒ 读push/写postMessage」这条新增能力的方向"
  source: "commit fb87cc0;ASAN+UBSAN+leak 114/114 零泄漏"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T23:46:12
  kind: reversal
  summary: "推翻「遗留只是 console 帧堆积 + 定时器需手动重渲」:真正的大头是 tb_pump 从不调 poll_timers。qzjs 从不回调宿主,邮箱是它唯一出站通道,不排它等于把 JS 的对外输出全堵死 —— console 堆积只是症状,location.href= 发出的导航指令永远没人执行才是要害"
  source: "实现 location 写侧时实测发现指令无人消费"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T00:19:24
  kind: decision
  summary: "修正 cookie public-suffix 那条过重表述(跨站泄漏实际已被 domain 后缀检查挡住),并记下「cookie domain 匹配不看端口」这个实现陷阱"
  source: "commit f49b3e5;实测验证 Domain=com 跨站攻击不成立"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T00:19:31
  kind: reversal
  summary: "推翻「cookie jar 缺 public suffix 列表 ⇒ Domain=com 跨站泄漏防不住」这条我此前写下的判断:实测不成立。cookie_domain_attr 已要求 domain 是请求 host 的后缀,evil.test 声明 Domain=com 会被拒,cookie 不会发往 bank.com。真正缺的只是「拒绝裸 TLD」(仅当请求 host 本身就是 com 时才被接受)。教训:凭「少了 X 机制 ⇒ 攻击 Y 成立」的推理写下结论,没实测"
  source: "写探针实测 evil.test 设 Domain=com 后是否发往 bank.com/victim.com"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T00:41:47
  kind: decision
  summary: "指针已切到 qzjs master(PR #1/#2/#3 全部合并);记录 squash merge 导致旧 SHA 非 master 祖先这个反直觉现象及核对办法"
  source: "commit 7690f12;全新 build 122/122、ASAN 零泄漏"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T04:27:03
  kind: decision
  summary: "已落地并全绿(149/149;ASAN+UBSAN+leak 全新构建 149/149 零泄漏);新增 chunk 边界不变性测试、boot 脚本语法门禁、<script type> 策略、HTML 规范偏差一批"
  source: "M2a:复杂页面压力测试(坏 UTF-8 + parser 缺陷批次)"
  affects: [qzjs-engine-swap]


## Timeline

- time: 2026-10-04T14:05:26
  kind: decision
  summary: "Created this page: JS 引擎迁移到 qzjs 子模块：架构与集成定案"
  source: "M2a 引擎替换决策"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T14:06:02
  kind: decision
  summary: "qzjs 作为唯一 JS 引擎：子模块 + THREAD 模型 + 控制面 eval 桥"
  source: "M2a 引擎替换决策"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T14:06:11
  kind: reversal
  summary: "推翻「必须用 QZ_EXTRA_SOURCES 才能同步 eval」：qzjs 控制面原生已有 op:\"eval\"/\"inspect\"，走公开 API 即可且不违反主权原则，QZ_EXTRA_SOURCES 方案作废"
  source: "深读 src/control.c 后"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T15:40:50
  kind: decision
  summary: "迁移完成:控制面 eval 桥已落地,附踩坑与构建要点"
  source: "实现 + 74 测试全绿 + 全新 build 目录验证"
  affects: [qzjs-engine-swap]

- time: 2026-10-04T16:25:45
  kind: reversal
  summary: "修正上一条结论:「minimal 之后单独打开 QZ_WITH_TLS 即可」是错的 —— 那只改了 cache,已生成的构建规则不回溯,实际编出 QZ_WITH_TLS=0(假开关,HTTPS 整段被 #if 掉)。正确做法:预置 QZ_PROFILE_LAST 跳过 profile-switch,且所有 QZ_WITH_* 必须在 add_subdirectory 之前落 cache"
  source: "开启 TLS 时实测 flags.make 发现"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:31:49
  kind: decision
  summary: "集成测试挖出子资源抓取的 4 个 bug(UAF/坏 JSON/相对 src/缺 window)与构建系统陈旧产物缺陷"
  source: "端到端集成测试 7 用例 + ASAN 全新构建 82/82"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:32:17
  kind: reversal
  summary: "「导航已完全由 qzjs 驱动、遗留仅 console 堆积」是错的 —— <script src> 那条路径从未真正跑过:tb_load_sync 有 use-after-free(fake transport 静默吸收,单测全绿是假信号)、map 拼出的是非法 JSON、相对 src 从不解析、window 全局缺失。四者叠在同一条代码路径上,真 curl 下首个 <script src> 即 segfault"
  source: "tests/integration/test_m2a_integration.cc 端到端复现并逐个定位"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:32:25
  kind: reversal
  summary: "「minimal 之后单独打开 QZ_WITH_TLS 即可」是错的 —— 那只改了 cache,已生成的构建规则不回溯,实际编出 QZ_WITH_TLS=0(假开关,HTTPS 整段被 #if 掉)。正确做法:预置 QZ_PROFILE_LAST 跳过 profile-switch,且所有 QZ_WITH_* 必须在 add_subdirectory 之前落 cache"
  source: "开启 TLS 时实测 flags.make 发现"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T04:32:36
  kind: decision
  summary: "确立测试分层:unit 直驱 engine seam 不碰网络,integration 走完整导航路径(真 HTTP+真 qzjs+真 curl),js/ 纯 JS 单测,goldens/ 逐字节。并立下门禁:凡是测试替身比被测物更宽容的地方,单测全绿都是假信号"
  source: "写 M2a 集成测试时的分工与教训"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:02:08
  kind: decision
  summary: "reentrancy 根治:传输回调与引擎工作两段分离。on_headers/on_body/on_done 只搬数据登记,nav_ctx 挂进 b->nav_ready;load_document/render(含 <script src> 子资源抓取)一律由 tb_pump 在循环顶层调 nav_commit_ready 做。配套:pending 计数在 commit 时才递减(否则 tb_session_idle 提前变真,pump 可能在 commit 前返回,调用方拿到上一张视图);tb_destroy 排空 nav_ready + nav_inflight"
  source: "根因是 curl_multi_add_handle 在 curl_multi_info_read 循环栈内被调用,属 UB"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:02:16
  kind: reversal
  summary: "推翻「reentrancy 是次要遗留、修掉 UAF 即可」:它是 UB —— curl_multi_add_handle 在 curl_multi_info_read 循环栈内被调用,表现随版本/时序/栈布局变化,不是稳定段错误。且「测试没崩」根本证明不了修好了:同一个 bug 上一轮就曾以「页面照常渲染、只是脚本没跑」的形式静默存在"
  source: "修完 UAF 后写结构性断言,并回退修复验证测试能变红"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:04:24
  kind: decision
  summary: "reentrancy 已根治(两段式)+ 导航中销毁泄漏已修 + UB 类修复的验收标准(必须证明测试能变红)"
  source: "commit 71c6f69;ASAN 84/84 零泄漏;回退修复验证测试变红"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T05:27:43
  kind: decision
  summary: "提交前复核:补回改写 compiled truth 时丢失的「qzos 那份 checkout 未推送、勿当基准」结论"
  source: "提交前逐条核对 compiled truth 与 HEAD 的差异"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T23:46:06
  kind: decision
  summary: "浏览器 API 补齐完成(cookie/location/history/navigator + tb_pump 排空邮箱 + 定时器驱动重渲),并确立「qzjs 无宿主回调 ⇒ 读push/写postMessage」这条新增能力的方向"
  source: "commit fb87cc0;ASAN+UBSAN+leak 114/114 零泄漏"
  affects: [qzjs-engine-swap]

- time: 2026-10-05T23:46:12
  kind: reversal
  summary: "推翻「遗留只是 console 帧堆积 + 定时器需手动重渲」:真正的大头是 tb_pump 从不调 poll_timers。qzjs 从不回调宿主,邮箱是它唯一出站通道,不排它等于把 JS 的对外输出全堵死 —— console 堆积只是症状,location.href= 发出的导航指令永远没人执行才是要害"
  source: "实现 location 写侧时实测发现指令无人消费"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T00:19:24
  kind: decision
  summary: "修正 cookie public-suffix 那条过重表述(跨站泄漏实际已被 domain 后缀检查挡住),并记下「cookie domain 匹配不看端口」这个实现陷阱"
  source: "commit f49b3e5;实测验证 Domain=com 跨站攻击不成立"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T00:19:31
  kind: reversal
  summary: "推翻「cookie jar 缺 public suffix 列表 ⇒ Domain=com 跨站泄漏防不住」这条我此前写下的判断:实测不成立。cookie_domain_attr 已要求 domain 是请求 host 的后缀,evil.test 声明 Domain=com 会被拒,cookie 不会发往 bank.com。真正缺的只是「拒绝裸 TLD」(仅当请求 host 本身就是 com 时才被接受)。教训:凭「少了 X 机制 ⇒ 攻击 Y 成立」的推理写下结论,没实测"
  source: "写探针实测 evil.test 设 Domain=com 后是否发往 bank.com/victim.com"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T00:41:47
  kind: decision
  summary: "指针已切到 qzjs master(PR #1/#2/#3 全部合并);记录 squash merge 导致旧 SHA 非 master 祖先这个反直觉现象及核对办法"
  source: "commit 7690f12;全新 build 122/122、ASAN 零泄漏"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T04:27:03
  kind: decision
  summary: "已落地并全绿(149/149;ASAN+UBSAN+leak 全新构建 149/149 零泄漏);新增 chunk 边界不变性测试、boot 脚本语法门禁、<script type> 策略、HTML 规范偏差一批"
  source: "M2a:复杂页面压力测试(坏 UTF-8 + parser 缺陷批次)"
  affects: [qzjs-engine-swap]

- time: 2026-10-06T09:54:13
  kind: decision
  summary: "qzjs 指针更新到 master 2020a598(PR #8~#15);issue #5(CA 注入点)已被上游解决;PR #2 交叉构建评审点因上游 Linux-only 范围决定而作废"
  source: "M2a:更新 qzjs 依赖"
  affects: [qzjs-engine-swap]
