#!/usr/bin/env bash
#===============================================================================
# WS63 SDK 共享输出树 · 专用互斥锁（t51）
#
# 为什么需要它（一手证据，2026-10-18 08:51）：
#   `ws63-firmware/scripts/node-build.sh` 会把自研源码临时**覆盖**进 SDK 的
#   `src/application/samples/custom/`，再删掉 SDK 的输出目录重新编译。
#   两个 node-build.sh 同时跑时，一方会删掉另一方正在用的中间文件：
#     · 一侧  cc1: fatal error: …/output/ws63/acore/ws63-liteos-app/mconfig.h: No such file or directory
#     · 另一侧 CMake  No SOURCES given to target: samples（samples/CMakeLists.txt:72）BUILD_EXIT=1
#   两边都失败，且原样重跑就能过 —— 典型的共享输出树竞争，不是代码缺陷。
#
#   `tools/build-lock.sh` 的锁槽（/tmp/hm-build.lock、/tmp/hm-build-2.lock）**只限并发数量**
#   （最多两个重构建，保护 7.4GB 内存），它**不互斥共享输出树**：两个槽各跑一份
#   node-build.sh 完全合法，然后就一起炸。所以数量锁之外还要这一层"同一棵树"的锁。
#
# 设计要点（每条都是刻意的）：
#   1) **按锁文件路径互斥**，默认 /tmp/hm-ws63-sdk.lock —— 与 /tmp/hm-build*.lock 是不同文件，
#      所以 RK2206 / harmony-app / 其它仓库的构建**永远不会**被这层锁挡住（要的只是"同一棵 SDK"）。
#   2) **用 flock(2)，不用 mkdir 标志目录**：进程无论怎么死（Ctrl-C、kill -9、OOM）
#      内核都会释放锁，不存在"残留锁需要人手删"的坑。
#   3) **拿到锁后 `exec` 目标命令**（而不是当子进程跑）：锁的持有者就是构建进程本身，
#      构建一结束（任何方式）锁立刻释放。fd 9 在 exec 后依然有效（本机实测：
#      `exec 9>>lock; flock -n 9; exec sleep 4` 期间另一进程拿不到锁，sleep 结束后能拿到）。
#   4) **超时不无限挂起**：默认 1800 s，可用 HM_WS63_SDK_LOCK_TIMEOUT 调；超时打印
#      锁路径 + 占用者记录后以 75 退出（EX_TEMPFAIL，"稍后重试"的约定码）。
#
# 用法：
#   ws63-firmware/scripts/ws63-sdk-lock.sh [选项] -- 命令 [参数...]
#     --sdk DIR          只是写进占用者记录的提示（这棵树是谁）
#     --label TEXT       写进占用者记录，说明这次跑的是什么
#     --lock PATH        覆盖锁文件路径（同 HM_WS63_SDK_LOCK）
#     --timeout SEC      覆盖等待上限（同 HM_WS63_SDK_LOCK_TIMEOUT）
#     -h | --help        本帮助
#
# 环境变量：
#   HM_WS63_SDK_LOCK          锁文件路径，默认 /tmp/hm-ws63-sdk.lock
#   HM_WS63_SDK_LOCK_TIMEOUT  等待上限（秒），默认 1800；0 = 不等待，拿不到就报错
#   HM_WS63_SDK_LOCK_LOG      可选：把"等待/获取"记录追加到这个文件（便于事后对时序）
#   HM_WS63_SDK_LOCK_HELD     由调用方设为 1 表示"已在锁内"，用于避免自己锁自己
#
# 注意（嵌套顺序）：**数量锁在外、本锁在内**，即
#   HM_BUILD_LOCK=… tools/build-lock.sh bash -c '… scripts/node-build.sh …'
#   绝不要在持有本锁时再去抢 tools/build-lock.sh 的槽 —— 那样两个进程可能各持一把互等。
#   node-build.sh 已经把本锁内建在自己的最前面，所以正常只需照旧调用 node-build.sh。
#===============================================================================
set -euo pipefail

LOCK="${HM_WS63_SDK_LOCK:-/tmp/hm-ws63-sdk.lock}"
TIMEOUT="${HM_WS63_SDK_LOCK_TIMEOUT:-1800}"
LOG="${HM_WS63_SDK_LOCK_LOG:-}"
SDK_HINT=""
LABEL=""
SELF_NAME="ws63-sdk-lock.sh"

