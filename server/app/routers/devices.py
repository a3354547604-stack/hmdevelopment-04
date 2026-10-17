"""设备：列表 / 绑定 / 实时状态 / 历史读数 / 阈值配置 / 下发 / 演示（契约 §6.5–§6.12）。

权限模型（§7.4）：一切以 `user_devices`（且 `deleted_at IS NULL`）为准——
`devices.owner_id` 只是首位绑定者的快照，改设置/演示/解绑要求 `role='owner'`。
"""
from __future__ import annotations

import logging
import random
import re
from typing import Any

from fastapi import APIRouter, Query
from pydantic import BaseModel

from .. import db
from ..deps import CurrentUser, device_context, online_row
from ..errors import ApiError, ok
from ..mocks import virtual
from ..payload import prune_none
from ..services import alerts, iotda
from ..settings_keys import DEFAULTS, dispatch_targets, normalize_payload, validate_items
from ..timeutil import dt_to_ms, ms_to_dt, now_ms

log = logging.getLogger("app.devices")
router = APIRouter(prefix="/api/devices", tags=["devices"])

SN_RE = re.compile(r"^[A-Za-z0-9_-]{3,32}$")
BIND_CODE_TTL_MIN = 10  # §6.6


class BindIn(BaseModel):
    sn: str
    bind_code: str
    name: str | None = None


class SettingsIn(BaseModel):
    items: dict[str, Any] | None = None


class ConfigIn(BaseModel):
    keys: list[str] | None = None


class DemoIn(BaseModel):
    event: str


# --------------------------------------------------------------------------- 列表与绑定

@router.get("")
def list_devices(user: CurrentUser) -> dict:
    rows = db.fetch_all(
        "SELECT d.id, d.sn, d.name, ud.role, o.online, o.last_heartbeat_ms, o.alarm_state "
        "FROM user_devices ud JOIN devices d ON d.id = ud.device_id "
        "LEFT JOIN device_online o ON o.device_id = d.id "
        "WHERE ud.user_id = :uid AND ud.deleted_at IS NULL ORDER BY d.id ASC",
        {"uid": user["id"]},
    )
    items = []
    for row in rows:
        items.append(
            prune_none(
                {
                    "id": int(row["id"]),
                    "sn": row["sn"],
                    "name": row["name"],
                    "role": row["role"],
                    # 180 s 规则由调度器写回 device_online.online（§8.7），列表只读现成结论
                    "online": bool(row["online"]) if row["online"] is not None else False,
                    "last_heartbeat": row["last_heartbeat_ms"],
                    "alarm_state": int(row["alarm_state"] or 0),
                    "unread_alerts": alerts.count_pending(int(row["id"])),
                }
            )
        )
    return ok({"total": len(items), "items": items})


@router.post("/bind", status_code=201)
def bind_device(payload: BindIn, user: CurrentUser) -> dict:
    sn = (payload.sn or "").strip()
    if not SN_RE.match(sn):
        raise ApiError(1001, field="sn")
    code = (payload.bind_code or "").strip()
    if not (code.isdigit() and len(code) == 6):
        raise ApiError(1001, field="bind_code", expected="6 位数字")

    device = db.fetch_one("SELECT * FROM devices WHERE sn = :sn", {"sn": sn})
    if not device:
        raise ApiError(1200, field="sn")

    existing = db.fetch_one(
        "SELECT * FROM user_devices WHERE user_id = :uid AND device_id = :did", {"uid": user["id"], "did": device["id"]}
    )
    if existing and existing["deleted_at"] is None:
        # 重复绑定同一台设备：幂等返回，不覆盖已有 role
        return ok({"device_id": int(device["id"]), "sn": sn, "role": existing["role"]})

    other_owner = db.fetch_one(
        "SELECT id FROM user_devices WHERE device_id = :did AND role = 'owner' AND deleted_at IS NULL",
        {"did": device["id"]},
    )
    if not device["bind_code"]:
        # 绑定码一次性：已被别人绑走且没有新码 → 1202（owner 需在 App 里重新生成绑定码邀请家人）
        if other_owner:
            raise ApiError(1202)
        raise ApiError(1201, field="bind_code", note="设备未预置绑定码或绑定码已使用")
    if dt_to_ms(device["bind_code_expire_at"]) < now_ms():
        raise ApiError(1203, field="bind_code")

    role = "owner" if not other_owner else "family"
    if device["bind_code"] != code:
        raise ApiError(1201, field="bind_code")

    if existing:
        db.execute(
            "UPDATE user_devices SET deleted_at = NULL, role = :role WHERE id = :id",
            {"role": role, "id": existing["id"]},
        )
    else:
        db.insert(
            "INSERT INTO user_devices (user_id, device_id, role) VALUES (:uid, :did, :role)",
            {"uid": user["id"], "did": device["id"], "role": role},
        )

    if role == "owner":
        # 绑定码一次性：绑定成功后清空（§7.3「绑定后清空」）
        db.execute(
            "UPDATE devices SET name = COALESCE(:name, name), owner_id = :uid, bind_code = NULL, "
            "bind_code_expire_at = NULL WHERE id = :did",
            {"name": payload.name, "uid": user["id"], "did": device["id"]},
        )
    log.info("用户 %s 绑定设备 %s（role=%s）", user["username"], sn, role)
    return ok({"device_id": int(device["id"]), "sn": sn, "role": role})


