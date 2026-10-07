# embed_js.cmake 的执行体(以 `cmake -P` 运行,变量由命令行 -D 传入)。
#
# 转义规则与旧实现一致,逐字节保持:
#   \  → \\    "  → \"    换行 → \n
# 其余字节(含 UTF-8 多字节序列、$ 之外的 CMake 特殊串)原样落入 C 字面量。
# 只写这些三种:JS 源里出现其它反斜杠序列时按 C 字符串语义解释,与旧行为相同。
#
# 文件列表由 configure 期传入(JS_SOURCES)而非在此 GLOB:`cmake -P` 脚本模式下
# CONFIGURE_DEPENDS 不被支持,直接 GLOB 又会让「新增 .js」这件事在依赖图里
# 不可见。configure 期已用 CONFIGURE_DEPENDS 做过 glob,传进来既准确又免费。

set(TB_JS_SRC ${JS_SOURCES})
if(NOT TB_JS_SRC)
  message(FATAL_ERROR "embed_js_run.cmake: JS_SOURCES 为空")
endif()
list(SORT TB_JS_SRC)

set(LINES "")
foreach(f ${TB_JS_SRC})
  file(READ ${f} SRC)
  string(REPLACE "\\" "\\\\" SRC "${SRC}")
  string(REPLACE "\"" "\\\"" SRC "${SRC}")
  string(REPLACE "\n" "\\n" SRC "${SRC}")
  get_filename_component(base ${f} NAME)
  string(APPEND LINES "  /* ${base} */\n  \"${SRC}\",\n")
endforeach()

# 中间态写到 .in 临时文件再 rename:custom command 只认 OUTPUT 的时间戳,
# 中途失败留下的半截 .inc 会被下一次 make 当成最新而跳过重建。
set(tmp "${OUT_FILE}.in")
file(WRITE ${tmp}
  "/* 由 cmake/embed_js.cmake 生成,勿手改。 */\nconst char *const tb_js_builtins[] = {\n${LINES}  NULL\n};\n")
file(RENAME ${tmp} ${OUT_FILE})