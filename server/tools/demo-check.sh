#!/usr/bin/env bash
# 演示数据自检（t49）：在 api 容器里跑 app.demo_check，逐条核对 D1–D7。
#
# 为什么在容器里跑而不是宿主机：判据既要走 REST（App 看到的响应），又要读 MySQL（补传证据、报表自洽），
# 容器里已有 DB_HOST/口令与 requests 依赖，宿主机未必装齐。
#
# 用法：
#   bash tools/demo-check.sh                     # dev 栈（docker-compose.yml，现场演示用这套）
#   bash tools/demo-check.sh --prod              # 生产栈（docker-compose.prod.yml）
#   bash tools/demo-check.sh -- --device-id 2    # `--` 之后的参数透传给 app.demo_check
set -euo pipefail

cd "$(dirname "$0")/.."                     # 统一在 server/ 下执行，compose 才找得到 .env 与相对路径

COMPOSE_FILE="docker-compose.yml"
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --prod) COMPOSE_FILE="docker-compose.prod.yml"; shift ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "未知参数：$1（可用 --prod，或 -- 之后透传给 app.demo_check）" >&2; exit 2 ;;
  esac
done

if [ -z "$(docker compose -f "$COMPOSE_FILE" ps -q api 2>/dev/null)" ]; then
  echo "api 容器没在跑：先 cd server && docker compose up -d（演示前还要跑 seed --demo）" >&2
  exit 3
fi

echo "== 演示数据自检（$COMPOSE_FILE）=="
if docker compose -f "$COMPOSE_FILE" exec -T api python -m app.demo_check ${EXTRA[@]+"${EXTRA[@]}"}; then
  echo "== 全绿：D1–D7 判据全部通过 =="
else
  rc=$?
  echo "== 有失败项（退出码 $rc）：先 python -m app.seed --demo 重置，再看上面 FAIL 的具体原因 ==" >&2
  exit "$rc"
fi
