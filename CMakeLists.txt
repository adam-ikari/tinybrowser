cmake_minimum_required(VERSION 3.16)
project(tinybrowser VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TB_BUILD_TESTS "Build tests" ON)
option(TB_BUILD_CLI   "Build tb CLI" ON)

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
target_include_directories(tinybrowser PUBLIC src)

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
