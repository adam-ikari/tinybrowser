include(GoogleTest)
# 每个测试一个可执行;源文件在本计划后续任务逐个创建。
# 公共配置:src 头 + harness;链接 tinybrowser + gtest_main。
set(TB_TEST_INCS ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/harness)

add_executable(test_smoke unit/test_smoke.cc)
target_include_directories(test_smoke PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_smoke PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_smoke)

add_executable(test_deps_link unit/test_deps_link.cc)
target_include_directories(test_deps_link PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_deps_link PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_deps_link)

if(TB_WITH_LIBCURL)
  add_executable(test_curl_link unit/test_curl_link.cc)
  target_include_directories(test_curl_link PRIVATE
    ${TB_TEST_INCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../deps/libcurl/include
  )
  target_link_libraries(test_curl_link PRIVATE tinybrowser gtest_main)
  gtest_discover_tests(test_curl_link)
endif()
