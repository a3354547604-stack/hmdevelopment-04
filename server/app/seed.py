"""mock 种子数据（`python -m app.seed`）。

为什么需要它：App 与验收脚本在**没有真实硬件**时也要能看到「有活动、有告警、有趋势」的完整报表，
否则 `GET /api/reports/*` 全是 0，看不出接口是否真的按契约算对了。

用法（容器内）：
    docker compose exec api python -m app.seed               # 幂等补齐：用户/设备/节点/3 天读数/3 条告警
    docker compose exec api python -m app.seed --reset        # 先清空业务表再灌（不可用于生产库）
    docker compose exec api python -m app.seed --bind-code    # 打印一个新的 6 位绑定码（10 分钟有效）
    docker compose exec api python -m app.seed --demo         # 现场演示数据包（t49，见下方「演示数据包」段）

安全：演示口令只在 dev 生效；`APP_ENV=prod` 时必须显式传 `--password`，避免把演示账号带上线。
"""
from __future__ import annotations

import argparse
import logging
import random
import sys
from datetime import date, timedelta

from . import db
from . import config
from .security import hash_password
from .services.scheduler import recompute_activity
from .timeutil import cst_day_bounds_ms, iso, ms_to_cst_date, ms_to_dt, now_ms

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


def _insert(device_id: int, node_sn: str, rtype: str, ts_ms: int, payload: dict,
            created_ms: int | None = None) -> None:
    """写一条读数。created_ms 只给「断网补传」样本用：ts 是采集时刻，created_at 才是入库时刻。

    为什么要能显式指定入库时刻：补传证据 = 「采集时刻在断网窗口内、入库时刻在恢复之后」。
    若让 created_at 取库里的 CURRENT_TIMESTAMP，演示库在凌晨重置时窗口还没到，
    这条证据就永远不成立（t49 D6 判据，也满足「重置结果与运行时刻无关」）。
    """
    columns = "device_id, node_sn, type, ts, ts_ms, payload_json"
    values = "VALUES (:d, :sn, :t, :ts, :ts_ms, :p)"
    params = {"d": device_id, "sn": node_sn, "t": rtype, "ts": ms_to_dt(ts_ms), "ts_ms": ts_ms,
              "p": db.jdump(payload)}
    if created_ms is not None:
        columns += ", created_at"
        values = "VALUES (:d, :sn, :t, :ts, :ts_ms, :p, :created)"
        params["created"] = ms_to_dt(created_ms)
    db.execute(f"INSERT IGNORE INTO sensor_readings ({columns}) {values}", params)


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


# --------------------------------------------------------------------------- 演示数据包（--demo，t49）
#
# 为什么另开一份而不是加参数到 run()：run() 的语义是「幂等补齐、不删数据」，pytest 与 README 都依赖
# 它的形状；演示要的是「先清空、再灌成确定状态」。混在一起会让默认种子越来越重、越来越难回归。
#
# 冻结时钟（关键设计取舍）：所有时刻锚定到 **CST 日历日的固定钟点**（今天/昨天/前天…），
# 等价于「把现在冻结在今天 20:00」。这样 00:05 与 23:55 各跑一次，报表数字、告警列表、断网窗口
# 完全相同——验收与演示才能复算。代价是：若在当天该钟点之前运行，列表里会出现「今天稍晚」的时刻，
# 这是刻意的（演示数据可随时复现），不是时间错误。`device_online.last_heartbeat` 例外：用真实 now，
# 否则调度器会立刻把网关判离线。

