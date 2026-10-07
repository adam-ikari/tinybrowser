/* qzjs 接入 smoke:验证引擎底座本身可用。
 *
 * 这一层替代了旧的 test_qjs_embed.cc —— 那个文件直接 JS_NewRuntime() 测
 * quickjs-ng 本体,而 tinybrowser 现在不再直接嵌入 QuickJS(它归 qzjs 所有),
 * 测它等于测第三方依赖。真正值得钉住的是**我们真正依赖的那条链路**:
 * qz_create → 控制面 op:"eval"/"inspect" → 邮箱回执。
 */
#include <qzjs/qzjs.h>
#include <cJSON.h>
#include <gtest/gtest.h>
#include <string>
#include <memory>
#include <cstring>
#include <stdlib.h>

/* 发一条控制面命令并阻塞等回执。返回 malloc'd 回执 JSON(调用者 qz_free_message)。 */
static char *ctl(qz_t *rt, const std::string &cmd, const char *correl) {
  if (qz_control(rt, cmd.c_str(), cmd.size()) != 0) return nullptr;
  for (int i = 0; i < 8; i++) {
    char *json = nullptr;
    size_t len = 0;
    if (qz_recv_message(rt, &json, &len, 3000) != 0 || !json) return nullptr;
    cJSON *root = cJSON_Parse(json);
    const cJSON *co = root ? cJSON_GetObjectItemCaseSensitive(root, "correl") : nullptr;
    bool match = cJSON_IsString(co) && correl && co->valuestring &&
                 std::string(co->valuestring) == correl;
    if (match) {
      /* json 归调用者,但 root 是本次循环的临时解析树,必须在这里释放 ——
       * 否则每次命中回执都漏一份 cJSON 树(ASAN: 83 allocations)。 */
      cJSON_Delete(root);
      return json;
    }
    cJSON_Delete(root);
    qz_free_message(json);
  }
  return nullptr;
}

/* JSON 字符串转义:脚本里必然含引号/反斜杠,直接拼进命令会产出坏 JSON,
 * 换来一个 "bad json" 错误回执 —— 看起来像引擎坏了,其实是测试自己没转义。 */
static std::string jesc(const std::string &s) {
  std::string o;
  for (unsigned char c : s) {
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o += (char)c;
        }
    }
  }
  return o;
}

static std::string eval_cmd(const char *script, const char *correl) {
  return std::string("{\"op\":\"eval\",\"correl\":\"") + correl +
         "\",\"timeout_ms\":5000,\"script\":\"" + jesc(script) + "\"}";
}

static std::string inspect_cmd(const char *expr, const char *correl) {
  return std::string("{\"op\":\"inspect\",\"correl\":\"") + correl +
         "\",\"timeout_ms\":5000,\"expr\":\"" + jesc(expr) + "\"}";
}

/* 便捷:求值并把 result 字段取成字符串。失败返回 nullptr。 */
static std::string *eval_to_string(qz_t *rt, const char *script, const char *correl) {
  std::string cmd = eval_cmd(script, correl);
  char *receipt = ctl(rt, cmd, correl);
  if (!receipt) return nullptr;
  cJSON *root = cJSON_Parse(receipt);
  const cJSON *ok = root ? cJSON_GetObjectItemCaseSensitive(root, "ok") : nullptr;
  std::string *out = nullptr;
  if (cJSON_IsTrue(ok)) {
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (cJSON_IsString(r) && r->valuestring) out = new std::string(r->valuestring);
  }
  cJSON_Delete(root);
  qz_free_message(receipt);
  return out;
}

static qz_t *make_rt(const char *initial_script = nullptr) {
  qz_config_t cfg;
  qz_config_init(&cfg);
  cfg.control_plane = QZ_CONTROL_IN_PROC;
  cfg.strict_mode = 1;
  cfg.initial_script = initial_script;
  return qz_create(&cfg);
}

TEST(QzjsEmbed, CreatesAndEvaluates) {
  qz_t *rt = make_rt();
  ASSERT_NE(rt, nullptr);
  std::unique_ptr<std::string> r(
      eval_to_string(rt, "JSON.stringify(1 + 2)", "c1"));
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(*r, "3");
  qz_destroy(rt);
}

/* 字符串结果必须逐字节原样回来:脚本源码要过一层 JSON 转义(命令是 JSON),
 * 回执又再转义一层,两处都不能吃掉 引号 / 反斜杠 / 换行。 */
TEST(QzjsEmbed, StringResultComesBackUnescaped) {
  qz_t *rt = make_rt();
  ASSERT_NE(rt, nullptr);
  /* 脚本源码里要的是**转义序列** \n,不是真换行 —— 真换行进单引号字符串
 * 是 JS 语法错误,所以 C 字面量里写 \\n(反斜杠+n 两个字符)。 */
  std::unique_ptr<std::string> r(eval_to_string(rt, "'a\"b\\\\c\\nd'", "c2"));
  /* eval 不做 stringify:脚本求值结果直接进回执。字符串原样回来。 */
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(*r, "a\"b\\c\nd");
  qz_destroy(rt);
}

