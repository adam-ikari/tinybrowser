---
id: qzjs-engine-swap
title: "JS 引擎迁移到 qzjs 子模块：架构与集成定案"
category: decision
status: active
tags: [js, qzjs, quickjs, engine, submodule]
created: "2026-10-04T14:05:26"
updated: "2026-10-05T05:27:43"
---

<!-- compiled_truth -->
# JS 引擎迁移到 qzjs（qzjs-engine-swap）

## 结论:已落地并全绿(84/84;ASAN+UBSAN+leak 全新构建 84/84 零泄漏)

## 定位:qzjs 不是 QuickJS 的 drop-in,而是「邮箱 + 事件循环」运行时
- `adam-ikari/qzjs` = *Embeddable QuickJS-ng runtime with PAL*。JS 跑在**库自有线程 + 自有 uv loop** 上。
- 主权铁律(`include/qzjs/qzjs.h:130`):**qzjs 从不执行宿主代码**。公共 API 无函数指针字段,宿主↔库的
  全部通讯只有一条通道:入站 `qz_post_message` / `qz_control`,出站 per-rt FIFO 邮箱
  `qz_recv_message` / `qz_free_message`(唤醒 fd `qz_message_fd`,eventfd)。
- 因此「宿主直接拿到 `JSContext*` 同步 eval」这条路**默认不存在**——JSContext 归 qzjs 线程独占。

## 定案一:子模块(从自己 GitHub 克隆)
- `deps/qzjs` → `https://github.com/adam-ikari/qzjs.git`,钉在 `ba4dd940bc27aff9f1f5b4309a1f6688af8674f0`
  (= 克隆时的 GitHub `master` HEAD)。shallow clone(`--depth 1`)。
- qzjs 自身还有 8 个嵌套子模块;只初始化 4 个:`quickjs-ng@6d46d07` / `libuv@84af0b1` /
  `miniz@77d0dce` / `mbedtls@068ff08`。
- **故意不初始化**:`wamr`(体量最大,tinybrowser 无 wasm 需求)、`wasm3`、`lz4`
  (仅 `QZ_POLYFILL_MODE=compressed` 需要,默认 `rodata`)、`googletest`。
- ⚠️ **`deps/qzjs/deps/mbedtls` 还有第三层嵌套子模块 `framework`(mbedtls-framework),
  必须初始化**,否则 mbedTLS 的 CMake config 步骤报
  "framework/CMakeLists.txt not found … Run: git submodule update --init"。
- ⚠️ **指针不指向 qzjs master**:qzjs PR #1(修 https fetch + `qz_destroy` 崩溃,已 rebase 到
  `c1425f9`)尚未合入 master。指向 master 会让 https fetch 全部失败、`qz_destroy` 崩溃回归。
  等 PR #1 合并后再切。
- ⚠️ **`/home/gem/project/qzos/qzjs` 是同一上游的另一份 checkout**,它的本地提交(`1d438237`
  等)多半**未推送到 GitHub** —— `git branch -r --contains <sha>` 在该 checkout 里查不到任何
  远程分支。**以 GitHub master 为准,不要拿本地那份当基准**,否则会把只存在于本地的改动
  误当成上游已有能力。

## 定案二:进程模型 = THREAD(单进程)
- `QZ_PROCESS_MODEL=THREAD`。ISOLATED(qzjs 默认)要随包部署 `qzjs-rt` 可执行档并在执行期定位它,
  对 TUI 浏览器是不必要的负担。
- 代价:拿不到跨进程 liveness ping(`qz_ping*` 仅 ISOLATED 编译存在)。用 `qz_ping_if_available()`
  跨模型探测,THREAD 下恒返回 `QZ_PING_UNAVAILABLE(-2)`——**不谎报健康**。

## 定案三:同步 eval 走控制面 `op:"eval"`,**不需要** `QZ_EXTRA_SOURCES`(推翻早先建议)
- 早先建议「用 `QZ_EXTRA_SOURCES` 把自有 .c 编进 libqzjs 直接拿 JSContext 做同步 eval」。**作废**——
  qzjs 已原生提供这条路,走公开 API 即可,且不违反主权原则、升级时不受内部头文件变动影响。
- 链路:`qz_control(rt, cmd)` → msgq → qzjs 线程的 `qz_wake_cb`(`src/thread.c:88`)
  → `flags==QZ_MSG_FLAG_CONTROL` 分支 → `qz_control_dispatch`(`src/control.c:998`)
  → `ctl_eval`(`src/control.c:884`)在**qzjs 线程上** `JS_Eval` → 回执入邮箱。
