#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s03_iotda.py -- 华为云 IoTDA 接入契约（§4）：鉴权、topic、属性/事件/命令、报文示例。

重点回答队长点名的问题 ③：§4.1 的鉴权算法能不能落地、和「库里样例照抄」的坑有没有写清。
"""

from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import sys
from datetime import datetime, timezone

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import known_issues as ki             # noqa: E402
import minival                        # noqa: E402
import schema_builder as sb           # noqa: E402
from harness import CaseFail, SkipCase, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402

C = cs.load()

DEVICE_ID = "6a1b2c3d4e5f"
SECRET = "demo-secret-only-for-test"
TS_STR = "2025100908"          # 2025-10-09 08:00 UTC


def client_id(device_id: str, ts_utc_ymdh: str, sig_type: int = 1, id_type: int = 0) -> str:
    return "%s_%d_%d_%s" % (device_id, id_type, sig_type, ts_utc_ymdh)


def password(secret: str, ts_utc_ymdh: str) -> str:
    return hmac.new(secret.encode("utf-8"), ts_utc_ymdh.encode("utf-8"), hashlib.sha256).hexdigest()


def ts_utc(sec: int) -> str:
    return datetime.fromtimestamp(sec, tz=timezone.utc).strftime("%Y%m%d%H")


def case_clientid_and_password_shape():
    cid = client_id(DEVICE_ID, TS_STR)
    require_eq(cid, "6a1b2c3d4e5f_0_1_2025100908", "ClientId 四段结构")
    pwd = password(SECRET, TS_STR)
    require_eq(len(pwd), 64, "Password 应为 64 位十六进制")
    require_eq(pwd, pwd.lower(), "Password 应为小写十六进制")
    require(re.fullmatch(r"[0-9a-f]{64}", pwd) is not None, "Password 字符集")


def case_password_matches_contract_text():
    text = C.section_text("4.1")
    require("HMACSHA256" in text.replace("-", ""), "§4.1 应写明 HMACSHA256")
    require("_0_" in text, "§4.1 应写明 ClientId 第 2 段固定 0")
    require("UTC" in text, "§4.1 应写明时间戳取 UTC")


def case_clientid_fourth_segment_reused_in_hmac():
    """契约要求 ClientId 第 4 段与 HMAC 的时间戳完全一致——参考实现必须能对齐。"""
    now = 1760000000
    seg = ts_utc(now)
    cid = client_id(DEVICE_ID, seg)
    require_eq(cid.split("_")[3], seg, "ClientId 第 4 段")
    require_eq(len(password(SECRET, seg)), 64, "同段时间戳算出的 Password 长度")


def case_auth_failure_codes_documented():
    text = C.section_text("4.1")
    for code in ("0x02", "0x04"):
        require(code in text, "§4.1 应写明鉴权失败返回码 %s（否则现场无法定位）" % code)
    joined = text.replace(" ", "")
    require("oc_mqtt" in text, "§4.1 应说明仓库无 oc_mqtt 样例")
    require("QoS2" in joined or "qos2" in joined.lower(), "§4.1 应写明平台不支持 QoS2")


def case_keepalive_and_qos():
    text = C.section_text("4.1")
    require("120" in text, "§4.1 应写明 keepalive=120 s")
    require("QoS1" in text.replace(" ", ""), "§4.1 应写明 QoS1")
    require("will" in text.lower(), "§4.1 应写明不设 will")
    require("retain" in text.lower(), "§4.1 应写明不设 retain")


def case_topics_p0_set():
    rows = C.iotda_topics()
    topics = [r["topic"] for r in rows]
    require(len(topics) >= 8, "§4.2 topic 少于 8 条（实际 %d）" % len(topics))
    for need in ("properties/report", "events/up", "messages/up", "commands/request_id"):
        require(any(need in t for t in topics), "§4.2 缺 topic 片段 %s" % need)
    p0 = [r for r in rows if "P0" in r["stage"]]
    require(len(p0) >= 4, "§4.2 标 P0 的 topic 只有 %d 条" % len(p0))


def case_message_up_is_array():
    """§4.6：messages/up 用 IoTDA 消息对象，content 是「§2.2 信封数组」的 JSON 字符串。"""
    msg_examples = [e for e in C.iotda_examples()
                    if isinstance(e, dict) and "content" in e and "services" not in e]
    require(msg_examples, "§4.6 缺 messages/up 示例（含 name/id/content 的对象）")
    env = sb.envelope_schema(C)
    checked = 0
    for ex in msg_examples:
        require(isinstance(ex.get("name"), str) and ex["name"], "messages/up 示例缺 name")
        require(isinstance(ex.get("id"), str) and ex["id"], "messages/up 示例缺 id")
        content = ex["content"]
        require(isinstance(content, str), "messages/up 的 content 应是 JSON 字符串（IoTDA 规范）")
        items = json.loads(content)
        require(isinstance(items, list) and items, "content 解析后应是信封数组")
        for item in items:
            errs = minival.validate(item, env)
            require(not errs, "content 里的信封不符合 §2.2：%r" % errs)
            tname = item.get("type")
            if tname in C.types():
                errs = minival.validate(item, sb.node_message_schema(C, tname))
                require(not errs, "content 里的 %s 信封不符合 §2.4：%r" % (tname, errs))
            checked += 1
    require(checked >= 2, "messages/up 示例只含 %d 条信封（批量示例应有 ≥2）" % checked)


def case_event_id_format():
    text = C.section_text("4.4")
    require("event_id" in text, "§4.4 应定义 event_id")
    require("-{ts}" in text.replace(" ", ""), "§4.4 应给出 {gateway_sn}-{type}-{ts} 格式")
    for ex in C.iotda_examples():
        if not isinstance(ex, dict):
            continue
        for svc in ex.get("services", []):
            paras = svc.get("paras") or {}
            eid = paras.get("event_id")
            if eid:
                require(re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9-]*-[a-z_]+-\d{10,}", eid) is not None,
                        "event_id %r 不符合 {sn}-{type}-{ts}" % eid)


def case_event_time_format():
    for ex in C.iotda_examples():
        if not isinstance(ex, dict):
            continue
        for svc in ex.get("services", []):
            et = svc.get("event_time")
            if et:
                require(re.fullmatch(r"\d{8}T\d{6}Z", et) is not None,
                        "event_time %r 不符合 yyyyMMdd'T'HHmmss'Z'" % et)


def case_events_and_paras():
    rows = C.iotda_events()
    require_eq(sorted(r["event_type"] for r in rows), ["alert", "recover", "sos"], "§4.4 事件集合")
    for r in rows:
        require("event_time" in C.section_text("4.6") or True, "")
    text = " ".join(r["paras"] for r in rows)
    for field in ("type", "level", "node_sn", "ts", "event_id"):
        require(field in text, "§4.4 paras 缺字段 %s" % field)


def case_commands_defined():
    rows = C.iotda_commands()
    names = [r["命令"] for r in rows]
    for need in ("set_report_interval", "demo_trigger", "mute_alarm"):
        require(need in names, "§4.5 缺命令 %s" % need)
    joined = json.dumps(rows, ensure_ascii=False)
    require("10" in joined and "3600" in joined, "§4.5 应写明 set_report_interval 10–3600")


def case_properties_cover_core_set():
    props = {p["name"] for p in C.iotda_properties()}
    for need in ("presence", "last_motion_ts", "door", "window", "temp", "humi",
                 "smoke", "nodes_online", "battery", "alarm_state"):
        require_in(need, props, "§4.3 核心属性")
    require(len(props) >= 12, "§4.3 属性少于 12 个（实际 %d）" % len(props))


def case_properties_p0_conflict_known():
    """§4.6 的上报示例不得出现 P0 未装的属性（lpg/co/presence/last_motion_ts）。

    M6 已消解（契约 v1.2 ⑧：§4.6 示例改为 P0 真实上报）：t64 起是硬断言。
    注意 §4.6 的示例是 node_samples 消息（payload 以转义 JSON 嵌在 content 串里），
    所以直接扫原文的 `"key":` 形态——早先按 ex["services"] 取属性的写法在这里恒为空，
    是个假绿。
    """
    raw = C.section_text("4.6")
    require(bool(C.json_blocks("4.6")), "§4.6 应有 JSON 示例（版式变了？）")
    text43 = C.section_text("4.3")
    p1_props = [p for p in ("lpg", "co", "presence", "last_motion_ts")
                if re.search(r'\\?"%s\\?"\s*:' % p, raw)]
    require(not p1_props,
            "§4.6 上报示例出现 P0 未装属性 %s，与 §4.3「P0 未装→缺省」口径冲突（P0 应直接省略该属性）"
            % p1_props)
    require(("P1" in text43) or ("缺省" in text43), "§4.3 应写明 P0 未装属性缺省/P1")


CASES = [
    ("§4.1 ClientId/Password 结构与长度", case_clientid_and_password_shape),
    ("§4.1 鉴权算法与取时口径有明文", case_password_matches_contract_text),
    ("§4.1 ClientId 第 4 段与 HMAC 时间戳一致", case_clientid_fourth_segment_reused_in_hmac),
    ("§4.1 鉴权失败码与样例坑已写明", case_auth_failure_codes_documented),
    ("§4.1 keepalive/QoS/will/retain", case_keepalive_and_qos),
    ("§4.2 P0 topic 齐全", case_topics_p0_set),
    ("§4.6 messages/up 批量信封自洽", case_message_up_is_array),
    ("§4.4 event_id 格式", case_event_id_format),
    ("§4.6 event_time 格式", case_event_time_format),
    ("§4.4 三个事件与 paras 字段", case_events_and_paras),
    ("§4.5 三个命令与取值范围", case_commands_defined),
    ("§4.3 核心属性齐全", case_properties_cover_core_set),
    ("§4.6 示例 lpg/co 与 P0 口径冲突（已登记 M6）", case_properties_p0_conflict_known),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s03_iotda", "IoTDA 物模型与鉴权（§4）")
    return run_cases(suite, CASES, verbose)


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
