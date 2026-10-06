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
      if (e === "nbsp") return "\u00A0";
      var cp = (e[1] === "x") ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
      /* 数值引用解释的是**码点**,不是 UTF-16 code unit。fromCharCode 会把
         0x1F600 截断成 0xF600(私有区码位),于是 "&#x1F600;" 渲染成一个无意义
         的字形而不是 U+1F600。生成式内容、模板与文档里这类引用很常见。 */
      if (!(cp >= 0) || cp === 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return "\uFFFD";   // NUL / 代理区 / 越界:HTML 规范要求映射到 U+FFFD
      return String.fromCodePoint(cp);
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
    this.curScriptType = null;  // 同上:type —— 决定它是不是真的 JS(见 js_engine.c 加载器)
    this.attrDrop = false;      // 当前属性名是重复项:值要丢弃(HTML 规范)
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
    /* 文档在 raw 元素中途结束(下载被截断,或 "</scriptx>" 这类不构成闭合标签的
       序列):raw 文本到此为止,按 HTML 规范这段内容仍是一段完整脚本。原先直接
       丢掉 —— 截断的下载里脚本就此消失。 */
    if (this.state === "raw" && this.rawTag === "script")
      this.scripts.push({ src: this.curScriptSrc, text: this.rawText, type: this.curScriptType });
    this.rawTag = null; this.rawText = ""; this.state = "text";
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
        /* 缓冲区恰好断在 '<' 之后:无法判断后面是标签还是字面 '<'。
           当字面处理是有害的 —— 实测把 "<title>" 切成 "<" + "title>...",
           整个元素连同其后正文都变成纯文本,并留下一条 "unmatched </title>"。
           **游标必须留在 lt 上**:先前 i = lt + 1 之后再 return,下个 chunk 就
           从 lt+1 接着扫,那个 '<' 被永久跳过(比不修更隐蔽)。 */
        if (lt + 1 >= L && !this.finalized) { this.i = lt; return; }
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
        var an = am[0].toLowerCase();
        // 跨 chunk:属性名到缓冲末尾,不消费,等续接
        if (this.i + an.length >= L && !this.finalized) return;
        this.i += an.length;
        /* 属性名一律小写(HTML 规范在 tokenizer 里就做了)。dom.js 取值一律用
           小写键(n.attrs.href / .class / .id),所以 "<a HREF=/x>" 原本 href 为空、
           链接是死的,"<div Class=x>" 则让 querySelector('.x') 找不到元素。
           标签名早已小写,属性名此前漏了。 */
        /* 重复属性:规范丢弃后者(保留第一个)。此前是后者覆盖前者。 */
        this.attrDrop = Object.prototype.hasOwnProperty.call(this.curAttrs, an);
        if (!this.attrDrop) this.curAttrs[an] = "";   // 先置空(boolean 默认)
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
        if (ce < 0) {
          /* "-->" 可能横跨 chunk 边界:末尾 2 字节留着等续接。原先直接 i = L,
             于是切在 "--" 与 ">" 之间时注释再也不会闭合,余下整篇文档(含后面
             所有的元素)被吞进注释,页面凭空少一截。 */
          var takeC = L - this.i - (this.finalized ? 0 : 2);
          if (takeC > 0) this.i += takeC;
          return;
        }
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
        /* 这里**不**适用 comment/raw 那套「留尾部」的做法,而且留了也没用:
           PI 状态的内容被整段丢弃(既不入 DOM 也不记进 warnings),所以
           「少扫了末尾一个字节」不可能改变任何可观察输出 —— 两种写法最终
           都停在同一个 '>' 之后。实测把这里的留尾部去掉,chunk 不变性测试
           全绿。故保持简单:扫尽即可。将来若改成保留 PI 文本,必须同时补上
           留尾部,否则跨切片的 "?>" 会失配。 */
        if (endAt < 0) { this.i = L; return; }
        this.i = endAt + (s[endAt] === "?" ? 2 : 1); this.state = "text";
      }
      else if (this.state === "raw") {
        var re = this._findRawEnd();
        if (re < 0) {
          /* needle 可能横跨 chunk 边界,末尾 needle.length-1 个字节先留着,下次
             feed 再匹配。原先直接吞掉剩余全部输入,于是跨切片的 "</script>"
             永远失配 —— 实测那样整个 <script> 连同其后**所有**正文都被当成
             脚本内容,元素直接丢失(喂 25 字节的 chunk 恰好不踩到,喂错一切点就中招)。 */
          var keep = this.finalized ? 0 : this.rawTag.length + 3;   // "</"+tag 的长度 - 1
          var take = L - this.i - keep;
          if (take > 0) { this.rawText += s.slice(this.i, this.i + take); this.i += take; }
          return;
        }
        /* '>' 未到:先把 re 之前的脚本文本收进 rawText,再把游标停在 re 等续接。
         顺序不能反 —— 先挪游标的话,这段文本就永远不会被计入,脚本正文变空
         (实测 "</script|" 切分时脚本文本被吞成空串)。下次 _findRawEnd 从 re
         起重新定位,slice(re, re) 为空,不会重复计入。 */
        var rgt = s.indexOf(">", re + this.rawTag.length + 2);
        if (rgt < 0) {
          this.rawText += s.slice(this.i, re);
          this.i = re;
          return;
        }
        this.rawText += s.slice(this.i, re);
        this.i = rgt + 1;
        /* 只有 <script> 进 scripts。<style> 的内容是 CSS 不是 JS —— 此前也塞进
           这里,而加载器对每一条都 eval,于是每个带 <style> 的页面都会拿到一条
           "script error: ... is not defined" 之类的假报错。没有 CSS 引擎,
           样式文本目前无人消费。 */
        if (this.rawTag === "script")
          this.scripts.push({ src: this.curScriptSrc, text: this.rawText, type: this.curScriptType });
        this.rawTag = null; this.rawText = ""; this.curScriptSrc = null; this.curScriptType = null;
        this.state = "text";
      }
    }
  };

  // _findRawEnd:定位 raw 元素(script/style)闭合标签的位置,找不到返回 -1。
  //
  // 两处规范细节,缺一不可:
  // 1) 闭合标签名后必须紧跟分隔符(空白 / '/' / '>')。否则 "</scriptfoo>" 里的
  //    "script" 只是普通脚本文本 —— 浏览器不会在那里结束 raw 文本。此前没有这个
  //    判断,"<script>a</scriptfoo>b</script>" 会把 a 当成全部脚本内容、b 之后的
  //    当成 HTML。
  // 2) 名字后缓冲区就断了,无法判定分隔符 —— 返回该位置让调用方因找不到 '>'
  //    而等待续接;下个 chunk 若接的是字母,_findRawEnd 会重新判定并跳过它。
  Parser.prototype._findRawEnd = function () {
    var needle = "</" + this.rawTag;
    for (var from = this.i; ; ) {
      var p = this.buf.indexOf(needle, from);
      if (p < 0) return -1;
      var after = this.buf.charAt(p + needle.length);
      if (after === "" || after === ">" || after === "/" || /[\t\n\f\r ]/.test(after)) return p;
      from = p + 1;
    }
  };

  Parser.prototype._setLastAttr = function (v) {
    // 写回最后一个属性名(记录在 this.lastAttrName)
    if (this.lastAttrName == null) { this._warn("attr value without name"); return; }
    if (this.attrDrop) { this.attrDrop = false; return; }   // 重复属性:值一并丢弃
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
      /* 记下 type:script 元素不全是脚本。application/ld+json、text/template、
         importmap 之类的数据块内容是数据不是 JS,加载器据此跳过执行与抓取。
         language 是 type 的过时别名,一并记(规范:type 缺失时看 language)。 */
      var ty = this.curAttrs.type != null ? this.curAttrs.type
                : (this.curAttrs.language != null ? this.curAttrs.language : null);
      if (selfClose) {
        // <script src=x /> 自闭合:无内容记录
        if (tag === "script") this.scripts.push({ src: src, text: "", type: ty });
        this.state = "text";
      } else {
        this.curScriptSrc = src;
        this.curScriptType = ty;
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
