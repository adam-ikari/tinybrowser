include(GoogleTest)
# 每个测试一个可执行;源文件在本计划后续任务逐个创建。
# 公共配置:src 头 + harness;链接 tinybrowser + gtest_main。
set(TB_TEST_INCS ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/../src/core ${CMAKE_CURRENT_SOURCE_DIR}/harness)

add_executable(test_smoke unit/test_smoke.cc)
target_include_directories(test_smoke PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_smoke PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_smoke)

add_executable(test_deps_link unit/test_deps_link.cc)
target_include_directories(test_deps_link PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_deps_link PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_deps_link)

add_executable(test_clock unit/test_clock.cc)
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

add_executable(test_session unit/test_session.cc)
target_include_directories(test_session PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_session PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_session)

add_executable(test_browser unit/test_browser.cc)
target_include_directories(test_browser PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_browser PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_browser)
