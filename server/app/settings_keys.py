"""设置项清单（契约 §6.9 字段表，21 项）。

契约 §6.9 的键集合口径（v1.2 / PM D-01）：**`gas_l1_to_l2_s` 不在 `items` 里**——它是 §8.2 的
算法参数（一级滞留升二级，P0 固定 60 s），既不进 §6.9 响应，也不接受 §6.10 PUT 下发
（下发即 `1003` 未知键）；要开放运行时配置必须走契约变更。见下方 `ALGORITHM_PARAMS`。

每项标注：
- `node`：谁消费（M0 = 汇聚/网关侧，M1/M2/M4 = 节点）；
- `dispatch`：P0 是否支持远程下发。P1 项（环境阈值、门长开告警、告警重推）先只落库与回显，
  下发时进入 `rejected`，避免「接受了却没人执行」的假生效。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from .errors import ApiError


@dataclass(frozen=True)
class KeySpec:
    name: str
    kind: str  # int | float | int_list
    default: Any
    minimum: float | None = None
    maximum: float | None = None
    node: str = "M0"
    dispatch: bool = True
    reason: str = ""


# §8.2 算法参数：**刻意不放进 SETTINGS_SPEC / DEFAULTS**。留在这里是为了给「它不是漏了、
# 而是被契约明确排除」留个出处——免得后来人看到 §8.2 提到它就顺手加回 items（那正是 D-01 要修的事）。
ALGORITHM_PARAMS: dict[str, Any] = {
    "gas_l1_to_l2_s": 60,  # 一级滞留多久升二级；P0 固定，改值走契约变更
}


SETTINGS_SPEC: dict[str, KeySpec] = {
    # ---- 活动/存在（M4 门厅：干簧管 + PIR）----
    "no_motion_min": KeySpec("no_motion_min", "int", 30, 5, 120, "M4"),
    "presence_hold_s": KeySpec("presence_hold_s", "int", 60, 10, 600, "M4"),
    "away_after_open_s": KeySpec("away_after_open_s", "int", 600, 60, 3600, "M4"),
    # ---- 燃气（M1 厨房：MQ-2 + AHT20）----
    "gas_t1_mv": KeySpec("gas_t1_mv", "int", 1500, 0, 5000, "M1"),
    "gas_t2_mv": KeySpec("gas_t2_mv", "int", 2500, 0, 5000, "M1"),
    "gas_l1_hold_s": KeySpec("gas_l1_hold_s", "int", 10, 2, 600, "M1"),
    "gas_l2_hold_s": KeySpec("gas_l2_hold_s", "int", 6, 2, 600, "M1"),
    "gas_recover_mv": KeySpec("gas_recover_mv", "int", 200, 0, 1000, "M1"),
    "gas_recover_hold_s": KeySpec("gas_recover_hold_s", "int", 30, 5, 3600, "M1"),
    # ---- 跌倒（M2 卧室：LD6002C，迟滞状态机 §8.4）----
    "fall_suppress_s": KeySpec("fall_suppress_s", "int", 30, 10, 600, "M2"),
    # ---- 频率/离线（汇聚 + 服务器）----
    "report_interval_s": KeySpec("report_interval_s", "int", 60, 10, 3600, "M0"),
    "node_offline_timeout_s": KeySpec(
        "node_offline_timeout_s", "int", 9, 3, 300, "M0", False,
        "由汇聚侧 3×3 s 巡检周期推导，远程改它需要重算汇聚轮询，P0 只读",
    ),
    "gateway_offline_timeout_s": KeySpec(
        "gateway_offline_timeout_s", "int", 180, 60, 3600, "M0", False, "服务器判定参数，不经过设备"
    ),
    # ---- 环境（P1：节点已上报温湿度，阈值告警暂未启用）----
    "temp_high_c": KeySpec("temp_high_c", "float", 35.0, -40, 125, "M1", False, "P1 环境阈值"),
    "temp_low_c": KeySpec("temp_low_c", "float", 5.0, -40, 125, "M1", False, "P1 环境阈值"),
    "humi_high_pct": KeySpec("humi_high_pct", "float", 85.0, 0, 100, "M1", False, "P1 环境阈值"),
    "humi_low_pct": KeySpec("humi_low_pct", "float", 20.0, 0, 100, "M1", False, "P1 环境阈值"),
    "env_hold_min": KeySpec("env_hold_min", "int", 30, 1, 1440, "M1", False, "P1 环境阈值"),
    # ---- 告警策略（服务器）----
    "alert_repush_s": KeySpec(
        "alert_repush_s", "int_list", [0, 60, 300], None, None, "M0", False, "服务器重推策略，P1 只落库回显"
    ),
    "alert_suppress_s": KeySpec("alert_suppress_s", "int", 60, 10, 3600, "M0", False, "服务器去重窗口"),
    # ---- 门长开（P1：契约 §8.6 已定义行为）----
    "door_open_alarm_s": KeySpec("door_open_alarm_s", "int", 180, 30, 3600, "M4", False, "P1 门长开告警"),
}

DEFAULTS: dict[str, Any] = {name: spec.default for name, spec in SETTINGS_SPEC.items()}

# 别名兼容：契约 §6.7 规定按「路径参数优先，冲突时 1001」，这里给两条等价别名
ALIASES: dict[str, str] = {
    "device_id": "deviceId",
    "alert_id": "alertId",
    "gateway_sn": "gatewaySn",
}

TRIGGER_TYPES = ("fall", "gas", "door", "presence", "heartbeat")
ALERT_TYPES = ("fall", "gas", "no_motion", "door_left", "env", "node_offline", "device_offline", "sos")
ALERT_LEVELS = (1, 2)  # 契约 §7.7：1 预警 / 2 报警


def _check_value(spec: KeySpec, value: Any) -> Any:
    if spec.kind == "int":
        if isinstance(value, bool) or not isinstance(value, int):
            raise ApiError(1001, field=spec.name, expected="int")
        if spec.minimum is not None and not (spec.minimum <= value <= spec.maximum):
            raise ApiError(1002, field=spec.name, allowed=[spec.minimum, spec.maximum])
        return value
    if spec.kind == "float":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ApiError(1001, field=spec.name, expected="number")
        value = float(value)
        if spec.minimum is not None and not (spec.minimum <= value <= spec.maximum):
            raise ApiError(1002, field=spec.name, allowed=[spec.minimum, spec.maximum])
        return value
    if spec.kind == "int_list":
        if not isinstance(value, list) or not all(isinstance(v, int) and not isinstance(v, bool) for v in value):
            raise ApiError(1001, field=spec.name, expected="int[]")
        if len(value) != 3 or value != sorted(value) or any(v < 0 or v > 86400 for v in value):
            raise ApiError(1002, field=spec.name, allowed=[0, 86400], note="3 个升序秒数")
        return value
    raise ApiError(1002, field=spec.name, note="未知取值类型")


def normalize_payload(payload: Any) -> dict[str, Any]:
    """校验 /api/config 请求体：{deviceId, items:{key:value}} 或裸 items（契约 §6.9）。"""
    if not isinstance(payload, dict):
        raise ApiError(1001, expected="object")
    items = payload.get("items", payload.get("settings"))
    if items is None and "deviceId" not in payload:
        items = payload  # 允许直接提交 {key: value}
    if not isinstance(items, dict):
        raise ApiError(1000, missing=["items"])
    return items


def validate_items(items: dict[str, Any]) -> dict[str, Any]:
    """逐项校验；未知键 1003，类型 1001，越界 1002（契约 §6.9）。"""
    validated: dict[str, Any] = {}
    for key, value in items.items():
        spec = SETTINGS_SPEC.get(key)
        if spec is None:
            raise ApiError(1003, field=key, allowed=sorted(SETTINGS_SPEC))
        validated[key] = _check_value(spec, value)

    merged = {name: DEFAULTS[name] for name in SETTINGS_SPEC}
    merged.update(validated)
    # 跨字段约束：L2 阈值必须高于 L1，否则分级状态机无法进入 L2
    if merged["gas_t2_mv"] <= merged["gas_t1_mv"]:
        raise ApiError(1002, field="gas_t2_mv", allowed=[merged["gas_t1_mv"] + 1, 5000], note="必须大于 gas_t1_mv")
    return validated


def dispatch_targets(items: dict[str, Any]) -> tuple[dict[str, Any], dict[str, str]]:
    """拆出可下发项与拒绝项（拒绝带原因），供 /api/config 的 accepted/rejected 使用。"""
    accepted: dict[str, Any] = {}
    rejected: dict[str, str] = {}
    for key, value in items.items():
        spec = SETTINGS_SPEC[key]
        if spec.dispatch:
            accepted[key] = value
        else:
            rejected[key] = spec.reason or "P0 不支持远程下发"
    return accepted, rejected
