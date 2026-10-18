"""演示数据包（`seed --demo`）与演示自检（`app.demo_check`）的回归测试（t49）。

为什么这样测：现场跑 `bash tools/demo-check.sh` 用的就是 `app.demo_check.run`，
这里用 TestClient 调**同一份** run()，所以「本机 pytest 绿」= 「现场自检会绿」。
第二条关键断言是「与运行时刻无关」：冻结时钟设计若被改坏（比如有人把时刻改成 now-偏移），
`test_demo_data_does_not_depend_on_run_hour` 会立刻红。
"""
from __future__ import annotations

from dataclasses import replace
from datetime import timedelta

import pytest

from app import config
from app import db
from app import demo_check
from app import seed as seed_mod
from app.seed import (DEMO_BACKFILL_DELAY_MIN, DEMO_OFFLINE_HOUR, DEMO_OFFLINE_MINUTES, DEMO_TODAY_ACTIVITY,
                      DEMO_YESTERDAY_ACTIVITY)
from app.timeutil import cst_day_bounds_ms, ms_to_cst_date, now_ms

TEST_BASE = "http://testserver"


def _offline_window() -> tuple[int, int]:
    """今天的断网窗口 [12:00, 13:00) → (起始 ms, 末 ms)，与 seed 的常量保持一致。"""
    day_from, _ = cst_day_bounds_ms(ms_to_cst_date(now_ms()))
    start = day_from + DEMO_OFFLINE_HOUR * 3600 * 1000
    return start, start + DEMO_OFFLINE_MINUTES * 60 * 1000 - 1


def _gateway_gap_min(device_id: int) -> float:
    stamps = sorted(int(row["ts_ms"]) for row in db.fetch_all(
        "SELECT ts_ms FROM sensor_readings WHERE device_id = :d AND type = 'gateway'", {"d": device_id}))
    gap = max((b - a for a, b in zip(stamps, stamps[1:])), default=0)
    return gap / 60000


def _pattern(device_id: int) -> dict:
    """与「今天」无关的数据形状：跨天重跑必须完全相同。"""
    days = [int(row["count"]) for row in db.fetch_all(
        "SELECT `count` FROM activity_daily WHERE device_id = :d ORDER BY `date` DESC LIMIT 3", {"d": device_id})]
    alerts = sorted((row["type"], int(row["level"]), row["status"]) for row in db.fetch_all(
        "SELECT type, level, status FROM alerts WHERE device_id = :d", {"d": device_id}))
    return {
        "days": days,
        "alerts": alerts,
        "readings": int(db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d",
                                       {"d": device_id}, 0) or 0),
        "gateway_gap_min": round(_gateway_gap_min(device_id), 1),
    }


# --------------------------------------------------------------------------- 数据包本身

def test_seed_demo_produces_reproducible_dataset(client):
    """--demo 必须一次给全 D1–D7 要用的数据，且条数与设计值一致（数字写死在 seed 里，便于复算）。"""
    summary = seed_mod.run_demo()
    device_id = summary["device_id"]
    today = ms_to_cst_date(now_ms())

    assert summary["activity_days"] == 60 and summary["alerts"] == 10
    # 96×3 天的网关快照减去今天断网窗口里被刻意跳过的 4 个点
    assert summary["readings"] == {"gateway": 284, "gas": 16, "fall": 4, "door": 5, "backfill": 11}
    assert db.fetch_scalar("SELECT `count` FROM activity_daily WHERE device_id = :d AND `date` = :day",
                           {"d": device_id, "day": today}, 0) == DEMO_TODAY_ACTIVITY
    assert db.fetch_scalar("SELECT `count` FROM activity_daily WHERE device_id = :d AND `date` = :day",
                           {"d": device_id, "day": today - timedelta(days=1)}, 0) == DEMO_YESTERDAY_ACTIVITY

    types = {row["type"] for row in db.fetch_all("SELECT DISTINCT type FROM alerts WHERE device_id = :d",
                                                 {"d": device_id})}
    assert types == {"gas", "fall", "sos", "no_motion", "door_left", "device_offline", "node_offline"}
    statuses = {row["status"] for row in db.fetch_all("SELECT DISTINCT status FROM alerts WHERE device_id = :d",
                                                      {"d": device_id})}
    assert statuses == {"pending", "resolved"}

    # D6 的库内证据：窗口内无网关上报、跳变够大、补传是恢复之后才入库的
    start, last = _offline_window()
    assert db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d AND type = 'gateway' "
                           "AND ts_ms BETWEEN :f AND :t", {"d": device_id, "f": start, "t": last}, 0) == 0
    assert _gateway_gap_min(device_id) >= 55
    window_rows = int(db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d "
                                      "AND ts_ms BETWEEN :f AND :t", {"d": device_id, "f": start, "t": last}, 0) or 0)
    backfill = int(db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d "
                                   "AND ts_ms BETWEEN :f AND :t AND created_at > FROM_UNIXTIME(:end / 1000)",
                                   {"d": device_id, "f": start, "t": last, "end": last + 1}, 0) or 0)
    assert window_rows == 14, "窗口内读数 = 11 条补传 + 12:00 燃气背景 + 12:12/12:48 两个活动点"
    assert backfill == 11
    created_ms = int(db.fetch_scalar("SELECT UNIX_TIMESTAMP(MIN(created_at)) * 1000 FROM sensor_readings "
                                     "WHERE device_id = :d AND ts_ms BETWEEN :f AND :t "
                                     "AND created_at > FROM_UNIXTIME(:end / 1000)",
                                     {"d": device_id, "f": start, "t": last, "end": last + 1}, 0) or 0)
    assert created_ms == last + 1 + DEMO_BACKFILL_DELAY_MIN * 60 * 1000, "补传的入库时刻应为窗口结束后 3 分钟"

    online = db.fetch_one("SELECT online, alarm_state, last_properties_json FROM device_online WHERE device_id = :d",
                          {"d": device_id})
    assert int(online["online"]) == 1 and int(online["alarm_state"]) == 2
    assert "bound_nodes" in (online["last_properties_json"] or "")


