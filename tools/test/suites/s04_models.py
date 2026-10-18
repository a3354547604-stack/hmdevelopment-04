#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s04_models.py -- 阈值状态机、错误码与数据表结构（§7/§8/§9）。

这一套把契约里的「行为描述」变成可运行断言：燃气分级、跌倒迟滞、无活动/外出、
离线判定、重推节奏。实现方（网关/服务器）只要用同一组输入跑出同样的状态序列即可，
不需要去猜「契约这句话到底什么意思」。
"""

from __future__ import annotations

import os
import re
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import known_issues as ki             # noqa: E402
import reference_impl as ri           # noqa: E402
from harness import CaseFail, SkipCase, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402

C = cs.load()

# 计划要求的 10 张表 + 契约新增的 push_tokens（简化核对，不是字段级 DDL 校验）
PLAN_TABLES = ["users", "devices", "user_devices", "nodes", "sensor_readings",
               "alerts", "alert_dispositions", "activity_hourly", "activity_daily", "device_online"]


def _gas():
    p = C.gas_params()
    return ri.GasDetector(p["gas_t1_mv"], p["gas_t2_mv"], p["gas_l1_hold_s"], p["gas_l2_hold_s"],
                          p["gas_l1_to_l2_s"], p["gas_recover_mv"], p["gas_recover_hold_s"],
                          dt_s=2.0)


def case_gas_level1_needs_hold():
    g = _gas()
    events = []
    for _ in range(4):                       # 8 s < 10 s
        events += g.feed(1500)
    require_eq([e.level for e in events], [], "仅 4 次超限（8 s）不应进入预警")
    events = g.feed(1500)                    # 第 5 次 = 10 s
    require_eq([(e.kind, e.level) for e in events], [("alert", 1)], "第 10 s 应发一级告警")
    require_eq(g.level, 1, "状态应为预警")


def case_gas_level2_needs_hold_or_l1_timeout():
    g = _gas()
    for _ in range(5):
        g.feed(1500)
    events = []
    for _ in range(3):                       # 2600 mV × 3 次 = 6 s
        events += g.feed(2600)
    require_eq([(e.kind, e.level) for e in events], [("alert", 2)], "应升到二级告警")
    g2 = _gas()
    for _ in range(5):
        g2.feed(1500)
    events = []
    for _ in range(30):                      # 一级滞留 60 s（1500 < T2）
        events += g2.feed(1500)
    require(any(e.level == 2 for e in events), "一级滞留 %d s 未升二级" % (30 * 2))


def case_gas_recover_hold():
    g = _gas()
    for _ in range(5):
        g.feed(1500)
    events = []
    for _ in range(14):                      # 28 s < 30 s
        events += g.feed(1200)               # < T1-200=1300
    require_eq(g.level, 1, "干净 28 s 不应恢复")
    events = g.feed(1200)                    # 第 15 次 = 30 s
    require(any(e.kind == "recover" for e in events), "干净时长满 30 s 应恢复")
    require_eq(g.level, 0, "状态应回正常")


def case_gas_spike_no_alarm():
    g = _gas()
    g.feed(3000)
    for _ in range(10):
        g.feed(300)
    require_eq(g.level, 0, "单次尖峰不应产生告警（需连续 10 s）")


def case_fall_enter_immediate():
    p = C.fall_params()
    f = ri.FallLatch(p["k_fall_zero"], p["t_hold_s"], p["fall_suppress_s"])
    events = f.feed(True, 0.0)
    require_eq([(e.kind, e.level) for e in events], [("alert", 2)], "fall=1 应立即进入 ALARM（level 2）")


def case_fall_exit_hysteresis():
    p = C.fall_params()
    f = ri.FallLatch(p["k_fall_zero"], p["t_hold_s"], p["fall_suppress_s"])
    f.feed(True, 0.0)
    # 3 次 fall=0，但距最后触发仅 20 s < T_hold=30 s：不得退出
    f.feed(False, 10.0)
    f.feed(False, 15.0)
    f.feed(False, 20.0)
    require_eq(f.state, "ALARM", "K 满足但不到 T_hold 时不得退出")
    events = f.feed(False, 30.0)
    require(any(e.kind == "recover" for e in events), "≥30 s 且 K 次 fall=0 后应退出并给 recover")


def case_fall_repeat_suppressed():
    p = C.fall_params()
    f = ri.FallLatch(p["k_fall_zero"], p["t_hold_s"], p["fall_suppress_s"])
    f.feed(True, 0.0)
    events = []
    for t in (5.0, 10.0, 25.0, 40.0):
        events += f.feed(True, t)
    require_eq(events, [], "ALARM 期内重复 fall=1 不应产生新告警")


def case_no_motion_after_30min():
    p = C.motion_params()
    tr = ri.PresenceTracker(p["no_motion_min"], p["away_after_open_s"], p["presence_hold_s"])
    tr.feed(0.0, motion=True)
    events = []
    for t in range(60, int(p["no_motion_min"] * 60), 60):
        events += tr.feed(float(t))
    require_eq(events, [], "30 分钟前不应报警")
    events = tr.feed(p["no_motion_min"] * 60.0)
    require(any(e.detail == "no_motion" for e in events), "满 30 分钟应报 no_motion")


def case_away_mode_after_door():
    p = C.motion_params()
    tr = ri.PresenceTracker(p["no_motion_min"], p["away_after_open_s"], p["presence_hold_s"])
    tr.feed(0.0, motion=True, door_open=False)
    tr.feed(10.0, motion=False, door_open=True)          # 门开 → 进入外出候选
    require_eq(tr.st.state, "away", "开门后应进入外出模式")
    events = []
    for t in range(20, int(p["away_after_open_s"]) + 60, 30):
        events += tr.feed(float(t), door_open=False)
    require(any(e.detail == "no_motion" for e in events), "外出窗口内无活动应报 no_motion")
    events = tr.feed(p["away_after_open_s"] + 100, motion=True)
    require_eq(tr.st.state, "home", "检测到活动应退出外出模式")


def case_offline_times():
    p = C.offline_params()
    require_eq(p["node_offline_s"], 9, "§8.7 节点离线秒数")
    require_eq(p["gateway_offline_s"], 180, "§8.7 网关离线秒数")
    tr = ri.OfflineTracker(p["node_offline_s"], p["gateway_offline_s"])
    require(not tr.node_offline(0, 8), "8 s 不应判离线")
    require(tr.node_offline(0, 9), "9 s 应判离线")
    require(not tr.gateway_offline(0, 179), "179 s 不应判网关离线")
    require(tr.gateway_offline(0, 180), "180 s 应判网关离线")


def case_repush_schedule():
    p = C.repush_params()
    require_eq(p["alert_repush_s"], [0, 60, 300], "§8.8 重推节奏")
    s = ri.RepushScheduler(p["alert_repush_s"])
    require(s.should_push(now=0, created_at=0, push_count=0, status="pending", recovered_at=None),
            "t=0 应首发")
    require(not s.should_push(now=59, created_at=0, push_count=1, status="pending", recovered_at=None),
            "t=59 不应重推")
    require(s.should_push(now=60, created_at=0, push_count=1, status="pending", recovered_at=None),
            "t=60 应重推（第 2 次）")
    require(s.should_push(now=300, created_at=0, push_count=2, status="pending", recovered_at=None),
            "t=300 应重推（第 3 次）")
    for bad in (dict(now=400, created_at=0, push_count=3, status="pending", recovered_at=None),
                dict(now=400, created_at=0, push_count=1, status="resolved", recovered_at=None),
                dict(now=400, created_at=0, push_count=1, status="pending", recovered_at=1.0)):
        require(not s.should_push(**bad), "停推条件未生效：%r" % bad)


def case_db_tables_cover_plan():
    tables = C.tables_in_db()
    missing = [t for t in PLAN_TABLES if t not in tables]
    require(not missing, "§7 缺计划要求的表：%r" % missing)
    require_in("push_tokens", tables, "§7 推送表")


def case_alerts_table_has_push_fields():
    cols = C.db_columns("alerts")
    for need in ("push_count", "next_push_at", "recovered_at", "status", "type", "level"):
        require_in(need, cols, "alerts 表字段")


def case_db_columns_have_pk():
    pk_notes = 0
    for tname in C.tables_in_db():
        cols = C.db_columns(tname)
        require(cols, "%s 表没有列" % tname)
    text = C.section_text("7.1")
    require("PRIMARY KEY" in text.upper() or "主键" in text, "§7.1 应写主键约定")


def case_biz_error_http_mapping():
    errors = C.biz_errors()
    http = C.http_codes()
    for code, info in sorted(errors.items()):
        h = re.search(r"\d{3}", info["http"])
        require(h is not None, "§9.2 code=%s 未写 HTTP 码（%r）" % (code, info["http"]))
        require_in(int(h.group(0)), http, "§9.2 code=%s 的 HTTP %s 不在 §9.1" % (code, h.group(0)))


def case_required_error_codes_present():
    errors = C.biz_errors()
    for code in (1000, 1001, 1002, 1003, 1004, 1100, 1101, 1102, 1103, 1104, 1105,
                 1200, 1201, 1202, 1203, 1204, 1205, 1300, 1301, 1302, 1400, 1500, 1501, 1502):
        require_in(code, errors, "§9.2 业务码")


def case_1502_conflict_known():
    """§3.6 不得再把 1502 当设备侧 result：1502 只属 §9.2 的 webhook 校验失败。

    B2 已消解（契约 v1.2 ①：§3.6 重发失败改 result=12、§9.3 新增 12）：t64 起是硬断言。
    允许 §3.6 **提到** 1502，但只能出现在「只用于 webhook / 不允许」这类禁止语境里。
    """
    framing = C.section_text("3.6")
    device = C.device_errors()
    require_in(12, device, "§9.3 设备码应有 12（链路超时/无应答）")
    require(("result=12" in framing) or ("`12`" in framing),
            "§3.6 重发失败应报 result=12（§9.3），而不是 1502")
    for m in re.finditer("1502", framing):
        ctx = framing[max(0, m.start() - 60):m.end() + 60]
        require(("只用于 webhook" in ctx) or ("不允许" in ctx),
                "§3.6 提到 1502 但不在禁止语境里（可能又把 1502 当设备侧 result 用了）：…%s…"
                % ctx.strip())


def case_device_error_codes():
    dev = C.device_errors()
    for code in range(0, 8):
        require_in(code, dev, "§9.3 设备侧错误码")
    require("CRC" in dev[4], "§9.3 code=4 含义应为 CRC 校验失败，实际 %r" % dev[4])


def case_settings_defaults_match_state_machine():
    items = C.settings_defaults()
    gas = C.gas_params()
    motion = C.motion_params()
    offline = C.offline_params()
    pairs = [
        ("gas_t1_mv", gas["gas_t1_mv"]), ("gas_t2_mv", gas["gas_t2_mv"]),
        ("gas_l1_hold_s", gas["gas_l1_hold_s"]), ("gas_l2_hold_s", gas["gas_l2_hold_s"]),
        ("gas_recover_mv", gas["gas_recover_mv"]), ("gas_recover_hold_s", gas["gas_recover_hold_s"]),
        ("no_motion_min", motion["no_motion_min"]), ("away_after_open_s", motion["away_after_open_s"]),
        ("node_offline_timeout_s", offline["node_offline_s"]),
        ("gateway_offline_timeout_s", offline["gateway_offline_s"]),
    ]
    for key, expected in pairs:
        require_eq(items[key], expected, "§6.9 %s 与 §8 默认值" % key)


def case_repush_in_settings():
    items = C.settings_defaults()
    require_eq(items["alert_repush_s"], C.repush_params()["alert_repush_s"], "§6.9 alert_repush_s")
    require_eq(items["alert_suppress_s"], C.repush_params()["alert_suppress_s"], "§6.9 alert_suppress_s")


CASES = [
    ("§8.2 燃气一级需连续 10 s", case_gas_level1_needs_hold),
    ("§8.2 燃气二级需连续 6 s 或一级滞留 60 s", case_gas_level2_needs_hold_or_l1_timeout),
    ("§8.2 燃气恢复需干净 30 s", case_gas_recover_hold),
    ("§8.2 单次尖峰不告警", case_gas_spike_no_alarm),
    ("§8.4 跌倒立即进入 ALARM", case_fall_enter_immediate),
    ("§8.4 跌倒退出需 K=3 且 ≥30 s", case_fall_exit_hysteresis),
    ("§8.4 ALARM 期内重复触发被抑制", case_fall_repeat_suppressed),
    ("§8.5 无活动 30 min 报警", case_no_motion_after_30min),
    ("§8.5 开门后外出模式与 600 s 窗口", case_away_mode_after_door),
    ("§8.7 离线 9 s / 180 s", case_offline_times),
    ("§8.8 重推 [0,60,300] 与停推条件", case_repush_schedule),
    ("§7 表结构覆盖计划 10 张 + push_tokens", case_db_tables_cover_plan),
    ("§7.7 alerts 含推送/恢复字段", case_alerts_table_has_push_fields),
    ("§7.x 每张表都有列", case_db_columns_have_pk),
    ("§9.2 业务码与 §9.1 HTTP 映射自洽", case_biz_error_http_mapping),
    ("§9.2 必查业务码齐全", case_required_error_codes_present),
    ("§3.6 result=1502 越界（已登记 B2）", case_1502_conflict_known),
    ("§9.3 设备侧码 0–7", case_device_error_codes),
    ("§6.9 默认值与 §8 阈值一致", case_settings_defaults_match_state_machine),
    ("§6.9 重推/抑制键与 §8.8 一致", case_repush_in_settings),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s04_models", "阈值状态机与数据模型（§7/§8/§9）")
    return run_cases(suite, CASES, verbose)


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
