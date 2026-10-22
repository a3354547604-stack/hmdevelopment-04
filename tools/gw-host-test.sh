#!/usr/bin/env bash
# =============================================================================
# tools/gw-host-test.sh — 网关纯逻辑离线自测（不需要板子、不需要串口）
#
# 编进自测的源码（能离开硬件跑的那部分）：
#   src/  : gw_proto / gw_rules / gw_crypto / gw_time / gw_iotda / gw_cache / gw_alarm / gw_433 / gw_timesync / gw_kvcfg
#   test/ : gw_host_plat.c（把 gpio/uart/flash 换内存桩）+ gw_selftest.c（断言）
#   不编  : gw_main.c（板级入口）、gw_plat.c（真外设）、gw_cloud.c（要 paho MQTT，只能上板验）
# 依赖：cJSON 直接用 SDK 里那份 third_party/cJSON/cJSON.c —— 与上板同版本，
#       避免"主机用别的 JSON 库测过了、板上行为却不一样"这种假绿灯。
#
# 两种模式（脚本自动选）：
#   ① 主机有编译器（gcc/cc/clang）→ 直接用主机编译器；
#   ② 主机没有（本机 WSL Ubuntu 26.04 就没装 gcc）→ 用 t4 的构建容器
#      hmprogram-rk2206-build（里面自带 gcc 9.4）：源码 docker cp 进容器 /tmp/gw-hosttest
#      编译运行，日志取回 rk2206-gateway/out/。所以"只有 docker 的机器"也能复现同一结果。
#
# 用法：tools/gw-host-test.sh
# 退出码：0 = 全部用例通过；1 = 有用例失败或编译失败；
#         2 = **环境不可用**（容器没在跑 / 没有编译器 / 本次没产生日志）——此时绝不打印「通过」
# ⚠ t142 假绿修复：环境不可用必须 exit 2，且判定只读**本次**运行产生的日志。
#   旧版把 run_container 守门返回的 2 用 `|| true` 吞掉，然后拿上一次的 host-test.log
#   判出「通过 459 / 失败 0」并打印「全部用例通过」——容器根本没起来（t141 首日踩到：
#   拿到假的 447，重开 Docker Desktop + docker start 容器后才是真的 459）。
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${GW_SDK:-$HOME/rk2206/lockzhiner-rk2206-openharmony3.0lts}"
CONT="${GW_CONTAINER:-hmprogram-rk2206-build}"
SRC="$ROOT/rk2206-gateway/src"
TEST="$ROOT/rk2206-gateway/test"
OUT="$ROOT/rk2206-gateway/out"
CC="${CC:-gcc}"
CJSON_SRC="$SDK/third_party/cJSON/cJSON.c"
CJSON_DIR_IN_CONT=/home/openharmony/third_party/cJSON

LOGF="$OUT/host-test.log"
BUILDF="$OUT/host-build.log"
STAGE=/tmp/gw-hosttest

mkdir -p "$OUT"
[ -f "$CJSON_SRC" ] || { echo "找不到 $CJSON_SRC（cJSON 随 SDK 提供；用 GW_SDK=<SDK 路径> 指定）" >&2; exit 2; }

# t139 起加 -DGW_HW_HOST：gw_hw.c 里只有「`keys` 参数分派」这层是纯逻辑，
# 采样/基线/报告文本要碰 SDK 的 ADC，主机上没有那些符号（同 gw_net.c 的 GW_NET_HOST 口径）。
CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter -DGW_HW_HOST=1'
OBJS='gw_proto.c gw_rules.c gw_crypto.c gw_time.c gw_iotda.c gw_cache.c gw_alarm.c gw_433.c gw_timesync.c gw_kvcfg.c gw_wifi_shell.c gw_scan.c gw_key.c gw_hw.c gw_hw_adc.c'
# ↑ t34 起多一个 gw_kvcfg.c（凭证 Flash KV 的编解码与串口命令本体：纯逻辑、不碰 SDK）。
#   它必须在这份清单里，否则 test/gw_selftest.c 的 test_kvcfg() 会因缺符号编译不过。
#   t136 起再多一个 gw_wifi_shell.c（`wifi show/set/scan` 的参数解析与边界校验：
#   副作用全走 gw_wifi_ops_t 注入，所以它能在主机上跑，test_wifi_cmd() 覆盖全部分支）。
#   gw_kvcfg_shell.c（osCmdReg 那层）是板级，主机刻意不编 —— 与 gw_plat.c/gw_main.c 同理。

