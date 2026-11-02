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
from ..settings_keys import (
    ALGORITHM_SPECS,
    DEFAULTS,
    KNOWN_KEYS,
    dispatch_targets,
    normalize_payload,
    validate_payload,
)
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
    # dt_to_ms(None) 返回 None，`None < int` 在 Python 3 里直接 TypeError → 500。
    # 出厂贴纸码的有效期可能长到无需过期，所以「没有有效期」必须是合法状态，不是异常。
    expire_ms = dt_to_ms(device["bind_code_expire_at"])
    if expire_ms is not None and expire_ms < now_ms():
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

def _gateway_online(device: dict, row: dict[str, Any]) -> bool:
    """网关是否在线 = 库里的在线位 **且** 心跳没过期（t187，契约 §8.7）。

    为什么要自己再判一次心跳：`scheduler.tick_gateway_offline()` 每 10 s 才把 `online` 置 0，
    在这最多 10 s（调度器停着时更久）的窗口里直接回放 `online=1`，App 会一边显示「主机联系不上」
    （心跳时间已陈旧）一边显示在线。阈值取设备级 `gateway_offline_timeout_s`，与调度器同一个来源
    （`alerts.device_settings`），避免两处口径漂移。没有心跳时间戳按离线处理：没有心跳就无法证明在线。
    """
    if not bool(row.get("online")):
        return False
    last = row.get("last_heartbeat_ms")
    if not last:
        return False
    timeout_s = int(alerts.device_settings(device).get("gateway_offline_timeout_s") or 180)
    return now_ms() - int(last) <= timeout_s * 1000


def _offline_properties(properties: dict[str, Any]) -> dict[str, Any]:
    """设备离线时把属性快照里的在线表整体置 false（t187）。

    离线只说明「现在收不到」，不代表上次的温湿度读数失效，所以只动 `nodes_online` 一个键。
    形状必须跟存储里一致：契约 §4.3 把 `nodes_online` 定义成 **JSON 文本**（字符串），
    但历史数据/测试里也可能直接存成对象，因此按原类型回写，避免把 App 的解析分支悄悄改掉。
    """
    if "nodes_online" not in properties:
        return properties
    raw = properties["nodes_online"]
    table: Any = raw
    if isinstance(raw, str):
        table = db.jload(raw, None)
    if not isinstance(table, dict):
        return properties
    offline = {key: False for key in table}
    return {**properties, "nodes_online": db.jdump(offline) if isinstance(raw, str) else offline}


# §6.7 `nodes[].name` 的兜底表（t198）：现场真机的 `nodes.location` 是 NULL，
# App 首页房间名只能退回通用「房间」；这里先按 sn、再按 type 给一个人话名字。
NODE_NAME_BY_SN: dict[str, str] = {
    "H1": "网关", "M1": "厨房", "M2": "卧室", "M3": "卧室", "M4": "门厅", "M5": "门厅",
}
NODE_NAME_BY_TYPE: dict[str, str] = {
    "hub": "网关", "gas": "厨房", "fall": "卧室", "door": "门厅", "presence": "门厅",
}


def _node_name(row: dict[str, Any]) -> str:
    """节点显示名：库里的 `location` 优先，其次 sn 映射，再次 type 映射，最后退回 sn。"""
    return (
        row.get("location")
        or NODE_NAME_BY_SN.get(str(row.get("sn")))
        or NODE_NAME_BY_TYPE.get(str(row.get("type")))
        or str(row.get("sn") or "")
    )


def _latest_readings(device_id: int) -> dict[str, dict[str, dict[str, Any]]]:
    """每个节点**每种 type** 的最新一条读数（t198），返回 `{sn: {type: {ts, payload}}}`。

    为什么不是每节点一条 SQL：`nodes` 有好几个节点、每节点又有若干 type，N+1 会把首页首屏
    拖成十几条往返。这里一次聚合取全：先 `GROUP BY (node_sn,type)` 取最大 `ts_ms`，再回表取那一行。
    索引：唯一键 `uk_readings(device_id,node_sn,type,ts_ms)` 的最左前缀正好覆盖，无需新增索引。
    """
    rows = db.fetch_all(
        "SELECT r.node_sn, r.type, r.ts_ms, r.payload_json FROM sensor_readings r "
        "JOIN (SELECT node_sn, type, MAX(ts_ms) AS mts FROM sensor_readings "
        "      WHERE device_id = :did GROUP BY node_sn, type) g "
        "  ON g.node_sn = r.node_sn AND g.type = r.type AND g.mts = r.ts_ms "
        "WHERE r.device_id = :did",
        {"did": device_id},
    )
    out: dict[str, dict[str, dict[str, Any]]] = {}
    for row in rows:
        out.setdefault(str(row["node_sn"]), {})[str(row["type"])] = {
            "ts": row["ts_ms"],
            "payload": db.jload(row["payload_json"], {}) or {},
        }
    return out


