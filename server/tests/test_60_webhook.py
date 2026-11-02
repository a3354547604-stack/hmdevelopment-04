"""契约 §5：IoTDA 规则引擎 webhook 的四类入库映射 + 三条硬约束。

这组测试刻意用「平台真实信封」（resource / event / notify_data.header.device_id / body.services[]），
因为 §5.2 要求服务器只能依赖这几个字段；用别的形状测等于没测。
"""
from __future__ import annotations

import json
from pathlib import Path

import pytest

from app import db
from app.config import settings
from app.services.telemetry import _NODE_SN_RE
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


def test_event_arrives_as_device_message_events_topic(client, device):
    """事件在 IoTDA 没有独立资源，是以 device.message（topic=…/sys/events/up）推来的。"""
    event = {"services": [{
        "service_id": "gateway", "event_type": "alert", "event_time": "20261017T120000Z",
        "paras": {"type": "fall", "level": 2, "node_sn": "M2", "ts": now_ms(), "event_id": f"{GATEWAY_SN}-fall-msg"}}]}
    body = {"services": [{"topic": f"$oc/devices/{GATEWAY_SN}/sys/events/up",
                          "content": json.dumps(event)}]}
    resp = _post(client, _envelope("device.message", GATEWAY_SN, body))
    assert resp.status_code == 200
    assert resp.json()["data"]["handled"] == 1
    row = db.fetch_one("SELECT type, level, node_sn, status FROM alerts WHERE device_id = :d", {"d": device["id"]})
    assert row["type"] == "fall" and int(row["level"]) == 2
    assert row["node_sn"] == "M2" and row["status"] == "pending"


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


# --------------------------------------------------------------------------- §0.7 节点 SN 形态（t179）

# v1.6 起节点 SN 两种形态并存：派生板 `<型号>-<4 位大写 hex>`（例 `M1-3FA2`）、
# efuse 不可用的回落板仍是短编号（`M1`）。服务端只有一处按形态判定（sos 的 `source`），
# 其余位置一律透传；所以这里既测那个正则，也测两条真会落库的链路。
def test_node_sn_regex_accepts_legacy_and_derived():
    for sn in ("M1", "M2", "M4", "H1", "M0", "M2-1C09", "M1-3FA2", "H1-0000"):
        assert _NODE_SN_RE.match(sn), f"应接受 {sn}"


def test_node_sn_regex_rejects_malformed():
    # M12：两位型号段（契约 §3.4：表项 `M1` 不命中 `M12`）；小写 hex；hex 位数不对；半截值
    for sn in ("M12", "M1-3fa2", "M1-3FA", "M1-3FA22", "", "M", "M1-", "-3FA2", "m1", "M1_3FA2"):
        assert not _NODE_SN_RE.match(sn), f"应拒绝 {sn}"


def test_message_persists_derived_node_sn(client, device):
    """派生 SN 的节点明细要原样落进 `sensor_readings.node_sn`（§7.4 列宽 16 够放 ≤12 字符）。"""
    ts = now_ms()
    content = json.dumps([{"sn": "M1-3FA2", "ts": ts, "type": "gas",
                           "payload": {"smoke": 640, "temp": 27.5, "humi": 48.0}}])
    resp = _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{ts}", "content": content}]}))
    assert resp.status_code == 200 and resp.json()["data"]["handled"] == 1
    row = db.fetch_one("SELECT node_sn FROM sensor_readings WHERE device_id = :d ORDER BY id DESC LIMIT 1",
                       {"d": device["id"]})
    assert row["node_sn"] == "M1-3FA2"
    # 型号段（`-` 之前）决定 nodes.type，派生 SN 不能被认成 unknown
    node = db.fetch_one("SELECT sn, type FROM nodes WHERE device_id = :d AND sn = :sn",
                        {"d": device["id"], "sn": "M1-3FA2"})
    assert node is not None and node["type"] == "gas"


def test_message_still_persists_legacy_node_sn(client, device):
    """回落板（efuse 不可用）上报的短编号必须照旧能进。"""
    ts = now_ms()
    content = json.dumps([{"sn": "M1", "ts": ts, "type": "gas",
                           "payload": {"smoke": 610, "temp": 26.0, "humi": 50.0}}])
    resp = _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{ts}", "content": content}]}))
    assert resp.status_code == 200 and resp.json()["data"]["handled"] == 1
    row = db.fetch_one("SELECT node_sn FROM sensor_readings WHERE device_id = :d ORDER BY id DESC LIMIT 1",
                       {"d": device["id"]})
    assert row["node_sn"] == "M1"


