# 把 src/js/*.js 转成 C 字符串数组。输入目录、输出路径由调用方传入。
#
# 实现为 custom command + OUTPUT,而不是 configure 期直接 file(WRITE):
# 内嵌 JS 是**构建产物**,必须在 .js 改动时自动重建。此前这个函数在 configure
# 期执行,结果改完 src/js/dom.js 直接 `make` 拿到的是旧 js_builtins.inc —— 构建
# 系统对源码改动完全无感(与 qzjs polyfill 字节码同一个坑,见 BRAIN
# qzjs-engine-swap「构建要点」)。
#
# 依赖声明:
#   - DEPENDS ${TB_JS_SRC} + CONFIGURE_DEPENDS  .js 内容或文件集合变化都触发重建
#   - DEPENDS 本脚本                              脚本改了也触发重建
#
# 用法:
#   tb_embed_js(<src/js 目录> <输出 .inc 路径> <输出目标名>)
# 之后 `<输出目标名>` 即为生成该 .inc 的目标(可被 add_dependencies 引用)。
function(tb_embed_js IN_DIR OUT_FILE OUT_TARGET)
  file(GLOB TB_JS_SRC CONFIGURE_DEPENDS ${IN_DIR}/*.js)
  list(SORT TB_JS_SRC)
  add_custom_command(
    OUTPUT ${OUT_FILE}
    COMMAND ${CMAKE_COMMAND}
            "-DJS_SOURCES=${TB_JS_SRC}"
            "-DOUT_FILE=${OUT_FILE}"
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_js_run.cmake
    DEPENDS ${TB_JS_SRC} ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/embed_js_run.cmake
    COMMENT "Embedding JS builtins -> ${OUT_FILE}"
    VERBATIM)
  add_custom_target(${OUT_TARGET} ALL DEPENDS ${OUT_FILE})
endfunction()