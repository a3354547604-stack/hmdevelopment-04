"""告警生命周期：去重/升级/重推/恢复/处置（契约 §6.13–§6.15、§7.7、§7.8、§8.8）。

关键规则（逐条对应契约）：
- 幂等：`event_id` 唯一键（§4.4），重复上报只入库一次；
- 去重窗口 `(device_id, node_sn, type)`：fall 用 `fall_suppress_s`(30 s)，其余用 `alert_suppress_s`(60 s)（§8.8）；
- 窗口内重复：**不新建**，只更新 `snapshot_json` 并把 `recovered_at` 置回 NULL；`push_count`/`next_push_at` 不动（§8.8）；
- 窗口内级别上升（1→2）：新建 `level=2` 一条，旧条置 `recovered_at=now`（§8.8）；
- 推送节奏 `[0, 60, 300]`，`push_count>=3` → `next_push_at=NULL` 永不再推（§7.7）；
- 恢复事件只写 `recovered_at` 并停推，**不自动改 status**（§8.4），闭环由人工处置结束（§6.15）。
"""
from __future__ import annotations

import logging
from typing import Any

from .. import db
from ..errors import ApiError
from ..payload import prune_none
from ..settings_keys import DEFAULTS
from ..summary import alert_summary
from ..timeutil import dt_to_ms, ms_to_dt, now_ms  # noqa: F401

log = logging.getLogger("app.alerts")

REPUSH_SCHEDULE = (0, 60, 300)  # §8.8 / §7.7 的默认节奏；设备可用 alert_repush_s 覆盖
ACTIONS = ("contacted", "checked", "false_alarm", "other")


# --------------------------------------------------------------------------- 配置

def device_settings(device: dict) -> dict[str, Any]:
    """设备级设置：`devices.settings_json.items`（未设置过则用契约默认值）。"""
    raw = db.jload(device.get("settings_json"), {}) or {}
    items = dict(DEFAULTS)
    items.update(raw.get("items") or {})
    return items


def suppress_window(alert_type: str, cfg: dict[str, Any]) -> int:
    if alert_type == "fall":
        return int(cfg.get("fall_suppress_s", 30))
    return int(cfg.get("alert_suppress_s", 60))


# --------------------------------------------------------------------------- 读

def alert_item(row: dict, cfg: dict[str, Any]) -> dict[str, Any]:
    """§6.13 列表项：字段名与顺序按契约；未恢复/未推送的键直接省略（§0.4 禁 null）。"""
    return prune_none({
        "id": row["id"],
        "device_id": row["device_id"],
        "node_sn": row["node_sn"],
        "type": row["type"],
        "level": row["level"],
        "ts": row["ts_ms"],
        "status": row["status"],
        "recovered_at": dt_to_ms(row["recovered_at"]),
        "push_count": row["push_count"],
        "last_push_at": dt_to_ms(row["last_push_at"]),
        "next_push_at": dt_to_ms(row["next_push_at"]),
        "summary": alert_summary(row["type"], row["level"], row["node_sn"], settings=cfg),
    })


def count_pending(device_id: int) -> int:
    return int(db.fetch_scalar(
        "SELECT COUNT(*) FROM alerts WHERE device_id = :d AND status = 'pending'", {"d": device_id}, 0
    ) or 0)


def list_alerts(
    device_id: int,
    status: str = "all",
    alert_type: str | None = None,
    from_ms: int | None = None,
    to_ms: int | None = None,
    limit: int = 50,
    offset: int = 0,
) -> dict[str, Any]:
    where = ["device_id = :d"]
    params: dict[str, Any] = {"d": device_id, "limit": limit, "offset": offset}
    if status and status != "all":
        where.append("status = :st")
        params["st"] = status
    if alert_type:
        where.append("type = :ty")
        params["ty"] = alert_type
    if from_ms is not None:
        where.append("ts_ms >= :f")
        params["f"] = from_ms
    if to_ms is not None:
        where.append("ts_ms <= :t")
        params["t"] = to_ms
    clause = " AND ".join(where)
    total = int(db.fetch_scalar(f"SELECT COUNT(*) FROM alerts WHERE {clause}", params, 0) or 0)
    rows = db.fetch_all(
        f"SELECT * FROM alerts WHERE {clause} ORDER BY ts_ms DESC, id DESC LIMIT :limit OFFSET :offset",
        params,
    )
    device = db.fetch_one("SELECT * FROM devices WHERE id = :d", {"d": device_id})
    cfg = device_settings(device) if device else dict(DEFAULTS)
    return {"total": total, "items": [alert_item(row, cfg) for row in rows]}


