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

/* ================= <textarea> 必须被当成可填控件 =================
 *
 * textarea 是**现在**最常见的文本输入控件:Google 搜索框就是
 * <textarea name="q" rows="1">,不是 <input>。此前 render.js 完全没有
 * textarea 分支 —— 它既不进 elems(不可见、不可点、不可填),也不进
 * _tb_form_pairs(填了也不会提交)。实测在 Google 上因此根本没法搜索。
 */

t("textarea becomes a fillable elem", function () {
  var r = R("<form action='/s'><textarea name='q' rows='1'></textarea><button>Go</button></form>");
  /* 记录序:form=1, textarea=2, button=3 */
  eq(r.elems.length, 3);
  eq(r.elems[1].type, "input");      // 与 input 同属可填文本控件
  eq(r.elems[1].name, "q");
  eq(r.elems[1].value, "");          // 空 textarea 的值是空串
  var info = JSON.parse(_tb_elem_info(r.elems[1].id));
  eq(info.tag, "textarea");          // 报真实 tag,不伪装成 input
  eq(info.type, "textarea");
});

t("textarea value lives in its content, not in a value attribute", function () {
  // 与 input 相反:input 的值在 attrs.value,textarea 的值在子文本
  var r = R("<textarea name='q'>hello world</textarea>");
  eq(r.elems[0].value, "hello world");
  var inp = R("<input name='q' value='from attr'>");
  eq(inp.elems[0].value, "from attr");
});

t("a filled textarea reports the filled value, not the original content", function () {
  var d = P.parse("<textarea name='q'>original</textarea>");
  document._tb_attach(d.root);
  var r = tb_render_js(d.root, "http://x/", 200);
  eq(r.elems[0].value, "original");
  // 模拟 tb_fill 的落点(_tb_set_value 写 __tb_value)
  _tb_set_value(r.elems[0].id, "typed by user");
  var r2 = tb_render_js(d.root, "http://x/", 200);
  eq(r2.elems[0].value, "typed by user");
});

t("textarea occupies its own line (treated as a block)", function () {
  // 不当 block 时内容会与前后文本挤在一行,填了值也读不出边界。
  // 换行数别凭印象数:block-after 加 1 个,而 block-before 只在「不在行首」时
  // 才加 —— 紧跟在 </p> 的 block-after 之后已经处于行首,故不再加。
  // 于是 p / textarea / p 三个兄弟 = "before" + \n + "inside" + \n + "after"。
  var r = R("<p>before</p><textarea name='q'>inside</textarea><p>after</p>");
  eq(r.text, "before\ninside\nafter");
  // 真正的对照:非 block 元素后面的文本会**贴在**它后面同一行,
  // 而 textarea 作为 block 会把它顶到下一行 —— 这才是可读性的差别。
  eq(R("<p>before</p><span>inside</span>tail").text, "before\ninsidetail");
  eq(R("<p>before</p><textarea name='q'>inside</textarea>tail").text, "before\ninside\ntail");
});

t("textarea is collected by _tb_form_pairs (submit carries the value)", function () {
  var d = P.parse("<form action='/s' method='get'><textarea name='q'>abc</textarea></form>");
  document._tb_attach(d.root);
  var r = tb_render_js(d.root, "http://x/", 200);
  _tb_set_value(r.elems[1].id, "search terms");   // elems[0]=form, [1]=textarea
  var out = JSON.parse(_tb_form_pairs(r.elems[0].id));
  eq(out.ok, true);
  eq(out.pairs.length, 1);
  eq(out.pairs[0].name, "q");
  eq(out.pairs[0].value, "search terms");
});

t("input, textarea and select coexist in one form", function () {
  var d = P.parse("<form action='/s'>" +
                  "<input name='a' value='1'>" +
                  "<textarea name='b'>2</textarea>" +
                  "<select name='c'><option>x</option></select>" +
                  "</form>");
  document._tb_attach(d.root);
  var r = tb_render_js(d.root, "http://x/", 200);
  var out = JSON.parse(_tb_form_pairs(r.elems[0].id));
  var got = [];
  for (var i = 0; i < out.pairs.length; i++) got.push(out.pairs[i].name + "=" + out.pairs[i].value);
  eq(got.join(","), "a=1,b=2,c=x");
});


/* ---- _tb_elem_info 的 select/textarea 分支 ----
 *
 * 这两个分支此前**完全没有测试**。发现方式很典型:做「破坏 select 默认值」那条
 * 负控时,把 _tb_elem_info 里一行与 _tb_control_value **完全相同**的
 * `if (n.tag === "select") {` 误当成后者替换掉了 —— 而测试全绿。
 * 也就是说这里坏掉了没人知道。这两条把它钉上。
 */
t("_tb_elem_info reports a select's options (was untested)", function () {
  var r = R("<select name='lang'><option>en</option><option>zh</option></select>");
  var info = JSON.parse(_tb_elem_info(r.elems[0].id));
  eq(info.tag, "select");
  eq(info.options.length, 2);
  eq(info.options[0], "en");
  eq(info.options[1], "zh");
});

t("_tb_elem_info options survive optgroup nesting (iterative, not direct children)", function () {
  // option 常被 optgroup 包住,只看直接子节点会取成整段 textContent
  // —— 实测过:那样 "en"+"zh" 会拼成 "enzh"
  var r = R("<select name='l'><optgroup label='g'><option>en</option></optgroup>" +
            "<optgroup label='h'><option>zh</option></optgroup></select>");
  var info = JSON.parse(_tb_elem_info(r.elems[0].id));
  eq(info.options.join(","), "en,zh");
});

t("_tb_elem_info reports a textarea's current value", function () {
  var d = P.parse("<textarea name='q'>content here</textarea>");
  document._tb_attach(d.root);
  var r = tb_render_js(d.root, "http://x/", 200);
  var info = JSON.parse(_tb_elem_info(r.elems[0].id));
  eq(info.tag, "textarea");
  eq(info.value, "content here");
  // 填过之后 _tb_elem_info 也要报填的值(视图与交互两条路径必须一致)
  _tb_set_value(r.elems[0].id, "typed");
  var info2 = JSON.parse(_tb_elem_info(r.elems[0].id));
  eq(info2.value, "typed");
});


done();
