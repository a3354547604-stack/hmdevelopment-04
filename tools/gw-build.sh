#!/usr/bin/env bash
# =============================================================================
# tools/gw-build.sh — 在 Docker 里编 RK2206 镜像（t4 网关任务的可复现构建入口）
#
# 目标：
#   hello   官方 b6_uart0 样例 → 证明"环境 + 工具链 + hb 流程"整条通（t4 交付物之一）
#   gateway 本仓库 rk2206-gateway/src 全量网关固件
#
# ── 为什么要改 vendor 树，又为什么必须还原 ──────────────────────────────────
# 这个 SDK 有**两个**必须动的开关，只动一个的话代码会被编成 .a 却**不被链进镜像**：
#   ①  vendor/lockzhiner/rk2206/samples/BUILD.gn 的 features 里启用我们的目标
#       （lite_component("app") 默认 features 全注释，不启用就不编）；
#   ②  device/rockchip/rk2206/sdk_liteos/Makefile 第 51 行 `app_LIBS = ` 要写上
#       `-l我们的库`。这一行默认是空的，而最终 liteos.elf 的链接命令是
#       `-Wl,--whole-archive $(boot_LIBS) $(hardware_LIBS) $(hdf_LIBS) $(app_LIBS) ...`，
#       空 -> 我们的 .a 根本没进链接。实测依据：只做①时
#       `nm liteos.elf | grep -c ' uart_example$'` = 0、`strings Firmware.img | grep -c HelloWorld` = 0；
#       补上②后两者都变成 1。SDK 的 README 从没提这一行（grep 全文无 app_LIBS），
#       所以这是本脚本存在的核心理由，也是为了让别人能从零复现同一张固件。
# 纪律：两个文件都先 git checkout 成原版 → 打**单行**补丁 → 构建 → **trap 里无条件还原**，
#       收尾断言这两处 `git status` 干净：对外的事实是"vendor 树一个字没改"。
#
# 用法：
#   tools/gw-build.sh hello    [--clean] [--keep]
#   tools/gw-build.sh gateway  [--clean] [--keep]
#   tools/gw-build.sh both     [--clean]
#   --clean  构建前清空 out/rk2206/lockzhiner-rk2206（"从零编译"的日志，耗时长）
#   --keep   构建后不还原 vendor 树（仅排障；验收不要加）
#
# 退出码：0 = 构建 + 全部断言通过；非 0 = 任一步失败（日志与证据都落在 rk2206-gateway/out/）
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDK="${GW_SDK:-$HOME/rk2206/lockzhiner-rk2206-openharmony3.0lts}"
CONT="${GW_CONTAINER:-hmprogram-rk2206-build}"
IMG="${GW_IMAGE:-swr.cn-south-1.myhuaweicloud.com/openharmony-docker/docker_oh_mini:3.2}"
TOOLCHAIN=/home/tools/gcc-arm-none-eabi-10.3-2021.10/bin
OUT="$ROOT/rk2206-gateway/out"
STAGE="$SDK/vendor/lockzhiner/rk2206/samples/t4_gateway"
SAMPLES_GN="$SDK/vendor/lockzhiner/rk2206/samples/BUILD.gn"
MAKEFILE="$SDK/device/rockchip/rk2206/sdk_liteos/Makefile"
OBJDIR=/home/openharmony/out/rk2206/lockzhiner-rk2206

MODE="${1:-}"
case "$MODE" in hello|gateway|both) ;; *) echo "用法: $0 {hello|gateway|both} [--clean] [--keep]" >&2; exit 2 ;; esac
shift || true
CLEAN=0; KEEP=0
for a in "$@"; do
    case "$a" in
        --clean) CLEAN=1 ;;
        --keep)  KEEP=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

mkdir -p "$OUT"
log()  { printf '\n\033[1m[gw-build] %s\033[0m\n' "$*"; }
fail() { printf '\n\033[1;31m[gw-build] 失败: %s\033[0m\n' "$*" >&2; exit 1; }
dv()   { docker exec "$CONT" bash -lc "$1"; }

