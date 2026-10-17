#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""schema_builder.py -- 用契约里的字段表生成 minival schema。

规则（不猜）：
    类型列：bool→boolean、int/int64→integer、double/number→number、string→string、
            object→object、`int[]`→array(integer)
    「必填」列以「是」开头 → required；「条件」只作为提示，不强制（契约没写清条件式时
     验收不应比契约更严；这类字段在审计套件里单独列出）
    范围列：`a–b` → minimum/maximum；`≥n` → minimum；`0/1`、`a/b` → enum；带「255=」的
     哨兵值放宽到上限（例如 battery 0–100 且 255=市电）
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional

from contract_source import Contract, Table, plain


def type_to_schema(type_text: str) -> Dict[str, Any]:
    t = plain(type_text).strip().lower()
    if t.endswith("[]"):
        inner = type_to_schema(t[:-2])
        return {"type": "array", "items": inner}
    base = t.split("/")[0].split("（")[0].split("(")[0].strip()
    mapping = {
        "bool": "boolean",
        "boolean": "boolean",
        "int": "integer",
        "int32": "integer",
        "int64": "integer",
        "integer": "integer",
        "double": "number",
        "float": "number",
        "decimal": "number",
        "number": "number",
        "string": "string",
        "str": "string",
        "object": "object",
        "array": "array",
    }
    if base not in mapping:
        raise ValueError("未知契约类型 %r（请在 schema_builder 里显式登记）" % type_text)
    return {"type": mapping[base]}


_NUM = r"-?\d+(?:\.\d+)?"


def range_to_schema(range_text: str, base_type: str = "") -> Dict[str, Any]:
    s = plain(range_text).replace("\u2212", "-").replace("\u2013", "-").replace("\u2014", "-").strip()
    if not s or s in ("—", "-", "/"):
        return {}
    m = re.match(r"^≥\s*(%s)$" % _NUM, s)
    if m:
        return {"minimum": _num(m.group(1))}
    m = re.match(r"^≤\s*(%s)$" % _NUM, s)
    if m:
        return {"maximum": _num(m.group(1))}
    m = re.match(r"^(%s)\s*-\s*(%s)\s*(.*)$" % (_NUM, _NUM), s)
    if m:
        lo, hi, tail = _num(m.group(1)), _num(m.group(2)), m.group(3)
        out: Dict[str, Any] = {"minimum": lo, "maximum": hi}
        sentinel = re.search(r"(%s)\s*=" % _NUM, tail)
        if sentinel:
            out["maximum"] = max(hi, _num(sentinel.group(1)))
            out["$comment"] = "含哨兵值 " + plain(tail)
        return out
    enums = _enum_from_range(s)
    if enums:
        return enums
    if s.isdigit():
        return {"enum": [int(s)]}
    if "JSON" in s or "文本" in s:
        return {}
    return {"$comment": "范围未结构化：" + s}


def _enum_from_range(s: str):
    """把 `0正常 1预警 2报警` / `true/false` / `idle/ap/sta` 这类范围文本变成 enum。"""
    pairs = re.findall(r"(\d+)([^\d\s]*)", s)
    if pairs and all(tail for _num_s, tail in pairs):
        return {"enum": [int(n) for n, _t in pairs]}
    if "/" in s:
        parts = [p.strip() for p in s.split("/") if p.strip()]
        if parts and all(p in ("true", "false") for p in parts):
            return {"enum": [p == "true" for p in parts]}
        if parts and all(re.match(r"^[a-z_]+$", p) for p in parts):
            return {"enum": parts}
    return None


def _num(text: str):
    v = float(text)
    return int(v) if v.is_integer() else v


