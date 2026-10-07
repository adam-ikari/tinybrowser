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
  // tagName 按 HTML 规范是大写(此前是小写,与新加的 nodeName 自相矛盾)
  eq(document.querySelector("div.a").tagName, "DIV");
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

/* ================= document.forms 与表单具名访问 =================
 *
 * 起因:Google 首页有一部分变体的搜索表单是 JS 动态建的,脚本第一句就是
 * document.forms —— 此前该属性不存在,直接 "document.forms is undefined",
 * 后面一串连锁失败。
 *
 * 浏览器语义要点(都容易做错,逐条钉住):
 *   - 集合每次取用现算:DOM 会被脚本改,缓存的集合立刻过期。
 *   - 具名访问(name 与 id 都算)只认**真具名**,不误伤对象自有的成员:
 *     一个叫 toString 的 input 不该把 form.toString 顶掉。
 *   - 具名控件不覆盖接口成员:浏览器里 form.submit 优先于叫 submit 的控件,
 *     脚本随时在调 form.submit()。
 */

var FORM_HTML = '<form id=fs name=search action=/s method=get>' +
                '<input name=q value=hello>' +
                '<textarea name=note>abc</textarea>' +
                '<select name=lang><option>en</option><option>zh</option></select>' +
                '<input type=submit name=go value=Go>' +
                '</form>';

t("document.forms is a live collection in document order", function () {
  doc('<body><p>x</p>' + FORM_HTML + '<form action=/t><input name=z></form></body>');
  eq(document.forms.length, 2);
  eq(document.forms[0].tag, "form");
  eq(document.forms[0].action, "/s");
  eq(document.forms[1].action, "/t");
  // 现算:后加的 form 要立刻可见
  var f = document.createElement("form");
  f.setAttribute("action", "/late");
  document.body.appendChild(f);
  eq(document.forms.length, 3);
  eq(document.forms[2].action, "/late");
});

t("document.forms supports index, name and id lookup", function () {
  doc(FORM_HTML);
  eq(document.forms[0].tag, "form");
  eq(document.forms["search"].tag, "form");     // 按 name
  eq(document.forms["fs"].tag, "form");          // 按 id
  eq(document.forms.namedItem("search").tag, "form");
  eq(document.forms.namedItem("nope"), null);
  eq(document.forms.item(0).tag, "form");
  eq(document.forms.item(9), null);
  eq(document.forms.length, 1);
});

t("form.elements enumerates controls and exposes length", function () {
  doc(FORM_HTML);
  var f = document.forms[0];
  eq(f.elements.length, 4);
  eq(f.length, 4);                                 // form 自身也有 length
  var tags = [];
  for (var i = 0; i < f.elements.length; i++) tags.push(f.elements[i].tag);
  eq(tags.join(","), "input,textarea,select,input");
});

t("named access on the form itself (form.q)", function () {
  doc(FORM_HTML);
  var f = document.forms[0];
  eq(f.q.tag, "input");
  eq(f["q"].tag, "input");                          // 方括号等价
  eq(f.note.tag, "textarea");
  eq(f.lang.tag, "select");
  eq(f.elements.q.tag, "input");                    // 集合上同样支持
  eq(f.elements.namedItem("note").tag, "textarea");
  // 没有的名字就是 undefined,不是抛异常
  eq(f.nosuch, undefined);
  eq(f.elements.nosuch, undefined);
});

t("named access covers id as well as name", function () {
  doc('<form action=/s><input id=byid name=byName></form>');
  var f = document.forms[0];
  eq(f.byid.tag, "input");
  eq(f.byName.tag, "input");
});

