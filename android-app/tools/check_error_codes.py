#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""契约错误码一致性检查：把《接口契约 v1.2》§9.2 的错误码表与本 App 的
ContractErrors.kt 对照，防止「契约改了、App 没跟上」。

为什么需要它：错误码是 App 与网关/服务器之间唯一的「口头约定」，
现场排障全靠它。契约一旦新增码而 App 没同步，用户只会看到「未知错误码 16xx」，
既不能自解释也不能指导操作——这类问题在真机上才发现就太晚了。
本脚本不需要 Android SDK、不需要联网，只读两个文件。

覆盖范围（写清楚，免得被读成「连文案也比过了」——t26 F-03）：

  比对（不一致 → 退出码 1）：
    ① 码集合：契约有而 App 缺 / App 有而契约无
    ② 常量声明：MESSAGES 里用了未声明的常量名（写错名字会让该码静默丢失）
    ③ code=0 登记为成功（契约 §6.1：code=0 才算成功）
    ④ 配网码（1600–1609）都有 hint() 操作指引——现场只能干瞪眼是最坏的结果

  不比对（差异只作为「提示」打印，不计入失败）：
    ⑤ 文案**文本**。契约 §9.2 的 `message` 列是「API 响应里的原文」，App 的
       MESSAGES 是「给用户看的措辞」，两者不要求逐字相同。例：契约 1600 =
       「网关不在配网模式」，App = 「网关不在配网模式（请长按配对键 3 秒）」——
       后者多了现场动作，是有意如此。本脚本只把这类措辞差异列出来供人扫一眼。

用法：
  python3 android-app/tools/check_error_codes.py
  python3 android-app/tools/check_error_codes.py --contract docs/技术/接口契约.md --kotlin <path>

退出码：0 = 上面 ①–④ 全通过；1 = 有差异
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_CONTRACT = os.path.join(ROOT, "docs", "技术", "接口契约.md")
DEFAULT_KOTLIN = os.path.join(
    ROOT, "android-app", "app", "src", "main", "java",
    "com", "hmprogram", "eldercare", "provision", "core", "ContractErrors.kt",
)

# 本地配网 API（§12）的码段：落在这一段里的码必须有现场操作指引
PROVISION_CODE_MIN = 1600
PROVISION_CODE_MAX = 1609


def parse_contract(path: str) -> dict[int, tuple[str, str]]:
    """从 §9.2 的表格里抓 code → (message 列, 说明 列)。

    表头形如：| code | message | HTTP | 说明 |
    只认 3–4 位纯数字的第一列，其余行（表头、分隔线）跳过。
    """
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()

    start = None
    for i, line in enumerate(lines):
        if re.match(r"^#+\s*9\.2", line.strip()):
            start = i
            break
    if start is None:
        raise SystemExit("!! 契约里找不到 §9.2 小节，契约结构可能变了")

    codes: dict[int, tuple[str, str]] = {}
    for line in lines[start:]:
        if line.startswith("#") and codes:
            break  # 下一节开始
        m = re.match(r"^\|\s*`?(\d{3,4})`?\s*\|([^|]*)\|([^|]*)\|([^|]*)\|", line)
        if m:
            code = int(m.group(1))
            message = m.group(2).strip()
            desc = m.group(4).strip()
            codes[code] = (message, desc)
    return codes


