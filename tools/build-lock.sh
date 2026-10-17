#!/usr/bin/env bash
# 串行化重构建：同一时刻只允许一个重构建进程。
# 背景：WSL 只有 7.4GB 内存 + 2GB swap，7 个 AI 员工并行跑 hvigor/gradle/docker build
# 时会把内存打到换页，磁盘 I/O 100%，Windows 侧 vmmem 吃满 CPU。
# 用法：
#   tools/build-lock.sh <命令> [参数...]
#   tools/build-lock.sh bash -c 'cd harmony-app && hvigorw --max-workers=2 assembleHap'
# 锁文件默认 /tmp/hm-build.lock（WSL 内共享），可用 HM_BUILD_LOCK 覆盖。
# 超时 3600s：避免一个卡死的构建永久占锁；超时后命令不会执行（flock 退出码 1）。
set -euo pipefail
LOCK="${HM_BUILD_LOCK:-/tmp/hm-build.lock}"
if [ "$#" -eq 0 ]; then
  echo "用法: $0 <命令> [参数...]" >&2
  exit 2
fi
exec flock -w 3600 "$LOCK" "$@"
