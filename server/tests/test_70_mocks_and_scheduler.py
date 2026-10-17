"""§6.19 推送注册、§6.12 演示链路、§8.7 离线判定、CAL-09 时钟纠正、种子脚本。

这组测试都在「无硬件」前提下把要交给验收/演示的路径跑一遍：
虚拟网关只替换华为云跳板，入库、去重、重推、报表全部是生产代码。
"""
from __future__ import annotations

import re

import pytest
from dataclasses import replace

from app import db
from app.config import settings
from app.services.scheduler import tick_gateway_offline, tick_once
from app.timeutil import ms_to_dt, now_ms

from conftest import GATEWAY_SN, create_device, create_membership, create_nodes, create_user, login


def _auth(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


# --------------------------------------------------------------------------- §6.19 推送注册

def test_push_register_and_idempotent_update(client, user):
    payload = {"push_token": "harmony-push-token-0001", "platform": "harmony"}
    resp = client.post("/api/push/register", headers=_auth(user["token"]), json=payload)
    assert resp.status_code == 200
    assert resp.json()["data"] == {"registered": True, "user_id": user["id"]}

    first = db.fetch_one("SELECT id, created_at, updated_at FROM push_tokens WHERE user_id = :u",
                         {"u": user["id"]})
    client.post("/api/push/register", headers=_auth(user["token"]), json=payload)
    rows = db.fetch_all("SELECT id FROM push_tokens WHERE user_id = :u", {"u": user["id"]})
    assert len(rows) == 1                                    # §6.19：同 token 幂等，不新增行
    again = db.fetch_one("SELECT id FROM push_tokens WHERE user_id = :u", {"u": user["id"]})
    assert again["id"] == first["id"]


def test_push_register_rejects_bad_platform_and_token(client, user):
    bad_platform = client.post("/api/push/register", headers=_auth(user["token"]),
                               json={"push_token": "harmony-push-token-0002", "platform": "ios"})
    assert bad_platform.status_code == 400 and bad_platform.json()["code"] == 1002
    short = client.post("/api/push/register", headers=_auth(user["token"]), json={"push_token": "abc"})
    assert short.status_code == 400 and short.json()["code"] in (1001, 1002)


def test_push_register_requires_login(client):
    resp = client.post("/api/push/register", json={"push_token": "harmony-push-token-0003"})
    assert resp.status_code == 401 and resp.json()["code"] == 1100


# --------------------------------------------------------------------------- §6.12 演示链路

def test_demo_gas_runs_through_real_chain(client, device):
    resp = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(device["token"]), json={"event": "gas"})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["event"] == "gas"
    assert data["source"] == "mock-virtual-gateway"         # mock 模式：华为云跳板被替换，其余同构
    alert = data["alert"]
    assert alert["type"] == "gas" and alert["level"] == 2 and alert["node_sn"] == "M1"
    assert alert["summary"] == "厨房燃气泄漏报警"
    kinds = {row["type"] for row in db.fetch_all(
        "SELECT DISTINCT type FROM sensor_readings WHERE device_id = :d", {"d": device["id"]})}
    assert {"gas", "gateway"} <= kinds                      # 节点明细 + 网关属性快照都入库


def test_demo_sos_creates_level2_alert(client, device):
    data = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(device["token"]),
                       json={"event": "sos"}).json()["data"]
    assert data["alert"]["type"] == "sos" and data["alert"]["level"] == 2
    assert data["alert"]["summary"] == "收到紧急求助"


def test_demo_twice_within_suppress_window_does_not_duplicate(client, device):
    """§8.8：跌倒 30 s 抑制窗口内连点两次，只应有一条告警，且不重置重推计数。"""
    first = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(device["token"]),
                        json={"event": "fall"}).json()["data"]["alert"]
    second = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(device["token"]),
                         json={"event": "fall"}).json()["data"]["alert"]
    assert second["id"] == first["id"]
    assert second["push_count"] == 1
    assert db.fetch_scalar("SELECT COUNT(*) FROM alerts WHERE device_id = :d AND type = 'fall'",
                           {"d": device["id"]}, 0) == 1


def test_demo_requires_membership(client, device):
    stranger = create_user()
    token = login(client, stranger["username"])
    resp = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(token), json={"event": "fall"})
    assert resp.status_code == 403 and resp.json()["code"] == 1104


# --------------------------------------------------------------------------- §8.7 离线判定

