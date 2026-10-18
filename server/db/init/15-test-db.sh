# 只在「数据卷第一次创建」时由 MySQL 官方 entrypoint 执行（已存在的卷不会重跑）。
# 目的：pytest 每个用例都会 TRUNCATE 业务表，必须打到独立的 <DB_NAME>_test 库，
# 否则一次 `docker compose run --rm api pytest` 就会把演示种子数据（app.seed）清空。
# 用 .sh 而不是 .sql 是因为要读 MYSQL_DATABASE / MYSQL_USER 环境变量（库名可被 .env 覆盖）。
#
# 【为什么 set 必须放在子 shell 里（t93，2026-10-18）】
# 本脚本是被官方 entrypoint 用 `. 15-test-db.sh`（source）执行的，脚本里顶层 `set` 会**污染 entrypoint 自己的 shell**。
# mysql:8.0 的 /usr/local/bin/docker-entrypoint.sh:342 正是 `if [ -n "$MYSQL_ONETIME_PASSWORD" ]`：
# 一旦把 nounset(-u) 带过去，entrypoint 后来调用 mysql_expire_root_user() 时就在这行以
# `MYSQL_ONETIME_PASSWORD: unbound variable` 中止 → db 容器 Exit 1 → 干净卷 `docker compose up -d` 起不来
# （t88 复核 F-04，一手日志 /tmp/t88v2-dbinit-bug.log）。
# 注意报错来自 -u 而不是 -e：只把 `set -eu` 改成 `set -u` 修不掉（-u 仍在），而只留 -e 又会让
# entrypoint 里任何一条返回非零的裸命令把整个初始化打断。所以两个选项都只在本脚本自己的子 shell 里开，
# 一个字节都不漏给 entrypoint。
if ! (
  set -eu
  mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" <<SQL
CREATE DATABASE IF NOT EXISTS \`${MYSQL_DATABASE}_test\`
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
GRANT ALL PRIVILEGES ON \`${MYSQL_DATABASE}_test\`.* TO '${MYSQL_USER}'@'%';
FLUSH PRIVILEGES;
SQL
); then
  # 真正建库失败时才让容器失败（exit 会终止 entrypoint，正是我们希望的结果）
  echo "[15-test-db] 创建 ${MYSQL_DATABASE}_test 失败，容器将以失败退出" >&2
  exit 1
fi
