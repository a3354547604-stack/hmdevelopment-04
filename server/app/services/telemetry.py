"""webhook 入口业务实现（契约 §5.2 信封、§5.3 入库映射）。

实现约束（§5.2 逐条）：
- 只依赖 `resource` + `notify_data.header.device_id` + `notify_data.body.services[]`；
- 无法识别的 `resource` **返回 200 并记 WARN**（不阻断平台重试队列）；
- 缺 `device_id` 时用 SN 兼容匹配（`node_id` / 属性里的 `device_sn`）；
- 平台重试靠 `event_id` 与 `sensor_readings` 唯一键幂等，不靠「只处理一次」。
"""
from __future__ import annotations

import json
import logging
import re
from typing import Any

from .. import db
from ..config import settings
from ..state import record_webhook
from ..timeutil import dt_to_ms, ms_to_dt, now_ms  # noqa: F401
from . import alerts

log = logging.getLogger("app.telemetry")

# 节点编号形如 M1/M2/H1（§0.7）
_NODE_SN_RE = re.compile(r"^[HM]\d{1,2}$")

RESOURCE_PROPERTY = "device.property"
RESOURCE_EVENT = "device.event"
RESOURCE_MESSAGE = "device.message"


# --------------------------------------------------------------------------- 解析辅助

def _services(body: dict[str, Any]) -> list[dict[str, Any]]:
    services = body.get("services")
    return services if isinstance(services, list) else []


def _properties(body: dict[str, Any]) -> dict[str, Any]:
    merged: dict[str, Any] = {}
    for service in _services(body):
        props = service.get("properties")
        if isinstance(props, dict):
            merged.update(props)
    return merged


def _resolve_device(header: dict[str, Any], body: dict[str, Any]) -> dict[str, Any] | None:
    device_id = header.get("device_id")
    if device_id:
        row = db.fetch_one(
            "SELECT * FROM devices WHERE iotda_device_id = :iid", {"iid": str(device_id)}
        )
        if row:
            return row
    # SN 兼容匹配：P0 手工配置规则引擎时可能只带 node_id
    props = _properties(body)
    for candidate in (header.get("node_id"), props.get("device_sn"), props.get("gateway_sn")):
        if candidate:
            row = db.fetch_one("SELECT * FROM devices WHERE sn = :sn", {"sn": str(candidate)})
            if row:
                log.warning("webhook 缺少 device_id，已按 SN=%s 兼容匹配", candidate)
                return row
    return None


def _reading_payload(payload: Any) -> str:
    return db.jdump(payload if payload is not None else {})


def _insert_reading(device_id: int, node_sn: str, rtype: str, ts_ms: int, payload: Any) -> bool:
    """唯一键 (device_id, node_sn, type, ts_ms) 幂等：重复上报不产生第二行。"""
    with db.conn() as connection:
        result = connection.execute(
            db.text(
                "INSERT IGNORE INTO sensor_readings (device_id, node_sn, type, ts, ts_ms, payload_json) "
                "VALUES (:d, :n, :t, :ts, :ts_ms, :p)"
            ),
            {
                "d": device_id,
                "n": node_sn,
                "t": rtype,
                "ts": ms_to_dt(ts_ms),
                "ts_ms": ts_ms,
                "p": _reading_payload(payload),
            },
        )
        return bool(result.rowcount)


def _touch_device_online(device_id: int, ts_ms: int, alarm_state: int | None = None,
                         properties: dict[str, Any] | None = None) -> None:
    db.execute(
        "INSERT INTO device_online (device_id, online, last_heartbeat, last_heartbeat_ms, "
        # alarm_state 是 NOT NULL：显式传 NULL 会绕过列默认值直接报 1048，所以 INSERT 侧先 COALESCE 成 0
        "last_properties_json, alarm_state) VALUES (:d, 1, :hb, :hb_ms, :props, COALESCE(:alarm, 0)) "
        "ON DUPLICATE KEY UPDATE online = 1, last_heartbeat = :hb, last_heartbeat_ms = :hb_ms, "
        "last_properties_json = COALESCE(:props, last_properties_json), alarm_state = COALESCE(:alarm, alarm_state)",
        {
            "d": device_id,
            "hb": ms_to_dt(ts_ms),
            "hb_ms": ts_ms,
            "props": db.jdump(properties) if properties is not None else None,
            "alarm": alarm_state,
        },
    )


