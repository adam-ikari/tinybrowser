load("./tests/js/lib/testlib.js");
var P = _tb_parser;
function R(html, url, status) {
  var d = P.parse(html);
  document._tb_attach(d.root);
  return tb_render_js(d.root, url || "http://x/", status === undefined ? 200 : status);
}

t("text extraction with collapse", function () {
  var r = R("<p>  hello   world  </p><p>\n\nline2</p>");
  // push 不折叠连续换行:p1 block-after 1 个 + p2 字面 "\n\n" 2 个 = 3 个换行
  eq(r.text, "hello world\n\n\nline2");
});

t("block elements newline separated", function () {
  var r = R("<div>a</div><div>b</div>");
  eq(r.text, "a\nb");
});

t("inline no newline", function () {
  var r = R("<p>a<b>b</b>c</p>");
  eq(r.text, "abc");
});

t("title extracted", function () {
  var r = R("<title>Hello</title><p>x</p>");
  eq(r.title, "Hello");
});

t("links and buttons elems", function () {
  var r = R("<a href='/a'>A</a><button>Go</button>");
  eq(r.elems.length, 2);
  eq(r.elems[0].type, "link");
  eq(r.elems[0].href, "/a");
  eq(r.elems[0].text, "A");
  eq(r.elems[1].type, "button");
  eq(r.elems[1].text, "Go");
});

t("input by type", function () {
  var r = R("<input name='q'><input type='submit' value='Go'>");
  eq(r.elems.length, 2);
  eq(r.elems[0].type, "input");
  eq(r.elems[1].type, "button");
});

t("select options", function () {
  var r = R("<select name='s'><option>en</option><option>zh</option></select>");
  eq(r.elems.length, 1);
  eq(r.elems[0].type, "select");
  eq(r.elems[0].options.length, 2);
  eq(r.elems[0].options[1], "zh");
});

t("form elem recorded", function () {
  var r = R("<form action='/x'><input name='q'></form>");
  eq(r.elems.length, 2);
  eq(r.elems[0].type, "form");
});

t("script/style/head skipped from text", function () {
  var r = R("<head><title>T</title></head><script>var x=1;</script><style>p{}</style><p>v</p>");
  eq(r.text, "v");
});

t("offsets increase within text", function () {
  var r = R("<p>aaa<a href='/x'>bbb</a>ccc</p>");
  // aaa(0-2) link(3-5) ccc(6-8)
  eq(r.text, "aaabbbccc");
  eq(r.elems[0].off, 3);
});

t("leading/trailing blank trimmed", function () {
  var r = R("<div></div><p>mid</p><div></div>");
  eq(r.text, "mid");
});

t("deep tree does not overflow (iterative)", function () {
  var ins = P.make();
  var s = "";
  for (var i = 0; i < 2000; i++) s += "<div>";
  s += "x";
  for (i = 0; i < 2000; i++) s += "</div>";
  ins.feed(s); ins.finalize();
  document._tb_attach(ins.root);
  var r = tb_render_js(ins.root, "http://x/", 200);
  eq(r.text, "x");
});

t("option text not in body", function () {
  var r = R("<select><option>en</option><option>zh</option></select>");
  eq(r.text, "");
});

t("elems id consecutive from 1 over recorded only (M1 counter parity)", function () {
  var r = R("<p>skip</p><a href='/a'>A</a><br><button>Go</button><input name='q'>");
  // 非记录元素(p/br/文本)不占号;link=1, button=2, input=3 —— 视图 id 计数器规则
  eq(r.elems.length, 3);
  eq(r.elems[0].id, 1);
  eq(r.elems[0].type, "link");
  eq(r.elems[1].id, 2);
  eq(r.elems[1].type, "button");
  eq(r.elems[2].id, 3);
  eq(r.elems[2].type, "input");
});

t("view id resolves to node via mapping (_tb_elem_info)", function () {
  var r = R("<a href='/x'>l</a><input name='q'><button>Go</button>");
  var a = JSON.parse(_tb_elem_info(r.elems[0].id));
  eq(a.tag, "a");
  eq(a.href, "/x");
  var inp = JSON.parse(_tb_elem_info(r.elems[1].id));
  eq(inp.tag, "input");
  eq(inp.type, "text");
  var btn = JSON.parse(_tb_elem_info(r.elems[2].id));
  eq(btn.tag, "button");
});

t("button form resolves to form's view id", function () {
  var r = R("<form action='/s'><input name='q'><button>Go</button></form>");
  // 记录序:form=1, input=2, button=3;_tb_form_of 应返回 form 的视图 id=1
  eq(r.elems[0].type, "form");
  eq(r.elems[0].id, 1);
  var btn = JSON.parse(_tb_elem_info(r.elems[2].id));
  eq(btn.tag, "button");
  eq(btn.form, 1);
});

done();
