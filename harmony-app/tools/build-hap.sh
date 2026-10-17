#!/usr/bin/env bash
# 一键构建 + 编译门禁自证（产出 HAP 构建日志）。
#
# 为什么要"门禁自证"这一步：
#   hvigor 只编译**从入口可达**的文件。页面没被 import 的话，写得再错也不会报错，
#   `BUILD SUCCESSFUL` 是假绿。所以要往一个真实被引用的文件里注入一个未定义常量，
#   确认它**确实**报 ArkTS:ERROR 且 BUILD FAILED，再回退。没做这一步的"编译通过"不算证据。
#
# 用法：bash harmony-app/tools/build-hap.sh [输出日志路径]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
OUT="${1:-$REPO/deliverables/01-HAP构建日志.txt}"
PROBE_FILE="$REPO/entry/src/main/ets/pages/HomePage.ets"
PROBE_ANCHOR="const POLL_INTERVAL_MS: number = 10 * LiteralConstants.MS_PER_SECOND;"

source "$HOME/harmony-toolchain/env.sh" >/dev/null 2>&1
# 工具链的 SDK 是扁平布局，hvigor 期望 sdk/<apiVersion>/<component>（见 ~/harmony-toolchain/BUILD-NOTES.md 坑 ③）
export OHOS_BASE_SDK_HOME="$HOME/harmony-toolchain/clt/command-line-tools/sdk"

restore_probe() {
  if [ -f "$PROBE_FILE.bak" ]; then
    mv -f "$PROBE_FILE.bak" "$PROBE_FILE"
  fi
}
trap restore_probe EXIT

mkdir -p "$(dirname "$OUT")"
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

cp "$PROBE_FILE" "$PROBE_FILE.bak"
python3 - "$PROBE_FILE" "$PROBE_ANCHOR" <<'PY'
import io, sys
path, anchor = sys.argv[1], sys.argv[2]
s = io.open(path, encoding='utf-8').read()
assert anchor in s, '注入锚点没找到，脚本需要更新'
s = s.replace(anchor, anchor + "\nconst GATE_PROBE: number = NOT_DEFINED_CONST_FOR_GATE_PROBE;", 1)
io.open(path, 'w', encoding='utf-8').write(s)
PY

cd "$REPO"
hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap 2>&1 | grep -E "ArkTS:ERROR|Cannot find name|BUILD (FAILED|SUCCESSFUL)" \
  | tee -a "$OUT"
restore_probe
echo "回退注入：grep 计数 = $(grep -c 'NOT_DEFINED_CONST' "$PROBE_FILE" || true)（应为 0）" | tee -a "$OUT"

{
  echo
  echo "=============================================================="
  echo "[2/2] 正式构建：hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap"
  echo "=============================================================="
} >> "$OUT"
hvigorw --no-daemon --no-analyze --max-workers=2 assembleHap 2>&1 | tee -a "$OUT" | grep -E "BUILD|WARN|ERROR" || true

{
  echo
  echo "=============================================================="
  echo "产物"
  echo "=============================================================="
  ls -l entry/build/default/outputs/default/*.hap 2>/dev/null || echo "未找到 hap 产物"
  echo
  echo "说明：本机无 hdc 设备、CLT 内无模拟器、/dev/kvm 不可写，"
  echo "      因此只做到「编译产 HAP」；安装/上架/真机运行标注为「待用户实测」。"
} >> "$OUT"

echo
echo "日志已写入：$OUT"
tail -14 "$OUT"
