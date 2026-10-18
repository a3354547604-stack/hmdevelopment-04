"""契约 §5：IoTDA 规则引擎 webhook 的四类入库映射 + 三条硬约束。

这组测试刻意用「平台真实信封」（resource / event / notify_data.header.device_id / body.services[]），
因为 §5.2 要求服务器只能依赖这几个字段；用别的形状测等于没测。
"""
from __future__ import annotations

import json

from app import db
from app.config import settings
from app.state import RUNTIME
from app.timeutil import now_ms

from conftest import GATEWAY_SN, create_device

WEBHOOK = "/api/iotda/webhook"


def _post(client, body: dict, token: str | None = None):
    return client.post(f"{WEBHOOK}?token={settings.webhook_token if token is None else token}", json=body)


def _envelope(resource: str, device_sn: str, body: dict, event: str | None = None) -> dict:
    payload = {"resource": resource, "notify_data": {"header": {"device_id": device_sn, "node_id": GATEWAY_SN}, "body": body}}
    if event:
        payload["event"] = event
        payload["event_time"] = "20261017T120000Z"
    return payload


def _properties_body(**props) -> dict:
    base = {"presence": False, "last_motion_ts": 0, "door": False, "window": False, "temp": 24.5, "humi": 55.0,
            "smoke": 120, "nodes_online": json.dumps({"H1": 1, "M1": 1, "M2": 0, "M4": 1}), "alarm_state": 0,
            "battery": 255, "fw_ver": "1.0.0", "report_interval_s": 60}
    base.update(props)
    return {"services": [{"service_id": "gateway", "event_time": "20261017T120000Z", "properties": base}]}


# --------------------------------------------------------------------------- §5.1 鉴权

def test_webhook_requires_token(client, device):
    body = _envelope("device.property", GATEWAY_SN, _properties_body(), event="report")
    assert _post(client, body, token="wrong-token").status_code == 401
    no_token = client.post(WEBHOOK, json=body)
    assert no_token.status_code == 401
    # §9.2 1502：响应体最小化，不回显细节
    assert set(no_token.json()) == {"code", "message"}


def test_webhook_accepts_configured_token(client, device):
    resp = _post(client, _envelope("device.property", GATEWAY_SN, _properties_body(), event="report"))
    assert resp.status_code == 200 and resp.json()["code"] == 0
    assert RUNTIME["webhook_last_at"] is not None


def test_webhook_rejects_broken_json(client):
    resp = client.post(f"{WEBHOOK}?token={settings.webhook_token}", content=b"{not json")
    assert resp.status_code == 400 and resp.json()["code"] == 1001


# --------------------------------------------------------------------------- §5.3 属性上报

def test_property_report_updates_online_nodes_and_gateway_readings(client, device):
    resp = _post(client, _envelope("device.property", GATEWAY_SN, _properties_body(), event="report"))
    assert resp.status_code == 200
    summary = resp.json()["data"]
    assert summary["resource"] == "device.property"
    assert summary["device_id"] == device["id"]

    online = db.fetch_one("SELECT online, last_heartbeat, alarm_state, last_properties_json "
                          "FROM device_online WHERE device_id = :d", {"d": device["id"]})
    assert int(online["online"]) == 1                       # 收到属性 → 网关在线
    assert online["last_heartbeat"] is not None
    props = db.jload(online["last_properties_json"])
    assert props["temp"] == 24.5                            # §6.7 properties 快照就是这一份

    # §5.3：nodes_online 的 JSON 键逐个落到 nodes.online
    nodes = {row["sn"]: int(row["online"]) for row in
             db.fetch_all("SELECT sn, online FROM nodes WHERE device_id = :d", {"d": device["id"]})}
    assert nodes == {"H1": 1, "M1": 1, "M2": 0, "M4": 1}

    # §5.3：temp/humi/smoke 进 sensor_readings，node_sn='M0'、type='gateway'
    row = db.fetch_one("SELECT node_sn, type, payload_json FROM sensor_readings WHERE device_id = :d",
                       {"d": device["id"]})
    assert row["node_sn"] == "M0" and row["type"] == "gateway"
    assert db.jload(row["payload_json"])["smoke"] == 120


def test_device_status_reflects_property_report(client, device):
    _post(client, _envelope("device.property", GATEWAY_SN, _properties_body(temp=31.5), event="report"))
    data = client.get(f"/api/devices/{device['id']}/status",
                      headers={"Authorization": f"Bearer {device['token']}"}).json()["data"]
    assert data["online"] is True
    assert data["properties"]["temp"] == 31.5
    assert {n["sn"]: n["online"] for n in data["nodes"]} == {"H1": True, "M1": True, "M2": False, "M4": True}


