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
#   1) 自研源码要临时**装进** SDK 扩展点 src/application/samples/custom/
#      （samples/CMakeLists.txt:32 的 add_subdirectory_if_exist(custom) 会引入它）。
#      这个目录**不是原厂 SDK 的一部分**，所以"还原"= **清空**（不是把进来时看到的东西贴回去，
#      那会把上一次的残留固化成基线，见 doc/13 §13.6）。trap EXIT/INT/TERM 保证清空，
#      中途 Ctrl-C 也不会留下半替换状态；退出时打印残留清单并断言 0 项。
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
#
# 并发（t51）：
#   两个 node-build.sh 同时跑**不会再互删中间文件** —— 脚本最前面会先过
#   ws63-firmware/scripts/ws63-sdk-lock.sh 的 SDK 专用锁，后到的那个等着。
#   等待上限默认 1800 s（HM_WS63_SDK_LOCK_TIMEOUT 可调），超时明确报错并以 75 退出。
#   这层锁**只**管同一棵 SDK，不影响别的仓库/工程；与 tools/build-lock.sh 的数量锁是两个东西，
#   关系与嵌套顺序见 ws63-firmware/doc/13-构建串行化.md。
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

#--------------------------------------------------------------- SDK 专用锁（t51）
# 必须**碰 SDK 之前**就拿到，所以放在最前面（下面几十行就会 rm -rf 暂存目录、删 SDK 输出）。
# 为什么用 re-exec 而不是"就地加个锁"：本锁要覆盖「清空 custom/ → 装源码 → 编译 → 清空」整段，
# 而清空走 trap EXIT —— 只有让整个脚本成为一次临界区，Ctrl-C 之后的清空也还在锁内。
# 锁的细节（flock/超时/占用者记录/为什么不与别的仓库互相阻塞）见 ws63-sdk-lock.sh 文件头
# 与 ws63-firmware/doc/13-构建串行化.md。
if [ "${HM_WS63_SDK_LOCK_HELD:-}" != "1" ]; then
    SDK_LOCK_SH="$HERE/ws63-sdk-lock.sh"
    [ -f "$SDK_LOCK_SH" ] || die "缺少 SDK 专用锁脚本：$SDK_LOCK_SH
    它负责让同一棵 SDK 上的两个构建不会互删中间文件（见 ws63-firmware/doc/13-构建串行化.md）。"
    export HM_WS63_SDK_LOCK_HELD=1
    # 用 bash 显式调起：不依赖锁脚本的可执行位（仓库在 DrvFs 上，权限位不总是可靠）
    exec bash "$SDK_LOCK_SH" \
        --sdk "$SDK_DIR" \
        --label "node-build.sh $*" \
        -- "$HERE/$(basename "${BASH_SOURCE[0]}")" "$@"
fi
say "已在 SDK 专用锁内（HM_WS63_SDK_LOCK_HELD=1），继续"

#--------------------------------------------------------------- 前置检查
[ -d "$SDK_DIR/src" ] || die "SDK 目录不存在：$SDK_DIR（用 SDK_DIR=... 指定，见 ws63-firmware/doc/1-通宵进展与烧录方法.md）"
# 注意：custom/ **不是原厂 SDK 的目录**，可能是我们（或别的工具）建出来的，
# 所以这里用 mkdir -p 而不是「不存在就 die」——原厂状态下它本来就不存在。
mkdir -p "$CUSTOM"
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

