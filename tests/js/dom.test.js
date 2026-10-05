load("./tests/js/lib/testlib.js");
var P = _tb_parser;
function doc(html) {
  var d = P.parse(html);
  document._tb_attach(d.root);
  return d;
}

t("querySelector basic", function () {
  var d = doc('<div id="a"><p class="x">1</p><p class="y">2</p><a href="/z">3</a></div>');
  var a = document.querySelector("p.x");
  eq(a.textContent, "1");
  eq(document.querySelector("a").attrs.href, "/z");
});

t("querySelectorAll doc order", function () {
  var d = doc("<ul><li>1</li><li>2</li></ul><p>3</p>");
  var ps = document.querySelectorAll("li");
  eq(ps.length, 2);
  eq(ps[0].textContent, "1");
  eq(ps[1].textContent, "2");
});

t("getElementById", function () {
  var d = doc('<p id="x">hi</p><p id="y">yo</p>');
  eq(document.getElementById("x").textContent, "hi");
  eq(document.getElementById("nope"), null);
});

t("class token matching", function () {
  var d = doc('<div class="a b"><span class="b">t</span></div>');
  eq(document.querySelectorAll(".b").length, 2);
  eq(document.querySelector("div.a").tagName, "div");
});

t("title read/write", function () {
  var d = doc("<title>My Page</title><p>x</p>");
  eq(document.title, "My Page");
  document.title = "Changed";
  eq(document.title, "Changed");
});

t("document.body / head convenience", function () {
  var d = doc("<head></head><body><p>x</p></body>");
  eq(document.body.tag, "body");
  eq(document.head.tag, "head");
  eq(document.body.childNodes[0].textContent, "x");
  var d2 = doc("<p>no body</p>");
  eq(document.body, null);
  eq(document.head, null);
});

t("readyState ro", function () {
  var d = doc("<p>x</p>");
  eq(document.readyState, "complete");
});

t("createElement + appendChild + textContent", function () {
  var d = doc("<div id='root'></div>");
  var div = document.getElementById("root");
  var span = document.createElement("span");
  div.appendChild(span);
  span.textContent = "hello";
  eq(span.textContent, "hello");
  eq(div.childNodes.length, 1);
  eq(div.innerHTML, "<span>hello</span>");
});

t("textContent write replaces children", function () {
  var d = doc("<div><span>a</span><span>b</span></div>");
  var div = document.querySelector("div");
  div.textContent = "z";
  eq(div.childNodes.length, 1);
  eq(div.childNodes[0].text, "z");
});

t("removeChild", function () {
  var d = doc("<div><span>a</span><span>b</span></div>");
  var div = document.querySelector("div");
  var s = document.querySelectorAll("span")[0];
  div.removeChild(s);
  eq(div.childNodes.length, 1);
});

t("attrs get/set/remove", function () {
  var d = doc('<a href="/a" class="c">t</a>');
  var a = document.querySelector("a");
  eq(a.getAttribute("href"), "/a");
  a.setAttribute("href", "/b");
  eq(a.getAttribute("href"), "/b");
  a.removeAttribute("class");
  eq(a.getAttribute("class"), null);
});

t("cookie passes through bridge", function () {
  // bridge 由 qjs_exe 环境注入?否——单测里 __tb_cookie 未定义时安全降级
  var d = doc("<p>x</p>");
  // 只在桥存在时测
  if (typeof __tb_cookie === "function") {
    __tb_cookie_set("k=v");
    eq(__tb_cookie(), "k=v");
  }
});

t("innerHTML read-only serializer", function () {
  var d = doc('<div id="d"><p>a&amp;b</p></div>');
  var div = document.getElementById("d");
  eq(div.innerHTML, "<p>a&amp;b</p>");
});

t("addEventListener stores", function () {
  var d = doc("<button id='b'>go</button>");
  var b = document.getElementById("b");
  var called = 0;
  b.addEventListener("click", function () { called++; });
  b._fire("click");
  eq(called, 1);
});

t("elem info for interaction", function () {
  var d = doc("<form id='f'><a href='/x'>l</a><input name='q'><button>Go</button></form>");
  var a = document.querySelector("a");
  var info = JSON.parse(_tb_elem_info(a.id));
  eq(info.tag, "a");
  eq(info.href, "/x");
  var btn = document.querySelector("button");
  var bi = JSON.parse(_tb_elem_info(btn.id));
  eq(bi.tag, "button");
  var inp = document.querySelector("input");
  eq(JSON.parse(_tb_elem_info(inp.id)).tag, "input");
});

done();
