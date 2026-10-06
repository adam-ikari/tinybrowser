/* M2a 端到端集成测试:真 HTTP server + 真 qzjs 引擎 + 真 curl transport。
 *
 * 与 tests/unit/test_js_engine.cc 的分工:
 *   - unit/  直接驱 engine seam(open/load_document/render),不碰网络。验的是
 *            「引擎能否加载一段 HTML 字符串」。
 *   - 这里    走完整浏览器路径(tb_navigate → transport → nav_on_done → engine),
 *            验的是「导航一次,脚本/console/定时器在真实链路里是否成立」。
 *            单元测试绕过的那一段 —— nav_on_done 的引擎接线、tb_pump 的驱动、
 *            cfg.on_console 的路由 —— 恰好是最容易接错的地方。
 */
#include "tb.h"
#include "view.h"
#include "browser_internal.h"   /* b->engine / b->js_doc:重渲只能经 engine seam */
#include "http_server.h"
#include "guard_transport.h"     /* 结构性断言:引擎工作不在传输回调栈内 */
#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

struct ConsoleCapture {
  std::vector<std::string> lines;
};

void on_console(tb_browser *, const char *level, const char *msg, void *ud) {
  static_cast<ConsoleCapture *>(ud)->lines.push_back(std::string(level) + ":" + msg);
}

bool has_console(const ConsoleCapture &c, const char *needle) {
  for (const auto &l : c.lines)
    if (l.find(needle) != std::string::npos) return true;
  return false;
}

/* console 帧走 qzjs 邮箱,派发发生在控制面往返途中或 poll_timers 里。导航本身
 * 会跑好几趟控制面(begin / finish / render 的 inspect),所以大部分消息在
 * tb_wait_idle 返回时已经派发;剩余的在显式 pump 后补齐。断言前统一 pump 几轮,
 * 测试就不依赖「消息恰好在第几趟往返到达」这种时序。 */
void pump_rounds(tb_browser *b, int rounds, uint32_t step_ms) {
  for (int i = 0; i < rounds; i++) tb_pump(b, step_ms);
}

tb_browser *make_browser(ConsoleCapture *cc, uint32_t idle_grace_ms = 300) {
  tb_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.idle_grace_ms = idle_grace_ms;
  cfg.user_agent = "tb-m2a-test";
  cfg.on_console = on_console;
  cfg.ud = cc;
  tb_browser *b = tb_create(&cfg);   /* 无显式 transport → 默认 curl */
  EXPECT_NE(b, nullptr);
  return b;
}

}  // namespace

/* 同步脚本跑完的可见效果:title 与 DOM 插入都落在同一张视图上。
 * 顺序要求:脚本在 render 之前执行,故视图反映的是「脚本改完」的 DOM。 */
TEST(M2aIntegration, SyncScriptMutatesDom) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Static</title>"
             "<body><p>static</p>"
             "<script>document.title = 'ByScript';"
             "var p = document.createElement('p');"
             "p.textContent = 'injected';"
             "document.body.appendChild(p);</script>"
             "</body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  pump_rounds(b, 3, 10);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  ASSERT_NE(v, nullptr);
  EXPECT_STREQ(tb_view_title(v), "ByScript");
  EXPECT_NE(strstr(tb_view_text(v), "static"), nullptr);
  EXPECT_NE(strstr(tb_view_text(v), "injected"), nullptr);
  /* 插入的节点在原节点之后:文档序 static 先、injected 后 */
  const char *a = strstr(tb_view_text(v), "static");
  const char *b2 = strstr(tb_view_text(v), "injected");
  EXPECT_LT(a, b2);
  tb_view_free(v);
  tb_destroy(b);
}

/* 脚本抛异常不得吃掉整页:渲染照常,异常信息进 console(error 级)。 */
TEST(M2aIntegration, ScriptErrorIsolated) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Err</title>"
             "<body><p>still ok</p>"
             "<script>throw new Error('boom');</script>"
             "</body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  pump_rounds(b, 3, 10);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Err");
  EXPECT_NE(strstr(tb_view_text(v), "still ok"), nullptr);
  tb_view_free(v);
  EXPECT_TRUE(has_console(cc, "boom")) << "console 帧: " << cc.lines.size();
  tb_destroy(b);
}