DEMO_ACTIVITY_DAYS = 60          # 30 天（§6.18 month）+ 上一周期 30 天，月报「较上月」才有基线可比
DEMO_GATEWAY_DAYS = 3            # 环境统计只需近几天，60 天 × 96 条快照没必要
DEMO_OFFLINE_HOUR = 12           # 断网窗口 = 今天 CST 12:00–13:00（§4.7 演示动作 6）
DEMO_OFFLINE_MINUTES = 60
DEMO_BACKFILL_DELAY_MIN = 3      # 补传数据的入库时刻 = 恢复后 3 分钟（数据内自证：created_at 晚于 ts）
DEMO_BACKFILL_STEP_MIN = 5       # 补传样本粒度：窗口内每 5 分钟一条
DEMO_BACKFILL_COUNT = 11         # (60-5)/5 = 11 条；D6 与用例按这个设计值断言
# 补传样本的显式标记（写进读数 payload）。为什么需要它：旧判据用「created_at 晚于窗口结束」认补传，
# 而窗口内只要有一条实时写入的行，13:00 CST 之后这条判据就会把实时行也算成补传（t61 F-03）——
# 判据一旦依赖运行钟点，同一份数据在上午与晚上会给出不同答案。改成按标记认，与钟点无关。
DEMO_BACKFILL_MARK = "backfill"
DEMO_TODAY_ACTIVITY = 18         # 今日活动次数（写死 → §6.17 趋势句可复算：-25% → 「较昨日下降 25%」）
DEMO_YESTERDAY_ACTIVITY = 24


def demo_activity_count(offset: int) -> int:
    """某日（0=今天）的活动次数。写死公式而不是随机数：演示数字必须能被人复算、逐次一致。"""
    if offset == 0:
        return DEMO_TODAY_ACTIVITY
    if offset == 1:
        return DEMO_YESTERDAY_ACTIVITY
    return 16 + (offset * 7) % 11


def _cst_at(day: date, hour: int, minute: int = 0, second: int = 0) -> int:
    start_ms, _ = cst_day_bounds_ms(day)
    return start_ms + (hour * 3600 + minute * 60 + second) * 1000


def seed_demo_activity(device_id: int, days: int = DEMO_ACTIVITY_DAYS) -> dict[str, int]:
    """31 天的活动读数（M4 presence）：小时 08–19、分钟 (i×3)%60，故同一天内分钟不重复。

    `active_minutes` 与 `count` 因此都等于设计值，日报/周报/月报三份口径能互相印证。

    落点刻意避开断网窗口（12:00–13:00）：断网期间网关本地缓存，恢复后补传，
    所以窗口里不该出现「实时写入」的活动点——否则 D6 的「窗口内读数 = 补传样本」不成立（t61 F-06）。
    """
    today = ms_to_cst_date(now_ms())
    per_day: dict[str, int] = {}
    for offset in range(days - 1, -1, -1):
        day = today - timedelta(days=offset)
        count = demo_activity_count(offset)
        for i in range(count):
            hour = 8 + (i % 12)
            if hour == DEMO_OFFLINE_HOUR:
                hour += 1                     # 挪出断网窗口（分钟不重复，不会与其它点撞唯一键）
            ts = _cst_at(day, hour, (i * 3) % 60)
            _insert(device_id, "M4", "presence", ts, {"presence": True, "last_motion_ts": ts, "count": 1})
        per_day[iso(day)] = count
    return per_day


def seed_demo_gateway(device_id: int, days: int = DEMO_GATEWAY_DAYS) -> int:
    """网关属性快照每 15 分钟一条；今天 12:00–13:00 故意断档（D6 的时间戳跳变 75 分钟）。"""
    today = ms_to_cst_date(now_ms())
    count = 0
    for offset in range(days - 1, -1, -1):
        day = today - timedelta(days=offset)
        for quarter in range(96):
            hour, minute = divmod(quarter * 15, 60)
            if offset == 0 and DEMO_OFFLINE_HOUR <= hour < DEMO_OFFLINE_HOUR + 1:
                continue
            ts = _cst_at(day, hour, minute)
            _insert(device_id, "M0", "gateway", ts, {
                "door": False,
                "window": False,
                "temp": round(22.0 + 3.0 * ((hour + 6) % 24) / 24, 1),
                "humi": round(48.0 + (hour % 9), 1),
                "smoke": 0,
                # §4.3：nodes_online/bound_nodes 是 JSON 文本而不是真数组（App 按字符串解析）
                "nodes_online": db.jdump({"H1": True, "M1": True, "M2": True, "M4": True}),
                "battery": 255,
                "alarm_state": 2,       # 今天 10:00 的燃气二级报警尚未恢复
            })
            count += 1
    return count


