// 纯 JS 宽松 HTML/XML tokenizer → DOM 树。流式接口。
// 节点: {type, tag, attrs, children, text, id, parent}
// id 只给元素节点递增分配(1 起),文本节点 id=0;root(html) 固定 id=0 不占计数。
var _tb_parser = (function () {
  var VOID = { br:1, img:1, input:1, hr:1, meta:1, link:1 };
  var IMPLICIT = { p:1, li:1, td:1, tr:1, th:1, option:1 };
  var RAW = { script:1, style:1 };
  function newNode(type, tag, p) {
    var n = { type: type, tag: tag, attrs: {}, children: [], text: "" };
    n.id = (type === "element") ? ++p.next_id : 0;
    return n;
  }

  function decodeEntities(s) {
    // &amp; &lt; &gt; &quot; &apos; &nbsp; + &#NN; + &#xHH; + 未知实体原样保留
    return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|amp|lt|gt|quot|apos|nbsp);/g, function (m, e) {
      if (e === "amp") return "&";
      if (e === "lt") return "<";
      if (e === "gt") return ">";
      if (e === "quot") return '"';
      if (e === "apos") return "'";
      if (e === "nbsp") return " ";
      if (e[1] === "x") return String.fromCharCode(parseInt(e.slice(2), 16));
      return String.fromCharCode(parseInt(e.slice(1), 10));
    });
  }

  function Parser() {
    this.buf = "";          // 未消费输入(跨 chunk 续接)
    this.i = 0;             // 已消费游标
    this.state = "text";    // text | tagopen | tagname | attrname | attrvalue | close | comment | doctype | pi | raw
    this.stack = [];        // 开元素栈
    this.curTag = null;     // 正在解析的 start tag 名字
    this.curAttrs = {};
    this.curQuote = "";     // 属性值引号 '' | '"' | ""(unquoted)
    this.lastAttrName = null;   // 最近完成的属性名(_setLastAttr 写回目标)
    this.rawTag = null;     // raw 状态下的标签名(script/style)
    this.rawText = "";      // raw 文本累积
    this.curScriptSrc = null;   // 当前 raw 元素的 src(在清空 curAttrs 前捕获)
    this.root = { type:"element", tag:"html", attrs:{}, children:[], text:"", id:0, parent:null };
    this.stack.push(this.root);
    this.scripts = [];
    this.warnings = [];
    this.textBuf = "";      // TEXT 状态累积的文本(未 flush)
    this.readyState = "loading";
    this.finalized = false;
    this.next_id = 0;               // 元素 id 计数(每实例独立,文本节点不计)
  }

  Parser.prototype.feed = function (chunk) {
    if (this.finalized) return;
    this.buf += chunk;
    this._scan();
  };

  Parser.prototype.finalize = function () {
    if (this.finalized) return;
    this.finalized = true;
    this._scan();                 // 扫尽残留
    this._flushText();
    while (this.stack.length > 1) {           // 隐式关闭所有未闭合标签
      this._warn("unclosed <" + this.stack[this.stack.length - 1].tag + ">");
      this.stack.pop();
    }
  };

  Parser.prototype._warn = function (m) { this.warnings.push(m); };
  Parser.prototype._flushText = function () {
    if (this.textBuf) {
      var n = newNode("text", null, this);
      n.text = decodeEntities(this.textBuf);
      this.textBuf = "";
      this._append(n);
    }
  };

  Parser.prototype._append = function (n) {
    var top = this.stack[this.stack.length - 1];
    n.parent = top;
    top.children.push(n);
  };

  // _scan:主状态机,消费 buf 直到无法推进(不完整结构留待下次 feed)。
  // 死锁约束:每个分支必须推进 this.i、改 this.state,或 return。
  Parser.prototype._scan = function () {
    var s = this.buf, L = s.length;
    while (true) {
      if (this.state === "text") {
        var lt = s.indexOf("<", this.i);
        if (lt < 0) {
          this.textBuf += s.slice(this.i);
          this.i = L;
          return;                       // 等更多输入或 finalize
        }
        this.textBuf += s.slice(this.i, lt);
        this.i = lt + 1;
        if (s.slice(lt + 1, lt + 4) === "!--") { this.state = "comment"; this.i = lt + 4; }
        else if (s[lt + 1] === "!") { this.state = "doctype"; this.i = lt + 2; }
        else if (s[lt + 1] === "?") { this.state = "pi"; this.i = lt + 2; }
        else if (s[lt + 1] === "/") { this.state = "close"; this.i = lt + 2; }
        else if (/[a-zA-Z]/.test(s[lt + 1] || "")) { this.state = "tagname"; this.curTag = ""; this.curAttrs = {}; }
        else { this.textBuf += "<"; this.i = lt + 1; }   // "<" 后非法字符,当字面
      }
      else if (this.state === "tagname") {
        var m = /^[a-zA-Z][a-zA-Z0-9:-]*/.exec(s.slice(this.i));
        if (!m) { if (this.i >= L) return; this._warn("bad tag name"); this.state = "text"; continue; }
        // 跨 chunk:标签名一直延伸到缓冲末尾,不消费,等续接(finalize 时照常消费)
        if (this.i + m[0].length >= L && !this.finalized) return;
        this.curTag = m[0].toLowerCase();
        this.i += m[0].length;
        this.state = "tagopen";
      }
      else if (this.state === "tagopen") {
        // 之后可能是 > / > 属性 或 EOF
        if (this.i >= L) return;
        var c = s[this.i];
        if (c === ">") { this._closeStartTag(false); this.i++; continue; }
        else if (c === "/" && s[this.i + 1] === ">") { this._closeStartTag(true); this.i += 2; continue; }
        else if (/\s/.test(c)) { this.i++; this.state = "attrname"; continue; }
        else { this.state = "attrname"; continue; }   // 无空格属性名
      }
      else if (this.state === "attrname") {
        if (this.i >= L) return;
        var c = s[this.i];
        if (c === ">" || c === "/") {
          if (c === "/" && s[this.i + 1] !== ">") {
            // "/" 后不是 ">":可能自闭合被截断,延后;否则宽容跳过
            if (this.i + 1 >= L && !this.finalized) return;
            this.i++;
            continue;
          }
          this._closeStartTag(c === "/");
          this.i += (c === ">" ? 1 : 2);
          continue;
        }
        if (/\s/.test(c)) { this.i++; continue; }
        var am = /^[^=\s/>]+/.exec(s.slice(this.i));
        if (!am) { if (this.i >= L) return; this._warn("bad attr"); this.state = "text"; continue; }
        var an = am[0];
        // 跨 chunk:属性名到缓冲末尾,不消费,等续接
        if (this.i + an.length >= L && !this.finalized) return;
        this.i += an.length;
        this.curAttrs[an] = "";               // 先置空(boolean 默认)
        this.lastAttrName = an;
        if (s[this.i] === "=") { this.i++; this.state = "attrvalue"; }
        // 否则 boolean attr,继续 attrname
      }
      else if (this.state === "attrvalue") {
        if (this.i >= L) return;
        var c = s[this.i];
        if (this.curQuote) {
          // 续接跨 chunk 的带引号属性值
          var qe = s.indexOf(this.curQuote, this.i);
          if (qe < 0) {
            if (!this.finalized) return;              // 等更多输入
            this._setLastAttr(s.slice(this.i));       // finalize:剩余全当值
            this.i = L; this.curQuote = ""; this.state = "text"; continue;
          }
          this._setLastAttr(s.slice(this.i, qe));
          this.i = qe + 1; this.curQuote = ""; this.state = "tagopen"; continue;
        }
        if (c === '"' || c === "'") {
          this.curQuote = c; this.i++;
          var end = s.indexOf(c, this.i);
          if (end < 0) {
            // 引号未闭合:跨 chunk 留待续接;finalize 则剩余当值
            if (!this.finalized) return;
            this._setLastAttr(s.slice(this.i));
            this.i = L; this.curQuote = ""; this.state = "text"; continue;
          }
          this._setLastAttr(s.slice(this.i, end));
          this.i = end + 1; this.curQuote = ""; this.state = "tagopen"; continue;
        } else if (c === ">") {
          this._closeStartTag(false); this.i++; continue;
        } else {
          var um = /^[^ \t\r\n>]+/.exec(s.slice(this.i));   // unquoted value
          if (!um) { if (this.i >= L) return; this._warn("bad attr value"); this.state = "text"; continue; }
          // 跨 chunk:值到缓冲末尾,不消费,等续接
          if (this.i + um[0].length >= L && !this.finalized) return;
          this._setLastAttr(um[0]);
          this.i += um[0].length;
          this.state = "tagopen";
        }
      }
      else if (this.state === "close") {
        // 要求先有 ">",再匹配名字(容忍 </p class=x> 这类带属性关闭标签)
        var gt = s.indexOf(">", this.i);
        if (gt < 0) {
          if (!this.finalized) return;              // 等更多输入
          this.i = L; this.state = "text"; continue;   // finalize:丢弃残缺关闭标签
        }
        var cm = /^[a-zA-Z][a-zA-Z0-9:-]*/.exec(s.slice(this.i, gt));
        if (!cm) { this._warn("bad close tag"); this.i = gt + 1; this.state = "text"; continue; }
        this._doClose(cm[0].toLowerCase());
        this.i = gt + 1;
        this.state = "text";
      }
      else if (this.state === "comment") {
        var ce = s.indexOf("-->", this.i);
        if (ce < 0) { this.i = L; return; }   // 未完成,续接
        this.i = ce + 3; this.state = "text";
      }
      else if (this.state === "doctype") {
        var de = s.indexOf(">", this.i);
        if (de < 0) { this.i = L; return; }
        this.i = de + 1; this.state = "text";
      }
      else if (this.state === "pi") {
        var pe = s.indexOf("?>", this.i);
        var p2 = s.indexOf(">", this.i);
        var endAt = (pe < 0) ? p2 : (p2 < 0 ? pe : Math.min(pe, p2));
        if (endAt < 0) { this.i = L; return; }
        this.i = endAt + (s[endAt] === "?" ? 2 : 1); this.state = "text";
      }
      else if (this.state === "raw") {
        var re = s.indexOf("</" + this.rawTag, this.i);
        if (re < 0) { this.rawText += s.slice(this.i); this.i = L; return; }
        this.rawText += s.slice(this.i, re);
        this.i = re + 2 + this.rawTag.length;
        // 跳过 </script> 的 > (宽容:直到 >)
        var rgt = s.indexOf(">", this.i);
        if (rgt < 0) { this.i = L; return; }
        this.i = rgt + 1;
        this.scripts.push({ src: this.curScriptSrc, text: this.rawText });
        this.rawTag = null; this.rawText = ""; this.curScriptSrc = null;
        this.state = "text";
      }
    }
  };

  Parser.prototype._setLastAttr = function (v) {
    // 写回最后一个属性名(记录在 this.lastAttrName)
    if (this.lastAttrName == null) { this._warn("attr value without name"); return; }
    this.curAttrs[this.lastAttrName] = decodeEntities(v);
  };

  // _closeStartTag:闭合一个 start tag,并设置后续状态(RAW→"raw",否则→"text")。
  // RAW 元素(script/style)不进 DOM 树、不入栈,只记录脚本/样式并进入 raw 状态。
  Parser.prototype._closeStartTag = function (selfClose) {
    this._flushText();
    var tag = this.curTag;
    if (VOID[tag]) selfClose = true;
    // 隐式闭合:同栈顶同名
    if (IMPLICIT[tag] && this.stack[this.stack.length - 1].tag === tag) {
      this.stack.pop();
    }
    if (RAW[tag]) {
      var src = this.curAttrs.src || null;
      if (selfClose) {
        // <script src=x /> 自闭合:无内容记录
        this.scripts.push({ src: src, text: "" });
        this.state = "text";
      } else {
        this.curScriptSrc = src;
        this.rawTag = tag;
        this.rawText = "";
        this.state = "raw";
      }
    } else {
      var n = newNode("element", tag, this);
      n.attrs = this.curAttrs;
      this._append(n);
      if (!selfClose) this.stack.push(n);
      this.state = "text";
    }
    this.curTag = null;
    this.curAttrs = {};
  };

  Parser.prototype._doClose = function (tag) {
    this._flushText();
    // 找栈中最近的同名(允许 mis-nest:中间元素全部隐式闭合)
    var idx = -1;
    for (var k = this.stack.length - 1; k >= 0; k--) {
      if (this.stack[k].tag === tag) { idx = k; break; }
    }
    if (idx < 0) { this._warn("unmatched </" + tag + ">"); return; }
    while (this.stack.length - 1 > idx) {
      this._warn("implicit close <" + this.stack[this.stack.length - 1].tag + ">");
      this.stack.pop();
    }
    this.stack.pop();
  };

  Parser.prototype.next_script = function () { return this.scripts.length ? this.scripts.shift() : null; };

  function parse(html) {
    var p = new Parser();
    p.feed(html);
    p.finalize();
    return { root: p.root, doc: p.root, scripts: p.scripts.slice(), readyState: "complete", warnings: p.warnings };
  }
  function make() { return new Parser(); }

  return { parse: parse, make: make, VOID: VOID, IMPLICIT: IMPLICIT, RAW: RAW, decodeEntities: decodeEntities };
})();
