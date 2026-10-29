"""§6.20 健康检查：免鉴权 + 关键字段（CAL-09 的 clock_source 必须如实）。"""
from __future__ import annotations


def test_health_without_token(client):
    resp = client.get("/api/health")
    assert resp.status_code == 200
    body = resp.json()
    assert body["code"] == 0, body
    data = body["data"]
    assert data["ok"] is True
    assert data["db"] == "ok"                 # §6.20 字面值（§9.1：不可用时 "down" + 503）
    assert data["version"]
    # CAL-09：节点校时完成前，任何时刻都必须回报 server，不能假装成设备时钟
    assert data["clock_source"] == "server"
    assert isinstance(data["server_ts"], int) and data["server_ts"] > 1_600_000_000_000
    # §0.4：从未发生的事件省略键，而不是塞 null
    for key in ("iotda_token_expire_at", "webhook_last_at"):
        assert key not in data


def test_time_for_gateway_without_token(client):
    """契约 v1.2 §6.21：网关联网后先取 UTC 再连 IoTDA，所以这个口必须免鉴权。"""
    import re
    import time

    before = int(time.time())
    resp = client.get("/api/time")
    assert resp.status_code == 200
    body = resp.json()
    assert body["code"] == 0
    data = body["data"]
    assert set(data) == {"utc_epoch", "epoch", "utc_epoch_ms", "iso8601", "tz", "server_ts"}
    assert data["tz"] == "UTC"
    # 秒/毫秒两个字段必须自洽，且与真实时间同秒（网关会直接拿去算 HMACSHA256 时间戳）
    assert data["utc_epoch_ms"] // 1000 == data["utc_epoch"]
    assert before <= data["utc_epoch"] <= before + 5
    assert re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", data["iso8601"])
    assert data["iso8601"].endswith("Z")


def test_time_is_not_cacheable(client):
    """§6.21 定稿：响应头必须 `Cache-Control: no-store`。

    缓存这个口 = 网关拿到的是**过期时间**，HMACSHA256 时间戳与 event_time 一起偏；共享缓存对 200 响应
    默认可做启发式缓存，所以不能用「GET 一般不缓存」当理由，必须显式声明。
    """
    resp = client.get("/api/time")
    assert resp.status_code == 200
    assert resp.headers.get("cache-control", "").lower() == "no-store"
    # 头不能代替主体：字段与 §6.21 表一致（避免只加头把 code/data 改坏）
    assert resp.json()["code"] == 0 and resp.json()["data"]["tz"] == "UTC"


def test_root_hint(client):
    resp = client.get("/")
    assert resp.status_code == 200
    assert resp.json()["data"]["service"] == "hmprogram-server"


def test_unknown_route_returns_envelope(client):
    """404 也要是 {code,message,data,ts}，否则 App 端的统一解包会炸。"""
    resp = client.get("/api/nope")
    assert resp.status_code == 404
    body = resp.json()
    assert set(body) >= {"code", "message", "data", "ts"}
    assert body["code"] != 0
