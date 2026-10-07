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

/* RFC 3986 §5.2.4 remove_dot_segments。
 * 输入是「以 / 开头、无 query/fragment」的路径;输出同样是这种形式。
 * 实现方式:按 '/' 切段,遇到 "." 丢弃、遇到 ".." 弹掉上一段(但不越过根),
 * 最后重新拼。段式实现比逐字符状态机好写得多,也不会出现「弹出时多删一段」
 * 这类下标错误 —— 本函数第一版就是逐字符写的,`/a/../b` 被算成 `/ab`。 */
static char *remove_dot_segments(const char *path) {
  if (!path) return NULL;
  /* 切成段(不含前导 '/',段间以 '\0' 分隔) */
  const char *p = path;
  while (*p == '/') p++;                 /* 跳过前导 '/' */

  size_t cap = strlen(p) / 2 + 4;        /* 段数上界,给足 */
  char **segs = (char **)malloc(cap * sizeof(char *));
  if (!segs) return NULL;
  size_t n = 0;
  while (*p) {
    const char *e = strchr(p, '/');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len == 1 && p[0] == '.') {
      /* 当前目录:丢弃 */
    } else if (len == 2 && p[0] == '.' && p[1] == '.') {
      /* 上一级:弹一段,但不越过根 */
      if (n > 0) { free(segs[--n]); }
    } else if (len > 0) {
      if (n < cap) {
        char *s = (char *)malloc(len + 1);
        if (!s) break;
        memcpy(s, p, len); s[len] = '\0';
        segs[n++] = s;
      }
    }
    if (!e) break;
    p = e + 1;
  }

  /* 重新拼成 "/a/b";空则 "/" */
  size_t total = 2;
  for (size_t i = 0; i < n; i++) total += strlen(segs[i]) + 1;
  char *out = (char *)malloc(total);
  if (out) {
    size_t o = 0;
    out[o++] = '/';
    for (size_t i = 0; i < n; i++) {
      if (i) out[o++] = '/';
      size_t l = strlen(segs[i]);
      memcpy(out + o, segs[i], l);
      o += l;
    }
    out[o] = '\0';
  }
  for (size_t i = 0; i < n; i++) free(segs[i]);
  free(segs);
  return out;
}

/* 把 base 归一化后拆成两段:
 *   *auth_out = "scheme://authority"(不含 path)
 *   *path_out = base 的 path(以 '/' 开头;base 无 path 时补 "/")
 * 调用方需分别 free。fragment 与 query 在这里就丢掉。 */
static int split_base(const char *base, char **auth_out, char **path_out,
                      char **query_out) {
  size_t n = strlen(base);
  char *b = (char *)malloc(n + 3);
  if (!b) return 0;
  memcpy(b, base, n + 1);
  b[n + 2] = '\0';
  char *f = strchr(b, '#'); if (f) *f = '\0';   /* fragment 总是丢弃 */
  /* query 必须留着:空 ref 与 "#f" 形式的解析要保留 base 的 query
   * (RFC 3986 5.2.2:空 ref 的 target = base 去掉 fragment)。 */
  char *q = strchr(b, '?');
  char *qs = NULL;
  if (q) {
    size_t ql = strlen(q);
    qs = (char *)malloc(ql + 1);
    if (qs) memcpy(qs, q, ql + 1);
    *q = '\0';
  }

  const char *auth = strstr(b, "//");
  const char *pathp = auth ? strchr(auth + 2, '/') : strchr(b, '/');
  if (!pathp) {
    /* base 没有 path(形如 "http://h")→ 视作 "/" */
    /* pathp 要在截断 query **之后**重新定位,否则 "?x=1" 会被当成 path。
       有 query 时把 '/' 插到 '?' 之前(memmove 右移),无 query 直接追加。 */
    if (!q) { b[n] = '/'; b[n + 1] = '\0'; pathp = b + n; }
    else    { memmove(q + 1, q, strlen(q) + 1); *q = '/'; pathp = q; }
  }
  size_t alen = (size_t)(pathp - b);
  char *a = (char *)malloc(alen + 1);
  if (!a) { free(b); free(qs); return 0; }
  memcpy(a, b, alen); a[alen] = '\0';
  size_t plen = strlen(pathp);
  char *p = (char *)malloc(plen + 1);
  if (!p) { free(a); free(b); free(qs); return 0; }
  memcpy(p, pathp, plen + 1);
  free(b);
  *auth_out = a;
  *path_out = p;
  *query_out = qs ? qs : dup_str("");
  return 1;
}