# --------------------------------------------------------------------------- 状态与读数

@router.get("/{device_id}/status")
def device_status(device_id: int, user: CurrentUser) -> dict:
    device = device_context(user, device_id)
    online = online_row(device_id) or {}
    properties = db.jload(online.get("last_properties_json"), {}) or {}
    nodes = db.fetch_all(
        "SELECT sn, type, location, online, last_seen_ms, rssi, battery FROM nodes "
        "WHERE device_id = :did ORDER BY id ASC",
        {"did": device_id},
    )
    return ok(
        prune_none(
            {
                "device_id": device_id,
                "online": bool(online.get("online")),
                "last_heartbeat": online.get("last_heartbeat_ms"),
                "alarm_state": int(online.get("alarm_state") or 0),
                "properties": properties,
                "nodes": [
                    prune_none(
                        {
                            "sn": row["sn"],
                            "type": row["type"],
                            "location": row["location"],
                            "online": bool(row["online"]),
                            "last_seen": row["last_seen_ms"],
                            "rssi": row["rssi"],
                            "battery": row["battery"],
                        }
                    )
                    for row in nodes
                ],
                "pending_alerts": alerts.count_pending(device_id),
                "server_ts": now_ms(),
            }
        )
    )


@router.get("/{device_id}/readings")
def device_readings(
    device_id: int,
    user: CurrentUser,
    type: str | None = Query(default=None),
    node_sn: str | None = Query(default=None),
    from_ms: int | None = Query(default=None, alias="from"),
    to_ms: int | None = Query(default=None, alias="to"),
    limit: int = Query(default=50, ge=1, le=200),
) -> dict:
    device_context(user, device_id)
    to_value = to_ms if to_ms is not None else now_ms()
    from_value = from_ms if from_ms is not None else to_value - 24 * 3600 * 1000
    if from_value > to_value:
        raise ApiError(1004, note="from 必须不晚于 to")

    where = ["device_id = :did", "ts_ms BETWEEN :f AND :t"]
    params: dict[str, Any] = {"did": device_id, "f": from_value, "t": to_value, "limit": limit}
    if type:
        where.append("type = :type")
        params["type"] = type
    if node_sn:
        where.append("node_sn = :sn")
        params["sn"] = node_sn
    clause = " AND ".join(where)
    total = int(db.fetch_scalar(f"SELECT COUNT(*) FROM sensor_readings WHERE {clause}", params, 0) or 0)
    rows = db.fetch_all(
        f"SELECT id, node_sn, type, ts_ms, payload_json FROM sensor_readings WHERE {clause} "
        "ORDER BY ts_ms DESC, id DESC LIMIT :limit",
        params,
    )
    return ok(
        {
            "total": total,
            "items": [
                {
                    "id": int(row["id"]),
                    "node_sn": row["node_sn"],
                    "type": row["type"],
                    "ts": row["ts_ms"],
                    "payload": db.jload(row["payload_json"], {}),
                }
                for row in rows
            ],
        }
    )


# --------------------------------------------------------------------------- 配置

def _settings_payload(device: dict) -> dict:
    """§6.9 的 `{source, updated_at, items}`。

    `updated_at` 取「这套参数开始生效的时刻」：改过就是改动时刻；从未改过则回落到设备入库时间。
    这样 App 端始终能显示一个真实时间，也避免 §0.4 禁止的 null/缺键（`updated_at` 是必需字段）。
    """
    raw = db.jload(device.get("settings_json"), {}) or {}
    items = dict(DEFAULTS)
    items.update(raw.get("items") or {})
    updated_at = raw.get("updated_at")
    if updated_at is None:
        updated_at = dt_to_ms(device.get("created_at")) if device.get("created_at") else now_ms()
    return {
        "source": raw.get("source") or "local",
        "updated_at": updated_at,
        "items": items,
    }


@router.get("/{device_id}/settings")
def get_settings(device_id: int, user: CurrentUser) -> dict:
    device = device_context(user, device_id)
    return ok(_settings_payload(device))


@router.put("/{device_id}/settings")
def put_settings(device_id: int, payload: SettingsIn, user: CurrentUser) -> dict:
    device = device_context(user, device_id, need_owner=True)
    items = normalize_payload(payload.model_dump())
    validated = validate_items(items)

    raw = db.jload(device.get("settings_json"), {}) or {}
    merged = dict(DEFAULTS)
    merged.update(raw.get("items") or {})
    merged.update(validated)
    # source 回到 local：改动还只在服务器上，未下发到设备（§6.11 的观察口径）
    stored = {"items": merged, "source": "local", "updated_at": now_ms()}
    db.execute("UPDATE devices SET settings_json = :s WHERE id = :did", {"s": db.jdump(stored), "did": device_id})
    log.info("设备 %s 阈值更新：%s", device["sn"], sorted(validated))
    return ok(_settings_payload({**device, "settings_json": db.jdump(stored)}))