def test_sos_source_accepts_derived_sn(client, device):
    """`_NODE_SN_RE` 的唯一用处：sos 的 `source` 是不是节点号。派生 SN 要能当来源。"""
    ts = now_ms()
    resp = _post(client, _envelope("device.event", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "event_type": "sos", "event_time": "20261017T120000Z",
        "paras": {"source": "M4-3FA5", "ts": ts, "event_id": f"{GATEWAY_SN}-sos-derived-{ts}"}}]}, event="sos"))
    assert resp.status_code == 200
    row = db.fetch_one("SELECT type, node_sn FROM alerts WHERE device_id = :d", {"d": device["id"]})
    assert row["type"] == "sos" and row["node_sn"] == "M4-3FA5"


@pytest.mark.parametrize("source", ["M12", "M1-3fa2", "M1-3FA"])
def test_sos_source_rejects_malformed_sn(client, device, source):
    """不是节点号就不写 node_sn —— §0.4 不编造来源；告警本身仍要落（handled=1）。

    每个 source 独立成例（clean_state 每例清库）：sos 有 60 s 去重窗口，
    同一 device/type 的第二个 sos 会被去重吃掉，串行断言会假红。
    """
    ts = now_ms()
    resp = _post(client, _envelope("device.event", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "event_type": "sos", "event_time": "20261017T120000Z",
        "paras": {"source": source, "ts": ts, "event_id": f"{GATEWAY_SN}-sos-bad-{ts}"}}]}, event="sos"))
    assert resp.status_code == 200 and resp.json()["data"]["handled"] == 1
    row = db.fetch_one("SELECT type, node_sn FROM alerts WHERE device_id = :d", {"d": device["id"]})
    assert row["type"] == "sos" and row["node_sn"] is None


# --------------------------------------------------------------------------- t196 真机样本

FIXTURE_DIR = Path(__file__).parent / "fixtures"


def _load_real_envelope(device_sn: str) -> dict:
    """读 t196 在 123.206.99.28 上抓到的真实 webhook 报文（脱敏样本），把占位符换成本测试的设备。"""
    raw = (FIXTURE_DIR / "iotda_message_node_samples.json").read_text(encoding="utf-8")
    raw = (raw.replace("__DEVICE_ID__", device_sn)
              .replace("__PRODUCT_ID__", "test-product-id")
              .replace("__APP_ID__", "test-app-id"))
    return json.loads(raw)


def test_real_iotda_passthrough_message_envelope(client, device):
    """真机报文比 §5.2 多一层平台透传信封：`body.content` = {name, id, content}。

    2026-11-02 线上取证（t196）：10 分钟 111 条 `POST /api/iotda/webhook 200`，`sensor_readings`
    却零新增 —— 平台把 `messages/up` 原样塞进 `notify_data.body.content`，旧的
    `handle_message` 只看到 dict（既非 str 也非 list）⇒ items=[] ⇒ handled=0，而响应仍是 200。
    这条用例用**真实报文形状**锁死该回归：必须 handled≥1、且 M1 的 status 行真的落库。
    """
    body = _load_real_envelope(str(device["sn"]))
    resp = _post(client, body)
    assert resp.status_code == 200
    summary = resp.json()["data"]
    assert summary["resource"] == "device.message"
    assert summary["handled"] == 1, summary

    row = db.fetch_one(
        "SELECT node_sn, type, payload_json FROM sensor_readings "
        "WHERE device_id = :d AND node_sn = 'M1' AND type = 'status'",
        {"d": device["id"]},
    )
    assert row is not None, "真机明细没有落库（回到 t196 的零落库状态）"
    payload = db.jload(row["payload_json"])
    assert payload["uptime_s"] == 1998 and payload["power_src"] == "usb"

    node = db.fetch_one("SELECT online FROM nodes WHERE device_id = :d AND sn = 'M1'", {"d": device["id"]})
    assert node is not None and int(node["online"]) == 1


# --------------------------------------------------------------------------- §6.7 首页数据（t198）

def _node_samples(client, samples: list[dict]) -> dict:
    ts = now_ms()
    content = json.dumps(samples)
    resp = _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{ts}", "content": content}]}))
    assert resp.status_code == 200, resp.text
    return resp.json()["data"]


def _status(client, device) -> dict:
    resp = client.get(f"/api/devices/{device['id']}/status",
                      headers={"Authorization": f"Bearer {device['token']}"})
    assert resp.status_code == 200, resp.text
    return resp.json()["data"]


