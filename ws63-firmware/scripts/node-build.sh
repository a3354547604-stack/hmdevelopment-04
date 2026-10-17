#!/usr/bin/env bash
#===============================================================================
# 节点固件 · 一键出四份固件（M1 厨房 / M2 卧室 / M4 门厅 / mock 演示）
#
# 产物（都在 ws63-firmware/out/）：
#   NODE_M1.fwpkg    厨房：MQ-2(ADC ch2/IO09) + AHT20(I2C1)
#   NODE_M2.fwpkg    卧室：LD6002C 跌倒雷达（UART2 + TX2 冗余通道）
#   NODE_M4.fwpkg    门厅：玄关/阳台门磁（IO00/IO02）
#   NODE_MOCK.fwpkg  无硬件演示：M1 角色 + 伪造数据（不碰 ADC/I2C/UART）
#
# 为什么必须用脚本而不是手敲 python3 build.py：
#   1) 自研源码要临时**覆盖**进 SDK 扩展点 src/application/samples/custom/
#      （samples/CMakeLists.txt 里 add_subdirectory_if_exist(custom) 无条件引入）。
#      编完必须原样还原生产源码：脚本用 trap 保证还原，中途 Ctrl-C 也不会留下半替换状态。
#   2) 每个目标的「源文件清单」是硬约束：M2 的固件里**不能**有 I2C 驱动、M1 里不能有雷达。
#      清单来自 ws63-firmware/scripts/node-targets.txt（与 tools/node-compile-check.py 同源），
#      CMakeLists.txt 由脚本按清单生成 —— 不可能出现「清单改了但 CMakeLists 忘改」。
#   3) NODE_ROLE 万一没传进构建，编译**不报错**，只会安静地又出一份 M1 固件：三块板同角色，
#      汇聚永远区分不出谁是谁。所以每次都用 grep -a 在 .fwpkg 里断言「只有本目标的角色标记」。
#
# 用法：
#   ws63-firmware/scripts/node-build.sh                 # 四个目标都编（顺序执行，约 4×5 分钟）
#   ws63-firmware/scripts/node-build.sh m2 mock         # 只编指定目标
#   SDK_DIR=/path/to/fbb_ws63-master ws63-firmware/scripts/node-build.sh
#   IMAGE=ws63wsl:latest ws63-firmware/scripts/node-build.sh
#===============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WS63_DIR="$(cd "$HERE/.." && pwd)"
REPO_ROOT="$(cd "$WS63_DIR/.." && pwd)"
SRC_DIR="$WS63_DIR/src"
OUT_DIR="$WS63_DIR/out"
TARGETS_FILE="$HERE/node-targets.txt"

SDK_DIR="${SDK_DIR:-$HOME/ws63/fbb_ws63-master}"
IMAGE="${IMAGE:-ws63wsl:latest}"

CUSTOM="$SDK_DIR/src/application/samples/custom"
FWPKG="$SDK_DIR/src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg"

say() { echo "[node-build] $*"; }
die() { echo "[node-build] 错误：$*" >&2; exit 1; }

#--------------------------------------------------------------- 前置检查
[ -d "$SDK_DIR/src" ] || die "SDK 目录不存在：$SDK_DIR（用 SDK_DIR=... 指定，见 ws63-firmware/doc/1-通宵进展与烧录方法.md）"
[ -d "$CUSTOM" ] || die "找不到 SDK 扩展点目录：$CUSTOM"
[ -f "$TARGETS_FILE" ] || die "找不到目标清单：$TARGETS_FILE"
[ -f "$SRC_DIR/node_main.c" ] || die "找不到节点源码：$SRC_DIR/node_main.c"
command -v docker >/dev/null || die "docker 不可用"
docker image inspect "$IMAGE" >/dev/null 2>&1 || die "镜像不存在：$IMAGE（应为本项目既有镜像，别另造）"
mkdir -p "$OUT_DIR"

#--------------------------------------------------------------- 读目标清单
# 行格式：目标名 | build.py 追加参数 | 源文件列表
T_NAMES=(); T_ARGS=(); T_FILES=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    name="$(echo "$line" | cut -d'|' -f1 | tr -d ' ')"
    args="$(echo "$line" | cut -d'|' -f2 | sed 's/^ *//; s/ *$//')"
    files="$(echo "$line" | cut -d'|' -f3 | tr -s ' ')"
    [ -n "$name" ] || die "清单里有空目标名：$line"
    T_NAMES+=("$name"); T_ARGS+=("$args"); T_FILES+=("$files")
done < "$TARGETS_FILE"
[ "${#T_NAMES[@]}" -gt 0 ] || die "清单里一个目标都没有"

