#!/usr/bin/env bash
# 从备份恢复 hmprogram 业务库，并核对「数据一致」。
#
# 用法：
#   server/tools/db-restore.sh server/backups/db-eldercare-20261018-012345.sql.gz          # 会要求输入 yes 确认
#   server/tools/db-restore.sh <备份文件> --yes                                             # 脚本/施工单里用，跳过确认
#   server/tools/db-restore.sh <备份文件> --yes --keep-db                                   # 只导数据，不 DROP 库
#
# 做什么（默认口径 = 干净恢复，能真正确认备份可用）：
#   1) 校验 gzip 完整性，防止拿半截文件恢复；
#   2) DROP DATABASE + CREATE DATABASE（清库），再 gzip -dc 导入；
#   3) 用同名 .rows.tsv 清单逐表比对行数：全部相同才算成功（退出码 0）。
#
# 危险点与保护：清库不可逆，所以默认要求人工输入 yes；只操作 <DB_NAME> 这一个库，
# 不会碰这台机器上别的项目的容器或库。
set -euo pipefail
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/db-common.sh"

DUMP="${1:-}"
CONFIRM="no"
KEEP_DB=0
for arg in "${@:2}"; do
  case "$arg" in
    --yes|-y) CONFIRM="yes" ;;
    --keep-db) KEEP_DB=1 ;;
    *) echo "✗ 不认识的参数：$arg（可用：--yes / --keep-db）" >&2; exit 2 ;;
  esac
done

if [ -z "$DUMP" ]; then
  echo "用法：$0 <备份文件.sql.gz> [--yes] [--keep-db]" >&2
  echo "  可用备份（最近 5 份）：" >&2
  ls -1t "$SERVER_DIR/backups"/db-*.sql.gz 2>/dev/null | head -5 >&2 || echo "  （server/backups/ 下还没有备份）" >&2
  exit 2
fi
[ -f "$DUMP" ] || { echo "✗ 找不到备份文件：$DUMP" >&2; exit 2; }

MANIFEST="${DUMP%.sql.gz}.rows.tsv"
DB="$(db_name)"

require_container
gzip -t "$DUMP" || { echo "✗ gzip 校验失败，备份文件已损坏：$DUMP" >&2; exit 3; }

echo "即将把备份恢复到库 $DB（容器 $DB_CONTAINER）："
echo "  备份文件：$DUMP（$(($(wc -c <"$DUMP" | tr -d ' ') / 1024)) KB）"
if [ "$KEEP_DB" -eq 1 ]; then
  echo "  方式    ：--keep-db：保留现有数据，直接把备份里的内容导进去（可能与现有行冲突）"
else
  echo "  方式    ：清库恢复 —— 先 DROP DATABASE $DB 再重建，现有数据会被备份内容**完全替换**"
fi
if [ "$CONFIRM" != "yes" ]; then
  printf '确认继续？输入 yes 回车（其它任何输入都取消）：'
  read -r answer
  [ "$answer" = "yes" ] || { echo "已取消，未做任何改动"; exit 1; }
fi

if [ "$KEEP_DB" -eq 0 ]; then
  echo "→ 清库：DROP DATABASE + CREATE DATABASE $DB"
  docker exec -e MYSQL_PWD="$(root_pass)" "$DB_CONTAINER" \
    mysql -uroot -e "DROP DATABASE IF EXISTS \`$DB\`; CREATE DATABASE \`$DB\` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;"
fi

echo "→ 导入数据（gzip -dc | mysql）"
gzip -dc "$DUMP" | docker exec -i -e MYSQL_PWD="$(db_pass)" "$DB_CONTAINER" \
  mysql -u"$(db_user)" "$DB"

if [ -f "$MANIFEST" ]; then
  compare_rows "$MANIFEST" "恢复后" || exit 4
  echo "✓ 恢复完成且数据一致：$DUMP"
else
  echo "✓ 导入完成（没有找到同名行数清单 ${MANIFEST##*/}，无法自动比对行数）"
  echo "  想核对的话：server/tools/db-rows.sh"
fi
echo "  下一步：curl -s http://127.0.0.1:3000/api/health   # 应回 db:ok"