TEST(QzjsEmbed, SyntaxErrorIsReportedNotCrashed) {
  qz_t *rt = make_rt();
  ASSERT_NE(rt, nullptr);
  std::string cmd = eval_cmd("this is not ( valid js", "c3");
  char *receipt = ctl(rt, cmd, "c3");
  ASSERT_NE(receipt, nullptr);
  cJSON *root = cJSON_Parse(receipt);
  const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
  const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
  EXPECT_FALSE(cJSON_IsTrue(ok));
  ASSERT_TRUE(cJSON_IsString(err));
  EXPECT_GT(std::strlen(err->valuestring), 0u);
  cJSON_Delete(root);
  qz_free_message(receipt);
  qz_destroy(rt);
}

TEST(QzjsEmbed, InspectReturnsJsonText) {
  qz_t *rt = make_rt();
  ASSERT_NE(rt, nullptr);
  /* inspect 自带 JSON.stringify:expr 必须是**值**而非已序列化的字符串,
 * 否则会被 stringify 两次,回执 json 字段里多一层引号。 */
std::string cmd = inspect_cmd("({a:1,b:[2,3]})", "c4");
  char *receipt = ctl(rt, cmd, "c4");
  ASSERT_NE(receipt, nullptr);
  cJSON *root = cJSON_Parse(receipt);
  const cJSON *j = cJSON_GetObjectItemCaseSensitive(root, "json");
  ASSERT_TRUE(cJSON_IsString(j));
  cJSON *inner = cJSON_Parse(j->valuestring);
  ASSERT_NE(inner, nullptr);
  const cJSON *a = cJSON_GetObjectItemCaseSensitive(inner, "a");
  const cJSON *b = cJSON_GetObjectItemCaseSensitive(inner, "b");
  EXPECT_EQ(a->valueint, 1);
  ASSERT_TRUE(cJSON_IsArray(b));
  EXPECT_EQ(cJSON_GetArraySize(b), 2);
  cJSON_Delete(inner);
  cJSON_Delete(root);
  qz_free_message(receipt);
  qz_destroy(rt);
}

TEST(QzjsEmbed, InitialScriptRunsAtCreate) {
  qz_t *rt = make_rt("globalThis.__probe = 'boot-ok';");
  ASSERT_NE(rt, nullptr);
  std::unique_ptr<std::string> r(eval_to_string(rt, "__probe", "c5"));
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(*r, "boot-ok");
  qz_destroy(rt);
}

TEST(QzjsEmbed, ThrowSurfacesAsErrorReceipt) {
  qz_t *rt = make_rt();
  ASSERT_NE(rt, nullptr);
  std::string cmd = eval_cmd("throw new Error('boom')", "c6");
  char *receipt = ctl(rt, cmd, "c6");
  ASSERT_NE(receipt, nullptr);
  cJSON *root = cJSON_Parse(receipt);
  const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
  const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
  const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
  EXPECT_FALSE(cJSON_IsTrue(ok));
  ASSERT_TRUE(cJSON_IsString(code));
  EXPECT_STREQ(code->valuestring, "JS_EXCEPTION");
  ASSERT_TRUE(cJSON_IsString(err));
  EXPECT_NE(std::string(err->valuestring).find("boom"), std::string::npos);
  cJSON_Delete(root);
  qz_free_message(receipt);
  qz_destroy(rt);
}

/* 严格模式:跑的是第三方网页脚本,fs 必须默认全禁(sandbox_root 留 NULL)。 */
TEST(QzjsEmbed, StrictModeDeniesFs) {
  qz_t *rt = make_rt();
  ASSERT_NE(rt, nullptr);
  std::unique_ptr<std::string> r(eval_to_string(
      rt, "JSON.stringify(typeof fs === 'undefined' || !fs.readFileSync)", "c7"));
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(*r, "true");
  qz_destroy(rt);
}

/* TLS 必须真的编进库。qzjs 在 configure 期读 QZ_WITH_TLS 决定链不链 mbedTLS,
 * 而我们此前是在 add_subdirectory 之后才改 cache —— 已生成的构建规则不回溯,
 * 产出过「cache 显示 ON、实际 QZ_WITH_TLS=0」的假开关。CMake 层已有说明,
 * 这里从运行期再钉一道:编译期宏必须为 1,否则 https 分支整段被 #if 掉。
 *
 * 注意本测试**不联网**:真实 https fetch 曾因 uv_io 的握手完成路径缺陷而
 * 失败(见下),那是网络依赖,不适合放进单元测试。这里只验「TLS 被编进去了」。 */
TEST(QzjsEmbed, TlsIsCompiledIn) {
#ifndef QZ_WITH_TLS
  FAIL() << "QZ_WITH_TLS undefined — TLS support not compiled in";
#endif
  EXPECT_EQ(QZ_WITH_TLS, 1)
      << "QZ_WITH_TLS=0: HTTPS paths are #if'd out. Check that every "
         "QZ_WITH_* is written to the CMake cache BEFORE "
         "add_subdirectory(deps/qzjs) — see deps/VERSIONS.md.";
}