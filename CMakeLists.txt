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
option(TB_WITH_LIBCURL "Build the libcurl-based transport (Task 3)" OFF)

# ---- vendored deps (all from deps/; no system packages) ----
# lexbor: HTML/CSS/HTML parser
set(LEXBOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LEXBOR_BUILD_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(deps/lexbor)

# libuv: event loop (M2 will use; built unconditionally so later tasks can link)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/libuv)

# mbedTLS: only TLS backend. No OpenSSL anywhere.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory(deps/mbedtls)

add_library(tinybrowser STATIC
  src/core/browser.c
  src/core/clock.c
  src/core/content.c
  src/core/url.c
  src/core/dom.c
  src/core/view.c
  src/core/render.c
  src/core/session.c
  src/core/curl_transport.c
)
target_include_directories(tinybrowser PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/lexbor/source
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/include
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls/include
)
# mbedTLS provides three static libs: mbedtls / mbedx509 / mbedcrypto.
# Use target names exposed by deps/mbedtls (canonical mbedtls 3.x names).
target_link_libraries(tinybrowser PUBLIC lexbor_static uv_a mbedtls mbedx509 mbedcrypto)

if(TB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(deps/googletest)
  add_subdirectory(tests)
endif()

if(TB_BUILD_CLI)
  add_executable(tb frontends/cli/tb_cli.c)
  target_link_libraries(tb PRIVATE tinybrowser)
  target_include_directories(tb PRIVATE deps/termbox2)
endif()