t("named access never shadows the form's own interface members", function () {
  // 浏览器语义:接口成员优先。否则一个叫 submit 的 input 会把
  // form.submit() 顶掉,脚本随时就在调它 —— 那是灾难性的。
  doc('<form action=/s><input name=submit value=x><input name=elements value=y>' +
      '<input name=action value=z></form>');
  var f = document.forms[0];
  eq(typeof f.submit, "function");
  eq(typeof f.elements, "object");
  eq(f.action, "/s");                               // 不是 input 的 value
  // 但仍然能从 elements 里按名字拿到那个 input
  eq(f.elements.namedItem("submit").tag, "input");
  eq(f.elements.namedItem("submit").value, "x");
});

t("form.action/method/target/enctype are readable and writable", function () {
  doc(FORM_HTML);
  var f = document.forms[0];
  eq(f.action, "/s");
  eq(f.method, "get");
  eq(f.target, "");
  eq(f.enctype, "application/x-www-form-urlencoded");
  f.action = "/other";
  f.method = "POST";                                // 大小写要归一
  f.target = "_blank";
  eq(f.action, "/other");
  eq(f.method, "post");
  eq(f.target, "_blank");
  eq(f.attrs.method, "post");
});

/* ---- element.value 必须是真访问器,不是普通自有属性 ----
 *
 * 脚本写得最多的就是 f.q.value = "..."。若没有访问器,赋值会变成一个与
 * attrs 无关的普通属性,而取值走 attrs.value —— 提交出去的还是页面原始值,
 * 脚本填的内容凭空消失,且不报任何错。 */
t("element.value is an accessor, not a plain own property", function () {
  doc('<form action=/s><input name=q value=orig><textarea name=note>abc</textarea>' +
      '<select name=lang><option>en</option><option>zh</option></select></form>');
  var f = document.forms[0];
  eq(f.q.value, "orig");                            // input 取 value 属性
  eq(f.note.value, "abc");                          // textarea 取内容
  eq(f.lang.value, "en");                           // select 取首个 option

  f.q.value = "typed";
  eq(f.q.value, "typed");
  // 关键:提交路径必须看得到脚本写的值
  eq(_tb_form_query(f), "q=typed&note=abc&lang=en");
  var pairs = JSON.parse(_tb_form_pairs(f.id));
  eq(pairs.pairs[0].value, "typed");

  f.note.value = "new note";
  eq(_tb_form_query(f), "q=typed&note=new%20note&lang=en");
});

t("select.value prefers a selected option over the first", function () {
  doc('<form action=/s><select name=lang><option>a</option><option selected>b</option>' +
      '<option>c</option></select></form>');
  eq(document.forms[0].lang.value, "b");
  // 无 selected 时仍回落第一个 option
  doc('<form action=/s><select name=lang><option>x</option><option>y</option></select></form>');
  eq(document.forms[0].lang.value, "x");
});

t("setAttribute('value') still works and is overridden by an explicit fill", function () {
  doc('<form action=/s><input name=q value=a></form>');
  var f = document.forms[0];
  f.q.setAttribute("value", "b");
  eq(f.q.value, "b");                                // 改了属性就读到
  f.q.value = "c";                                   // 显式赋值优先
  eq(f.q.value, "c");
  eq(_tb_form_query(f), "q=c");
});

t("form.reset() drops filled values, back to the DOM's own", function () {
  doc(FORM_HTML);
  var f = document.forms[0];
  f.q.value = "typed";
  f.note.value = "changed";
  eq(_tb_form_query(f), "q=typed&note=changed&lang=en&go=Go");
  f.reset();
  eq(f.q.value, "hello");
  eq(f.note.value, "abc");
  eq(_tb_form_query(f), "q=hello&note=abc&lang=en&go=Go");
});

t("form.submit() asks the host to navigate (mailbox nav channel)", function () {
  // qzjs 从不回调宿主,故 submit 走已有的导航指令通道(与 location.href= 同源)。
  // bootstrap 垫片把 __tb_send_nav 换成「记下指令」,语义等价且可断言。
  doc('<form action=/s method=get><input name=q value=hi></form>');
  __tb_last_nav = null;
  eq(document.forms[0].submit(), true);
  ok(__tb_last_nav, "应发出导航指令");
  eq(__tb_last_nav.action, "navigate");
  eq(__tb_last_nav.url, "/s?q=hi");
});

