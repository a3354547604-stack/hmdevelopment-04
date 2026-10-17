#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s01_schema.py -- 契约内部一致性 + 契约自带示例的 schema 校验。

这一套回答的问题：「契约自己举的例子，符不符合契约自己定义的字段表？」
契约改一个字段类型/必填/枚举，这里就会红，从而提醒实现方与验收方同步。
"""

from __future__ import annotations

import json
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import minival                        # noqa: E402
import schema_builder as sb           # noqa: E402
import known_issues as ki             # noqa: E402
from harness import CaseFail, CaseResult, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402

C = cs.load()


def _valid(schema, value, what):
    errs = minival.validate(value, schema)
    if errs:
        raise CaseFail("%s 不符合契约 schema：\n  %s" % (what, "\n  ".join(errs[:8])))


def case_types_have_payload_tables():
    types = C.types()
    require_eq(sorted(types), ["door", "env", "fall", "gas", "presence", "status"], "§2.3 type 集合")
    for t in types:
        table = C.payload_fields(t)
        require(len(table.rows) >= 2, "§2.4.%s 字段表只有 %d 行" % (t, len(table.rows)))


def case_payload_field_types_are_known():
    for t in C.types():
        schema = sb.payload_schema(C, t)
        require(schema.get("properties"), "§2.4.%s 生成的 schema 没有属性" % t)


def case_envelope_schema_required():
    schema = sb.envelope_schema(C)
    for name in ("sn", "ts", "type", "payload"):
        require_in(name, schema.get("required", []), "§2.2 必填字段 %s" % name)


def case_envelope_rejects_missing_payload():
    schema = sb.envelope_schema(C)
    bad = {"sn": "M1", "ts": 1760000000000, "type": "gas"}
    errs = minival.validate(bad, schema)
    require(any("payload" in e for e in errs), "§2.2 缺 payload 应当报错，实际 %r" % errs)


def case_node_message_schema_accepts_minimal():
    schema = sb.node_message_schema(C, "fall")
    good = {"sn": "M2", "ts": 1760000123000, "type": "fall", "payload": {"fall": 1, "presence": 1}}
    errs = minival.validate(good, schema)
    require(not errs, "最小 fall 报文应通过：%r" % errs)


def case_iotda_examples_match_property_schema():
    examples = C.iotda_examples()
    require(len(examples) >= 3, "§4.6 应有至少 3 个 JSON 示例，实际 %d" % len(examples))
    checked = 0
    for ex in examples:
        if not isinstance(ex, dict) or "services" not in ex:
            continue
        svc = ex["services"][0]
        if "properties" in svc:
            _valid(sb.iotda_property_schema(C), svc["properties"], "§4.6 properties 示例")
            checked += 1
    require(checked >= 1, "§4.6 里没有可校验的 properties 示例")


def case_iotda_property_schema_covers_declared():
    declared = [p["name"] for p in C.iotda_properties()]
    schema = sb.iotda_property_schema(C)
    for name in declared:
        require_in(name, schema["properties"], "§4.3 属性 %s" % name)


def case_rest_examples_use_envelope():
    env = sb.rest_envelope_schema()
    checked = 0
    for num, _title in C.sections_matching("6."):
        if num.count(".") != 1:
            continue
        for val in C.json_blocks(num):
            cands = val if isinstance(val, list) else [val]
            for cand in cands:
                if isinstance(cand, dict) and "code" in cand:
                    errs = minival.validate(cand, env)
                    if errs and all("缺少必填字段 'ts'" in e for e in errs):
                        if num == "6.11":
                            ki.known("V11-1", "§6.11 示例 %s" % json.dumps(cand, ensure_ascii=False)[:60])
                        continue
                    if errs:
                        raise CaseFail("§%s 响应示例不符合 §6.1 信封：%r" % (num, errs))
                    checked += 1
    require(checked >= 3, "§6 里只找到 %d 个带 code 的 JSON 示例（应有响应示例）" % checked)


def case_settings_keys_and_types():
    items = C.settings_defaults()
    schema = sb.settings_schema_from_example(C)
    require_eq(sorted(schema["properties"]), sorted(items), "§6.9 items 键集合与 schema")
    errs = minival.validate(items, schema)
    require(not errs, "§6.9 items 自校验失败：%r" % errs)


def case_settings_cover_state_machine_params():
    """§8 里声明「可配=是」的参数必须在 §6.9 items 里能改——否则 App 改不动。"""
    items = C.settings_defaults()
    missing = []
    for num in ("8.2", "8.5"):
        t = C.table(num, must_have=["参数", "默认值"])
        has_cfg = any("可配" in h for h in t.header)
        for row in t.rows:
            name = t.p(row, "参数")
            if not name or name.startswith(("进入", "退出", "恢复", "采样")):
                continue
            if has_cfg and t.p(row, "可配").startswith("否"):
                continue
            if name not in items:
                missing.append("§%s %s" % (num, name))
    if missing:
        only_m4 = all(m.endswith(" gas_l1_to_l2_s") for m in missing)
        if only_m4:
            ki.known("M4")
        raise CaseFail("以下可配参数在 §6.9 items 里缺失（App 无法下发）：" + ", ".join(missing))


def case_field_names_unique():
    for num in ("2.2", "2.5"):
        t = C.table(num, must_have=["字段"])
        names = [t.p(r, "字段") for r in t.rows]
        dup = [n for n in names if names.count(n) > 1]
        require(not dup, "§%s 字段名重复：%r" % (num, sorted(set(dup))))


def case_downlink_fields_cover_cmd_enum():
    """§2.5 的 cmd 枚举必须和 §3.4 的 0x11 子表一致（同一枚举两处写，必须同口径）。"""
    t = C.downlink_fields()
    schema = sb.downlink_schema(C)
    require_in("cmd", schema["properties"], "§2.5 下行 cmd 字段")
    down = set()
    for r in t.rows:
        if t.p(r, t.header[0]) == "cmd":
            down = {p.strip() for p in t.p(r, "说明").split("/") if p.strip()}
    up = set(C.uart_cmd_enum(0x11))
    require(bool(down) and bool(up), "取不到 §2.5 或 §3.4 的 cmd 枚举（版式变了）")
    diff = down ^ up
    if diff == {"reboot", "reboot_node", "time_sync"}:
        ki.known("M13", "§2.5=%s vs §3.4=%s" % (sorted(down), sorted(up)))
    if diff:
        raise CaseFail("§2.5 与 §3.4 的 cmd 枚举不一致：仅 §2.5 有 %s；仅 §3.4 有 %s"
                       % (sorted(down - up), sorted(up - down)))


CASES = [
    ("§2.3 每个 type 都有 payload 字段表", case_types_have_payload_tables),
    ("§2.4.x 字段类型都能映射成 schema", case_payload_field_types_are_known),
    ("§2.2 信封四个必填字段", case_envelope_schema_required),
    ("§2.2 缺 payload 会被 schema 拒绝", case_envelope_rejects_missing_payload),
    ("§2.2+§2.4.2 最小 fall 报文可通过", case_node_message_schema_accepts_minimal),
    ("§4.6 属性示例符合 §4.3 定义", case_iotda_examples_match_property_schema),
    ("§4.3 声明属性都在 schema 里", case_iotda_property_schema_covers_declared),
    ("§6 响应示例符合 §6.1 信封", case_rest_examples_use_envelope),
    ("§6.9 items 键集合与类型自洽", case_settings_keys_and_types),
    ("§8 可配参数都能在 §6.9 改", case_settings_cover_state_machine_params),
    ("字段名无重复", case_field_names_unique),
    ("§2.5 下行 cmd 枚举完整", case_downlink_fields_cover_cmd_enum),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s01_schema", "契约 schema 与示例一致性")
    return run_cases(suite, CASES, verbose)


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
