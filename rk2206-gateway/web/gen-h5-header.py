#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
web/gen-h5-header.py — 由唯一真源 web/index.html 生成 src/gw_h5.h

为什么要生成而不是直接写头文件：
  H5 配网页（契约 §12.11，P0 主路径）有两个消费方：
    ① 固件：必须作为 C 字符串编进镜像（页面要和网关同生共死，现场没有网、没有 CDN）；
    ② 核对工具：t27 的 tools/check_local_api.py --js <html> 需要**独立 HTML 文件**
       才能机器核对字段与 §12 接口。
  两个消费方要的形态不同。手写两份必然漂移（改了页面忘了改头文件 —— 这种错在板上
  只表现为"手机看到的还是上一版"，最难查），所以：web/index.html 是唯一真源，
  gw_h5.h 是生成物，改了真源就跑这个脚本。

用法：
  python3 rk2206-gateway/web/gen-h5-header.py            # 由 index.html 重新生成 gw_h5.h
  python3 rk2206-gateway/web/gen-h5-header.py --check     # 只校验生成物与真源一致（CI/编译自检用）
退出码：0 = 一致/已生成；1 = --check 下不一致；2 = 真源缺失或读不了
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GATEWAY = os.path.dirname(HERE)
SRC_HTML = os.path.join(HERE, "index.html")
DST_H = os.path.join(GATEWAY, "src", "gw_h5.h")

HEADER_COMMENT = """/*
 * gw_h5.h — 网关内置 H5 配网页面（C 字符串常量）
 *
 * ⚠ 本文件是**生成物**，不要手改！
 *   唯一真源：rk2206-gateway/web/index.html
 *   再生成：  python3 rk2206-gateway/web/gen-h5-header.py
 *   校验：    python3 rk2206-gateway/web/gen-h5-header.py --check
 *            （tools/gw-compile-check.sh 每次都会跑这个校验）
 *
 * 为什么页面里一个双引号都没有：
 *   这段 HTML 要作为 C 字符串字面量编进固件。HTML 允许单引号属性、JS 允许单引号
 *   字符串，于是 C 里一个 \\\" 都不需要 —— 改页面时不会因为漏转义一个引号把固件编崩。
 *   生成脚本仍会正确转义双引号（只是会打印一条提醒），但请尽量守住这条约定。
 *
 * 契约 §12.11 六条约束的落点（逐条对应见 rk2206-gateway/配网实现说明.md）：
 *   1 单文件、CSS/JS 内联、无外部字体图标 → 全文没有一处 http(s):// 外链
 *   2 无 CDN、全离线 → 同上；页面写明"连上热点后提示无法访问互联网属正常"
 *   3 只调 §12 五个本地接口 → 全文只有 /api/provision/ 与 /api/pair/ 两个路径前缀（别在这里写星号：
 *     斜杠加星号连在一起 里那对字符会被 C 编译器当成注释开头，报 -Wcomment）
 *   4 不存口令明文、不回显 password → 口令提交后立刻清空输入框，不落 localStorage
 *   5 每请求 3 s 超时 + 1600–1606 文案表 → api() 里 AbortController(3000) + ERR 表
 *   6 入口 http://<AP>/、HTTP 非 HTTPS 同源 → 全部接口用相对路径 /api/...
 *     （因此 AP 地址由驱动决定也不影响页面，见 §12.3 与 CAL-12）
 */
"""


def esc(line: str) -> str:
    return line.replace("\\", "\\\\").replace('"', '\\"')


def render(html: str) -> str:
    lines = html.split("\n")
    # split 出来的最后一段若是空串，说明文件以换行结尾：它是行尾的产物，不是一行内容
    if lines and lines[-1] == "":
        lines = lines[:-1]
    body = []
    for ln in lines:
        body.append('"%s\\n"' % esc(ln))
    out = [HEADER_COMMENT, "#ifndef GW_H5_H", "#define GW_H5_H", "", "#define GW_H5_INDEX_HTML \\"]
    for i, ln in enumerate(body):
        tail = " \\" if i + 1 < len(body) else ""
        out.append(ln + tail)
    out.append("")
    out.append("#endif /* GW_H5_H */")
    return "\n".join(out) + "\n"


def main() -> int:
    if not os.path.isfile(SRC_HTML):
        sys.stderr.write("真源不存在：%s\n" % SRC_HTML)
        return 2
    with open(SRC_HTML, "r", encoding="utf-8", newline="") as f:
        html = f.read()
    if "\r" in html:
        sys.stderr.write("真源含 CR（CRLF 换行）：请存成 LF，否则固件里的 HTML 会多出 \\r\n")
        return 2
    text = render(html)
    n_dq = html.count('"')
    if n_dq:
        sys.stderr.write("提醒：真源有 %d 个双引号（已正确转义；约定是尽量不用）\n" % n_dq)

    check = "--check" in sys.argv[1:]
    old = ""
    if os.path.isfile(DST_H):
        with open(DST_H, "r", encoding="utf-8", newline="") as f:
            old = f.read()
    if check:
        if old == text:
            print("[gw_h5] 生成物与真源一致：web/index.html (%d 字节) → src/gw_h5.h" % len(html.encode("utf-8")))
            return 0
        sys.stderr.write("[gw_h5] ✗ src/gw_h5.h 与真源 web/index.html 不一致！"
                         "跑 python3 rk2206-gateway/web/gen-h5-header.py 重新生成\n")
        return 1
    with open(DST_H, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print("[gw_h5] 已生成 src/gw_h5.h（真源 %d 字节 / %d 行）" % (len(html.encode("utf-8")), html.count("\n")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