def get_alert_row(alert_id: int) -> dict:
    row = db.fetch_one("SELECT * FROM alerts WHERE id = :id", {"id": alert_id})
    if not row:
        raise ApiError(1300)
    return row


def detail(alert_id: int, cfg: dict[str, Any]) -> dict[str, Any]:
    row = get_alert_row(alert_id)
    ts_ms = row["ts_ms"]
    dispositions = db.fetch_all(
        "SELECT d.id, d.user_id, u.username, d.action, d.note, d.created_at "
        "FROM alert_dispositions d LEFT JOIN users u ON u.id = d.user_id "
        "WHERE d.alert_id = :id ORDER BY d.created_at ASC, d.id ASC",
        {"id": alert_id},
    )
    before = db.fetch_all(
        "SELECT id, node_sn, type, ts_ms, payload_json FROM sensor_readings "
        "WHERE device_id = :d AND ts_ms <= :ts ORDER BY ts_ms DESC LIMIT 5",
        {"d": row["device_id"], "ts": ts_ms},
    )
    after = db.fetch_all(
        "SELECT id, node_sn, type, ts_ms, payload_json FROM sensor_readings "
        "WHERE device_id = :d AND ts_ms > :ts ORDER BY ts_ms ASC LIMIT 5",
        {"d": row["device_id"], "ts": ts_ms},
    )

    def reading(r: dict) -> dict:
        return {
            "id": r["id"],
            "node_sn": r["node_sn"],
            "type": r["type"],
            "ts": r["ts_ms"],
            "payload": db.jload(r["payload_json"], {}),
        }

    data = prune_none({
        "id": row["id"],
        "device_id": row["device_id"],
        "node_sn": row["node_sn"],
        "type": row["type"],
        "level": row["level"],
        "ts": row["ts_ms"],
        "event_id": row["event_id"],
        "status": row["status"],
        "recovered_at": dt_to_ms(row["recovered_at"]),
        "snapshot": db.jload(row["snapshot_json"], None),
        "push_count": row["push_count"],
        "last_push_at": dt_to_ms(row["last_push_at"]),
        "next_push_at": dt_to_ms(row["next_push_at"]),
        "created_at": dt_to_ms(row["created_at"]),
        "summary": alert_summary(row["type"], row["level"], row["node_sn"], settings=cfg),
        "dispositions": [
            prune_none(
                {
                    "id": d["id"],
                    "user_id": d["user_id"],
                    "username": d["username"],
                    "action": d["action"],
                    "note": d["note"],
                    "at": dt_to_ms(d["created_at"]),
                }
            )
            for d in dispositions
        ],
        "readings_before": [reading(r) for r in reversed(before)],
        "readings_after": [reading(r) for r in after],
    })
    return data


def confirm_on_read(alert_id: int) -> None:
    """§6.15 状态机：App 打开详情页 → `pending` 置 `confirmed`（停推，但不算闭环）。"""
    db.execute(
        "UPDATE alerts SET status = 'confirmed', next_push_at = NULL "
        "WHERE id = :id AND status = 'pending'",
        {"id": alert_id},
    )


# --------------------------------------------------------------------------- 写

