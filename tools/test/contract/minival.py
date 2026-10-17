#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""minival.py -- 极简 JSON Schema 校验器（只用标准库，不引 jsonschema 依赖）。

为什么自己写：验收脚本要在任何机器（含只装了 python3 的打包机）上跑，
不能要求先 pip install。这里只实现契约用得到的子集，并在 schema 里出现
未支持关键字时**报错**——静默忽略关键字会让验收假绿。

支持：type(可数组) / enum / const / required / properties / additionalProperties
      items / minItems / maxItems / minimum / maximum / exclusiveMinimum / exclusiveMaximum
      minLength / maxLength / pattern / oneOf / anyOf / nullable
"""

from __future__ import annotations

import re
from typing import Any, Dict, List

_TYPES = {
    "string": str,
    "integer": int,
    "number": (int, float),
    "boolean": bool,
    "object": dict,
    "array": list,
    "null": type(None),
}

SUPPORTED = {
    "type", "enum", "const", "required", "properties", "additionalProperties",
    "items", "minItems", "maxItems", "minimum", "maximum", "exclusiveMinimum",
    "exclusiveMaximum", "minLength", "maxLength", "pattern", "oneOf", "anyOf",
    "nullable", "title", "description", "$comment",
}


class SchemaError(RuntimeError):
    """schema 本身写错了（含未支持关键字）——测试应当失败。"""


def _check_supported(schema: Any, path: str) -> None:
    if not isinstance(schema, dict):
        return
    unknown = set(schema) - SUPPORTED
    if unknown:
        raise SchemaError("%s: 不支持的 schema 关键字 %s（minival 拒绝静默忽略）" % (path, sorted(unknown)))
    for key in ("properties",):
        for name, sub in (schema.get(key) or {}).items():
            _check_supported(sub, "%s.%s" % (path, name))
    if isinstance(schema.get("items"), dict):
        _check_supported(schema["items"], path + "[]")
    for key in ("oneOf", "anyOf"):
        for i, sub in enumerate(schema.get(key, []) or []):
            _check_supported(sub, "%s.%s[%d]" % (path, key, i))


def _type_ok(value: Any, expected: str) -> bool:
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "boolean":
        return isinstance(value, bool)
    py = _TYPES.get(expected)
    if py is None:
        raise SchemaError("未知 type=%r" % expected)
    if py is int:
        return isinstance(value, int) and not isinstance(value, bool)
    return isinstance(value, py)


def _validate(value: Any, schema: Dict[str, Any], path: str, errors: List[str]) -> None:
    if value is None and schema.get("nullable"):
        return

    if "const" in schema and value != schema["const"]:
        errors.append("%s: 期望常量 %r，实际 %r" % (path, schema["const"], value))

    if "enum" in schema and value not in schema["enum"]:
        errors.append("%s: 取值 %r 不在枚举 %r 内" % (path, value, schema["enum"]))

    if "type" in schema:
        types = schema["type"]
        types = types if isinstance(types, list) else [types]
        if not any(_type_ok(value, t) for t in types):
            errors.append("%s: 类型应为 %s，实际 %s (%r)" % (path, "/".join(types), type(value).__name__, value))
            return

    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            errors.append("%s: 长度 %d < %d" % (path, len(value), schema["minLength"]))
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            errors.append("%s: 长度 %d > %d" % (path, len(value), schema["maxLength"]))
        if "pattern" in schema and not re.search(schema["pattern"], value):
            errors.append("%s: %r 不匹配 pattern %r" % (path, value, schema["pattern"]))

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append("%s: %r < minimum %r" % (path, value, schema["minimum"]))
        if "maximum" in schema and value > schema["maximum"]:
            errors.append("%s: %r > maximum %r" % (path, value, schema["maximum"]))
        if "exclusiveMinimum" in schema and value <= schema["exclusiveMinimum"]:
            errors.append("%s: %r <= exclusiveMinimum %r" % (path, value, schema["exclusiveMinimum"]))
        if "exclusiveMaximum" in schema and value >= schema["exclusiveMaximum"]:
            errors.append("%s: %r >= exclusiveMaximum %r" % (path, value, schema["exclusiveMaximum"]))

    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            errors.append("%s: 元素 %d < minItems %d" % (path, len(value), schema["minItems"]))
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errors.append("%s: 元素 %d > maxItems %d" % (path, len(value), schema["maxItems"]))
        if isinstance(schema.get("items"), dict):
            for i, item in enumerate(value):
                _validate(item, schema["items"], "%s[%d]" % (path, i), errors)

    if isinstance(value, dict):
        props = schema.get("properties") or {}
        for name in schema.get("required", []) or []:
            if name not in value:
                errors.append("%s: 缺少必填字段 %r" % (path, name))
        for name, sub in props.items():
            if name in value:
                _validate(value[name], sub, "%s.%s" % (path, name), errors)
        extra = set(value) - set(props)
        ap = schema.get("additionalProperties", True)
        if ap is False:
            for name in sorted(extra):
                errors.append("%s: 出现契约未定义的字段 %r" % (path, name))
        elif isinstance(ap, dict):
            for name in sorted(extra):
                _validate(value[name], ap, "%s.%s" % (path, name), errors)

    for key in ("anyOf", "oneOf"):
        branches = schema.get(key)
        if not branches:
            continue
        ok = 0
        for sub in branches:
            sub_errors: List[str] = []
            _validate(value, sub, path, sub_errors)
            if not sub_errors:
                ok += 1
        if key == "anyOf" and ok == 0:
            errors.append("%s: 不满足 anyOf 任一分支" % path)
        if key == "oneOf" and ok != 1:
            errors.append("%s: 满足 oneOf 的分支数 %d（应为 1）" % (path, ok))


def validate(value: Any, schema: Dict[str, Any], path: str = "$") -> List[str]:
    """返回错误列表；空列表 = 通过。schema 自身非法时抛 SchemaError。"""
    _check_supported(schema, path)
    errors: List[str] = []
    _validate(value, schema, path, errors)
    return errors


def is_valid(value: Any, schema: Dict[str, Any]) -> bool:
    return not validate(value, schema)
