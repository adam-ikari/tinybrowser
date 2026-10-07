// DOM 子集。节点为 parser 纯数据;此处挂原型方法与全局 document。
// 约束:所有树遍历均为显式栈迭代(QuickJS 栈深受限,禁递归)。
var _tb_node_proto = {
  /* tagName 按 HTML 规范返回**大写**(元素名在 HTML 里是 ASCII 大写不敏感,
   DOM 的 tagName/nodeName 对 HTML 元素一律大写)。此前这里返回小写,而我新加的
   nodeName 返回大写 —— 同一个节点两个属性大小写不一致,更糟。
   页面脚本普遍写 el.tagName === "DIV",对着真浏览器写的代码都按大写来。
   ⚠️ 内部逻辑一律用 n.tag(小写,parser 归一过),不用 tagName,故无连带影响。 */
get tagName() { return this.tag ? String(this.tag).toUpperCase() : ""; },
  get childNodes() { return this.children || []; },
  get parentNode() { return this.parent || null; },
  /* nodeType:脚本很常用(x.Node.ELEMENT_NODE / n.nodeType === 1)。
     此前整个属性都没有,于是 `Node is not defined` 之后紧跟一串
     "not a function"。取值按 DOM 标准:元素 1、文本 3、文档 9。 */
  get nodeType() {
    if (this.type === "text") return 3;
    if (this.type === "element") return 1;
    return 9;
  },
  get nodeName() { return this.type === "text" ? "#text" : (this.tagName.toUpperCase ? this.tagName.toUpperCase() : this.tagName); },
  /* appendChild 要做两件此前没做的事,否则 JS 动态建的控件在页面里是死的:
   1) 给新节点(及其子树)挂原型 —— 此前 createElement 建的节点靠自带的
      __proto__ 活着,但 createTextNode 式直接塞进来的对象、以及带孙节点的
      片段,原型是缺的,于是 .tagName / .textContent 全 undefined。
   2) 挂进 form 时重新定义具名属性 —— 具名属性是 attach 时算好的,
      之后塞进来的控件不会自动出现 form.q。 */
appendChild: function (n) {
    if (!n) throw new Error("appendChild: null");
    n.parent = this;
    this.children.push(n);
    var stack = [n];
    while (stack.length) {
      var x = stack.pop();
      if (x.type === "element")
        Object.setPrototypeOf(x, x.tag === "form" ? _tb_form_proto : _tb_node_proto);
      for (var i = x.children.length - 1; i >= 0; i--) stack.push(x.children[i]);
    }
    for (var p = this; p; p = p.parent) {
      if (p.tag === "form") { _tb_define_named(p); break; }
    }
    return n;
  },
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
  /* 节点之间有 parent 指针 → **循环引用**。JSON.stringify 遇到循环会抛
   * TypeError,而 qzjs 的 ctl_eval 是靠 JSON.stringify 生成回执的 → 回执丢失,
   * 宿主只能等到超时(实测整整 5s)并报一句误导的 "control round-trip failed"。
   *
   * 触发条件很窄但真实:表达式本身就是 appendChild/removeChild 的返回值时。
   * 页面脚本内部调用不走 eval 缝,碰不到;但 `tb_eval_js(b,
   * "document.body.appendChild(el)")` 这种会踩到 —— 症状是「莫名卡 5 秒还失败」。
   *
   * 浏览器里 JSON.stringify(element) 得到 "{}"(DOM 节点没有可枚举自有属性)。
   * 这里返回 {} 与浏览器一致,并顺带消掉循环。取内容请用 outerHTML / textContent,
   * 那才是正路。 */
  toJSON: function () { return {}; },
  setAttribute: function (n, v) { this.attrs[n] = String(v); },
  removeAttribute: function (n) { delete this.attrs[n]; },
  /* value 必须是真的访问器,不能靠 `el.value = x` 落成普通自有属性。
   *
   * 脚本写得最多的就是 `f.q.value = "..."` 和读回来。若没有这个访问器,
   * 赋值会变成一个与 attrs 无关的普通属性,而 _tb_control_value 读的是
   * attrs.value —— 提交出去的还是页面原始值,用户/脚本填的内容凭空消失,
   * 而且**不报任何错**。set 写进 __tb_value(tb_fill/tb_select 用的同一个槽),
   * get 走 _tb_control_value,于是「DOM 原值 / 填过的值」只有一处定义。
   *
   * setAttribute("value", …) 仍然有效:getter 的优先级是 __tb_value > attrs.value。 */
  get value() { return _tb_control_value(this); },
  set value(v) { this.__tb_value = String(v); },
  get innerHTML() {
    // 子树序列化(children-only:与 innerHTML 惯例一致,测试要求 div.innerHTML
    // 不含 <div> 自身包裹)。显式栈 enter/exit 帧做先序序列化,禁递归。
    var out = "";
    var stack = [];
    var ch = this.children || [];
    for (var i = ch.length - 1; i >= 0; i--) stack.push({ n: ch[i], exit: false });
    while (stack.length) {
      var f = stack.pop();
      var n = f.n;
      if (n.type === "text") { out += _tb_esc(n.text); continue; }
      if (f.exit) { out += "</" + n.tag + ">"; continue; }
      var a = "";
      for (var k in n.attrs) a += " " + k + '="' + _tb_esc(n.attrs[k]) + '"';
      out += "<" + n.tag + a + ">";
      stack.push({ n: n, exit: true });
      for (var j = n.children.length - 1; j >= 0; j--) stack.push({ n: n.children[j], exit: false });
    }
    return out;
  },
  get outerHTML() {
    // 自身 + 子树。与 innerHTML 差只在最外层包裹:开标签 + this.innerHTML + 闭标签。
    // 文本节点无包裹,就是转义后的文本(浏览器语义)。
    if (this.type === "text") return _tb_esc(this.text || "");
    var a = "";
    for (var k in this.attrs) a += " " + k + '="' + _tb_esc(this.attrs[k]) + '"';
    return "<" + this.tag + a + ">" + this.innerHTML + "</" + this.tag + ">";
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
function _tb_esc(s) { return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;"); }

// 新节点 id 计数器。parser 不暴露 nextId/内部计数(grep 证实),故在 _tb_attach
// 里按树上最大数值 id 播种:createElement / textContent 写入新建节点时,
// 新 id 永不与已解析节点的 id 冲突。
var _tb_next_id = function () { return 1; };

/* ---- HTMLCollection:length + 数字下标 + 命名访问 ----
 *
 * 网页脚本找控件的三种写法都要支持:
 *   document.forms[0] / document.forms["search"] / document.forms.namedItem("search")
 *   form.elements[0] / form.elements.q
 * 前两种靠数组,第三种靠「任意属性名 → 具名元素」的解析。
 *
 * 命名访问只能用 Proxy:名字是运行期才知道的,原型上没法预先定义。
 * get 陷阱里对非字符串键(symbol)直接放行 —— 否则 Symbol.iterator 之类
 * 的探查会被当成「找具名元素」而误返回节点。
 */
function _tb_collection(list) {
  var c = {
    length: list.length,
    item: function (i) { return list[i] || null; },
    namedItem: function (nm) {
      for (var i = 0; i < list.length; i++) {
        var a = list[i].attrs || {};
        if (a.name === nm || a.id === nm) return list[i];
      }
      return null;
    },
    toArray: function () { return list.slice(); }
  };
  for (var i = 0; i < list.length; i++) c[i] = list[i];
  if (typeof Proxy !== "function") return c;          // 无 Proxy 时退化为纯下标
  return new Proxy(c, {
    get: function (t, k) {
      if (typeof k === "string" && !(k in t)) {
        var n = t.namedItem(k);
        if (n) return n;
      }
      return t[k];
    }
  });
}

/* 文档序收集 root 子树里满足 pred 的元素(含 root 本身)。显式栈,禁递归。 */
function _tb_collect(root, pred) {
  var out = [], stack = [root];
  while (stack.length) {
    var n = stack.pop();
    if (!n) continue;
    if (n.type === "element" && pred(n)) out.push(n);
    for (var i = n.children.length - 1; i >= 0; i--) stack.push(n.children[i]);
  }
  return out;
}

var _tb_CONTROL_TAGS = { input:1, textarea:1, select:1, button:1,
                         fieldset:1, output:1, object:1, keygen:1 };

/* form 内的控件,文档序。含 form 自身之外的后代(form 本身不算控件)。 */
function _tb_form_controls(form) {
  var out = [], stack = [];
  for (var i = form.children.length - 1; i >= 0; i--) stack.push(form.children[i]);
  while (stack.length) {
    var n = stack.pop();
    if (!n) continue;
    if (n.type === "element" && _tb_CONTROL_TAGS[n.tag]) out.push(n);
    for (var j = n.children.length - 1; j >= 0; j--) stack.push(n.children[j]);
  }
  return out;
}

/* form 的 query 串。GET 提交用;POST 走不了 mailbox(见 form.submit 的说明)。 */
function _tb_form_query(form) {
  var parts = [], ctrls = _tb_form_controls(form);
  for (var i = 0; i < ctrls.length; i++) {
    var nm = ctrls[i].attrs.name;
    /* 无 name 的控件不提交(HTML 规范)。submit/reset/button 类型的 input
       有 name 也会随表单提交,故此处不按 type 过滤。 */
    if (!nm) continue;
    if (ctrls[i].tag === "input" && ctrls[i].attrs.type === "file") continue;
    parts.push(encodeURIComponent(nm) + "=" + encodeURIComponent(_tb_control_value(ctrls[i])));
  }
  return parts.join("&");
}

/* 给 form 定义具名属性(form.q)。只在 attach / appendChild 时做,因为名字
 * 是运行期才知道的 —— 与 _tb_collection 不同,这里不能靠 Proxy:节点身份
 * 必须保持不变(parser 的 children、attrs、_tb_view_nodes 都按引用比)。
 *
 * 已有同名成员不覆盖:浏览器里 form.submit / form.elements 这类接口成员
 * 优先于具名控件,一个叫 "submit" 的 input 不该把 form.submit() 顶掉。 */
function _tb_define_named(form) {
  var ctrls = _tb_form_controls(form);
  for (var i = 0; i < ctrls.length; i++) {
    var node = ctrls[i];
    var names = [node.attrs.name, node.attrs.id];
    for (var k = 0; k < names.length; k++) {
      var nm = names[k];
      if (!nm || nm in form) continue;
      try {
        Object.defineProperty(form, nm, {
          get: (function (nd) { return function () { return nd; }; })(node),
          configurable: true, enumerable: false
        });
      } catch (e) { /* 不可定义的键(Proxy 键等)跳过,不影响其余 */ }
    }
  }
}

/* form 专属接口。与 _tb_node_proto 分开是因为「div.elements 是空的」
   这类半吊子属性不如没有 —— 分开之后只有 <form> 上才有。 */
var _tb_form_proto = Object.create(_tb_node_proto);
_tb_form_proto.elements = function () { return this; };   // 占位,下方用 getter 覆盖
Object.defineProperty(_tb_form_proto, "elements", {
  get: function () { return _tb_collection(_tb_form_controls(this)); },
  configurable: true
});
Object.defineProperty(_tb_form_proto, "length", {
  get: function () { return _tb_form_controls(this).length; },
  configurable: true
});
_tb_form_proto.action = "";      // 占位,下方 getter 覆盖
Object.defineProperty(_tb_form_proto, "action", {
  get: function () { return this.attrs.action || ""; },
  set: function (v) { this.attrs.action = String(v); },
  configurable: true
});
Object.defineProperty(_tb_form_proto, "method", {
  get: function () { return (this.attrs.method || "get").toLowerCase(); },
  set: function (v) { this.attrs.method = String(v).toLowerCase(); },
  configurable: true
});
Object.defineProperty(_tb_form_proto, "target", {
  get: function () { return this.attrs.target || ""; },
  set: function (v) { this.attrs.target = String(v); },
  configurable: true
});
Object.defineProperty(_tb_form_proto, "enctype", {
  get: function () { return this.attrs.enctype || "application/x-www-form-urlencoded"; },
  configurable: true
});
_tb_form_proto.reset = function () {
  var ctrls = _tb_form_controls(this);
  for (var i = 0; i < ctrls.length; i++) delete ctrls[i].__tb_value;
};
_tb_form_proto.submit = function () {
  /* qzjs 从不回调宿主,故不能像宿主 API 那样直接提交。走已有的导航指令通道
     (与 location.href= 同一条 mailbox):把 action + query 拼好交给宿主。
     相对 action 由宿主按当前文档 URL 解析,语义正确。
     POST **不支持**:导航指令只做 GET。POST 表单静默降级成 GET+query 会是
     语义错误(参数位置不同),故出声警告让脚本与人都知道。 */
  try {
    if (this.method === "post") {
      __tb_console.warn('form.submit(): POST not supported, ignoring');
      return false;
    }
    var q = _tb_form_query(this);
    var url = this.action + (q ? (this.action.indexOf("?") < 0 ? "?" : "&") + q : "");
    __tb_send_nav("navigate", url);
    return true;
  } catch (e) {
    return false;
  }
};

var document = {
  _root: null,
  _tb_attach: function (root) {
    document._root = root;
    var maxId = 0;
    // 迭代挂原型 + 顺带统计最大数值 id(禁递归)
    var stack = [root];
    var forms = [];
    while (stack.length) {
      var n = stack.pop();
      if (n.id > maxId) maxId = n.id;
      Object.setPrototypeOf(n, n.tag === "form" ? _tb_form_proto : _tb_node_proto);
      if (n.tag === "form") forms.push(n);
      for (var k = n.children.length - 1; k >= 0; k--) stack.push(n.children[k]);
    }
    /* 具名属性(form.q)必须**遍历结束后**再定:定义时要读到整棵子树的控件,
       而 _tb_form_controls 会走 children —— 控件若还没挂上原型也无妨,
       但顺序上放在这里更清楚,也便于一次算完。 */
    for (var f = 0; f < forms.length; f++) _tb_define_named(forms[f]);
    _tb_next_id = function () { return ++maxId; };
  },
  get documentElement() { return document._root; },
  /* ---- 集合类成员 ----
   * document.forms 是网页脚本找表单的正路。缺了它,页面自己的 JS 直接
   * "document.forms is undefined",后续一串连锁失败(实测 Google 首页就是)。
   * 每次取用都现算:DOM 会被脚本改,缓存下来的集合会立刻过期。
   */
  get forms() {
    return _tb_collection(_tb_collect(document._root, function (n) { return n.tag === "form"; }));
  },
  get links() {
    return _tb_collection(_tb_collect(document._root,
      function (n) { return n.tag === "a" && !!n.attrs.href; }));
  },
  get images() {
    return _tb_collection(_tb_collect(document._root, function (n) { return n.tag === "img"; }));
  },
  get all() { return document.body ? _tb_collection([document.body]) : _tb_collection([]); },
  /* U+26A0 FE0F 刻意没有 document.scripts / document.styleSheets:parser 把
     script/style 记进 scripts 列表而**不进 DOM 树**(见 parser.js 的 RAW 分支),
     故这类集合恒为空。发一个永远返回空的 API 比不发更糟 —— 脚本会以为
     「页面没有脚本」而走进错误分支。将来若把 script/style 也放进树,再补。 */
  getElementsByTagName: function (tag) {
    tag = String(tag).toLowerCase();
    return _tb_collection(_tb_collect(document._root, function (n) { return n.tag === tag; }));
  },
  getElementsByClassName: function (cls) {
    var want = String(cls).split(/\s+/).filter(function (s) { return s; });
    return _tb_collection(_tb_collect(document._root, function (n) {
      var have = (n.attrs.class || "").split(/\s+/);
      for (var i = 0; i < want.length; i++) if (have.indexOf(want[i]) < 0) return false;
      return want.length > 0;
    }));
  },
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
    tag = String(tag).toLowerCase();
    var n = { type: "element", tag: tag, attrs: {}, children: [], text: "", id: _tb_next_id(), parent: null };
    Object.setPrototypeOf(n, tag === "form" ? _tb_form_proto : _tb_node_proto);
    return n;
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
 *   select   → 首个带 selected 的 option 的文本(无任何 selected 时取第一个
 *              option,即 HTML 默认值);选过则以 __tb_value 为准
 *
 * 此前一律 `n.attrs.value || ""`,于是 textarea 与 select 提交上去的值恒为空。
 * Google 搜索框正是 <textarea name="q">,所以搜索词永远发不出去 ——
 * 实测这正是「在 Google 上搜不了」的直接原因。
 * __tb_value 优先于一切:用户填/选的值必须压过 DOM 里的默认值。 */
function _tb_control_value(n) {
  if (n.__tb_value !== undefined && n.__tb_value !== null) return String(n.__tb_value);
  if (n.tag === "textarea") return n.textContent;
  if (n.tag === "select") {
    // 显式栈按文档序收集 option:首个带 selected 的生效,否则第一个
    // (option 可能被 optgroup 包住,故遍历整棵子树而非只看直接子节点)
    var first = null, chosen = null;
    var st = [n];
    while (st.length) {
      var x = st.pop();
      if (x.type === "element" && x.tag === "option") {
        if (!first) first = x;
        if (x.attrs.selected !== undefined && !chosen) chosen = x;
      }
      for (var k = x.children.length - 1; k >= 0; k--) st.push(x.children[k]);
    }
    var pick = chosen || first;
    if (pick) {
      var fc = pick.children.length ? pick.children[0] : null;
      return (fc && fc.type === "text") ? fc.text : "";
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

/* ---- Node / Element / HTMLElement 全局 ----
 *
 * 真实页面极频繁地用 `x instanceof Node`、`Node.ELEMENT_NODE`、`node.nodeType`。
 * 此前 `Node` 根本不存在,Google 首页一上来就抛 "Node is not defined"。
 *
 * 做法是把 prototype 直接指向节点原型,于是 `n instanceof Node` 为真 ——
 * instanceof 只检查 Node.prototype 是否在原型链上,而我们的节点正是以
 * _tb_node_proto 为原型。
 *
 * 只在缺失时定义:qzjs 的 polyfill 若将来自己提供同名全局,不能被我们覆盖。
 */
if (typeof globalThis.Node === "undefined") {
  globalThis.Node = function Node() {};
  globalThis.Node.prototype = _tb_node_proto;
  globalThis.Node.ELEMENT_NODE = 1;
  globalThis.Node.TEXT_NODE = 3;
  globalThis.Node.COMMENT_NODE = 8;
  globalThis.Node.DOCUMENT_NODE = 9;
}
if (typeof globalThis.Element === "undefined") {
  globalThis.Element = function Element() {};
  globalThis.Element.prototype = _tb_node_proto;
}
if (typeof globalThis.HTMLElement === "undefined") {
  globalThis.HTMLElement = function HTMLElement() {};
  globalThis.HTMLElement.prototype = _tb_node_proto;
}
/* 节点也是 form 的实例(链上是 form_proto → node_proto),故 instanceof 一并成立。 */
if (typeof globalThis.HTMLFormElement === "undefined") {
  globalThis.HTMLFormElement = function HTMLFormElement() {};
  globalThis.HTMLFormElement.prototype = _tb_form_proto;
}
