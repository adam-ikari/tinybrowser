---
id: qzjs-engine-swap
title: "JS 引擎迁移到 qzjs 子模块：架构与集成定案"
category: decision
status: active
tags: [js, qzjs, quickjs, engine, submodule]
created: "2026-10-04T14:05:26"
updated: "2026-10-04T16:25:45"
---

<!-- compiled_truth -->
# JS 引擎迁移到 qzjs（qzjs-engine-swap）

## 结论:已落地并全绿(74/74 ctest,含全新 build 目录从零构建)

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
- 注意:`/home/gem/project/qzos/qzjs` 是同一上游的另一份 checkout,其 `1d438237` **未推送**到
  GitHub——以 GitHub master 为准,不要拿本地那份当基准。

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
  `tb_load_sync` 批量抓取 → `__tb_finish_load(map)` 执行。解析只做一次。顺带消除 nested pump 重入。

## 构建要点(踩过的坑)
- **qzjs 内嵌 polyfill 字节码是构建产物,不入库**。fresh clone 必须
  `npm --prefix deps/qzjs/polyfill ci`,然后**重新 configure**(qzjs 在 configure 期查工具链)。
  已加 `message(WARNING)` 在 tinybrowser 的 CMakeLists 里提前说清这件事。
- **切勿用 `set(QZ_WITH_WAMR OFF CACHE ... FORCE)` 关 wamr**:qzjs 的 profile-switch 块
  (`deps/qzjs/CMakeLists.txt:169`)在 `add_subdirectory` 期间会用 FORCE 把整组 `QZ_WITH_*`
  重写一遍,外层 set(无论前置还是普通变量)都被盖掉。正确做法是 `QZ_PROFILE=minimal`
  (它就是「不要 wasm」的档位)。
- 但 minimal 顺带把 **TLS 也关了**,而浏览器没 TLS 就只能看明文页。qzjs 的 profile-switch
  在 `add_subdirectory` 内跑完,所以**在其之后**再 `set(QZ_WITH_TLS ON CACHE BOOL "" FORCE)`
  才不会被回写。最终缓存:`THREAD` / `minimal` / `QZ_WITH_TLS=ON` / `QZ_WITH_WAMR=OFF`。
- `cjson` 是 qzjs 的 PUBLIC 依赖,宿主 `#include <cJSON.h>` 解析回执无需额外配置。

## 实现期踩到的三个 bug(都是「测试看着像引擎坏了,其实是自己写错」)
1. **inspect 会自动 `JSON.stringify`**。在表达式里再包一层 `JSON.stringify(...)` → 双层编码,
   宿主拿到 `"\"true\""` 而非 `"true"`,strcmp 判不中 → 误判「文档没加载」。
   `__tb_begin_load` 因此改成 `return srcs`(返回数组本身),布尔探针改用 `op:"eval"` + `String(...)`。
2. **cJSON 取嵌套字段取错层级**:`level`/`msg` 在 `__tb_console` 对象**里面**,我最初从 root 上取,
   静默丢掉整条 console 消息。
3. **硬编码表达式长度**:`js_eval(..., 24, ...)` 少一个字符 → `SyntaxError: Unexpected end of input`。
   改用 `sizeof(STR)-1`。

## 现状与遗留
- `test_qjs_embed.cc`(直接 `JS_NewRuntime` 测 quickjs-ng 本体)已删除——tinybrowser 不再直接嵌入
  QuickJS,测它等于测第三方依赖。替换为 `test_qzjs_embed.cc`:测**我们真正依赖的链路**
  (qz_create → 控制面 eval/inspect → 邮箱回执),含 escaping 往返、语法错误、throw、
  initial_script、strict 模式禁 fs。
- ⚠️ **`browser.c` 的导航路径仍走旧的 C 管线**(`tb_dom_parse` + `tb_render`),
  `engine->load_document` / `engine->render` **没有任何生产调用方**,只有测试在调。
  这是迁移前就有的状态(M2a 未接线),不是本次引入。TUI 行为与迁移前一致。
  要让 qzjs 真正驱动浏览器,需把 `nav_on_done` 切到 engine seam——**下一步工作**。
- `js_on_console` 的 drain 只在控制面往返与 `poll_timers` 发生;若宿主长时间不泵,console 帧会堆积。

## 与既有结论的关系
- [[quickjs-refcount]] 的引用计数约定(`JS_SetPropertyStr` 取走所有权、子引用先于父释放)
  **只对直接嵌入 quickjs-ng 的旧引擎成立**。走控制面后宿主不再持有/释放 `JSValue`,
  那两条踩坑对新引擎不再适用;内存/引用语义改由 qzjs 内部负责。


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