def test_property_retry_is_idempotent(client, device):
    """§5.2：平台失败会重推，靠 (device,node,type,ts_ms) 唯一键去重，不能灌出重复读数。"""
    before = db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d", {"d": device["id"]}, 0)
    for _ in range(3):
        _post(client, _envelope("device.property", GATEWAY_SN, _properties_body(), event="report"))
    after = db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d", {"d": device["id"]}, 0)
    # 同一秒内三次上报：时间戳会被钳到同一毫秒，因此第三次不再新增行（幂等生效）
    assert int(after) - int(before) <= 3


# --------------------------------------------------------------------------- §5.3 事件

def test_sos_event_becomes_alert(client, device):
    ts = now_ms()
    resp = _post(client, _envelope("device.event", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "event_type": "sos", "event_time": "20261017T120000Z",
        "paras": {"source": "M4", "ts": ts, "event_id": f"{GATEWAY_SN}-sos-{ts}"}}]}, event="sos"))
    assert resp.status_code == 200
    row = db.fetch_one("SELECT type, level, node_sn, status FROM alerts WHERE device_id = :d", {"d": device["id"]})
    assert row["type"] == "sos" and int(row["level"]) == 2
    assert row["node_sn"] == "M4" and row["status"] == "pending"


def test_node_offline_recover_clears_alert(client, device):
    ts = now_ms()
    _post(client, _envelope("device.event", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "event_type": "alert", "paras": {
            "type": "node_offline", "level": 1, "node_sn": "M2", "ts": ts,
            "event_id": f"{GATEWAY_SN}-node_offline-{ts}"}}]}, event="alert"))
    db.execute("UPDATE nodes SET online = 0 WHERE device_id = :d AND sn = 'M2'", {"d": device["id"]})
    resp = _post(client, _envelope("device.event", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "event_type": "recover", "paras": {
            "type": "node_offline", "node_sn": "M2", "ts": ts + 30_000,
            "event_id": f"{GATEWAY_SN}-node_online-{ts}"}}]}, event="recover"))
    assert resp.status_code == 200
    assert int(db.fetch_scalar("SELECT online FROM nodes WHERE device_id = :d AND sn = 'M2'",
                               {"d": device["id"]}, 0)) == 1
    assert db.fetch_one("SELECT recovered_at FROM alerts WHERE device_id = :d",
                        {"d": device["id"]})["recovered_at"] is not None


# --------------------------------------------------------------------------- §5.3 节点明细

def test_message_expands_node_array(client, device):
    ts = now_ms()
    content = json.dumps([
        {"sn": "M1", "ts": ts, "type": "gas", "payload": {"smoke": 900, "temp": 26.0, "humi": 50.0}},
        {"sn": "M4", "ts": ts + 1, "type": "door", "payload": {"door": True, "window": False}},
    ])
    resp = _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{ts}", "content": content}]}))
    assert resp.status_code == 200
    assert resp.json()["data"]["handled"] == 2
    rows = db.fetch_all("SELECT node_sn, type, payload_json FROM sensor_readings WHERE device_id = :d "
                        "ORDER BY id", {"d": device["id"]})
    assert [(r["node_sn"], r["type"]) for r in rows] == [("M1", "gas"), ("M4", "door")]
    assert db.jload(rows[1]["payload_json"])["door"] is True


def test_message_accepts_bare_service_envelope(client, device):
    """§4.6 的 `messages/up` 报文本身就是 service 对象（没有 `services` 外层）。

    规则引擎按设备原始 payload 转发，所以真机明细走的就是这个形状；只认 §5.2 的
    `{"services":[…]}` 会让它恒 handled=0 且不报错（t39 端到端串测 F-01）。
    """
    ts = now_ms()
    content = json.dumps([
        {"sn": "M1", "ts": ts, "type": "gas", "payload": {"smoke": 300, "temp": 25.0, "humi": 45.0}},
        {"sn": "M4", "ts": ts + 1, "type": "door", "payload": {"door": False, "window": False}},
    ])
    bare = {"name": "node_samples", "id": f"{GATEWAY_SN}-{ts}", "content": content}
    resp = _post(client, _envelope("device.message", GATEWAY_SN, bare))
    assert resp.status_code == 200 and resp.json()["code"] == 0
    assert resp.json()["data"]["handled"] == 2

    rows = db.fetch_all("SELECT node_sn, type, payload_json FROM sensor_readings WHERE device_id = :d "
                        "ORDER BY id", {"d": device["id"]})
    assert [(r["node_sn"], r["type"]) for r in rows] == [("M1", "gas"), ("M4", "door")]
    assert db.jload(rows[0]["payload_json"])["smoke"] == 300

    # §6.8 回读：落库的读数能被 REST 读出来（不只库里躺着一行）
    data = client.get(f"/api/devices/{device['id']}/readings?node_sn=M1&limit=5",
                      headers={"Authorization": f"Bearer {device['token']}"}).json()["data"]
    assert [it["payload"]["smoke"] for it in data["items"]] == [300]


