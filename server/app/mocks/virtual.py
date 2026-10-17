"""虚拟网关（mock）：无硬件时生成**与真实网关同构**的 IoTDA 报文。

为什么这样设计（而不是让 demo 直接往库里写告警）：
契约 §6.12 要求演示注入走真实链路「IoTDA → 网关 → 上报 → webhook」。本模块扮演网关的
角色，产出 §4.4 的节点 JSON 与 §5.2 的报文信封，然后调用与线上**同一个**处理函数
`app.services.telemetry.handle_webhook`。因此演示链路里除「华为云跳板」外没有任何 mock 分支：
告警去重、重推、入库、报表归并全是生产代码。

CAL-09 提醒：节点 JSON 的 `ts` 是自开机毫秒数（§0.2），网关获取不到有效时间，
真实链路由服务器按接收时刻纠正（`CLOCK_SOURCE=server`）；本模块同样只填 `ts=0`，
由 `telemetry.handle_message` 统一改写，保证 mock 与真实行为一致。
"""
from __future__ import annotations

import json
from typing import Any

from ..config import settings
from ..state import record_command
from ..timeutil import now_ms
from ..services import telemetry

RESOURCE_PROPERTY = "device.property"
RESOURCE_EVENT = "device.event"
RESOURCE_MESSAGE = "device.message"

# 演示事件 → (节点, 告警类型, 等级, 节点 JSON payload, 网关事件 paras 附加字段)
DEMO_EVENTS: dict[str, dict[str, Any]] = {
    "fall": {
        "node_sn": "M2",
        "type": "fall",
        "level": 2,
        "node_payload": {"fall": 1, "duration_ms": 18000, "radar_state": 2},
        "snapshot": {"fall": 1, "duration_ms": 18000},
    },
    "gas": {
        "node_sn": "M1",
        "type": "gas",
        "level": 2,
        "node_payload": {"smoke": 0, "lpg": 3200, "temp": 26.5, "humi": 58.0, "level": 2},
        "snapshot": {"lpg": 3200, "level": 2},
    },
    "sos": {
        "node_sn": "H1",
        "type": "sos",
        "level": 2,
        "node_payload": {"sos": 1, "source": "button"},
        "snapshot": {"sos": 1},
    },
}


def device_platform_id(device: dict[str, Any]) -> str:
    """IoTDA 设备 ID：未配置真实平台时用 `mock-{sn}`，保证 envelope 能反查到设备（§5.2）。"""
    return device.get("iotda_device_id") or f"mock-{device['sn']}"


def envelope(device: dict[str, Any], resource: str, event: str, services: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "resource": resource,
        "event": event,
        "event_time": _iso(now_ms()),
        "notify_data": {
            "header": {
                "device_id": device_platform_id(device),
                "node_id": device["sn"],
                "product_id": "hmprogram-gateway",
                "app_id": settings.iotda_app_id or "mock-app",
            },
            # body 是「唯一业务数据源」（§5.2），结构与 §4.6 的 services[] 一致
            "body": {"services": services},
        },
    }


def property_envelope(device: dict[str, Any], properties: dict[str, Any]) -> dict[str, Any]:
    return envelope(device, RESOURCE_PROPERTY, "report", [{"service_id": "gateway", "properties": properties}])


def message_envelope(device: dict[str, Any], items: list[dict[str, Any]]) -> dict[str, Any]:
    """§4.6 `messages/up`：`content` 是节点 JSON 数组的**字符串**。"""
    return envelope(
        device,
        RESOURCE_MESSAGE,
        "messages",
        [{"service_id": "gateway", "content": json.dumps(items, ensure_ascii=False)}],
    )


def event_envelope(device: dict[str, Any], event_type: str, paras: dict[str, Any]) -> dict[str, Any]:
    return envelope(device, RESOURCE_EVENT, event_type, [{"service_id": "gateway", "event_type": event_type, "paras": paras}])


def node_item(sn: str, rtype: str, payload: dict[str, Any], rssi: int = -62, battery: int = 255) -> dict[str, Any]:
    """§4.4 节点 JSON：`sn/ts/type/payload`（+ 汇聚补的机内字段 rssi/battery）。"""
    return {"sn": sn, "ts": 0, "type": rtype, "payload": payload, "rssi": rssi, "battery": battery}


def gateway_properties(device: dict[str, Any], nodes_online: dict[str, bool] | None = None) -> dict[str, Any]:
    """§4.3 属性快照（演示用最小值，字段名与契约一致）。"""
    ts = now_ms()
    return {
        "fw_ver": device.get("fw_ver") or "1.0.0",
        "report_interval_s": 60,
        "nodes_online": json.dumps(nodes_online or {"H1": True, "M1": True, "M2": True, "M4": True}),
        "bound_nodes": 4,
        "pair_state": "bound",
        "battery": 255,
        "alarm_state": 0,
        "temp": 24.5,
        "humi": 52.0,
        "last_motion_ts": ts,
        "presence": False,
        "door": False,
        "window": False,
        "smoke": 0,
        "lpg": 120,
    }


def run_demo(device: dict[str, Any], event: str) -> dict[str, Any]:
    """执行一次演示注入，返回处理摘要。

    步骤（与真实链路逐段对应）：
    1. `POST /api/devices/{id}/demo` 已由路由层经 `iotda.dispatch` 下发 `demo_trigger`（记录在流水里）；
    2. 虚拟网关「收到」命令后上报属性快照（`device.property`）与节点明细（`device.message`）；
    3. 汇总成一条聚合告警事件（`device.event` / `alert`）——真实固件同样由汇聚聚合后上报（§4.4）；
    4. 三段 envelope 全部交给 `telemetry.handle_webhook`（线上同一入口）。
    """
    spec = DEMO_EVENTS.get(event)
    if not spec:
        return {"ok": False, "reason": "unsupported_event"}

    node_sn = spec["node_sn"]
    alert_type = spec["type"]
    level = int(spec["level"])
    ts = now_ms()
    event_id = f"{device['sn']}-{alert_type}-{ts}"

    results: list[dict[str, Any]] = []
    results.append(telemetry.handle_webhook(property_envelope(device, gateway_properties(device))))
    results.append(
        telemetry.handle_webhook(
            message_envelope(device, [node_item(node_sn, alert_type, spec["node_payload"])])
        )
    )
    results.append(
        telemetry.handle_webhook(
            event_envelope(
                device,
                "alert",
                {
                    "type": alert_type,
                    "level": level,
                    "node_sn": node_sn,
                    "ts": ts,
                    "event_id": event_id,
                    "snapshot": spec["snapshot"],
                },
            )
        )
    )
    record_command("demo_reported", event_id, device["sn"], "mock://virtual-gateway", {"event": event}, "mock")
    return {"ok": True, "event": event, "event_id": event_id, "node_sn": node_sn, "results": results}


def _iso(ms: int) -> str:
    from datetime import datetime, timezone

    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"