def seed_demo_nodes_readings(device_id: int) -> dict[str, int]:
    """M1 燃气二级过程、M2 跌倒（今天 + 昨天）、M4 门磁，以及断网窗口内补传的节点样本。"""
    today = ms_to_cst_date(now_ms())
    yesterday = today - timedelta(days=1)
    counts = {"gas": 0, "fall": 0, "door": 0, "backfill": 0}

    # 燃气：背景值（每 2 h 一条，分钟错开 07 避开过程点）远低于 §8.2 一级阈值 1500 mV
    # 断网窗口内不写背景值：窗口里只留补传样本（见 seed_demo_nodes_readings 末尾）
    for hour in range(0, 24, 2):
        if hour == DEMO_OFFLINE_HOUR:
            continue
        _insert(device_id, "M1", "gas", _cst_at(today, hour, 7), {
            "smoke": 180, "lpg": 190, "temp": 24.2, "humi": 52.0, "adc_mv": 420, "level": 0, "warmup": False,
        })
        counts["gas"] += 1
    # 过程点：09:58 越过一级（1500）→ 10:00 越过二级（2500，§8.2 gas_t2_mv）→ 10:20 恢复
    for hour, minute, adc in ((9, 58, 1620), (10, 0, 2680), (10, 2, 2910), (10, 20, 320)):
        level = 2 if adc >= 2500 else (1 if adc >= 1500 else 0)
        _insert(device_id, "M1", "gas", _cst_at(today, hour, minute), {
            "smoke": adc, "lpg": 1900 if level == 2 else 210, "temp": 25.1, "humi": 55.0,
            "adc_mv": adc, "level": level, "warmup": False,
        })
        counts["gas"] += 1

    # 跌倒：昨天 15:00（已处置）与今天 15:00（待处置），fall=1 后 40 s 自行恢复
    for day in (yesterday, today):
        ts = _cst_at(day, 15, 0)
        _insert(device_id, "M2", "fall", ts,
                {"fall": 1, "presence": 1, "src": "ld6002c", "dist": 2.4, "radar_state": 2})
        _insert(device_id, "M2", "fall", ts + 40 * 1000,
                {"fall": 0, "presence": 0, "src": "ld6002c", "dist": 0, "radar_state": 0})
        counts["fall"] += 2

    # 门磁（P0 活动/离家判定的唯一数据源，契约 §14 ⑭）：今天 关→开→关 展示「离家再回家」
    for hour, minute, opened in ((8, 0, False), (8, 30, True), (9, 40, False)):
        _insert(device_id, "M4", "door", _cst_at(today, hour, minute), {"door": opened, "window": False})
        counts["door"] += 1
    # 昨天 09:27–09:30 门开了 3 分钟（对应 door_left 告警，§8.6 阈值 180 s）
    for hour, minute, opened in ((9, 27, True), (9, 30, False)):
        _insert(device_id, "M4", "door", _cst_at(yesterday, hour, minute), {"door": opened, "window": False})
        counts["door"] += 1

    # 断网窗口内补传的节点样本：ts 落在 12:00–13:00（断网期间），created_at 显式写成恢复后 3 分钟，
    # 并在 payload 里打 origin=backfill 标记 —— D6 与用例按标记认补传，不看墙钟（t61 F-03）。
    off_start = _cst_at(today, DEMO_OFFLINE_HOUR, 0)
    off_end = off_start + DEMO_OFFLINE_MINUTES * 60 * 1000
    for index, minute in enumerate(range(DEMO_BACKFILL_STEP_MIN, DEMO_OFFLINE_MINUTES, DEMO_BACKFILL_STEP_MIN)):
        _insert(device_id, ("M1", "M2", "M4")[index % 3], "status", off_start + minute * 60 * 1000, {
            "uptime_s": 4 * 3600 + minute * 60,
            "rssi": -58 - (index % 5),
            "power_src": "usb",
            "battery": 255,
            "origin": DEMO_BACKFILL_MARK,       # 演示标记：仅 seed 写，设备侧不会上报这个字段
        }, created_ms=off_end + DEMO_BACKFILL_DELAY_MIN * 60 * 1000)
        counts["backfill"] += 1
    assert counts["backfill"] == DEMO_BACKFILL_COUNT, "补传样本条数与设计值不一致（改窗口/粒度时同步改常量）"
    return counts