/* console 各级别都要能穿过导航路径路由到 cfg.on_console。 */
TEST(M2aIntegration, ConsoleBridgeRouted) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Con</title><body><p>x</p>"
             "<script>console.log('hello from js');"
             "console.info('an info');"
             "console.warn('a warning');</script></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  pump_rounds(b, 5, 10);

  EXPECT_TRUE(has_console(cc, "log:hello from js"));
  EXPECT_TRUE(has_console(cc, "info:an info"));
  EXPECT_TRUE(has_console(cc, "warn:a warning"));
  tb_destroy(b);
}

/* setTimeout 由 qzjs polyfill 在库自有 loop 上触发,宿主 pump 之后回调应已生效。
 *
 * 视图刷新由宿主自动完成(见 nav_refresh_view),所以这里直接读 tb_observe 的
 * 结果,不再手动过 engine seam 重渲 —— 那正是本用例当初要绕开的缺陷。
 * 同样的行为在 BrowserApi.TimerMutationRefreshesView 里也有覆盖,那边还多
 * 断言了「不刷新就永远看不到」的反面。 */
TEST(M2aIntegration, TimerFiresThroughPump) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>T</title><body><p>x</p>"
             "<script>setTimeout(function(){"
             "  var p = document.createElement('p');"
             "  p.textContent = 'ticked';"
             "  document.body.appendChild(p);"
             "}, 50);</script></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  bool ticked = false;
  for (int i = 0; i < 50 && !ticked; i++) {
    tb_pump(b, 20);
    tb_view *v2 = nullptr;
    if (tb_observe(b, &v2).code == 0) {
      ticked = strstr(tb_view_text(v2), "ticked") != nullptr;
      tb_view_free(v2);
    }
  }
  EXPECT_TRUE(ticked) << "setTimeout 回调 1s 内未生效";
  tb_destroy(b);
}

/* tb_eval_js 在已加载文档上求值,能读到页面脚本留下的状态。 */
TEST(M2aIntegration, EvalJsOnLoadedDocument) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Loaded</title>"
             "<body><p>hello</p>"
             "<script>window.__marker = 42;"
             "document.body.setAttribute('data-x', 'y');</script>"
             "</body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "document.title", &out).code, 0);
  EXPECT_STREQ(out, "Loaded");
  free(out);

  /* 页面脚本定义的全局在后续 eval 里可见(同一份 JS 全局,未随导航重置)。 */
  ASSERT_EQ(tb_eval_js(b, "String(window.__marker)", &out).code, 0);
  EXPECT_STREQ(out, "42");
  free(out);

  ASSERT_EQ(tb_eval_js(b, "document.querySelector('p').textContent", &out).code, 0);
  EXPECT_STREQ(out, "hello");
  free(out);

  tb_destroy(b);
}

/* <script src> 走两段式:__tb_begin_load 返回 src 列表 → 宿主用 transport 抓取
 * → __tb_finish_load 执行。抓取失败不应中断页面渲染。 */
TEST(M2aIntegration, ExternalScriptSrc) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Ext</title><body><p>host</p>"
             "<script src='/ext.js'></script></body>"}},
      {"/ext.js", {200, "application/javascript",
                   "var el = document.createElement('p');"
                   "el.textContent = 'from ext';"
                   "document.body.appendChild(el);"}},
      {"/missing.js", {404, "text/plain", "nope"}},
      {"/missing", {200, "text/html",
                    "<!DOCTYPE html><title>Miss</title><body><p>kept</p>"
                    "<script src='/missing.js'></script></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Ext");
  EXPECT_NE(strstr(tb_view_text(v), "host"), nullptr);
  EXPECT_NE(strstr(tb_view_text(v), "from ext"), nullptr);
  tb_view_free(v);

  /* 抓不到 src(404):页面仍渲染,只是脚本没跑。 */
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/missing").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Miss");
  EXPECT_NE(strstr(tb_view_text(v), "kept"), nullptr);
  tb_view_free(v);

  tb_destroy(b);
}