def _update_nodes(device_id: int, entries: dict[str, dict[str, Any]], ts_ms: int) -> int:
    """entries: {sn: {"online":bool, "rssi":int|None, "battery":int|None, "last_seen_ms":int|None}}"""
    updated = 0
    for sn, info in entries.items():
        seen = info.get("last_seen_ms") or ts_ms
        db.execute(
            "INSERT INTO nodes (device_id, sn, type, online, last_seen, last_seen_ms, rssi, battery) "
            "VALUES (:d, :sn, :type, :online, :seen, :seen_ms, :rssi, :battery) "
            "ON DUPLICATE KEY UPDATE online = :online, last_seen = :seen, last_seen_ms = :seen_ms, "
            "rssi = COALESCE(:rssi, rssi), battery = COALESCE(:battery, battery)",
            {
                "d": device_id,
                "sn": sn,
                "type": info.get("type") or _node_type(sn),
                "online": 1 if info.get("online") else 0,
                "seen": ms_to_dt(seen),
                "seen_ms": seen,
                "rssi": info.get("rssi"),
                "battery": info.get("battery"),
            },
        )
        updated += 1
    return updated


_NODE_TYPES = {"H1": "hub", "M1": "gas", "M2": "fall", "M3": "fall", "M4": "presence", "M5": "presence"}


def _node_type(sn: str) -> str:
    return _NODE_TYPES.get((sn or "").split("-")[0], "unknown")


# --------------------------------------------------------------------------- 三类资源

def handle_property(device: dict[str, Any], body: dict[str, Any]) -> int:
    props = _properties(body)
    ts = now_ms()
    alarm_state = props.get("alarm_state")
    _touch_device_online(device["id"], ts, int(alarm_state) if alarm_state is not None else None, props)

    # nodes_online 是 JSON 文本（§4.3），兼容平台把对象直接给过来
    nodes_online = props.get("nodes_online")
    if isinstance(nodes_online, str):
        try:
            nodes_online = json.loads(nodes_online)
        except ValueError:
            log.warning("nodes_online 不是合法 JSON：%r", nodes_online[:80])
            nodes_online = None
    handled = 0
    if isinstance(nodes_online, dict):
        handled = _update_nodes(
            device["id"], {sn: {"online": bool(flag)} for sn, flag in nodes_online.items()}, ts
        )

    # §5.3：属性快照整体入 sensor_readings，node_sn="M0"、type="gateway"
    _insert_reading(device["id"], "M0", "gateway", ts, props)
    # 收到任意上报即视为网关在线：清掉未恢复的 device_offline 告警
    alerts.mark_recovered(device["id"], "device_offline", None, ts)
    return handled


def handle_event(device: dict[str, Any], body: dict[str, Any], envelope_event: str | None) -> int:
    services = _services(body)
    service = services[0] if services else {}
    event_type = service.get("event_type") or envelope_event
    paras = service.get("paras") if isinstance(service.get("paras"), dict) else {}
    ts = int(paras.get("ts") or now_ms())
    _touch_device_online(device["id"], now_ms())
    cfg = alerts.device_settings(device)

    if event_type == "sos":
        # §4.4 sos paras 只有 source：它可能就是一个节点号（M4 按键），也可能是按钮/App。
        # 是节点号就写进 node_sn，让 App 能显示来源；不是就留空（§0.4，不编造）。
        source = paras.get("source")
        node_sn = paras.get("node_sn") or (source if isinstance(source, str) and _NODE_SN_RE.match(source) else None)
        _, created = alerts.raise_alert(
            device, "sos", 2, node_sn, paras, ts_ms=ts, event_id=paras.get("event_id"), cfg=cfg
        )
        return 1 if created else 0

    if event_type == "alert":
        alert_type = paras.get("type")
        if not alert_type:
            log.warning("alert 事件缺少 type：%s", paras)
            return 0
        level = int(paras.get("level") or 1)
        _, created = alerts.raise_alert(
            device,
            alert_type,
            level,
            paras.get("node_sn"),
            paras.get("snapshot"),
            ts_ms=ts,
            event_id=paras.get("event_id"),
            cfg=cfg,
        )
        return 1 if created else 0

    if event_type == "recover":
        alert_type = paras.get("type")
        if not alert_type:
            log.warning("recover 事件缺少 type：%s", paras)
            return 0
        node_sn = paras.get("node_sn")
        if alert_type in ("node_online", "node_offline"):
            # §8.7：节点恢复 → nodes.online=1；同时清掉 node_offline 告警
            target = node_sn or "M1"
            db.execute(
                "UPDATE nodes SET online = 1, last_seen = :seen, last_seen_ms = :seen_ms "
                "WHERE device_id = :d AND sn = :sn",
                {"seen": ms_to_dt(ts), "seen_ms": ts, "d": device["id"], "sn": target},
            )
            return alerts.mark_recovered(device["id"], "node_offline", target, ts)
        return alerts.mark_recovered(device["id"], alert_type, node_sn, ts)

    log.warning("未知 event_type=%s，body=%s", event_type, json.dumps(body, ensure_ascii=False)[:200])
    return 0