def raise_alert(
    device: dict,
    alert_type: str,
    level: int,
    node_sn: str | None = None,
    snapshot: Any = None,
    ts_ms: int | None = None,
    event_id: str | None = None,
    cfg: dict[str, Any] | None = None,
) -> tuple[dict[str, Any] | None, bool]:
    """立一条告警（含幂等、去重窗口、升级）。返回 (告警行, 是否新建)。"""
    cfg = cfg or device_settings(device)
    ts = ts_ms or now_ms()
    event_id = event_id or f"{device['sn']}-{alert_type}-{ts}"

    dupe = db.fetch_one("SELECT * FROM alerts WHERE event_id = :e", {"e": event_id})
    if dupe:
        # §4.4：同一 event_id 重复上报（平台重试/网关补传）只入库一次
        log.info("告警幂等命中 event_id=%s（alert=%s）", event_id, dupe["id"])
        return dupe, False

    since = ts - suppress_window(alert_type, cfg) * 1000
    recent = db.fetch_one(
        "SELECT * FROM alerts WHERE device_id = :d AND type = :t "
        "AND (node_sn <=> :n) AND ts_ms >= :since ORDER BY ts_ms DESC, id DESC LIMIT 1",
        {"d": device["id"], "t": alert_type, "n": node_sn, "since": since},
    )

    if recent:
        if level > int(recent["level"]):
            # §8.8：窗口内级别上升 → 新建高等级告警，旧条视为已恢复
            db.execute(
                "UPDATE alerts SET recovered_at = :now, next_push_at = NULL WHERE id = :id",
                {"now": ms_to_dt(ts), "id": recent["id"]},
            )
            log.info("告警升级：alert=%s level=%s→%s", recent["id"], recent["level"], level)
        else:
            # §8.8：窗口内重复 → 不新建，只刷新快照与 recovered_at
            db.execute(
                "UPDATE alerts SET snapshot_json = :snap, recovered_at = NULL WHERE id = :id",
                {"snap": db.jdump(snapshot) if snapshot is not None else None, "id": recent["id"]},
            )
            updated = db.fetch_one("SELECT * FROM alerts WHERE id = :id", {"id": recent["id"]})
            return updated, False

    alert_id = db.insert(
        "INSERT INTO alerts "
        "(device_id, node_sn, type, level, ts, ts_ms, event_id, status, snapshot_json, push_count, next_push_at) "
        "VALUES (:d, :n, :t, :l, :ts, :ts_ms, :eid, 'pending', :snap, 0, :next)",
        {
            "d": device["id"],
            "n": node_sn,
            "t": alert_type,
            "l": level,
            "ts": ms_to_dt(ts),
            "ts_ms": ts,
            "eid": event_id,
            "snap": db.jdump(snapshot) if snapshot is not None else None,
            "next": ms_to_dt(ts + REPUSH_SCHEDULE[0] * 1000),
        },
    )
    row = db.fetch_one("SELECT * FROM alerts WHERE id = :id", {"id": alert_id})
    # §7.7 的推送节奏第一档是 0 s：新建即投递一次，后端不把安全告警压在下一个 tick 上。
    # 之后 60 s/300 s 的重推由调度器按 next_push_at 接手（run_repush_once）。
    row = push_alert(row, cfg)
    return row, True


def push_schedule(cfg: dict[str, Any] | None = None) -> list[int]:
    """设备级推送节奏（§6.9 `alert_repush_s`，默认 [0,60,300]），非法值回落到契约默认。"""
    raw = (cfg or {}).get("alert_repush_s") or list(REPUSH_SCHEDULE)
    try:
        schedule = [int(x) for x in raw]
    except (TypeError, ValueError):
        return list(REPUSH_SCHEDULE)
    return schedule or list(REPUSH_SCHEDULE)


