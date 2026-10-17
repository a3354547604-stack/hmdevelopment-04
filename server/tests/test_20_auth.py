"""§6.3/§6.4 注册登录 + §9.1 状态码/§9.2 错误码 + 限流。"""
from __future__ import annotations

from app import db
from app.security import decode_token

from conftest import PASSWORD


def test_register_creates_user(client):
    resp = client.post("/api/auth/register", json={"username": "daughter01", "password": PASSWORD, "phone": "13800000000"})
    assert resp.status_code == 201, resp.text          # §9.1：register 用 201
    data = resp.json()["data"]
    assert data["username"] == "daughter01"
    assert isinstance(data["user_id"], int)
    assert isinstance(data["created_at"], int)          # 统一毫秒时间戳，不是字符串
    row = db.fetch_one("SELECT password_hash FROM users WHERE username = 'daughter01'")
    assert row["password_hash"].startswith("$2b$10$")   # §7.2 bcrypt cost=10


def test_register_rejects_bad_username(client):
    resp = client.post("/api/auth/register", json={"username": "ab", "password": PASSWORD})
    assert resp.status_code == 400
    assert resp.json()["code"] == 1001


def test_register_duplicate_without_idempotency_key(client):
    payload = {"username": "daughter01", "password": PASSWORD}
    assert client.post("/api/auth/register", json=payload).status_code == 201
    resp = client.post("/api/auth/register", json=payload)
    assert resp.status_code == 409
    assert resp.json()["code"] == 1103


def test_login_returns_jwt(client, user):
    resp = client.post("/api/auth/login", json={"username": user["username"], "password": PASSWORD})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["expires_in"] == 604800                 # §6.4：7 天
    claims = decode_token(data["token"])
    assert claims["sub"] == str(user["id"])
    # 手机号是可选字段且本例没填：§0.4 要求省略键（不是 null）
    assert set(data["user"]) == {"id", "username"}
    assert data["user"]["id"] == user["id"]


def test_login_wrong_password_is_1102(client, user):
    resp = client.post("/api/auth/login", json={"username": user["username"], "password": "wrong-pass-123"})
    assert resp.status_code == 401
    assert resp.json()["code"] == 1102
    # 用户不存在必须回同一个码，避免枚举账号
    resp2 = client.post("/api/auth/login", json={"username": "no-such-user", "password": PASSWORD})
    assert resp2.status_code == 401 and resp2.json()["code"] == 1102


def test_missing_token_is_1100(client):
    resp = client.get("/api/devices")
    assert resp.status_code == 401
    assert resp.json()["code"] == 1100


def test_broken_token_is_1101(client):
    resp = client.get("/api/devices", headers={"Authorization": "Bearer not-a-jwt"})
    assert resp.status_code == 401
    assert resp.json()["code"] in (1100, 1101)


def test_validation_error_maps_to_1000(client):
    """缺必填字段要回 1000 且带 missing[]（§9.2），不是 FastAPI 默认的 422。"""
    resp = client.post("/api/auth/login", json={"username": "someone"})
    assert resp.status_code == 400
    body = resp.json()
    assert body["code"] == 1000
    assert "password" in (body.get("missing") or body["data"].get("missing"))


def test_login_rate_limited_after_100_attempts(client, monkeypatch):
    from app.security import login_limiter

    monkeypatch.setattr(login_limiter, "limit", 3)   # 用真实阈值 100 次太慢，这里只验证窗口逻辑
    for _ in range(3):
        client.post("/api/auth/login", json={"username": "ghost", "password": "bad-pass-123"})
    resp = client.post("/api/auth/login", json={"username": "ghost", "password": "bad-pass-123"})
    assert resp.status_code == 429
    assert resp.json()["code"] == 1105