def fields_schema(table: Table, type_col: str = "类型", required_col: str = "必填",
                  range_col: str = "范围", enum_col: str = "") -> Dict[str, Any]:
    props: Dict[str, Any] = {}
    required: List[str] = []
    has_range = any(range_col in h for h in table.header)
    has_req = any(required_col in h for h in table.header)
    for row in table.rows:
        name = table.p(row, table.header[0])
        if not name or name.startswith("#"):
            continue
        sub = type_to_schema(table.cell(row, type_col)) if any(type_col in h for h in table.header) else {}
        if has_range:
            sub.update(range_to_schema(table.cell(row, range_col), sub.get("type", "")))
        if enum_col and any(enum_col in h for h in table.header):
            toks = re.findall(r"`([^`]+)`", table.cell(row, enum_col))
            if toks and len(toks) > 1:
                sub["enum"] = toks
        props[name] = sub
        if has_req and table.p(row, required_col).startswith("是"):
            required.append(name)
    schema: Dict[str, Any] = {"type": "object", "properties": props, "additionalProperties": False}
    if required:
        schema["required"] = required
    return schema


def envelope_schema(c: Contract) -> Dict[str, Any]:
    t = c.envelope_fields()
    return fields_schema(t)


def payload_schema(c: Contract, type_name: str) -> Dict[str, Any]:
    t = c.payload_fields(type_name)
    return fields_schema(t)


def downlink_schema(c: Contract) -> Dict[str, Any]:
    t = c.downlink_fields()
    return fields_schema(t)


def node_message_schema(c: Contract, type_name: str) -> Dict[str, Any]:
    """§2.2 信封 + 指定 type 的 payload 约束。"""
    env = envelope_schema(c)
    env["properties"]["type"] = {"type": "string", "enum": [type_name]}
    env["properties"]["payload"] = payload_schema(c, type_name)
    return env


def iotda_property_schema(c: Contract) -> Dict[str, Any]:
    """§4.3 属性定义 → schema（属性名 → 类型/范围）。"""
    t = c.table("4.3", must_have=["属性", "物模型类型"])
    props: Dict[str, Any] = {}
    for row in t.rows:
        name = t.p(row, "属性")
        if not name or name in ("属性",):
            continue
        sub = type_to_schema(t.cell(row, "物模型类型"))
        rng = t.cell(row, "取值") if any("取值" in h for h in t.header) else ""
        sub.update(range_to_schema(rng, sub.get("type", "")))
        props[name] = sub
    return {"type": "object", "properties": props, "additionalProperties": False}


def services_envelope_schema(c: Contract, body_key: str = "properties") -> Dict[str, Any]:
    """§4.6 上行报文的 {"services":[{service_id, event_time, <body_key>}]} 外形。"""
    inner: Dict[str, Any] = {"type": "object", "properties": {
        "service_id": {"type": "string", "enum": ["gateway"]},
        "event_time": {"type": "string", "pattern": r"^\d{8}T\d{6}Z$"},
    }, "required": ["service_id", "event_time"], "additionalProperties": True}
    inner["properties"][body_key] = iotda_property_schema(c) if body_key == "properties" else {"type": "object"}
    return {"type": "object",
            "properties": {"services": {"type": "array", "minItems": 1, "items": inner}},
            "required": ["services"], "additionalProperties": False}


def settings_schema_from_example(c: Contract) -> Dict[str, Any]:
    """§6.9 的 items JSON 示例 → schema（类型来自示例，键集合即契约冻结集合）。"""
    items = c.settings_defaults()
    props: Dict[str, Any] = {}
    for key, val in items.items():
        if isinstance(val, bool):
            props[key] = {"type": "boolean"}
        elif isinstance(val, int):
            props[key] = {"type": "integer"}
        elif isinstance(val, float):
            props[key] = {"type": "number"}
        elif isinstance(val, list):
            props[key] = {"type": "array", "items": {"type": "integer"}}
        else:
            props[key] = {"type": "string"}
    return {"type": "object", "properties": props, "additionalProperties": False}


def rest_envelope_schema() -> Dict[str, Any]:
    """§6.1 统一响应信封。"""
    return {
        "type": "object",
        "properties": {
            "code": {"type": "integer"},
            "message": {"type": "string"},
            "data": {},
            "ts": {"type": "integer"},
        },
        "required": ["code", "message", "ts"],
        "additionalProperties": False,
    }