# 命令行筛选
WANT=("$@")
if [ "${#WANT[@]}" -gt 0 ]; then
    for w in "${WANT[@]}"; do
        found=0
        for n in "${T_NAMES[@]}"; do [ "$w" = "$n" ] && found=1; done
        [ "$found" = "1" ] || die "未知目标 '$w'；可选：${T_NAMES[*]}"
    done
fi
selected() {
    [ "${#WANT[@]}" -eq 0 ] && return 0
    for w in "${WANT[@]}"; do [ "$w" = "$1" ] && return 0; done
    return 1
}

#--------------------------------------------------------------- 暂存/还原
STASH="$SDK_DIR/.node-build-stash"
RESTORED=0
restore() {
    [ "$RESTORED" = "1" ] && return 0
    RESTORED=1
    if [ -d "$STASH" ]; then
        rm -rf "$CUSTOM"
        mkdir -p "$CUSTOM"
        cp -a "$STASH/." "$CUSTOM/"
        rm -rf "$STASH"
        say "已还原 $CUSTOM（生产源码与 CMakeLists 回到编译前状态）"
    fi
}
trap restore EXIT INT TERM

rm -rf "$STASH"; mkdir -p "$STASH"
cp -a "$CUSTOM/." "$STASH/"
say "已暂存原有 custom/ → $STASH"

