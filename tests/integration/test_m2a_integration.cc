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

/* ---- 坏 UTF-8 不得毁掉整页 ----
 *
 * 文档正文是被拼进 JS 源码的字符串字面量交给 qzjs 求值的,而**非法 UTF-8 会让
 * 整个 eval 失败**。实测一个截断的多字节序列就让 load_document 直接返回 -1 ——
 * 一个坏字节,整页连正文都渲染不出来。
 * 坏字节在真实场景里很常见:下载被截断、charset 声明与实际不符、
 * GBK/Latin-1 页面被当 UTF-8 处理。浏览器的做法是把非法序列替换成 U+FFFD。 */

TEST(BrowserApi, TruncatedUtf8DoesNotKillThePage) {
  HttpServer srv({
      // 正文里嵌一个截断的 3 字节序列(E4 B8 缺第三个字节)
      {"/", {200, "text/html",
             "<title>Bad UTF8</title><body><p>before</p>"
             "<p>\xE4\xB8</p><p>after</p></body>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Bad UTF8");
  /* 关键:坏字节前后的正常内容都还在,而不是整页空白 */
  EXPECT_NE(strstr(tb_view_text(v), "before"), nullptr) << tb_view_text(v);
  EXPECT_NE(strstr(tb_view_text(v), "after"), nullptr) << tb_view_text(v);
  tb_view_free(v);
  tb_destroy(b);
}

/* 完全非法的字节序列、以及会让 JS 字符串语义混乱的编码,都必须被替换而非
 * 让整页失败:过长编码(C0/C1)、代理区(D800-DFFF,JS 字符串里非法)、
 * 越界(> U+10FFFF)、游离续字节。 */
TEST(BrowserApi, StructurallyInvalidUtf8IsReplacedNotFatal) {
  struct Case { const char *name; const char *html; };
  const Case cases[] = {
    {"游离续字节",   "<title>T</title><p>a\x80\x80" "b</p>"},
    {"过长编码 C0",  "<title>T</title><p>a\xC0\xAF" "b</p>"},
    {"过长编码 C1",  "<title>T</title><p>a\xC1\xBF" "b</p>"},
    {"代理区高字节", "<title>T</title><p>a\xED\xA0\x80" "b</p>"},
    {"代理区低字节", "<title>T</title><p>a\xED\xBF\xBF" "b</p>"},
    {"越界 F4 90",  "<title>T</title><p>a\xF4\x90\x80\x80" "b</p>"},
    {"非法 F5-FF",   "<title>T</title><p>a\xF5\x80\x80\x80" "b</p>"},
    {"纯 0xFF",      "<title>T</title><p>a\xFF" "b</p>"},
  };
  for (const auto &c : cases) {
    HttpServer srv({{"/", {200, "text/html", c.html}}});
    ConsoleCapture cc;
    tb_browser *b = make_browser(&cc);
    ASSERT_NE(b, nullptr) << c.name;
    ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0) << c.name;
    ASSERT_EQ(tb_wait_idle(b, 5000), 0) << c.name;
    tb_view *v = nullptr;
    ASSERT_EQ(tb_observe(b, &v).code, 0) << c.name;
    EXPECT_STREQ(tb_view_title(v), "T") << c.name;
    /* 坏字节只毁掉自己那一段,前后文必须完好 */
    EXPECT_NE(strstr(tb_view_text(v), "a"), nullptr) << c.name << ": " << tb_view_text(v);
    EXPECT_NE(strstr(tb_view_text(v), "b"), nullptr) << c.name << ": " << tb_view_text(v);
    tb_view_free(v);
    tb_destroy(b);
  }
}

/* 合法 UTF-8 必须逐字节透传,不能被替换机制误伤 —— 这是本修复最大的风险:
 * 替换逻辑一旦写错,所有非 ASCII 页面都会变成一片 U+FFFD。 */
TEST(BrowserApi, ValidUtf8PassesThroughUnchanged) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95</title>"
             "<p>\xE4\xB8\xAD\xE6\x96\x87 \xE6\xB5\x8B\xE8\xAF\x95 "
             "\xF0\x9F\x98\x80 \xE2\x82\xAC \xF0\xA0\x80\x80</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  /* 2/3/4 字节序列全部原样到达 */
  EXPECT_STREQ(tb_view_title(v), "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95");
  const char *txt = tb_view_text(v);
  EXPECT_NE(strstr(txt, "\xE4\xB8\xAD\xE6\x96\x87"), nullptr) << txt;   /* 3 字节 */
  EXPECT_NE(strstr(txt, "\xF0\x9F\x98\x80"), nullptr) << txt;         /* 4 字节 emoji */
  EXPECT_NE(strstr(txt, "\xE2\x82\xAC"), nullptr) << txt;               /* 3 字节 € */
  EXPECT_EQ(strstr(txt, "\xEF\xBF\xBD"), nullptr) << "合法 UTF-8 被误替换成 U+FFFD: " << txt;
  tb_view_free(v);
  tb_destroy(b);
}

/* ---- <script> 不一定是脚本:数据块不得被执行,也不得被抓取 ----
 *
 * parser 只负责把 type 记在脚本记录上;「算不算脚本」是加载器的策略
 * (js_engine.c 的 __tb_is_js_script)。此前策略缺失,后果有二:
 *   - 内容被 eval:结构化数据的 JSON 抛 "expecting ';'",而 <style> 的 CSS 更是
 *     每个带样式的页面都会拿到一条 "script error: ... is not defined" 假报错;
 *   - 带 src 的数据块会**真的发起一次网络请求**去抓一个根本不是脚本的 URL。
 */

/* 结构化数据 + 模板 + importmap:一律不执行,也不产生任何 console 输出。
 * 此前每一条都会变成一条假报错 —— 真实页面上这是「每页必错」的噪音。 */
TEST(BrowserApi, DataBlockScriptsAreNotExecuted) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Data Blocks</title>"
             "<script type=\"application/ld+json\">{\"@context\":\"https://schema.org\","
             "\"@type\":\"Article\",1/0:\"boom\"}</script>"
             "<script type=\"application/json\">{\"a\":1}</script>"
             "<script type=\"text/template\"><b>hi</b> {{x}}</script>"
             "<script type=\"importmap\">{\"imports\":{\"a\":\"/a.js\"}}</script>"
             "<script type=\"text/x-handlebars-template\"><div></div></script>"
             "<p>body</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  pump_rounds(b, 3, 5);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Data Blocks");
  EXPECT_NE(strstr(tb_view_text(v), "body"), nullptr) << tb_view_text(v);

  /* 关键断言:一条 script error 都没有。此前每种数据块各贡献一条假报错。 */
  for (const auto &l : cc.lines)
    EXPECT_EQ(l.find("script error"), std::string::npos)
        << "数据块被当 JS 执行了,产生假报错: " << l;
  tb_view_free(v);
  tb_destroy(b);
}

