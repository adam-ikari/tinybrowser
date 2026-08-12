#include "hint.h"

static const char KEYS[] = "abcdefghijklmnopqrstuvwxyz";

size_t tb_hint_keys(size_t n, size_t i, char *dst, size_t cap) {
  if (i >= n || cap < 2) return 0;
  if (n <= 26) {
    dst[0] = KEYS[i];
    return 1;
  }
  dst[0] = KEYS[i % 26];
  dst[1] = KEYS[i / 26];
  return 2;
}

void tb_off_to_pos(const char *text, size_t off, int *row, int *col) {
  int r = 0, c = 0;
  if (text) {
    for (size_t i = 0; i < off && text[i] != '\0'; i++) {
      if (text[i] == '\n') { r++; c = 0; }
      else c++;
    }
  }
  if (row) *row = r;
  if (col) *col = c;
}

size_t tb_pos_to_off(const char *text, int row, int col) {
  if (!text) return 0;
  size_t off = 0;
  int r = 0;
  while (*text) {
    if (r == row) break;
    if (*text++ == '\n') r++;
    off++;
  }
  /* text 现指向第 row 行起点(或已到末尾) */
  int c = 0;
  while (*text && *text != '\n' && c < col) { text++; off++; c++; }
  return off;
}