def test_status_nodes_have_name_and_latest(client, device):
    """首页房间卡的两根支柱：`nodes[].name` 与 `nodes[].latest`（t198）。

    现场症状是 App 只显示 `--` 与「正常」：`nodes[]` 只有 sn/type/online，房间名与最新读数
    都拿不到。这里锁死「每个节点都有非空 name」与「latest 按 type 给出最新一条 payload」。
    """
    ts = now_ms()
    _node_samples(client, [
        {"sn": "M1", "ts": ts, "type": "gas", "payload": {"smoke": 630, "co": 2094, "temp": 29.1, "humi": 34.0}},
        {"sn": "M2", "ts": ts + 1, "type": "fall", "payload": {"fall": 0, "presence": 1, "dist": 2.4}},
    ])
    data = _status(client, device)
    nodes = {n["sn"]: n for n in data["nodes"]}
    assert all(n.get("name") for n in data["nodes"]), data["nodes"]
    assert nodes["M1"]["name"] == "厨房" and nodes["M2"]["name"] == "卧室"

    gas = nodes["M1"]["latest"]["gas"]
    assert gas["payload"]["smoke"] == 630 and gas["payload"]["co"] == 2094
    assert gas["ts"] == nodes["M1"]["latest"]["gas"]["ts"] and isinstance(gas["ts"], int)
    assert nodes["M2"]["latest"]["fall"]["payload"]["dist"] == 2.4


def test_status_latest_is_newest_per_type(client, device):
    """`latest` 取的是**每种 type 的最新一条**，不是最早一条、也不是只有最后一批。"""
    base = now_ms()
    _node_samples(client, [{"sn": "M1", "ts": base, "type": "gas", "payload": {"smoke": 100}}])
    # 第二批：同一 type、更晚的 ts（node_samples 两批不同 id，避免幂等吃掉）
    resp = _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-later",
        "content": json.dumps([{"sn": "M1", "ts": base + 5000, "type": "gas", "payload": {"smoke": 777}}])}]}))
    assert resp.status_code == 200
    latest = {n["sn"]: n for n in _status(client, device)["nodes"]}["M1"]["latest"]
    assert latest["gas"]["payload"]["smoke"] == 777, latest


def test_message_reattributes_sn_type_mismatch(client, device):
    """网关串台时服务端按 `nodes.type` 纠正：现场 DB 里出现过 `M2 gas`、`M1 fall`（t198）。

    gas 只在 M1、fall 只在 M2（`nodes` 表里各自唯一），所以这种串台能无歧义地改挂；
    改挂后既不留 `M2 gas` 脏行，也不影响 `latest` 的取值。
    """
    ts = now_ms()
    handled = _node_samples(client, [
        {"sn": "M2", "ts": ts, "type": "gas", "payload": {"smoke": 640, "co": 2000}},
        {"sn": "M1", "ts": ts + 1, "type": "fall", "payload": {"fall": 1, "dist": 2.1}},
    ])
    assert handled["handled"] == 2

    combos = {(r["node_sn"], r["type"]) for r in db.fetch_all(
        "SELECT node_sn, type FROM sensor_readings WHERE device_id = :d", {"d": device["id"]})}
    assert ("M2", "gas") not in combos and ("M1", "fall") not in combos, combos
    assert ("M1", "gas") in combos and ("M2", "fall") in combos, combos

    nodes = {n["sn"]: n for n in _status(client, device)["nodes"]}
    assert nodes["M1"]["latest"]["gas"]["payload"]["smoke"] == 640
    assert nodes["M2"]["latest"]["fall"]["payload"]["fall"] == 1


def test_status_frames_downsampled_per_minute(client, device):
    """同一节点同一分钟的 status 只落 1 行（t198）：真机 status 占了 50%+ 的行。"""
    ts = now_ms()
    bucket = ts - ts % 60_000
    _node_samples(client, [
        {"sn": "M1", "ts": bucket, "type": "status", "payload": {"uptime_s": 10}},
        {"sn": "M1", "ts": bucket + 1000, "type": "status", "payload": {"uptime_s": 11}},
        {"sn": "M1", "ts": bucket + 2000, "type": "status", "payload": {"uptime_s": 12}},
    ])
    rows = db.fetch_all("SELECT ts_ms, payload_json FROM sensor_readings "
                        "WHERE device_id = :d AND node_sn = 'M1' AND type = 'status'", {"d": device["id"]})
    assert len(rows) == 1, [(r["ts_ms"], db.jload(r["payload_json"])) for r in rows]
    assert int(rows[0]["ts_ms"]) == bucket


def test_node_samples_upserts_missing_nodes(client, user):
    """`node_samples` 到达就 upsert `nodes`（t198）：没有这步，App 的在线状态永远缺数据。"""
    device_id = create_device(owner_id=user["id"])
    from conftest import create_membership
    create_membership(user["id"], device_id, "owner")
    assert db.fetch_one("SELECT id FROM nodes WHERE device_id = :d", {"d": device_id}) is None

    ts = now_ms()
    _post(client, _envelope("device.message", GATEWAY_SN, {"services": [{
        "service_id": "gateway", "name": "node_samples", "id": f"{GATEWAY_SN}-{ts}",
        "content": json.dumps([{"sn": "M1", "ts": ts, "type": "gas", "payload": {"smoke": 320}}])}]}))

    node = db.fetch_one("SELECT sn, type, online FROM nodes WHERE device_id = :d AND sn = 'M1'",
                        {"d": device_id})
    assert node is not None and node["type"] == "gas" and int(node["online"]) == 1
