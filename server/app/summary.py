"""告警文案（契约 §6.13 的 summary 表：**由服务器生成，不信设备或 App 传入的文本**）。

契约表是逐字基准（验收会比对）：
`fall/2` 卧室检测到跌倒、`gas/1` 厨房燃气浓度偏高、`gas/2` 厨房燃气泄漏报警、`sos/2` 收到紧急求助、
`no_motion/1` 30 分钟未检测到活动、`node_offline/1` 节点 {sn} 已离线、`device_offline/1` 网关已离线、
`door_left/1` 门已打开超过 3 分钟、`env/1` 室内温湿度异常。

两处「可动」而不是「写死」的取舍（默认值下与契约表逐字一致）：
- `no_motion` 的分钟数取设备设置里的 `no_motion_min`（用户可在设置页改阈值，文案必须跟着变）；
- `door_left` 的分钟数取 `door_open_alarm_s`；
- 房间名优先用 `nodes.location`，节点未登记位置时按节点默认位置兜底（M1 厨房 / M2 卧室）。
"""
from __future__ import annotations

import math
from typing import Any

from .settings_keys import DEFAULTS

# 兜底文案（与契约 §6.13 表格逐字一致；{location} 会在有位置信息时被替换）
SUMMARY_TABLE: dict[tuple[str, int], str] = {
    ("fall", 2): "{location}检测到跌倒",
    ("gas", 1): "{location}燃气浓度偏高",
    ("gas", 2): "{location}燃气泄漏报警",
    ("sos", 2): "收到紧急求助",
    ("env", 1): "室内温湿度异常",
    ("device_offline", 1): "网关已离线",
    ("door_left", 1): "门已打开超过 3 分钟",
    ("node_offline", 1): "节点离线",
}

# 位置敏感类型：只有这两个类型在契约文案里带房间名
_LOCATION_AWARE = {
    ("fall", 2): "卧室",
    ("gas", 1): "厨房",
    ("gas", 2): "厨房",
}

# 节点出厂位置（契约 §7.5 的 location 由用户/施工单登记，未登记时按节点用途兜底）
NODE_DEFAULT_LOCATION = {
    "M1": "厨房",
    "M2": "卧室",
    "M3": "卧室",
    "M4": "门厅",
    "M5": "卫生间",
}


def alert_summary(
    alert_type: str,
    level: int,
    node_sn: str | None = None,
    location: str | None = None,
    settings: dict[str, Any] | None = None,
) -> str:
    cfg = {**DEFAULTS, **(settings or {})}
    place = location or NODE_DEFAULT_LOCATION.get((node_sn or "").split("-")[0], "")

    if alert_type == "no_motion":
        minutes = int(cfg.get("no_motion_min", 30))
        if level >= 2:
            # 二级文案不在契约表内（P1 才用），给「动态阈值的两倍」而不是硬编码
            return f"{max(minutes * 2, 60)} 分钟未检测到活动"
        return f"{minutes} 分钟未检测到活动"
    if alert_type == "door_left":
        seconds = int(cfg.get("door_open_alarm_s", 180))
        return f"门已打开超过 {max(1, round(seconds / 60))} 分钟"
    if alert_type == "node_offline" and node_sn:
        return f"节点 {node_sn} 已离线"

    template = SUMMARY_TABLE.get((alert_type, level))
    if template:
        if "{location}" in template:
            fallback = _LOCATION_AWARE.get((alert_type, level), "")
            return template.format(location=place or fallback or "室内")
        return template
    # 未在契约表内的组合（例如 P1 的 env/2）：给中性文案，绝不返回空串（App 直接展示该字段）
    return f"{alert_type} 告警（等级 {level}）"


# 契约 v1.2 §6.17：`compare.title` 是**固定文案**，不参与趋势判断。原先「今日活动频次」是拼在
# text 里的前缀，被 PM（m00683）拆成独立键，因此 title 必现——包括没有昨日基线的情形。
TREND_TITLE = "今日活动频次"


def _round_half_up(value: float) -> int:
    """四舍五入成整数百分比（契约 §6.17 要求整数）：内建 `round()` 是银行家舍入，10.5 得 10，不合契约。"""
    if value >= 0:
        return int(math.floor(value + 0.5))
    return -int(math.floor(-value + 0.5))


def trend_compare(today: int, yesterday: int) -> tuple[int | None, str | None]:
    """契约 v1.2 §6.17 `compare`：返回 (delta_pct, text)；两者都为 None ⇒ **无昨日基线**。

    规则（契约原文，v1.2 定稿）：`delta_pct ≤ −10` →「较昨日下降 N%」；`≥ +10` →「较昨日上升 N%」；
    `−10 < delta_pct < +10` →「与昨日基本持平」。**边界值算升降**（恰好 +10% 是「上升 10%」）。
    无昨日基线（`yesterday` 为 0，或 `today = yesterday = 0`）→ 调用方省略 `yesterday`/`delta_pct`/`text`
    三个键，只留 `title` 与 `today`（§0.4：无值即省略键，不写 null 占位）。
    """
    if yesterday <= 0:
        return None, None
    delta_pct = _round_half_up((today - yesterday) / yesterday * 100)
    n = abs(delta_pct)
    if delta_pct <= -10:
        return delta_pct, f"较昨日下降 {n}%"
    if delta_pct >= 10:
        return delta_pct, f"较昨日上升 {n}%"
    return delta_pct, "与昨日基本持平"


def prev_compare_text(total: int, prev_total: int, prev_label: str) -> str:
    """契约 §6.18 `activity.compare_prev`：形如「较上周下降 5%」。"""
    if total + prev_total == 0:
        return "暂无活动数据"
    if prev_total == 0:
        return f"{prev_label}无活动记录"
    delta = (total - prev_total) / prev_total * 100
    n = abs(int(round(delta)))
    if delta < -10:
        return f"较{prev_label}下降 {n}%"
    if delta > 10:
        return f"较{prev_label}上升 {n}%"
    return f"较{prev_label}基本持平"