/* 带 src 的数据块绝不能被抓取 —— 那是一个根本不是脚本的 URL。
 * 这是本组修复里唯一有「外部副作用」的一条:多余的请求会被第三方看到,
 * 也会拖慢首屏。用 request_count 卡住。 */
TEST(BrowserApi, DataBlockWithSrcIsNeverFetched) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>No Fetch</title>"
             "<script type=\"application/json\" src=\"/not-really-a-script.json\"></script>"
             "<script type=\"text/template\" src=\"/tpl.html\"></script>"
             "<p>body</p>"}},
      {"/not-really-a-script.json", {200, "application/json", "{}"}},
      {"/tpl.html", {200, "text/html", "<b>x</b>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  /* 只有文档本身那一次请求 */
  EXPECT_EQ(srv.request_count(), 1)
      << "数据块的 src 被抓取了(应有且仅有 1 次请求:文档本身)";
  tb_destroy(b);
}

/* 真正的脚本仍然照常执行 —— 过滤不能过头。
 * type 缺失、空、以及带 charset 参数的 JavaScript MIME 类型都算脚本。 */
TEST(BrowserApi, RealScriptsStillRunAlongsideDataBlocks) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Runs</title>"
             "<script type=\"application/ld+json\">{\"a\":1}</script>"
             "<script>__tb_mark = 'no-type';</script>"
             "<script type=\"\">__tb_mark += '|empty';</script>"
             "<script type=\"text/javascript;charset=utf-8\">__tb_mark += '|charset';</script>"
             "<script type=\"module\">__tb_mark += '|module';</script>"
             "<p>body</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(__tb_mark)", &out).code, 0);
  ASSERT_NE(out, nullptr);
  EXPECT_STREQ(out, "no-type|empty|charset|module") << out;
  free(out);
  tb_destroy(b);
}

