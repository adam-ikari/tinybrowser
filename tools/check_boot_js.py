"""从 js_engine.c 里抽出 TB_BOOT_SRC + 内置 JS,交给 node --check 做语法门禁。

动机:boot 脚本是一大串 C 字符串字面量,C 相邻字面量拼接时**不换行**。于是
字面量里的 `//` 行注释会把**后面的字面量整段吃掉** —— C 编译器完全看不出来,
boot 脚本却少了一行,表现为 qz_create 直接失败(abi 版本照常打印,看不出原因)。
这个脚本就是那扇门禁。
"""
import io, re, subprocess, sys, glob, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def boot_js(path):
    lines = io.open(path, encoding="utf-8").read().split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith("static const char *TB_BOOT_SRC"))
    end = next(i for i in range(start, len(lines)) if lines[i].rstrip().endswith('";'))
    body = "\n".join(lines[start:end + 1])
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    def unesc(t):
        return (t.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"')
                 .replace("\\\\", "\\").replace("\\'", "'"))
    return "".join(unesc(t) for t in lits)


def main():
    src = os.path.join(ROOT, "src", "core", "js_engine.c")
    out = "/tmp/opencode/boot_check.js"
    io.open(out, "w", encoding="utf-8").write(boot_js(src))
    bad = 0
    for f in [out] + sorted(glob.glob(os.path.join(ROOT, "src", "js", "*.js"))):
        r = subprocess.run(["node", "--check", f], capture_output=True, text=True)
        name = os.path.relpath(f, ROOT) if not f.startswith("/tmp") else "boot(拼接后)"
        if r.returncode:
            bad = 1
            print("语法错误:", name)
            print(r.stderr[-400:])
        else:
            print("OK:", name)
    # 同一文件里若字面量内部仍有 //,再警告一次(那是本脚本存在的理由)
    lines = io.open(src, encoding="utf-8").read().split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith("static const char *TB_BOOT_SRC"))
    end = next(i for i in range(start, len(lines)) if lines[i].rstrip().endswith('";'))
    for i in range(start, end + 1):
        m = re.match(r'\s*"(?:[^"\\]|\\.)*//', lines[i])
        if m:
            print("字面量内的 // 行注释(会吃掉下一段字面量):", i + 1, lines[i].strip()[:70])
            bad = 1
    return bad


sys.exit(main())
