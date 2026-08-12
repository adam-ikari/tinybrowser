#ifndef TB_CORE_HINT_H
#define TB_CORE_HINT_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* vimium 式 hint 键位分配。n = 可点元素总数,i = 元素序号(0-based)。
   n<=26:单字母 keys[i];n>26:全两字母 keys[i%26] + keys[i/26](N<=676 全唯一)。
   返回写入 dst 的字符数(1 或 2);i>=n 或 cap<2 返回 0。dst 需至少 2 字节。 */
size_t tb_hint_keys(size_t n, size_t i, char *dst, size_t cap);

/* 字节偏移 → (行,列),均 0 起。text 可 NULL。 */
void tb_off_to_pos(const char *text, size_t off, int *row, int *col);

/* (行,列) → 字节偏移;越界钳制到文本末尾。text 可 NULL。 */
size_t tb_pos_to_off(const char *text, int row, int col);

#ifdef __cplusplus
}
#endif
#endif
