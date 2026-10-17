"""mock 种子数据（`python -m app.seed`）。

为什么需要它：App 与验收脚本在**没有真实硬件**时也要能看到「有活动、有告警、有趋势」的完整报表，
否则 `GET /api/reports/*` 全是 0，看不出接口是否真的按契约算对了。

用法（容器内）：
    docker compose exec api python -m app.seed               # 幂等补齐：用户/设备/节点/3 天读数/3 条告警
    docker compose exec api python -m app.seed --reset        # 先清空业务表再灌（不可用于生产库）
    docker compose exec api python -m app.seed --bind-code    # 打印一个新的 6 位绑定码（10 分钟有效）

安全：演示口令只在 dev 生效；`APP_ENV=prod` 时必须显式传 `--password`，避免把演示账号带上线。
"""
from __future__ import annotations

import argparse
import logging
import random
import sys
from datetime import timedelta

from . import db
from . import config
from .security import hash_password
from .services.scheduler import recompute_activity
from .timeutil import cst_day_bounds_ms, ms_to_cst_date, ms_to_dt, now_ms

log = logging.getLogger("app.seed")

DEMO_USERNAME = "demo"
DEMO_PASSWORD = "demo-pass-123"      # 仅 dev：README 明确要求上线前改掉
GATEWAY_SN = "M0-3F1A2B"             # 契约 §0.7 示例 SN
UNBOUND_SN = "M0-9C2D4E"             # 用于演示/测试「绑定」流程的第二台网关

NODES = [
    # (sn, type, location)：P0 = M1/M2/M4 + 汇聚 H1（契约 §0.7）
    ("H1", "hub", "客厅"),
    ("M1", "gas", "厨房"),
    ("M2", "fall", "卧室"),
    ("M4", "presence", "门厅"),
]

BUSINESS_TABLES = [
    "alert_dispositions",
    "alerts",
    "sensor_readings",
    "activity_hourly",
    "activity_daily",
    "device_online",
    "nodes",
    "user_devices",
    "push_tokens",
    "devices",
    "users",
]


def reset_tables() -> None:
    if config.settings.is_prod:
        # 纵深防御：CLI 已经拦了一层，但 `run(reset=True)` 可能被脚本/运维直接调用
        raise RuntimeError("APP_ENV=prod 下禁止清空业务表（--reset 仅限本地与演示环境）")
    for table in BUSINESS_TABLES:
        db.execute(f"TRUNCATE TABLE `{table}`")
    log.warning("已清空业务表：%s", ", ".join(BUSINESS_TABLES))


# --------------------------------------------------------------------------- 基础实体

def ensure_user(username: str, password: str, phone: str | None = None) -> int:
    row = db.fetch_one("SELECT id FROM users WHERE username = :u", {"u": username})
    if row:
        return int(row["id"])
    user_id = db.insert(
        "INSERT INTO users (username, password_hash, phone) VALUES (:u, :p, :ph)",
        {"u": username, "p": hash_password(password), "ph": phone},
    )
    log.info("已创建演示账号 %s（id=%s）", username, user_id)
    return user_id


def ensure_device(sn: str, name: str, owner_id: int, bind_code: str | None = None, iotda_device_id: str | None = None) -> int:
    row = db.fetch_one("SELECT id FROM devices WHERE sn = :sn", {"sn": sn})
    if row:
        return int(row["id"])
    expire = ms_to_dt(now_ms() + config.settings.bind_code_ttl_min * 60 * 1000) if bind_code else None
    device_id = db.insert(
        "INSERT INTO devices (sn, name, bind_code, bind_code_expire_at, iotda_device_id, owner_id, fw_ver) "
        "VALUES (:sn, :name, :code, :expire, :plat, :owner, :fw)",
        {
            "sn": sn,
            # IoTDA 设备 ID：契约 §0.1 明确「服务器主键 ≠ 平台字符串」，没有平台账号时用 SN 兜底以便 mock 链路自洽
            "name": name,
            "code": bind_code,
            "expire": expire,
            "plat": iotda_device_id,
            "owner": owner_id,
            "fw": "1.0.0",
        },
    )
    log.info("已创建设备 %s（id=%s）", sn, device_id)
    return device_id


