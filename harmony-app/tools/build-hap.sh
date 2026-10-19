#!/usr/bin/env bash
# 一键构建 + 编译门禁自证（产出 HAP 构建日志）。
#
# 为什么要"门禁自证"这一步：
#   hvigor 只编译**从入口可达**的文件。页面没被 import 的话，写得再错也不会报错，
#   `BUILD SUCCESSFUL` 是假绿。所以要往一个真实被引用的文件里注入一个未定义常量，
#   确认它**确实**报 ArkTS:ERROR 且 BUILD FAILED，再回退。没做这一步的"编译通过"不算证据。
#
# 退出码契约（t118 修正）：
#   本脚本是 `tools/freeze-check.sh` 的 `harmony_build` **硬项**，所以 rc 必须真实：
#     rc=0  ⇔ 自证阶段确实红了（BUILD FAILED + ArkTS:ERROR）、注入已回退、正式构建 BUILD
#              SUCCESSFUL 且 hvigor rc=0、本轮**新产出**了 HAP（mtime ≥ 构建起始时间戳）。
#     其余任何情况（自证没红 / 构建失败 / HAP 缺失 / HAP 是上一轮旧产物）一律 rc≠0。
#   历史缺陷（t109 复核发现）：旧版用 `set -uo pipefail` 却没有显式断言，加上流水线里的
#   `tee` 与 `|| true`、以及"HAP 不存在只 echo"，导致**脚本 rc 恒为 0**——两阶段日志都写
#   BUILD FAILED，脚本仍报 0，冻结闸门在 App 侧等于失效。
#   为什么这里仍然**不**加 `set -e`：自证阶段的"预期失败"本身就是成功路径，`-e` 会在
#   正确行为上直接终止脚本并把 rc 变成失败；失败语义必须由下面的**显式断言**给出。
#
# 用法：bash harmony-app/tools/build-hap.sh [输出日志路径]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="${1:-$REPO/deliverables/01-HAP构建日志.txt}"
PROBE_FILE="$REPO/entry/src/main/ets/pages/HomePage.ets"
PROBE_ANCHOR="const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;"
HAP_DIR="$REPO/entry/build/default/outputs/default"

mkdir -p "$(dirname "$OUT")"
# 日志路径先固化成绝对路径，再 cd 进工程目录：这样调用方传相对路径也按它的 cwd 解析（t118）
OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
# hvigorw 必须在工程目录里跑（否则它找不到 hvigor/hvigor-config.json5，日志头部取不到工具链版本）
cd "$REPO"

fail() {
  echo "[FATAL] $*" | tee -a "$OUT" >&2
  echo "构建门禁：**失败**（rc=1）" | tee -a "$OUT" >&2
  exit 1
}

# ── hvigor 配置物化（t79）───────────────────────────────────────────────────────
# hvigor-config.json5 里那两条 file: 依赖指向**本机工具链绝对路径**，写进公开仓库会带上
# 本机家目录（真实用户名）。仓库只存模板，这里在调 hvigorw 之前物化成 hvigor-config.json5。
# 为什么要这一步：hvigor 自己不做 ${VAR} 展开（已核对工具链 bin/hvigorw.js 无变量替换逻辑，
# file: 依赖原样交给 npm/pnpm install），所以模板里用 @TOOLCHAIN@ 占位、由本脚本替换。
HVIGOR_CFG="$REPO/hvigor/hvigor-config.json5"
HVIGOR_TPL="$REPO/hvigor/hvigor-config.template.json5"
if grep -q '@TOOLCHAIN@' "$HVIGOR_CFG" 2>/dev/null || [ ! -f "$HVIGOR_CFG" ]; then
  sed "s|@TOOLCHAIN@|$HOME/harmony-toolchain|g" "$HVIGOR_TPL" > "$HVIGOR_CFG" \
    && echo "[setup] 已由模板物化 hvigor-config.json5（工具链：$HOME/harmony-toolchain）"
fi

source "$HOME/harmony-toolchain/env.sh" >/dev/null 2>&1
# 工具链的 SDK 是扁平布局，hvigor 期望 sdk/<apiVersion>/<component>（见 ~/harmony-toolchain/BUILD-NOTES.md 坑 ③）
export OHOS_BASE_SDK_HOME="$HOME/harmony-toolchain/clt/command-line-tools/sdk"

restore_probe() {
  if [ -f "$PROBE_FILE.bak" ]; then
    mv -f "$PROBE_FILE.bak" "$PROBE_FILE"
  fi
}
trap restore_probe EXIT

{
  echo "HMProgram 应用端（harmony-app）HAP 构建日志"
  echo "生成时间（UTC+8）：$(TZ=Asia/Shanghai date '+%Y-%m-%d %H:%M:%S')"
  echo "工具链：$(hvigorw --version 2>/dev/null | tail -1)　Node：$(node -v)"
  echo "SDK：$(ls "$OHOS_BASE_SDK_HOME" 2>/dev/null | tr '\n' ' ')"
  echo
  echo "=============================================================="
  echo "[1/2] 编译门禁自证：注入未定义常量，必须 BUILD FAILED（hvigor --max-workers=2，见 AGENTS.md 资源上限）"
  echo "=============================================================="
} > "$OUT"

command -v hvigorw >/dev/null 2>&1 || fail "hvigorw 不在 PATH（工具链未就绪）——构建门禁无法自证"

