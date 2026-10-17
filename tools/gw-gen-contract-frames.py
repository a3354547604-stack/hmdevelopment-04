#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 docs/技术/接口契约.md §3.7 抽出「实测示例帧」，生成 rk2206-gateway/test/gw_contract_frames.inc。

为什么要有这个脚本：契约 §3.7 的六条样例帧是**唯一**对帧格式（SOF/VER/TYPE/SEQ/LEN/CRC 字节序）
的权威冻结证据（v1.2 起六条都是完整字节，注释里明说「验收脚本可对六条全部做硬断言」）。
手抄 hex 到 C 数组里迟早会抄错一位，而且契约更新后没人会记得去改测试；
所以只保留「一份来源」：脚本读契约、生成 .inc，测试再逐条解析 + 重编比对。

用法：
    tools/gw-gen-contract-frames.py            # 生成（覆盖）
    tools/gw-gen-contract-frames.py --check    # 只校验磁盘上的 .inc 与契约一致（CI/自测用）
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTRACT = os.path.join(ROOT, "docs", "技术", "接口契约.md")
OUT = os.path.join(ROOT, "rk2206-gateway", "test", "gw_contract_frames.inc")

HEX2 = re.compile(r"^[0-9A-Fa-f]{2}$")


def parse_frames(path):
    """返回 [(name, [byte, ...]), ...]；只读 §3.7 那一节的表格。"""
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().splitlines()

    start = None
    for i, ln in enumerate(lines):
        if ln.startswith("###") and "3.7" in ln and "示例帧" in ln:
            start = i
            break
    if start is None:
        raise SystemExit("✗ 契约里找不到 «### 3.7 实测示例帧» 小节")

    frames = []
    for ln in lines[start + 1:]:
        if ln.startswith("###") or ln.startswith("## "):
            break  # 下一节开始
        if not ln.startswith("|"):
            if frames:
                break  # 表格结束
            continue
        cells = [c.strip() for c in ln.strip().strip("|").split("|")]
        if len(cells) < 2 or set(cells[0]) <= set("-: "):
            continue  # 表头分隔行
        name, hexcell = cells[0], cells[-1]
        # 去掉 markdown 装饰，只留可读名字
        name = name.replace("**", "").replace("`", "").strip()
        toks = [t for t in re.split(r"[\s,]+", re.sub(r"[^0-9A-Fa-f\s]", " ", hexcell)) if t]
        if len(toks) < 4 or not all(HEX2.match(t) for t in toks):
            continue
        frames.append((name, [int(t, 16) for t in toks]))

    if len(frames) != 6:
        raise SystemExit("✗ 期望 §3.7 恰好 6 条样例帧，实际解析到 %d 条（契约表格格式变了？）" % len(frames))
    return frames


def render(frames):
    out = []
    out.append("/* 自动生成，请勿手改：%s §3.7 实测示例帧" % os.path.relpath(CONTRACT, ROOT))
    out.append(" * 生成器：tools/gw-gen-contract-frames.py（--check 可校验是否与契约一致）")
    out.append(" * 用途：gw_selftest.c 逐条「解析 + 重编」，把帧格式钉死在契约给出的字节上。 */")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    uint16_t    len;")
    out.append("    uint8_t     bytes[GW_FRAME_MAX];")
    out.append("} gw_contract_frame_t;")
    out.append("")
    out.append("static const gw_contract_frame_t k_contract_frames[] = {")
    for name, data in frames:
        out.append("    { \"%s\", %d, {" % (name.replace('"', "'"), len(data)))
        for i in range(0, len(data), 12):
            chunk = ", ".join("0x%02X" % b for b in data[i:i + 12])
            out.append("        %s%s" % (chunk, "," if i + 12 < len(data) else ""))
        out.append("    } },")
    out.append("};")
    out.append("")
    out.append("#define GW_CONTRACT_FRAME_COUNT ((int)(sizeof(k_contract_frames) / sizeof(k_contract_frames[0])))")
    out.append("")
    return "\n".join(out)


def main():
    text = render(parse_frames(CONTRACT))
    check = "--check" in sys.argv
    if check:
        old = open(OUT, encoding="utf-8").read() if os.path.exists(OUT) else ""
        if old != text:
            print("✗ %s 与契约 §3.7 不一致：跑 tools/gw-gen-contract-frames.py 重新生成"
                  % os.path.relpath(OUT, ROOT))
            return 1
        print("✓ %s 与契约 §3.7 一致（6 条样例帧）" % os.path.relpath(OUT, ROOT))
        return 0
    with open(OUT, "w", encoding="utf-8") as fh:
        fh.write(text)
    print("✓ 已生成 %s（%d 条样例帧，共 %d 字节 hex）"
          % (os.path.relpath(OUT, ROOT), 6, sum(1 for _ in re.finditer(r"0x[0-9A-F]{2}", text))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
