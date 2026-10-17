"""§6.16–§6.18 报表：活动时段 / 趋势 / 汇总。

报表的日与小时一律按 UTC+8 切分（§7.6），所以固定日期用 `cst_day_bounds_ms` 反推，
不写死 UTC 时刻——否则测试在不同机器上会随容器时区漂移。
"""
from __future__ import annotations

from app import db
from app.timeutil import cst_day_bounds_ms, ms_to_cst_date, ms_to_dt, now_ms

from conftest import create_user, login


def _auth(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def _seed_presence(device_id: int, day, hour: int, count: int, minute: int = 0) -> None:
    """往指定 UTC+8 日/小时灌 presence=true 读数（报表只统计 presence=true）。"""
    base, _ = cst_day_bounds_ms(day)
    ts0 = base + hour * 3_600_000 + minute * 60_000
    for index in range(count):
        ts = ts0 + index * 1_000
        db.execute(
            "INSERT IGNORE INTO sensor_readings (device_id, node_sn, type, ts, ts_ms, payload_json) "
            "VALUES (:d, 'M4', 'presence', :ts, :ts_ms, :p)",
            {"d": device_id, "ts": ms_to_dt(ts), "ts_ms": ts,
             "p": db.jdump({"presence": True, "last_motion_ts": ts})},
        )


# --------------------------------------------------------------------------- §6.16 活动

def _first_reading_hour(device_id: int) -> int:
    """本次灌入的读数落在哪个 UTC+8 小时（seed_readings 遇到未来小时会落回当前时刻）。"""
    from app.timeutil import cst_hour

    row = db.fetch_one("SELECT MIN(ts_ms) AS m FROM sensor_readings WHERE device_id = :d", {"d": device_id})
    return cst_hour(int(row["m"]))


def test_activity_day_has_24_hours(client, device, seed_readings):
    seed_readings(count=3, hour=9)
    hour = _first_reading_hour(device["id"])
    day = ms_to_cst_date(now_ms())
    resp = client.get("/api/reports/activity", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "period": "day", "date": day.isoformat()})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert set(data) == {"device_id", "period", "from", "to", "total", "hourly", "daily"}
    assert data["period"] == "day"
    assert len(data["hourly"]) == 24                      # §6.16：day → 24 项
    assert [h["hour"] for h in data["hourly"]] == list(range(24))
    assert len(data["daily"]) == 1 and data["daily"][0]["date"] == day.isoformat()
    assert data["total"] == 3
    assert data["daily"][0]["count"] == 3
    assert data["hourly"][hour]["count"] == 3
    assert sum(h["count"] for h in data["hourly"]) == 3          # 没有别的桶被误算
    # from/to 必须是该 UTC+8 日历日的闭区间毫秒
    assert (data["from"], data["to"]) == cst_day_bounds_ms(day)


def test_activity_week_omits_hourly(client, device):
    day = ms_to_cst_date(now_ms())
    resp = client.get("/api/reports/activity", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "period": "week", "date": day.isoformat()})
    data = resp.json()["data"]
    assert "hourly" not in data                            # §6.16：week/month 省略 hourly（不是空数组）
    assert len(data["daily"]) == 7
    assert data["daily"][-1]["date"] == day.isoformat()    # 以 date 结尾
    assert data["total"] == 0


def test_activity_month_has_30_days(client, device):
    day = ms_to_cst_date(now_ms())
    data = client.get("/api/reports/activity", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "period": "month", "date": day.isoformat()}).json()["data"]
    assert len(data["daily"]) == 30


def test_activity_no_data_is_empty_not_error(client, device):
    """§9.2 1400：报表无数据不是错误，回空数组 + total=0。"""
    day = ms_to_cst_date(now_ms())
    resp = client.get("/api/reports/activity", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "date": day.isoformat()})
    assert resp.status_code == 200
    body = resp.json()
    assert body["code"] == 0 and body["data"]["total"] == 0


def test_activity_bad_period_1002(client, device):
    resp = client.get("/api/reports/activity", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "period": "year"})
    assert resp.status_code == 400 and resp.json()["code"] == 1002


def test_activity_stranger_1104(client, device):
    stranger = create_user()
    token = login(client, stranger["username"])
    resp = client.get("/api/reports/activity", headers=_auth(token), params={"device_id": device["id"]})
    assert resp.status_code == 403 and resp.json()["code"] == 1104


# --------------------------------------------------------------------------- §6.17 趋势

def test_trend_series_length_and_today(client, device, seed_readings):
    seed_readings(count=2, hour=9)
    resp = client.get("/api/reports/trend", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "days": 7})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert set(data) == {"device_id", "days", "series", "compare"}
    assert data["days"] == 7 and len(data["series"]) == 7
    assert [s["date"] for s in data["series"]] == sorted(s["date"] for s in data["series"])
    assert data["series"][-1]["date"] == ms_to_cst_date(now_ms()).isoformat()
    compare = data["compare"]
    assert set(compare) == {"today", "yesterday", "delta_pct", "text"}
    assert compare["today"] == 2 and compare["yesterday"] == 0
    assert compare["text"] == "昨日无活动记录"


