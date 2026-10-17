#!/usr/bin/env bash
#===============================================================================
# 双板星闪（SLE）连通性冒烟包 · 一键出两份固件
#
# 产出的两份固件（都在 ws63-firmware/spike/out/）：
#   SPIKE_MASTER.fwpkg  → 主节点板（SLE server：广播 + 每秒 notify）
#   SPIKE_SLAVE.fwpkg   → 从节点板（SLE client：扫描 + 连接 + 打印收到的每一帧）
#
# 为什么不是「一个 image 跑两种角色」：冒烟测试要的是单变量实验，角色烧死在一块板上，
# 连不上时才能立刻排除「是不是角色选错了」。见 ws63-firmware/spike/spike_role.h。
#
# 为什么必须用这个脚本而不是手敲 python3 build.py：
#   1) 冒烟源文件要临时**覆盖**进 SDK 的 src/application/samples/custom/（官方预留扩展点，
#      samples/CMakeLists.txt 里 add_subdirectory_if_exist(custom) 无条件引入），
#      编完必须把生产固件的源文件与 CMakeLists 原样还原——脚本用 trap 保证还原，
#      中途 Ctrl-C 也不会把生产源码留在半替换状态；
#   2) 角色宏来自 build.py 的 -def=SPIKE_ROLE=n。这个宏万一没传进去，编译**不会报错**，
#      只会安静地产出第二份主节点固件，两块板都广播、永远连不上。所以脚本每次都用
#      grep -a 在 .fwpkg 里断言「只有本角色的标记串」，对不上就退出且不落盘产物。
#
# 用法：
#   tools/spike-build.sh                    # 两套都编
#   tools/spike-build.sh server|client      # 只编一套
#   SDK_DIR=/path/to/fbb_ws63-master tools/spike-build.sh
#   IMAGE=ws63wsl:latest tools/spike-build.sh
#===============================================================================
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SPIKE_SRC="$REPO_ROOT/ws63-firmware/spike"
OUT_DIR="$SPIKE_SRC/out"

SDK_DIR="${SDK_DIR:-$HOME/ws63/fbb_ws63-master}"
IMAGE="${IMAGE:-ws63wsl:latest}"
WHICH="${1:-all}"

CUSTOM="$SDK_DIR/src/application/samples/custom"
FWPKG="$SDK_DIR/src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg"

say() { echo "[spike-build] $*"; }
die() { echo "[spike-build] 错误：$*" >&2; exit 1; }

#--------------------------------------------------------------- 前置检查
[ -n "$SDK_DIR" ] || die "SDK_DIR 为空"
[ -d "$SDK_DIR/src" ] || die "SDK 目录不存在：$SDK_DIR（用 SDK_DIR=... 指定，或见 ws63-firmware/doc/1-通宵进展与烧录方法.md）"
[ -d "$CUSTOM" ] || die "找不到 SDK 扩展点目录：$CUSTOM"
[ -f "$SPIKE_SRC/spike_server.c" ] || die "找不到冒烟源码：$SPIKE_SRC"
command -v docker >/dev/null || die "docker 不可用"
docker image inspect "$IMAGE" >/dev/null 2>&1 || die "镜像不存在：$IMAGE（应为本项目既有镜像，别另造）"

mkdir -p "$OUT_DIR"

#--------------------------------------------------------------- 暂存/还原
# 暂存整个 custom/（含生产 .c/.h 与 CMakeLists.txt），编完原样贴回。
# 不假设「repo 里那份一定更新」——只做无损往返，不做版本仲裁。
STASH="$SDK_DIR/.spike-stash"
RESTORED=0
restore() {
    [ "$RESTORED" = "1" ] && return 0
    RESTORED=1
    if [ -d "$STASH" ] && [ -n "$CUSTOM" ] && [ -n "$SDK_DIR" ]; then
        rm -rf "$CUSTOM"
        mkdir -p "$CUSTOM"
        cp -a "$STASH/." "$CUSTOM/"
        rm -rf "$STASH"
        say "已还原 $CUSTOM（生产固件源码与 CMakeLists 回到编译前状态）"
    fi
}
trap restore EXIT INT TERM

rm -rf "$STASH"
mkdir -p "$STASH"
cp -a "$CUSTOM/." "$STASH/"
say "已暂存生产源文件 → $STASH"

# 冒烟源码覆盖进扩展点。生产 .c 文件仍留在目录里，但冒烟版 CMakeLists.txt
# 只列 spike_*.c，它们不会被编进镜像。
cp "$SPIKE_SRC"/spike_*.c "$SPIKE_SRC"/spike_*.h "$CUSTOM/"
cp "$SPIKE_SRC"/CMakeLists.txt "$CUSTOM/CMakeLists.txt"
say "已安装冒烟源码：$(cd "$CUSTOM" && ls spike_*.c | tr '\n' ' ')"

