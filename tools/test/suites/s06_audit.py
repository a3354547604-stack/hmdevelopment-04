#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s06_audit.py -- 契约自身的可核对性审计（不是测实现，是测契约有没有自相矛盾）。

为什么验收基线里要有「审契约」这一套
    契约是唯一技术事实源。它自相矛盾时，实现方怎么选都是错、验收方怎么判都能被反驳。
    这套用例把「已知的、已经写进复核报告的问题」登记成 SKIP（带编号），
    把**新出现的**矛盾判 FAIL——这样契约每改一版，跑一遍就知道有没有引入新打架。
"""

from __future__ import annotations

import os
import re
import sys
from datetime import datetime, timezone

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import known_issues as ki             # noqa: E402
from harness import CaseFail, SkipCase, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402

C = cs.load()
FULL = C.text


def _line_of(needle: str) -> int:
    idx = FULL.find(needle)
    return FULL[:idx].count("\n") + 1 if idx >= 0 else -1


def case_section_numbers_unique():
    dups = C.duplicate_section_numbers()
    if dups:
        ki.known("V11-2", "重复编号：%r" % dups)


def case_all_section_refs_exist():
    """所有 §x.y 引用都必须指向真实存在的标题（防手滑改成不存在的编号）。"""
    headings = {num for num, _line, _lvl, _title in cs.iter_headings(FULL)}
    # 「实施计划 §9.4」「引脚与接线表 §2」是引用别的文档，不算契约内部引用
    body = re.sub(r"(实施计划|引脚与接线表|接线表|项目章程)\s*§\d+(?:\.\d+)*", "", FULL)
    refs = sorted(set(re.findall(r"§(\d+(?:\.\d+){0,3})", body)))
    missing = [r for r in refs if r not in headings]
    missing = [r for r in missing if not any(h == r or h.startswith(r + ".") for h in headings)]
    require(not missing, "引用了不存在的章节：%r" % missing)


def case_settings_key_ref():
    """§2.5 的 key 说明必须指向 §6.9（settings 读取），不是 §6.7（设备状态）。"""
    text = C.section_text("2.5")
    m = re.search(r"settings 键名）|settings", text)
    if "§6.7" in text and "§6.9" not in text:
        ki.known("M3", "§2.5 引用 §6.7（设备状态），settings 在 §6.9；L%d" % _line_of("settings 键名"))
    require("§6.9" in text or "§6.10" in text, "§2.5 应引用 §6.9/§6.10 的 settings 键名")


def case_gateway_offline_ref():
    """§6.5 的 online 说明应指向 §8.7（离线判定），不是 §8.5（无活动）。"""
    text = C.section_text("6.5")
    if "§8.5" in text and "§8.7" not in text:
        ki.known("M3", "§6.5 online 引用 §8.5，离线判定在 §8.7；L%d" % _line_of("180 s 规则"))
    require("§8.7" in text, "§6.5 应引用 §8.7")


def case_disposition_ref():
    """§8.4 的「闭环由人工处置结束」应指向 §6.15（处置），不是 §6.10。"""
    text = C.section_text("8.4")
    if "§6.10" in text and "§6.15" not in text:
        ki.known("M3", "§8.4 处置引用 §6.10（settings 写），实际在 §6.15；L%d" % _line_of("闭环仍由人工处置结束"))
    require("§6.15" in text, "§8.4 应引用 §6.15")


def case_push_table_ref():
    text = C.section_text("6.19")
    if "§7.11" in text and "§7.12" not in text:
        ki.known("M3", "§6.19 push_tokens 引用 §7.11（device_online），实际 §7.12；L%d" % _line_of("§7.11"))
    require("§7.12" in text or "§7.11" not in text, "§6.19 应引用 §7.12")


def case_door_dispatch_ref():
    text = C.section_text("3.8")
    if "§8.4" in text and "§8.5" not in text:
        ki.known("M3", "§3.8 door 分派引用 §8.4（跌倒），实际 §8.5；L%d" % _line_of("door→门状态"))
    require("§8.5" in text, "§3.8 应引用 §8.5")


def case_activity_merge_ref():
    """§5.3 / §0.2 的活动归并应指向 §7.9/§7.10，不是 §7.6（原始读数）。"""
    bad = []
    for sec in ("5.3", "0.2", "4.1"):
        text = C.section_text(sec)
        if "§7.6" in text and "§7.9" not in text:
            bad.append(sec)
    if bad:
        ki.known("M3", "活动归并引用 §7.6（原始读数），实际 §7.9/§7.10：§%s" % ", §".join(bad))


def case_adc_mv_order_claim():
    """§11 #2 声称 adc_mv 顺序 [ch2,ch3,ch4,ch5] 与 §2.4.1 完全一致——P0 只定义长度 1。"""
    t = C.table("11", must_have=["#", "结论"])
    row = next((r for r in t.rows if "adc_mv" in t.cell(r, "契约要求")), None)
    require(row is not None, "§11 找不到 adc_mv 一致性行")
    sec_241 = C.section_text("2.4.1")
    claim = t.cell(row, "契约要求")
    if "ch4" in claim and ("长度 1" in sec_241 or "[ch2]" in sec_241):
        ki.known("M5", "§11 #2 声称 %s，而 §2.4.1 P0 只发 [ch2]；L%d"
                 % (re.search(r"\[ch2[^\]]*\]", claim).group(0) if re.search(r"\[ch2[^\]]*\]", claim) else claim,
                    _line_of("adc_mv")))