/* reentrancy 回归:引擎工作不得发生在传输回调栈内。
 *
 * 曾经的 bug:nav_on_done 直接在传输回调里跑 load_document/render,而
 * <script src> 的抓取(tb_load_sync)会同步调 transport->open/poll。对 curl
 * 传输而言此刻正坐在 curl_multi_info_read 的循环里 —— 在 curl 自己的栈上
 * 重入 curl_multi_add_handle,UB。
 *
 * 这里用 guard_transport 直接测量「回调执行期间有没有人再调 open/poll」。
 * 断言写成两条,缺一不可:
 *   reentrant_calls == 0   ← 真正的门禁
 *   callback_count  >= 2   ← 保证这条路径真被走到了(否则 0 只是因为没触发)
 * 少了后一条,测试在 bug 回归时会「因为没进那条路」而假绿。 */
TEST(M2aIntegration, EngineWorkStaysOutsideTransportCallback) {
  guard_transport gt;
  gt.init();
  gt.body =
      "<!DOCTYPE html><title>Re</title><body><p>host</p>"
      "<script src='/app.js'></script></body>";
  gt.content_type = "text/html";

  tb_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "tb-m2a-test";
  cfg.idle_grace_ms = 0;
  cfg.transport = &gt.base;      /* 注入观测用 transport */
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, "http://example.test/page").code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  EXPECT_EQ(gt.reentrant_calls, 0)
      << "引擎工作在传输回调栈内做了同步网络往返 —— 这就是 UAF/reentrancy 的根";
  EXPECT_GE(gt.callback_count, 2)
      << "只派发了 " << gt.callback_count
      << " 次回调;<script src> 的子资源请求没发出,本用例没测到想测的东西";

  /* 顺带确认页面确实渲染出来了(否则「没重入」可能只是因为压根没加载)。 */
  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Re");
  tb_view_free(v);

  tb_destroy(b);
}

/* 未及提交的导航在 tb_destroy 时必须被收走:nav_ctx 自己 malloc,还带着整页
 * body。导航中直接销毁(不 wait_idle)是最容易漏的路径。 */
TEST(M2aIntegration, DestroyDuringNavigationFreesPendingCtx) {
  guard_transport gt;
  gt.init();
  /* open 立刻完成、poll 才派发,所以 tb_create 之后立刻 destroy,
     on_done 还没跑过 —— 但 op 已 active,正是「在飞」的状态。 */
  gt.body = "<title>Pending</title><p>x</p>";

  tb_config cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.user_agent = "tb-m2a-test";
  cfg.transport = &gt.base;
  tb_browser *b = tb_create(&cfg);
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(tb_navigate(b, "http://example.test/pending").code, 0);
  tb_destroy(b);   /* 不得崩、不得漏 */
}

/* ---- 浏览器 API:cookie / location / history / navigator ----
 *
 * 这一组验的是「补齐 API」那轮的产出。三条边界都必须测到:
 *   - 读侧(宿主 push __tb_env):location.href 等纯本地读,无控制面往返
 *   - 写侧(JS → mailbox → 宿主):location.href= / history.back()
 *   - 真网络往返:cookie 不只是「jar 里有」,还要「真的发出去了」
 */

/* cookie 端到端:响应的 Set-Cookie 收进 jar,后续请求真的带上 Cookie 头。
 * 用 /echo-cookie 回显服务端实际收到的 Cookie —— 只断言 jar 内容是不够的,
 * jar 与请求头之间的连线只有真发一次请求才验证得到。 */
