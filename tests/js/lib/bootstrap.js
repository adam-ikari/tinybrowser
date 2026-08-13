// 运行器环境垫片:quickjs-ng 将经典全局 load 移入 std.loadScript(需 --std),
// 且单测直接引用全局 _tb_parser。此处还原 load 并预载 parser.js 暴露 _tb_parser。
// 通过 `qjs --std -I tests/js/lib/bootstrap.js tests/js/xxx.test.js` 使用。
globalThis.load = std.loadScript;
load("./src/js/parser.js");