def seed_demo_online(device_id: int) -> None:
    """在线态 + §4.3 属性快照。P0 未装的 presence/last_motion_ts/lpg/co 按 §4.6 直接省略。"""
    now = now_ms()
    props = {
        "door": False,
        "window": False,
        "temp": 23.4,
        "humi": 51.2,
        "smoke": 0,
        "nodes_online": db.jdump({"H1": True, "M1": True, "M2": True, "M4": True}),
        "bound_nodes": db.jdump(["M1", "M2", "M4"]),
        "pair_state": "sta",
        "battery": 255,
        "alarm_state": 2,               # 今天 10:00 的燃气二级报警尚未恢复（§8.1）
    }
    db.execute(
        "INSERT INTO device_online (device_id, online, last_heartbeat, last_heartbeat_ms, last_properties_json, alarm_state) "
        "VALUES (:d, 1, :hb, :hb_ms, :props, 2) "
        "ON DUPLICATE KEY UPDATE online = 1, last_heartbeat = VALUES(last_heartbeat), "
        "last_heartbeat_ms = VALUES(last_heartbeat_ms), last_properties_json = VALUES(last_properties_json), "
        "alarm_state = VALUES(alarm_state)",
        {"d": device_id, "hb": ms_to_dt(now), "hb_ms": now, "props": db.jdump(props)},
    )