# ── [1/2] 自证：必须红 ─────────────────────────────────────────────────────────
cp "$PROBE_FILE" "$PROBE_FILE.bak"
python3 - "$PROBE_FILE" "$PROBE_ANCHOR" <<'PY'
import io, sys
path, anchor = sys.argv[1], sys.argv[2]
s = io.open(path, encoding='utf-8').read()
assert anchor in s, '注入锚点没找到，脚本需要更新'
s = s.replace(anchor, anchor + "\nconst GATE_PROBE: number = NOT_DEFINED_CONST_FOR_GATE_PROBE;", 1)
io.open(path, 'w', encoding='utf-8').write(s)
PY
[ $? -eq 0 ] || fail "探针注入失败（锚点没找到？）"

cd "$REPO"
hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap 2>&1 | grep -E "ArkTS:ERROR|Cannot find name|BUILD (FAILED|SUCCESSFUL)" \
  | tee -a "$OUT"
probe_rc=${PIPESTATUS[0]}
restore_probe
restored_hits="$(grep -c 'NOT_DEFINED_CONST' "$PROBE_FILE" || true)"
red_build="$(grep -c 'BUILD FAILED' "$OUT" || true)"
red_arkts="$(grep -cE 'ArkTS:ERROR|Cannot find name' "$OUT" || true)"
{
  echo "自证断言：hvigor rc=$probe_rc（应≠0）；日志 BUILD FAILED=$red_build 处、ArkTS 错误=$red_arkts 处（都应≥1）；回退注入 grep 计数=$restored_hits（应为 0）"
} | tee -a "$OUT"

# 显式断言（t118）：自证阶段没红 = 门禁自证失效，必须让整脚本失败，而不是靠日志文字/`|| true` 吞掉
[ "$probe_rc" -ne 0 ] || fail "自证阶段没红：hvigor rc=0（注入未定义常量后本应 BUILD FAILED）"
[ "$red_build" -ge 1 ] || fail "自证阶段日志里没有 BUILD FAILED"
[ "$red_arkts" -ge 1 ] || fail "自证阶段日志里没有 ArkTS:ERROR/Cannot find name"
[ "$restored_hits" = "0" ] || fail "回退注入失败：$PROBE_FILE 仍含 NOT_DEFINED_CONST（工作树被污染）"

# ── [2/2] 正式构建 ────────────────────────────────────────────────────────────
# 产物新鲜度（t118）：先清掉上一轮 HAP，再记构建起始时间戳，最后要求列出的 HAP
# mtime ≥ 起始时间戳。这样"列的是上一轮旧产物"在结构上不可能发生，而不是靠肉眼判断。
PREV_HAP="$(ls "$HAP_DIR"/*.hap 2>/dev/null || true)"
BUILD_START="$(date +%s)"
{
  echo
  echo "=============================================================="
  echo "[2/2] 正式构建：hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap"
  echo "=============================================================="
  if [ -n "$PREV_HAP" ]; then
    echo "清理上一轮产物（保证下面列出的必是本轮产出）：$PREV_HAP"
  else
    echo "构建前 $HAP_DIR 下无 HAP 产物"
  fi
  echo "构建起始时间戳（epoch，新鲜度基准）：$BUILD_START"
} >> "$OUT"
rm -f "$HAP_DIR"/*.hap

hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap 2>&1 | tee -a "$OUT" | grep -E "BUILD|WARN|ERROR"
build_rc=${PIPESTATUS[0]}
echo "正式构建 hvigor rc=$build_rc（应为 0）" >> "$OUT"

[ "$build_rc" -eq 0 ] || fail "正式构建 rc=$build_rc ≠ 0"
grep -q 'BUILD SUCCESSFUL' "$OUT" || fail "正式构建日志里没有 BUILD SUCCESSFUL"

# ── 产物：存在 + 新鲜（t118）；列出时不含属主用户名（t79 遗留修复）────────────────
HAP="$(ls "$HAP_DIR"/*.hap 2>/dev/null | head -1)"
[ -n "$HAP" ] || fail "未找到 HAP 产物（$HAP_DIR/*.hap）——本轮构建没有产出，不许只 echo 了事"
hap_mtime="$(stat -c %Y "$HAP")"
[ "$hap_mtime" -ge "$BUILD_START" ] || fail "HAP 不是本轮产物：mtime(epoch)=$hap_mtime < 构建起始=$BUILD_START"

{
  echo
  echo "=============================================================="
  echo "产物"
  echo "=============================================================="
  # stat 而非 `ls -l`：`ls -l` 会打印属主/属组（本机真实用户名），写进交付日志即泄露（t79 遗留）
  stat -c '%s bytes  mtime=%y  %n' "$HAP"
  echo "新鲜度判据：HAP mtime(epoch)=$hap_mtime ≥ 构建起始 epoch=$BUILD_START ⇒ 本轮产出"
  echo
  echo "说明：本机无 hdc 设备、CLT 内无模拟器、/dev/kvm 不可写，"
  echo "      因此只做到「编译产 HAP」；安装/上架/真机运行标注为「待用户实测」。"
} >> "$OUT"

echo
echo "日志已写入：$OUT"
tail -20 "$OUT"
echo "构建门禁：**通过**（rc=0；自证红、注入已回退、正式构建成功、HAP 为本轮产出）"
exit 0
