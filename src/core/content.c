#include "content.h"
#include <ctype.h>
#include <string.h>

const char *tb_content_normalize(const char *content_type) {
  static char buf[128];
  size_t i = 0, o = 0;
  while (content_type && content_type[i] && content_type[i] != ';' &&
         o < sizeof buf - 1) {
    char c = content_type[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      buf[o++] = (char)tolower((unsigned char)c);
    i++;
  }
  buf[o] = '\0';
  return o == 0 ? NULL : buf;
}

tb_content_kind tb_content_classify(const char *content_type, int attachment) {
  if (attachment) return TB_CONTENT_NOT_RENDERABLE;
  const char *ct = tb_content_normalize(content_type);
  if (!ct) return TB_CONTENT_NOT_RENDERABLE;
  if (strncmp(ct, "text/", 5) == 0) return TB_CONTENT_RENDER;
  if (strcmp(ct, "application/xhtml+xml") == 0) return TB_CONTENT_RENDER;
  if (strcmp(ct, "application/xml") == 0) return TB_CONTENT_RENDER;
  if (strcmp(ct, "image/svg+xml") == 0) return TB_CONTENT_RENDER;
  return TB_CONTENT_NOT_RENDERABLE;
}