/* <style> 的内容是 CSS 不是 JS,不能被 eval。
 * 此前每个带 <style> 的页面都会拿到一条 "script error: ... is not defined"。 */
TEST(BrowserApi, StyleContentIsNotEvaledAsJavaScript) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Styled</title>"
             "<style>p{color:red}</style>"
             "<style>.a{margin:0}</style>"
             "<p>body</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  pump_rounds(b, 3, 5);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Styled");
  for (const auto &l : cc.lines)
    EXPECT_EQ(l.find("script error"), std::string::npos)
        << "CSS 被当 JS 求值了: " << l;
  tb_view_free(v);
  tb_destroy(b);
}

/* 数值实体按码点解释:此前 "&#x1F600;" 落在私有区码位 U+F600,
 * 渲染成一个无意义字形。这条从解析一路走到视图,确认没有中途再被截断。 */
TEST(BrowserApi, AstralNumericEntitySurvivesToTheView) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Entities</title>"
             "<p>star=&#x1F600; euro=&#8364; bmp=&#65;</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  const char *txt = tb_view_text(v);
  EXPECT_NE(strstr(txt, "\xF0\x9F\x98\x80"), nullptr) << "星形码点被截断: " << txt;
  EXPECT_EQ(strstr(txt, "\xEF\xA0\x80"), nullptr) << "落进私有区 U+F600: " << txt;
  EXPECT_NE(strstr(txt, "\xE2\x82\xAC"), nullptr) << txt;
  EXPECT_NE(strstr(txt, "A"), nullptr) << txt;
  tb_view_free(v);
  tb_destroy(b);
}

