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

# ---- libcurl: isolated ExternalProject. mbedTLS is the ONLY TLS backend. ----
if(TB_WITH_LIBCURL)
  include(ExternalProject)
  set(TB_CURL_BIN ${CMAKE_CURRENT_BINARY_DIR}/libcurl-prefix/src/libcurl-build)
  set(MBEDTLS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls)
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
  src/core/session.c
  src/core/curl_transport.c
)
target_include_directories(tinybrowser PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/lexbor/source
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/libuv/include
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/mbedtls/include
  # 用自己的 libcurl 头(与 libcurl.a 同源,避免误用系统 OpenSSL 版头)。
  ${CMAKE_CURRENT_SOURCE_DIR}/deps/libcurl/include
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
  ${TB_LIBCURL} lexbor_static uv_a mbedtls mbedx509 mbedcrypto)

if(TB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(deps/googletest)
  add_subdirectory(tests)
endif()

if(TB_BUILD_CLI AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/frontends/cli/tb_cli.c)
  add_executable(tb frontends/cli/tb_cli.c)
  target_link_libraries(tb PRIVATE tinybrowser)
  target_include_directories(tb PRIVATE deps/termbox2)
endif()
