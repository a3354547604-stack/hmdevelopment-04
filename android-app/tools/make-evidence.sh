#!/usr/bin/env bash
# t38（收口 t26 F-01/F-02）：把「本机真实跑过的证据」落进仓库，别只留在 /tmp。
#
# 为什么需要它：BUILD-LOG §0 原来指向 /tmp/final-test.log、/tmp/final-asm.log，
# 而 /tmp 一重启就没了——复核者只能自己重建一遍。本脚本每次运行产出一份
# **带提交号 + UTC 时点** 的快照，存进 android-app/docs/evidence/ 随仓库走；
# 数字（字段数/响应数）也随之动态产出，不会再出现「文档里的数字对不上当前树」
# （F-02：数字是时点的函数，判断标准是退出码 0）。
#
# 用法：
#   bash android-app/tools/make-evidence.sh                # 只跑不需要 SDK 的检查（秒级）
#   bash android-app/tools/make-evidence.sh --with-gradle  # 追加单测 + assembleDebug（需 JDK/SDK，走 tools/build-lock.sh）
#
# 输出：android-app/docs/evidence/evidence-<commit>-<UTC>.md（同时打印到 stdout）
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT"

WITH_GRADLE=0
ARGS_DISPLAY=""
if [ "${1:-}" = "--with-gradle" ]; then WITH_GRADLE=1; ARGS_DISPLAY=" --with-gradle"; fi

COMMIT=$(git rev-parse --short HEAD)
COMMIT_FULL=$(git rev-parse HEAD)
BRANCH=$(git rev-parse --abbrev-ref HEAD)
TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="android-app/docs/evidence/evidence-${COMMIT}-${TS}.md"
mkdir -p "$(dirname "$OUT")"

# run <说明> <命令...>：把命令、输出、退出码写进快照
run() {
    local desc="$1"; shift
    echo "### $desc"
    echo
    echo '```console'
    echo "\$ $*"
    "$@" 2>&1
    local rc=$?
    echo '```'
    echo
    echo "**退出码**：$rc"
    echo
    return $rc
}