/* 属性名一律小写 —— dom.js 取值全用小写键,此前 "<a HREF=...>" 的链接是死的。 */
TEST(BrowserApi, UppercaseAttributesAreLowercased) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Attrs</title>"
             "<a HREF='/target' CLASS='big'>link</a>"}},
      {"/target", {200, "text/html", "<title>Target</title><p>arrived</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  ASSERT_STREQ(tb_view_title(v), "Attrs");
  /* 点击这个链接必须真的能导航 —— 大写 HREF 时它解析成空链接,点击是死路 */
  struct tb_elem e{};
  ASSERT_EQ(tb_view_elem(v, 0, &e), 0);
  ASSERT_STREQ(e.type, "link");
  ASSERT_STREQ(e.href, "/target");
  int link_id = e.id;
  tb_view_free(v);
  ASSERT_EQ(tb_click(b, link_id).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Target");
  EXPECT_NE(strstr(tb_view_text(v), "arrived"), nullptr);
  tb_view_free(v);
  tb_destroy(b);
}

/* ---- <textarea> 搜索框:现代站点的主流写法,此前完全不可用 ----
 *
 * 起因是拿 Google 首页试真实页面时发现的:它的搜索框不是 <input>,而是
 * <textarea name="q" rows="1">。而 render.js 此前**没有 textarea 分支**,
 * dom.js 的 _tb_form_pairs 也不收它,于是那个框既不渲染、不可点,填了值
 * 提交时也不带 —— 在 Google 上根本没法搜索。
 *
 * 这几条用本地 fixture 而不是真实站点:真实站点会 A/B 分流、按地域跳转、
 * 还会在我们请求过多时直接 TLS 拒连(实测已发生),拿它当测试必然是 flaky 的。
 * fixture 的形状照抄 Google 的:form[action=/search] + textarea[name=q]。 */

TEST(BrowserApi, TextareaSearchBoxIsFillableAndSubmittable) {
  HttpServer srv({
      // 形状照抄 Google 首页:搜索框是 textarea,不是 input
      {"/", {200, "text/html",
             "<title>Search Home</title>"
             "<form action=\"/search\" method=\"get\">"
             "<textarea name=\"q\" rows=\"1\" placeholder=\"Search\"></textarea>"
             "<input type=\"submit\" value=\"Google Search\">"
             "</form>"}},
      {"/search", {200, "text/html",
                   "<title>Results</title><p>you searched</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);

  /* textarea 必须在视图里,且可识别 —— 此前它根本不出现 */
  int ta_id = -1, form_id = -1;
  for (int i = 0; i < tb_view_nelems(v); i++) {
    struct tb_elem e;
    if (tb_view_elem(v, i, &e)) continue;
    if (e.name && strcmp(e.name, "q") == 0) ta_id = e.id;
    if (form_id < 0 && e.type && strcmp(e.type, "form") == 0) form_id = e.id;
  }
  ASSERT_NE(ta_id, -1) << "textarea 没有进入视图,搜索框不可见也不可填";
  ASSERT_NE(form_id, -1);
  tb_view_free(v);

  ASSERT_EQ(tb_fill(b, ta_id, "tinybrowser m2a").code, 0);
  ASSERT_EQ(tb_submit(b, form_id).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Results");
  /* 提交出去的 URL 必须真的带上 textarea 里的值。
   * 空格编成 %20 而非 form-urlencoded 惯用的 + —— 那是既有的 URL 编码选择
   * (对 input 一样),两条路由对服务端解码结果相同,不在本次范围内改。 */
  EXPECT_NE(strstr(tb_view_url(v), "q=tinybrowser%20m2a"), nullptr) << tb_view_url(v);
  tb_view_free(v);
  tb_destroy(b);
}

/* 填过的值要出现在视图上,并且重渲不得把它退回 textarea 的原始内容。
 * 视图 id 是渲染计数器,重渲会重建;靠 __tb_value 记在节点上才跨得过去。 */
TEST(BrowserApi, FilledTextareaValueSurvivesRerender) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Fill</title>"
             "<form action=\"/search\"><textarea name=\"q\">original</textarea></form>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  int ta_id = -1;
  for (int i = 0; i < tb_view_nelems(v); i++) {
    struct tb_elem e;
    if (tb_view_elem(v, i, &e)) continue;
    if (e.name && strcmp(e.name, "q") == 0) ta_id = e.id;
  }
  ASSERT_NE(ta_id, -1);
  struct tb_elem before;
  ASSERT_EQ(tb_view_elem(v, 0, &before), 0);
  tb_view_free(v);

  ASSERT_EQ(tb_fill(b, ta_id, "typed value").code, 0);
  /* 定时器改 DOM 会触发指纹重渲;这里直接 pump 几轮走同一条重渲路径 */
  pump_rounds(b, 3, 5);

  ASSERT_EQ(tb_observe(b, &v).code, 0);
  int ta_id2 = -1;
  bool saw_value = false;
  for (int i = 0; i < tb_view_nelems(v); i++) {
    struct tb_elem e;
    if (tb_view_elem(v, i, &e)) continue;
    if (e.name && strcmp(e.name, "q") == 0) { ta_id2 = e.id; saw_value = (strcmp(e.value, "typed value") == 0); }
  }
  EXPECT_NE(ta_id2, -1);
  EXPECT_TRUE(saw_value) << "填过的 textarea 值在重渲后丢了";
  tb_view_free(v);
  tb_destroy(b);
}

/* select 的默认值此前也是空的:一律读 attrs.value,而 select 没有 value 属性。
 * HTML 规定 select 的默认选中项是第一个 option —— 修 _tb_control_value 时
 * 顺带暴露的既有缺口,这里钉住。 */
TEST(BrowserApi, SelectDefaultsToFirstOption) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Sel</title>"
             "<form action=\"/s\"><select name=\"lang\">"
             "<option value=\"en\">English</option>"
             "<option value=\"zh\">Chinese</option>"
             "</select></form>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  int sel_id = -1, form_id = -1;
  bool default_ok = false;
  for (int i = 0; i < tb_view_nelems(v); i++) {
    struct tb_elem e;
    if (tb_view_elem(v, i, &e)) continue;
    if (e.type && strcmp(e.type, "select") == 0) { sel_id = e.id; default_ok = (strcmp(e.value, "English") == 0); }
    if (form_id < 0 && e.type && strcmp(e.type, "form") == 0) form_id = e.id;
  }
  EXPECT_NE(sel_id, -1);
  EXPECT_TRUE(default_ok) << "select 的默认值应是首个 option 的文本";
  tb_view_free(v);

  /* 选过之后以选中的为准 */
  ASSERT_EQ(tb_select(b, sel_id, "Chinese").code, 0);
  ASSERT_EQ(tb_submit(b, form_id).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  tb_destroy(b);
}

/* ---- 页面脚本自己建表单:document.forms / 具名访问 / .value / submit() ----
 *
 * Google 首页有一部分变体的搜索表单是 JS 动态建的(静态 HTML 里没有 form)。
 * 此前 document.forms 不存在、form.q 取不到、el.value 不存在,脚本一上来
 * 就断,表单根本建不起来。这条用**页面脚本自己建表单**的 fixture 覆盖整条链:
 * document.forms → 建 form → 具名访问 → .value → form.submit() → 宿主导航。
 */

TEST(BrowserApi, ScriptBuiltFormNavigatesViaFormSubmit) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Dynamic Form</title>"
             "<div id=mount></div>"
             "<script>"
             "var f = document.createElement('form');"
             "  f.setAttribute('action', '/search');"
             "  f.setAttribute('method', 'get');"
             "  var q = document.createElement('input');"
             "  q.setAttribute('name', 'q');"
             "  q.value = 'built by script';"
             "  f.appendChild(q);"
             "  document.getElementById('mount').appendChild(f);"
             "  /* 走一遍:document.forms + 具名访问 + .value */"
             "  window.__found = document.forms.length === 1"
             "    && document.forms[0].q.value === 'built by script';"
             "  document.forms[0].submit();"
             "</script>"}},
      {"/search", {200, "text/html",
                   "<title>Dynamic Results</title><p>landed</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  pump_rounds(b, 3, 5);

  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(window.__found)", &out).code, 0);
  EXPECT_STREQ(out, "true") << "document.forms / 具名访问 / .value 这条链有问题: " << out;
  free(out);
  /* form.submit() 的 nav 指令在前面的 pump 里已被 drain 取出并 tb_navigate(异步):
   * 必须等导航+加载完成,observe 才看得到 /search。__found 已在此前 eval 读出,
   * 导航后新文档会重置 window,故 wait_idle 必须放在 eval 之后。 */
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  /* form.submit() 经导航指令通道把宿主带到了 /search?q=... */
  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_STREQ(tb_view_title(v), "Dynamic Results") << "url=" << tb_view_url(v);
  EXPECT_NE(strstr(tb_view_url(v), "q=built%20by%20script"), nullptr) << tb_view_url(v);
  tb_view_free(v);
  tb_destroy(b);
}

/* 脚本填的值必须真的进提交 —— 这条钉的是 element.value 的访问器。
 * 没有访问器时 f.q.value = "x" 会变成与 attrs 无关的普通属性,
 * 提交出去的还是页面原值,且不报任何错。 */
TEST(BrowserApi, ScriptAssignedValueReachesTheWire) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Assign</title>"
             "<form action=\"/echo\" method=\"get\">"
             "<input name=\"q\" value=\"original\">"
             "</form>"
             "<script>"
             "document.forms[0].q.value = 'assigned by script';"
             "</script>"}},
      {"/echo", {200, "text/html", "<title>Echo</title><p>done</p>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  tb_view *v = nullptr;
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  int form_id = -1;
  for (int i = 0; i < tb_view_nelems(v); i++) {
    struct tb_elem e;
    if (tb_view_elem(v, i, &e)) continue;
    if (form_id < 0 && e.type && strcmp(e.type, "form") == 0) form_id = e.id;
  }
  ASSERT_NE(form_id, -1);
  tb_view_free(v);

  ASSERT_EQ(tb_submit(b, form_id).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);
  ASSERT_EQ(tb_observe(b, &v).code, 0);
  EXPECT_NE(strstr(tb_view_url(v), "q=assigned%20by%20script"), nullptr) << tb_view_url(v);
  tb_view_free(v);
  tb_destroy(b);
}

/* tagName 按规范大写。此前是小写,而同一节点新加的 nodeName 是大写 ——
 * 同一个属性对两种大小写,比只错一个更糟。 */
TEST(BrowserApi, TagNameIsUppercaseLikeBrowsers) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>TagName</title>"
             "<div id=probe>text</div>"
             "<script>"
             "var d = document.getElementById('probe');"
             "window.__tn = d.tagName + '|' + d.nodeName + '|' + d.nodeType"
             "   + '|' + (d instanceof Node) + '|' + (d instanceof Element);"
             "</script>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(window.__tn)", &out).code, 0);
  EXPECT_STREQ(out, "DIV|DIV|1|true|true") << out;
  free(out);
  tb_destroy(b);
}

/* WebAssembly:启用 QZ_WITH_WAMR 后页面 JS 可用 WebAssembly API。
 * 用同步 new Module/Instance 路径(避开 Promise/microtask 时机),实例化一个
 * add(a,b) 模块并调用,断言结果。字节码是最小有效 wasm:
 *   (module (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))
 * SIMD 关了(QZ 侧 WAMR_BUILD_SIMD=0),但 add 不需要 SIMD。 */
TEST(BrowserApi, WebAssemblyInstantiateRunsExports) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Wasm</title><div id=m></div>"
             "<script>"
             "var wb = new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,"
             "1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);"
             "var wm = new WebAssembly.Module(wb);"
             "var wi = new WebAssembly.Instance(wm);"
             "window.__wasm = wi.exports.add(2, 3);"
             "</script>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(window.__wasm)", &out).code, 0);
  EXPECT_STREQ(out, "5") << "wamr 实例化 add(2,3) 应得 5;当前: " << out;
  free(out);
  tb_destroy(b);
}

