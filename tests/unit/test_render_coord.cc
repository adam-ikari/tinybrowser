#include "dom.h"
#include "render.h"
#include "tb.h"
#include <gtest/gtest.h>
#include <fstream>
#include <iterator>
#include <string>
#include <string.h>

static std::string slurp(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

static tb_view *render_fixture(const char *name) {
  std::string dir = TB_TEST_SRC_DIR + std::string("tests/goldens/fixtures/");
  std::string html = slurp(dir + name + ".html");
  tb_dom *d = tb_dom_parse(html.data(), html.size());
  tb_view *v = tb_render(d, ("http://example.test/" + std::string(name)).c_str(), 200);
  tb_dom_free(d);
  return v;
}

static bool is_clickable(const char *type) {
  return strcmp(type, "link") == 0 || strcmp(type, "button") == 0 ||
         strcmp(type, "input") == 0 || strcmp(type, "select") == 0;
}

TEST(RenderCoord, OffPointsAtElemTextStart) {
  for (const char *name : {"hello", "nav", "forms"}) {
    tb_view *v = render_fixture(name);
    const char *text = tb_view_text(v);
    ASSERT_NE(text, nullptr);
    for (int i = 0; i < tb_view_nelems(v); i++) {
      struct tb_elem e{};
      ASSERT_EQ(tb_view_elem(v, i, &e), 0);
      if (!is_clickable(e.type)) continue;
      ASSERT_NE(e.text, nullptr);
      size_t len = strlen(e.text);
      ASSERT_LE(e.off, strlen(text)) << name << " elem " << i;
      ASSERT_LE(e.off + len, strlen(text)) << name << " elem " << i;
      // 更强断言(优于 strstr):off 处前缀必须恰好等于该元素文本
      EXPECT_EQ(strncmp(text + e.off, e.text, len), 0) << name << " elem " << i;
    }
    tb_view_free(v);
  }
}