def test_gateway_offline_after_three_report_periods(client, device):
    """§8.7：180 s 无任何上报（3×60 s）→ device_online.online=0 + device_offline 告警。"""
    # 先让网关上线（模拟一次属性上报），再把它最后一次心跳挪到 200 s 之前
    client.post(f"/api/iotda/webhook?token={settings.webhook_token}", json={
        "resource": "device.property",
        "notify_data": {"header": {"device_id": device["sn"]}, "body": {"services": [
            {"service_id": "gateway", "properties": {"temp": 24.0, "humi": 50.0, "smoke": 100}}]}},
    })
    stale = now_ms() - 200_000
    db.execute("UPDATE device_online SET last_heartbeat_ms = :m, last_heartbeat = :d WHERE device_id = :id",
               {"m": stale, "d": ms_to_dt(stale), "id": device["id"]})

    offlined = tick_gateway_offline()
    assert offlined == [device["id"]]
    assert db.fetch_scalar("SELECT online FROM device_online WHERE device_id = :d", {"d": device["id"]}, 1) == 0
    alert = db.fetch_one("SELECT type, level, event_id, status FROM alerts WHERE device_id = :d",
                         {"d": device["id"]})
    assert alert["type"] == "device_offline" and int(alert["level"]) == 1
    assert alert["event_id"] == f"{GATEWAY_SN}-device_offline-{stale}"   # 同一次离线只报一条
    assert alert["status"] == "pending"

    # 再跑一轮：同一次离线不得重复告警（event_id 幂等）
    tick_gateway_offline()
    assert db.fetch_scalar("SELECT COUNT(*) FROM alerts WHERE device_id = :d", {"d": device["id"]}, 0) == 1

    # 新上报（含明细）到达 → 恢复在线，并清掉 device_offline 告警
    client.post(f"/api/iotda/webhook?token={settings.webhook_token}", json={
        "resource": "device.message",
        "notify_data": {"header": {"device_id": device["sn"]}, "body": {"services": [
            {"service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{now_ms()}",
             "content": '[{"sn":"M4","ts":0,"type":"presence","payload":{"presence":true,"last_motion_ts":0}}]'}]}},
    })
    assert db.fetch_scalar("SELECT online FROM device_online WHERE device_id = :d", {"d": device["id"]}, 0) == 1
    assert db.fetch_one("SELECT recovered_at FROM alerts WHERE device_id = :d",
                        {"d": device["id"]})["recovered_at"] is not None


def test_gateway_offline_timeout_respects_device_setting(client, device):
    """§8.7 的超时可被设备级设置覆盖（App 里改过就以库里的为准）。"""
    client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
               json={"items": {"gateway_offline_timeout_s": 1800}})
    client.post(f"/api/iotda/webhook?token={settings.webhook_token}", json={
        "resource": "device.property",
        "notify_data": {"header": {"device_id": device["sn"]}, "body": {"services": [
            {"service_id": "gateway", "properties": {"temp": 24.0, "humi": 50.0}}]}},
    })
    stale = now_ms() - 200_000
    db.execute("UPDATE device_online SET last_heartbeat_ms = :m WHERE device_id = :id",
               {"m": stale, "id": device["id"]})
    assert tick_gateway_offline() == []                     # 放宽到 30 min 后不应判定离线


def test_gateway_offline_uses_db_setting_not_default(client, device):
    """反向验证：即使把默认值改小，也必须以 devices.settings_json 为准。"""
    client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
               json={"items": {"gateway_offline_timeout_s": 60}})
    client.post(f"/api/iotda/webhook?token={settings.webhook_token}", json={
        "resource": "device.property",
        "notify_data": {"header": {"device_id": device["sn"]}, "body": {"services": [
            {"service_id": "gateway", "properties": {"temp": 24.0}}]}},
    })
    stale = now_ms() - 90_000
    db.execute("UPDATE device_online SET last_heartbeat_ms = :m WHERE device_id = :id",
               {"m": stale, "id": device["id"]})
    assert tick_gateway_offline() == [device["id"]]


# --------------------------------------------------------------------------- CAL-09 时钟纠正

def test_node_timestamps_are_corrected_to_server_time(client, device):
    """§0.2/CAL-09：节点 ts 是自开机毫秒数，服务器按接收时刻纠正，不能信 0 或 1970。"""
    before = now_ms()
    client.post(f"/api/iotda/webhook?token={settings.webhook_token}", json={
        "resource": "device.message",
        "notify_data": {"header": {"device_id": device["sn"]}, "body": {"services": [
            {"service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{before}",
             "content": '[{"sn":"M1","ts":0,"type":"gas","payload":{"smoke":700,"temp":25.0,"humi":52.0}}]'}]}},
    })
    row = db.fetch_one("SELECT ts_ms FROM sensor_readings WHERE device_id = :d AND node_sn = 'M1'",
                       {"d": device["id"]})
    assert before <= int(row["ts_ms"]) <= now_ms()          # 落到服务器接收时刻，而不是 0/1970
    health = client.get("/api/health").json()["data"]
    assert health["clock_source"] == "server"


