"""§6.5–§6.12 设备接口：列表/绑定/状态/读数/阈值/下发/演示。"""
from __future__ import annotations

from app import db
from app.settings_keys import DEFAULTS

from conftest import BIND_CODE, GATEWAY_SN, create_device, create_membership, create_user, login


def _auth(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


# --------------------------------------------------------------------------- §6.5

def test_list_devices_shape(client, device):
    resp = client.get("/api/devices", headers=_auth(device["token"]))
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["total"] == 1
    item = data["items"][0]
    assert item["sn"] == GATEWAY_SN
    assert item["role"] == "owner"
    assert item["online"] is False          # 还没有心跳上报
    assert item["unread_alerts"] == 0
    # §0.4：last_heartbeat 未上报时**省略键**而不是给 null，所以只断言必需键 + 白名单
    assert {"id", "sn", "name", "role", "online", "alarm_state", "unread_alerts"} <= set(item)
    assert set(item) <= {"id", "sn", "name", "role", "online", "last_heartbeat", "alarm_state", "unread_alerts"}


def test_other_user_sees_nothing(client):
    stranger = create_user()
    token = login(client, stranger["username"])
    create_device()
    assert client.get("/api/devices", headers=_auth(token)).json()["data"]["total"] == 0


# --------------------------------------------------------------------------- §6.6 绑定

def test_bind_success_and_code_consumed(client, user):
    device_id = create_device()
    resp = client.post(
        "/api/devices/bind",
        headers=_auth(user["token"]),
        json={"sn": GATEWAY_SN, "bind_code": BIND_CODE, "name": "奶奶家网关"},
    )
    assert resp.status_code == 201, resp.text        # §9.1：bind 用 201
    data = resp.json()["data"]
    assert data == {"device_id": device_id, "sn": GATEWAY_SN, "role": "owner"}
    row = db.fetch_one("SELECT bind_code, owner_id, name FROM devices WHERE id = :d", {"d": device_id})
    assert row["bind_code"] is None                   # 绑定码一次性
    assert int(row["owner_id"]) == user["id"]
    assert row["name"] == "奶奶家网关"


def test_bind_unknown_sn_1200(client, user):
    resp = client.post("/api/devices/bind", headers=_auth(user["token"]),
                       json={"sn": "M0-FFFFFF", "bind_code": BIND_CODE})
    assert resp.status_code == 404 and resp.json()["code"] == 1200


def test_bind_wrong_code_1201(client, user):
    create_device()
    resp = client.post("/api/devices/bind", headers=_auth(user["token"]),
                       json={"sn": GATEWAY_SN, "bind_code": "000001"})
    assert resp.status_code == 400 and resp.json()["code"] == 1201


def test_bind_expired_code_1203(client, user):
    create_device(expire_in_s=-60)                   # 已过期 1 分钟
    resp = client.post("/api/devices/bind", headers=_auth(user["token"]),
                       json={"sn": GATEWAY_SN, "bind_code": BIND_CODE})
    assert resp.status_code == 410 and resp.json()["code"] == 1203


def test_bind_already_owned_by_someone_else_1202(client, user):
    other = create_user()
    device_id = create_device(bind_code=None, owner_id=other["id"])
    create_membership(other["id"], device_id, "owner")
    resp = client.post("/api/devices/bind", headers=_auth(user["token"]),
                       json={"sn": GATEWAY_SN, "bind_code": BIND_CODE})
    assert resp.status_code == 409 and resp.json()["code"] == 1202


def test_bind_same_device_twice_is_idempotent(client, user):
    create_device()
    payload = {"sn": GATEWAY_SN, "bind_code": BIND_CODE}
    assert client.post("/api/devices/bind", headers=_auth(user["token"]), json=payload).status_code == 201
    second = client.post("/api/devices/bind", headers=_auth(user["token"]), json=payload)
    assert second.status_code == 201 and second.json()["data"]["role"] == "owner"


def test_family_member_binds_with_fresh_code_as_family(client, user):
    """家人二次绑定：设备已有 owner，码有效时进来的是 family 角色（§7.4）。"""
    owner = create_user()
    device_id = create_device(owner_id=owner["id"])
    create_membership(owner["id"], device_id, "owner")
    family = create_user()
    token = login(client, family["username"])
    resp = client.post("/api/devices/bind", headers=_auth(token), json={"sn": GATEWAY_SN, "bind_code": BIND_CODE})
    assert resp.status_code == 201 and resp.json()["data"]["role"] == "family"


# --------------------------------------------------------------------------- §6.7 状态

def test_device_status_shape(client, device):
    resp = client.get(f"/api/devices/{device['id']}/status", headers=_auth(device["token"]))
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert {"device_id", "online", "alarm_state", "nodes", "pending_alerts", "server_ts"} <= set(data)
    assert set(data) <= {"device_id", "online", "last_heartbeat", "alarm_state", "properties",
                         "nodes", "pending_alerts", "server_ts"}
    assert [n["sn"] for n in data["nodes"]] == ["H1", "M1", "M2", "M4"]
    assert all({"sn", "type", "location", "online", "last_seen"} <= set(n) for n in data["nodes"])
    assert all(set(n) <= {"sn", "type", "location", "online", "last_seen", "rssi", "battery"} for n in data["nodes"])
    assert data["properties"] == {}          # 还没收到属性上报 → 空对象而不是 null


def test_status_forbidden_for_stranger_1104(client, device):
    stranger = create_user()
    token = login(client, stranger["username"])
    resp = client.get(f"/api/devices/{device['id']}/status", headers=_auth(token))
    assert resp.status_code == 403 and resp.json()["code"] == 1104


def test_status_unknown_device_1200(client, user):
    resp = client.get("/api/devices/999999/status", headers=_auth(user["token"]))
    assert resp.status_code == 404 and resp.json()["code"] == 1200


# --------------------------------------------------------------------------- §6.8 读数

def test_readings_desc_and_filtered(client, device, seed_readings):
    seed_readings(count=2, hour=9)
    resp = client.get(f"/api/devices/{device['id']}/readings", headers=_auth(device["token"]),
                      params={"type": "presence", "limit": 50})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["total"] == 2
    assert len(data["items"]) == 2
    assert data["items"][0]["ts"] >= data["items"][1]["ts"]      # §6.8：按 ts 倒序
    assert data["items"][0]["payload"]["presence"] is True
    assert set(data["items"][0]) == {"id", "node_sn", "type", "ts", "payload"}


def test_readings_invalid_range_1004(client, device):
    resp = client.get(f"/api/devices/{device['id']}/readings", headers=_auth(device["token"]),
                      params={"from": 2_000_000_000_000, "to": 1_000_000_000_000})
    assert resp.status_code == 400 and resp.json()["code"] == 1004


# --------------------------------------------------------------------------- §6.9/§6.10 设置

def test_settings_defaults_match_contract(client, device):
    resp = client.get(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]))
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert set(data) == {"source", "updated_at", "items"}
    assert data["source"] == "local"
    assert data["items"]["no_motion_min"] == 30
    assert data["items"]["gas_t1_mv"] == 1500
    assert data["items"]["alert_repush_s"] == [0, 60, 300]
    assert len(data["items"]) == len(DEFAULTS) == 21
    # 契约 §6.9 注（PM D-01）：gas_l1_to_l2_s 是 §8.2 的算法参数，不进 items
    assert "gas_l1_to_l2_s" not in data["items"]


