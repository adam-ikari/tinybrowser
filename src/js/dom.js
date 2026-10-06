// DOM 子集。节点为 parser 纯数据;此处挂原型方法与全局 document。
// 约束:所有树遍历均为显式栈迭代(QuickJS 栈深受限,禁递归)。
var _tb_node_proto = {
  get tagName() { return this.tag ? this.tag : ""; },
  get childNodes() { return this.children || []; },
  get parentNode() { return this.parent || null; },
  appendChild: function (n) { n.parent = this; this.children.push(n); return n; },
  removeChild: function (n) {
    var i = this.children.indexOf(n);
    if (i < 0) throw new Error("removeChild: not a child");
    this.children.splice(i, 1); n.parent = null; return n;
  },
  get textContent() {
    // 迭代收集后代文本
    var out = "", stack = [this];
    while (stack.length) {
      var n = stack.pop();
      if (n.type === "text") out += n.text;
      else for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
    }
    return out;
  },
  set textContent(v) {
    this.children = [];
    var t = { type: "text", tag: null, attrs: {}, children: [], text: String(v), id: _tb_next_id(), parent: this };
    this.children.push(t);
  },
  getAttribute: function (n) { return Object.prototype.hasOwnProperty.call(this.attrs, n) ? this.attrs[n] : null; },
  setAttribute: function (n, v) { this.attrs[n] = String(v); },
  removeAttribute: function (n) { delete this.attrs[n]; },
  get innerHTML() {
    // 子树序列化(children-only:与 innerHTML 惯例一致,测试要求 div.innerHTML
    // 不含 <div> 自身包裹)。显式栈 enter/exit 帧做先序序列化,禁递归。
    function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }
    var out = "";
    var stack = [];
    var ch = this.children || [];
    for (var i = ch.length - 1; i >= 0; i--) stack.push({ n: ch[i], exit: false });
    while (stack.length) {
      var f = stack.pop();
      var n = f.n;
      if (n.type === "text") { out += esc(n.text); continue; }
      if (f.exit) { out += "</" + n.tag + ">"; continue; }
      var a = "";
      for (var k in n.attrs) a += " " + k + '="' + esc(n.attrs[k]) + '"';
      out += "<" + n.tag + a + ">";
      stack.push({ n: n, exit: true });
      for (var j = n.children.length - 1; j >= 0; j--) stack.push({ n: n.children[j], exit: false });
    }
    return out;
  },
  addEventListener: function (type, cb) {
    if (!this._listeners) Object.defineProperty(this, "_listeners", { value: {}, configurable: true, writable: true });
    if (!this._listeners[type]) this._listeners[type] = [];
    this._listeners[type].push(cb);
  },
  _fire: function (type) {
    if (!this._listeners || !this._listeners[type]) return;
    var cbs = this._listeners[type].slice();
    for (var i = 0; i < cbs.length; i++) { try { cbs[i]({}); } catch (e) {} }
  }
};

// 新节点 id 计数器。parser 不暴露 nextId/内部计数(grep 证实),故在 _tb_attach
// 里按树上最大数值 id 播种:createElement / textContent 写入新建节点时,
// 新 id 永不与已解析节点的 id 冲突。
var _tb_next_id = function () { return 1; };