t("form.submit() refuses POST loudly instead of silently degrading to GET", function () {
  // POST 的参数在 body 里,不在 query 里。静默降级成 GET+query 是语义错误。
  doc('<form action=/s method=post><input name=q value=hi></form>');
  var f = document.forms[0];
  __tb_last_nav = null;
  var warned = 0;
  var realWarn = __tb_console.warn;
  __tb_console.warn = function () { warned++; };
  try {
    eq(f.submit(), false);
  } finally {
    __tb_console.warn = realWarn;
  }
  eq(__tb_last_nav, null, "POST 不得发出导航指令");
  ok(warned > 0, "POST 必须出声警告,不能静默");
});

t("form.submit() appends with & when action already has a query", function () {
  doc('<form action=/s?a=1><input name=q value=hi></form>');
  __tb_last_nav = null;
  document.forms[0].submit();
  eq(__tb_last_nav.url, "/s?a=1&q=hi");
});

t("submit includes only named controls (spec) and skips file inputs", function () {
  doc('<form action=/s>' +
      '<input value=noname>' +
      '<input name=ok value=1>' +
      '<input type=file name=up>' +
      '<textarea>no name</textarea>' +
      '<input type=submit name=Send value=Send>' +
      '<input type=submit value=Noname>' +
      '</form>');
  /* 无 name 的控件一律不提交(spec):那个 value=Noname 的 submit 就是反例,
     它有 value 但没有 name,不能进 query。 */
  eq(_tb_form_query(document.forms[0]), "ok=1&Send=Send");
});

/* ---- 其它集合 ---- */

t("document.links / images / all", function () {
  doc('<body><a href=/a>1</a><a>no href</a><img src=i.png><img src=j.png></body>');
  eq(document.links.length, 1);                      // 只算有 href 的
  eq(document.images.length, 2);
  eq(document.all.length, 1);                         // body 的浅层代理,够用了
});

t("document.scripts is deliberately absent (script is not in the DOM tree)", function () {
  // parser 把 script/style 记进 scripts 列表而**不进 DOM 树**。所以
  // document.scripts 若存在就恒为空 —— 脚本会据此以为「页面没有脚本」而
  // 走进错误分支。宁可不发这个 API。
  doc('<body><script>1</script></body>');
  eq(document.scripts, undefined);
  eq(document.querySelector("script"), null);
});

t("getElementsByTagName / getElementsByClassName", function () {
  doc('<div class="a"><p class="a b">1</p><span class="b">2</span><p>3</p></div>');
  eq(document.getElementsByTagName("p").length, 2);
  eq(document.getElementsByTagName("P").length, 2);  // 大小写不敏感
  eq(document.getElementsByClassName("a").length, 2);
  eq(document.getElementsByClassName("a b").length, 1);
  eq(document.getElementsByClassName("zz").length, 0);
  eq(document.getElementsByTagName("div")[0].tag, "div");
});

/* ---- Node / Element / nodeType ---- */

t("Node/Element/HTMLElement/HTMLFormElement exist and instanceof works", function () {
  eq(typeof Node, "function");
  eq(Node.ELEMENT_NODE, 1);
  eq(Node.TEXT_NODE, 3);
  eq(Node.DOCUMENT_NODE, 9);
  doc(FORM_HTML);
  var f = document.forms[0];
  // instanceof 只检查 prototype 是否在原型链上:我们把 Node.prototype
  // 直接指向节点原型,故为真。
  eq(f instanceof Node, true);
  eq(f instanceof Element, true);
  eq(f instanceof HTMLElement, true);
  eq(f instanceof HTMLFormElement, true);
});

