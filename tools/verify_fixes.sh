#!/bin/sh
# 逐条回退修复,确认对应测试能变红。备份 → 破坏 → 构建 → 跑指定测试 → 恢复。
# 判定标准是「测试真的红了」,不是「输出里出现了什么」。
set -u
cd "$(dirname "$0")/.." || exit 1

PASS_J=/tmp/opencode/rv_js_parser.log
PASS_I=/tmp/opencode/rv_integ.log
bk_p=src/js/parser.js
bk_c=src/core/js_engine.c
cp $bk_p /tmp/opencode/rv_parser.bak
cp $bk_c /tmp/opencode/rv_engine.bak

restore() { cp /tmp/opencode/rv_parser.bak $bk_p; cp /tmp/opencode/rv_engine.bak $bk_c; }

sabotage() {   # sabotage <名字> <file> <python-replace-script> <测试正则>
  name=$1; file=$2; py=$3; re=$4
  restore
  python3 -c "$py" || { echo "== $name: 破坏脚本没匹配上,跳过"; return; }
  if ! cmake --build build -j"$(nproc)" > /tmp/opencode/rv_build.log 2>&1; then
    echo "== $name: 构建失败"; grep -m3 error: /tmp/opencode/rv_build.log; restore; return
  fi
  out=$(ctest --test-dir build -R "$re" 2>&1)
  if echo "$out" | grep -q "100% tests passed"; then
    echo "== $name: 仍然全绿  <-- 这条测试没钉住修复"
  else
    echo "== $name: 变红 ✓  ($(echo "$out" | grep -oE '[0-9]+ tests? failed out of [0-9]+' | head -1))"
  fi
  restore
}

P='src/js/parser.js'
C='src/core/js_engine.c'

sabotage "1 数值实体 fromCodePoint→fromCharCode" $P \
"s=open('$P').read(); s=s.replace('String.fromCodePoint(cp)','String.fromCharCode(cp)'); open('$P','w').write(s)" \
'js_parser|BrowserApi.AstralNumeric'

sabotage "2 属性名不转小写" $P \
"s=open('$P').read(); s=s.replace('var an = am[0].toLowerCase();','var an = am[0];'); open('$P','w').write(s)" \
'js_parser|BrowserApi.UppercaseAttributes'

sabotage "3 重复属性改为后者覆盖" $P \
"s=open('$P').read(); s=s.replace('this.curAttrs[an] = \"\";   // 先置空','this.curAttrs[an] = \"\"; this.attrDrop = false; // SABOTAGE 保留后者'); open('$P','w').write(s)" \
'js_parser'

sabotage "4 数据块不再过滤" $C \
"s=open('$C').read(); s=s.replace(\"if (!__tb_is_js_script(s)) continue;   /* 数据块:既不执行也不抓取 */\",''); open('$C','w').write(s)" \
'js_parser|BrowserApi.DataBlock'

sabotage "5 style 又塞回 scripts" $P \
"s=open('$P').read(); s=s.replace('if (this.rawTag === \"script\")\n          this.scripts.push','if (true)\n          this.scripts.push'); open('$P','w').write(s)" \
'js_parser|BrowserApi.StyleContent|BrowserApi.DataBlock'

sabotage "6 raw 跨切:不留尾部" $P \
"s=open('$P').read(); s=s.replace('var keep = this.finalized ? 0 : this.rawTag.length + 3;','var keep = 0;'); open('$P','w').write(s)" \
'js_parser'

sabotage "7 raw 闭合标签不要求分隔符" $P \
"s=open('$P').read(); s=s.replace('if (after === \"\" || after === \">\" || after === \"/\" || /[\\\\t\\\\n\\\\f\\\\r ]/.test(after)) return p;','if (p >= 0) return p;'); open('$P','w').write(s)" \
'js_parser'

sabotage "8 注释跨切:不留尾部" $P \
"s=open('$P').read(); s=s.replace('var takeC = L - this.i - (this.finalized ? 0 : 2);','var takeC = L - this.i;'); open('$P','w').write(s)" \
'js_parser'

# 9 原为「PI 跨切留尾部」。已删除该逻辑:PI 内容整段被丢弃,留不留尾部都不可观察
#   (见 parser.js pi 分支的注释),所以这里刻意没有这一条 —— 留着只会假装有测试覆盖。

sabotage "10 '<' 在 chunk 末尾照常推进游标" $P \
"s=open('$P').read(); s=s.replace('if (lt + 1 >= L && !this.finalized) { this.i = lt; return; }',''); open('$P','w').write(s)" \
'js_parser'

sabotage "11 未闭合脚本在 EOF 被丢弃" $P \
"s=open('$P').read(); s=s.replace('if (this.state === \"raw\" && this.rawTag === \"script\")','if (false)'); open('$P','w').write(s)" \
'js_parser'

restore
cmake --build build -j"$(nproc)" > /tmp/opencode/rv_build.log 2>&1 && echo "已恢复并重建"