#--------------------------------------------------------------- 单目标构建
# $1=目标名 $2=追加参数 $3=源文件列表
build_one() {
    local name="$1" extra="$2" files="$3"
    local log="$OUT_DIR/build-$name.log"
    local kind own_tag other_tags own_marker other_marker
    local f base old_hash new_hash

    # 汇聚目标（hub/hubmock）与节点目标产出的是两种完全不同的固件：
    # 汇聚是 SLE **Central**（只扫描/连接），节点是 Peripheral（广播/服务端）。
    # 拿错包烧板比编不过更危险（两块板同角色、或汇聚去广播），所以产物名与断言都分开。
    case "$name" in hub|hubmock) kind="hub" ;; *) kind="node" ;; esac
    local dest="$OUT_DIR/$(echo "$kind" | tr 'a-z' 'A-Z')_$(echo "$name" | tr 'a-z' 'A-Z').fwpkg"

    # 角色断言用的标记串（来自 node_cfg.h 的 NODE_BUILD_TAG / hub_cfg.h 的 HUB_BUILD_TAG）
    case "$name" in
        m1)   own_marker="EC-NODE-ROLE=M1 type=gas" ;;
        m2)   own_marker="EC-NODE-ROLE=M2 type=fall" ;;
        m4)   own_marker="EC-NODE-ROLE=M4 type=door+presence" ;;
        mock) own_marker="EC-NODE-ROLE=M1 type=gas" ;;
        hub)     own_marker="HUB-ROLE=CENTRAL sn=H1 fw=0.1.0 mock=0" ;;
        hubmock) own_marker="HUB-ROLE=CENTRAL sn=H1 fw=0.1.0 mock=1" ;;
        *)    die "目标 $name 没有定义角色标记，请在 build_one 里补" ;;
    esac
    other_tags=""
    if [ "$kind" = "hub" ]; then
        # 汇聚镜像里出现任何节点角色标记 = 编进镜像的是节点代码（或反射了节点常量表）
        other_tags="EC-NODE-ROLE="
    else
        for n in "${T_NAMES[@]}"; do
            [ "$n" = "$name" ] && continue
            case "$n" in
                m2) other_tags="$other_tags EC-NODE-ROLE=M2 type=fall" ;;
                m4) other_tags="$other_tags EC-NODE-ROLE=M4 type=door+presence" ;;
            esac
        done
    fi

    # 1) 清空扩展点里的自研源码（原件在 STASH 里），只放本目标的文件
    rm -f "$CUSTOM"/*.c "$CUSTOM"/*.h "$CUSTOM"/CMakeLists.txt "$CUSTOM"/HsTest.* 2>/dev/null || true
    cp "$SRC_DIR"/*.h "$CUSTOM/"
    local cmake_sources=""
    for f in $files; do
        [ -f "$SRC_DIR/$f" ] || die "目标 $name 的源文件不存在：$SRC_DIR/$f（清单与实际源码不一致）"
        cp "$SRC_DIR/$f" "$CUSTOM/"
        cmake_sources="$cmake_sources
    \"\${CMAKE_CURRENT_SOURCE_DIR}/$f\""
    done
    [ -n "$cmake_sources" ] || die "目标 $name 没有任何源文件"
    # CMakeLists 由清单生成：脚本是唯一事实源，避免「清单改了但 CMakeLists 忘改」
    {
        echo "# 本文件由 ws63-firmware/scripts/node-build.sh 按 node-targets.txt 生成，请勿手改。"
        echo "# 目标：$name（追加参数：${extra:-无}）"
        echo "set(SOURCES \"\${SOURCES}\"$cmake_sources"
        echo "    PARENT_SCOPE)"
    } > "$CUSTOM/CMakeLists.txt"
    say "==== 目标 $name：安装 $(echo $files | wc -w) 个源文件 + CMakeLists（参数 '${extra:-无}'）"

    # 2) 编译。先删旧产物：这样「产物存在」就等于「这次真的编出来了」。
    # 只能在容器里删 —— 上次编译是 root 在容器里写的，主机 rm 会 Permission denied（踩过）。
    old_hash=$(sha256sum "$FWPKG" 2>/dev/null | cut -d' ' -f1 || true)
    if ! docker run --rm -v "$SDK_DIR":/sdk -w /sdk/src "$IMAGE" /bin/bash -c \
            "export PATH=/usr/bin:/bin:/usr/sbin:/sbin; \
             rm -f output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg; \
             python3 build.py ws63-liteos-app -c $extra" \
            >"$log" 2>&1; then
        say "编译失败，日志尾部："
        tail -25 "$log" >&2
        grep -q "PermissionError" "$log" &&
            say "提示：报 PermissionError 时按 ws63-firmware/doc/1-通宵进展与烧录方法.md 执行 chmod -R +x $SDK_DIR/src/tools/" >&2
        die "目标 $name 编译失败（完整日志：$log）"
    fi
    grep -q "packet success" "$log" || die "目标 $name 日志里没有 'packet success'，构建可能没真正完成：$log"
    [ -f "$FWPKG" ] || die "目标 $name 报成功但没产出 $FWPKG"

    new_hash=$(sha256sum "$FWPKG" | cut -d' ' -f1)
    if [ -n "$old_hash" ] && [ "$old_hash" = "$new_hash" ]; then
        die "目标 $name 产物的 sha256 与编译前完全相同（$new_hash）——这不是本次编出来的包"
    fi

    # 3) 角色断言（必须 -F：方括号在基本正则里是字符集，会退化成“命中任意一个字母”的假计数，踩过）
    local n_own n_other n_mock
    n_own=$(grep -acF -- "$own_marker" "$FWPKG" || true)
    [ "${n_own:-0}" -ge 1 ] || die "目标 $name 角色断言失败：镜像里找不到 '$own_marker'。
    说明 NODE_ROLE/目标宏没生效，或 CMakeLists 列的源文件不对（编进镜像的不是本目标代码）。"
    for other_marker in $other_tags; do
        n_other=$(grep -acF -- "$other_marker" "$FWPKG" || true)
        [ "${n_other:-0}" -eq 0 ] || die "目标 $name 角色断言失败：镜像里出现了别的角色标记 '$other_marker'×$n_other。
    绝不把这种包交给用户烧板，否则两块板可能同角色。"
    done
    if [ "$kind" = "hub" ]; then
        say "$name 角色断言通过（汇聚镜像）：'$own_marker'×$n_own，节点角色标记 0"
    elif [ "$name" = "mock" ]; then
        n_mock=$(grep -acF -- "[MOCK]" "$FWPKG" || true)
        [ "${n_mock:-0}" -ge 1 ] || die "mock 目标里找不到 '[MOCK]' 标记：伪数据源没编进去"
    else
        n_mock=$(grep -acF -- "[MOCK]" "$FWPKG" || true)
        [ "${n_mock:-0}" -eq 0 ] || die "真实目标 $name 里出现了 '[MOCK]'：mock 驱动混进了固件"
    fi
    if [ "$kind" != "hub" ]; then
        say "$name 角色断言通过：'$own_marker'×$n_own，其它角色标记 0，[MOCK]×$n_mock"
    fi

    cp "$FWPKG" "$dest"
    {
        echo "target=$name"
        echo "extra_build_args=$extra"
        echo "sources=$files"
        echo "bytes=$(stat -c %s "$dest")"
        echo "sha256=$(sha256sum "$dest" | cut -d' ' -f1)"
        echo "built_at=$(date -Iseconds)"
        echo "role_marker=$own_marker count=$n_own"
    } | tee "$OUT_DIR/$name.info.txt" >/dev/null
    say "$name 产物：$dest（$(stat -c %s "$dest") 字节）"
}

#--------------------------------------------------------------- 主流程
for i in "${!T_NAMES[@]}"; do
    n="${T_NAMES[$i]}"
    selected "$n" || continue
    build_one "$n" "${T_ARGS[$i]}" "${T_FILES[$i]}"
done

say "完成。产物与指纹见 $OUT_DIR/；烧录与验收见 ws63-firmware/doc/11-*.md"