# ------------------------------ 0. 前置检查 ------------------------------
log "前置检查"
[ -d "$SDK/.git" ] || fail "SDK 不存在或不是 git 仓库: $SDK（或设 GW_SDK=<SDK 路径>）"
[ -f "$SAMPLES_GN" ] || fail "找不到 $SAMPLES_GN"
[ -f "$MAKEFILE" ] || fail "找不到 $MAKEFILE"
command -v docker >/dev/null || fail "没有 docker"
docker image inspect "$IMG" >/dev/null 2>&1 || fail "本地没有镜像 $IMG（先 docker pull $IMG）"
if [ "$(docker inspect -f '{{.State.Running}}' "$CONT" 2>/dev/null || echo false)" != "true" ]; then
    log "构建容器 $CONT 不在运行，尝试 docker start"
    docker start "$CONT" >/dev/null 2>&1 || fail "容器 $CONT 不存在，先用下面这条重建：
  docker run -d --name $CONT -v $SDK:/home/openharmony -w /home/openharmony $IMG sleep infinity"
fi
dv "command -v hb >/dev/null"            || fail "容器里没有 hb（先 ./build/prebuilts_download.sh + pip3 install build/lite）"
dv "[ -x $TOOLCHAIN/arm-none-eabi-gcc ]" || fail "容器里没有 $TOOLCHAIN/arm-none-eabi-gcc（工具链没进 PATH 就是首次构建报 ccache: Could not find compiler 的原因）"

git -C "$SDK" rev-parse HEAD > "$OUT/sdk-rev.txt" 2>/dev/null || echo "(非 git 检出)" > "$OUT/sdk-rev.txt"
dv 'cat /home/openharmony/ohos_config.json' > "$OUT/ohos_config.json.snapshot" 2>/dev/null || true
log "SDK rev: $(cat "$OUT/sdk-rev.txt")"

# ------------------------------ 1. 还原 / 打补丁 ------------------------------
restore() {
    if [ "$KEEP" = "1" ]; then log "--keep：保留 vendor 补丁与暂存目录（排障模式，验收请去掉）"; return; fi
    log "还原 vendor 树（samples/BUILD.gn + sdk_liteos/Makefile + 暂存目录）"
    git -C "$SDK" checkout -- vendor/lockzhiner/rk2206/samples/BUILD.gn 2>/dev/null || true
    git -C "$SDK" checkout -- device/rockchip/rk2206/sdk_liteos/Makefile   2>/dev/null || true
    rm -rf "$STAGE"
    local dirty
    dirty="$(git -C "$SDK" status --porcelain -- vendor/lockzhiner/rk2206/samples device/rockchip/rk2206/sdk_liteos/Makefile | wc -l)"
    if [ "$dirty" = "0" ]; then
        log "vendor 树已还原干净（git status 无改动）"
    else
        printf '\033[1;31m[gw-build] vendor 树仍有 %s 处改动，请人工检查\033[0m\n' "$dirty" >&2
    fi
}
trap restore EXIT

reset_vendor() {
    git -C "$SDK" checkout -- vendor/lockzhiner/rk2206/samples/BUILD.gn
    git -C "$SDK" checkout -- device/rockchip/rk2206/sdk_liteos/Makefile
    grep -q "^app_LIBS = $" "$MAKEFILE" || fail "Makefile 的 app_LIBS 不是空行原样（SDK 版本变了？）"
    grep -q "^        # *\"\./b6_uart0:uart0_example\"," "$SAMPLES_GN" || fail "samples/BUILD.gn 的 b6_uart0 特征行不在预期位置（SDK 版本变了？）"
}

enable_feature() {
    local feat="$1"
    # 三种情况，缺一不可：
    #   ① SDK 自带的样例（hello 目标用）：文件里有一行被注释掉的 `#"./b6_uart0:..."`，
    #      把它解注释。**不能**用"追加一行"代替——否则 features 里会出现两个样例，
    #      固件里混进别人的 main()，此时"构建成功"反而变成假绿灯。
    #   ② 本仓库自建的目标（gateway 目标用）：SDK 里根本没有 t4_gateway 这一行，
    #      所以只能插到 `features = [` 之后一行。原脚本只做①，gateway 目标直接
    #      以「启用 feature 失败」收场（t4 首次跑就撞上）。
    #   ③ 已经启用：不必再动（脚本应可重复执行）。
    if grep -q "^ *\"$feat\"," "$SAMPLES_GN"; then
        log "feature $feat 已启用"
    elif grep -q "^ *# *\"$feat\"," "$SAMPLES_GN"; then
        sed -i "s|^\( *\)# *\"$feat\",|\1\"$feat\",|" "$SAMPLES_GN"
    else
        sed -i "s|^\( *\)features = \[$|\1features = [\n\1    \"$feat\",|" "$SAMPLES_GN"
    fi
    grep -q "^ *\"$feat\"," "$SAMPLES_GN" || fail "启用 feature $feat 失败"
    # 断言"恰好一个"：多一个是上面①说的假绿灯，少一个说明 sed 没落地。
    local n; n="$(grep -cE '^ *"\./' "$SAMPLES_GN" || true)"
    [ "$n" = "1" ] || fail "features 里启用了 $n 个样例（应恰好 1 个）"
}