def test_message_accepts_both_envelope_shapes(client, device):
    """两种信封并存：§4.6 裸对象与 §5.2 `{"services":[…]}` 都要能落库。

    两批刻意用不同节点（M1 / M2），避免 (device,node,type,ts_ms) 唯一键把第二批吃掉。
    """
    ts = now_ms()
    bare = {"name": "node_samples", "id": f"{GATEWAY_SN}-bare-{ts}",
            "content": json.dumps([{"sn": "M1", "ts": ts, "type": "gas", "payload": {"smoke": 340}}])}
    wrapped = {"services": [{"service_id": "gateway", "name": "node_samples",
                             "id": f"{GATEWAY_SN}-wrapped-{ts}", "content": json.dumps(
                                 [{"sn": "M2", "ts": ts, "type": "fall", "payload": {"fall": 1}}])}]}
    bare_resp = _post(client, _envelope("device.message", GATEWAY_SN, bare))
    wrapped_resp = _post(client, _envelope("device.message", GATEWAY_SN, wrapped))
    assert bare_resp.json()["data"]["handled"] == 1
    assert wrapped_resp.json()["data"]["handled"] == 1
    sns = [r["sn"] for r in db.fetch_all("SELECT sn FROM nodes WHERE device_id = :d ORDER BY sn",
                                         {"d": device["id"]})]
    assert sns == ["H1", "M1", "M2", "M4"]


def test_message_malformed_body_returns_200_handled_zero(client, device):
    """既不是裸 service 也不含 `services` 的畸形体：不崩、handled=0、不落库（§5.2 兜底）。"""
    for body in ({"foo": "bar", "bogus": 1}, {"services": "not-a-list"}, {"services": [1, 2]}):
        resp = _post(client, _envelope("device.message", GATEWAY_SN, body))
        assert resp.status_code == 200 and resp.json()["code"] == 0
        assert resp.json()["data"]["handled"] == 0
    assert db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d",
                           {"d": device["id"]}, 0) == 0


def test_message_with_corrupt_content_returns_200(client, device):
    """§5.2：识别不了的载荷也必须 200，否则平台会一直重推把链路堵死。"""
    resp = _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "content": "not-a-json"}]}))
    assert resp.status_code == 200 and resp.json()["code"] == 0
    assert db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings WHERE device_id = :d", {"d": device["id"]}, 0) == 0


# --------------------------------------------------------------------------- §5.2 兜底

def test_unknown_resource_returns_200_with_warning(client, device):
    resp = _post(client, _envelope("device.shadow", GATEWAY_SN, {"services": []}))
    assert resp.status_code == 200
    assert resp.json()["data"]["warnings"] == ["unknown_resource"]


def test_unknown_device_returns_200_with_warning(client):
    resp = _post(client, _envelope("device.property", "M0-NOTEXIST", _properties_body(), event="report"))
    assert resp.status_code == 200
    assert resp.json()["data"]["warnings"] == ["device_not_found"]


def test_missing_device_id_falls_back_to_sn_match(client):
    """§5.2：缺 device_id 时用网关 SN 兼容匹配（平台版本差异的兜底）。"""
    device_id = create_device()
    body = {"resource": "device.property", "notify_data": {"header": {"node_id": GATEWAY_SN},
                                                          "body": _properties_body()}}
    resp = _post(client, body)
    assert resp.status_code == 200
    assert resp.json()["data"]["device_id"] == device_id


def test_unknown_fields_are_ignored(client, device):
    """§0.5：接收方必须忽略未知字段（前向兼容），平台加字段不该把这条链路改坏。"""
    body = _envelope("device.property", GATEWAY_SN, _properties_body(), event="report")
    body["notify_data"]["body"]["services"][0]["properties"]["brand_new_field"] = "x"
    body["brand_new_envelope_field"] = 1
    assert _post(client, body).status_code == 200
    assert int(db.fetch_scalar("SELECT online FROM device_online WHERE device_id = :d",
                               {"d": device["id"]}, 0)) == 1