TEST(BrowserApi, CookieRoundTripsOverRealRequests) {
  HttpServer srv({
      {"/set", {200, "text/html", "<title>Set</title><p>set page</p>",
                "Set-Cookie: sid=abc123; Path=/\r\n"
                "Set-Cookie: pref=dark; Path=/\r\n"}},
      {"/echo-cookie", {200, "text/plain", ""}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  /* 第一次访问 /set:响应带两条 Set-Cookie */
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/set").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  /* 第二次访问 /echo:服务端的正文就是它收到的 Cookie 头 */
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/echo-cookie").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  std::string got = tb_view_text(v);
  EXPECT_NE(got.find("sid=abc123"), std::string::npos) << "实际收到: " << got;
  EXPECT_NE(got.find("pref=dark"), std::string::npos) << "实际收到: " << got;
  tb_view_free(v);
  tb_destroy(b);
}

/* document.cookie 之前是假实现:__tb_cookie 从未定义,读恒为 ""、写静默失效。
 * 现在读侧由宿主 push、值要在页面脚本执行时就已就位。 */
TEST(BrowserApi, DocumentCookieReadsServerCookiesInScript) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>C</title><body><p>x</p>"
             "<script>document.title = 'saw:' + (document.cookie.indexOf('sid=xyz') >= 0);</script>"
             "</body>",
             "Set-Cookie: sid=xyz; Path=/\r\n"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  /* 关键是「true」:脚本执行那一刻 document.cookie 就得含 sid=xyz,
     这要求宿主在 load_document 之前就把 env 推好(见 js_push_env 调用点)。 */
  EXPECT_STREQ(tb_view_title(v), "saw:true");
  tb_view_free(v);
  tb_destroy(b);
}

/* HttpOnly:HTTP 上照发,但页面脚本读不到 document.cookie。 */
TEST(BrowserApi, HttpOnlyCookieInvisibleToScript) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>H</title><body><p>x</p>"
             "<script>document.title = 'leak:' + (document.cookie.indexOf('secret=1') >= 0);</script>"
             "</body>",
             "Set-Cookie: secret=1; HttpOnly; Path=/\r\n"
             "Set-Cookie: plain=1; Path=/\r\n"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "leak:false");
  tb_view_free(v);
  tb_destroy(b);
}

/* document.cookie 写侧:JS 赋值要真进 jar,并在后续请求发出。 */
TEST(BrowserApi, DocumentCookieWriteReachesJar) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>W</title><body><p>x</p>"
             "<script>document.cookie = 'fromjs=1; path=/';</script></body>"}},
      {"/echo-cookie", {200, "text/plain", ""}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  /* 写入走 mailbox,须 pump 到宿主取出 —— 否则静默丢失。 */
  for (int i = 0; i < 5; i++) tb_pump(b, 20);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/echo-cookie").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_NE(std::string(tb_view_text(v)).find("fromjs=1"), std::string::npos)
      << "实际收到: " << tb_view_text(v);
  tb_view_free(v);
  tb_destroy(b);
}

/* location 的读侧:各派生属性必须与 href 自洽。全部在页面脚本里断言,
 * 这样测的是「脚本看到的 location」,而不是宿主内部状态。
 *
 * 路由 key 不含 fragment:fragment 不会发给服务端,浏览器请求的是
 * "/a/b?x=1&y=2"。而 location.hash 仍应从 effective URL 里读出 #frag ——
 * 这正是要验的点(fragment 不上网但浏览器知道它)。 */
