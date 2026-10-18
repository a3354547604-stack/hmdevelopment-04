#!/usr/bin/env bash
# =============================================================================
# tools/test/run_e2e_host.sh — t39 主机侧端到端串测的一键入口
#
# 为什么要包一层 build-lock：本脚本会起容器编译两份 C harness（汇聚 + 网关）
# 并起 server 的 db/api 两个容器，属于「重活」，按 AGENTS.md 必须串行占槽。
# 用法：
#   bash tools/test/run_e2e_host.sh              # 全链（内部已串行，不必再套锁）
#   bash tools/test/run_e2e_host.sh --dry        # 只做不需要构建/容器的部分
#   HM_BUILD_LOCK=/tmp/hm-build-2.lock bash tools/test/run_e2e_host.sh   # 用 2 号槽
# 退出码：0 全链通过 / 1 有段失败（报告里逐条给期望与实测）/ 2 环境不可用
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOCK="$ROOT/tools/build-lock.sh"
ARGS=("$@")

if [ "${1:-}" = "--dry" ]; then
    # --dry 不编译、不起容器，按任务卡属于「无需重活的验证」，不占构建槽
    exec python3 "$ROOT/tools/test/e2e/run_e2e.py" "${ARGS[@]}"
fi

if [ ! -x "$LOCK" ]; then
    echo "[e2e] 找不到 $LOCK，直接跑（会自己管容器）" >&2
    exec python3 "$ROOT/tools/test/e2e/run_e2e.py" "${ARGS[@]}"
fi

exec "$LOCK" python3 "$ROOT/tools/test/e2e/run_e2e.py" "${ARGS[@]}"
