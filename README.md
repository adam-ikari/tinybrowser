# tinybrowser

可嵌入的 C99 文本浏览器内核(agent 可观察、可操作;Web1.0 式交互文本,无视觉)。

## 构建(零系统依赖)
    make init      # git submodule update --init --recursive
    make           # CMake 配置 + 构建(libtinybrowser.a / tb / 测试)
    make test      # ctest 全绿
    make clean

## 依赖
全部 vendored(git submodule):libuv / mbedtls(唯一 TLS 后端)/ lexbor / libcurl / termbox2 / googletest。
不使用 OpenSSL;TLS 仅 1.2/1.3。

## 用法(示例)
    ./build/tb https://example.com/
    # 或经 C API:见 src/tb.h

## 里程碑
M1 文本内核(本计划完成)→ M2 JS 运行时 → M3 MCP → M4 硬化 → M5 CDP。