#--------------------------------------------------------------- 退出时清空扩展点（t51 / F-01）
# custom/ **不是原厂 SDK 的一部分**：原厂归档 `fbb_gitee.zip`（18373 条）里
# src/application/samples/ 下只有 bt/ peripheral/ radar/ wifi/ 与 CMakeLists.txt、Kconfig，
# samples/custom 出现 0 次。它是 samples/CMakeLists.txt:32 留给我们的临时扩展点，
# 本脚本与 tools/spike-build.sh 都往里丢源码 —— 所以用完的正确状态就是**空的**。
#
# 为什么不能"把进来时看到的东西贴回去"：进来时看到的往往正是上一次的残留。把残留当"原件"
# 存下来、退出时再贴回，污染就被固化成基线 —— 2026-10-18 08:51 那次双向失败就是这么在树里
# 留下 22 项残留的（doc/13 §13.1、§13.6）。所以：进来时非空就告警+取证，退出时无条件清空。
#
# 为什么清空而不是删掉整个目录：tools/spike-build.sh 开头有 `cp -a "$CUSTOM/." "$STASH/"`，
# 目录不存在时它（set -euo pipefail）会直接失败。空目录对 SDK 是安全的：
# src/build/cmake/build_function.cmake:11 的 add_subdirectory_if_exist 要求
# `${_path}/CMakeLists.txt` 存在才会 add_subdirectory，空目录会被跳过。
STASH="$SDK_DIR/.node-build-stash"     # 只用来清掉旧版本（把残留当原件）留下的暂存目录
CUSTOM_RESIDUE_LOG="$OUT_DIR/custom-residue.log"

custom_count() { [ -d "$CUSTOM" ] && find "$CUSTOM" -mindepth 1 2>/dev/null | wc -l || echo 0; }
custom_list()  { [ -d "$CUSTOM" ] && find "$CUSTOM" -mindepth 1 -printf '%P\n' 2>/dev/null | sort || true; }
clean_custom() { mkdir -p "$CUSTOM"; find "$CUSTOM" -mindepth 1 -exec rm -rf {} + 2>/dev/null || true; }

ENTRY_COUNT="$(custom_count)"
ENTRY_LIST="$(custom_list)"
CLEANED=0
cleanup() {
    [ "$CLEANED" = "1" ] && return 0
    CLEANED=1
    clean_custom
    rm -rf "$STASH"
    local after
    after="$(custom_count)"
    {
        echo ""
        echo "--- run $(date -Iseconds 2>/dev/null || date) pid=$$ ---"
        echo "# node-build.sh 退出时的 SDK 扩展点残留清单（t51 / F-01）"
        echo "custom_dir=$CUSTOM"
        echo "expected_residue=0"
        echo "residue_count=$after"
        echo "entry_residue_count=$ENTRY_COUNT"
        if [ -n "$ENTRY_LIST" ]; then
            echo "entry_residue_list:"
            echo "$ENTRY_LIST" | sed 's/^/  /'
        fi
        if [ "$after" != "0" ]; then
            echo "residue_list:"
            custom_list | sed 's/^/  /'
        fi
    } >> "$CUSTOM_RESIDUE_LOG" 2>/dev/null || true
    if [ "$after" = "0" ]; then
        say "custom/ 已清空到原厂状态（残留 0 项；清单 $CUSTOM_RESIDUE_LOG）"
    else
        say "错误：custom/ 清空后仍有 $after 项残留：$(custom_list | tr '\n' ' ')"
        say "      残留会混进下一次构建，必须人工处理（清单 $CUSTOM_RESIDUE_LOG）"
        exit 1
    fi
}
trap cleanup EXIT INT TERM

if [ "$ENTRY_COUNT" != "0" ]; then
    say "注意：custom/ 进来时非空（$ENTRY_COUNT 项）—— 这不是原厂状态，是上次异常退出或别的工具留下的残留："
    echo "$ENTRY_LIST" | sed 's/^/        /'
    say "      本次先清空再构建，退出时也清空；上面这份清单已记入 $CUSTOM_RESIDUE_LOG 供追溯。"