run_host() {
    echo "[gw-host-test] 模式：主机编译器 $CC"
    set +e
    "$CC" $CFLAGS -I "$SRC" -I "$TEST/stub" -I "$(dirname "$CJSON_SRC")" \
        $(for o in $OBJS; do printf '%s ' "$SRC/$o"; done) \
        "$TEST/gw_host_plat.c" "$TEST/gw_selftest.c" "$CJSON_SRC" \
        -o "$OUT/gw_selftest" > "$BUILDF" 2>&1
    local brc=$?
    [ "$brc" = "0" ] || { cat "$BUILDF"; return 1; }
    "$OUT/gw_selftest" > "$LOGF" 2>&1
    local trc=$?
    # 自测程序自己的退出码只表达「用例过/不过」，统一归一到 0/1，
    # 免得它的非 0 值（如 2）被上层误当成「环境不可用」。
    [ "$trc" = "0" ] && return 0 || return 1
}

run_container() {
    echo "[gw-host-test] 模式：构建容器 $CONT（主机没有编译器）"
    # 返回码约定：0 = 跑完；1 = 用例/编译失败；**3 = 环境不可用**（上层映射成脚本 exit 2）
    docker inspect -f '{{.State.Running}}' "$CONT" 2>/dev/null | grep -q true || {
        echo "容器 $CONT 没在运行：docker start $CONT（或见 tools/gw-build.sh 顶部注释重建）" >&2
        return 3
    }
    # 用 tar 流而不是 docker cp：docker cp 在 /mnt/c（DrvFs）上出现过只拷了一部分文件、
    # 却仍然返回 0 的情况，随后 gcc 报一串 "No such file or directory" 让人以为是代码问题。
    # tar 是单流、可断言的，拷完立刻数文件，少了就报「暂存失败」而不是「编译失败」。
    docker exec "$CONT" bash -lc "rm -rf $STAGE && mkdir -p $STAGE/src $STAGE/stub"
    tar cf - -C "$SRC"  . | docker exec -i "$CONT" bash -c "tar xf - -C $STAGE/src"
    tar cf - -C "$TEST" . | docker exec -i "$CONT" bash -c "tar xf - -C $STAGE"
    staged="$(docker exec "$CONT" bash -lc "ls $STAGE/src/*.c 2>/dev/null | wc -l; ls $STAGE/src/*.h 2>/dev/null | wc -l; ls $STAGE/stub/*.h $STAGE/stub/lwip/*.h 2>/dev/null | wc -l" | tr '\n' ' ')"
    set -- $staged
    if [ "${1:-0}" -lt 12 ] || [ "${2:-0}" -lt 12 ] || [ "${3:-0}" -lt 7 ]; then
        echo "[gw-host-test] 暂存失败：src 只拷到 ${1:-0} 个 .c / ${2:-0} 个 .h，stub ${3:-0} 个头（期望 12/12/7）" >&2
        return 3
    fi

    # 注意：docker exec bash -lc 的字符串里**不能**用反斜杠续行 —— 宿主的双引号会先把
    # "\<换行>" 吃掉，内层 bash 收到的是换行分隔的多条命令，就会去执行 .c 文件。
    # 所以这里拼成一条单行命令。
    local cmd="cd $STAGE && gcc $CFLAGS -I $STAGE/src -I $STAGE/stub -I $CJSON_DIR_IN_CONT $(for o in $OBJS; do printf '%s ' "$STAGE/src/$o"; done)$STAGE/gw_host_plat.c $STAGE/gw_selftest.c $CJSON_DIR_IN_CONT/cJSON.c -o $STAGE/gw_selftest 2> build.log; brc=\$?; echo BUILD_RC=\$brc; cat build.log; if [ \$brc = 0 ]; then $STAGE/gw_selftest; echo RUN_RC=\$?; fi"
    set +e
    docker exec "$CONT" bash -lc "$cmd" > "$LOGF" 2>&1
    local rc=$?
    set -e
    docker exec "$CONT" bash -lc "rm -rf $STAGE" >/dev/null 2>&1 || true
    return $rc
}

# t142：判定只许读**本次**运行产生的日志。先删上一次的——旧版最致命的假绿就在这：
# run_container 的守门 `return 2` 被 `|| true` 吞掉，随后拿上一次的 host-test.log
# 判出「通过 459 / 失败 0」并打印「全部用例通过」，而容器根本没跑。
rm -f "$LOGF" "$BUILDF"

mode=host
run_rc=0
if ! command -v "$CC" >/dev/null 2>&1; then
    mode=container
    run_container || run_rc=$?
else
    run_host || run_rc=$?
fi

if [ "$run_rc" = "3" ]; then
    echo "[gw-host-test] 环境不可用（脚本 exit 2，不是用例失败）：$mode 模式没跑起来，本次没有结果——不判定为成功。" >&2
    exit 2
fi
if [ "$run_rc" != "0" ]; then
    echo "[gw-host-test] 本次运行失败（exit 1）：编译或用例没通过。" >&2
    if [ -s "$BUILDF" ]; then
        grep -E "error:" "$BUILDF" | head -20 >&2 || true
    fi
    if [ -s "$LOGF" ]; then
        grep -E "✗|失败 [1-9]" "$LOGF" | head -20 >&2 || true
    fi
    exit 1
fi
if [ ! -s "$LOGF" ]; then
    echo "[gw-host-test] 环境不可用（exit 2）：跑完却没有日志 $LOGF——上一次的日志已删除、不复用，故不判定为成功。" >&2
    exit 2
fi

# ------------------------------ 判定 ------------------------------
ok=1
if [ "$mode" = "container" ]; then
    # docker exec 中途失败（引擎掉线等）时日志里连 BUILD_RC 都不会有：那是环境问题，不是用例失败
    if ! grep -q "^BUILD_RC=" "$LOGF"; then
        echo "[gw-host-test] 环境不可用（脚本 exit 2）：容器日志里没有 BUILD_RC 标记（docker exec 失败？），不判定为成功。" >&2
        exit 2
    fi
    # 把编译输出和运行输出分开：编译段到 RUN_RC 之前，运行段在其后
    sed -n '1,/^RUN_RC=/p' "$LOGF" > "$BUILDF" || true
    grep -q "^BUILD_RC=0" "$LOGF" || ok=0
    grep -q "^RUN_RC=0"   "$LOGF" || ok=0
else
    grep -q "error:" "$BUILDF" && ok=0 || true
fi
grep -q "失败 0 ====" "$LOGF" || ok=0

warn="$(grep -c "warning:" "$BUILDF" 2>/dev/null || true)"
summary="$(grep -oE '通过 [0-9]+ / 失败 [0-9]+' "$LOGF" | tail -1 || true)"
echo "[gw-host-test] 编译 warning=$warn；用例 $summary；日志 $LOGF"

if [ "$ok" = "1" ]; then
    echo "[gw-host-test] 全部用例通过"
    exit 0
fi
echo "[gw-host-test] 有失败用例或编译错误（见 $LOGF / $BUILDF）" >&2
grep -E "✗|error:|BUILD_RC=[^0]|RUN_RC=[^0]" "$LOGF" | head -20 >&2 || true
exit 1