def push_alert(row: dict[str, Any], cfg: dict[str, Any] | None = None) -> dict[str, Any]:
    """投递一次推送并把 push_count/next_push_at 推进到下一档（§7.7）。"""
    from ..config import settings  # 局部导入：避免测试改环境变量后拿到旧值

    recipients = db.fetch_all(
        "SELECT u.id, u.username FROM users u JOIN user_devices ud ON ud.user_id = u.id "
        "WHERE ud.device_id = :d AND ud.deleted_at IS NULL",
        {"d": row["device_id"]},
    )
    tokens = db.fetch_scalar(
        "SELECT COUNT(*) FROM push_tokens WHERE user_id IN "
        "(SELECT user_id FROM user_devices WHERE device_id = :d AND deleted_at IS NULL)",
        {"d": row["device_id"]},
        0,
    )
    mode = settings.push_mode
    if mode == "real":
        # P1：Push Kit 需 AGC 凭证，仓库不存密钥；此处只记日志，避免假装送达
        log.warning("PUSH_MODE=real 但未接入 Push Kit（P1），alert=%s 仅记录投递意图", row["id"])
    log.info(
        "[push:%s] alert=%s type=%s level=%s → %d 位成员（%d 个 token）",
        mode,
        row["id"],
        row["type"],
        row["level"],
        len(recipients),
        int(tokens or 0),
    )
    schedule = push_schedule(cfg)
    count = int(row["push_count"]) + 1
    pushed_at = now_ms()
    # `WHERE push_count = :expected` 是乐观锁：调度器 tick 与 /demo 的即时推送可能同时命中同一条，
    # 没有它就会出现「一次告警被记两次推送」。抢不到就认为别人已经推过。
    next_at = None if count >= len(schedule) else int(row["ts_ms"]) + schedule[count] * 1000
    claimed = db.execute(
        "UPDATE alerts SET push_count = :c, last_push_at = :at, next_push_at = :next "
        "WHERE id = :id AND push_count = :expected",
        {
            "c": count,
            "at": ms_to_dt(pushed_at),
            "next": ms_to_dt(next_at) if next_at else None,
            "id": row["id"],
            "expected": int(row["push_count"]),
        },
    )
    if not claimed:
        log.info("告警 %s 已由其它路径推送（push_count=%s），本次跳过", row["id"], row["push_count"])
    refreshed = db.fetch_one("SELECT * FROM alerts WHERE id = :id", {"id": row["id"]})
    return refreshed or row


def repush_due(limit: int = 100) -> list[dict[str, Any]]:
    """§7.7：扫 `status='pending' AND recovered_at IS NULL AND next_push_at<=now`。"""
    return db.fetch_all(
        "SELECT * FROM alerts WHERE status = 'pending' AND recovered_at IS NULL "
        "AND next_push_at IS NOT NULL AND next_push_at <= :now ORDER BY next_push_at ASC LIMIT :limit",
        {"now": ms_to_dt(now_ms()), "limit": limit},
    )


def run_repush_once(factory=None) -> int:
    """调度器 tick 调用；返回本轮重推条数。"""
    rows = repush_due()
    if not rows:
        return 0
    pushed = 0
    for row in rows:
        device = db.fetch_one("SELECT id, sn, settings_json FROM devices WHERE id = :d", {"d": row["device_id"]})
        push_alert(row, device_settings(device or {}))
        pushed += 1
    return pushed


def mark_recovered(device_id: int, alert_type: str, node_sn: str | None, ts_ms: int | None = None) -> int:
    """恢复事件：写 `recovered_at` 并停推，不改 `status`（§8.4/§8.8）。"""
    sets = ["recovered_at = :at", "next_push_at = NULL"]
    params: dict[str, Any] = {
        "at": ms_to_dt(ts_ms or now_ms()),
        "d": device_id,
        "t": alert_type,
    }
    where = "device_id = :d AND type = :t AND recovered_at IS NULL"
    if node_sn:
        where += " AND (node_sn <=> :n)"
        params["n"] = node_sn
    return db.execute(f"UPDATE alerts SET {', '.join(sets)} WHERE {where}", params)


def create_disposition(user: dict[str, Any], alert_id: int, action: str, note: str | None) -> dict[str, Any]:
    """§6.15 处置：写记录 + `status='resolved'` + 停推（闭环的唯一出口）。"""
    if action not in ACTIONS:
        raise ApiError(1302, allowed=list(ACTIONS))
    row = get_alert_row(alert_id)
    if row["status"] == "resolved":
        raise ApiError(1301)
    ts = now_ms()
    disposition_id = db.insert(
        "INSERT INTO alert_dispositions (alert_id, user_id, action, note, created_at) "
        "VALUES (:a, :u, :act, :note, :at)",
        {
            "a": alert_id,
            "u": user["id"],
            "act": action,
            "note": note,
            "at": ms_to_dt(ts),
        },
    )
    db.execute(
        "UPDATE alerts SET status = 'resolved', next_push_at = NULL, recovered_at = COALESCE(recovered_at, :at) "
        "WHERE id = :id",
        {"at": ms_to_dt(ts), "id": alert_id},
    )
    return {
        "alert_id": alert_id,
        "status": "resolved",
        "action": action,
        "disposition_id": disposition_id,
        "ts": ts,
    }
