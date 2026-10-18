#!/usr/bin/env bash
# 数据库备份/恢复脚本的公共函数（被 db-backup.sh / db-restore.sh / db-rows.sh source）。
#
# 为什么用 `docker exec` 而不是宿主机 mysql 客户端：部署机只保证有 docker，
# 不保证装了 mysql-client；少一个客户端就会让新手卡在「command not found」。
# 为什么校验清单用 TSV 而不是 JSON：部署机上不保证有 python3，纯 bash/awk 才靠得住。
set -euo pipefail

SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_FILE="${ENV_FILE:-$SERVER_DIR/.env}"
DB_CONTAINER="${DB_CONTAINER:-hmprogram-server-db}"

# 读 .env 里的一个键：去掉行尾注释与首尾空白。
# 注意：口令里不要出现「空格+#」（会被当成注释，与 python-dotenv 的口径一致）。
env_get() {
  local key="$1" default="${2:-}" val
  if [ -f "$ENV_FILE" ]; then
    val="$(sed -n "s/^[[:space:]]*${key}=//p" "$ENV_FILE" | head -n1 \
           | sed 's/[[:space:]]*#.*$//' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//' | tr -d '\r')"
  else
    val=""
  fi
  printf '%s' "${val:-$default}"
}

db_name()   { env_get DB_NAME eldercare; }
db_user()   { env_get DB_USER eldercare; }
db_pass()   { env_get DB_PASSWORD hmprogram-dev-pass; }
root_pass() { env_get MYSQL_ROOT_PASSWORD hmprogram-dev-root; }

require_container() {
  if ! command -v docker >/dev/null 2>&1; then
    echo "✗ 找不到 docker：本脚本要在部署机上跑，且 docker 必须在 PATH 里" >&2
    exit 2
  fi
  local running
  running="$(docker inspect -f '{{.State.Running}}' "$DB_CONTAINER" 2>/dev/null || echo missing)"
  if [ "$running" != "true" ]; then
    echo "✗ db 容器 $DB_CONTAINER 没在运行（当前状态：$running）" >&2
    echo "  先起服务：cd $SERVER_DIR && docker compose -f docker-compose.prod.yml up -d db" >&2
    exit 2
  fi
}

# 业务库里的全部基本表（按名字排序，保证多次输出可比）
table_list() {
  docker exec -e MYSQL_PWD="$(db_pass)" "$DB_CONTAINER" \
    mysql -N -B -u"$(db_user)" -D "$(db_name)" \
    -e "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME"
}

table_rows() {
  docker exec -e MYSQL_PWD="$(db_pass)" "$DB_CONTAINER" \
    mysql -N -B -u"$(db_user)" -D "$(db_name)" -e "SELECT COUNT(*) FROM \`$1\`" | tr -d '\r'
}

# 逐表行数清单（TSV：表名 <TAB> 行数），供恢复后比对
rows_tsv() {
  local table
  while IFS= read -r table; do
    [ -n "$table" ] || continue
    printf '%s\t%s\n' "$table" "$(table_rows "$table")"
  done < <(table_list)
}

total_rows() { awk -F'\t' '{s += $2} END {print s + 0}' "$1"; }

# 比对「清单 vs 当前库」：一致返回 0，有差异返回 1（并把差异逐行打出来）。
# 注意：用重定向而不是进程替换 `<( )`，否则循环跑在子 shell 里，COMPARE_BAD 回不来。
compare_rows() {
  local manifest="$1" label="${2:-恢复后}"
  local table expect got tmp
  COMPARE_BAD=0
  tmp="$(mktemp)"
  echo "→ $label 逐表行数比对（清单：$(basename "$manifest")）"
  while IFS=$'\t' read -r table expect; do
    [ -n "$table" ] || continue
    case "$table" in \#*) continue ;; esac   # 清单头部是 # 注释行（校验和、时间等）
    got="$(table_rows "$table")"
    if [ "$expect" = "$got" ]; then
      printf '  ✓ %-24s %s\n' "$table" "$got"
    else
      printf '  ✗ %-24s 期望 %s，实际 %s\n' "$table" "$expect" "$got"
      COMPARE_BAD=$((COMPARE_BAD + 1))
    fi
  done <"$manifest"

  # 库里多出来的表（清单里没记）同样算差异——「少恢复了一张表」不能算一致
  table_list >"$tmp"
  while IFS= read -r table; do
    [ -n "$table" ] || continue
    if ! grep -qP "^$(printf '%s' "$table" | sed 's/[.[\*^$]/\\&/g')\t" "$manifest"; then
      printf '  ✗ %-24s 清单里没有这张表（多出来的表）\n' "$table"
      COMPARE_BAD=$((COMPARE_BAD + 1))
    fi
  done <"$tmp"
  rm -f "$tmp"

  if [ "$COMPARE_BAD" -eq 0 ]; then
    echo "✓ 一致：全部表行数与清单相同（共 $(total_rows "$manifest") 行）"
    return 0
  fi
  echo "✗ 有 $COMPARE_BAD 处不一致，不要把它当成恢复成功" >&2
  return 1
}

utc_stamp() { date -u +%Y%m%d-%H%M%S; }
