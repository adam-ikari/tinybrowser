---
id: quickjs-refcount
title: "quickjs-ng 引用计数约定与 M2a 引擎踩坑"
category: decision
status: active
tags: [js, quickjs, refcount]
created: "2026-08-18T03:24:59"
updated: "2026-08-18T03:25:18"
---

<!-- compiled_truth -->
# quickjs-ng 引用计数约定(M2a js_engine 调试实证)

## JS_SetPropertyStr 取走 val 的"所有权"
- 语义:内部 `set_value` 只释放旧值、不复制新值;设置成功后**调用者不得再 `JS_FreeValue(val)`**,否则属性悬空、GC 时 `gc_decref_child` 断言崩溃(`JS_REF_COUNT(p) > 0`)。
- 已实测:`JS_SetPropertyStr` 后再 `JS_FreeValue(val)` → 段错误。
- 与官方 bellard QuickJS(借用引用、调用者 free)不同——**本仓库钉的 quickjs-ng v0.16.1 是所有权转移**。
- 位置:`src/core/js_engine.c` 的 `_tb_current_doc` 赋值处有注释。

## JSValue 子引用必须先于父释放
- 从父 `JSValue` 经 `JS_GetProperty*` 派生的所有子引用(`je_id`/`je_type`/…),必须在其父(`je`)被 `JS_FreeValue` **之前**逐一释放;否则父引用计数归零被回收后,子引用指向已回收值,触发相同断言。

## js_memory_limit 在 builtins 注入之后设置
- 引擎自身 builtins(dom.js/parser.js/render.js ~27KB)不应受用户配置的文档内存限制约束;`JS_SetMemoryLimit` 在 open 尾部(注入完成后)调用,限制文档加载期(脚本)与运行期。

## load_document = parse + _tb_attach + 脚本执行
- `_tb_parser.parse()` 返回 `{root, doc, scripts, readyState, warnings}` **包装对象**,树根是 `.doc`(=root);`tb_render_js` 期望直接收到树根节点。
- `document._tb_attach(root)` 必须调用(挂 `_tb_node_proto` 原型 + 播种 `_tb_next_id`)。
- 脚本执行走全局 `_tb_load_document(body)`(C 字符串注入):parse → attach → 同步执行 scripts(inline eval / src 走 `__tb_load_sync` 同步桥)→ readyState=complete。
- 每 script 独立 try/catch 隔离;`js_exec_ms_limit` 超时经 `JS_SetInterruptHandler` 中断该 script。

## 调试方法备忘
- 崩溃在 `JS_FreeRuntime`/GC 时,用 gdb 在 quickjs.c `gc_decref_child` 断言处断点,`frame 2` 看 `JS_MarkContext` 的调用点(是 `global_obj` 还是别的 root)可缩小范围。
- 二分法:写最小 C 复现(open+load_document+close / open+render+close),分离问题在 load 还是 render。


## Timeline

- time: 2026-08-18T03:24:59
  kind: decision
  summary: "Created this page: quickjs-ng 引用计数约定与 M2a 引擎踩坑"
  source: "M2a js_engine 调试实证"
  affects: [quickjs-refcount]

- time: 2026-08-18T03:25:18
  kind: decision
  summary: Rewrote compiled_truth to the new best understanding
  source: brain update-truth
  affects: [quickjs-refcount]