def test_seed_demo_is_repeatable(client):
    """可重复执行：跑两次条数不翻倍、形状不变（验收要求「幂等、可重复执行」）。"""
    first = seed_mod.run_demo()
    before = _pattern(first["device_id"])
    second = seed_mod.run_demo()
    assert second["device_id"] == first["device_id"]
    assert second["readings"] == first["readings"]
    assert _pattern(second["device_id"]) == before
    assert db.fetch_scalar("SELECT COUNT(*) FROM devices WHERE sn = :s", {"s": seed_mod.GATEWAY_SN}, 0) == 1


def test_demo_data_does_not_depend_on_run_hour(client, monkeypatch):
    """冻结时钟：把「现在」往后推 10 小时（可能跨 CST 零点）重跑，数据形状必须一模一样。"""
    device_id = seed_mod.run_demo()["device_id"]
    before = _pattern(device_id)

    shifted = now_ms() + 10 * 3600 * 1000
    monkeypatch.setattr(seed_mod, "now_ms", lambda: shifted)
    again = seed_mod.run_demo()

    assert again["device_id"] == device_id
    assert _pattern(device_id) == before


# --------------------------------------------------------------------------- 自检脚本

def test_demo_check_passes_on_demo_data(client):
    """现场自检在本机也要全绿（同一份 run()，所以这里绿 = 现场绿）。"""
    seed_mod.run_demo()
    assert demo_check.run(client, base=TEST_BASE) == 0


def test_demo_check_does_not_consume_pending_alert(client):
    """自检不得改变被检状态：§6.13 规定打开 pending 详情即翻 confirmed，而演示要靠它演处置。"""
    seed_mod.run_demo()
    before = db.fetch_one("SELECT id, status, next_push_at FROM alerts WHERE type = 'gas' AND level = 2")
    assert before and before["status"] == "pending"
    assert demo_check.run(client, base=TEST_BASE, quiet=True) == 0
    after = db.fetch_one("SELECT status, next_push_at FROM alerts WHERE id = :i", {"i": before["id"]})
    assert after["status"] == "pending", "自检把待处置燃气报警吃掉了"
    assert after["next_push_at"] == before["next_push_at"], "自检没有恢复 next_push_at（重推时刻）"


def test_demo_check_fails_on_empty_db(client):
    """反向验证：库是空的（没灌 --demo）时必须红，否则自检没有意义。"""
    assert demo_check.run(client, base=TEST_BASE, quiet=True) == 1


# --------------------------------------------------------------------------- 安全边界

def test_seed_demo_refused_in_prod(client, monkeypatch):
    """--demo 隐含清库，生产环境必须拒绝（与 --reset 同级）。"""
    monkeypatch.setattr(config, "settings", replace(config.settings, app_env="prod"))
    assert seed_mod.main(["--demo"]) == 2
    with pytest.raises(RuntimeError):
        seed_mod.run_demo()