fi
rm -rf "$STASH"
clean_custom

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

    # 1) 装本目标的文件。上面已把扩展点清空，所以这里只需要 cp ——
    #    「进来时看到的残留」不会被留下，也不会在退出时被贴回（t51 / F-01）。
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
    # 同时让**容器自己**把开工时刻与产物 mtime 打出来（[fresh] 行）：三个数字同源于容器时钟，
    # 不受 WSL/宿主时钟漂移影响，用来证明「这个包是本次编出来的」。
    old_hash=$(sha256sum "$FWPKG" 2>/dev/null | cut -d' ' -f1 || true)
    if ! docker run --rm -v "$SDK_DIR":/sdk -w /sdk/src "$IMAGE" /bin/bash -c \
            "export PATH=/usr/bin:/bin:/usr/sbin:/sbin; \
             T0=\$(date +%s); \
             rm -f output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg; \
             python3 build.py ws63-liteos-app -c $extra; RC=\$?; \
             echo \"[fresh] start=\$T0 file=\$(stat -c %Y output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg 2>/dev/null || echo none) rc=\$RC\"; \
             exit \$RC" \
            >"$log" 2>&1; then
        say "编译失败，日志尾部："
        tail -25 "$log" >&2
        grep -q "PermissionError" "$log" &&
            say "提示：报 PermissionError 时按 ws63-firmware/doc/1-通宵进展与烧录方法.md 执行 chmod -R +x $SDK_DIR/src/tools/" >&2
        die "目标 $name 编译失败（完整日志：$log）"
    fi
    grep -q "packet success" "$log" || die "目标 $name 日志里没有 'packet success'，构建可能没真正完成：$log"
    [ -f "$FWPKG" ] || die "目标 $name 报成功但没产出 $FWPKG"

    # 「是不是本次编出来的」**不能**用「sha256 变了」判断：本项目的 .fwpkg 是**可复现**的 ——
    # 连续两次编同一个目标会得到逐位相同的包（实测 NODE_M2 两次相同、HUB_HUB 三次相同）。
    # 拿 sha256 变化当判据，会在「上一轮编的正好是同一个目标」时**假报失败**（t51 实测踩到：
    # 并发跑 m1&m2 时谁先拿到锁是随机的，m2 先跑且上一轮也是 m2 → old==new → 误判为"陈旧包"）。
    # 真证据 = 容器里已 rm -f 旧包 + 编完还在 + 产物 mtime ≥ 容器内的开工时刻。
    new_hash=$(sha256sum "$FWPKG" | cut -d' ' -f1)
    local fresh_line f_start f_file
    fresh_line=$(grep -o '\[fresh\].*' "$log" | tail -1 || true)
    f_start=$(echo "$fresh_line" | sed -n 's/.*start=\([0-9]\{1,\}\).*/\1/p')
    f_file=$(echo "$fresh_line" | sed -n 's/.*file=\([0-9]\{1,\}\).*/\1/p')
    [ -n "$f_start" ] && [ -n "$f_file" ] ||
        die "目标 $name 编译日志里没有可用的 [fresh] 行（构建没按预期收尾，或产物没产出）：$log"
    [ "$f_file" -ge "$f_start" ] ||
        die "目标 $name 产物比本次开工时刻还旧（容器内 file=$f_file < start=$f_start）——这不是本次编出来的包"
    if [ -n "$old_hash" ] && [ "$old_hash" = "$new_hash" ]; then
        say "$name 产物与编译前逐位相同（$new_hash）—— 可复现构建，属正常（说明见 doc/13 §13.8）"
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

    # F-05：mock 目标按 -def=NODE_ROLE=1,NODE_MOCK=1 编译，它的角色标记串与真 M1 **逐字相同**
    # （NODE_BUILD_TAG 只由 SN 与 type 拼成），单看 info.txt 的 role_marker 会把 mock 包当成
    # 真 M1 包 —— 现场拿错包烧板。这里在 mock 的 role_marker 后追加 mock=1 做区分；
    # 镜像里另有 '[MOCK]' 标记可查（下面已断言 true 目标必须 ×0、mock 必须 ≥1）。
    local marker_suffix=""
    if [ "$name" = "mock" ]; then
        marker_suffix=" mock=1"
    fi
    cp "$FWPKG" "$dest"
    {
        echo "target=$name"
        echo "extra_build_args=$extra"
        echo "sources=$files"
        echo "bytes=$(stat -c %s "$dest")"
        echo "sha256=$(sha256sum "$dest" | cut -d' ' -f1)"
        echo "built_at=$(date -Iseconds)"
        echo "role_marker=$own_marker count=$n_own$marker_suffix"
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
