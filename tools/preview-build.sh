#!/usr/bin/env bash
# preview-build.sh —— 用「本机口径」构建预览产物（product=harmonyos）
#
# 为什么需要它：仓库默认 product 是 `default`（学校 OpenHarmony 工具链，API 12 整数写法），
# 这台机器只装了 HarmonyOS 26 SDK，直接构建 default 会报 sdk provider is null。
# 本脚本把 product 换成 `harmonyos`（见 harmony-app/build-profile.json5 的注释），
# 让同一份代码在本机也能出预览产物；交付/验收那边仍然用 default，不需要这个脚本。
#
# 用法：
#   bash tools/preview-build.sh            # 增量构建
#   bash tools/preview-build.sh --clean    # 先删 entry/.preview 再构建
#
# 注意：IDE 打开着同一工程时，它会占住 entry/.preview，导致构建挂住不返回。
# 先关 IDE（taskkill //IM devecostudio64.exe //F），构建完再开。
set -eu

DEVECO_HOME="${DEVECO_HOME:-C:/Program Files/Huawei/DevEco Studio}"
export DEVECO_SDK_HOME="${DEVECO_SDK_HOME:-$DEVECO_HOME/sdk}"
NODE="$DEVECO_HOME/tools/node/node.exe"
HVIGOR="$DEVECO_HOME/tools/hvigor/bin/hvigorw.js"

HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE/harmony-app"

if [ "${1:-}" = "--clean" ]; then
  echo "[preview-build] 清理 entry/.preview"
  rm -rf entry/.preview
  shift
fi

echo "[preview-build] SDK=$DEVECO_SDK_HOME  product=harmonyos"
exec "$NODE" "$HVIGOR" \
  --mode module \
  -p module=entry@default \
  -p product=harmonyos \
  -p pageType=page \
  -p compileResInc=true \
  -p previewMode=true \
  -p buildRoot=.preview \
  PreviewBuild --analyze=normal --parallel --incremental "$@"