def test_put_settings_persists(client, device):
    resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                      json={"items": {"no_motion_min": 45, "gas_t1_mv": 1600}})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["items"]["no_motion_min"] == 45
    assert data["items"]["gas_t1_mv"] == 1600
    assert data["items"]["gas_t2_mv"] == 2500            # 未提交的键保持默认
    assert isinstance(data["updated_at"], int) and data["updated_at"] > 0
    again = client.get(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]))
    assert again.json()["data"]["items"]["no_motion_min"] == 45


def test_put_settings_out_of_range_1002(client, device):
    resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                      json={"items": {"no_motion_min": 1000}})
    assert resp.status_code == 400 and resp.json()["code"] == 1002


def test_put_settings_unknown_key_1003(client, device):
    resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                      json={"items": {"not_a_key": 1}})
    assert resp.status_code == 400 and resp.json()["code"] == 1003


def test_put_settings_algorithm_param_1003_and_not_persisted(client, device):
    """§6.9 注（PM D-01）：gas_l1_to_l2_s 只属 §8.2，既不在 items 里，也不接受 PUT 下发。"""
    before = client.get(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]))
    assert before.status_code == 200 and "gas_l1_to_l2_s" not in before.json()["data"]["items"]

    resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                      json={"items": {"gas_l1_to_l2_s": 30}})
    assert resp.status_code == 400 and resp.json()["code"] == 1003, resp.text

    after = client.get(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]))
    items = after.json()["data"]["items"]
    assert "gas_l1_to_l2_s" not in items                    # 没落库
    assert items["gas_recover_mv"] == 200                   # 相邻键未受影响

    # §6.11 下发面同样不认这个键（与 §6.10 一致）
    conf = client.post(f"/api/devices/{device['id']}/config", headers=_auth(device["token"]),
                       json={"keys": ["gas_l1_to_l2_s"]})
    assert conf.status_code == 400 and conf.json()["code"] == 1003, conf.text


