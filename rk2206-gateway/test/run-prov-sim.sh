#!/usr/bin/env bash
# =============================================================================
# rk2206-gateway/test/run-prov-sim.sh — 配网本地 API 主机自测（不需要板子、不需要手机）
#
# 为什么单独一个脚本、不塞进 tools/gw-host-test.sh：
#   ① tools/ 归 t16（t10 只被授权改 gw-build.sh / gw-compile-check.sh 两个文件），
#      而这个模拟器是 t10 的交付物，放在 rk2206-gateway/test/ 下才不会跟别人抢文件；
#   ② 两者的"被测对象"不同：gw-host-test 测协议/规则（原来那 9 个源文件），
#      这里测配网状态机 + HTTP 服务 + 路由（gw_prov / gw_http / gw_prov_route）。
#
# 编进自测的源码：
#   src/  : gw_prov.c（配网状态机与 5 个 API 逻辑）、gw_http.c（HTTP 服务端）、
#           gw_prov_route.c（路由 + H5 常量）
#   test/ : gw_prov_sim.c（假时钟 + 假 Wi-Fi + 假汇聚 + 真 TCP 回环请求）
#   cJSON 用 SDK 里那份 third_party/cJSON/cJSON.c（与上板同版本，避免假绿灯）
#   不编  : gw_net.c / gw_store.c（要 wifi_lite 与 Vendor Flash，只能上板验）
#
# 两种模式（自动选，和 gw-host-test.sh 一致）：
#   ① 主机有编译器 → 直接用主机编译器；
#   ② 主机没有（本机 WSL 没装 gcc）→ 用构建容器 hmprogram-rk2206-build（自带 gcc 9.4）。
#
# 用法：bash rk2206-gateway/test/run-prov-sim.sh
# 退出码：0 = 全部用例通过；1 = 有用例失败或编译失败；2 = 环境不齐
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="${GW_SDK:-$HOME/rk2206/lockzhiner-rk2206-openharmony3.0lts}"
CONT="${GW_CONTAINER:-hmprogram-rk2206-build}"
SRC="$ROOT/rk2206-gateway/src"
TEST="$ROOT/rk2206-gateway/test"
OUT="$ROOT/rk2206-gateway/out"
CC="${CC:-gcc}"
CJSON_SRC="$SDK/third_party/cJSON/cJSON.c"
CJSON_DIR_IN_CONT=/home/openharmony/third_party/cJSON

LOGF="$OUT/prov-sim.log"
BUILDF="$OUT/prov-sim-build.log"
STAGE=/tmp/gw-prov-sim

mkdir -p "$OUT"
[ -f "$CJSON_SRC" ] || { echo "找不到 $CJSON_SRC（cJSON 随 SDK 提供；用 GW_SDK=<SDK 路径> 指定）" >&2; exit 2; }

CFLAGS='-std=c99 -O1 -g -Wall -Wextra -Wno-unused-parameter'
OBJS='gw_prov.c gw_http.c gw_prov_route.c'

run_host() {
    echo "[prov-sim] 模式：主机编译器 $CC"
    set +e
    # shellcheck disable=SC2046
    "$CC" $CFLAGS -I "$SRC" -I "$TEST/stub" -I "$(dirname "$CJSON_SRC")" \
        $(for o in $OBJS; do printf '%s ' "$SRC/$o"; done) \
        "$TEST/gw_prov_sim.c" "$CJSON_SRC" \
        -o "$OUT/gw_prov_sim" > "$BUILDF" 2>&1
    local brc=$?
    [ "$brc" = "0" ] || { cat "$BUILDF"; return 1; }
    "$OUT/gw_prov_sim" > "$LOGF" 2>&1
    return $?
}

run_container() {
    echo "[prov-sim] 模式：构建容器 $CONT（主机没有编译器）"
    docker inspect -f '{{.State.Running}}' "$CONT" 2>/dev/null | grep -q true || {
        echo "容器 $CONT 没在运行：docker start $CONT" >&2
        return 2
    }
    # tar 单流：docker cp 在 /mnt/c（DrvFs）上出现过只拷一部分却返回 0 的情况
    docker exec "$CONT" bash -lc "rm -rf $STAGE && mkdir -p $STAGE/src $STAGE/stub"
    tar cf - -C "$SRC"  . | docker exec -i "$CONT" bash -c "tar xf - -C $STAGE/src"
    tar cf - -C "$TEST" . | docker exec -i "$CONT" bash -c "tar xf - -C $STAGE"
    staged="$(docker exec "$CONT" bash -lc "ls $STAGE/src/*.c 2>/dev/null | wc -l; ls $STAGE/stub/*.h $STAGE/stub/lwip/*.h 2>/dev/null | wc -l" | tr '\n' ' ')"
    set -- $staged
    if [ "${1:-0}" -lt 17 ] || [ "${2:-0}" -lt 7 ]; then
        echo "[prov-sim] 暂存失败：src 只拷到 ${1:-0} 个 .c，stub ${2:-0} 个头（期望 ≥17/7）" >&2
        return 2
    fi

    local cmd="cd $STAGE && gcc $CFLAGS -I $STAGE/src -I $STAGE/stub -I $CJSON_DIR_IN_CONT $(for o in $OBJS; do printf '%s ' "$STAGE/src/$o"; done)$STAGE/gw_prov_sim.c $CJSON_DIR_IN_CONT/cJSON.c -o $STAGE/gw_prov_sim 2> build.log; brc=\$?; echo BUILD_RC=\$brc; cat build.log; if [ \$brc = 0 ]; then $STAGE/gw_prov_sim; echo RUN_RC=\$?; fi"
    set +e
    docker exec "$CONT" bash -lc "$cmd" > "$LOGF" 2>&1
    local rc=$?
    set -e
    docker exec "$CONT" bash -lc "rm -rf $STAGE" >/dev/null 2>&1 || true
    return $rc
}

mode=host
if command -v "$CC" >/dev/null 2>&1; then
    run_host || true
else
    mode=container
    run_container || true
fi

ok=1
if [ "$mode" = "container" ]; then
    sed -n '1,/^RUN_RC=/p' "$LOGF" > "$BUILDF" || true
    grep -q "^BUILD_RC=0" "$LOGF" || ok=0
    grep -q "^RUN_RC=0"   "$LOGF" || ok=0
else
    grep -q "error:" "$BUILDF" && ok=0 || true
fi
grep -q "失败 0 ====" "$LOGF" || ok=0

warn="$(grep -c "warning:" "$BUILDF" 2>/dev/null || true)"
summary="$(grep -oE '通过 [0-9]+ / 失败 [0-9]+' "$LOGF" | tail -1 || true)"
echo "[prov-sim] 编译 warning=$warn；用例 $summary；日志 $LOGF"

if [ "$ok" = "1" ]; then
    echo "[prov-sim] 全部用例通过（配网本地 API §12）"
    exit 0
fi
echo "[prov-sim] 有失败用例或编译错误（见 $LOGF / $BUILDF）" >&2
grep -E "\[FAIL\]|error:|BUILD_RC=[^0]|RUN_RC=[^0]" "$LOGF" | head -25 >&2 || true
exit 1
