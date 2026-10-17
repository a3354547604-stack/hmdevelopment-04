"""pytest 公共夹具。

要点：
1. **先设环境变量再导入 app**——`app.config.settings` 在导入时求值，晚设无效；
2. 每个测试前清空业务表并重置进程内运行态（幂等缓存、命令流水），保证测试互不污染；
3. 数据库连不上时给出可操作的报错，而不是一串 AttributeError。
"""
from __future__ import annotations

import os
import secrets

# 测试专用库：pytest 会 TRUNCATE 业务表，绝不能打到演示/生产库（见 README「验证」）。
# 必须在导入 app 之前改（app.config.settings 在导入时求值）。
_base_db = os.environ.get("DB_NAME", "eldercare")
os.environ["DB_NAME"] = _base_db if _base_db.endswith("_test") else f"{_base_db}_test"

os.environ.setdefault("SCHEDULER_ENABLED", "0")   # 测试里手动调 tick_once，不要后台线程抢行
os.environ.setdefault("IOTDA_DISPATCH_MODE", "mock")
os.environ.setdefault("PUSH_MODE", "mock")
os.environ.setdefault("LOG_LEVEL", "WARNING")

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from app import db, middleware, state  # noqa: E402
from app.main import app  # noqa: E402
from app.security import hash_password, login_limiter  # noqa: E402
from app.seed import BUSINESS_TABLES  # noqa: E402
from app.services.scheduler import recompute_activity  # noqa: E402
from app.timeutil import cst_day_bounds_ms, ms_to_cst_date, ms_to_dt, now_ms  # noqa: E402

PASSWORD = "test-pass-123"
GATEWAY_SN = "M0-3F1A2B"
BIND_CODE = "135790"


@pytest.fixture(scope="session", autouse=True)
def database():
    """会话级：确保库与表存在（DDL 见 db/schema.sql，幂等）。"""
    try:
        db.ensure_database()
        db.apply_schema()
    except Exception as exc:  # noqa: BLE001
        pytest.exit(f"无法连接 MySQL（先 `docker compose up -d db` 或设 DB_HOST/DB_PORT）：{exc}", returncode=3)
    yield


@pytest.fixture(autouse=True)
def clean_state(database):
    """每个测试：清空业务表 + 运行态。"""
    for table in BUSINESS_TABLES:
        db.execute(f"TRUNCATE TABLE `{table}`")
    state.reset()
    middleware.reset()
    login_limiter.reset()   # 限流器是进程内状态，不清会让后续测试莫名 429
    yield


@pytest.fixture
def client():
    with TestClient(app) as test_client:  # with 语句触发 lifespan（建表 / 不起调度器）
        yield test_client


# --------------------------------------------------------------------------- 数据构造

def create_user(username: str | None = None, password: str = PASSWORD, phone: str | None = None) -> dict:
    username = username or f"u{secrets.token_hex(4)}"
    user_id = db.insert(
        "INSERT INTO users (username, password_hash, phone) VALUES (:u, :p, :ph)",
        {"u": username, "p": hash_password(password), "ph": phone},
    )
    return {"id": user_id, "username": username, "password": password}


def create_device(sn: str = GATEWAY_SN, bind_code: str | None = BIND_CODE, expire_in_s: int = 600,
                  owner_id: int = 0, iotda_device_id: str | None = None) -> int:
    expire = ms_to_dt(now_ms() + expire_in_s * 1000) if bind_code else None
    return db.insert(
        "INSERT INTO devices (sn, name, bind_code, bind_code_expire_at, iotda_device_id, owner_id) "
        "VALUES (:sn, :name, :code, :expire, :plat, :owner)",
        {"sn": sn, "name": sn, "code": bind_code, "expire": expire,
         "plat": iotda_device_id or sn, "owner": owner_id},
    )


def create_membership(user_id: int, device_id: int, role: str = "owner") -> None:
    db.execute(
        "INSERT INTO user_devices (user_id, device_id, role) VALUES (:u, :d, :r) "
        "ON DUPLICATE KEY UPDATE role = VALUES(role), deleted_at = NULL",
        {"u": user_id, "d": device_id, "r": role},
    )


def create_nodes(device_id: int) -> None:
    for sn, node_type, location in (("H1", "hub", "客厅"), ("M1", "gas", "厨房"),
                                    ("M2", "fall", "卧室"), ("M4", "presence", "门厅")):
        db.execute(
            "INSERT INTO nodes (device_id, sn, type, location, online, last_seen, last_seen_ms) "
            "VALUES (:d, :sn, :t, :loc, 1, :seen, :seen_ms)",
            {"d": device_id, "sn": sn, "t": node_type, "loc": location,
             "seen": ms_to_dt(now_ms()), "seen_ms": now_ms()},
        )


def login(client: TestClient, username: str, password: str = PASSWORD) -> str:
    resp = client.post("/api/auth/login", json={"username": username, "password": password})
    assert resp.status_code == 200, resp.text
    return resp.json()["data"]["token"]


@pytest.fixture
def user(client) -> dict:
    account = create_user()
    account["token"] = login(client, account["username"])
    return account


@pytest.fixture
def device(client, user) -> dict:
    """已绑定给 user（owner）的网关设备 + 4 个节点。"""
    device_id = create_device(owner_id=user["id"])
    create_membership(user["id"], device_id, "owner")
    create_nodes(device_id)
    return {"id": device_id, "sn": GATEWAY_SN, "user": user, "token": user["token"]}


@pytest.fixture
def seed_readings(device):
    """给当前设备灌 presence 读数并归并当天报表（报表测试用），返回首个 ts。"""
    def _add(count: int = 2, hour: int = 9) -> int:
        day = ms_to_cst_date(now_ms())
        base, _ = cst_day_bounds_ms(day)
        ts = base + hour * 3600 * 1000
        if ts > now_ms():
            ts = now_ms() - 60 * 1000  # 落回今天，避免"未来的读数"被报表当成未开始的小时
        for i in range(count):
            db.execute(
                "INSERT IGNORE INTO sensor_readings (device_id, node_sn, type, ts, ts_ms, payload_json) "
                "VALUES (:d, 'M4', 'presence', :ts, :ts_ms, :p)",
                {"d": device["id"], "ts": ms_to_dt(ts + i * 1000), "ts_ms": ts + i * 1000,
                 "p": db.jdump({"presence": True, "last_motion_ts": ts + i * 1000})},
            )
        recompute_activity(day)
        return ts
    return _add