char *tb_url_resolve(const char *base, const char *ref) {
  if (!base || !ref) return NULL;
  if (has_scheme(ref)) return dup_str(ref);   /* 绝对 URL,原样返回 */

  char *auth = NULL, *bpath = NULL, *bquery = NULL;
  if (!split_base(base, &auth, &bpath, &bquery)) return NULL;

  char *out = NULL;

  if (ref[0] == '#') {
    /* 只有 fragment:沿用 base 的 path 与 query */
    size_t need = strlen(auth) + strlen(bpath) + strlen(bquery) + strlen(ref) + 1;
    out = (char *)malloc(need);
    if (out) snprintf(out, need, "%s%s%s%s", auth, bpath, bquery, ref);
  } else if (ref[0] == '?') {
    /* 只有 query:换掉 base 的 query,path 保留 */
    size_t need = strlen(auth) + strlen(bpath) + strlen(ref) + 1;
    out = (char *)malloc(need);
    if (out) snprintf(out, need, "%s%s%s", auth, bpath, ref);
  } else if (ref[0] == '/' && ref[1] == '/') {
    /* 协议相对 URL("//host/path"):继承 base 的 scheme,换掉 authority。
     * 真实页面极常见(<script src="//cdn.example/x.js">)。旧实现把它当成
     * 绝对路径,结果是 "https://a.test//b.test/z" —— **解析到了错误的 host**。 */
    char *sch = dup_str(auth);
    const char *se = sch ? strstr(sch, "://") : NULL;
    if (se) {
      size_t sl = (size_t)(se - sch);
      sch[sl] = '\0';
      /* ref+2 是 "host[:port]/path"。**只有 path 部分**能走 remove_dot_segments:
       * 若把整串(含 host)丢进去,host 会被当成路径第一段,遇到 ../ 就被弹掉 ——
       * 实测 "https://a.test/x/y" + "//b.test/z" 曾解析成 "https:///b.test/"。 */
      const char *hp = ref + 2;
      const char *slash = strchr(hp, '/');
      size_t hostlen = slash ? (size_t)(slash - hp) : strlen(hp);
      char *rp = slash ? remove_dot_segments(slash) : NULL;
      size_t need = sl + hostlen + (rp ? strlen(rp) : 0) + 4;
      out = (char *)malloc(need);
      if (out) snprintf(out, need, "%s://%.*s%s", sch, (int)hostlen, hp,
                        rp ? rp : "");
      free(rp);
    }
    free(sch);
  } else if (ref[0] == '/') {
    /* 绝对路径:换掉 base 的 path,其余保留 */
    char *rp = remove_dot_segments(ref);
    size_t need = strlen(auth) + strlen(ref) + 2;
    out = (char *)malloc(need);
    if (out) snprintf(out, need, "%s%s", auth, rp ? rp : ref);
    free(rp);
  } else if (ref[0] == '\0') {
    /* 空 ref:同一文档 = base 去掉 fragment(RFC 3986 5.2.2),query 保留。
     * 旧实现返回 base 的**根目录**,把路径整个丢了。 */
    size_t need = strlen(auth) + strlen(bpath) + strlen(bquery) + 1;
    out = (char *)malloc(need);
    if (out) snprintf(out, need, "%s%s%s", auth, bpath, bquery);
  } else {
    /* 相对路径:base 的 path 去掉最后一段(到最后一个 '/'),再拼 ref。
     * base 的 path 至少是 "/",故 dir 一定以 '/' 结尾。 */
    const char *slash = strrchr(bpath, '/');
    size_t dirlen = slash ? (size_t)(slash - bpath) + 1 : 1;
    size_t mlen = dirlen + strlen(ref) + 1;
    char *merged = (char *)malloc(mlen);
    if (merged) {
      memcpy(merged, bpath, dirlen);
      memcpy(merged + dirlen, ref, strlen(ref) + 1);
      char *rp = remove_dot_segments(merged);
      size_t need = strlen(auth) + strlen(rp ? rp : "") + 1;
      out = (char *)malloc(need);
      if (out) snprintf(out, need, "%s%s", auth, rp ? rp : "");
      free(rp);
      free(merged);
    }
  }

  free(auth);
  free(bpath);
  free(bquery);
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
