#!/usr/bin/env bash
# 无设备自检：真实 HttpApiService + Mock 传输层，跑契约 §6 的全部 18 个接口 + 4 条故障路径。
#
# 为什么要在 /tmp 里做：ArkTS 源码 import 的是 `@kit.NetworkKit`（鸿蒙专有包），
# Node 里没有。这里把 ets 源码复制一份到 /tmp，在旁边放一个同名桩包，
# 让 esbuild 把它解析掉。**仓库本体不动，桩包也不进 git。**
#
# 用法：bash harmony-app/tools/run-selfcheck.sh [输出日志路径]
#
# 退出码契约（t65 修复）：**自检有失败项时本脚本必须 exit≠0**。
# 以前这里踩了一个假绿：`selfcheck-entry.ts` 设了 `process.exitCode=1`，但脚本用管道
# `{ … } | tee` 输出日志、管道之后还有 `echo`，于是调用方拿到的 `$?` 永远是最后一个
# `echo` 的 0 —— 「门禁绿了」变成假绿。现在取管道中 node 的 `PIPESTATUS[0]` 透传，
# 并把该退出码写进日志（通过/失败两条路径都留证）。
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
} > "$OUT"

# 自检本体。**必须直接取 node 的退出码**：以前把 node 放在 `{ …; } | tee` 的中间，
# 组的状态取的是末尾 echo 的 0，于是自检崩了/失败了脚本仍报绿（t65 修的假绿）。
node "$WORK/selfcheck.cjs" 2>&1 | tee -a "$OUT"
SC_EXIT="${PIPESTATUS[0]}"

{
  echo "----------------------------------------------------------------------"
  echo "结论：以上为「真实 HttpApiService + Mock 传输层」的运行结果。"
  echo "      它证明 URL 拼接 / Authorization 头 / 错误码翻译 / 契约字段解析在无服务器时可用；"
  echo "      不代表 server/ 真实接口可用（需 server 就绪后按 README 复跑）。"
  echo "自检退出码：$SC_EXIT（0 = 全部通过；非 0 = 有失败项，此时本脚本也以该码退出）"
} | tee -a "$OUT"

echo
if [ "$SC_EXIT" -ne 0 ]; then
  echo "自检未通过：退出码 $SC_EXIT（日志：$OUT）" >&2
  exit "$SC_EXIT"
fi
echo "日志已写入：$OUT"
