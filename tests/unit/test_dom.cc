#include "dom.h"
#include <gtest/gtest.h>
#include <string.h>

static tb_dom *parse(const char *html) {
    return tb_dom_parse(html, strlen(html));
}

TEST(Dom, ParseAndTraverse) {
    tb_dom *d = parse("<p>hi <b>there</b></p><a href='/x'>L</a>");
    ASSERT_NE(d, nullptr);
    tb_node *root = tb_dom_root(d);
    tb_node *p = tb_dom_first_child(root);
    while (p && !tb_dom_is_element(p)) p = tb_dom_next_sibling(p);
    ASSERT_NE(p, nullptr);
    EXPECT_STREQ(tb_dom_tag(p), "p");
    EXPECT_EQ(tb_dom_attr(p, "href"), nullptr);
    tb_node *b = tb_dom_first_child(p);
    EXPECT_STREQ(tb_dom_text(b), "hi ");  // first child of <p> is text "hi "
    b = tb_dom_next_sibling(b);
    EXPECT_STREQ(tb_dom_tag(b), "b");
    tb_node *a = tb_dom_next_sibling(p);
    EXPECT_STREQ(tb_dom_tag(a), "a");
    EXPECT_STREQ(tb_dom_attr(a, "href"), "/x");
    tb_dom_free(d);
}

TEST(Dom, IdsStableAndIdentityBound) {
    tb_dom *d = parse("<a>1</a><a>2</a>");
    tb_node *root = tb_dom_root(d);
    tb_node *n = tb_dom_first_child(root);
    while (n && !tb_dom_is_element(n)) n = tb_dom_next_sibling(n);
    tb_node *a1 = n;
    tb_node *a2 = tb_dom_next_sibling(a1);
    int id1 = tb_dom_id(d, a1);
    int id2 = tb_dom_id(d, a2);
    EXPECT_NE(id1, 0);
    EXPECT_NE(id2, 0);
    EXPECT_NE(id1, id2);
    EXPECT_EQ(tb_dom_id(d, a1), id1);
    EXPECT_EQ(tb_dom_id(d, a2), id2);
    tb_dom_free(d);
}

TEST(Dom, IdsIndependentAcrossDocuments) {
    tb_dom *d1 = parse("<a>1</a>");
    tb_dom *d2 = parse("<a>1</a>");
    tb_node *r1 = tb_dom_root(d1), *r2 = tb_dom_root(d2);
    tb_node *n1 = tb_dom_first_child(r1), *n2 = tb_dom_first_child(r2);
    EXPECT_EQ(tb_dom_id(d1, n1), 1);
    EXPECT_EQ(tb_dom_id(d2, n2), 1);
    EXPECT_NE((void*)n1, (void*)n2);
    tb_dom_free(d1);
    tb_dom_free(d2);
}