- 命令 `{"op":"eval","correl":"<id>","timeout_ms":T,"script":"<code>"}`;回执
  `{"ctl":true,"correl":"<id>","ok":1,"result":<val>}`;异常时 `ok:0` + `error` + `code`
  (`JS_EXCEPTION`/`INTERRUPTED`/`INVALID_ARG`/`UNKNOWN_CMD`/`NOT_FOUND`/`BAD_REQUEST`/`TIMEOUT`)。
- `op:"inspect"` = 求值 + `JS_JSONStringify` → 回执 `json` 字段。**用它取代逐字段搬 `tb_view`。**
- `src/control.c` 在 `_qz_core_sources` 里是**无条件**的(CMakeLists:734),只有
  `control_endpoint.c`(LOCAL 档 uv_pipe 端点)是 ISOLATED-only → **THREAD 下控制面可用**。
- 必须 `cfg.control_plane = QZ_CONTROL_IN_PROC`(默认 `QZ_CONTROL_OFF` 让 `qz_control` 恒 -1)。
- `correl` 是回执**唯一**配对依据,缺/空/非字符串会被硬拒。唯一豁免是 `op:"interrupt"`。

## 定案四:删掉 tinybrowser 自带的 quickjs-ng / libuv / mbedtls
- 目标名冲突是硬阻塞:qzjs `add_subdirectory` 它那份 quickjs-ng/libuv/mbedtls,产出与
  tinybrowser **同名**的 target(`qjs`/`uv_a`/`mbedtls`+`mbedx509`+`mbedcrypto`),
  同一次 configure 里定义两次直接报错。
- qzjs 还要给它的 quickjs-ng 打 5 个补丁(C99 atomics / drain-jobs / bc-reader-hardening /
  debugger / libuv-c99-atomics),是工作树改动 → quickjs-ng **必须**来自 qzjs 子模块。
- 连带:libcurl 的 ExternalProject 改指 `deps/qzjs/deps/mbedtls`;`test_tls` 的 include 也改。

## 落地后的接口变更
- `tb_config.js_memory_limit` **已删除**(qzjs 无 `JS_SetMemoryLimit` 对应旋钮,用户确认接受失效)。
  OOM 现在表现为引擎自身抛 JS 异常 → eval 回执 `ok:0`。
- `js_exec_ms_limit` 语义改为「控制面回执窗口」,默认 5000ms(对齐 qzjs `ctl_extract` 缺省),
  超时投递 `op:"interrupt"`。
- 6 个 `__tb_*` host bridge 全部消失。console 改走 `postMessage` 进邮箱 → 宿主
  `poll_timers` / 控制面往返途中派发到 `cfg.on_console`。定时器改由 qzjs polyfill 提供
  (宿主不再有 timer 链表)。
- `<script src>` 改**两段式**:`__tb_begin_load(body)` 解析并返回 src 列表 → 宿主用既有
  `tb_load_sync` 批量抓取 → `__tb_finish_load(map)` 执行。解析只做一次。
- **`load_document` seam 加了 `base_url` 参数**(修正③):`<script src>` 必须相对文档最终 URL
  解析,原签名没有 base,相对引用根本无法工作。相对 src 解析不了就跳过该条,不猜。

## 构建要点(踩过的坑)
- **qzjs 内嵌 polyfill 字节码是构建产物,不入库**。fresh clone 必须
  `npm --prefix deps/qzjs/polyfill ci`,然后**重新 configure**(qzjs 在 configure 期查工具链)。
  已加 `message(WARNING)` 在 tinybrowser 的 CMakeLists 里提前说清这件事。
- ⚠️ **「内嵌代码不进库」这条规律对 tinybrowser 自己同样成立**:`tb_embed_js` 原先在
  configure 期把 `src/js/*.js` 直接 `file(WRITE)` 成 `js_builtins.inc`,此后无人再生成它。
  后果是**改完任意 `.js` 直接 `make`,编进去的还是旧 JS**,构建系统对源码改动完全无感。
  已改成 `add_custom_command(OUTPUT)` + 独立执行体 `cmake/embed_js_run.cmake`,依赖 `.js` 内容、
  `.js` 文件集合(`CONFIGURE_DEPENDS`)与生成脚本自身。**教训:凡是 configure 期 file(WRITE)
  出来的构建产物,都要当成陈旧缓存审一遍。**
- **切勿用 `set(QZ_WITH_WAMR OFF CACHE ... FORCE)` 关 wamr**:qzjs 的 profile-switch 块
  (`deps/qzjs/CMakeLists.txt:169`)在 `add_subdirectory` 期间会用 FORCE 把整组 `QZ_WITH_*`
  重写一遍,外层 set(无论前置还是普通变量)都被盖掉。正确做法是 `QZ_PROFILE=minimal`
  (它就是「不要 wasm」的档位)。