TEST(BrowserApi, LocationPropertiesAreConsistent) {
  HttpServer srv({
      {"/a/b?x=1&y=2", {200, "text/html",
             "<!DOCTYPE html><title>L</title><body><p>x</p>"
             "<script>"
             "var l = location;"
             "document.title = [l.protocol, l.hostname, l.pathname,"
             "                  l.search, l.hash].join('|');"
             "</script></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/a/b?x=1&y=2#frag").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  std::string want = "http:|127.0.0.1|/a/b|?x=1&y=2|#frag";
  EXPECT_STREQ(tb_view_title(v), want.c_str());
  tb_view_free(v);
  tb_destroy(b);
}

/* location 的写侧:脚本赋值 location.href 要真的导航过去。 */
TEST(BrowserApi, LocationAssignmentNavigates) {
  HttpServer srv({
      {"/from", {200, "text/html",
                 "<!DOCTYPE html><title>From</title><body><p>from</p>"
                 "<script>location.href = '/to';</script></body>"}},
      {"/to", {200, "text/html", "<title>To</title><p>arrived</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/from").c_str()).code, 0);
  /* 导航由 mailbox 里的指令发起,pump 到它落地。 */
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  for (int i = 0; i < 5; i++) tb_pump(b, 20);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "To");
  EXPECT_NE(strstr(tb_view_text(v), "arrived"), nullptr);
  tb_view_free(v);
  tb_destroy(b);
}

/* history.back()/forward() 要真的驱动 session 栈。 */
TEST(BrowserApi, HistoryBackAndForward) {
  HttpServer srv({
      {"/one", {200, "text/html", "<title>One</title><p>one</p>"}},
      {"/two", {200, "text/html", "<title>Two</title><p>two</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/one").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  ASSERT_EQ(tb_navigate(b, (srv.base() + "/two").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  /* 宿主层的历史栈:back 必须能回到第一页 */
  ASSERT_EQ(tb_back(b).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "One");
  tb_view_free(v);

  ASSERT_EQ(tb_forward(b).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Two");
  tb_view_free(v);
  tb_destroy(b);
}

/* navigator.userAgent:qzjs 自带 navigator,我们只补 userAgent/platform,
 * 不能覆盖掉 qzjs 提供的其它属性。 */
TEST(BrowserApi, NavigatorExposesUserAgent) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>N</title><body><p>x</p>"
             "<script>document.title = navigator.userAgent;</script></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "tb-m2a-test");   /* = make_browser 里设的 UA */
  tb_view_free(v);

  /* 不能把 qzjs 原有的 navigator 属性弄丢 */
  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(typeof navigator.platform)", &out).code, 0);
  EXPECT_STREQ(out, "string");
  free(out);
  tb_destroy(b);
}

/* 定时器改 DOM 后宿主视图要自动刷新。
 *
 * 这是 M2a 遗留清单里的一项:qzjs 的定时器在它自有 loop 上跑,回调改了 DOM,
 * 但宿主视图是导航时渲染的那一张,不会自己更新。真实页面大量依赖这个
 * (轮询状态、动画、倒计时),不刷新等于定时器白写。
 *
 * 注意不要断言「定时器不能在导航期间触发」:qzjs 的 loop 与宿主并行,
 * 导航过程本身有多趟控制面往返(推 env / begin / finish / render),
 * 挂钟时间足够让短延时到期 —— 真实浏览器在主线程被阻塞时也一样。
 * 要验的是「最终视图反映了定时器的改动」,这才是宿主该保证的事。 */
TEST(BrowserApi, TimerMutationRefreshesView) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Tick</title><body><p>start</p>"
             "<script>setTimeout(function(){"
             "  var p = document.createElement('p');"
             "  p.textContent = 'ticked';"
             "  document.body.appendChild(p);"
             "}, 30);</script></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  /* pump 到定时器到期。不刷新视图的话这里永远看不到 ticked。 */
  bool ticked = false;
  for (int i = 0; i < 60 && !ticked; i++) {
    tb_pump(b, 20);
    tb_view *v2 = nullptr;
    if (tb_observe(b, &v2).code == 0) {
      ticked = strstr(tb_view_text(v2), "ticked") != nullptr;
      tb_view_free(v2);
    }
  }
  EXPECT_TRUE(ticked) << "定时器改了 DOM,但 tb_observe 拿到的视图没刷新";
  tb_destroy(b);
}

/* tb_load_sync 的所有权回归。
 *
 * 曾经的 bug:nav_on_done 在传输回调里同步抓子资源,抓完无条件
 * transport->cancel(op) —— 但 op 在 on_done 触发时已被传输层释放,
 * 于是拿着野指针 curl_multi_remove_handle,真 curl 下必 segfault。
 * fake transport 的 cancel 在链表里找不到就静默返回,把这个 bug 藏住了,
 * 所以它只在真网络路径上炸。
 *
 * 这个测试的形状就是「导航一个带 <script src> 的页面」—— 只要
 * load_document 里还会去抓子资源,UAF 就会复现。 */
TEST(M2aIntegration, RepeatedSubresourceLoadsDoNotCorruptTransport) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<!DOCTYPE html><title>Loop</title><body><p>host</p>"
             "<script src='/ext.js'></script></body>"}},
      {"/ext.js", {200, "application/javascript", "var n = (globalThis.n || 0) + 1;"
                                             "globalThis.n = n;"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  /* 多轮:每轮都走一次子资源抓取。反复进出这个路径是 UAF 最容易显形的方式,
   * 也顺带覆盖「同连接连续复用」这条 curl 连接池路径。 */
  for (int i = 0; i < 5; i++) {
    ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0) << "round " << i;
    ASSERT_EQ(tb_wait_idle(b, 5000), 0) << "round " << i;
    tb_view *v = nullptr;
    ASSERT_EQ(tb_observe(b, &v).code, 0) << "round " << i;
    EXPECT_STREQ(tb_view_title(v), "Loop") << "round " << i;
    tb_view_free(v);
  }

  /* 全局变量在多轮之间累加,证明外部脚本每轮都真的执行了。 */
  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(globalThis.n)", &out).code, 0);
  EXPECT_STREQ(out, "5");
  free(out);

  tb_destroy(b);
}
/* ---- 内容分类走真实响应头(端到端) ----
 *
 * tb_content_classify(ct, attachment) 在单元层测过,但那条链的前半段从未
 * 端到端验证:curl 的 header_cb 检出 "attachment" token → op->attachment=1
 * → on_headers → nav_on_headers → 视图的 is_not_renderable。
 * 中间任一环坏掉(例如 header_has_token 匹配失败),单元测试照样全绿 ——
 * 因为它们是直接把 attachment=1 喂进去的。 */

TEST(BrowserApi, ContentDispositionAttachmentIsNotRendered) {
  HttpServer srv({
      // 正文是合法 HTML,只有 Content-Disposition 决定它不该被渲染
      {"/dl", {200, "text/html", "<title>File</title><p>looks like a page</p>",
                "Content-Disposition: attachment; filename=\"a.html\"\r\n"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/dl").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_EQ(tb_view_not_renderable(v), 1)
      << "带 attachment 的响应被当页面渲染了 —— 检测链某一环坏了";
  tb_view_free(v);
  tb_destroy(b);
}

/* attachment 判定必须按 token 而不是子串:filename="x-attachment-y" 里
 * 出现 "attachment" 不该被误判成附件。 */
TEST(BrowserApi, AttachmentTokenIsNotMatchedAsSubstring) {
  HttpServer srv({
      {"/inline", {200, "text/html", "<title>Inline</title><p>page</p>",
                    "Content-Disposition: inline; filename=\"my-attachment-file.html\"\r\n"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/inline").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_EQ(tb_view_not_renderable(v), 0)
      << "inline 被误判成 attachment(filename 里含该子串)";
  EXPECT_STREQ(tb_view_title(v), "Inline");
  tb_view_free(v);
  tb_destroy(b);
}

/* Content-Type 带参数与大小写混写仍应可渲染 —— 真实服务器极常见
 * (text/html; charset=utf-8、Text/HTML)。 */
TEST(BrowserApi, ContentTypeParamsAndCaseStillRender) {
  HttpServer srv({
      {"/a", {200, "text/html; charset=utf-8", "<title>Params</title><p>x</p>"}},
      {"/b", {200, "TEXT/HTML", "<title>Upper</title><p>y</p>"}},
      {"/c", {200, "text/html;charset=UTF-8", "<title>Tight</title><p>z</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  const char *paths[] = {"/a", "/b", "/c"};
  const char *titles[] = {"Params", "Upper", "Tight"};
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(tb_navigate(b, (srv.base() + paths[i]).c_str()).code, 0) << paths[i];
    ASSERT_EQ(tb_wait_idle(b, 5000), 0) << paths[i];
    tb_view *v = nullptr;
    ASSERT_EQ(tb_observe(b, &v).code, 0) << paths[i];
    EXPECT_EQ(tb_view_not_renderable(v), 0) << paths[i];
    EXPECT_STREQ(tb_view_title(v), titles[i]) << paths[i];
    tb_view_free(v);
  }
  tb_destroy(b);
}

/* 不可渲染的响应必须仍然带上状态码与 URL —— TUI 要显示"下载了而不是
 * 渲染失败",这两个字段就是它唯一的依据。 */
TEST(BrowserApi, NotRenderableViewCarriesStatusAndUrl) {
  HttpServer srv({
      {"/f.bin", {404, "application/octet-stream", "\x00\x01"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/f.bin").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_EQ(tb_view_not_renderable(v), 1);
  EXPECT_EQ(tb_view_status(v), 404);
  EXPECT_STREQ(tb_view_not_renderable_type(v), "application/octet-stream");
  EXPECT_NE(strstr(tb_view_url(v), "/f.bin"), nullptr)
      << "不可渲染视图丢了 URL,TUI 无法显示落地位置";
  tb_view_free(v);
  tb_destroy(b);
}
