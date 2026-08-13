#include <quickjs.h>
#include <gtest/gtest.h>

TEST(QjsEmbed, EvalExpression) {
  JSRuntime *rt = JS_NewRuntime();
  ASSERT_NE(rt, nullptr);
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  JSValue r = JS_Eval(ctx, "1 + 2", 5, "<test>", JS_EVAL_TYPE_GLOBAL);
  int32_t v = 0;
  ASSERT_FALSE(JS_IsException(r));
  ASSERT_EQ(JS_ToInt32(ctx, &v, r), 0);
  EXPECT_EQ(v, 3);
  JS_FreeValue(ctx, r);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}

TEST(QjsEmbed, MemoryLimitWorks) {
  JSRuntime *rt = JS_NewRuntime();
  JSContext *ctx = JS_NewContext(rt);
  ASSERT_NE(ctx, nullptr);
  // 注意:限制须在 JS_NewContext 之后设置——context 创建本身的内存开销即可能
  // 超过 64KB,若先设限制则 JS_NewContext 返回 NULL,JS_Eval(NULL) 会崩溃。
  JS_SetMemoryLimit(rt, 64 * 1024);
  JSValue r = JS_Eval(ctx, "var a=[]; while(true) a.push('x'.repeat(1024));", 40, "<t>", JS_EVAL_TYPE_GLOBAL);
  EXPECT_TRUE(JS_IsException(r));
  JS_FreeValue(ctx, r);
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
}