def test_batch_samples_are_millisecond_offset_to_avoid_collision(client, device):
    """同批多条节点明细若都用服务器同一毫秒，会被唯一键吃掉 → 逐条 +1 ms 错开。"""
    content = (
        '[{"sn":"M1","ts":0,"type":"gas","payload":{"smoke":700,"temp":25.0,"humi":52.0}},'
        '{"sn":"M2","ts":0,"type":"fall","payload":{"fall":0,"presence":1}},'
        '{"sn":"M4","ts":0,"type":"door","payload":{"door":false,"window":false}}]'
    )
    client.post(f"/api/iotda/webhook?token={settings.webhook_token}", json={
        "resource": "device.message",
        "notify_data": {"header": {"device_id": device["sn"]}, "body": {"services": [
            {"service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{now_ms()}", "content": content}]}},
    })
    types = [row["type"] for row in db.fetch_all(
        "SELECT type FROM sensor_readings WHERE device_id = :d ORDER BY id", {"d": device["id"]})]
    assert types == ["gas", "fall", "door"]                 # 三条都在，没有互相覆盖


# --------------------------------------------------------------------------- 种子数据

def test_seed_produces_demo_dataset(client):
    """`python -m app.seed --reset` 是演示与验收的起点，必须真能灌出可看的报表数据。"""
    from app import seed as seed_mod

    result = seed_mod.run(reset=True, days=3)
    assert result["device_id"] > 0 and result["user_id"] > 0
    assert db.fetch_scalar("SELECT COUNT(*) FROM nodes WHERE device_id = :d", {"d": result["device_id"]}, 0) == 4
    assert db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d",
                           {"d": result["device_id"]}, 0) > 50
    alerts = db.fetch_all("SELECT DISTINCT status FROM alerts WHERE device_id = :d", {"d": result["device_id"]})
    assert {"pending", "resolved"} <= {row["status"] for row in alerts}
    days = db.fetch_scalar("SELECT COUNT(*) FROM activity_daily WHERE device_id = :d", {"d": result["device_id"]}, 0)
    assert int(days) >= 2                                   # 活动日报表有数据，App 的图表才有东西画

    # 幂等：再跑一次不新增设备/用户/节点
    again = seed_mod.run(days=3)
    assert again["device_id"] == result["device_id"]
    assert db.fetch_scalar("SELECT COUNT(*) FROM devices WHERE sn = :s", {"s": GATEWAY_SN}, 0) == 1
    assert db.fetch_scalar("SELECT COUNT(*) FROM nodes WHERE device_id = :d", {"d": result["device_id"]}, 0) == 4


def test_seed_refuses_reset_in_prod(client, monkeypatch):
    """种子脚本带 --reset 会清库，生产环境必须拒绝执行（防手滑）。两层都要拦。"""
    from app import config
    from app import seed as seed_mod

    # Settings 是 frozen dataclass（避免运行时被悄悄改坏），所以要整对象替换
    monkeypatch.setattr(config, "settings", replace(config.settings, app_env="prod"))
    assert seed_mod.main(["--reset"]) == 2                   # CLI 层：拒绝并返回非零
    with pytest.raises(RuntimeError):                        # 服务层：直接被调用也拦
        seed_mod.run(reset=True)
    assert seed_mod.main(["--password", "stronger-pass-9"]) == 0  # 显式口令则放行


def test_mock_seed_user_can_login_and_see_report(client):
    """种子账号能登录 + 能看到自己的设备和报表，是 App 侧联调的第一条路。"""
    from app import seed as seed_mod

    result = seed_mod.run(reset=True)
    token = login(client, seed_mod.DEMO_USERNAME, seed_mod.DEMO_PASSWORD)
    devices = client.get("/api/devices", headers=_auth(token)).json()["data"]
    assert devices["total"] == 1
    device_id = devices["items"][0]["id"]
    assert devices["items"][0]["unread_alerts"] >= 1        # 种子里有一条待处置告警
    activity = client.get("/api/reports/activity", headers=_auth(token),
                          params={"device_id": device_id, "period": "day"}).json()["data"]
    assert activity["total"] > 0


def test_tick_once_reports_each_job(client, device):
    result = tick_once()
    assert set(result) == {"repushed", "offlined", "activity"}
    assert result["repushed"] == 0 and result["offlined"] == []
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}", result["activity"]["today"]["day"])


def test_scheduler_skips_offline_devices_without_heartbeat(client):
    """没有 device_online 行的设备（从未上报）不能被判定离线，也不能产生告警。"""
    device_id = create_device()
    user = create_user()
    create_membership(user["id"], device_id, "owner")
    create_nodes(device_id)
    assert tick_gateway_offline() == []
    assert db.fetch_scalar("SELECT COUNT(*) FROM alerts", {}, 0) == 0
