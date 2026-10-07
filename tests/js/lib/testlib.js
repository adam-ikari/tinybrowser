// 极简断言:qjs_exe 无外部依赖。失败累积,最终 throw 使 CLI 非零退出。
// eq/ok 是唯一计数源;t 成功不计数,异常时只记失败。
var __passed = 0, __failed = 0;
function eq(a, b, msg) {
  if (a !== b) { __failed++; console.log("FAIL " + (msg || "eq") + ": got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
  else __passed++;
}
function ok(cond, msg) { eq(!!cond, true, msg || "ok"); }
function t(name, fn) {
  try { fn(); }
  catch (e) { __failed++; console.log("FAIL " + name + ": " + e); }
}
function done() {
  console.log(__passed + " passed, " + __failed + " failed");
  if (__failed) throw new Error(__failed + " failures");
}