- 但 minimal 顺带把 **TLS 也关了**,而浏览器没 TLS 就只能看明文页。qzjs 的 profile-switch
  在 `add_subdirectory` 内跑完,所以**在其之后**再 `set(QZ_WITH_TLS ON CACHE BOOL "" FORCE)`
  才不会被回写。最终缓存:`THREAD` / `minimal` / `QZ_WITH_TLS=ON` / `QZ_WITH_WAMR=OFF`。
  ⚠️ 这条后来又被推翻,见 Timeline 最后一条。
- `cjson` 是 qzjs 的 PUBLIC 依赖,宿主 `#include <cJSON.h>` 解析回执无需额外配置。

## 实现期踩到的 bug(都是「测试看着像引擎坏了,其实是自己写错」)
1. **inspect 会自动 `JSON.stringify`**。在表达式里再包一层 `JSON.stringify(...)` → 双层编码,
   宿主拿到 `"\"true\""` 而非 `"true"`,strcmp 判不中 → 误判「文档没加载」。
   `__tb_begin_load` 因此改成 `return srcs`(返回数组本身),布尔探针改用 `op:"eval"` + `String(...)`。
2. **cJSON 取嵌套字段取错层级**:`level`/`msg` 在 `__tb_console` 对象**里面**,最初从 root 上取,
   静默丢掉整条 console 消息。
3. **硬编码表达式长度**:`js_eval(..., 24, ...)` 少一个字符 → `SyntaxError: Unexpected end of input`。
   改用 `sizeof(STR)-1`。
4. **`window` 在 qzjs 里不存在**(有 `self`,且 `self === globalThis`)。`window.x = v` 是网页脚本
   最常见写法,缺了它大批真实页面第一行就 ReferenceError。已在 boot 脚本里补
   `globalThis.window = globalThis` 别名(同一对象,非拷贝)。`location` 同样缺失,尚未补。

## 子资源抓取(`<script src>`)—— 集成测试挖出的四个 bug
这条路径**此前从未被真正跑过**:单元测试全用 fake transport,而 fake 恰好把最严重的那个藏住了。
端到端集成测试(真 HTTP server + 真 qzjs + 真 curl)的第一个 `<script src>` 用例直接 segfault。

1. **use-after-free**:`tb_load_sync` 无条件 `transport->cancel(op)`,但传输层的所有权约定是
   「on_done 一触发就收走并释放 op」(`check_multi_info` 派发 `on_done` 后立即 `free(op)`)。
   于是拿着野指针 `curl_multi_remove_handle`。fake transport 的 `cancel` 在链表里找不到就静默
   返回 → **单元测试永远看不到这处崩溃**。修法:只在 op 仍在飞(既没 done 也没 failed)时 cancel。
2. **reentrancy(已修)**:传输回调与引擎工作**两段分离**。
   - 传输回调(`on_headers`/`on_body`/`on_done`)只搬数据、登记,不做任何引擎工作;
     `on_done` 把 `nav_ctx` 挂进 `b->nav_ready` 就返回。
   - 引擎侧的一切(`load_document`/`render`,含 `<script src>` 子资源抓取)由 `tb_pump`
     在循环顶层调 `nav_commit_ready` 做 —— 那时才真正离开 curl 的栈。
   - 配套:`pending` 计数在 commit 时才递减。若在 `on_done` 里就 `nav_done`,
     `tb_session_idle` 会提前变真,`tb_pump` 可能在 commit 之前返回,调用方拿到的是
     上一张视图。让 pending 挂到 commit 完成,pump 才可能判空闲。
   - `tb_destroy` 排空 `nav_ready` + `nav_inflight` 两个队列:`nav_ctx` 自己 malloc
     且带整页 body,传输层只管自己的 op。详见「导航中销毁的泄漏」。
3. **坏 JSON**:map 从 `"{}"` 起手、每条 `sprintf("%s,%s:%s")` 往后追加 —— 花括号在左,后面拖一串
   悬空键值,不是合法 object;末行 `map[strlen-1]='}'` 想替换「尾 `,`」,但此刻末尾是值的收尾
   引号,替换后吃掉引号。**后果隐蔽**:非法对象字面量被 `finish_load` 当普通实参报错跳过,页面正文
   照常渲染 —— 看起来一切正常,只是外部脚本永远没跑。改用 cJSON 拼 object、
   `cJSON_PrintUnformatted` 序列化。