#--------------------------------------------------------------- 编译
# $1=角色名(master/slave)  $2=build.py 追加参数
build_one() {
    local name="$1" extra="$2"
    local own_tag other_role own_role
    case "$name" in
        master) own_tag="[SPIKE-SRV]" own_role="SPIKE-ROLE=MASTER" other_role="SPIKE-ROLE=SLAVE" ;;
        slave)  own_tag="[SPIKE-CLI]" own_role="SPIKE-ROLE=SLAVE"  other_role="SPIKE-ROLE=MASTER" ;;
        *)      die "未知角色 $name" ;;
    esac
    local log="$OUT_DIR/build-$name.log"
    local dest="$OUT_DIR/SPIKE_$(echo "$name" | tr 'a-z' 'A-Z').fwpkg"
    local old_hash new_hash

    say "==== 编译 $name（extra='$extra'）→ 日志 $log"
    old_hash=$(sha256sum "$FWPKG" 2>/dev/null | cut -d' ' -f1 || true)
    # 先删旧产物：这样「产物存在」就等于「这次真的编出来了」，不会拿上一版的包冒充。
    # 注意只能在容器里删——上一次编译是 root 在容器里写的，产物属主是 root，
    # 主机上 rm 会 Permission denied（这个坑真踩过）。
    if ! docker run --rm -v "$SDK_DIR":/sdk -w /sdk/src "$IMAGE" /bin/bash -c \
            "export PATH=/usr/bin:/bin:/usr/sbin:/sbin; \
             rm -f output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg; \
             python3 build.py ws63-liteos-app -c $extra" \
            >"$log" 2>&1; then
        say "编译失败，日志尾部："
        tail -25 "$log" >&2
        grep -q "PermissionError" "$log" &&
            say "提示：报 PermissionError 时按 ws63-firmware/doc/1-通宵进展与烧录方法.md 执行 chmod -R +x $SDK_DIR/src/tools/" >&2
        die "$name 编译失败"
    fi
    [ -f "$FWPKG" ] || die "$name 编译报成功但没产出 $FWPKG"

    new_hash=$(sha256sum "$FWPKG" | cut -d' ' -f1)
    if [ -n "$old_hash" ] && [ "$old_hash" = "$new_hash" ]; then
        die "$name 产物的 sha256 与编译前完全相同（$new_hash）——这不是本次编出来的包"
    fi

    # 角色断言：镜像里必须只有本角色的标记串。
    # 用 SPIKE-ROLE=xxx 而不是 [SPIKE-SRV]/[SPIKE-CLI]——从端那句「一帧都没收到」的
    # 调试提示里合法地印着主端的标签，拿标签当判据会误杀。
    # 必须 -F：方括号在基本正则里是字符集，"[SPIKE-CLI]" 会被当成 [E-C] 区间直接报
    # "Invalid range end"，[SPIKE-SRV] 更会退化成“命中任意一个字母”的假计数（踩过）。
    local n_tag n_own n_other
    n_tag=$(grep -acF -- "$own_tag"   "$FWPKG" || true)
    n_own=$(grep -acF -- "$own_role"  "$FWPKG" || true)
    n_other=$(grep -acF -- "$other_role" "$FWPKG" || true)
    if [ "${n_tag:-0}" -lt 1 ] || [ "${n_own:-0}" -lt 1 ] || [ "${n_other:-0}" -ne 0 ]; then
        die "$name 角色断言失败：'$own_tag'×${n_tag:-0}（应≥1） '$own_role'×${n_own:-0}（应≥1）
     '$other_role'×${n_other:-0}（应=0）。说明 SPIKE_ROLE 没生效或两个角色的源码混编了
     ——绝不把这种包交给用户烧板，否则两块板是同一个角色、永远连不上。"
    fi
    say "$name 角色断言通过：'$own_tag'×$n_tag，'$own_role'×$n_own，'$other_role'×$n_other（必须为 0）"

    cp "$FWPKG" "$dest"
    {
        echo "file=$name"
        echo "extra_build_args=$extra"
        echo "bytes=$(stat -c %s "$dest")"
        echo "sha256=$(sha256sum "$dest" | cut -d' ' -f1)"
        echo "built_at=$(date -Iseconds)"
        echo "role_marker=$own_role count=$n_own"
    } | tee "$OUT_DIR/$name.info.txt"
    say "$name 产物：$dest"
}

case "$WHICH" in
    all)    build_one master ""
            build_one slave  "-def=SPIKE_ROLE=2" ;;
    server|master) build_one master "" ;;
    client|slave)  build_one slave  "-def=SPIKE_ROLE=2" ;;
    *) die "参数只能是 all / server / client，收到：$WHICH" ;;
esac

say "完成。烧录与验收见 ws63-firmware/spike/施工单-双板星闪冒烟.md"