{
    echo "# 证据快照（本机实测 · 自动生成）"
    echo
    echo "> 由 \`bash android-app/tools/make-evidence.sh${ARGS_DISPLAY}\` 生成——**不要手改**，重跑即可。"
    echo "> 生成时点（UTC）：\`${TS}\` ／ 提交：\`${COMMIT_FULL}\`（\`${COMMIT}\`）／ 分支：\`${BRANCH}\`"
    echo
    echo "## 0. 工作区状态（快照绑定的是哪个版本）"
    echo
    echo '```console'
    echo "\$ git status --porcelain -- android-app -- ':!android-app/docs/evidence'"
    dirty=$(git status --porcelain -- android-app -- ':!android-app/docs/evidence')
    if [ -z "$dirty" ]; then echo "（空 → 快照绑定的是提交 ${COMMIT}，不是脏工作区）"; else echo "$dirty"; echo "（↑ 非空 → 本次快照含未提交改动，复核时以这些改动为准）"; fi
    echo '```'
    echo "> 本目录自身（\`android-app/docs/evidence/\`）不入上面这条 git status——快照记录的是「除它以外」的工作区状态。"
    echo

    echo "## 1. 契约同步检查（不需要 Android SDK，秒级）"
    echo
    run "check_error_codes.py：码集合 + code=0 登记 + 配网码操作指引（**不比对文案文本**，措辞差异只提示）" \
        python3 android-app/tools/check_error_codes.py
    run "check_local_api.py：§12 接口集合 / 请求字段 / 响应字段" \
        python3 android-app/tools/check_local_api.py
    run "check_local_api.py --self-test：正向样例 rc=0、负样例 rc=1、可选链样例 rc=1" \
        python3 android-app/tools/check_local_api.py --self-test
    run "check_local_api.py --js 现场样例：真 H5 写法（可选链/简写属性/await fetch().json()）" \
        python3 android-app/tools/check_local_api.py --js android-app/tools/h5_samples/good.html
    echo

    echo "## 2. 构建产物（APK）"
    echo
    APK="android-app/dist/eldercare-provision-0.1.0-debug.apk"
    echo '```console'
    if [ -f "$APK" ]; then
        echo "\$ ls -l $APK && sha256sum $APK"
        ls -l "$APK"
        sha256sum "$APK"
        echo
        echo "（dist/ 被根 .gitignore 忽略：APK 是构建产物，不入库；重跑 §3 的 assembleDebug 即可再生）"
    else
        echo "$APK 不存在——跑 android-app/tools/make-evidence.sh --with-gradle 生成"
    fi
    echo '```'
    echo

    if [ "$WITH_GRADLE" = "1" ]; then
        echo "## 3. 单元测试与出包（gradle，经 tools/build-lock.sh 串行）"
        echo
        export JAVA_HOME="${JAVA_HOME:-$HOME/toolchains/jdk17}"
        export ANDROID_HOME="${ANDROID_HOME:-$HOME/toolchains/android-sdk}"
        export PATH="$JAVA_HOME/bin:$PATH"
        G="${GRADLE_BIN:-$HOME/toolchains/gradle-8.7/bin/gradle}"
        INIT="$ROOT/android-app/tools/wsl-build-dir.init.gradle"
        LOG=$(mktemp)
        echo '```console'
        echo "\$ tools/build-lock.sh $G -I android-app/tools/wsl-build-dir.init.gradle -p android-app testDebugUnitTest --rerun assembleDebug --console=plain --max-workers=2"
        set +e
        tools/build-lock.sh "$G" -I "$INIT" -p android-app testDebugUnitTest --rerun assembleDebug \
            --console=plain --max-workers=2 >"$LOG" 2>&1
        grc=$?
        set -e
        grep -E "^(> Task :app:(testDebugUnitTest|assembleDebug|compileDebug(Kotlin|UnitTestKotlin))|BUILD|FAILURE|[0-9]+ actionable)" "$LOG" || true
        echo '```'
        echo
        echo "**gradle 退出码**：${grc}（完整日志 $(( $(wc -l <"$LOG") )) 行，未入库；重跑上面这条命令即可）"
        echo
        XML_DIR="$HOME/eldercare-build/_app/test-results/testDebugUnitTest"
        if ls "$XML_DIR"/*.xml >/dev/null 2>&1; then
            echo "JUnit XML 汇总（\`$XML_DIR\`，mtime $(date -u -r "$(ls -t "$XML_DIR"/*.xml | head -1)" +%Y-%m-%dT%H:%M:%SZ)）："
            echo
            python3 - "$XML_DIR" <<'PY'
import glob, os, sys, xml.etree.ElementTree as ET
d = sys.argv[1]
tot = fail = skip = 0
print("| 测试类 | 用例 | 失败 | 跳过 |")
print("|---|---|---|---|")
for p in sorted(glob.glob(os.path.join(d, "*.xml"))):
    r = ET.parse(p).getroot()
    t, f, s = int(r.get("tests", 0)), int(r.get("failures", 0)) + int(r.get("errors", 0)), int(r.get("skipped", 0))
    tot += t; fail += f; skip += s
    print(f"| `{os.path.basename(p)[5:-4]}` | {t} | {f} | {s} |")
print(f"| **合计** | **{tot}** | **{fail}** | **{skip}** |")
PY
            echo
        fi
        rm -f "$LOG"
        "$G" --stop >/dev/null 2>&1 || true
    else
        echo "## 3. 单元测试与出包"
        echo
        echo "本次未跑（未加 \`--with-gradle\`）。要在快照里带上单测与出包证据："
        echo
        echo '```bash'
        echo "bash android-app/tools/make-evidence.sh --with-gradle"
        echo '```'
        echo
    fi

    echo "## 4. 读这份快照时的注意"
    echo
    echo "- **判断标准是退出码 0**：字段数/响应数这类数字随时点变化（契约 v1.2 增字段、脚本演进都会改），不要拿数字逐字比对旧文档。"
    echo "- 契约同步检查只比 **码集合/字段名集合**；文案文本不比对（见 \`check_error_codes.py\` 头部「覆盖范围」）。"
    echo "- 真机安装、\`WifiNetworkSpecifier\` 连 AP、系统权限、真网关 B2 全流程仍需用户上真机执行（BUILD-LOG §5）。"
} >"$OUT"

echo "已写入 $OUT"
echo
cat "$OUT"