/* 复杂页面综合验证:模拟现代站点(嵌套表单 + 外部 script + wasm + 动态 DOM +
 * CSS)。断言加载、脚本、wasm、表单访问全部正常,且无任何 console 报错——
 * 复杂页面最怕的就是「加载出来但一路报错」。 */
TEST(BrowserApi, ComplexModernPageLoadsCleanly) {
  HttpServer srv({
      {"/", {200, "text/html",
             "<title>Complex</title>"
              "<style>.x{color:red}</style>"
              "<body>"
             "<form id=f action=/search method=get>"
             "<input name=q value=hi>"
             "<select name=s><option>a</option><option selected>b</option></select>"
             "<textarea name=t>note</textarea>"
             "</form>"
             "<script src=/app.js></script>"
             "<script>"
             "var wb = new Uint8Array([0,97,115,109,1,0,0,0,1,7,1,96,2,127,127,"
             "1,127,3,2,1,0,7,7,1,3,97,100,100,0,0,10,9,1,7,0,32,0,32,1,106,11]);"
             "var wm = new WebAssembly.Module(wb);"
             "var wi = new WebAssembly.Instance(wm);"
             "window.__wasm = wi.exports.add(3, 4);"
             "var d = document.createElement('div'); d.id='dynamic'; document.body.appendChild(d);"
             "window.__hasForm = document.forms.length === 1"
             "  && document.forms.f.q.value === 'hi'"
             "  && document.forms.f.s.value === 'b'"
             "  && document.forms.f.t.value === 'note';"
              "</script></body>"}},
      {"/app.js", {200, "application/javascript",
             "window.__ext = document.getElementById('f').tagName;"}},
      {"/search", {200, "text/html", "<title>R</title>"}},
  });
  ConsoleCapture cc;
  tb_browser *b = make_browser(&cc);
  ASSERT_NE(b, nullptr);

  ASSERT_EQ(tb_navigate(b, (srv.base() + "/").c_str()).code, 0);
  ASSERT_EQ(tb_wait_idle(b, 5000), 0);

  char *out = nullptr;
  ASSERT_EQ(tb_eval_js(b, "String(window.__ext)", &out).code, 0);
  EXPECT_STREQ(out, "FORM") << "外部 script 应已执行: " << out;
  free(out); out = nullptr;

  ASSERT_EQ(tb_eval_js(b, "String(window.__wasm)", &out).code, 0);
  EXPECT_STREQ(out, "7") << "wasm add(3,4) 应得 7: " << out;
  free(out); out = nullptr;

  ASSERT_EQ(tb_eval_js(b, "document.getElementById('dynamic') != null", &out).code, 0);
  EXPECT_STREQ(out, "true") << "脚本动态建的元素应存在: " << out;
  free(out); out = nullptr;

  ASSERT_EQ(tb_eval_js(b, "String(window.__hasForm)", &out).code, 0);
  EXPECT_STREQ(out, "true") << "具名表单/具名访问/.value 链: " << out;
  free(out); out = nullptr;

  EXPECT_TRUE(cc.lines.empty()) << "复杂页面不应有任何 console 报错, 收到 "
                                << cc.lines.size() << " 条, 首条: "
                                << (cc.lines.empty() ? std::string() : cc.lines[0]);
  tb_destroy(b);
}