4. **相对 `src` 从不解析**:裸相对路径直接丢给 transport,curl 拿到无 host 的路径 → 见上面
   `load_document` 加 `base_url`。

## 导航生命周期:两段式(定稿)
`do_navigate` → 传输回调(搬运/登记)→ `b->nav_ready` 队列 → `tb_pump` 顶层
`nav_commit_ready`(引擎工作)→ 视图 + session + 回调。

- 传输层**不回收 `nav_ctx`**,它只管自己的 op;`nav_ctx` 由宿主在 commit 或
  `tb_destroy` 时回收。
- `nav_inflight` 链表在 `do_navigate` 里、**早于** `open` 挂链 —— `open` 可能同步
  派发回调。

## 导航中销毁的泄漏(先前就存在,被新测试挖出)
调用方在导航途中直接 `tb_destroy`(不 `tb_wait_idle`),`on_done` 永不触发,
`do_navigate` 的 `calloc` 就成了泄漏 —— ASAN 实测 216B 直接 + 28B 间接(含整页
body)。这条路径此前从没被测过。加 `nav_inflight` 链表 + `tb_destroy` 排空解决。

## UB 类修复:必须证明测试能变红
「测试没崩」**证明不了** UB 修好了 —— 表现随版本、时序、栈布局变化。同一个
reentrancy bug,在修 UAF 之前就曾以「页面照常渲染,只是脚本没跑」的形式静默存在。

所以这类修复的验收标准是:**临时回退修复,确认测试变红**。本次回退后
`EngineWorkStaysOutsideTransportCallback` 的 `reentrant_calls` 从 0 变 2,测试变红,
才恢复。绿测试若没被证明能变红,就只是一张空断言。

测量手段:`tests/harness/guard_transport.h` —— 自包含极简 transport,在派发回调
期间置 `in_callback`,统计回调期间发生的 `open`/`poll` 次数。
门禁是 `reentrant_calls == 0`,**且必须配 `callback_count >= 2`** —— 少了后一条,
bug 回归时测试会「因为没进那条路」而假绿。这与 fake transport 藏住 UAF 是同一种
陷阱,同一个错误不能踩两次。

## 测试替身比被测物更宽容 = 盲区(方法论教训)
fake transport 的 `cancel` / `fake_open` 在「不认识的 op」上静默返回,于是把 UAF 完全吸收。
**凡是替身比被测物更宽容的地方,单测全绿都是假信号。** 判断某条路径是否真的被覆盖,要问
「测试里有没有任何一处会真的失败」—— `ExternalScriptSrc` 与
`RepeatedSubresourceLoadsDoNotCorruptTransport` 就是为回答这个问题写的:后者 5 轮
create→导航(含子资源抓取)→destroy,并断言外部脚本的全局变量逐轮累加到 5,证明它每轮都真跑了,
而不只是「没崩」。

## 测试分层(定稿)
- `tests/unit/test_js_engine.cc` —— 直接驱 engine seam(`open`/`load_document`/`render`),
  **不碰网络**。验的是「引擎能否加载一段 HTML 字符串」。
- `tests/integration/test_m2a_integration.cc` —— 真 HTTP server + 真 qzjs + 真 curl,
  走完整导航路径(`tb_navigate` → transport → `nav_on_done` → engine)。验的是「导航一次,
  脚本/console/定时器在真实链路里是否成立」。单元测试绕过的那一段(nav_on_done 的引擎接线、
  tb_pump 的驱动、`cfg.on_console` 的路由)恰是最容易接错的地方。
- `tests/js/*.test.js` —— 纯 JS 单测(qjs + bootstrap 垫片),覆盖 parser/dom/render。
- `tests/goldens/` —— `golden_runner` 驱 seam 出视图 dump 逐字节比对。
- `tests/harness/guard_transport.h` —— 观测用 transport,不测时序,只测「重入没发生」。

## 现状与遗留
- ✅ 导航与交互**已完全**改由 qzjs 引擎驱动(`nav_on_done` 走 engine seam,C 侧 DOM 树已删)。
  交互(click/fill/select/submit)经控制面在 JS 自己的树上查询
  (`_tb_elem_info`/`_tb_form_info`/`_tb_form_pairs`)。
- 缺 `window`(已补)/ `location`(未补)/ `document.cookie` 的真实现(当前单串往返)。
- `js_on_console` 的 drain 只在控制面往返与 `poll_timers` 发生;若宿主长时间不泵,console 帧会堆积。
- 定时器回调改了 DOM 后宿主视图不会自动刷新,需显式重新 render。
- 「内嵌代码不进库」只查了 `src/js/*.js`;若将来还有别的 configure 期生成物,同样要审。


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