def ensure_membership(user_id: int, device_id: int, role: str) -> None:
    db.execute(
        "INSERT INTO user_devices (user_id, device_id, role) VALUES (:u, :d, :r) "
        "ON DUPLICATE KEY UPDATE role = VALUES(role), deleted_at = NULL",
        {"u": user_id, "d": device_id, "r": role},
    )


def ensure_nodes(device_id: int) -> None:
    for sn, node_type, location in NODES:
        db.execute(
            "INSERT INTO nodes (device_id, sn, type, location, online, last_seen, last_seen_ms, rssi, battery) "
            "VALUES (:d, :sn, :t, :loc, 1, :seen, :seen_ms, -62, 255) "
            "ON DUPLICATE KEY UPDATE type = VALUES(type), location = VALUES(location), online = 1",
            {"d": device_id, "sn": sn, "t": node_type, "loc": location, "seen": ms_to_dt(now_ms()), "seen_ms": now_ms()},
        )


# --------------------------------------------------------------------------- 三天读数

def seed_readings(device_id: int, days: int = 3, seed: int = 20261017) -> dict[str, int]:
    """灌入 3 天数据：gateway 属性快照 + presence 活动 + M1 燃气 + 24 h 前的跌倒。

    数量刻意压小（每 15 分钟一条网关属性 + 活动时段每小时 1-3 条 presence），
    让种子在秒级完成，同时足够让 ×24/×7 的报表口径有真实数字。
    """
    rng = random.Random(seed)  # 固定种子：重跑种子得到同一份报表，便于回归对比
    today = ms_to_cst_date(now_ms())
    counts = {"gateway": 0, "presence": 0, "gas": 0, "fall": 0}

    for offset in range(days - 1, -1, -1):
        day = today - timedelta(days=offset)
        start_ms, _ = cst_day_bounds_ms(day)
        day_end = min(start_ms + 24 * 3600 * 1000, now_ms())

        # 网关属性快照：契约 §4.3 的 12 属性（每 15 分钟一条，用于 §6.18 环境统计）
        ts = start_ms
        while ts <= day_end:
            hour = (ts // 3600000) % 24
            temp = round(22.0 + 4.0 * ((hour - 6) % 24) / 24 + rng.uniform(-0.6, 0.6), 1)
            humi = round(48.0 + rng.uniform(-8, 8), 1)
            _insert(device_id, "M0", "gateway", ts, {
                "temp": temp,
                "humi": humi,
                "smoke": 0,
                "nodes_online": {"H1": True, "M1": True, "M2": True, "M4": True},
                "alarm_state": 0,
            })
            counts["gateway"] += 1
            ts += 15 * 60 * 1000

        # 活动时段 06:00-22:00，每小时 1-3 次 presence=true（§7.9 归并成 activity_hourly）
        for hour in range(6, 23):
            for _ in range(rng.randint(1, 3)):
                ts = start_ms + hour * 3600 * 1000 + rng.randint(0, 3500) * 1000
                if ts > day_end:
                    continue
                _insert(device_id, "M4", "presence", ts, {
                    "presence": True,
                    "last_motion_ts": ts,
                    "count": rng.randint(1, 3),
                })
                counts["presence"] += 1

        # 厨房燃气背景值：每 2 小时一条（远低于 §8.2 的 1500 mV 一级阈值）
        for hour in range(0, 24, 2):
            ts = start_ms + hour * 3600 * 1000 + rng.randint(0, 60) * 1000
            if ts > day_end:
                continue
            _insert(device_id, "M1", "gas", ts, {
                "smoke": rng.randint(120, 260),
                "lpg": rng.randint(120, 260),
                "temp": round(24.0 + rng.uniform(-1, 1), 1),
                "humi": round(52.0 + rng.uniform(-5, 5), 1),
                "adc_mv": rng.randint(300, 700),
                "warmup": False,
            })
            counts["gas"] += 1

        # 昨天下午来一次真实跌倒（已恢复），让报表与告警列表都有实物
        if offset == 1:
            ts = start_ms + 15 * 3600 * 1000
            _insert(device_id, "M2", "fall", ts, {"fall": 1, "presence": 1, "src": "ld6002c", "dist": 2.4})
            _insert(device_id, "M2", "fall", ts + 40 * 1000, {"fall": 0, "presence": 0, "src": "ld6002c"})
            counts["fall"] += 2

    return counts


def _insert(device_id: int, node_sn: str, rtype: str, ts_ms: int, payload: dict) -> None:
    db.execute(
        "INSERT IGNORE INTO sensor_readings (device_id, node_sn, type, ts, ts_ms, payload_json) "
        "VALUES (:d, :sn, :t, :ts, :ts_ms, :p)",
        {"d": device_id, "sn": node_sn, "t": rtype, "ts": ms_to_dt(ts_ms), "ts_ms": ts_ms, "p": db.jdump(payload)},
    )


# --------------------------------------------------------------------------- 告警样例

def seed_alerts(device_id: int, user_id: int) -> dict[str, int]:
    """3 条覆盖三种状态/类型的告警：跌倒已处置、燃气待处置、无活动误报已处置。"""
    now = now_ms()
    specs = [
        # (type, level, node_sn, ts_offset_ms, event_id, snapshot, status, disposition)
        ("fall", 2, "M2", now - 26 * 3600 * 1000, "fall", {"fall": 1, "presence": 1}, "resolved", ("checked", "已确认老人无恙，沙发起身时误判")),
        ("gas", 1, "M1", now - 95 * 60 * 1000, "gas", {"adc_mv": 1680, "smoke": 1680}, "pending", None),
        ("no_motion", 1, "M4", now - 3 * 24 * 3600 * 1000, "no_motion", {"last_motion_ts": now - 3 * 24 * 3600 * 1000 - 1900 * 1000}, "resolved", ("false_alarm", "老人午睡")),
    ]
    created = 0
    for atype, level, node_sn, ts, tag, snapshot, status, disposition in specs:
        event_id = f"{GATEWAY_SN}-{tag}-{ts}"
        exists = db.fetch_one("SELECT id FROM alerts WHERE event_id = :e", {"e": event_id})
        if exists:
            continue
        alert_id = db.insert(
            "INSERT INTO alerts (device_id, node_sn, type, level, ts, ts_ms, event_id, status, recovered_at, "
            "snapshot_json, push_count, last_push_at, next_push_at) "
            "VALUES (:d, :n, :t, :l, :ts, :ts_ms, :eid, :st, :rec, :snap, :pc, :lpa, :npa)",
            {
                "d": device_id,
                "n": node_sn,
                "t": atype,
                "l": level,
                "ts": ms_to_dt(ts),
                "ts_ms": ts,
                "eid": event_id,
                "st": status,
                "rec": ms_to_dt(ts + 20 * 60 * 1000) if status == "resolved" else None,
                "snap": db.jdump(snapshot),
                "pc": 1 if status == "resolved" else 1,
                "lpa": ms_to_dt(ts),
                "npa": ms_to_dt(now) if status == "pending" else None,
            },
        )
        created += 1
        if disposition:
            action, note = disposition
            db.insert(
                "INSERT INTO alert_dispositions (alert_id, user_id, action, note, created_at) "
                "VALUES (:a, :u, :act, :note, :at)",
                {"a": alert_id, "u": user_id, "act": action, "note": note, "at": ms_to_dt(ts + 12 * 60 * 1000)},
            )
    return {"alerts": created}


def seed_online(device_id: int) -> None:
    db.execute(
        "INSERT INTO device_online (device_id, online, last_heartbeat, last_heartbeat_ms, last_properties_json, alarm_state) "
        "VALUES (:d, 1, :hb, :hb_ms, :props, 1) "
        "ON DUPLICATE KEY UPDATE online = 1, last_heartbeat = VALUES(last_heartbeat), "
        "last_heartbeat_ms = VALUES(last_heartbeat_ms), last_properties_json = VALUES(last_properties_json)",
        {
            "d": device_id,
            "hb": ms_to_dt(now_ms()),
            "hb_ms": now_ms(),
            # alarm_state=1：种子里有一条待处置的燃气预警，列表/详情才有东西可看
            "props": db.jdump({"temp": 23.4, "humi": 51.2, "smoke": 0, "alarm_state": 1,
                               "nodes_online": {"H1": True, "M1": True, "M2": True, "M4": True}}),
        },
    )


def run(reset: bool = False, days: int = 3, username: str = DEMO_USERNAME, password: str = DEMO_PASSWORD,
        make_bind_code: bool = False) -> dict:
    if reset:
        reset_tables()
    user_id = ensure_user(username, password)
    device_id = ensure_device(GATEWAY_SN, "客厅网关", user_id)
    ensure_membership(user_id, device_id, "owner")
    unbound_id = ensure_device(UNBOUND_SN, "备用网关", 0)
    ensure_nodes(device_id)
    ensure_nodes(unbound_id)
    readings = seed_readings(device_id, days=days)
    seed_online(device_id)
    alerts_created = seed_alerts(device_id, user_id)

    # 归并活动报表（正常由调度器做；种子后立刻跑一次，否则报表要等 5 分钟才有数）
    activity: dict[str, int] = {}
    today = ms_to_cst_date(now_ms())
    for offset in range(days):
        activity.update(recompute_activity(today - timedelta(days=offset)))

    if make_bind_code:
        import secrets as _secrets

        code = f"{_secrets.randbelow(1000000):06d}"
        db.execute(
            "UPDATE devices SET bind_code = :c, bind_code_expire_at = :e WHERE id = :d",
            {"c": code, "e": ms_to_dt(now_ms() + config.settings.bind_code_ttl_min * 60 * 1000), "d": unbound_id},
        )
        print(f"[bind-code] sn={UNBOUND_SN} code={code} 有效期={config.settings.bind_code_ttl_min} 分钟")

    return {
        "user_id": user_id,
        "device_id": device_id,
        "unbound_device_id": unbound_id,
        "readings": readings,
        "activity": activity,
        **alerts_created,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="灌入 mock 种子数据（默认幂等）")
    parser.add_argument("--reset", action="store_true", help="先 TRUNCATE 业务表（只允许 non-prod）")
    parser.add_argument("--days", type=int, default=3, help="读数天数（默认 3）")
    parser.add_argument("--username", default=DEMO_USERNAME)
    parser.add_argument("--password", default=DEMO_PASSWORD, help="演示口令；APP_ENV=prod 时必须显式覆盖")
    parser.add_argument("--bind-code", action="store_true", dest="bind_code", help="额外打印一个 6 位绑定码")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s | %(message)s")
    if config.settings.is_prod:
        if args.reset:
            print("拒绝执行：APP_ENV=prod 下不允许 --reset", file=sys.stderr)
            return 2
        if args.password == DEMO_PASSWORD:
            print("拒绝执行：APP_ENV=prod 下必须用 --password 指定真实口令", file=sys.stderr)
            return 2

    db.ensure_database()
    db.apply_schema()
    summary = run(reset=args.reset, days=args.days, username=args.username, password=args.password,
                  make_bind_code=args.bind_code)
    print("seed 完成：" + ", ".join(f"{key}={value}" for key, value in summary.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