t("nodeType and nodeName", function () {
  doc('<p>text</p>');
  var p = document.querySelector("p");
  eq(p.nodeType, 1);
  eq(p.nodeName, "P");
  var txt = p.children[0];
  eq(txt.type, "text");
  eq(txt.nodeType, 3);
  eq(txt.nodeName, "#text");
  eq(txt instanceof Node, true);
});

t("appendChild attaches prototypes to the whole inserted subtree", function () {
  // 此前 createElement 建的节点靠自带 __proto__ 活着,但直接塞进来的对象
  // 与带孙节点的片段原型是缺的 —— .tagName / .textContent 全 undefined。
  doc('<div id=host></div>');
  var host = document.querySelector("#host");
  var frag = { type: "element", tag: "section", attrs: {}, children: [], text: "", id: 999, parent: null };
  var kid = { type: "element", tag: "b", attrs: {}, children: [], text: "", id: 998, parent: null };
  kid.children.push({ type: "text", tag: null, attrs: {}, children: [], text: "hi", id: 0, parent: kid });
  frag.children.push(kid);
  host.appendChild(frag);
  eq(frag.tagName, "SECTION");
  eq(kid.tagName, "B");
  eq(frag.textContent, "hi");
  eq(frag.parentNode.id, host.id);
});

t("appendChild into a form exposes the new control by name", function () {
  doc('<form action=/s><input name=q></form>');
  var f = document.forms[0];
  eq(f.late, undefined);
  var inp = document.createElement("input");
  inp.setAttribute("name", "late");
  inp.setAttribute("value", "v");
  f.appendChild(inp);
  eq(f.late.tag, "input");                            // 具名属性随之出现
  eq(_tb_form_query(f), "q=&late=v");
});

t("createElement('form') gets the form interface", function () {
  doc('<div></div>');
  var f = document.createElement("form");
  eq(typeof f.submit, "function");
  eq(typeof f.reset, "function");
  eq(f.action, "");
  eq(f.method, "get");
  eq(f.elements.length, 0);
});

t("a plain element has no form interface (split prototypes)", function () {
  // 「div.elements 是空的」这种半吊子属性不如没有:分开原型后只有 <form> 上有。
  doc('<div id=d></div>');
  var d = document.querySelector("#d");
  eq(d.elements, undefined);
  eq(d.submit, undefined);
  eq(typeof d.getAttribute, "function");             // 节点原型仍在
  eq(d.tagName, "DIV");
});


/* ---- 节点不能有循环引用,否则 eval 缝会卡满超时 ----
 *
 * 节点之间有 parent 指针 → 循环。qzjs 的 ctl_eval 靠 JSON.stringify 生成回执,
 * 遇循环抛 TypeError → 回执丢失 → 宿主等满超时(实测整整 5s)并报一句误导的
 * "control round-trip failed"。这是**既有**缺陷,不是这批改动引入的。
 *
 * 触发条件很窄:表达式本身就是 appendChild/removeChild 的返回值时。页面脚本
 * 内部调用不走 eval 缝碰不到,但 tb_eval_js(b, "el.appendChild(x)") 会踩到。
 * 浏览器里 JSON.stringify(element) 是 "{}",这里也必须一致。
 */
t("nodes are JSON-safe: no cycle via parent", function () {
  doc('<body><div id=h></div></body>');
  var h = document.querySelector("#h");
  var kid = document.createElement("span");
  h.appendChild(kid);
  eq(JSON.stringify(h), "{}");                      // 与浏览器一致
  eq(JSON.stringify(kid), "{}");
  eq(JSON.stringify([h, kid]), "[{},{}]");
  // 更贴近真实故障:表达式本身就是一个返回节点的调用
  eq(String(h.appendChild(document.createElement("b"))), "[object Object]");
  // 但节点的正经取值口不受影响
  ok(h.outerHTML.length > 0);
  eq(h.childNodes.length, 2);
});


done();
