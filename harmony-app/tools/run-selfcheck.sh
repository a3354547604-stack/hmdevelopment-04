#!/usr/bin/env bash
# 无设备自检：真实 HttpApiService + Mock 传输层，跑契约 §6 的全部 18 个接口 + 4 条故障路径。
#
# 为什么要在 /tmp 里做：ArkTS 源码 import 的是 `@kit.NetworkKit`（鸿蒙专有包），
# Node 里没有。这里把 ets 源码复制一份到 /tmp，在旁边放一个同名桩包，
# 让 esbuild 把它解析掉。**仓库本体不动，桩包也不进 git。**
#
# 用法：bash harmony-app/tools/run-selfcheck.sh [输出日志路径]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
WORK="${TMPDIR:-/tmp}/hm-selfcheck"
OUT="${1:-$REPO/deliverables/02-无设备自检日志.txt}"
ESBUILD="$HOME/harmony-toolchain/esbuild/bin/esbuild"

if [ ! -x "$ESBUILD" ]; then
  echo "找不到 esbuild：$ESBUILD" >&2
  exit 1
fi

rm -rf "$WORK"
mkdir -p "$WORK/node_modules/@kit.NetworkKit" "$WORK/ets" "$WORK/tools"
cp -r "$REPO/entry/src/main/ets/." "$WORK/ets/"
cp "$REPO/tools/selfcheck-entry.ts" "$WORK/tools/"

# 桩：只导出 mock 模式永远走不到的 NetHttpClient，`http.createHttp()` 给个占位对象，
# 一旦被真正调用就抛错——避免"以为测了真实传输层"的假象。
cat > "$WORK/node_modules/@kit.NetworkKit/package.json" <<'JSON'
{ "name": "@kit.NetworkKit", "version": "0.0.0-stub", "main": "index.js" }
JSON
cat > "$WORK/node_modules/@kit.NetworkKit/index.js" <<'JS'
exports.http = {
  createHttp: function () {
    throw new Error('selfcheck: 本自检只用 Mock 传输层，不应走到真实 NetHttpClient');
  }
};
JS

"$ESBUILD" "$WORK/tools/selfcheck-entry.ts" \
  --bundle --platform=node --format=cjs --target=node18 \
  --resolve-extensions=.ets,.ts,.js \
  --loader:.ets=ts \
  --log-level=warning \
  --outfile="$WORK/selfcheck.cjs" || { echo "esbuild 打包失败" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
{
  echo "HMProgram 应用端（harmony-app）无设备自检日志"
  echo "生成时间（UTC+8）：$(TZ=Asia/Shanghai date '+%Y-%m-%d %H:%M:%S')"
  echo "Node：$(node -v)　esbuild：$("$ESBUILD" --version)"
  echo "被测代码：harmony-app/entry/src/main/ets/service/{HttpApiService,MockTransport,SessionStore}.ets"
  echo "----------------------------------------------------------------------"
  node "$WORK/selfcheck.cjs"
  echo "----------------------------------------------------------------------"
  echo "结论：以上为「真实 HttpApiService + Mock 传输层」的运行结果。"
  echo "      它证明 URL 拼接 / Authorization 头 / 错误码翻译 / 契约字段解析在无服务器时可用；"
  echo "      不代表 server/ 真实接口可用（需 server 就绪后按 README 复跑）。"
} | tee "$OUT"

echo
echo "日志已写入：$OUT"