def test_put_settings_co_t1_mv_accepted_but_not_in_items(client, device):
    """t171：co_t1_mv（M1 的 MQ-7 一级阈值）属于「可下发的算法参数」——

    接受 PUT（不再按 1003 未知键拒），但**不进 §6.9 的 `items`**：那是 App 消费的 21 键固定形状，
    混进去会同时撑破 App 的字段核对与契约；值单独落在 `settings_json.algorithm`。
    """
    resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                      json={"items": {"co_t1_mv": 900}})
    assert resp.status_code == 200, resp.text
    items = resp.json()["data"]["items"]
    assert len(items) == len(DEFAULTS) == 21
    assert "co_t1_mv" not in items
    assert items["gas_t1_mv"] == 1500                     # 相邻键未受影响
    row = db.fetch_one("SELECT settings_json FROM devices WHERE id = :d", {"d": device["id"]})
    assert db.jload(row["settings_json"])["algorithm"]["co_t1_mv"] == 900


def test_put_settings_co_t1_mv_boundaries_inclusive(client, device):
    """范围 200–3300 含端点（固件侧按同一区间判一级 CO）。"""
    for good in (200, 3300):
        resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                          json={"items": {"co_t1_mv": good}})
        assert resp.status_code == 200, (good, resp.text)


def test_put_settings_co_t1_mv_out_of_range_1002(client, device):
    """越界回 1002，且 `field`/`allowed` 指回 co_t1_mv 的合法区间。"""
    for bad in (199, 3301):
        resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                          json={"items": {"co_t1_mv": bad}})
        assert resp.status_code == 400 and resp.json()["code"] == 1002, (bad, resp.text)
        assert resp.json()["data"]["field"] == "co_t1_mv"
        assert resp.json()["data"]["allowed"] == [200, 3300]


def test_config_dispatches_co_t1_mv(client, device):
    """§6.10/§6.11：co_t1_mv 是「认得出的键」——进 accepted（走 set_threshold），不是 1003。"""
    resp = client.post(f"/api/devices/{device['id']}/config", headers=_auth(device["token"]),
                       json={"keys": ["co_t1_mv"]})
    assert resp.status_code == 200, resp.text
    data = resp.json()["data"]
    assert data["accepted"] == ["co_t1_mv"]
    assert data["rejected"] == []
    assert data["request_id"]                            # 已投递 IoTDA（mock 模式也返回 request_id）

    # 下发用的是设备侧 set_threshold 命令（与 §3.4 的 0x11 set 帧一一对应）
    from app.services import iotda
    key = "co_t1_mv"
    assert key not in iotda._OFFICIAL_COMMANDS