def seed_demo_alerts(device_id: int, user_id: int) -> dict[str, int]:
    """10 条告警覆盖演示所需的 7 类与三种状态，含 5 条已处置（§6.18 误报统计有数）。

    第 8 类 `env`（§6.13）属 P1、P0 网关不产生，这里**刻意不造**：造了就等于替网关断言
    「它会上报室内温湿度异常」，而 `rk2206-gateway/` 里没有任何 `env` 产生路径（t89 F-01）。"""
    today = ms_to_cst_date(now_ms())
    yesterday = today - timedelta(days=1)
    now = now_ms()
    # (type, level, node_sn, ts_ms, status, recovered_ms, snapshot, disposition(action, note))
    specs = [
        ("gas", 2, "M1", _cst_at(today, 10, 0), "pending", None,
         {"adc_mv": 2680, "smoke": 2680, "lpg": 1900, "level": 2}, None),
        ("gas", 1, "M1", _cst_at(yesterday, 18, 0), "resolved", _cst_at(yesterday, 18, 20),
         {"adc_mv": 1680, "smoke": 1680, "level": 1}, ("checked", "已开窗通风，浓度回到背景值")),
        ("fall", 2, "M2", _cst_at(today, 15, 0), "pending", None,
         {"fall": 1, "presence": 1, "src": "ld6002c", "dist": 2.4}, None),
        ("fall", 2, "M2", _cst_at(yesterday, 15, 0), "resolved", _cst_at(yesterday, 15, 1),
         {"fall": 1, "presence": 1, "src": "ld6002c", "dist": 2.4}, ("checked", "已上门确认老人无恙")),
        ("no_motion", 1, "M4", _cst_at(today, 11, 0), "pending", None,
         {"last_motion_ts": _cst_at(today, 8, 30)}, None),
        ("door_left", 1, "M4", _cst_at(yesterday, 9, 30), "resolved", _cst_at(yesterday, 9, 31),
         {"door": True, "open_s": 180}, ("false_alarm", "老人整理门厅，属误报")),
        ("sos", 2, None, _cst_at(today, 14, 0), "pending", None,
         {"sos": 1, "source": "button", "ts": _cst_at(today, 14, 0)}, None),
        ("sos", 2, None, _cst_at(today, 14, 5), "resolved", _cst_at(today, 14, 20),
         {"sos": 1, "source": "app", "ts": _cst_at(today, 14, 5)}, ("contacted", "已电话联系老人，确认安全")),
        # 网关离线→恢复：12:00–13:00 断网（与上面快照断档同一窗口）
        ("device_offline", 1, None, _cst_at(today, 11, 48), "resolved", _cst_at(today, 13, 0),
         {"last_heartbeat_ms": _cst_at(today, 11, 45), "timeout_s": 180}, ("checked", "网关恢复上报，已自动清除")),
        ("node_offline", 1, "M4", _cst_at(yesterday, 16, 0), "resolved", _cst_at(yesterday, 16, 10),
         {"misses": 3}, ("checked", "节点重新应答，已恢复")),
    ]
    created = 0
    for atype, level, node_sn, ts, status, recovered, snapshot, disposition in specs:
        event_id = f"{GATEWAY_SN}-{atype}-{ts}"
        if db.fetch_one("SELECT id FROM alerts WHERE event_id = :e", {"e": event_id}):
            continue
        alert_id = db.insert(
            "INSERT INTO alerts (device_id, node_sn, type, level, ts, ts_ms, event_id, status, recovered_at, "
            "snapshot_json, push_count, last_push_at, next_push_at) "
            "VALUES (:d, :n, :t, :l, :ts, :ts_ms, :eid, :st, :rec, :snap, 1, :lpa, :npa)",
            {
                "d": device_id, "n": node_sn, "t": atype, "l": level,
                "ts": ms_to_dt(ts), "ts_ms": ts, "eid": event_id, "st": status,
                "rec": ms_to_dt(recovered) if recovered else None,
                "snap": db.jdump(snapshot),
                "lpa": ms_to_dt(ts),
                # 待处置的给一个「5 分钟后重推」（§7.7 重推序列 0/60/300 s）：演示时列表能看见下次推送时刻，
                # 又不会在开场就被调度器推满 3 次
                "npa": ms_to_dt(now + 300 * 1000) if status == "pending" else None,
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


def run_demo(username: str = DEMO_USERNAME, password: str = DEMO_PASSWORD, make_bind_code: bool = False) -> dict:
    """把演示库重置到「D1–D7 都能演示」的确定状态（t49）。可重复执行，结果逐次相同。"""
    reset_tables()
    user_id = ensure_user(username, password)
    device_id = ensure_device(GATEWAY_SN, "客厅网关", user_id)
    ensure_membership(user_id, device_id, "owner")
    unbound_id = ensure_device(UNBOUND_SN, "备用网关", 0)
    ensure_nodes(device_id)
    ensure_nodes(unbound_id)

    activity = seed_demo_activity(device_id)
    readings = {"gateway": seed_demo_gateway(device_id), **seed_demo_nodes_readings(device_id)}
    seed_demo_online(device_id)
    alerts_created = seed_demo_alerts(device_id, user_id)

    # 归并 31 天活动报表（正常由调度器做；这里立刻做，否则月报要等 5 分钟才有数）
    today = ms_to_cst_date(now_ms())
    for offset in range(DEMO_ACTIVITY_DAYS):
        recompute_activity(today - timedelta(days=offset))

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
        "activity_days": len(activity),
        "today": DEMO_TODAY_ACTIVITY,
        "yesterday": DEMO_YESTERDAY_ACTIVITY,
        "readings": readings,
        **alerts_created,
    }


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
    parser.add_argument("--demo", action="store_true",
                        help="现场演示数据包：清空后按冻结时钟灌 31 天活动 + D1–D7 全量数据（隐含 --reset）")
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(name)s | %(message)s")
    if config.settings.is_prod:
        # --demo 隐含清库，与 --reset 同级：生产库一律拒绝（防手滑）
        if args.reset or args.demo:
            print("拒绝执行：APP_ENV=prod 下不允许 --reset/--demo", file=sys.stderr)
            return 2
        if args.password == DEMO_PASSWORD:
            print("拒绝执行：APP_ENV=prod 下必须用 --password 指定真实口令", file=sys.stderr)
            return 2

    db.ensure_database()
    db.apply_schema()
    if args.demo:
        summary = run_demo(username=args.username, password=args.password, make_bind_code=args.bind_code)
        print("演示数据完成：" + ", ".join(f"{key}={value}" for key, value in summary.items()))
        print("下一步：bash tools/demo-check.sh 逐条核对 D1–D7")
        return 0
    summary = run(reset=args.reset, days=args.days, username=args.username, password=args.password,
                  make_bind_code=args.bind_code)
    print("seed 完成：" + ", ".join(f"{key}={value}" for key, value in summary.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
