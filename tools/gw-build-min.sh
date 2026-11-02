#!/usr/bin/env bash
# =============================================================================
# tools/gw-build-min.sh — 编「最小网关固件」（t192，源码在 rk2206-gateway/min/）
#
# 与 tools/gw-build.sh 的关系：**同一条 SDK 补丁纪律**（暂存源码 → 启用 feature →
# app_LIBS 挂库 → 构建 → 断言 → trap 无条件还原），但目标、暂存目录、库名、锚点串全不同，
# 所以是两个脚本而不是一个加参数：gw-build.sh 的断言里写死了 24 个源文件的编译记录，
# 混在一起会让"哪个目标失败"变得看不出来。
#
# 用法：
#   bash tools/gw-build-min.sh --check     只做主机侧检查（不联网、不构建）
#                                          ① 主机单测（58 条断言，含契约 §3.7 真帧与 PUBACK 超时）
#                                          ② arm-none-eabi-gcc -fsyntax-only 过一遍 5 个 .c
#   bash tools/build-lock.sh bash tools/gw-build-min.sh     出镜像（2097152 B）
#
# 版本号（t194）：脚本**从源码里读** `MIN_IMAGE_TAG`（rk2206-gateway/min/min_gw.h），
# 不再手抄常量。每次构建产两份：
#   out/gateway-Firmware.img                      RKDevTool 用的固定名（永远=本次构建）
#   out/gateway-Firmware-<TAG>-<md5 前 6 位>.img   带版本号，留档用；同名不同 md5 不可能出现
# 并在结尾打印一行「版本对照：TAG=… BUILD=… md5=… 文件=…」，与开机横幅对得上。
#
# 退出码：0 = 全部通过；非 0 = 失败（日志在 rk2206-gateway/out/）
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${GW_SDK:-$HOME/rk2206/lockzhiner-rk2206-openharmony3.0lts}"
CONT="${GW_CONTAINER:-hmprogram-rk2206-build}"
IMG="${GW_IMAGE:-swr.cn-south-1.myhuaweicloud.com/openharmony-docker/docker_oh_mini:3.2}"
TOOLCHAIN=/home/tools/gcc-arm-none-eabi-10.3-2021.10/bin
OUT="$ROOT/rk2206-gateway/out"
SRC="$ROOT/rk2206-gateway/min"
STAGE="$SDK/vendor/lockzhiner/rk2206/samples/t5_min"
SAMPLES_GN="$SDK/vendor/lockzhiner/rk2206/samples/BUILD.gn"
MAKEFILE="$SDK/device/rockchip/rk2206/sdk_liteos/Makefile"
OBJDIR=/home/openharmony/out/rk2206/lockzhiner-rk2206
LIB=gateway_min
FEAT='./t5_min:gateway_min'
SYM=min_crc16
# 版本串的**唯一真源**是 min_gw.h 的 MIN_IMAGE_TAG（t194：以前脚本里再抄一份，
# 手改一处忘另一处就会「镜像里是 v0.2、脚本按 v0.1 断言」→ 刷错版本）。
TAG="$(sed -n 's/^#define[[:space:]]\{1,\}MIN_IMAGE_TAG[[:space:]]\{1,\}"\([^"]*\)".*/\1/p' \
        "$ROOT/rk2206-gateway/min/min_gw.h" | head -n 1)"
[ -n "$TAG" ] || { echo "[gw-build-min] 从 rk2206-gateway/min/min_gw.h 读不到 MIN_IMAGE_TAG" >&2; exit 2; }
if [ -n "${GW_MIN_TAG:-}" ] && [ "$GW_MIN_TAG" != "$TAG" ]; then
    echo "[gw-build-min] GW_MIN_TAG=$GW_MIN_TAG 与 min_gw.h 的 $TAG 不一致：改版本请改 min_gw.h" >&2
    exit 2
fi
ANCHOR="$TAG"
LOGNAME=build-min

CHECK=0
for a in "$@"; do
    case "$a" in
        --check) CHECK=1 ;;
        *) echo "未知参数: $a（只支持 --check）" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
log()  { printf '\n\033[1m[gw-build-min] %s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m[gw-build-min] 失败: %s\033[0m\n' "$*" >&2; exit 1; }
dv()   { docker exec "$CONT" bash -lc "$1"; }

[ -d "$SRC" ] || fail "找不到源码目录 $SRC"