# app_LIBS 必须恰好是拿到的那个值：--whole-archive 会把整库拉进镜像，
# 留空 = 我们的代码不进镜像（本脚本注释①/②说的就是这个坑）。
set_app_libs() {
    local lib="$1"
    sed -i "s|^app_LIBS = .*$|app_LIBS = -l$lib|" "$MAKEFILE"
    local got; got="$(grep -m1 '^app_LIBS' "$MAKEFILE")"
    [ "$got" = "app_LIBS = -l$lib" ] || fail "Makefile app_LIBS 未按预期写入（实际：$got）"
}

# ------------------------------ 2. 构建一个目标 ------------------------------
build_one() {
    local target="$1" feat="$2" lib="$3" logname="$4" sym="$5" anchor="$6"
    local blog="$OUT/$logname.log"

    log "== 目标 $target =="
    reset_vendor
    rm -rf "$STAGE"

    if [ "$target" = "gateway" ]; then
        log "暂存网关源码 → $STAGE"
        mkdir -p "$STAGE"
        cp "$ROOT"/rk2206-gateway/src/*.c "$ROOT"/rk2206-gateway/src/*.h "$STAGE/"
        cp "$ROOT/rk2206-gateway/BUILD.gn" "$STAGE/BUILD.gn"
        local n_c n_h
        n_c="$(ls "$STAGE"/*.c | wc -l)"; n_h="$(ls "$STAGE"/*.h | wc -l)"
        [ "$n_c" = "12" ] || fail "暂存目录 .c 数量 $n_c（应为 12）"
        [ "$n_h" = "12" ] || fail "暂存目录 .h 数量 $n_h（应为 12）"
    fi
    enable_feature "$feat"
    set_app_libs "$lib"

    if [ "$CLEAN" = "1" ]; then
        log "$target: 清空 $OBJDIR（从零编译）"
        dv "rm -rf $OBJDIR"
    fi

    # -v 必须留着：ninja 只有在 verbose 时才把每条完整的 gcc 命令行打进日志，
    # 而 tools/gw-compile-check.sh 就是靠日志里那条命令行「原样借用」hb 的全部
    # -I/-D/-mcpu 参数（自己再拼一套必然漏 SDK 路径，实测漏到过 MQTTClient.h 都找不到）。
    # 代价只是日志变大（几 MB，out/ 不进 git），换来改一行代码 2 秒自检而不是 1 分钟。
    log "$target: hb build -f -v（日志 → $(basename "$blog")）"
    set +e
    docker exec "$CONT" bash -lc "export PATH=$TOOLCHAIN:\$PATH; cd /home/openharmony; time hb build -f -v" > "$blog" 2>&1
    local rc=$?
    set -e
    log "$target: hb 退出码 $rc，日志 $(wc -l < "$blog") 行"

    # ------------------------------ 3. 断言 ------------------------------
    local bad=0
    [ "$rc" = "0" ] || { bad=1; printf '   ✗ hb build 退出码 %s\n' "$rc"; }
    if grep -q "build success" "$blog"; then printf '   ✓ 日志出现 "build success"\n'; else bad=1; printf '   ✗ 日志没有 "build success"\n'; fi
    if grep -q "\[OHOS ERROR\]" "$blog"; then
        bad=1; printf '   ✗ 日志有 [OHOS ERROR]（前 5 条）：\n'; grep -m5 "\[OHOS ERROR\]" "$blog" | sed 's/^/       /'
    else
        printf '   ✓ 无 [OHOS ERROR]\n'
    fi
    if grep -q "ccache: error" "$blog"; then bad=1; printf '   ✗ 出现 ccache: Could not find compiler（PATH 没带上工具链）\n'; fi

    # 每个源文件都真的编了（防止"以为编了、其实是上次的 .o"）
    if [ "$target" = "gateway" ]; then
        local miss="" src
        for src in gw_main gw_proto gw_rules gw_iotda gw_cloud gw_cache gw_timesync gw_crypto gw_time gw_alarm gw_433 gw_plat; do
            grep -q "libgateway_app\.$src\.o" "$blog" || miss="$miss $src"
        done
        if [ -z "$miss" ]; then printf '   ✓ 12 个源文件都有编译记录\n'; else bad=1; printf '   ✗ 这些源文件没编到:%s\n' "$miss"; fi
    fi

    if dv "test -f $OBJDIR/libs/lib$lib.a"; then printf '   ✓ 产物 libs/lib%s.a 存在\n' "$lib"; else bad=1; printf '   ✗ 产物 libs/lib%s.a 不存在\n' "$lib"; fi

    # 最强断言：符号进了最终 ELF（--whole-archive 生效）+ 整串出现在固件镜像里
    if dv "export PATH=$TOOLCHAIN:\$PATH; arm-none-eabi-nm $OBJDIR/liteos.elf | grep -qE ' [Tt] $sym\$'"; then
        printf '   ✓ liteos.elf 含符号 %s（已链进最终镜像）\n' "$sym"
    else
        bad=1; printf '   ✗ liteos.elf 找不到符号 %s（app_LIBS 没生效？）\n' "$sym"
    fi
    if dv "strings $OBJDIR/images/Firmware.img | grep -q '$anchor'"; then
        printf '   ✓ Firmware.img 内含锚点串 "%s"\n' "$anchor"
    else
        bad=1; printf '   ✗ Firmware.img 内找不到锚点串 "%s"\n' "$anchor"
    fi

    local img="$SDK/out/rk2206/lockzhiner-rk2206/images/Firmware.img"
    if [ -f "$img" ]; then
        local sha md5 sz elf
        sha="$(sha256sum "$img" | cut -d' ' -f1)"; md5="$(md5sum "$img" | cut -d' ' -f1)"; sz="$(stat -c%s "$img")"
        elf="$(stat -c%s "$SDK/out/rk2206/lockzhiner-rk2206/liteos.elf")"
        printf '   ✓ Firmware.img %s B（liteos.elf %s B）\n     sha256=%s\n     md5=%s\n' "$sz" "$elf" "$sha" "$md5"
        { echo "target=$target"; echo "feature=$feat"; echo "app_libs=-l$lib"; echo "sdk_rev=$(cat "$OUT/sdk-rev.txt")";
          echo "build_log=$logname.log"; echo "firmware_img=images/Firmware.img"; echo "firmware_size=$sz";
          echo "liteos_elf_size=$elf"; echo "firmware_sha256=$sha"; echo "firmware_md5=$md5";
          echo "assert_symbol=$sym"; echo "assert_string=$anchor"; } > "$OUT/$logname.evidence.txt"
        sha256sum "$img" > "$OUT/$logname.sha256"
        # 另存一份「烧录用」的副本到仓库 out/：RKDevTool/flash.py 只能在 Windows 上跑，
        # 而 SDK 输出目录在 WSL 的 home 里，用户得先找到它才能烧。副本名去掉 build- 前缀。
        cp -f "$img" "$OUT/${logname#build-}-Firmware.img"
        printf '   ✓ 烧录副本 out/%s-Firmware.img（sha256 同上）\n' "${logname#build-}"

    else
        bad=1; printf '   ✗ 没有生成 images/Firmware.img\n'
    fi

    [ "$bad" = "0" ] || fail "$target 构建或断言未通过（见 $blog）"
    log "$target 通过"
}

case "$MODE" in
    hello)   build_one hello   './b6_uart0:uart0_example'  'uart0_example' 'build-hello'   'uart_example'     'HelloWorld!' ;;
    gateway) build_one gateway './t4_gateway:gateway_app'  'gateway_app'   'build-gateway' 'gw_crc16_modbus'  'ELDERCARE-GW-M0-v1.2' ;;
    both)    build_one hello   './b6_uart0:uart0_example'  'uart0_example' 'build-hello'   'uart_example'     'HelloWorld!'
             build_one gateway './t4_gateway:gateway_app'  'gateway_app'   'build-gateway' 'gw_crc16_modbus'  'ELDERCARE-GW-M0-v1.2' ;;
esac

log "全部通过。证据目录：$OUT"
ls -la "$OUT"
