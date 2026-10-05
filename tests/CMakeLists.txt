include(GoogleTest)
# 每个测试一个可执行;源文件在本计划后续任务逐个创建。
# 公共配置:src 头 + harness;链接 tinybrowser + gtest_main。
set(TB_TEST_INCS ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../src/core ${CMAKE_CURRENT_SOURCE_DIR}/harness)

add_executable(test_smoke unit/test_smoke.cc harness/fakes.c)
target_include_directories(test_smoke PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_smoke PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_smoke)

add_executable(test_deps_link unit/test_deps_link.cc)
target_include_directories(test_deps_link PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_deps_link PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_deps_link)

add_executable(test_clock unit/test_clock.cc harness/fakes.c)
target_include_directories(test_clock PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_clock PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_clock)

if(TB_WITH_LIBCURL)
  add_executable(test_curl_link unit/test_curl_link.cc)
  target_include_directories(test_curl_link PRIVATE
    ${TB_TEST_INCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/libcurl/include
  )
  target_link_libraries(test_curl_link PRIVATE tinybrowser gtest_main)
  gtest_discover_tests(test_curl_link)
endif()

add_executable(test_content unit/test_content.cc)
target_include_directories(test_content PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_content PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_content)

add_executable(test_url unit/test_url.cc)
target_include_directories(test_url PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_url PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_url)

add_executable(test_dom unit/test_dom.cc)
target_include_directories(test_dom PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_dom PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_dom)

add_executable(test_render unit/test_render.cc)
target_include_directories(test_render PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_render PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_render)

add_executable(test_session unit/test_session.cc harness/fakes.c)
target_include_directories(test_session PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_session PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_session)

add_executable(test_browser unit/test_browser.cc harness/fakes.c)
target_include_directories(test_browser PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_browser PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_browser)

add_executable(test_interact unit/test_interact.cc harness/fakes.c)
target_include_directories(test_interact PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_interact PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_interact)

add_executable(test_network unit/test_network.cc harness/http_server.cc)
target_include_directories(test_network PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_network PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_network)

# 不显式列出 mbedtls:tinybrowser 已 PUBLIC 链接(顺序 curl→mbedtls,静态库单向解析),
# 显式列出会让 mbedtls 提前到 libcurl 前面导致未定义符号。
add_executable(test_tls unit/test_tls.cc harness/tls_server.cc)
# mbedTLS 头随 qzjs 子模块进来(qzjs 自己 add_subdirectory 这份)。
target_include_directories(test_tls PRIVATE ${TB_TEST_INCS} ${CMAKE_CURRENT_SOURCE_DIR}/../deps/qzjs/deps/mbedtls/include)
target_compile_definitions(test_tls PRIVATE TB_TEST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")
target_link_libraries(test_tls PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_tls)

add_executable(golden_runner goldens/golden_runner.cc)
target_include_directories(golden_runner PRIVATE ${TB_TEST_INCS})
target_link_libraries(golden_runner PRIVATE tinybrowser qjs gtest_main)
add_test(NAME golden COMMAND golden_runner ${CMAKE_SOURCE_DIR}/)

add_executable(test_render_coord unit/test_render_coord.cc)
target_include_directories(test_render_coord PRIVATE ${TB_TEST_INCS})
# 注意:此处用 ${CMAKE_SOURCE_DIR}/ 而非 ${CMAKE_CURRENT_SOURCE_DIR}/ —
# 测试代码按 repo 根拼接 "tests/goldens/fixtures/",与 golden_runner 的前缀约定一致。
target_compile_definitions(test_render_coord PRIVATE TB_TEST_SRC_DIR="${CMAKE_SOURCE_DIR}/")
target_link_libraries(test_render_coord PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_render_coord)

add_executable(test_hint unit/test_hint.cc)
target_include_directories(test_hint PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_hint PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_hint)

# qzjs 接入 smoke:验证引擎底座可创建、控制面 eval/inspect 往返可用。
add_executable(test_qzjs_embed unit/test_qzjs_embed.cc)
target_include_directories(test_qzjs_embed PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_qzjs_embed PRIVATE tinybrowser qzjs gtest_main)
gtest_discover_tests(test_qzjs_embed)

# js_engine.c seam 实现单元测试(Task 5)
add_executable(test_js_engine unit/test_js_engine.cc harness/fakes.c)
target_include_directories(test_js_engine PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_js_engine PRIVATE tinybrowser qzjs gtest_main)
gtest_discover_tests(test_js_engine)

# Cookie jar:纯 C 逻辑测试(域/路径匹配、过期、HttpOnly/Secure)。
add_executable(test_cookie unit/test_cookie.cc)
target_include_directories(test_cookie PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_cookie PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_cookie)

# M2a 端到端集成:真 HTTP server + 真 qzjs + 真 curl,走完整导航路径。
add_executable(test_m2a_integration integration/test_m2a_integration.cc harness/http_server.cc)
target_include_directories(test_m2a_integration PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_m2a_integration PRIVATE tinybrowser qzjs gtest_main)
gtest_discover_tests(test_m2a_integration)

# 纯 JS 单测:parser.js tokenizer。qjs 二进制 OUTPUT_NAME=qjs,目标名 qjs_exe。
# quickjs-ng 移除全局 load(移入 std.loadScript,需 --std),
# 故经 bootstrap.js 垫片还原 load 并预载 parser.js 暴露 _tb_parser。
# 默认构建也带出 qjs_exe(quickjs-ng EXCLUDE_FROM_ALL,不会自动构建)。
add_custom_target(tb_js_runner ALL DEPENDS qjs_exe)
add_test(NAME js_parser
  COMMAND $<TARGET_FILE:qjs_exe> --std -I ${CMAKE_CURRENT_SOURCE_DIR}/js/lib/bootstrap.js
          ${CMAKE_CURRENT_SOURCE_DIR}/js/parser.test.js)
add_test(NAME js_dom
  COMMAND $<TARGET_FILE:qjs_exe> --std -I ${CMAKE_CURRENT_SOURCE_DIR}/js/lib/bootstrap.js
          ${CMAKE_CURRENT_SOURCE_DIR}/js/dom.test.js)
add_test(NAME js_render
  COMMAND $<TARGET_FILE:qjs_exe> --std -I ${CMAKE_CURRENT_SOURCE_DIR}/js/lib/bootstrap.js
          ${CMAKE_CURRENT_SOURCE_DIR}/js/render.test.js)
# bootstrap/testlib 用相对路径 load,须以仓库根为工作目录
set_tests_properties(js_parser PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(js_dom PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(js_render PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