@router.post("/{device_id}/config")
def post_config(device_id: int, payload: ConfigIn, user: CurrentUser) -> dict:
    device = device_context(user, device_id, need_owner=True)
    settings_payload = _settings_payload(device)
    items: dict[str, Any] = settings_payload["items"]

    requested = payload.keys
    if requested:
        unknown = [key for key in requested if key not in DEFAULTS]
        if unknown:
            raise ApiError(1003, field=unknown[0], allowed=sorted(DEFAULTS))
        selected = {key: items[key] for key in requested}
    else:
        selected = dict(items)

    accepted, rejected = dispatch_targets(selected)
    dispatched_at = None
    request_ids: list[str] = []
    for key, value in accepted.items():
        result = iotda.dispatch_settings_key(device, key, value)
        request_ids.append(result["request_id"])
        dispatched_at = result["dispatched_at"]

    if accepted:
        raw = db.jload(device.get("settings_json"), {}) or {}
        # source=device 表示「已投递到设备」（不是已生效）：设备回报后才由 webhook 改回
        stored = {"items": raw.get("items") or dict(DEFAULTS), "source": "device", "updated_at": now_ms()}
        db.execute("UPDATE devices SET settings_json = :s WHERE id = :did",
                   {"s": db.jdump(stored), "did": device_id})

    if rejected:
        log.warning("设备 %s 有 %d 个键不可下发：%s", device["sn"], len(rejected),
                    {k: v for k, v in sorted(rejected.items())})
    # §6.11：绝不返回「已成功」——accepted 只表示已投递到 IoTDA。
    # PM 裁定（契约 v1.2）：accepted/rejected 是 data 里的两个**整型**计数，不返回数组明细。
    return ok(
        {
            "command_id": f"cmd-{dispatched_at or now_ms()}-{device_id}",
            "request_id": request_ids[0] if request_ids else iotda.new_request_id(),
            "accepted": len(accepted),
            "rejected": len(rejected),
            "dispatched_at": dispatched_at or now_ms(),
            "timeout_s": iotda.COMMAND_TIMEOUT_S,
        }
    )


@router.post("/{device_id}/demo")
def post_demo(device_id: int, payload: DemoIn, user: CurrentUser) -> dict:
    """演示注入（§6.12）：先下发 `demo_trigger`，再由虚拟网关把结果回灌到真实 webhook 链路。"""
    device = device_context(user, device_id, need_owner=True)
    event = (payload.event or "").strip().lower()
    if event not in virtual.DEMO_EVENTS:
        raise ApiError(1206, allowed=sorted(virtual.DEMO_EVENTS))

    dispatched = iotda.dispatch(device, "demo_trigger", {"event": event}, timeout_s=iotda.COMMAND_TIMEOUT_S)
    outcome = virtual.run_demo(device, event)

    alert_row = None
    if outcome.get("ok"):
        alert_row = db.fetch_one("SELECT * FROM alerts WHERE event_id = :e", {"e": outcome["event_id"]})
        if not alert_row:
            # 命中 §8.8 去重窗口（30 s 内同类型重复）：本轮不新建告警，
            # 但演示仍要展示「当前生效的那条」，因此回查最近一条同类型告警。
            alert_row = db.fetch_one(
                "SELECT * FROM alerts WHERE device_id = :d AND type = :t ORDER BY ts_ms DESC, id DESC LIMIT 1",
                {"d": device_id, "t": virtual.DEMO_EVENTS[event]["type"]},
            )
    alert_data = None
    if alert_row:
        # 演示是「老人先触发、家属立刻收到」的场景，因此不等 0 s 定时器直接推第一次；
        # 已在去重窗口内的告警（push_count>0）不重复推送，避免演示连点把推送计数刷高。
        if not int(alert_row["push_count"] or 0):
            alert_row = alerts.push_alert(alert_row)
        # 形状与 §6.13 列表项一致（含 summary），App 演示页可直接复用同一渲染
        alert_data = alerts.alert_item(alert_row, alerts.device_settings(device))

    return ok(
        {
            "command_id": dispatched["command_id"],
            "request_id": dispatched["request_id"],
            # 与 §6.11 同口径（PM 裁定：accepted/rejected 为整型计数）
            "accepted": 1,
            "rejected": 0,
            "dispatched_at": dispatched["dispatched_at"],
            "timeout_s": dispatched["timeout_s"],
            "event": event,
            # 附加字段（契约 §6.12 未列）：便于 curl 演示与验收脚本断言链路真的走通了
            "alert": alert_data,
            "source": "mock-virtual-gateway" if dispatched["mode"] == "mock" else "iotda",
        }
    )


# --------------------------------------------------------------------------- 绑定码（施工/演示用）

def new_bind_code() -> tuple[str, str]:
    """生成 6 位绑定码与过期时刻（供 CLI/测试；生产由施工流程写入）。"""
    code = f"{random.randint(0, 999999):06d}"
    expire = ms_to_dt(now_ms() + BIND_CODE_TTL_MIN * 60 * 1000)
    return code, expire
