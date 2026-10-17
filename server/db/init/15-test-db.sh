# 只在「数据卷第一次创建」时由 MySQL 官方 entrypoint 执行（已存在的卷不会重跑）。
# 目的：pytest 每个用例都会 TRUNCATE 业务表，必须打到独立的 <DB_NAME>_test 库，
# 否则一次 `docker compose run --rm api pytest` 就会把演示种子数据（app.seed）清空。
# 用 .sh 而不是 .sql 是因为要读 MYSQL_DATABASE / MYSQL_USER 环境变量（库名可被 .env 覆盖）。
set -eu

mysql -uroot -p"${MYSQL_ROOT_PASSWORD}" <<SQL
CREATE DATABASE IF NOT EXISTS \`${MYSQL_DATABASE}_test\`
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;
GRANT ALL PRIVILEGES ON \`${MYSQL_DATABASE}_test\`.* TO '${MYSQL_USER}'@'%';
FLUSH PRIVILEGES;
SQL
