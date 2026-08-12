#include "hint.h"
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <string.h>

TEST(HintKeys, SingleLetterUpTo26) {
  char dst[4] = {0};
  for (size_t i = 0; i < 26; i++) {
    ASSERT_EQ(tb_hint_keys(26, i, dst, sizeof dst), 1u);
    EXPECT_EQ(dst[0], (char)('a' + i));
  }
  EXPECT_EQ(tb_hint_keys(26, 26, dst, sizeof dst), 0u);  /* 越界 */
}

TEST(HintKeys, SingleElement) {
  char dst[4] = {0};
  ASSERT_EQ(tb_hint_keys(1, 0, dst, sizeof dst), 1u);
  EXPECT_EQ(dst[0], 'a');
}

TEST(HintKeys, HundredElementsAllTwoLetter) {
  char dst[4] = {0};
  for (size_t i = 0; i < 100; i++)
    EXPECT_EQ(tb_hint_keys(100, i, dst, sizeof dst), 2u);
}

TEST(HintKeys, TwoLettersAbove26) {
  char dst[4] = {0};
  ASSERT_EQ(tb_hint_keys(27, 0, dst, sizeof dst), 2u);
  EXPECT_EQ(dst[0], 'a'); EXPECT_EQ(dst[1], 'a');
  ASSERT_EQ(tb_hint_keys(27, 26, dst, sizeof dst), 2u);
  EXPECT_EQ(dst[0], 'a'); EXPECT_EQ(dst[1], 'b');   /* i=26 → i%26=0 → 'a', i/26=1 → 'b' */
}

TEST(HintKeys, AllUniqueUpTo676) {
  std::set<std::string> seen;
  for (size_t i = 0; i < 676; i++) {
    char dst[4] = {0};
    ASSERT_EQ(tb_hint_keys(676, i, dst, sizeof dst), 2u);
    seen.insert(std::string(dst, 2));
  }
  EXPECT_EQ(seen.size(), 676u);   /* 全部 hint 唯一 */
}

TEST(Coord, OffToPosBasics) {
  int r = -1, c = -1;
  tb_off_to_pos("ab\ncd", 0, &r, &c); EXPECT_EQ(r, 0); EXPECT_EQ(c, 0);
  tb_off_to_pos("ab\ncd", 2, &r, &c); EXPECT_EQ(r, 0); EXPECT_EQ(c, 2);
  tb_off_to_pos("ab\ncd", 3, &r, &c); EXPECT_EQ(r, 1); EXPECT_EQ(c, 0);
  tb_off_to_pos("ab\ncd", 5, &r, &c); EXPECT_EQ(r, 1); EXPECT_EQ(c, 2);
}

TEST(Coord, PosToOffBasics) {
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 0, 0), 0u);
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 0, 2), 2u);
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 1, 0), 3u);
  EXPECT_EQ(tb_pos_to_off("ab\ncd", 1, 2), 5u);
}

TEST(Coord, RoundTrip) {
  const char *text = "line one\nline two\nline three\nlast";
  for (size_t off = 0; off <= strlen(text); off++) {
    if (off < strlen(text) && text[off] == '\n') continue;  /* 换行符无唯一往返(映射到下行起点) */
    int r, c;
    tb_off_to_pos(text, off, &r, &c);
    EXPECT_EQ(tb_pos_to_off(text, r, c), off);
  }
}

TEST(Coord, EdgeCases) {
  int r, c;
  tb_off_to_pos("", 0, &r, &c);       EXPECT_EQ(r, 0); EXPECT_EQ(c, 0);
  tb_off_to_pos(NULL, 0, &r, &c);     EXPECT_EQ(r, 0); EXPECT_EQ(c, 0);
  EXPECT_EQ(tb_pos_to_off(NULL, 0, 0), 0u);
  EXPECT_EQ(tb_pos_to_off("abc", 5, 0), 3u);    /* row 越界 → 末尾 */
  EXPECT_EQ(tb_pos_to_off("abc", 0, 99), 3u);   /* col 越界 → 行尾 */
}