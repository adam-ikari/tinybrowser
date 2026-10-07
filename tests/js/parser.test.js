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

/* ================= chunk 边界不变性(性质测试) =================
 *
 * feed() 是流式接口,切点由网络决定,解析结果不该依赖切点。
 * 修之前,三类切点会让结果与整段解析不同 —— 最严重的是 raw 元素:
 * "</script" 一旦跨切点就永远失配,整个 <script> 连同其后**所有**正文都被
 * 当成脚本内容,元素直接消失(喂 25 字节恰好不踩到,换个切点就中招)。
 *
 * 下面的做法是穷举:对每份文档的每一个字节位置都切一刀,要求与整段解析
 * **完全一致**(DOM、scripts、warnings 全比)。任何切点不一致都会在这里报出来。
 */
// 节点带 parent 指针,JSON.stringify 直接抛 circular reference —— 自己走树。
// 只序列化「可观察的解析结果」:tag / attrs / text / 子节点顺序 / scripts / warnings。
function ser(n, out) {
  if (n.type === "text") { out.push("T(" + n.text + ")"); return; }
  var a = [], k;
  for (k in n.attrs) a.push(k + "=" + n.attrs[k]);
  out.push("<" + n.tag + " " + a.sort().join(",") + ">");
  for (var i = 0; i < n.children.length; i++) ser(n.children[i], out);
  out.push("</" + n.tag + ">");
}
// cuts:null = 整段一次喂;数字 = 在该字节处切一刀;数字数组 = 切多刀
function snapshot(html, cuts) {
  var p = _tb_parser.make();
  if (cuts === null) p.feed(html);
  else {
    var at = [0].concat(cuts, [html.length]);
    for (var i = 1; i < at.length; i++) p.feed(html.slice(at[i - 1], at[i]));
  }
  p.finalize();
  var out = [];
  ser(p.root, out);
  var sc = [];
  for (var i = 0; i < p.scripts.length; i++)
    sc.push((p.scripts[i].src || "-") + "|" + (p.scripts[i].type || "-") + "|" + p.scripts[i].text);
  return out.join("") + "#S:" + sc.join(";;") + "#W:" + p.warnings.join(";");
}

var CHUNK_DOCS = [
  ["script",       "<title>T</title><script>var a=1;</script><p>body</p>"],
  ["script+text",  "<title>T</title><script>var a=1;</script><p>after</p><script>var b=2;</script>"],
  ["style",        "<title>T</title><style>p{color:red}</style><p>body</p>"],
  ["textarea",     "<title>T</title><textarea> a &lt; b </textarea>"],
  ["comment",      "<title>T</title><!-- note --><p>body</p>"],
  ["commentdash",  "<title>T</title><!-- a--b --><p>x</p>"],
  ["pi",           "<title>T</title><?xml-stylesheet href=a?><p>body</p>"],
  ["doctype",      "<!DOCTYPE html><title>T</title><p>body</p>"],
  ["quoted attr",  '<title>T</title><div class="a b" id=x>y</div>'],
  ["unquoted attr","<title>T</title><a href=/a/b?x=1&y=2>l</a>"],
  ["entities",     "<title>T</title><p>&lt;&amp;&nbsp;&#65;&#x1F600;</p>"],
  ["utf8",         "<title>T</title><p>\u4e2d\u6587 \ud83d\ude00</p>"],
  ["nested tags",  "<title>T</title><div><p>a<b>c</b></p></div>"],
  ["misnest",      "<title>T</title><b><i>x</b></i><p>y</p>"],
  ["void",         "<title>T</title><p>a<br>b<img src=x>c</p>"],
  ["rawtext in js","<title>T</title><script>var s='</div>';var t=\"<p>\";</script><p>x</p>"],
  ["close+attrs",  "<title>T</title><ul><li>a<li>b</ul><p>x</p>"],
  ["empty",        ""],
  ["text only",    "hello world"],
  ["bare lt",      "<title>T</title><p>a < b and 3<4</p>"]
];

t("chunk split must not change the parse (every byte offset)", function () {
  for (var d = 0; d < CHUNK_DOCS.length; d++) {
    var name = CHUNK_DOCS[d][0], html = CHUNK_DOCS[d][1];
    var ref = snapshot(html, null);
    for (var k = 1; k < html.length; k++) {
      eq(snapshot(html, k), ref,
         name + " split at offset " + k + " (" + html[k - 1] + "|" + html[k] + ")");
    }
  }
});

t("chunk split must not change the parse (three-way, small docs)", function () {
  // 两刀:覆盖「needle 被切成三段」这种两刀抓不到的情况
  var docs = ["<title>T</title><script>var a=1;</script><p>b</p>",
              "<title>T</title><!-- c --><p>b</p>",
              "<title>T</title><style>p{}</style><p>b</p>"];
  for (var d = 0; d < docs.length; d++) {
    var html = docs[d], ref = snapshot(html, null);
    for (var a = 1; a < html.length; a++) {
      for (var b = a + 1; b < html.length; b++)
        eq(snapshot(html, [a, b]), ref, "docs[" + d + "] cuts " + a + "," + b);
    }
  }
});

/* ================= raw 文本的闭合标签判定 ================= */

t("</scriptfoo> does not close a script (spec: name must end at a delimiter)", function () {
  var d = _tb_parser.parse("<title>T</title><script>var a=1;</scriptfoo>var b=2;</script>");
  eq(d.scripts.length, 1);
  eq(d.scripts[0].text, "var a=1;</scriptfoo>var b=2;");
});