def case_alert_types_have_summary():
    """§8 里出现的每个告警类型，都必须在 §6.13 的 summary 表里有文案。"""
    sec8 = C.section_text("8")
    declared = set(re.findall(r"type=([a-z_]+)", sec8)) - {"node_online"}  # node_online 是 recover 类型，不是告警
    summaries = {t for (t, _lv) in C.alert_summaries()}
    missing = sorted(declared - summaries)
    require(not missing, "§8 告警类型在 §6.13 没有 summary 文案：%r" % missing)


def case_summary_table_shape():
    """§6.13 的 summary 表应有 9 行，level 只有 1/2，且每个类型名在正文出现过。"""
    summaries = C.alert_summaries()
    require_eq(len(summaries), 9, "§6.13 summary 行数（fall/gas×2/sos/no_motion/node_offline/"
                                  "device_offline/door_left/env）")
    bad_level = [(t, lv) for (t, lv) in summaries if lv not in (1, 2)]
    require(not bad_level, "summary level 只能是 1/2：%r" % bad_level)
    missing = [t for (t, _lv) in summaries if t not in FULL]
    require(not missing, "summary 类型名在契约正文没出现：%r" % missing)


def case_api_table_matches_sections():
    """§6.2 总表里的每个路径，都要能在 §6.3–§6.20 的小节标题里找到对应。"""
    endpoints = C.endpoints()
    body = C.section_text("6")
    missing = [e["path"] for e in endpoints if e["path"] not in body]
    require(not missing, "§6.2 总表里有路径未在正文出现：%r" % missing)


def case_cal_table():
    text = C.section_text("10")
    cal_ids = sorted(set(re.findall(r"CAL-(\d{2})", text)))
    require(len(cal_ids) >= 11, "§10 标定项不足 11 条（实际 %d）" % len(cal_ids))
    require("状态" in text, "§10 表应带「状态」列")


def case_timestamp_units():
    text = C.section_text("0")
    require("Unix ms" in text or "毫秒" in text, "§0 应统一时间戳单位（Unix ms）")


def case_timestamp_example_known():
    """§0.2 的时间换算示例 1760000000000 实际是 2025-10-09T08:53:20Z。"""
    text = C.section_text("0")
    m = re.search(r"1760000000000[^\n]*?(\d{4}-\d{2}-\d{2})T(\d{2}):(\d{2}):(\d{2})", text)
    if not m:
        return
    claimed = m.group(0)
    real = datetime.fromtimestamp(1760000000000 / 1000, tz=timezone.utc)
    if "%04d-%02d-%02d" % (real.year, real.month, real.day) != m.group(1):
        ki.known("M7", "契约写 %r，实际 %s；L%d"
                 % (claimed[:40], real.strftime("%Y-%m-%dT%H:%M:%SZ"), _line_of("1760000000000")))


def case_version_header():
    first = FULL.splitlines()[0]
    if "v1.1" in FULL and "v1.1" not in first:
        ki.known("V11-3", "首行标题是 %r，但正文含 v1.1 内容" % first)


def case_wifi_password_contradiction():
    """§12.6：password 既写「必填 8–63」又写「开放网络传空串」——P0 只做 WPA2，按 8–63 实现。"""
    text = C.section_text("12.6")
    if "开放网络" in text and "8–63" in text:
        ki.known("M14", "§12.6 两条约束互斥；mock 与 s07 按 WPA2 的 8–63 执行")
    require("8–63" in text, "§12.6 应写明 WPA2 口令长度 8–63")


CASES = [
    ("§12.6 口令约束自洽（已登记 M14）", case_wifi_password_contradiction),
    ("§ 章节编号唯一", case_section_numbers_unique),
    ("§ 交叉引用都指向存在的章节", case_all_section_refs_exist),
    ("§2.5 settings 键名引用 §6.9", case_settings_key_ref),
    ("§6.5 离线引用 §8.7", case_gateway_offline_ref),
    ("§8.4 处置引用 §6.15", case_disposition_ref),
    ("§6.19 push_tokens 引用 §7.12", case_push_table_ref),
    ("§3.8 door 分派引用 §8.5", case_door_dispatch_ref),
    ("§5.3 活动归并引用 §7.9/§7.10", case_activity_merge_ref),
    ("§11 #2 adc_mv 顺序声明", case_adc_mv_order_claim),
    ("§8 告警类型都有 summary 文案", case_alert_types_have_summary),
    ("§6.13 summary 表 9 行且 level 合法", case_summary_table_shape),
    ("§6.2 总表路径都在正文", case_api_table_matches_sections),
    ("§10 CAL 标定表完整", case_cal_table),
    ("§0 时间戳单位统一", case_timestamp_units),
    ("§0.2 时间换算示例（已登记 M7）", case_timestamp_example_known),
    ("契约版本标题与内容一致（已登记 V11-3）", case_version_header),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s06_audit", "契约自身一致性与交叉引用（§0–§12）")
    return run_cases(suite, CASES, verbose)


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