@router.get("/{device_id}/status")
def device_status(device_id: int, user: CurrentUser) -> dict:
    device = device_context(user, device_id)
    online = online_row(device_id) or {}
    is_online = _gateway_online(device, online)
    properties = db.jload(online.get("last_properties_json"), {}) or {}
    if not is_online:
        # 网关判离线 ⇒ 在线表与节点在线位一律按离线返回：缓存里的 true 只是历史快照，
        # 回放它会让 App 同时显示「主机联系不上」与「4 个在线」（t187 现场实测的缺陷）。
        properties = _offline_properties(properties)
    nodes = db.fetch_all(
        "SELECT sn, type, location, online, last_seen_ms, rssi, battery FROM nodes "
        "WHERE device_id = :did ORDER BY id ASC",
        {"did": device_id},
    )
    latest = _latest_readings(device_id)
    return ok(
        prune_none(
            {
                "device_id": device_id,
                "online": is_online,
                "last_heartbeat": online.get("last_heartbeat_ms"),
                "alarm_state": int(online.get("alarm_state") or 0),
                "properties": properties,
                "nodes": [
                    prune_none(
                        {
                            "sn": row["sn"],
                            "type": row["type"],
                            # kind 给 App 过滤房间行用（t187）：type="hub" 的节点就是汇聚自己，
                            # 不是房间里的传感器；App 只给 kind="sensor" 画行，免得多出一行。
                            # 只增字段不改旧字段，老客户端忽略即可（契约 §6.7 的 type 语义不变）。
                            "kind": "hub" if row["type"] == "hub" else "sensor",
                            "location": row["location"],
                            # t198：App 首页的房间卡直接消费 name 与 latest——
                            # 以前 nodes[] 只有 sn/type/online，App 无论怎么写都拿不到温湿度与燃气值，
                            # 只能显示 `--` 与「正常」。两字段都是**只增不改**，老客户端忽略即可。
                            "name": _node_name(row),
                            "latest": latest.get(str(row["sn"])),
                            "online": bool(row["online"]) and is_online,
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


def _algorithm_items(device: dict) -> dict[str, Any]:
    """`settings_json.algorithm` 里的算法参数（co_t1_mv 等）。

    它们**不进** `_settings_payload` 的 `items`：§6.9 的 items 是 App 消费的 21 键固定形状
    （t171 的原话是「不进入用户可调 SETTINGS_SPEC」，同一条纪律在这里体现为响应形状不变）。
    """
    raw = db.jload(device.get("settings_json"), {}) or {}
    stored = raw.get("algorithm") or {}
    return {k: v for k, v in stored.items() if k in ALGORITHM_SPECS}


@router.get("/{device_id}/settings")
def get_settings(device_id: int, user: CurrentUser) -> dict:
    device = device_context(user, device_id)
    return ok(_settings_payload(device))


@router.put("/{device_id}/settings")
def put_settings(device_id: int, payload: SettingsIn, user: CurrentUser) -> dict:
    device = device_context(user, device_id, need_owner=True)
    items = normalize_payload(payload.model_dump())
    validated, algorithm = validate_payload(items)

    raw = db.jload(device.get("settings_json"), {}) or {}
    merged = dict(DEFAULTS)
    merged.update(raw.get("items") or {})
    merged.update(validated)
    # source 回到 local：改动还只在服务器上，未下发到设备（§6.11 的观察口径）
    stored = {"items": merged, "source": "local", "updated_at": now_ms()}
    if algorithm or raw.get("algorithm"):
        # 算法参数与 items 分开存：既能持久化又不污染 §6.9 的 21 键响应
        algo_now = dict(raw.get("algorithm") or {})
        algo_now.update(algorithm)
        stored["algorithm"] = algo_now
    db.execute("UPDATE devices SET settings_json = :s WHERE id = :did", {"s": db.jdump(stored), "did": device_id})
    log.info("设备 %s 阈值更新：%s（算法参数 %s）", device["sn"], sorted(validated), sorted(algorithm))
    return ok(_settings_payload({**device, "settings_json": db.jdump(stored)}))


@router.post("/{device_id}/config")
def post_config(device_id: int, payload: ConfigIn, user: CurrentUser) -> dict:
    device = device_context(user, device_id, need_owner=True)
    settings_payload = _settings_payload(device)
    items: dict[str, Any] = settings_payload["items"]

    algorithm = _algorithm_items(device)
    requested = payload.keys
    if requested:
        # §6.10：认得出的键 = 21 个可调项 + 已登记的算法参数（co_t1_mv），其余才是 1003
        unknown = [key for key in requested if key not in DEFAULTS and key not in ALGORITHM_SPECS]
        if unknown:
            raise ApiError(1003, field=unknown[0], allowed=KNOWN_KEYS)
        selected = {}
        for key in requested:
            selected[key] = items[key] if key in items else algorithm.get(key, ALGORITHM_SPECS[key].default)
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
    # PM 裁定（契约 v1.2 D-05，939/942 行）：accepted/rejected 是 data 里的两个**字符串数组**
    # （元素=键名）；被拒原因另放可选的 `reasons`（键名 → 原因），没有拒绝项就不带这个键（§0.4）。
    result: dict[str, Any] = {
        "command_id": f"cmd-{dispatched_at or now_ms()}-{device_id}",
        "request_id": request_ids[0] if request_ids else iotda.new_request_id(),
        "accepted": sorted(accepted),
        "rejected": sorted(rejected),
        "dispatched_at": dispatched_at or now_ms(),
        "timeout_s": iotda.COMMAND_TIMEOUT_S,
    }
    if rejected:
        result["reasons"] = {k: rejected[k] for k in sorted(rejected)}
    return ok(result)


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
            # 与 §6.11 同口径（PM 裁定 D-05）：accepted/rejected 是字符串数组（元素=键名）
            "accepted": ["demo_trigger"],
            "rejected": [],
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
