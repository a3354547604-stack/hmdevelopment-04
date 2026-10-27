#!/usr/bin/env bash
# check-citations.sh — 「文档优先协议」的确定性门禁（见 docs/协议/文档优先协议.md）
#
# 为什么是脚本而不是提示词：提示词/规则文件都是 non-deterministic，
# 只有 hook / CI / pre-commit 这类外部校验能保证「没来源就不许写成事实」。
#
# 用法：
#   bash tools/check-citations.sh <markdown 文件> [...]
#   bash tools/check-citations.sh --selftest
#
# 检查三条（命中任一 = 违规，exit 1）：
#   R1 带【实测】/【原理图】/【官方文档】标签的行，必须同时有指针：
#      路径:行号 / http(s) URL / 反引号里的命令。
#   R2 模糊话术（根据官方文档|我记得|通常|一般|大概|应该是）必须同行带
#      [待验证] 或 [仅线索]，否则视为「拿记忆当事实」。
#   R3 [待验证] / [仅线索] 段落里出现祈使句（请把|请将|请接|请填）→ 违规，
#      应改成条件句（「若…则…」）。
set -uo pipefail

fail=0
viol() { printf '%s:%s: %s\n' "$1" "$2" "$3"; fail=1; }

check_file() {
  local f="$1" out
  [ -f "$f" ] || { printf '%s: 文件不存在\n' "$f"; fail=1; return; }
  # 跳过围栏代码块，以及「规则定义/反模式」这类自指行
  out="$(awk -v F="$f" '
    /^```/ { fence = !fence; next }
    fence { next }
    /模糊话术|反模式|禁给|禁止|不许|只算线索|禁按/ { next }
    /【实测】|【原理图】|【官方文档】/ {
      if ($0 !~ /[A-Za-z0-9_.\/-]+:[0-9]+/ && $0 !~ /https?:\/\// && $0 !~ /`[^`]+`/) {
        printf "%s:%d: R1 有事实标签但没有指针（路径:行号 / URL / 反引号命令）\n", F, NR
      }
      next
    }
    /根据官方文档|我记得|通常|一般|大概|应该是/ {
      if ($0 !~ /\[待验证\]/ && $0 !~ /\[仅线索\]/) {
        printf "%s:%d: R2 模糊话术缺 [待验证]/[仅线索]（禁拿记忆当事实）\n", F, NR
      }
      next
    }
    /\[待验证\]|\[仅线索\]/ { flag=1; next }
    /^#/ { flag=0 }
    flag==1 && /请把|请将|请接|请填/ {
      printf "%s:%d: R3 [待验证] 段里出现祈使句，应改条件句（若…则…）\n", F, NR
    }' "$f")"
  if [ -n "$out" ]; then
    printf '%s\n' "$out"
    fail=1
  fi
}

selftest() {
  local d; d="$(mktemp -d)"
  cat >"$d/bad.md" <<'EOF'
# 坏样本
引脚是 PB6，根据官方文档就是这么写的。
【实测】用户通断测过。
【官方文档】IoTDA 的接入地址表格，见 https://example.com/doc#sec-2 。
[待验证]
请把线接到第 19 脚。
EOF
  cat >"$d/good.md" <<'EOF'
# 好样本
引脚 TX=GPIO0_PB6（【实测】用户万用表通断 2026-10-27，见 `rk2206-gateway/src/gw_config.h:25`）。
【官方文档】接入地址见 https://example.com/doc#sec-2 。
模糊说法已标：这大概是因为驱动未填频率 [待验证]。
[待验证]
若 E53 座第 19 脚与 B6 导通，则 TX=PB6；否则按 B7 处理。
EOF
  local out; out="$(check_file "$d/bad.md")"
  local bad_ok=1
  echo "$out" | grep -q 'R1' || bad_ok=0
  echo "$out" | grep -q 'R2' || bad_ok=0
  echo "$out" | grep -q 'R3' || bad_ok=0
  local good_out; good_out="$(check_file "$d/good.md")"
  local good_ok=1
  [ -z "$good_out" ] || good_ok=0
  if [ "$bad_ok" = 1 ] && [ "$good_ok" = 1 ]; then
    echo "[selftest] PASS（坏样本 3 条全被抓、好样本 0 违规）"
    rm -rf "$d"; return 0
  fi
  echo "[selftest] FAIL（bad_ok=$bad_ok good_ok=$good_ok）"
  echo "--- bad.md 命中 ---"; echo "$out"
  echo "--- good.md 命中（应为空） ---"; echo "$good_out"
  rm -rf "$d"; return 1
}

case "${1:-}" in
  --selftest) selftest; exit $? ;;
  "" ) echo "用法：bash tools/check-citations.sh <markdown...> | --selftest"; exit 2 ;;
esac

for f in "$@"; do check_file "$f"; done

if [ "$fail" = 0 ]; then
  echo "[check-citations] PASS：$# 个文件无违规"
  exit 0
fi
echo "[check-citations] FAIL：见上方 file:line"
exit 1
