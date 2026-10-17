"""响应体字段清洗（契约 §0.4：「无值」= 字段缺省，**不写 null 占位**）。

只清**最外层**的 None：平台下发的快照（`properties`/`snapshot`/`payload`）是对上游报文的
事实记录，里面出现 null 说明平台确实发了 null，改写会掩盖证据，所以不下钻。
"""
from __future__ import annotations

from typing import Any


def prune_none(data: dict[str, Any] | None) -> dict[str, Any]:
    if not data:
        return {}
    return {key: value for key, value in data.items() if value is not None}
