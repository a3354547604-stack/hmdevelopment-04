#!/usr/bin/env bash
# =============================================================================
# tools/freeze-check.sh — 冻结前一键总验收（t101）
#
# 一条命令按固定顺序跑完全部验收，逐项打印 rc 与耗时，最后给一张表 + 一份 json。
#
# 用法：
#   bash tools/freeze-check.sh --quick      # 日常自检：不构建、不起容器（秒级~分钟级）
#   bash tools/freeze-check.sh --full       # 冻结前：含构建与容器（约 5~10 分钟）
#   bash tools/freeze-check.sh --list       # 只打印计划（项/命令/模式），不执行
#   bash tools/freeze-check.sh --quick --only suite,e2e     # 只跑指定项
#   bash tools/freeze-check.sh --full --no-cleanup          # 跑完不停 server 容器
#
# 退出码：0 = 所有硬项通过（skip 不算失败）；1 = 有硬项失败（末行列出）；2 = 用法/环境错误。
#
# 纪律：--full 里的构建/容器项内部走 tools/build-lock.sh（≤2 槽），跑完自动
#       `cd server && docker compose stop`（数据卷保留）。不碰 guanyin-* 与别人的容器。
# 细节与逐项命令表见 tools/README-一键验收.md。
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ $# -eq 0 ]; then
    echo "用法：bash tools/freeze-check.sh {--quick|--full} [--list] [--only id,id] [--no-cleanup]" >&2
    echo "      不带参数默认 --quick（日常自检）；冻结前请显式用 --full" >&2
    exec python3 "$ROOT/tools/freeze_check.py" --quick "$@"
fi

exec python3 "$ROOT/tools/freeze_check.py" "$@"