# ------------------------------ 0. 容器 ------------------------------
log "前置检查"
command -v docker >/dev/null || fail "没有 docker"
if [ "$(docker inspect -f '{{.State.Running}}' "$CONT" 2>/dev/null || echo false)" != "true" ]; then
    log "构建容器 $CONT 不在运行，尝试 docker start"
    docker start "$CONT" >/dev/null 2>&1 || fail "容器 $CONT 不存在：
  docker run -d --name $CONT -v $SDK:/home/openharmony -w /home/openharmony $IMG sleep infinity"
fi
dv "command -v gcc >/dev/null" || fail "容器里没有 gcc（主机单测要用）"
dv "[ -x $TOOLCHAIN/arm-none-eabi-gcc ]" || fail "容器里没有 $TOOLCHAIN/arm-none-eabi-gcc"
dv "mkdir -p /tmp/gw-min" >/dev/null
docker cp "$SRC/." "$CONT:/tmp/gw-min/" >/dev/null

# ------------------------------ 1. --check ------------------------------
if [ "$CHECK" = "1" ]; then
    log "① 主机单测（gcc -DMIN_HOST_TEST）"
    set +e
    dv "cd /tmp/gw-min && gcc -DMIN_HOST_TEST -I. -std=gnu99 -Wall -Wextra -Werror -O1 \
        -o /tmp/gw-min/t_min min_proto.c min_kv.c min_iotda.c test/test_min.c" \
        > "$OUT/min-host-build.log" 2>&1
    rc=$?
    set -e
    [ "$rc" = "0" ] || { tail -20 "$OUT/min-host-build.log"; fail "主机单测编译失败（-Werror）"; }
    dv "cd /tmp/gw-min && ./t_min" | tee "$OUT/min-host-test.log"
    grep -q "失败 0" "$OUT/min-host-test.log" || fail "主机单测有用例失败（见 $OUT/min-host-test.log）"

    log "② 交叉编译语法检查（arm-none-eabi-gcc -fsyntax-only）"
    # 参数**不自己拼**：原样借 hb 编 gw_plat.c 的那条命令行（-I/-D/-mcpu 全部来自
    # out/build-gateway.log）。自拼过两版都在 soc.h / lwipopts.h / arch/cc.h 上翻车，
    # 而 GW 侧那份日志本来就在仓库里（tools/gw-compile-check.sh 用的是同一招）。
    BLOG="$OUT/build-gateway.log"
    WORKDIR_IN_CONT=/home/openharmony/out/rk2206/lockzhiner-rk2206
    if [ -f "$BLOG" ] && grep -q 'ccache arm-none-eabi-gcc.*gw_plat\.c' "$BLOG"; then
        FLAGS="$(grep -m1 'ccache arm-none-eabi-gcc.*gw_plat\.c' "$BLOG" \
            | sed -e 's/^.*ccache arm-none-eabi-gcc //' \
                  -e 's| -c .*||' \
                  -e 's|-I\.\./\.\./\.\./vendor/lockzhiner/rk2206/samples/t4_gateway||' \
                  -e 's|-I\.\./\.\./\.\./|-I/home/openharmony/|g')"
        log "   编译参数来源：$(basename "$BLOG")（与 gw_plat.c 同一套）"
    else
        fail "拿不到编译参数：$BLOG 里没有完整 gcc 命令行。
                   先跑一次 bash tools/build-lock.sh bash tools/gw-build.sh gateway（它带 -v），
                   再来 --check。宁可要一次分钟级构建，也不要一堆假编译错误。"
    fi
    set +e
    dv "export PATH=$TOOLCHAIN:\$PATH; cd /tmp/gw-min && for f in min_main.c min_proto.c min_kv.c min_net.c min_iotda.c; do \
        printf '%-14s ' \"\$f\"; \
        if arm-none-eabi-gcc -fsyntax-only -Wall $FLAGS -I/tmp/gw-min \$f 2>/tmp/gw-min/err.txt; then echo OK; \
        else echo ERR; head -8 /tmp/gw-min/err.txt; fi; done" 2>&1 | tee "$OUT/min-compile-check.log"
    set -e
    grep -q "ERR" "$OUT/min-compile-check.log" && fail "交叉语法检查有文件报错（见 $OUT/min-compile-check.log）"

    log "③ 版本串两种写法都能编过（默认宏 / -D 注入）"
    # t194：min_gw.h 里的 MIN_IMAGE_TAG 用 #ifndef 兜底，所以
    #   ① 不注入也能单独编过（默认值）；
    #   ② 需要临时打一版试验镜像时，可以 -DMIN_IMAGE_TAG='"MIN-GW-v9.9-test"' 覆盖。
    # 这里把两条都真编一遍，避免哪天把 #ifndef 写丢。
    set +e
    dv "export PATH=$TOOLCHAIN:\$PATH; cd /tmp/gw-min && \
        arm-none-eabi-gcc -fsyntax-only -Wall $FLAGS -I/tmp/gw-min min_main.c && echo '   ✓ 默认 MIN_IMAGE_TAG 编过' && \
        arm-none-eabi-gcc -fsyntax-only -Wall $FLAGS -I/tmp/gw-min \
            -DMIN_IMAGE_TAG='\"MIN-GW-v9.9-test\"' min_main.c && echo '   ✓ -D 注入 MIN_IMAGE_TAG 也编过'" \
        2>&1 | tee "$OUT/min-tag-check.log"
    rc=$?
    set -e
    [ "$rc" = "0" ] || fail "版本串自检失败（见 $OUT/min-tag-check.log）"

    log "--check 通过：主机单测 + 交叉语法检查 + 版本串两种写法"
    exit 0
