#!/usr/bin/env bash
# =============================================================================
# tools/gw-compile-check.sh — 网关 12 个源文件的「秒级」交叉编译自检
#
# 为什么需要它：tools/gw-build.sh 每次都走完整 hb 流程（850+ 步、约 1 分钟），
# 而修一个编译错误（少个参数、宏名写错）本来只需要看一眼 gcc 的输出。
# 这个脚本把 hb 实际用的那条 gcc 命令行**原样借过来**（-I/-D/-mcpu 全部来自
# out/build-gateway.log，不自己编一套），只把源码换成当前工作树的 src/，
# 于是同一个错误能在 2 秒内复现，而不是 1 分钟。
#
# 与 gw-build.sh 的分工：这里是"语法/接口对不对"，能过 ≠ 链得进镜像；
# 「符号进了 liteos.elf」「锚点串进了 Firmware.img」这两条只有 gw-build.sh 能断言。
# 循环里：改代码 → gw-compile-check.sh（秒级）→ gw-build.sh（分钟级，收尾用）。
#
# 前提：① 容器 hmprogram-rk2206-build 在运行；
#       ② out/build-gateway.log 存在（先跑过一次 gw-build.sh gateway；
#          没有日志时会退化成"用 BUILD.gn 里的 include_dirs 拼"，可能少几条 SDK 路径）。
# 用法：tools/gw-compile-check.sh          只报错误，全绿则退出码 0
#       tools/gw-compile-check.sh -v       连编译命令一起打印
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONT="${GW_CONTAINER:-hmprogram-rk2206-build}"
SRC="$ROOT/rk2206-gateway/src"
OUT="$ROOT/rk2206-gateway/out"
LOG="$OUT/build-gateway.log"
WORKDIR_IN_CONT=/home/openharmony/out/rk2206/lockzhiner-rk2206   # ninja 的工作目录：-I 里的 ../../../ 从这里算
STAGE=/tmp/gwcc
TOOLCHAIN=/home/tools/gcc-arm-none-eabi-10.3-2021.10/bin
VERBOSE=0
[ "${1:-}" = "-v" ] && VERBOSE=1

[ -d "$SRC" ] || { echo "找不到 $SRC" >&2; exit 2; }
docker inspect -f '{{.State.Running}}' "$CONT" 2>/dev/null | grep -q true || {
    echo "容器 $CONT 没在运行：docker start $CONT" >&2; exit 2;
}
[ -f "$LOG" ] || { echo "警告：没有 $LOG，将只用 BUILD.gn 的 include_dirs（可能缺 SDK 路径）" >&2; }

# ---- 1. 借用 hb 实际用的编译参数 -------------------------------------------------
# 唯一来源：hb 日志里那条完整 gcc 命令行（tools/gw-build.sh 用 `hb build -f -v` 跑，
# ninja verbose 会把每条命令整条打进日志）。**不再自己拼一套** —— 试过：照 BUILD.gn 的
# include_dirs 补参数，会一路缺 los_list.h / target_config.h / soc.h / core_cm4.h /
# arch/cc.h / cmsis_os2.h / los_context.h / securec.h / wifi_device.h；改用「SDK 里所有
# 含 .h 的目录全 -I 进来」又会因为 porting 版 lwip/netif.h 和 musl bits/alltypes.h
# 抢先命中而报 IFNAMSIZ / off_t 这种**假**错误。所以只认日志，拿不到就明确失败：
# 宁可让人先跑一次 gw-build.sh，也不要给出一堆假编译错误。
FLAGSFILE="$OUT/gw-compile-flags.txt"
if [ ! -f "$LOG" ] || ! grep -q 'ccache arm-none-eabi-gcc.*gw_plat\.c' "$LOG"; then
    echo "[gw-compile-check] 拿不到编译参数：$LOG 里没有完整的 gcc 命令行。" >&2
    echo "                   先跑一次 tools/build-lock.sh bash tools/gw-build.sh gateway（它带 -v）。" >&2
    exit 2
fi
grep -m1 'ccache arm-none-eabi-gcc.*gw_plat\.c' "$LOG" \
  | sed -e 's/^.*ccache arm-none-eabi-gcc //' \
        -e 's| -c .*||' \
        -e 's|-I\.\./\.\./\.\./vendor/lockzhiner/rk2206/samples/t4_gateway||' \
        -e 's|-I\.\./\.\./\.\./|-I/home/openharmony/|g' \
  > "$FLAGSFILE.tmp"
n_i="$(grep -o -- '-I' "$FLAGSFILE.tmp" | wc -l)"
if [ "$n_i" -lt 8 ]; then
    echo "[gw-compile-check] 参数不完整：只解析出 $n_i 条 -I（期望 ≥8）。日志格式变了？" >&2
    rm -f "$FLAGSFILE.tmp"
    exit 2
fi
mv "$FLAGSFILE.tmp" "$FLAGSFILE"
printf '# 参数来源：%s 里的 hb 原始命令行（%s；%s 条 -I）\n' "$(basename "$LOG")" "$(date '+%F %T')" "$n_i" > "$OUT/gw-compile-flags.note"
FLAGS="$(tr '\n' ' ' < "$FLAGSFILE")"

# ---- 2. 源码进容器（tar 单流 + 数量断言；docker cp 在 /mnt/c 上静默少拷过文件）----
docker exec "$CONT" bash -lc "rm -rf $STAGE && mkdir -p $STAGE"
tar cf - -C "$SRC" . | docker exec -i "$CONT" bash -c "tar xf - -C $STAGE"
n_c="$(docker exec "$CONT" bash -lc "ls $STAGE/*.c | wc -l")"
[ "$n_c" -ge 12 ] || { echo "暂存失败：只拷到 $n_c 个 .c（期望 12）" >&2; exit 2; }

# ---- 3. 逐个源文件编译（-fsyntax-only 也会报"参数个数不对"这类接口错误）----
set +e
docker exec "$CONT" bash -lc "export PATH=$TOOLCHAIN:\$PATH; cd $WORKDIR_IN_CONT; \
    err=0; ok=0; \
    for f in $STAGE/*.c; do \
        out=\$(arm-none-eabi-gcc $FLAGS -I$STAGE -c \"\$f\" -o /dev/null 2>&1); \
        if [ \$? = 0 ]; then ok=\$((ok+1)); else err=\$((err+1)); echo \"==== \$(basename \$f) ====\"; echo \"\$out\"; fi; \
    done; \
    echo \"RESULT ok=\$ok err=\$err\"; exit \$err" > "$OUT/gw-compile-check.log" 2>&1
rc=$?
set -e

[ "$VERBOSE" = "1" ] && { echo "FLAGS: $FLAGS"; echo "WORKDIR: $WORKDIR_IN_CONT"; }
grep -vE '^$' "$OUT/gw-compile-check.log" | tail -60
echo "[gw-compile-check] 日志 $OUT/gw-compile-check.log"
if [ "$rc" != "0" ]; then
    echo "[gw-compile-check] 有编译错误（0 个错误才算过；能过也还要 gw-build.sh 断言链接）" >&2
    exit 1
fi
echo "[gw-compile-check] 12 个源文件全部编译通过（不含链接断言）"
exit 0
