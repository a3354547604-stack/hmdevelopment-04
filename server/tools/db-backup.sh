#!/usr/bin/env bash
# 备份 hmprogram 业务库：mysqldump（在 db 容器内执行）→ gzip → server/backups/
#
# 用法：
#   server/tools/db-backup.sh [输出目录]        # 默认 server/backups/
#   BACKUP_KEEP=7 server/tools/db-backup.sh     # 只保留最近 7 份（默认 14）
#
# 产物（成对出现，恢复时要一起用）：
#   db-<库名>-<UTC时间戳>.sql.gz     数据本体（含建表语句、触发器、事件）
#   db-<库名>-<UTC时间戳>.rows.tsv   逐表行数清单，恢复后用它核对「数据一致」
#
# 只碰 hmprogram 项目的 db 容器与 <DB_NAME> 库，不会动这台机器上别的项目的容器/库。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/db-common.sh"

OUT_DIR="${1:-$SERVER_DIR/backups}"
KEEP="${BACKUP_KEEP:-14}"
mkdir -p "$OUT_DIR"

require_container
DB="$(db_name)"
USER="$(db_user)"
STAMP="$(utc_stamp)"
BASE="db-${DB}-${STAMP}"
DUMP="$OUT_DIR/$BASE.sql.gz"
MANIFEST="$OUT_DIR/$BASE.rows.tsv"

echo "→ 备份库 $DB（容器 $DB_CONTAINER）→ $DUMP"

# --single-transaction：InnoDB 一致性快照，不锁表（在线备份不打断服务）
# --quick：大表不整块读进内存（部署机内存小）
# --no-tablespaces：不要求 PROCESS 权限，非 root 账号也能备份
# 口令走 MYSQL_PWD，不出现在命令行参数里（ps 看不到）
if ! docker exec -e MYSQL_PWD="$(db_pass)" "$DB_CONTAINER" \
      mysqldump --single-transaction --quick --routines --events --triggers \
                --default-character-set=utf8mb4 --no-tablespaces \
                -u"$USER" "$DB" 2>"$OUT_DIR/.dump.err" | gzip -9 >"$DUMP"; then
  echo "✗ mysqldump 失败：" >&2
  cat "$OUT_DIR/.dump.err" >&2
  rm -f "$DUMP" "$OUT_DIR/.dump.err"
  exit 3
fi
rm -f "$OUT_DIR/.dump.err"

# 立即自检：gzip 能解开 + 里面有 CREATE TABLE（防止「备份了个空文件还以为是好的」）
gzip -t "$DUMP"
if ! gzip -dc "$DUMP" | grep -q "CREATE TABLE"; then
  echo "✗ 备份文件里没有 CREATE TABLE，判定为坏备份，已删除" >&2
  rm -f "$DUMP"
  exit 3
fi

rows_tsv >"$MANIFEST"
SIZE="$(wc -c <"$DUMP" | tr -d ' ')"
SHA="$(sha256sum "$DUMP" | cut -d' ' -f1)"
TOTAL="$(total_rows "$MANIFEST")"

# 元数据写在清单头部注释行里：纯文本，没有 python3 也能看
{
  printf '# dump=%s\n' "$(basename "$DUMP")"
  printf '# sha256=%s\n' "$SHA"
  printf '# bytes=%s\n' "$SIZE"
  printf '# taken_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf '# db=%s\n' "$DB"
  printf '# total_rows=%s\n' "$TOTAL"
} | cat - "$MANIFEST" >"$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST"

# 轮转：只保留最近 KEEP 份（.sql.gz 与 .rows.tsv 一起删）
while IFS= read -r old; do
  [ -n "$old" ] || continue
  rm -f "$old" "${old%.sql.gz}.rows.tsv"
  echo "  · 清理旧备份 $(basename "$old")（保留最近 $KEEP 份）"
done < <(ls -1t "$OUT_DIR"/db-*.sql.gz 2>/dev/null | tail -n +$((KEEP + 1)) || true)

echo "✓ 备份完成：$(basename "$DUMP")（$((SIZE / 1024)) KB，共 $TOTAL 行）"
echo "  校验和：$SHA"
echo "  行数清单：$(basename "$MANIFEST")"
