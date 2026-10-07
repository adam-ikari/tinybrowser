// 运行器环境垫片:quickjs-ng 将经典全局 load 移入 std.loadScript(需 --std),
// 且单测直接引用全局 _tb_parser。此处还原 load 并预载 parser.js 暴露 _tb_parser。
// 通过 `qjs --std -I tests/js/lib/bootstrap.js tests/js/xxx.test.js` 使用。
globalThis.load = std.loadScript;
load("./src/js/parser.js");
load("./src/js/dom.js");
load("./src/js/render.js");

// 引导桥的垫片。生产里 __tb_console / __tb_send_nav 由 js_engine.c 的 boot 脚本
// 提供(见 TB_BOOT_SRC);单测只 load 上面三个内置 js,故被测代码一调
// __tb_console 就是 ReferenceError —— 那不是被测物的行为,是垫片缺一块。
// 补在这里而不是各测试文件里,免得每个测试各写一份、各写错一份。
if (typeof globalThis.__tb_console === "undefined") {
  globalThis.__tb_console = {
    log: function () {}, info: function () {}, warn: function () {},
    error: function () {}, debug: function () {}
  };
}
if (typeof globalThis.__tb_send_nav === "undefined") {
  // 记录最近一条导航指令,供测试断言。生产里它 postMessage 进邮箱,
  // 由宿主 poll_timers 取出执行 —— 这里换成记下来,语义等价且可断言。
  globalThis.__tb_last_nav = null;
  globalThis.__tb_send_nav = function (action, url) {
    globalThis.__tb_last_nav = { action: action, url: url || "" };
  };
}