var document = {
  _root: null,
  _tb_attach: function (root) {
    document._root = root;
    var maxId = 0;
    // 迭代挂原型 + 顺带统计最大数值 id(禁递归)
    var stack = [root];
    while (stack.length) {
      var n = stack.pop();
      if (n.id > maxId) maxId = n.id;
      Object.setPrototypeOf(n, _tb_node_proto);
      for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
    }
    _tb_next_id = function () { return ++maxId; };
  },
  get documentElement() { return document._root; },
  // 便捷引用:网页脚本第一件事就是 document.body.appendChild(...)。没有它的话
  // 每个 fixture 都得写 querySelector("body"),噪音大且易错。
  get body() { return document.querySelector("body"); },
  get head() { return document.querySelector("head"); },
  get readyState() { return _tb_parser._readyState || "complete"; },
  get title() {
    // 迭代找 title 文本
    var q = document.querySelectorAll("title");
    return q.length ? q[0].textContent : "";
  },
  set title(v) {
    var q = document.querySelectorAll("title");
    if (q.length) { q[0].textContent = v; return; }
    // 无 title:在 head 下建一个(没有 head 则建)
    var head = document.querySelector("head");
    if (!head) {
      head = document.createElement("head");
      var html = document._root;
      html.children.unshift(head);
      head.parent = html;
    }
    var t = document.createElement("title");
    t.textContent = v;
    head.appendChild(t);
  },
  createElement: function (tag) {
    return { type: "element", tag: String(tag).toLowerCase(), attrs: {}, children: [], text: "", id: _tb_next_id(), parent: null, __proto__: _tb_node_proto };
  },
  getElementById: function (id) {
    return document.querySelector("#" + id);
  },
  querySelectorAll: function (sel) {
    // 解析 sel:tag | .class | #id | tag.class
    var re = /^([a-zA-Z][a-zA-Z0-9:-]*)?([.#][a-zA-Z0-9_-]+)?$/;
    var m = re.exec(sel);
    if (!m) throw new Error("unsupported selector: " + sel);
    var tag = m[1] ? m[1].toLowerCase() : null;
    var cls = null, id = null;
    if (m[2]) {
      if (m[2][0] === ".") cls = m[2].slice(1);
      else id = m[2].slice(1);
    }
    var out = [];
    var stack = [document._root];
    while (stack.length) {
      var n = stack.pop();
      if (n.type === "element") {
        // #id 双匹配:HTML 字符串 id 属性 或 数值 id(供 _tb_elem_info/getElementById 以数值 id 查询)
        if ((!tag || n.tag === tag) &&
            (!cls || (n.attrs["class"] || "").split(/\s+/).indexOf(cls) >= 0) &&
            (!id || n.attrs.id === id || String(n.id) === id)) out.push(n);
        for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
      }
    }
    return out;
  },
  querySelector: function (sel) {
    var all = document.querySelectorAll(sel);
    return all.length ? all[0] : null;
  },
  /* cookie 走宿主 jar(见 cookie.h):读是 push 过来的 __tb_env.cookie(宿主在
     导航提交后算好推给本页),写是往 mailbox 发一条指令。
     此前这里是 `typeof __tb_cookie === 'function' ? ... : ''` —— 而该函数
     **从未定义**,于是 document.cookie 恒为 ""、赋值静默失效,是个假实现。
     HttpOnly 的 cookie 由宿主在计算时就排除,JS 侧看不见。 */
  get cookie() { return __tb_env.cookie || ""; },
  set cookie(v) { __tb_set_cookie(String(v)); }
};

// 交互辅助(供 C 侧 engine->eval):返回 JSON 字符串。
// id 是 render.js 输出的视图 id(计数器,方案A)。优先经 __tb_view_nodes 映射回节点;
// 未渲染过的文档(无映射)退回 getElementById 按 parser/attrs id 查。
function _tb_elem_info(id) {
  var n = null;
  if (typeof __tb_view_nodes === "object" && __tb_view_nodes[String(id)])
    n = __tb_view_nodes[String(id)];
  if (!n) n = document.getElementById(String(id));
  if (!n || n.type !== "element") return JSON.stringify({ tag: "" });
  var o = { tag: n.tag };
  if (n.tag === "a") o.href = n.attrs.href || "";
  if (n.tag === "input") o.type = n.attrs.type || "text";
  if (n.tag === "textarea") {
    /* Google 搜索框等现代文本控件。报真实 tag(不伪装成 input),由 C 侧
     * tb_fill 显式接受两者 —— 谎报 tag 会让「这个元素到底是什么」查不出来。 */
    o.type = "textarea";
    o.value = _tb_control_value(n);
  }
  if (n.tag === "button") { o.form = _tb_form_of(n); }
  if (n.tag === "select") {
    o.options = [];
    /* option 可能被包在其它标签里(optgroup 等),故迭代整棵子树,不能只看
     * 直接子节点 —— 直接子节点遍历会把 select 的 textContent 整段取成
     * 一个 option(实测 "en"+"zh" 拼成 "enzh")。 */
    var st = [n];
    while (st.length) {
      var x = st.pop();
      if (x.type === "element" && x.tag === "option") o.options.push(x.textContent);
      for (var q = x.children.length - 1; q >= 0; q--) st.push(x.children[q]);
    }
  }
  return JSON.stringify(o);
}
function _tb_form_of(node) {
  // 迭代向上找 form 祖先;返回其视图 id(render 计数器,与 _tb_elem_info 的 id 同域)。
  var p = node.parent;
  while (p) {
    if (p.type === "element" && p.tag === "form") {
      for (var k in __tb_view_nodes) if (__tb_view_nodes[k] === p) return Number(k);
      return p.id;   // 兜底:未渲染过的文档退回 parser id
    }
    p = p.parent;
  }
  return 0;
}

/* ---- 表单交互辅助(供 C 侧经控制面调用,不再走 C DOM 树) ----
 * 交互全部改为「宿主发控制面 eval → JS 在自己树上查」,故 C 侧不再有
 * find_node_by_id / collect_form_pairs,这些能力必须在 JS 侧提供。
 * 统一返回 JSON 字符串(与 _tb_elem_info 一致),便于宿主 cJSON 解析。 */

/* 视图 id → 节点。未渲染过的文档退回 getElementById。 */
function _tb_node_by_id(id) {
  var n = null;
  if (typeof __tb_view_nodes === "object" && __tb_view_nodes[String(id)])
    n = __tb_view_nodes[String(id)];
  if (!n) n = document.getElementById(String(id));
  return (n && n.type === "element") ? n : null;
}

/* 控件「当前值」的来源有三样,不能一律读 attrs.value:
 *   input    → value 属性
 *   textarea → **子文本**(textarea 没有 value 属性);填过则以 __tb_value 为准
 *   select   → 首个 option 的文本(HTML 规定 select 的默认值就是第一个 option;
 *              没有任何 selected 时即是它);选过则以 __tb_value 为准
 *
 * 此前一律 `n.attrs.value || ""`,于是 textarea 与 select 提交上去的值恒为空。
 * Google 搜索框正是 <textarea name="q">,所以搜索词永远发不出去 ——
 * 实测这正是「在 Google 上搜不了」的直接原因。
 * __tb_value 优先于一切:用户填/选的值必须压过 DOM 里的默认值。 */
function _tb_control_value(n) {
  if (n.__tb_value !== undefined && n.__tb_value !== null) return String(n.__tb_value);
  if (n.tag === "textarea") return n.textContent;
  if (n.tag === "select") {
    // 显式栈按文档序找第一个 option(option 可能被 optgroup 包住)
    var st = [n];
    while (st.length) {
      var x = st.pop();
      if (x.type === "element" && x.tag === "option") {
        var fc = x.children.length ? x.children[0] : null;
        return (fc && fc.type === "text") ? fc.text : "";
      }
      for (var k = x.children.length - 1; k >= 0; k--) st.push(x.children[k]);
    }
    return "";
  }
  return n.attrs.value || "";
}

/* form 的 action/method。未渲染文档用 getElementById 兜底。 */
function _tb_form_info(formId) {
  var f = _tb_node_by_id(formId);
  if (!f || f.tag !== "form") return JSON.stringify({ ok: false });
  return JSON.stringify({
    ok: true,
    action: f.attrs.action || "",
    method: (f.attrs.method || "get").toLowerCase()
  });
}

/* 收集 form 内所有具名控件,按文档序返回 [{id, name, value}]。
 * value 优先取 tb_fill/tb_select 写在**节点自身**的 __tb_value,
 * 否则回落 DOM 的 value 属性。
 *
 * 值必须存在节点上,不能另开一张 id → value 表:宿主传进来的是**渲染视图 id**
 * (render.js 的计数器,经 __tb_view_nodes 映射到节点),而这里遍历拿到的是
 * **解析节点 id**。两套 id 只有在简单文档上才碰巧相等 —— 实测
 * `<form><input><select><button>` 上渲染 q=3 / 解析 q=2,查表直接落空,
 * 提交出去的是原始 value 而非用户填的值。存在节点上则不存在换算问题。 */
function _tb_form_pairs(formId) {
  var f = _tb_node_by_id(formId);
  if (!f || f.tag !== "form") return JSON.stringify({ ok: false, pairs: [] });
  var pairs = [];
  // 迭代遍历(禁递归),保持文档序
  var stack = [f];
  while (stack.length) {
    var n = stack.pop();
    if (n.type === "element" &&
        (n.tag === "input" || n.tag === "select" || n.tag === "textarea")) {
      /* textarea 必须一并收:它的值在内容里不在 value 属性里,而现在大量站点
       * (含 Google 搜索框)用 <textarea name="q">。漏了它,填了值提交时也不带。 */
      var name = n.attrs.name;
      if (name) {
        pairs.push({ id: n.id, name: name, value: _tb_control_value(n) });
      }
    }
    for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
  }
  return JSON.stringify({ ok: true, pairs: pairs });
}

/* tb_fill / tb_select 的落点:把值记在节点上,提交时由 _tb_form_pairs 读。
 * id 可以是渲染视图 id 或解析节点 id(_tb_node_by_id 两者都认)。 */
function _tb_set_value(id, value) {
  var n = _tb_node_by_id(id);
  if (!n) return false;
  n.__tb_value = String(value);
  return true;
}

/* select 的 option 是否存在。宿主只传 id + option,判定在 JS 侧做——
 * 在 C 里拼这段遍历表达式既难维护又会撑爆宿主侧的表达式缓冲(option 可能被
 * optgroup 包住,必须遍历子树而非直接子节点)。
 * 返回 true/false。 */
function _tb_has_option(id, option) {
  var n = _tb_node_by_id(id);
  if (!n || n.tag !== "select") return false;
  var want = String(option);
  var st = [n];
  while (st.length) {
    var x = st.pop();
    if (x.type === "element" && x.tag === "option" && x.textContent === want)
      return true;
    for (var q = x.children.length - 1; q >= 0; q--) st.push(x.children[q]);
  }
  return false;
}