def handle_message(device: dict[str, Any], body: dict[str, Any]) -> int:
    """节点明细（§4.6 `messages/up`）：`content` 是节点 JSON 数组的字符串。"""
    services = _services(body)
    if not services:
        return 0
    content = services[0].get("content")
    items: list[Any]
    if isinstance(content, str):
        try:
            items = json.loads(content)
        except ValueError:
            log.warning("messages content 不是合法 JSON：%r", content[:120])
            return 0
    elif isinstance(content, list):
        items = content
    else:
        items = []
    if isinstance(items, dict):
        items = [items]

    now = now_ms()
    inserted = 0
    for index, item in enumerate(items):
        if not isinstance(item, dict) or not item.get("sn"):
            continue
        raw_ts = item.get("ts")
        # CAL-09：节点无 RTC，ts 是自开机毫秒数 → clock_source=server 时按服务器接收时刻纠正。
        # 同一批多条用 +index 毫秒错开，保证 (device,node,type,ts_ms) 唯一键不互相覆盖。
        ts = (now + index) if settings.clock_source == "server" else int(raw_ts or now) + index
        node_sn = str(item["sn"])
        rtype = str(item.get("type") or "status")
        payload = item.get("payload") if isinstance(item.get("payload"), dict) else {}
        if _insert_reading(device["id"], node_sn, rtype, ts, payload):
            inserted += 1
        _update_nodes(
            device["id"],
            {
                node_sn: {
                    "online": True,
                    "rssi": item.get("rssi"),
                    "battery": item.get("battery"),
                    "last_seen_ms": ts,
                }
            },
            ts,
        )
    _touch_device_online(device["id"], now)
    alerts.mark_recovered(device["id"], "device_offline", None, now)
    return inserted


def handle_webhook(body: dict[str, Any]) -> dict[str, Any]:
    """统一入口；返回处理摘要（路由据此决定响应体）。"""
    resource = body.get("resource")
    notify = body.get("notify_data") if isinstance(body.get("notify_data"), dict) else {}
    header = notify.get("header") if isinstance(notify.get("header"), dict) else {}
    body_data = notify.get("body") if isinstance(notify.get("body"), dict) else {}

    device = _resolve_device(header, body_data)
    if not device:
        log.warning("webhook 无法匹配设备，header=%s（返回 200，不阻断平台重试）", header)
        return {"handled": 0, "resource": resource, "device_id": None, "warnings": ["device_not_found"]}

    record_webhook(resource)
    envelope_event = body.get("event") if isinstance(body.get("event"), str) else None

    if resource == RESOURCE_PROPERTY:
        handled = handle_property(device, body_data)
    elif resource == RESOURCE_EVENT:
        handled = handle_event(device, body_data, envelope_event)
    elif resource == RESOURCE_MESSAGE:
        handled = handle_message(device, body_data)
    else:
        # §5.2：无法识别的 resource 记 WARN 并返回 200
        log.warning("未知 resource=%s，已忽略（body 前 200 字：%s）", resource,
                    json.dumps(body, ensure_ascii=False)[:200])
        return {"handled": 0, "resource": resource, "device_id": device["id"], "warnings": ["unknown_resource"]}

    return {
        "handled": handled,
        "resource": resource,
        "device_id": device["id"],
        "sn": device["sn"],
        "warnings": [],
    }
