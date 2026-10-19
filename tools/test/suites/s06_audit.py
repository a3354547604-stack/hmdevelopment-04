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
    # V11-2 已消解（§12 编号唯一）：t64 起是硬断言
    dups = C.duplicate_section_numbers()
    require(not dups, "章节编号重复：%r" % dups)


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
    """§2.5 的 key 说明必须指向 §6.9（settings 读取），不是 §6.7（设备状态）。

    M3 已消解（契约 v1.2 ⑤ 交叉引用 6 类 7 处）：t64 起是硬断言。
    """
    text = C.section_text("2.5")
    require("§6.9" in text or "§6.10" in text, "§2.5 应引用 §6.9/§6.10 的 settings 键名")
    require(not ("§6.7" in text and "§6.9" not in text),
            "§2.5 引用了 §6.7（设备状态），settings 键名在 §6.9")


def case_gateway_offline_ref():
    """§6.5 的 online 说明应指向 §8.7（离线判定），不是 §8.5（无活动）。"""
    text = C.section_text("6.5")
    require("§8.7" in text, "§6.5 应引用 §8.7")
    require(not ("§8.5" in text and "§8.7" not in text), "§6.5 online 不应引用 §8.5")


def case_disposition_ref():
    """§8.4 的「闭环由人工处置结束」应指向 §6.15（处置），不是 §6.10。"""
    text = C.section_text("8.4")
    require("§6.15" in text, "§8.4 应引用 §6.15")
    require(not ("§6.10" in text and "§6.15" not in text), "§8.4 处置不应引用 §6.10")


def case_push_table_ref():
    text = C.section_text("6.19")
    require("§7.12" in text or "§7.11" not in text, "§6.19 应引用 §7.12")


def case_door_dispatch_ref():
    text = C.section_text("3.8")
    require("§8.5" in text, "§3.8 应引用 §8.5")
    require(not ("§8.4" in text and "§8.5" not in text), "§3.8 door 分派不应引用 §8.4")


def case_activity_merge_ref():
    """§5.3 的活动归并应指向 §7.9/§7.10（activity_hourly/daily），不是 §7.6（原始读数）。

    M3 已消解；§0.2 的「报表日历切分」是另一处引用，单列 case_report_calendar_ref。
    """
    text = C.section_text("5.3")
    require("§7.9" in text or "§7.10" in text, "§5.3 活动归并应引用 §7.9/§7.10")
    require(not ("§7.6" in text and "§7.9" not in text),
            "§5.3 活动归并引用了 §7.6（sensor_readings 原始读数），实际归并在 §7.9/§7.10")


def case_report_calendar_ref():
    """§0.2「报表的日/小时按 UTC+8 日历切分」的落点应是活动归并表 §7.9/§7.10。

    M3-0.2 已由 t90（提交 494e5a9）收口：§0.2 改为「切分口径与表结构见 §7.9/§7.10；
    原始读数表见 §7.6」。t97 起本用例是**硬断言**——再退回只引 §7.6 就会红，
    不再有 SKIP 通道（§7.6 是 sensor_readings，不是日历切分的定义处）。
    """
    text = C.section_text("0.2")
    # 目标行是「- 报表的「日/小时」一律按 Asia/Shanghai（UTC+8）日历切分（…）」；
    # 用「报表的」而不是「报表」做判据——§0.2 另有一行讲时间戳样例（含「日历日」与
    # 「报表 `daily[].date`」），会把「报表」误命中。
    cands = [l for l in text.splitlines() if "报表的" in l and "日历" in l]
    require(cands, "§0.2 找不到「报表的…日历切分」那一行")
    line = cands[0]
    require("§7.9" in line or "§7.10" in line,
            "§0.2 报表日历行的权威落点应是 §7.9/§7.10：%s" % line.strip()[:90])
    require("§7.6" not in line or "§7.9" in line or "§7.10" in line,
            "§0.2 报表日历行不得只引 §7.6（原始读数表）：%s" % line.strip()[:90])


def case_adc_mv_order_claim():
    """§11 #2 声称 adc_mv 顺序 [ch2,ch3,ch4,ch5] 与 §2.4.1 完全一致——P0 只定义长度 1。"""
    t = C.table("11", must_have=["#", "结论"])
    row = next((r for r in t.rows if "adc_mv" in t.cell(r, "契约要求")), None)
    require(row is not None, "§11 找不到 adc_mv 一致性行")
    sec_241 = C.section_text("2.4.1")
    claim = t.cell(row, "契约要求")
    # M5 已消解（契约 v1.2 ⑦：§11 #2 改「P0 单路 [ch2]」）：t64 起是硬断言。
    # 注意：claim 里出现 ch3/ch4 是 P1 扩展说明，允许；不允许的是把 P0 也说成多路。
    require("[ch2]" in claim, "§11 #2 应写明 P0 = [ch2]（与 §2.4.1 长度 1 一致）")
    require("长度 1" in sec_241 or "P0 = `[ch2]`" in sec_241,
            "§2.4.1 应写明 P0 长度 1 = [ch2]")
    p0_multi = re.search(r"P0[^。\n]{0,20}\[ch2\s*,\s*ch3", claim)
    require(p0_multi is None,
            "§11 #2 不得声称 P0 的顺序是多路：…%s…" % (p0_multi.group(0) if p0_multi else ""))


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


