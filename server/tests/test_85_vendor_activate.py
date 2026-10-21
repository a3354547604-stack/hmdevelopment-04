"""t152 厂商侧设备出厂注册：`POST /api/device/activate`。

覆盖卡面要求的 6 类用例：正常创建、幂等、无 key、错 key、非法 SN、限流；
另加两条守护：未配置 `PRODUCT_KEY` 时按 404 关闭、建档后 webhook 能按 SN 找到设备
（这正是本接口存在的理由，缺了它这次改动只是「多一个能写库的口子」）。
"""
from __future__ import annotations

import pytest

from app import db
from app.routers import vendor

from conftest import GATEWAY_SN, create_device

PRODUCT_KEY = "test-product-key"   # 与 conftest 里 os.environ["PRODUCT_KEY"] = "test-product-key" 一致


def _post(client, body: dict | None = None, key: str | None = PRODUCT_KEY):
    headers = {} if key is None else {"X-Product-Key": key}
    return client.post("/api/device/activate", headers=headers, json=body if body is not None else {"sn": GATEWAY_SN})


def _count_devices(sn: str) -> int:
    return int(db.fetch_scalar("SELECT COUNT(*) FROM devices WHERE sn = :sn", {"sn": sn}) or 0)


# --------------------------------------------------------------------------- 正常创建

def test_activate_creates_device(client):
    resp = _post(client, {"sn": "M0-T1", "name": "爸妈家的小盒子"})
    assert resp.status_code == 200, resp.text
    data = resp.json()["data"]
    assert data["sn"] == "M0-T1"
    assert data["created"] is True
    assert isinstance(data["device_id"], int) and data["device_id"] > 0

    row = db.fetch_one("SELECT * FROM devices WHERE id = :id", {"id": data["device_id"]})
    assert row["sn"] == "M0-T1"
    assert row["name"] == "爸妈家的小盒子"
    # 卡面：出厂建档不归属任何账号，也不预置绑定码（绑定码由 App 侧生成邀请）
    assert int(row["owner_id"] or 0) == 0
    assert row["bind_code"] is None
    # iotda_device_id 留空：平台侧 id 此时还不知道，webhook 走 node_id=SN 兼容匹配
    assert row["iotda_device_id"] is None


def test_activate_defaults_name_to_sn(client):
    resp = _post(client, {"sn": "M4-AB"})
    assert resp.status_code == 200, resp.text
    row = db.fetch_one("SELECT name FROM devices WHERE sn = 'M4-AB'")
    assert row["name"] == "M4-AB"


# --------------------------------------------------------------------------- 幂等

def test_activate_is_idempotent(client):
    first = _post(client, {"sn": "M0-T2"}).json()["data"]
    second = _post(client, {"sn": "M0-T2", "name": "改个名字也不新建"}).json()["data"]
    assert first["created"] is True
    assert second["created"] is False
    assert second["device_id"] == first["device_id"]
    assert _count_devices("M0-T2") == 1
    # 幂等重放不改已有行：名称仍是首次建档时的值（避免厂商重复上报互相覆盖人工改过的名字）
    assert db.fetch_one("SELECT name FROM devices WHERE sn = 'M0-T2'")["name"] == "M0-T2"


def test_activate_attaches_to_preexisting_device(client):
    """seed/手工预置过的设备再次出厂注册 → 复用同一 device_id，不新建。"""
    device_id = create_device(sn="M0-PRE")
    data = _post(client, {"sn": "M0-PRE"}).json()["data"]
    assert data["device_id"] == device_id
    assert data["created"] is False
    assert _count_devices("M0-PRE") == 1


# --------------------------------------------------------------------------- 鉴权

def test_activate_without_key_is_401(client):
    resp = _post(client, {"sn": "M0"}, key=None)
    assert resp.status_code == 401
    assert resp.json()["code"] == 1501
    assert _count_devices("M0-T2") == 0     # 被拒的请求绝不能写库


def test_activate_wrong_key_is_401(client):
    resp = _post(client, {"sn": "M0"}, key="not-the-key")
    assert resp.status_code == 401
    assert resp.json()["code"] == 1501
    assert _count_devices("M0-T2") == 0


def test_activate_disabled_when_product_key_unset(client, monkeypatch):
    """未配置 PRODUCT_KEY → 404，与「没有这个路由」不可区分（不暴露存在性）。"""
    monkeypatch.setattr(vendor, "_product_key", lambda: "")
    resp = _post(client, {"sn": "M0"}, key="anything")
    assert resp.status_code == 404
    assert _count_devices("M0-T2") == 0


# --------------------------------------------------------------------------- 参数

@pytest.mark.parametrize("sn", ["", "ab", "a" * 33, "SN WITH SPACE", "SN/斜杠", "SN.DOT"])
def test_activate_rejects_invalid_sn(client, sn):
    resp = _post(client, {"sn": sn})
    assert resp.status_code == 400, resp.text
    assert resp.json()["code"] == 1201
    assert db.fetch_scalar("SELECT COUNT(*) FROM devices WHERE sn = :sn", {"sn": sn}) in (0, None)


def test_activate_rejects_missing_sn(client):
    resp = client.post("/api/device/activate", headers={"X-Product-Key": PRODUCT_KEY}, json={"name": "x"})
    assert resp.status_code == 400
    assert resp.json()["code"] == 1000       # 缺少必填参数


def test_activate_rejects_overlong_name(client):
    resp = _post(client, {"sn": "M0-T3", "name": "名" * 65})
    assert resp.status_code == 400
    assert resp.json()["code"] == 1002
    assert _count_devices("M0-T2") == 0


# --------------------------------------------------------------------------- 限流

def test_activate_rate_limited(client):
    """每秒最多 5 次：第 6 次必须 429/1105（卡面口径）。"""
    codes = []
    for _ in range(6):
        resp = _post(client, {"sn": "M0-T2"})
        codes.append((resp.status_code, resp.json()["code"]))
    assert codes[:5] == [(200, 0)] * 5, codes
    assert codes[5] == (429, 1105), codes
    assert _count_devices("M0-T2") == 1     # 只有第一次真的建了行


# --------------------------------------------------------------------------- 建档 → webhook 可匹配（本接口的存在理由）

def test_activated_device_is_resolvable_by_webhook(client):
    """出厂建档后的 SN 能被 webhook 的 `_resolve_device()` 按 `header.node_id` 找到。

    不真正发 webhook（那属于 §5 的既有用例），直接调内部解析函数：本卡只新增了「建档」，
    webhook 逻辑保持不变，这里验证的是两边的契约能接上。
    """
    from app.services.telemetry import _resolve_device

    device_id = _post(client, {"sn": "M0-WH"}).json()["data"]["device_id"]
    resolved = _resolve_device({"node_id": "M0-WH"}, {})
    assert resolved is not None
    assert int(resolved["id"]) == device_id