t("</script> + space/tab/newline/slash/gt all close a script", function () {
  var delims = [" ", "\t", "\n", "\r", "\f", "/", ">"];
  for (var i = 0; i < delims.length; i++) {
    var d = _tb_parser.parse("<title>T</title><script>var a=1;</script" + delims[i] + "><p>x</p>");
    eq(d.scripts.length, 1, "delimiter " + JSON.stringify(delims[i]));
    eq(d.scripts[0].text, "var a=1;");
  }
});

t("</scriptx> is not a closing tag, so it stays script text", function () {
  // 规范如此:raw 文本只在 "</script" 后紧跟分隔符处结束。"></scriptx>" 不匹配,
  // 于是那串字符属于脚本正文,直到 EOF。
  var d = _tb_parser.parse("<title>T</title><script>var a=1;</scriptx>");
  eq(d.scripts.length, 1);
  eq(d.scripts[0].text, "var a=1;</scriptx>");
});

t("script truncated by a cut-off download still runs (EOF ends raw text)", function () {
  var d = _tb_parser.parse("<title>T</title><script>var a=1;");
  eq(d.scripts.length, 1);
  eq(d.scripts[0].text, "var a=1;");
});

/* ================= 属性名小写 / 重复属性 ================= */

t("attribute names are lowercased", function () {
  // dom.js 取值一律用小写键(n.attrs.href / .class / .id),标签名早已小写,
  // 属性名此前漏了 —— 于是 <a HREF=/x> 的 href 是空、链接是死的。
  var d = _tb_parser.parse("<title>T</title><a HREF=/x CLASS=big DATA-Y=1>l</a>");
  var a = d.root.children[1];
  eq(a.attrs.href, "/x");
  eq(a.attrs.class, "big");
  eq(a.attrs["data-y"], "1");
});

t("duplicate attributes keep the first (spec drops the later one)", function () {
  eq(_tb_parser.parse("<title>T</title><div a=1 a=2>x</div>").root.children[1].attrs.a, "1");
  // 混合:boolean 在前、带值在后
  eq(_tb_parser.parse("<title>T</title><div a a=2>x</div>").root.children[1].attrs.a, "");
  eq(_tb_parser.parse("<title>T</title><div a=2 a>x</div>").root.children[1].attrs.a, "2");
});

/* ================= 数值实体按码点解释 ================= */

t("numeric references above U+FFFF decode to the code point, not truncated", function () {
  // fromCharCode(0x1F600) 会 & 0xFFFF == 0xF600(私有区码位),此前
  // "&#x1F600;" 渲染成一个无意义字形。现在用 fromCodePoint。
  eq(_tb_parser.decodeEntities("&#x1F600;"), String.fromCodePoint(0x1F600));
  eq(_tb_parser.decodeEntities("&#128512;"), String.fromCodePoint(0x1F600));
  ok(_tb_parser.decodeEntities("&#x1F600;").codePointAt(0) === 0x1F600);
  eq(_tb_parser.decodeEntities("&#x10FFFF;"), String.fromCodePoint(0x10FFFF));
});

t("out-of-range and surrogate numeric references map to U+FFFD", function () {
  eq(_tb_parser.decodeEntities("&#0;"), "\uFFFD");
  eq(_tb_parser.decodeEntities("&#xD800;"), "\uFFFD");   // 代理区:JS 字符串里本就非法
  eq(_tb_parser.decodeEntities("&#xDFFF;"), "\uFFFD");
  eq(_tb_parser.decodeEntities("&#x110000;"), "\uFFFD"); // 越界
  eq(_tb_parser.decodeEntities("&#xFFFFFF;"), "\uFFFD");
});

t("BMP numeric references and named entities unchanged", function () {
  eq(_tb_parser.decodeEntities("&#65;"), "A");
  eq(_tb_parser.decodeEntities("&#x41;"), "A");
  eq(_tb_parser.decodeEntities("&amp;&lt;&gt;&quot;&apos;"), "&<>\"'");
  eq(_tb_parser.decodeEntities("&nbsp;").charCodeAt(0), 0xA0);
  eq(_tb_parser.decodeEntities("&unknown;"), "&unknown;");
  eq(_tb_parser.decodeEntities("no entities here"), "no entities here");
  eq(_tb_parser.decodeEntities(""), "");
});

/* ================= <script> 不一定是脚本 ================= */

t("script type is recorded so the loader can tell data blocks from code", function () {
  eq(_tb_parser.parse('<title>T</title><script>var a=1;</script>').scripts[0].type, null);
  eq(_tb_parser.parse('<title>T</title><script type="text/javascript">var a=1;</script>')
     .scripts[0].type, "text/javascript");
  eq(_tb_parser.parse('<title>T</title><script type="application/ld+json">{"a":1}</script>')
     .scripts[0].type, "application/ld+json");
  eq(_tb_parser.parse('<title>T</title><script type="text/javascript;charset=utf-8">x</script>')
     .scripts[0].type, "text/javascript;charset=utf-8");
});

t("language is a fallback for the obsolete absent type", function () {
  eq(_tb_parser.parse('<title>T</title><script language="javascript">x</script>')
     .scripts[0].type, "javascript");
  // type 在时 language 不参与
  eq(_tb_parser.parse('<title>T</title><script type="application/json" language="javascript">x</script>')
     .scripts[0].type, "application/json");
});

t("style content is not a script (CSS must not be eval'd as JS)", function () {
  // RAW 含 script 与 style,此前两者都塞进 scripts,加载器对每条都 eval
  var d = _tb_parser.parse("<title>T</title><style>p{color:red}</style><script>var a=1;</script>");
  eq(d.scripts.length, 1);
  eq(d.scripts[0].text, "var a=1;");
});

t("style is not recorded even when self-closed", function () {
  var d = _tb_parser.parse("<title>T</title><style /><script src=/a.js />");
  eq(d.scripts.length, 1);
  eq(d.scripts[0].src, "/a.js");
});


done();
