load("./tests/js/lib/testlib.js");
var P = _tb_parser.parse;   // Task 2 完成后存在

t("basic tree", function () {
  var d = P("<html><body><p>hi <b>there</b></p></body></html>");
  var body = d.root.children[0];
  eq(body.tag, "html");
  var p = body.children[0].children[0];
  eq(p.tag, "p");
  eq(p.children.length, 2);
  eq(p.children[1].tag, "b");
  eq(p.children[1].children[0].text, "there");
});

t("void elements self-close", function () {
  var d = P("<p>a<br>b<img src=x>c<input><hr>d</p>");
  var p = d.root.children[0];
  eq(p.children.length, 8);
  eq(p.children[1].tag, "br");
  ok(p.children[1].children === undefined || p.children[1].children.length === 0);
  eq(p.children[3].tag, "img");
});

t("unquoted and boolean attrs", function () {
  var d = P("<input name=q value=hello checked disabled>");
  var n = d.root.children[0];
  eq(n.attrs.name, "q");
  eq(n.attrs.value, "hello");
  eq(n.attrs.checked, "");
  eq(n.attrs.disabled, "");
});

t("quoted attrs with escapes", function () {
  var d = P('<a href="/x?a=1&amp;b=2" title="he said \'hi\'">t</a>');
  var n = d.root.children[0];
  eq(n.attrs.href, "/x?a=1&b=2");
  eq(n.attrs.title, "he said 'hi'");
});

t("implicit close p/li/td/tr/th/option", function () {
  var d = P("<ul><li>a<li>b</ul><table><tr><td>x<td>y</table>");
  var ul = d.root.children[0];
  eq(ul.children.length, 2);
  eq(ul.children[1].children[0].text, "b");
  var table = d.root.children[1];
  var tr = table.children[0];
  eq(tr.children.length, 2);
  eq(tr.children[1].children[0].text, "y");
});

t("mis-nesting tolerated", function () {
  var d = P("<b>one<i>two</b>three</i>");
  eq(d.root.children[0].tag, "b");
  eq(d.root.children[0].children[0].text, "one");
  // </b> 时 i 被隐式闭合,i 的内容归属 b 下
  eq(d.root.children[0].children.length, 2);
  ok(d.warnings.length >= 1);
});

t("entity decode in text", function () {
  var d = P("<p>a&amp;b &lt;c&gt; &quot;d&quot; &apos;e&apos; &nbsp;x &#65;&#x42;</p>");
  var p = d.root.children[0];
  eq(p.children[0].text, "a&b <c> \"d\" 'e'  x AB");
});

t("script raw text collected as record", function () {
  var d = P("<p>a</p><script>if (1 < 2 && x) { var s = \"&amp;\"; }</script><p>b</p>");
  var p0 = d.root.children[0];
  eq(p0.children[0].text, "a");
  var p1 = d.root.children[1];
  eq(p1.children[0].text, "b");
  // raw 内容不进 DOM
  var sc = d.scripts[0];
  eq(sc.src, null);
  ok(sc.text.indexOf("&amp;") >= 0);
  ok(sc.text.indexOf("<") >= 0);
});

t("script src attribute", function () {
  var d = P("<script src='/app.js'></script>");
  eq(d.scripts[0].src, "/app.js");
  eq(d.scripts[0].text, "");
});

t("style raw text skipped from tree", function () {
  var d = P("<style>a > b { color: red }</style><p>x</p>");
  eq(d.root.children.length, 1);
  eq(d.root.children[0].tag, "p");
});

t("comment doctype pi skipped", function () {
  var d = P("<!DOCTYPE html><!-- hi --><?pi x?><p>ok</p>");
  eq(d.root.children.length, 1);
  eq(d.root.children[0].tag, "p");
});

t("text across chunk boundary is contiguous", function () {
  var ins = _tb_parser.make();
  ins.feed("<p>hel");
  ins.feed("lo <b>w");
  ins.feed("orld</b></p>");
  ins.finalize();
  var p = ins.root.children[0];
  eq(p.children[0].text, "hello ");
  eq(p.children[1].children[0].text, "world");
});

t("tag split across chunk boundary", function () {
  var ins = _tb_parser.make();
  ins.feed("<a hr");
  ins.feed("ef='/x'");
  ins.feed(">t</a>");
  ins.feed("<p>q");
  ins.finalize();
  eq(ins.root.children[0].attrs.href, "/x");
  eq(ins.root.children[1].children[0].text, "q");
});

t("malformed input warns and continues", function () {
  var d = P("<p>a< <p>b");
  ok(d.warnings.length >= 1);
  var p1 = d.root.children[d.root.children.length - 1];
  eq(p1.children[0].text, "b");
});

t("ids increment from 1", function () {
  var d = P("<p>x</p><a href='/y'>y</a>");
  eq(d.root.children[0].id, 1);
  eq(d.root.children[1].id, 2);
});

t("readyState loading before finalize", function () {
  var ins = _tb_parser.make();
  ins.feed("<p>x</p>");
  eq(ins.readyState, "loading");
  ins.finalize();
});

done();
