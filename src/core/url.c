#include "url.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int is_scheme_char(char c) {
  return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.';
}

static int has_scheme(const char *s) {
  if (!s || !isalpha((unsigned char)*s)) return 0;
  for (const char *p = s + 1; *p; p++) {
    if (*p == ':') return 1;
    if (!is_scheme_char(*p)) return 0;
  }
  return 0;
}

static char *dup_str(const char *s) {
  size_t n = strlen(s);
  char *o = (char *)malloc(n + 1);
  if (!o) return NULL;
  memcpy(o, s, n + 1);
  return o;
}

char *tb_url_resolve(const char *base, const char *ref) {
  if (!base || !ref) return NULL;
  if (has_scheme(ref)) return dup_str(ref);   /* 绝对 */

  /* 拆 base 为 scheme://authority/path?query#fragment */
  size_t n = strlen(base);
  char *b = (char *)malloc(n + 1);
  memcpy(b, base, n + 1);
  char *q = strchr(b, '?'); if (q) *q = '\0';
  char *f = strchr(b, '#'); if (f) *f = '\0';

  if (ref[0] == '#') {           /* 纯 fragment:base 去掉 fragment + ref */
    char *out = (char *)malloc(n + strlen(ref) + 1);
    snprintf(out, n + strlen(ref) + 1, "%s%s", b, ref);
    free(b);
    return out;
  }
  if (ref[0] == '?') {           /* 纯 query:base 路径 + ref */
    char *out = (char *)malloc(n + strlen(ref) + 1);
    snprintf(out, n + strlen(ref) + 1, "%s%s", b, ref);
    free(b);
    return out;
  }

  if (ref[0] == '/') {           /* 绝对路径:scheme://authority + ref */
    const char *auth = strstr(b, "//");
    const char *pathp = auth ? strchr(auth + 2, '/') : strchr(b, '/');
    size_t prefix = pathp ? (size_t)(pathp - b) : strlen(b);
    char *out = (char *)malloc(prefix + strlen(ref) + 1);
    memcpy(out, b, prefix);
    strcpy(out + prefix, ref);
    free(b);
    return out;
  }

  /* 相对路径:取 base 的目录部分(到最后一个 '/'),追加上 ref */
  const char *dir_end = strrchr(b, '/');
  size_t dirlen = dir_end ? (size_t)(dir_end - b + 1) : 0;
  char *out = (char *)malloc(dirlen + strlen(ref) + 1);
  memcpy(out, b, dirlen);
  strcpy(out + dirlen, ref);
  free(b);
  return out;
}

static int is_unreserved(char c) {
  return isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_' || c == '~';
}

char *tb_url_encode(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s);
  char *out = (char *)malloc(n * 3 + 1);
  size_t o = 0;
  static const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (is_unreserved((char)c)) {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 0xF];
    }
  }
  out[o] = '\0';
  return out;
}
