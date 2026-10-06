// 渲染:DOM 树 → {text, title, url, status, elems}。
//
// 本文件曾逐行对照 src/core/render.c(M1 的 C 版渲染器)编写。那套代码在 M2a
// 迁移到 qzjs 引擎后已彻底死掉并被删除 —— 继续保留对它的行号引用会失效,
// 也会让人以为「改 render.c 就能改行为」。故此处只保留**语义**说明,
// 不再指向任何文件。行为规格由 tests/goldens/ 下的 golden 逐字节钉住。
// 约束:所有树遍历均为显式栈迭代(QuickJS 栈深受限,禁递归)。

var _tb_block = { p:1, div:1, h1:1, h2:1, h3:1, h4:1, h5:1, h6:1, li:1, ul:1, ol:1,
  table:1, tr:1, section:1, article:1, header:1, footer:1, nav:1, aside:1,
  br:1, hr:1, blockquote:1, pre:1, form:1, select:1, fieldset:1 };
var _tb_skip = { script:1, style:1, noscript:1, head:1 };

// 视图 id(计数器) → 节点 映射。render 输出 M1 式 id——只给记录的
// link/form/input/button/select 按 walk 序从 1 连续编号,
// 而非 parser 的 n.id(所有元素都编号)。此映射供 _tb_elem_info/_tb_form_info
// 把视图 id 解析回节点(方案A:golden 逐字节不变 + 交互 id 与 M1 一致)。
var __tb_view_nodes = {};

function tb_render_js(doc, url, status) {
  var c = { text: "", title: "", elems: [], url: url || "", status: status };
  var nextId = 0;                // 视图 id 计数器:仅记录的元素递增
  __tb_view_nodes = {};          // 每渲染一张视图重建映射
  var at_line_start = true;      // 行首空白不追加

  // push:按字符逐个处理。不折叠连续换行;\r 走 else 分支原样追加。
  function push(s) {
    for (var k = 0; k < s.length; k++) {
      var ch = s[k];
      if (at_line_start && (ch === " " || ch === "\t")) continue;       // 行首空白
      if (ch === "\n") {
        while (c.text.length > 0 && c.text[c.text.length - 1] === " ")  // 行尾空白(只剥 ' ')
          c.text = c.text.slice(0, -1);
        c.text += "\n";
        at_line_start = true;
      } else if (ch === " " || ch === "\t") {
        if (!at_line_start && c.text.length > 0 &&
            c.text[c.text.length - 1] !== " " && c.text[c.text.length - 1] !== "\n")
          c.text += " ";                                                // 折叠连续空白
      } else {
        c.text += ch;                                                   // 含 \r 原样
        at_line_start = false;
      }
    }
  }
  function newline() { push("\n"); }

  // add_elem:href/name/value 全部从 attrs 同名取。
  // id 用计数器而非 n.id —— 记录序从 1 连续,golden 逐字节钉住这个编号规则。
  function addElem(type, n) {
    var e = { id: ++nextId, type: type, text: "", href: n.attrs.href || "",
              name: n.attrs.name || "", value: n.attrs.value || "",
              options: [], off: c.text.length };
    if (type === "select" || type === "form") {
      e.text = "";                                                      // 容器:text 无意义
    } else {
      // 后代全部文本(不含自身,迭代),isspace 式去首尾(JS trim;fixtures 全 ASCII)
      var out = "", st = [n];
      while (st.length) {
        var x = st.pop();
        if (x.type === "text") out += x.text;
        else for (var q = x.children.length - 1; q >= 0; q--) st.push(x.children[q]);
      }
      e.text = out.trim();
    }
    c.elems.push(e);
    __tb_view_nodes[String(e.id)] = n;   // 视图 id → 节点(交互查询用)
  }
  function isBlock(tag) { return _tb_block[tag] ? true : false; }

  // walk:显式栈帧 {n, phase, savedSel}。禁递归(QuickJS 栈深受限)。
  // phase 0=进入(dispatch + block-before + 逆序压子),1=退出(block-after + 恢复 curSel)。
  // curSel = 当前 select 的 elem 下标(非节点 id);-1 = 不在 select 内。
  var curSel = -1;
  var stack = [{ n: doc, phase: 0 }];

  while (stack.length) {
    var f = stack[stack.length - 1];
    var n = f.n;
    if (f.phase === 0) {
      f.phase = 1;
      f.savedSel = curSel;
      if (n.type === "text") { push(n.text); stack.pop(); continue; }   // 文本无条件 push
      if (n.type !== "element") { stack.pop(); continue; }
      var tag = n.tag;
      if (_tb_skip[tag] || tag === "title") { stack.pop(); continue; }  // 整棵子树跳过

      if (tag === "a" && n.attrs.href) {
        addElem("link", n);
      } else if (tag === "input") {
        var it = n.attrs.type || "";
        if (it === "hidden") { stack.pop(); continue; }                 // 跳过整棵子树、不记 elem
        addElem((it === "submit" || it === "button" || it === "reset") ? "button" : "input", n);
      } else if (tag === "button") {
        addElem("button", n);
      } else if (tag === "select") {
        addElem("select", n);
        curSel = c.elems.length - 1;                                   // elem 下标
      } else if (tag === "form") {
        addElem("form", n);
      } else if (tag === "option") {
        if (curSel >= 0) {
          var fc = n.children.length ? n.children[0] : null;           // 首子文本
          c.elems[curSel].options.push((fc && fc.type === "text") ? fc.text : "");
        }
        curSel = f.savedSel;
        stack.pop();                                                   // 不进子树、无 block 换行
        continue;
      }

      if (isBlock(tag) && !at_line_start) newline();                   // block-before
      for (var k = n.children.length - 1; k >= 0; k--)                 // 逆序入栈保文档序
        stack.push({ n: n.children[k], phase: 0 });
    } else {
      if (isBlock(n.tag) && !at_line_start) newline();                 // block-after
      curSel = f.savedSel;
      stack.pop();
    }
  }

  // off 钳制 + 首尾裁行。仅当 start>0(前导换行被裁)时执行。
  var t = c.text;
  var start = 0;
  while (t[start] === "\n") start++;
  var end = t.length;
  while (end > start && t[end - 1] === "\n") end--;
  var nl = end - start;
  c.text = t.slice(start, end);
  if (start) {
    for (var i = 0; i < c.elems.length; i++) {
      var o = c.elems[i].off >= start ? c.elems[i].off - start : 0;
      c.elems[i].off = o > nl ? nl : o;
    }
  }

  // title:文档序第一个 <title> 的首子文本。
  // 解析器不自动包 head,故用整树显式栈查找(等价于"文档序第一个 title")。
  var tstack = [doc];
  while (tstack.length) {
    var tn = tstack.pop();
    if (tn.type === "element") {
      if (tn.tag === "title") {
        var tc = tn.children.length ? tn.children[0] : null;
        c.title = (tc && tc.type === "text") ? tc.text : "";
        break;
      }
      for (var j = tn.children.length - 1; j >= 0; j--) tstack.push(tn.children[j]);
    }
  }

  return c;
}