usage() {
    # 打印文件头注释块（第 2 行到第一个非注释行为止）—— 头注释就是帮助文本，不必维护两份
    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "${BASH_SOURCE[0]}"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --sdk)     SDK_HINT="${2:-}"; shift 2 ;;
        --label)   LABEL="${2:-}"; shift 2 ;;
        --lock)    LOCK="${2:-}"; shift 2 ;;
        --timeout) TIMEOUT="${2:-}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        --)        shift; break ;;
        *)         break ;;
    esac
done
[ $# -gt 0 ] || { echo "[ws63-lock] 错误：没有给要执行的命令。用 -- 分隔，见 --help" >&2; exit 2; }

case "$TIMEOUT" in
    ''|*[!0-9]*) echo "[ws63-lock] 错误：超时值必须是秒数（0 或正整数），实际 '$TIMEOUT'" >&2; exit 2 ;;
esac

OWNER="$LOCK.owner"

# 记录一律带 UTC 时间戳与 pid：并发时序是这张卡的主要证据，事后要能对上。
wlog() {
    local line
    line="[ws63-lock] $(date -u +%Y-%m-%dT%H:%M:%SZ) pid=$$ $*"
    echo "$line" >&2
    if [ -n "$LOG" ]; then echo "$line" >> "$LOG" 2>/dev/null || true; fi
}

owner_dump() {
    [ -f "$OWNER" ] || { echo "    （没有占用者记录 $OWNER）"; return 0; }
    local opid
    opid="$(sed -n 's/^pid=//p' "$OWNER" 2>/dev/null | head -1)"
    echo "    占用者记录：$OWNER"
    sed 's/^/      /' "$OWNER" 2>/dev/null || true
    if [ -n "$opid" ] && [ "$opid" != "$$" ]; then
        if kill -0 "$opid" 2>/dev/null; then
            echo "      → pid $opid 仍在运行：$(ps -o lstart=,cmd= -p "$opid" 2>/dev/null | head -1)"
        else
            echo "      → pid $opid 已不存在（可能是上一次的残留记录，锁本身不受影响）"
        fi
    fi
}

mkdir -p "$(dirname "$LOCK")" 2>/dev/null || true

# fd 9 全程持有这个锁（见文件头要点 3）。用 >> 而不是 >：避免截断时惊动另一个进程的观测。
exec 9>>"$LOCK" || { echo "[ws63-lock] 错误：打不开锁文件 $LOCK（检查目录权限）" >&2; exit 1; }

WAITED=0
if ! flock -n 9; then
    WAITED=1
    wlog "等待中：同一棵 WS63 SDK 已被占用（锁 $LOCK，等待上限 ${TIMEOUT}s${SDK_HINT:+，树 $SDK_HINT}）"
    owner_dump >&2
    if [ "$TIMEOUT" = "0" ]; then
        wlog "错误：锁被占用且超时值设为 0（不等待）——放弃，不无限挂起"
        echo "  · 锁文件：$LOCK" >&2
        owner_dump >&2
        exit 75
    fi
    # flock -w 覆盖"被信号打断"的情况：返回非 0 一律当作没拿到
    if ! flock -w "$TIMEOUT" 9; then
        wlog "错误：等待超过 ${TIMEOUT}s 仍未拿到锁——放弃（不会无限挂起）"
        echo "  · 锁文件：$LOCK" >&2
        owner_dump >&2
        echo "  · 处置：等占用者结束再重试；想等更久用 HM_WS63_SDK_LOCK_TIMEOUT=<秒>。" >&2
        echo "          确认没有构建在跑（上面 pid 已不存在）时，残留的 $OWNER 记录可以直接忽略——" >&2
        echo "          flock 的持有关系由内核维护，进程一死锁自动释放，不需要也不应该手删锁文件。" >&2
        exit 75
    fi
    wlog "已获取锁（等待结束）"
fi

{
    echo "pid=$$"
    echo "host=$(hostname 2>/dev/null || echo '?')"
    echo "user=$(id -un 2>/dev/null || echo '?')"
    echo "started_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "lock=$LOCK"
    [ -n "$SDK_HINT" ] && echo "sdk=$SDK_HINT"
    [ -n "$LABEL" ] && echo "label=$LABEL"
    echo "cwd=$PWD"
    printf 'cmd='
    printf '%q ' "$@"
    echo
} > "$OWNER" 2>/dev/null || true

if [ "$WAITED" = "1" ]; then
    wlog "开始执行（等待过锁）：$*"
else
    wlog "无需等待，直接拿到锁：$*"
fi

# 锁的持有者=接下来这条命令本身：它一退出（正常/异常/被杀）内核就释放锁。
# 代价是这里不再有"执行结束"的日志——那正是要点 3 换来的健壮性。
exec "$@"