def case_timestamp_example():
    """§0.2 的时间换算示例必须自洽（毫秒值 ↔ 写出来的 UTC 时刻）。

    M7 已消解（契约 v1.2 ⑨：全文毫秒样例统一到 2026-10-17，CRC 锁定帧与鉴权夹具除外）：
    t64 起是硬断言。
    """
    text = C.section_text("0")
    m = re.search(r"(\d{13})[^\n]*?(\d{4}-\d{2}-\d{2})T(\d{2}):(\d{2}):(\d{2})", text)
    require(m is not None, "§0.2 应有「毫秒值 ↔ UTC 时刻」的换算示例")
    real = datetime.fromtimestamp(int(m.group(1)) / 1000, tz=timezone.utc)
    require("%04d-%02d-%02d" % (real.year, real.month, real.day) == m.group(2),
            "§0.2 时间换算示例不自洽：写的是 %s，%s 实际是 %s"
            % (m.group(0)[:44], m.group(1), real.strftime("%Y-%m-%dT%H:%M:%SZ")))


def case_version_header():
    """标题版本必须与「变更记录」最新一行一致，且正文不得出现更高版本。"""
    first = FULL.splitlines()[0]
    m = re.match(r"#\s*接口契约\s*(v\d+\.\d+)", first)
    require(m is not None, "首行应是「# 接口契约 vX.Y（…）」：%r" % first[:60])
    ver = m.group(1)
    require(re.search(r"\*\*%s\*\*" % re.escape(ver), FULL) is not None,
            "「变更记录」里没有 %s 行（标题与变更记录不一致）" % ver)
    vers = sorted(set(re.findall(r"v(\d+\.\d+)", FULL)),
                  key=lambda s: [int(x) for x in s.split(".")])
    require(vers[-1] == ver.lstrip("v"),
            "正文出现比标题更高的版本：标题 %s，正文最高 v%s" % (ver, vers[-1]))


def case_wifi_password_contradiction():
    """§12.6 的口令约束必须自洽：8–63 字节，且明文拒绝开放网络（不接空串）。

    M14 已消解（契约 v1.2 ⑫）：t64 起是硬断言。
    """
    text = C.section_text("12.6")
    require("8–63" in text, "§12.6 应写明 WPA2 口令长度 8–63")
    require("不支持开放网络" in text, "§12.6 应明文写「本阶段不支持开放网络」")
    bad = re.search(r"可传空串|允许空串|接受空口令|空口令可", text)
    require(bad is None, "§12.6 不得再写「开放网络可传空串」这类与 8–63 互斥的口径：…%s…"
                         % (bad.group(0) if bad else ""))
    require("1602" in text and "open_not_supported" in text,
            "§12.6 应写明开放网络 → 400 code=1602 reason=open_not_supported")


def case_no_silent_skip():
    """不允许静默 SKIP：known() 引用必须在 PENDING 里，PENDING 不许有死条目。

    这条守的是「基线有牙」：已消解的问题若还被 SKIP 引用，等于把回归藏起来；
    没人引用的 PENDING 条目则是没人收口的空头登记。
    """
    suites_dir = os.path.dirname(os.path.abspath(__file__))
    refs = {}
    for fn in sorted(os.listdir(suites_dir)):
        if not fn.endswith(".py"):
            continue
        with open(os.path.join(suites_dir, fn), encoding="utf-8") as f:
            src = f.read()
        for m in re.finditer(r"\bki\.(?:known|pending)\(\s*\"([A-Za-z0-9._\-]+)\"", src):
            refs.setdefault(m.group(1), []).append(fn)
    pending, resolved = set(ki.PENDING), set(ki.RESOLVED)
    stale = sorted(set(refs) & resolved)
    unknown = sorted(set(refs) - pending - resolved - set(ki.HISTORICAL))
    dead = sorted(pending - set(refs))
    require(not stale, "以下已消解问题仍被 SKIP 引用（必须改成硬断言）：%r" % stale)
    require(not unknown, "以下 known 编号未登记：%r" % unknown)
    require(not dead, "PENDING 里以下编号没有任何用例引用（空头登记，需收口或转 HISTORICAL）：%r" % dead)


CASES = [
    ("§12.6 口令约束自洽（8–63 且拒绝开放网络）", case_wifi_password_contradiction),
    ("§ 章节编号唯一", case_section_numbers_unique),
    ("§ 交叉引用都指向存在的章节", case_all_section_refs_exist),
    ("§2.5 settings 键名引用 §6.9", case_settings_key_ref),
    ("§6.5 离线引用 §8.7", case_gateway_offline_ref),
    ("§8.4 处置引用 §6.15", case_disposition_ref),
    ("§6.19 push_tokens 引用 §7.12", case_push_table_ref),
    ("§3.8 door 分派引用 §8.5", case_door_dispatch_ref),
    ("§5.3 活动归并引用 §7.9/§7.10", case_activity_merge_ref),
    ("§0.2 报表日历引用落点（硬断言）", case_report_calendar_ref),
    ("§11 #2 adc_mv 顺序声明", case_adc_mv_order_claim),
    ("§8 告警类型都有 summary 文案", case_alert_types_have_summary),
    ("§6.13 summary 表 9 行且 level 合法", case_summary_table_shape),
    ("§6.2 总表路径都在正文", case_api_table_matches_sections),
    ("§10 CAL 标定表完整", case_cal_table),
    ("§0 时间戳单位统一", case_timestamp_units),
    ("§0.2 时间换算示例自洽", case_timestamp_example),
    ("契约版本标题与变更记录一致", case_version_header),
    ("不允许静默 SKIP（known 引用 ↔ PENDING 清单对账）", case_no_silent_skip),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s06_audit", "契约自身一致性与交叉引用（§0–§12）")
    return run_cases(suite, CASES, verbose)


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
