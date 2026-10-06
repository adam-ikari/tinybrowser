#!/usr/bin/env python3
"""逐条回退修复,确认对应测试能变红。

判定标准是「测试真的红了」。三次踩过的坑,都促成了这里的写法:
  1) 用 str.replace 改坏代码时,**没匹配上也会 exit 0** —— 「破坏成功」与
     「什么都没改」长得一模一样,于是报告成「测试没钉住修复」,把人引向
     错误方向。所以每条都断言「改动确实落进文件」,否则单独报错。
  2) 早先一整批条目传错了文件变量(全指向 parser.js),12~17 条集体
     「仍然全绿」,看起来像测试失效。
  3) shell 里嵌 python -c 传多行字符串 + 引号,必然被 quoting 咬掉。

固件用 python 做替换(精确字符串 + 断言),shell 只负责 cmake/ctest。
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (名字, 文件, 原文, 改后, 测试正则)
CASES = [
    # ---- 第一批:坏 UTF-8 / 属性 / <script type> / chunk 边界 ----
    ("1 数值实体 fromCodePoint→fromCharCode",
     "src/js/parser.js",
     "return String.fromCodePoint(cp);",
     "return String.fromCharCode(cp); /*SABOTAGE*/",
     "js_parser|BrowserApi.AstralNumeric"),
    ("2 属性名不转小写",
     "src/js/parser.js",
     "var an = am[0].toLowerCase();",
     "var an = am[0]; /*SABOTAGE*/",
     "js_parser|BrowserApi.UppercaseAttributes"),
    ("3 重复属性改为后者覆盖",
     "src/js/parser.js",
     'if (!this.attrDrop) this.curAttrs[an] = "";',
     'this.curAttrs[an] = ""; this.attrDrop = false; /*SABOTAGE*/',
     "js_parser"),
    ("4 数据块不再过滤",
     "src/core/js_engine.c",
     "if (!__tb_is_js_script(s)) continue;",
     "/*SABOTAGE*/",
     "BrowserApi.DataBlock"),
    ("5 style 又塞回 scripts",
     "src/js/parser.js",
     'if (this.rawTag === "script")',
     'if (true /*SABOTAGE*/)',
     "js_parser|BrowserApi.StyleContent|BrowserApi.DataBlock"),
    ("6 raw 跨切:不留尾部",
     "src/js/parser.js",
     "var keep = this.finalized ? 0 : this.rawTag.length + 3;",
     "var keep = 0; /*SABOTAGE*/",
     "js_parser"),
    ("7 raw 闭合标签不要求分隔符",
     "src/js/parser.js",
     'if (after === "" || after === ">" || after === "/" || /[\\t\\n\\f\\r ]/.test(after)) return p;',
     "if (p >= 0) return p; /*SABOTAGE*/",
     "js_parser"),
    ("8 注释跨切:不留尾部",
     "src/js/parser.js",
     "var takeC = L - this.i - (this.finalized ? 0 : 2);",
     "var takeC = L - this.i; /*SABOTAGE*/",
     "js_parser"),
    ("10 '<' 在 chunk 末尾照常推进游标",
     "src/js/parser.js",
     "if (lt + 1 >= L && !this.finalized) { this.i = lt; return; }",
     "/*SABOTAGE*/",
     "js_parser"),
    ("11 未闭合脚本在 EOF 被丢弃",
     "src/js/parser.js",
     'if (this.state === "raw" && this.rawTag === "script")',
     "if (false) /*SABOTAGE*/",
     "js_parser"),

    # ---- 第二批:textarea / 控件取值 / 指纹(Google 搜索框那批) ----
    ("12 render.js 不把 textarea 记成 elem",
     "src/js/render.js",
     '      } else if (tag === "textarea") {',
     '      } else if (tag === "textarea" && false) { /*SABOTAGE*/',
     "js_render|BrowserApi.TextareaSearchBox"),
    ("13 视图侧读 attrs.value(与提交侧各算一套)",
     "src/js/render.js",
     "value: _tb_control_value(n),",
     'value: n.attrs.value || "", /*SABOTAGE*/',
     "js_render|BrowserApi.SelectDefaults"),
    ("14 _tb_control_value 不认 textarea",
     "src/js/dom.js",
     '  if (n.tag === "textarea") return n.textContent;',
     "  /*SABOTAGE*/",
     "js_render"),
    # 锚点必须唯一:_tb_elem_info 里有一行**完全相同**的
    # `  if (n.tag === "select") {`,只按单行替换会打到那处(定义更靠前),
    # 于是「破坏」落在了别处、测试自然全绿。故连同上一行一起匹配。
    ("15 _tb_control_value 不认 select(默认值回落空串)",
     "src/js/dom.js",
     '  if (n.tag === "textarea") return n.textContent;\n  if (n.tag === "select") {',
     '  if (n.tag === "textarea") return n.textContent;\n  if (false) { /*SABOTAGE*/',
     "js_render|BrowserApi.SelectDefaults"),
    ("16 _tb_form_pairs 不收 textarea",
     "src/js/dom.js",
     '(n.tag === "input" || n.tag === "select" || n.tag === "textarea")',
     '(n.tag === "input" || n.tag === "select") /*SABOTAGE*/',
     "js_render|BrowserApi.TextareaSearchBox"),
    ("16b _tb_elem_info 的 select options 分支",
     "src/js/dom.js",
     '  if (n.tag === "select") {\n    o.options = [];',
     '  if (false) { /*SABOTAGE*/\n    o.options = [];',
     "js_render"),
    ("17 指纹不含已填控件的值",
     "src/core/browser.c",
     "return n + ':' + t + ':' + fv.length + ':' + fv + ':' + document.title;",
     "return n + ':' + t + ':' + document.title; /*SABOTAGE*/",
     "BrowserApi.FilledTextarea"),
    ("18 tb_fill 不认 textarea",
     "src/core/browser.c",
     'if (strcmp(tag, "input") != 0 && strcmp(tag, "textarea") != 0) {',
     'if (strcmp(tag, "input") != 0) { /*SABOTAGE*/',
     "BrowserApi.TextareaSearchBox"),
    ("19 http_server 不做去 query 的兜底",
     "tests/harness/http_server.cc",
     "std::string bare = path.substr(0, path.find('?'));",
     "std::string bare = path; /*SABOTAGE*/",
     "BrowserApi.TextareaSearchBox"),
]


def sh(cmd, **kw):
    return subprocess.run(cmd, shell=True, cwd=ROOT, capture_output=True, text=True, **kw)


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    tmp = tempfile.mkdtemp(prefix="verifyfixes-")
    backups = {}
    for _, rel, _, _, _ in CASES:
        if rel not in backups:
            dst = os.path.join(tmp, rel.replace("/", "_"))
            shutil.copy2(os.path.join(ROOT, rel), dst)
            backups[rel] = dst

    def restore():
        for rel, dst in backups.items():
            path = os.path.join(ROOT, rel)
            # ⚠️ 必须 copyfile + 显式 utime,**不能用 copy2**:copy2 保留 mtime,
            # 恢复后源文件的时间戳与破坏前一样旧,make 认为没改动 → 不重编 →
            # 上一轮破坏掉的代码继续留在二进制里。实测栽过:源码 grep 不到
            # SABOTAGE、测试却一直红,根因就是库里还是坏的。
            shutil.copyfile(dst, path)
            os.utime(path, None)

    stale = 0
    try:
        for name, rel, old, new, regex in CASES:
            if only and only not in name:
                continue
            restore()
            path = os.path.join(ROOT, rel)
            src = open(path, encoding="utf-8").read()
            if old not in src:
                print(f"== {name}: 模式未命中({rel})  <-- 脚本过期,不是测试没钉住")
                stale += 1
                continue
            if old == new:
                print(f"== {name}: 破坏前后相同  <-- 脚本 bug")
                stale += 1
                continue
            open(path, "w", encoding="utf-8").write(src.replace(old, new, 1))
            if "SABOTAGE" not in open(path, encoding="utf-8").read():
                print(f"== {name}: 改动没落盘  <-- 脚本 bug")
                stale += 1
                continue

            b = sh("cmake --build build -j$(nproc)")
            if b.returncode != 0:
                errs = [l for l in b.stderr.splitlines() + b.stdout.splitlines()
                        if " error" in l][:3]
                print(f"== {name}: 构建失败 {errs}")
                continue
            r = sh(f'ctest --test-dir build -R "{regex}"')
            if "100% tests passed" in r.stdout:
                print(f"== {name}: 仍然全绿  <-- 这条测试没钉住修复")
            else:
                n = ""
                for line in r.stdout.splitlines():
                    if "tests failed out of" in line:
                        n = line.strip().splitlines()[-1] if line.strip() else n
                m = [l for l in r.stdout.splitlines() if "tests failed out of" in l]
                print(f"== {name}: 变红 OK  ({m[-1].strip() if m else '有失败'})")
    finally:
        restore()
        b = sh("cmake --build build -j$(nproc)")
        print()
        print("已恢复并重建" if b.returncode == 0 else "已恢复,但重建失败!")
        shutil.rmtree(tmp, ignore_errors=True)
    return 1 if stale else 0


sys.exit(main())
