#include "tb.h"
#include <gtest/gtest.h>
#include <uv.h>
#include <lexbor/html/parser.h>
#include <mbedtls/version.h>

TEST(Deps, LinkAndHeaderVersions) {
  unsigned int v = mbedtls_version_get_number();
  ASSERT_GT(v, 0u);

  uv_loop_t loop;
  ASSERT_EQ(uv_loop_init(&loop), 0);
  uv_loop_close(&loop);

  // lexbor: real parse a fragment, proves usable, not just linked.
  lxb_html_document_t *doc = lxb_html_document_create();
  ASSERT_NE(doc, nullptr);
  lxb_status_t st = lxb_html_document_parse(doc,
      (const lxb_char_t *)"<p>hi</p>", 9);
  ASSERT_EQ(st, LXB_STATUS_OK);
  lxb_html_document_destroy(doc);
}
