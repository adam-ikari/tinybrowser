#include "content.h"
#include <gtest/gtest.h>

TEST(Content, Renderable) {
  EXPECT_EQ(tb_content_classify("text/html", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify("text/plain", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify("text/html; charset=utf-8", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify("TEXT/HTML", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify("application/xhtml+xml", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify("application/xml", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify("image/svg+xml", 0), TB_CONTENT_RENDER);
  EXPECT_EQ(tb_content_classify(nullptr, 0), TB_CONTENT_NOT_RENDERABLE);
  EXPECT_EQ(tb_content_classify("", 0), TB_CONTENT_NOT_RENDERABLE);
}

TEST(Content, NotRenderable) {
  EXPECT_EQ(tb_content_classify("image/png", 0), TB_CONTENT_NOT_RENDERABLE);
  EXPECT_EQ(tb_content_classify("application/pdf", 0), TB_CONTENT_NOT_RENDERABLE);
  EXPECT_EQ(tb_content_classify("application/octet-stream", 0), TB_CONTENT_NOT_RENDERABLE);
  EXPECT_EQ(tb_content_classify("application/zip", 0), TB_CONTENT_NOT_RENDERABLE);
  EXPECT_EQ(tb_content_classify("video/mp4", 0), TB_CONTENT_NOT_RENDERABLE);
  EXPECT_EQ(tb_content_classify("audio/mpeg", 0), TB_CONTENT_NOT_RENDERABLE);
}

TEST(Content, AttachmentForcesNotRenderable) {
  EXPECT_EQ(tb_content_classify("text/html", 1), TB_CONTENT_NOT_RENDERABLE);
}

TEST(Content, NormalizeStripsParams) {
  EXPECT_STREQ(tb_content_normalize("text/html; charset=utf-8"), "text/html");
  EXPECT_STREQ(tb_content_normalize("  Application/JSON ; charset=utf-8"), "application/json");
  EXPECT_EQ(tb_content_normalize(nullptr), nullptr);
}
