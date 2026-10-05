#include "cookie.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- HTTP-date 解析(RFC 1123 / RFC 850 / asctime 三种格式) ----
 * 只需认出 cookie Expires 用得上的格式;认不出返回 0,调用方按「无 Expires」
 * 处理(session cookie),与浏览器对坏日期的宽松处理一致。 */
static const char *MONTHS[12] = {"jan","feb","mar","apr","may","jun",
                                 "jul","aug","sep","oct","nov","dec"};

static long long parse_http_date(const char *s) {
  if (!s) return 0;
  /* 找 "日 月 年" 里的月与年:两种格式的共同点是第 2 个非空白 token 是缩写月,
     且其后 4 位数字是年。 */
  int mon = -1, year = 0, day = 0, hh = 0, mm = 0, ss = 0;

  /* 跳过 weekday("Wed," / "Wednesday,"),它之后才是日期 */
  const char *p = s;
  while (*p && isalpha((unsigned char)*p)) p++;
  while (*p == ',' || *p == ' ' || *p == '\t') p++;

  if (!isdigit((unsigned char)*p)) return 0;      /* 期望 2-digit day */
  day = (*p++ - '0') * 10;
  if (!isdigit((unsigned char)*p)) return 0;
  day += *p++ - '0';
  while (*p == '-' || *p == ' ' || *p == '\t') p++;

  if (!isalpha((unsigned char)*p)) return 0;
  for (int i = 0; i < 12; i++) {
    if (strncasecmp(p, MONTHS[i], 3) == 0) { mon = i; break; }
  }
  if (mon < 0) return 0;
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;

  if (!isdigit((unsigned char)*p)) return 0;
  for (int i = 0; i < 4; i++) {
    if (!isdigit((unsigned char)*p)) return 0;
    year = year * 10 + (*p++ - '0');
  }
  if (year < 100) year += (year < 70) ? 2000 : 1900;
  if (*p == ' ') p++;
  if (isdigit((unsigned char)*p)) {
    hh = (*p++ - '0') * 10;
    if (isdigit((unsigned char)*p)) hh += *p++ - '0';
    if (*p == ':') p++;
    if (isdigit((unsigned char)*p)) {
      mm = (*p++ - '0') * 10;
      if (isdigit((unsigned char)*p)) mm += *p++ - '0';
    }
    if (*p == ':' && isdigit((unsigned char)*(p + 1))) {
      p++;
      ss = (*p++ - '0') * 10;
      if (isdigit((unsigned char)*p)) ss += *p++ - '0';
    }
  }

  /* timegm:不依赖本地 TZ,避免跨时区把过期时间算错。 */
  static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
  long long y = year;
  long long days = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += cum[mon];
  if (mon > 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days++;
  days += day - 1;
  return days * 86400LL + hh * 3600LL + mm * 60LL + ss;
}

typedef struct {
  char *name;
  char *value;
  char *domain;     /* 已规范化:小写,不带前导 '.' */
  int host_only;    /* 1 = Set-Cookie 未写 Domain 属性 → 只能精确匹配 host。
                       RFC 6265 5.2.2/5.3:host-only cookie **不得**发给子域。
                       缺省 Domain 时若走后缀匹配,example.test 设的 cookie 会
                       发到 sub.example.test —— 这是真实的越界,单测抓到过。 */
  char *path;
  int secure;       /* Secure 属性 */
  int http_only;    /* HttpOnly 属性:JS 不可见 */
  long long expires;/* 绝对秒数;0 = session cookie */
} cookie;

struct tb_cookie_jar {
  cookie *items;
  int n, cap;
};

/* ---- 小工具 ---- */

static char *dup_str(const char *s) {
  size_t n = strlen(s);
  char *o = (char *)malloc(n + 1);
  if (o) memcpy(o, s, n + 1);
  return o;
}

/* 就地小写。cookie 的 domain 比较必须大小写不敏感。 */
static void to_lower(char *s) {
  for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static char *str_trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  char *e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = '\0';
  return s;
}

/* 属性名精确比较(大小写不敏感)。**不能**用前缀比较:那会让 `pathological=x`
 * 被当成 `path`、`domainfoo=1` 被当成 `domain`,于是凭空的属性被采纳成
 * domain/path。浏览器按精确名匹配,这里也该如此。 */
static int attr_is(const char *name, const char *want) {
  return strcasecmp(name, want) == 0;
}

/* ---- URL 拆解(只要 scheme/host/path,够 cookie 用) ---- */

typedef struct { char scheme[16]; char host[256]; char path[512]; } url_parts;

static int url_split(const char *url, url_parts *out) {
  if (!url) return 0;
  memset(out, 0, sizeof *out);
  const char *p = strstr(url, "://");
  if (!p || (size_t)(p - url) >= sizeof out->scheme) return 0;
  size_t sl = (size_t)(p - url);
  memcpy(out->scheme, url, sl);
  out->scheme[sl] = '\0';
  to_lower(out->scheme);
  const char *host = p + 3;
  const char *hend = host;
  while (*hend && *hend != '/' && *hend != '?' && *hend != '#') hend++;
  size_t hl = (size_t)(hend - host);
  if (hl == 0 || hl >= sizeof out->host) return 0;
  memcpy(out->host, host, hl);
  out->host[hl] = '\0';
  to_lower(out->host);
  if (*hend == '/') {
    size_t pl = strcspn(hend, "?#");
    if (pl >= sizeof out->path) pl = sizeof out->path - 1;
    memcpy(out->path, hend, pl);
    out->path[pl] = '\0';
  } else {
    out->path[0] = '/';
    out->path[1] = '\0';
  }
  return 1;
}

static int is_https(const url_parts *u) { return strcmp(u->scheme, "https") == 0; }

/* ---- domain 匹配 ---- */

/* req_host 与 cookie_domain 是否匹配 cookie。
 * cookie_domain 不带前导 '.'(规范化过)。 */
static int domain_match(const char *req_host, const char *cookie_domain) {
  if (strcmp(req_host, cookie_domain) == 0) return 1;
  size_t cl = strlen(cookie_domain);
  /* 后缀匹配:req_host 必须以 "." + cookie_domain 结尾,且前面有实义前缀。 */
  if (cl + 1 < strlen(req_host)) {
    const char *tail = req_host + (strlen(req_host) - cl - 1);
    if (tail[0] == '.' && strcasecmp(tail + 1, cookie_domain) == 0) return 1;
  }
  return 0;
}

/* Set-Cookie 的 Domain 属性:合法则规范化返回,非法(含 public suffix 风险)返回 NULL。
 * attr 为 NULL/空 → *host_only 置 1(缺省即 host-only,精确匹配)。
 * attr 非空     → *host_only 置 0(按后缀匹配,可发往子域)。 */
static char *cookie_domain_attr(const char *attr, const char *req_host,
                                int *host_only) {
  *host_only = 0;
  if (!attr || !attr[0]) { *host_only = 1; return dup_str(req_host); }
  const char *d = attr;
  while (*d == '.') d++;                       /* 前导 '.' 忽略 */
  if (!*d) return NULL;
  char *low = dup_str(d);
  if (!low) return NULL;
  to_lower(low);
  /* 必须是请求 host 本身或其父域(否则是跨站声明,拒绝)。 */
  if (!domain_match(req_host, low) && strcasecmp(req_host, low) != 0) {
    free(low);
    return NULL;
  }
  return low;
}

/* ---- path 匹配 ---- */

/* cookie_path 是否适用于 req_path(RFC 6265 5.1.4 的路径前缀规则)。 */
static int path_match(const char *req_path, const char *cookie_path) {
  if (!cookie_path || !cookie_path[0]) return 1;
  if (strcmp(req_path, cookie_path) == 0) return 1;
  size_t n = strlen(cookie_path);
  if (strncmp(req_path, cookie_path, n) != 0) return 0;
  /* 前缀必须落在路径分量边界上:cookie_path 以 '/' 结尾,或 req 的下一字符是 '/' */
  if (cookie_path[n - 1] == '/') return 1;
  return req_path[n] == '/' || req_path[n] == '\0';
}

/* Set-Cookie 的 Path 属性:缺省取请求 path 的目录部分。 */
static char *cookie_path_attr(const char *attr, const char *req_path) {
  if (attr && attr[0] == '/') return dup_str(attr);
  const char *last = strrchr(req_path, '/');
  if (!last || last == req_path) return dup_str("/");
  size_t n = (size_t)(last - req_path);
  char *o = (char *)malloc(n + 1);
  if (!o) return NULL;
  memcpy(o, req_path, n);
  o[n] = '\0';
  return o;
}

/* ---- jar 生命周期 ---- */

tb_cookie_jar *tb_cookie_jar_new(void) {
  return (tb_cookie_jar *)calloc(1, sizeof(tb_cookie_jar));
}

void tb_cookie_jar_free(tb_cookie_jar *j) {
  if (!j) return;
  for (int i = 0; i < j->n; i++) {
    free(j->items[i].name); free(j->items[i].value);
    free(j->items[i].domain); free(j->items[i].path);
  }
  free(j->items);
  free(j);
}

int tb_cookie_jar_count(tb_cookie_jar *j) { return j ? j->n : 0; }

/* ---- 解析与存储 ---- */

void tb_cookie_jar_set(tb_cookie_jar *j, const char *raw, const char *req_url) {
  if (!j || !raw || !req_url) return;
  url_parts u;
  if (!url_split(req_url, &u)) return;

  /* 拆「第一个分号前」= name=value;其余是属性 */
  const char *semi = strchr(raw, ';');
  size_t nvlen = semi ? (size_t)(semi - raw) : strlen(raw);
  if (nvlen == 0) return;
  const char *eq = memchr(raw, '=', nvlen);
  if (!eq) return;                       /* 没有 '=' → 不是 cookie,丢弃 */

  char *nv = (char *)malloc(nvlen + 1);
  if (!nv) return;
  memcpy(nv, raw, nvlen);
  nv[nvlen] = '\0';
  char *name = nv;
  char *value = strchr(nv, '=');
  *value++ = '\0';
  name = str_trim(name);
  value = str_trim(value);
  if (!*name) { free(nv); return; }     /* 空名 → 丢弃 */

  /* 属性扫描 */
  char *attr_domain = NULL, *attr_path = NULL;
  int secure = 0, http_only = 0, host_only = 0;
  long long max_age = 0, expires = 0;   /* 0 = 未指定 */
  int have_max_age = 0;

  while (semi) {
    semi++;
    const char *comma = strchr(semi, ';');
    size_t alen = comma ? (size_t)(comma - semi) : strlen(semi);
    char *a = (char *)malloc(alen + 1);
    if (!a) break;
    memcpy(a, semi, alen);
    a[alen] = '\0';
    char *eq2 = strchr(a, '=');
    char *aname = a, *aval = NULL;
    if (eq2) { *eq2 = '\0'; aval = eq2 + 1; }
    aname = str_trim(aname);
    if (aval) aval = str_trim(aval);

    /* 重复属性(如 `a=1; domain=x; domain=y`)按后者胜 —— 与浏览器一致。
       覆盖前必须释放前一个副本,否则泄漏(此前 attr_domain/attr_path 被直接
       覆盖丢弃)。 */
    if (attr_is(aname, "domain")) {
      free(attr_domain);
      attr_domain = aval ? dup_str(aval) : NULL;
    } else if (attr_is(aname, "path")) {
      free(attr_path);
      attr_path = aval ? dup_str(aval) : NULL;
    }
    else if (attr_is(aname, "secure"))   secure = 1;
    else if (attr_is(aname, "httponly")) http_only = 1;
    else if (attr_is(aname, "max-age")) { if (aval) { max_age = atoll(aval); have_max_age = 1; } }
    else if (attr_is(aname, "expires")) { if (aval) expires = parse_http_date(aval); }
    free(a);
    semi = comma;
  }

  char *dom = cookie_domain_attr(attr_domain, u.host, &host_only);
  if (!dom) {                       /* 非法 domain → 整条丢弃 */
    free(nv); free(attr_domain); free(attr_path);
    return;
  }
  char *pth = cookie_path_attr(attr_path, u.path);
  if (!pth) pth = dup_str("/");

  /* 计算过期时刻。Max-Age 优先于 Expires(RFC 6265 5.2.2)。
     Max-Age <= 0 意味着立即删除。 */
  long long expiry = 0;
  int delete_now = 0;
  if (have_max_age) {
    if (max_age <= 0) delete_now = 1;
    else expiry = (long long)time(NULL) + max_age;
  } else if (expires > 0) {
    expiry = expires;
    if (expiry <= (long long)time(NULL)) delete_now = 1;
  }

  /* 替换同 (name, domain, path) 的既有条目 */
  int idx = -1;
  for (int i = 0; i < j->n; i++) {
    cookie *c = &j->items[i];
    if (strcmp(c->name, name) == 0 && strcmp(c->domain, dom) == 0 &&
        strcmp(c->path, pth) == 0) { idx = i; break; }
  }

  if (delete_now) {
    if (idx >= 0) {           /* 过期即删 */
      free(j->items[idx].name); free(j->items[idx].value);
      free(j->items[idx].domain); free(j->items[idx].path);
      memmove(&j->items[idx], &j->items[idx + 1],
              (size_t)(j->n - idx - 1) * sizeof(cookie));
      j->n--;
    }
  } else if (idx >= 0) {
    free(j->items[idx].value);
    j->items[idx].value = strdup(value);
    j->items[idx].host_only = host_only;
    j->items[idx].secure = secure;
    j->items[idx].http_only = http_only;
    j->items[idx].expires = expiry;
  } else {
    if (j->n == j->cap) {
      int nc = j->cap ? j->cap * 2 : 8;
      cookie *bigger = (cookie *)realloc(j->items, (size_t)nc * sizeof(cookie));
      if (!bigger) goto bail;
      j->items = bigger;
      j->cap = nc;
    }
    cookie *c = &j->items[j->n];
    c->name = strdup(name);
    c->value = strdup(value);
    c->domain = dom; dom = NULL;
    c->host_only = host_only;
    c->path = pth; pth = NULL;
    c->secure = secure;
    c->http_only = http_only;
    c->expires = expiry;
    j->n++;
  }
bail:
  free(nv); free(attr_domain); free(attr_path); free(dom); free(pth);
}

/* ---- 取出 ---- */

/* 收集匹配项。visible_only=1 时排除 HttpOnly。 */
static char *collect(tb_cookie_jar *j, const char *req_url, int visible_only) {
  url_parts u;
  if (!j || !url_split(req_url, &u)) return strdup("");
  long long now = (long long)time(NULL);

  /* 先做一遍过期清理(惰性删除:不单独跑后台任务) */
  for (int i = 0; i < j->n; ) {
    if (j->items[i].expires && j->items[i].expires <= now) {
      free(j->items[i].name); free(j->items[i].value);
      free(j->items[i].domain); free(j->items[i].path);
      memmove(&j->items[i], &j->items[i + 1],
              (size_t)(j->n - i - 1) * sizeof(cookie));
      j->n--;
    } else i++;
  }

  size_t cap = 256, len = 0;
  char *out = (char *)malloc(cap);
  if (!out) return NULL;
  out[0] = '\0';
  for (int i = 0; i < j->n; i++) {
    cookie *c = &j->items[i];
    if (visible_only && c->http_only) continue;
    if (c->secure && !is_https(&u)) continue;
    /* host-only cookie 只认精确 host;带 Domain 属性的才走后缀匹配。 */
    if (c->host_only ? (strcmp(u.host, c->domain) != 0)
                     : !domain_match(u.host, c->domain)) continue;
    if (!path_match(u.path, c->path)) continue;
    size_t need = strlen(c->name) + strlen(c->value) + 4;
    while (len + need + 1 > cap) {
      cap *= 2;
      char *bigger = (char *)realloc(out, cap);
      if (!bigger) { free(out); return NULL; }
      out = bigger;
    }
    if (len) out[len++] = ';', out[len++] = ' ';
    size_t kl = strlen(c->name), vl = strlen(c->value);
    memcpy(out + len, c->name, kl); len += kl;
    out[len++] = '=';
    memcpy(out + len, c->value, vl); len += vl;
    out[len] = '\0';
  }
  return out;
}

char *tb_cookie_jar_header(tb_cookie_jar *j, const char *req_url) {
  return collect(j, req_url, 0);
}

char *tb_cookie_jar_visible(tb_cookie_jar *j, const char *req_url) {
  return collect(j, req_url, 1);
}