fi

# ------------------------------ 2. 构建 ------------------------------
[ -d "$SDK/.git" ] || fail "SDK 不存在或不是 git 仓库: $SDK（或设 GW_SDK=<SDK 路径>）"
[ -f "$SAMPLES_GN" ] && [ -f "$MAKEFILE" ] || fail "SDK 结构与预期不符（缺 samples/BUILD.gn 或 Makefile）"
docker image inspect "$IMG" >/dev/null 2>&1 || fail "本地没有镜像 $IMG"

restore() {
    log "还原 vendor 树（samples/BUILD.gn + sdk_liteos/Makefile + 暂存目录）"
    git -C "$SDK" checkout -- vendor/lockzhiner/rk2206/samples/BUILD.gn 2>/dev/null || true
    git -C "$SDK" checkout -- device/rockchip/rk2206/sdk_liteos/Makefile   2>/dev/null || true
    rm -rf "$STAGE"
    local dirty
    dirty="$(git -C "$SDK" status --porcelain -- vendor/lockzhiner/rk2206/samples device/rockchip/rk2206/sdk_liteos/Makefile | wc -l)"
    [ "$dirty" = "0" ] && log "vendor 树已还原干净" || printf '\033[1;31m[gw-build-min] vendor 树仍有 %s 处改动\033[0m\n' "$dirty" >&2
}
trap restore EXIT

git -C "$SDK" rev-parse HEAD > "$OUT/sdk-rev.txt" 2>/dev/null || echo "(非 git 检出)" > "$OUT/sdk-rev.txt"
git -C "$SDK" checkout -- vendor/lockzhiner/rk2206/samples/BUILD.gn
git -C "$SDK" checkout -- device/rockchip/rk2206/sdk_liteos/Makefile