def test_put_settings_requires_owner_1104(client, device):
    family = create_user()
    token = login(client, family["username"])
    create_membership(family["id"], device["id"], "family")
    assert client.get(f"/api/devices/{device['id']}/settings", headers=_auth(token)).status_code == 200
    resp = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(token),
                      json={"items": {"no_motion_min": 45}})
    assert resp.status_code == 403 and resp.json()["code"] == 1104


# --------------------------------------------------------------------------- §6.11 下发

def test_config_dispatch_shape_and_rejections(client, device):
    resp = client.post(f"/api/devices/{device['id']}/config", headers=_auth(device["token"]),
                       json={"keys": ["report_interval_s", "gateway_offline_timeout_s"]})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["accepted"] == ["report_interval_s"]
    assert data["rejected"] == ["gateway_offline_timeout_s"]   # gateway_offline_timeout_s 是服务器侧参数，不下发
    # 被拒原因放可选的 reasons（键名 → 原因），只覆盖被拒项
    assert set(data["reasons"]) == {"gateway_offline_timeout_s"}
    assert all(isinstance(v, str) and v for v in data["reasons"].values())
    assert len(data["request_id"]) <= 64
    assert data["timeout_s"] == 10
    assert isinstance(data["dispatched_at"], int)
    # §6.11：绝不返回「已成功」；accepted 只代表已投递到 IoTDA（mock 模式即记录流水）
    blob = resp.text
    for word in ("已成功", "success", "succeeded", "OK\n"):
        assert word not in blob
    from app.state import last_command
    assert last_command()["command"]          # 下发流水已记录，便于演示时排障


def test_config_without_keys_dispatches_all_dispatchable(client, device):
    resp = client.post(f"/api/devices/{device['id']}/config", headers=_auth(device["token"]), json={})
    data = resp.json()["data"]
    from app.settings_keys import DEFAULTS, dispatch_targets

    # 逐键对账：可下发/不可下发的划分由 settings_keys 单点决定，接口只做汇总（数组元素=键名）
    accepted, rejected = dispatch_targets(dict(DEFAULTS))
    assert data["accepted"] == sorted(accepted)
    assert data["rejected"] == sorted(rejected)
    assert set(data["reasons"]) == set(rejected)                # 只有被拒项才有原因，且一条不少
    assert len(data["accepted"]) + len(data["rejected"]) == len(DEFAULTS)


# --------------------------------------------------------------------------- §6.12 演示

def test_demo_fall_creates_alert_and_pushes(client, device):
    resp = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(device["token"]), json={"event": "fall"})
    assert resp.status_code == 200, resp.text
    data = resp.json()["data"]
    assert data["event"] == "fall"
    assert data["accepted"] == ["demo_trigger"]      # §6.12 与 §6.11 同口径：字符串数组
    assert data["rejected"] == []
    assert data["alert"]["type"] == "fall" and data["alert"]["level"] == 2
    assert data["alert"]["node_sn"] == "M2"
    assert data["alert"]["push_count"] == 1         # 演示要立刻推一次（不等调度器）
    assert data["alert"]["status"] == "pending"

    # 真实链路副作用：读数 + 节点明细 + 属性快照都进了库
    readings = db.fetch_all("SELECT type FROM sensor_readings WHERE device_id = :d", {"d": device["id"]})
    kinds = {row["type"] for row in readings}
    assert "fall" in kinds and "gateway" in kinds
    online = db.fetch_one("SELECT online FROM device_online WHERE device_id = :d", {"d": device["id"]})
    assert online and bool(online["online"]) is True


def test_demo_invalid_event_1206(client, device):
    resp = client.post(f"/api/devices/{device['id']}/demo", headers=_auth(device["token"]), json={"event": "explode"})
    assert resp.status_code == 400 and resp.json()["code"] == 1206