def test_trend_compare_text_is_integer_percent(client, device):
    """§6.17 文案：delta_pct 取整，>+10 / <-10 才说升降，其余「基本持平」。"""
    today = ms_to_cst_date(now_ms())
    yesterday = today.fromordinal(today.toordinal() - 1)
    _seed_presence(device["id"], today, 10, 11)          # 今天 11
    _seed_presence(device["id"], yesterday, 9, 10)       # 昨天 10 → +10% → 持平
    from app.services.scheduler import recompute_activity

    recompute_activity(today)
    recompute_activity(yesterday)
    compare = client.get("/api/reports/trend", headers=_auth(device["token"]),
                         params={"device_id": device["id"], "days": 2}).json()["data"]["compare"]
    assert (compare["today"], compare["yesterday"]) == (11, 10)
    assert compare["delta_pct"] == 10
    assert compare["text"] == "与昨日基本持平"

    db.execute("TRUNCATE TABLE sensor_readings")
    db.execute("TRUNCATE TABLE activity_hourly")
    db.execute("TRUNCATE TABLE activity_daily")
    _seed_presence(device["id"], today, 1, 1)            # 今天 1 次（hour=1, count=1）
    _seed_presence(device["id"], yesterday, 10, 10)      # 昨天 10 → -90%
    recompute_activity(today)
    recompute_activity(yesterday)
    compare = client.get("/api/reports/trend", headers=_auth(device["token"]),
                         params={"device_id": device["id"], "days": 2}).json()["data"]["compare"]
    assert compare["delta_pct"] == -90
    assert compare["text"] == "较昨日下降 90%"      # §6.17 规则行原文（见 README「契约口径」）


def test_trend_days_bounds(client, device):
    resp = client.get("/api/reports/trend", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "days": 91})
    assert resp.status_code == 400 and resp.json()["code"] == 1002


# --------------------------------------------------------------------------- §6.18 汇总

def test_summary_shape(client, device, seed_readings):
    seed_readings(count=4, hour=9)
    day = ms_to_cst_date(now_ms())
    resp = client.get("/api/reports/summary", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "period": "week", "date": day.isoformat()})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert set(data) == {"device_id", "period", "from", "to", "activity", "alerts", "env"}
    activity = data["activity"]
    assert set(activity) == {"total", "active_days", "avg_daily", "peak_hour", "min_hour", "compare_prev"}
    assert activity["total"] == 4 and activity["active_days"] == 1
    assert activity["peak_hour"] == _first_reading_hour(device["id"])
    assert isinstance(activity["compare_prev"], str) and activity["compare_prev"]
    alerts = data["alerts"]
    assert set(alerts) == {"total", "by_type", "resolved", "pending", "false_alarm"}
    assert alerts["total"] == 0 and alerts["by_type"] == {}
    assert set(data["env"]) == {"temp_avg", "temp_min", "temp_max", "humi_avg"}


def test_summary_alerts_and_false_alarm_counting(client, device):
    """§6.18：false_alarm 处置计入误报统计；by_type 分行计数。"""
    from test_40_alerts import push_alert

    push_alert(client, device)
    push_alert(client, device, alert_type="gas", level=1, node_sn="M1")
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"], "type": "fall"}).json()["data"]["items"][0]["id"]
    client.post(f"/api/alerts/{alert_id}/disposition", headers=_auth(device["token"]),
                json={"action": "false_alarm", "note": "猫碰倒的"})
    day = ms_to_cst_date(now_ms())
    alerts = client.get("/api/reports/summary", headers=_auth(device["token"]),
                        params={"device_id": device["id"], "period": "week", "date": day.isoformat()}
                        ).json()["data"]["alerts"]
    assert alerts["total"] == 2
    assert alerts["by_type"] == {"fall": 1, "gas": 1}
    assert alerts["resolved"] == 1 and alerts["pending"] == 1
    assert alerts["false_alarm"] == 1


def test_summary_env_stats_from_gateway_readings(client, device):
    """env 段取网关属性快照里的 temp/humi（node_sn='M0'，type='gateway'）。"""
    from app.services.telemetry import handle_property

    device_row = db.fetch_one("SELECT * FROM devices WHERE id = :d", {"d": device["id"]})
    for temp, humi in ((22.5, 45.0), (28.5, 61.0)):
        handle_property(device_row, {"services": [{"service_id": "gateway", "properties": {
            "temp": temp, "humi": humi, "smoke": 120, "presence": True}}]})
    day = ms_to_cst_date(now_ms())
    env = client.get("/api/reports/summary", headers=_auth(device["token"]),
                     params={"device_id": device["id"], "period": "month", "date": day.isoformat()}
                     ).json()["data"]["env"]
    assert env["temp_min"] == 22.5 and env["temp_max"] == 28.5
    assert env["temp_avg"] == 25.5
    assert env["humi_avg"] == 53.0


def test_summary_period_day_rejected(client, device):
    resp = client.get("/api/reports/summary", headers=_auth(device["token"]),
                      params={"device_id": device["id"], "period": "day"})
    assert resp.status_code == 400 and resp.json()["code"] == 1002


def test_summary_requires_device_id(client, user):
    resp = client.get("/api/reports/summary", headers=_auth(user["token"]))
    assert resp.status_code == 400
    assert resp.json()["code"] == 1000


# --------------------------------------------------------------------------- 归并调度

def test_recompute_activity_is_idempotent(client, device, seed_readings):
    """§7.9：归并每 5 分钟刷新 —— 重复跑必须收敛到同一结果（用 REPLACE INTO 而非累加）。"""
    from app.services.scheduler import recompute_activity

    seed_readings(count=5, hour=14)
    day = ms_to_cst_date(now_ms())
    first = db.fetch_scalar("SELECT count FROM activity_hourly WHERE device_id = :d AND hour = 14",
                            {"d": device["id"]}, 0)
    recompute_activity(day)
    recompute_activity(day)
    second = db.fetch_scalar("SELECT count FROM activity_hourly WHERE device_id = :d AND hour = 14",
                             {"d": device["id"]}, 0)
    assert int(first) == int(second) == 5
    daily = db.fetch_scalar("SELECT count FROM activity_daily WHERE device_id = :d", {"d": device["id"]}, 0)
    assert int(daily) == 5