log "暂存最小固件源码 → $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$SRC"/*.c "$SRC"/*.h "$STAGE/"
cp "$SRC/BUILD.gn" "$STAGE/BUILD.gn"
n_c="$(ls "$STAGE"/*.c | wc -l)"
[ "$n_c" = "5" ] || fail "暂存目录 .c 数量 $n_c（应为 5）"

log "启用 feature $FEAT"
if grep -q "^ *\"$FEAT\"," "$SAMPLES_GN"; then
    :
else
    sed -i "s|^\( *\)features = \[$|\1features = [\n\1    \"$FEAT\",|" "$SAMPLES_GN"
fi
grep -q "^ *\"$FEAT\"," "$SAMPLES_GN" || fail "启用 feature 失败"
n_feat="$(grep -cE '^ *"\./' "$SAMPLES_GN" || true)"
[ "$n_feat" = "1" ] || fail "features 里启用了 $n_feat 个样例（应恰好 1 个，否则别人的 main() 会混进镜像）"

log "app_LIBS = -l$LIB"
sed -i "s|^app_LIBS = .*$|app_LIBS = -l$LIB|" "$MAKEFILE"
got="$(grep -m1 '^app_LIBS' "$MAKEFILE")"
[ "$got" = "app_LIBS = -l$LIB" ] || fail "Makefile app_LIBS 未按预期写入（实际：$got）"

blog="$OUT/$LOGNAME.log"
log "hb build -f -v（日志 → $(basename "$blog")）"
set +e
docker exec "$CONT" bash -lc "export PATH=$TOOLCHAIN:\$PATH; cd /home/openharmony; time hb build -f -v" > "$blog" 2>&1
rc=$?
set -e
log "hb 退出码 $rc，日志 $(wc -l < "$blog") 行"

bad=0
[ "$rc" = "0" ] || { bad=1; printf '   ✗ hb build 退出码 %s\n' "$rc"; }
grep -q "build success" "$blog" && printf '   ✓ 日志出现 "build success"\n' || { bad=1; printf '   ✗ 日志没有 "build success"\n'; }
if grep -q "\[OHOS ERROR\]" "$blog"; then
    bad=1; printf '   ✗ 日志有 [OHOS ERROR]（前 5 条）：\n'; grep -m5 "\[OHOS ERROR\]" "$blog" | sed 's/^/       /'
else
    printf '   ✓ 无 [OHOS ERROR]\n'
fi
miss=""
for src in $(cd "$STAGE" && ls *.c | sed 's/\.c$//'); do
    grep -q "lib$LIB\.$src\.o" "$blog" || miss="$miss $src"
done
if [ -z "$miss" ]; then printf '   ✓ 5 个源文件都有编译记录\n'; else bad=1; printf '   ✗ 这些源文件没编到:%s\n' "$miss"; fi
dv "test -f $OBJDIR/libs/lib$LIB.a" && printf '   ✓ 产物 libs/lib%s.a 存在\n' "$LIB" || { bad=1; printf '   ✗ 产物 libs/lib%s.a 不存在\n' "$LIB"; }
if dv "export PATH=$TOOLCHAIN:\$PATH; arm-none-eabi-nm $OBJDIR/liteos.elf | grep -qE ' [Tt] $SYM\$'"; then
    printf '   ✓ liteos.elf 含符号 %s（已链进最终镜像）\n' "$SYM"
else
    bad=1; printf '   ✗ liteos.elf 找不到符号 %s（app_LIBS 没生效？）\n' "$SYM"
fi
if dv "strings $OBJDIR/images/Firmware.img | grep -q '$ANCHOR'"; then
    printf '   ✓ Firmware.img 内含锚点串 "%s"\n' "$ANCHOR"
else
    bad=1; printf '   ✗ Firmware.img 内找不到锚点串 "%s"\n' "$ANCHOR"
fi

img="$SDK/out/rk2206/lockzhiner-rk2206/images/Firmware.img"
if [ -f "$img" ]; then
    sz="$(stat -c%s "$img")"; md5="$(md5sum "$img" | cut -d' ' -f1)"; sha="$(sha256sum "$img" | cut -d' ' -f1)"
    elf="$(stat -c%s "$SDK/out/rk2206/lockzhiner-rk2206/liteos.elf")"
    printf '   ✓ Firmware.img %s B（liteos.elf %s B）\n     sha256=%s\n     md5=%s\n' "$sz" "$elf" "$sha" "$md5"
    { echo "target=min"; echo "feature=$FEAT"; echo "app_libs=-l$LIB"; echo "sdk_rev=$(cat "$OUT/sdk-rev.txt")";
      echo "build_log=$LOGNAME.log"; echo "firmware_img=images/Firmware.img"; echo "firmware_size=$sz";
      echo "liteos_elf_size=$elf"; echo "firmware_sha256=$sha"; echo "firmware_md5=$md5";
      echo "assert_symbol=$SYM"; echo "assert_string=$ANCHOR"; } > "$OUT/$LOGNAME.evidence.txt"
    bdate="$(date '+%Y-%m-%d %H:%M:%S')"
    fixed="$OUT/gateway-Firmware.img"
    verimg="$OUT/gateway-Firmware-$TAG-${md5:0:6}.img"
    cp -f "$img" "$fixed"
    cp -f "$img" "$verimg"
    printf '   ✓ 烧录副本 out/gateway-Firmware.img（RKDevTool 固定名）\n'
    printf '   ✓ 带版本号副本 %s\n' "${verimg#"$ROOT"/}"
    printf '\n   ★ 版本对照：TAG=%s BUILD=%s md5=%s 文件=%s\n\n' \
        "$TAG" "$bdate" "$md5" "${verimg#"$ROOT"/}"
    { echo "image_tag=$TAG"; echo "image_build=$bdate"; echo "image_versioned=${verimg#"$ROOT"/}"; } \
        >> "$OUT/$LOGNAME.evidence.txt"
else
    bad=1; printf '   ✗ 没有生成 images/Firmware.img\n'
fi
[ "$bad" = "0" ] || fail "min 构建或断言未通过（见 $blog）"
log "min 通过。证据：$OUT/$LOGNAME.evidence.txt"