def parse_kotlin(path: str) -> tuple[dict[int, str], set[int], list[str]]:
    """从 ContractErrors.kt 抓常量定义、MESSAGES 表与 hint() 覆盖的码。

    MESSAGES 表用的是常量名（`MISSING_PARAM to "缺少必填参数"`），不是裸数字，
    所以必须两步走：先建「常量名 → 码值」，再把表里的名字翻译成码值。
    只匹配裸数字会得到「一个都没登记」的假结论（本脚本第一版就踩了这个坑）。
    """
    with open(path, encoding="utf-8") as f:
        src = f.read()

    consts: dict[str, int] = {
        name: int(value)
        for name, value in re.findall(r"const val ([A-Z0-9_]+)\s*=\s*(\d+)", src)
    }

    block = re.search(r"mapOf\((.*?)\n\s*\)", src, re.S)
    if not block:
        raise SystemExit("!! ContractErrors.kt 里找不到 MESSAGES 的 mapOf(...) 块")

    messages: dict[int, str] = {}
    unnamed: list[str] = []
    for entry in re.finditer(r"([A-Za-z0-9_]+)\s+to\s+\"((?:[^\"\\]|\\.)*)\"", block.group(1)):
        name, text = entry.group(1), entry.group(2)
        if name not in consts:
            unnamed.append(name)
            continue
        messages[consts[name]] = text

    hint_block = re.search(r"fun hint\(code: Int\): String\? = when \(code\) \{(.*?)\n\s*else -> null", src, re.S)
    hinted = {
        consts[name]
        for name in re.findall(r"([A-Z][A-Z0-9_]+)\s*->", hint_block.group(1) if hint_block else "")
        if name in consts
    }
    return messages, hinted, unnamed


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--contract", default=DEFAULT_CONTRACT)
    ap.add_argument("--kotlin", default=DEFAULT_KOTLIN)
    args = ap.parse_args()

    contract = parse_contract(args.contract)
    messages, hinted, unnamed = parse_kotlin(args.kotlin)

    problems: list[str] = []
    for name in unnamed:
        problems.append(f"表里用了未声明的常量名：{name}")

    # code=0（成功）语义上不属于「错误码」，但契约表里通常有这一行；没有就按 §6.1 单独核对
    if 0 not in contract:
        if 0 in messages and messages[0].strip().lower() == "ok":
            contract[0] = ("ok", "成功（契约 §6.1：code=0 才算成功）")
        else:
            problems.append("App 未把 code=0 登记为成功")

    missing = sorted(set(contract) - set(messages))
    extra = sorted(set(messages) - set(contract))
    for code in missing:
        message, desc = contract[code]
        problems.append(f"契约有、App 缺：{code}  {message} {desc}".rstrip())
    for code in extra:
        problems.append(f"App 有、契约无：{code}  {messages[code]}")

    # 配网链路（§12/§13）的码必须有「照这个动作做」的指引，否则现场只能干瞪眼
    provision = sorted(c for c in messages if PROVISION_CODE_MIN <= c <= PROVISION_CODE_MAX)
    provision_hinted = [c for c in provision if c in hinted]
    for code in provision:
        if code not in hinted:
            problems.append(f"配网错误码缺 hint() 操作指引：{code}  {messages[code]}")

    # 措辞差异：只提示，不算失败（覆盖范围⑤）
    wording_diff = [
        (code, contract[code][0], messages[code])
        for code in sorted(set(contract) & set(messages))
        if contract[code][0].strip() != messages[code].strip()
    ]

    span = f"{provision[0]}–{provision[-1]}" if provision else "无"
    print(f"契约 §9.2 错误码 {len(contract)} 条；ContractErrors.kt 文案 {len(messages)} 条；"
          f"配网码 {len(provision)} 条（{span}，带 hint() 操作指引 {len(provision_hinted)} 条）")

    if wording_diff:
        print(f"提示：{len(wording_diff)} 条码的 App 文案与契约 `message` 列措辞不同"
              f"（**不计入失败**，App 是面向用户的措辞）：")
        for code, want, got in wording_diff:
            print(f"   - {code}: 契约「{want}」/ App「{got}」")

    if problems:
        print(f"!! 发现 {len(problems)} 处不一致（码集合 / 常量声明 / 操作指引）：")
        for p in problems:
            print("   -", p)
        return 1

    print("一致：错误码集合、code=0 登记、配网码操作指引、常量声明与契约 §9.2 逐条对齐；"
          "文案文本不比对（措辞差异见上「提示」行）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
