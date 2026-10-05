cmake_minimum_required(VERSION 3.16)
project(tinybrowser VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# No system packages. All deps are vendored under deps/ (pinned versions in
# deps/VERSIONS.md). We integrate them via add_subdirectory so the build only
# sees source we own.
option(TB_BUILD_TESTS "Build tests" ON)
option(TB_BUILD_CLI   "Build tb CLI" ON)
option(TB_WITH_LIBCURL "Build the libcurl-based transport (Task 3)" ON)

# ---- vendored deps (all from deps/; no system packages) ----
# qzjs: JS 引擎(M2a)。qzjs 自带 QuickJS-ng + libuv + mbedTLS + miniz,并以
# add_subdirectory 引入它们——目标名(qjs / uv_a / mbedtls / mbedx509 /
# mbedcrypto)与本仓库原先自带的那几份完全同名。故 deps/quickjs-ng、
# deps/libuv、deps/mbedtls 已删除,统一用 qzjs 的那一份(qzjs 还要给它的
# quickjs-ng 打 C99-atomics / drain-jobs 等补丁,必须是它自己的副本)。
set(QZ_PROCESS_MODEL "THREAD" CACHE STRING "" FORCE)   # 单进程;ISOLATED 需随包部署 qzjs-rt
set(QZ_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(QZ_BUILD_CLI      OFF CACHE BOOL "" FORCE)
set(QZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QZ_BUILD_DEBUGGER OFF CACHE BOOL "" FORCE)
# QZ_PROFILE=minimal 是关掉 WebAssembly 引擎(wamr/wasm3)的档位。浏览器不跑 wasm,
# 且本仓库不初始化 wamr/wasm3 子模块(见 deps/VERSIONS.md)。
set(QZ_PROFILE "minimal" CACHE STRING "" FORCE)
# ⚠️ 关键:把 QZ_PROFILE_LAST 预置成与 QZ_PROFILE 相同,qzjs 的 profile-switch 块
# (deps/qzjs/CMakeLists.txt:169 `if(NOT QZ_PROFILE_LAST STREQUAL _qz_profile)`)
# 就不会执行 —— 它会用 FORCE 把整组 QZ_WITH_* 重写一遍(standard/empty 分支里
# QZ_WITH_WAMR 和 QZ_WITH_TLS 都被强制成 ON),把我们的设定全部盖掉。
set(QZ_PROFILE_LAST "minimal" CACHE STRING "" FORCE)

# 下面的 QZ_WITH_* 必须全部在 add_subdirectory **之前**落进 cache。
# qzjs 是在 add_subdirectory 期间读取这些值来决定:链不链 mbedTLS、编不编
# QZ_WITH_TLS 宏(uv_io.c / tcp_io.c 的 HTTPS 分支全靠它)。之后再改 cache 不会
# 回溯已生成的构建规则 —— 曾经因此出现「cache 显示 ON、flags.make 里却是
# QZ_WITH_TLS=0」的假开关:HTTPS 代码整段被 #if 掉,且 libqzjs 没链 mbedTLS。
set(QZ_WITH_TLS        ON  CACHE BOOL "" FORCE)  # 浏览器必须能开 https 页面
set(QZ_WITH_CRYPTO_EXT ON  CACHE BOOL "" FORCE)  # TLS 依赖 crypto(TLS 会强制它,显式写清)
set(QZ_WITH_COMPRESS   ON  CACHE BOOL "" FORCE)
set(QZ_WITH_TEXTCODEC  ON  CACHE BOOL "" FORCE)
set(QZ_WITH_WAMR       OFF CACHE BOOL "" FORCE)  # 不跑 wasm,且未初始化该子模块
set(QZ_WITH_WASM3      OFF CACHE BOOL "" FORCE)

add_subdirectory(deps/qzjs)

# lexbor: HTML/CSS/HTML parser
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(deps/lexbor)

# libuv 与 mbedTLS 由 deps/qzjs 一并 add_subdirectory(见上),不重复引入。

# ---- polyfill toolchain 前置检查 ----
# qzjs 内嵌的 polyfill 字节码是构建产物(不入库),构建 qzjs 需要 npm 工具链
# 现场生成。qzjs 把这个检查放在 **configure 期**:npm ci 之前 configure 的话,
# 这里就会判定"工具链缺失",随后构建 qzjs 时报一句看不懂的缺文件错误。
# 与其让用户去翻 qzjs 的文档,不如在这里把话说清楚。
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/qzjs/polyfill/node_modules/esbuild")
  message(WARNING
    "qzjs polyfill 工具链缺失 —— 直接构建会失败。\n"
    "  修复: npm --prefix deps/qzjs/polyfill ci\n"
    "  然后 **重新 configure**(qzjs 在 configure 期做工具链检查)。\n"
    "  详见 deps/VERSIONS.md。")
endif()

# ---- libcurl: isolated ExternalProject. mbedTLS is the ONLY TLS backend. ----
if(TB_WITH_LIBCURL)
  include(ExternalProject)
  set(TB_CURL_BIN ${CMAKE_CURRENT_BINARY_DIR}/libcurl-prefix/src/libcurl-build)
  # mbedTLS 的源码与产物都来自 qzjs 的子模块(它在自己的 CMakeLists 里
  # add_subdirectory 这份 mbedtls,二进制落在 <build>/deps/mbedtls)。
  set(MBEDTLS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/deps/qzjs/deps/mbedtls)
  set(MBEDTLS_BIN ${CMAKE_CURRENT_BINARY_DIR}/deps/mbedtls/library)
  ExternalProject_Add(libcurl
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcurl
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DBUILD_SHARED_LIBS=OFF
      -DBUILD_CURL_EXE=OFF
      -DBUILD_TESTING=OFF
      -DHTTP_ONLY=ON
      -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF
      -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF
      -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON
      -DCMAKE_USE_MBEDTLS=ON
      -DCURL_USE_MBEDTLS=ON
      -DCURL_USE_OPENSSL=OFF
      -DCMAKE_USE_OPENSSL=OFF
      -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
      -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF
      -DMBEDTLS_INCLUDE_DIR=${MBEDTLS_SRC}/include
      -DMBEDTLS_LIBRARY=${MBEDTLS_BIN}/libmbedtls.a
      -DMBEDX509_LIBRARY=${MBEDTLS_BIN}/libmbedx509.a
      -DMBEDCRYPTO_LIBRARY=${MBEDTLS_BIN}/libmbedcrypto.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${TB_CURL_BIN}/lib/libcurl.a
  )
  add_dependencies(libcurl mbedtls mbedx509 mbedcrypto)
endif()

add_library(tinybrowser STATIC
  src/core/browser.c
  src/core/clock.c
  src/core/content.c
  src/core/url.c
  src/core/dom.c
  src/core/view.c
  src/core/render.c
  src/core/hint.c
  src/core/session.c
  src/core/curl_transport.c
  src/core/js_engine.c
)
target_include_directories(tinybrowser PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/lexbor/source
  # 用自己的 libcurl 头(与 libcurl.a 同源,避免误用系统 OpenSSL 版头)。
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcurl/include
  ${CMAKE_CURRENT_BINARY_DIR}/src/core   # js_builtins.inc
)
# mbedTLS provides three static libs: mbedtls / mbedx509 / mbedcrypto.
# Use target names exposed by deps/mbedtls (canonical mbedtls 3.x names).
# libcurl.a's objects reference mbedtls_* symbols, so it must appear BEFORE
# the mbedTLS archives on consumers' link lines (static archive order).
if(TB_WITH_LIBCURL)
  add_dependencies(tinybrowser libcurl)
  set(TB_LIBCURL ${TB_CURL_BIN}/lib/libcurl.a)
else()
  set(TB_LIBCURL "")
endif()
target_link_libraries(tinybrowser PUBLIC
  ${TB_LIBCURL} lexbor_static qzjs uv_a mbedtls mbedx509 mbedcrypto)

# ---- JS builtins 注入:src/js/*.js → js_builtins.inc(Task 2-4 填充内容) ----
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_js.cmake)
set(TB_JS_BUILTINS ${CMAKE_CURRENT_BINARY_DIR}/src/core/js_builtins.inc)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/src/core)
tb_embed_js(${CMAKE_CURRENT_SOURCE_DIR}/src/js ${TB_JS_BUILTINS} tb_js_builtins)
add_dependencies(tinybrowser tb_js_builtins)

if(TB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(deps/googletest)
  add_subdirectory(tests)
endif()

if(TB_BUILD_CLI AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/cli/tb_cli.c)
  add_executable(tb src/frontends/cli/tb_cli.c)
  target_link_libraries(tb PRIVATE tinybrowser)
  target_include_directories(tb PRIVATE deps/termbox2 ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
endif()
