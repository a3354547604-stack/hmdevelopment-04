#!/usr/bin/env bash
# 看/核对 hmprogram 业务库的逐表行数。
#
# 用法：
#   server/tools/db-rows.sh                                   # 打印当前各表行数
#   server/tools/db-rows.sh server/backups/db-....rows.tsv    # 与某份备份清单比对（不一致退出码 1）
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/db-common.sh"

require_container
MANIFEST="${1:-}"

if [ -z "$MANIFEST" ]; then
  echo "当前库 $(db_name) 各表行数："
  tmp="$(mktemp)"
  rows_tsv >"$tmp"
  while IFS=$'\t' read -r table count; do printf '  %-24s %s\n' "$table" "$count"; done <"$tmp"
  echo "  合计行数：$(total_rows "$tmp")"
  rm -f "$tmp"
  exit 0
fi

[ -f "$MANIFEST" ] || { echo "✗ 找不到清单文件：$MANIFEST" >&2; exit 2; }
compare_rows "$MANIFEST" "当前库"
