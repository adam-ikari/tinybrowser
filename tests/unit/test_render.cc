#include "render.h"
#include "tb.h"
#include <gtest/gtest.h>
#include <string.h>

static tb_view *render(const char *html) {
  tb_dom *d = tb_dom_parse(html, strlen(html));
  tb_view *v = tb_render(d, "http://x/", 200);
  tb_dom_free(d);
  return v;
}

TEST(Render, TitleAndText) {
  tb_view *v = render("<title>T</title><h1>Hi</h1><p>a <b>b</b> c</p>");
  EXPECT_STREQ(tb_view_title(v), "T");
  ASSERT_NE(tb_view_text(v), nullptr);
  EXPECT_STREQ(tb_view_text(v), "Hi\na b c");
  EXPECT_EQ(tb_view_status(v), 200);
  tb_view_free(v);
}

TEST(Render, LinkElements) {
  tb_view *v = render("<a href='/a'>A</a> <a href='/b'>B</a>");
  ASSERT_EQ(tb_view_nelems(v), 2);
  struct tb_elem e{};
  ASSERT_EQ(tb_view_elem(v, 0, &e), 0);
  EXPECT_EQ(e.id, 1);
  EXPECT_STREQ(e.type, "link");
  EXPECT_STREQ(e.href, "/a");
  tb_view_free(v);
}

TEST(Render, FormElements) {
  tb_view *v = render(
    "<form action='/s' method='post'>"
    "<input name='q' value='cats'>"
    "<select name='lang'><option>en</option><option selected>zh</option></select>"
    "<button>Go</button></form>");
  ASSERT_EQ(tb_view_nelems(v), 4);   // form + input + select + button
  struct tb_elem e{};
  ASSERT_EQ(tb_view_elem(v, 1, &e), 0);   // input
  EXPECT_STREQ(e.type, "input");
  EXPECT_STREQ(e.name, "q");
  EXPECT_STREQ(e.value, "cats");
  ASSERT_EQ(tb_view_elem(v, 2, &e), 0);   // select
  EXPECT_STREQ(e.type, "select");
  EXPECT_EQ(e.noptions, 2);
  EXPECT_STREQ(e.options[0], "en");
  EXPECT_STREQ(e.options[1], "zh");
  tb_view_free(v);
}

TEST(Render, CloneIsDeep) {
  tb_view *v = render("<a href='/x'>L</a>");
  tb_view *c = tb_view_clone(v);
  tb_view_free(v);
  EXPECT_STREQ(tb_view_text(c), "L");
  ASSERT_EQ(tb_view_nelems(c), 1);
  struct tb_elem e{};
  tb_view_elem(c, 0, &e);
  EXPECT_STREQ(e.href, "/x");
  tb_view_free(c);
}
