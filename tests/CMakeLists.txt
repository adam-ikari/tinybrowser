include(GoogleTest)
# 每个测试一个可执行;源文件在本计划后续任务逐个创建。
# 现在只有冒烟测试,先把公共配置写好。
set(TB_TEST_INCS ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR}/harness)
add_executable(test_smoke unit/test_smoke.cc)
target_include_directories(test_smoke PRIVATE ${TB_TEST_INCS})
target_link_libraries(test_smoke PRIVATE tinybrowser gtest_main)
gtest_discover_tests(test_smoke)
