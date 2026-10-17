"""推送 token 注册（契约 §6.19）、健康检查（§6.20）与取时接口（契约 v1.2 预留）。

`/api/health` 与 `/api/time` 都免鉴权：前者是 App/网关/验收脚本在未登录时判断服务与数据库是否可用的依据，
后者是网关无 RTC、联网后连 IoTDA 前必须拿到 UTC 的唯一步骤（先取时再握手，否则 TLS/鉴权时间戳会偏）。
两者都只暴露「不敏感」的观测值：版本、数据库连通性、webhook 最近时刻、时钟来源（CAL-09）、当前 UTC。
"""
from __future__ import annotations

import logging
from datetime import datetime, timezone

from fastapi import APIRouter
from fastapi.responses import JSONResponse
from pydantic import BaseModel

from .. import db
from ..config import settings
from ..db import ping
from ..deps import CurrentUser
from ..errors import ERROR_TABLE, ApiError, ok
from ..state import RUNTIME
from ..timeutil import now_ms

log = logging.getLogger("app.push")
router = APIRouter(prefix="/api", tags=["push", "health", "time"])

PLATFORMS = ("harmony", "android")


class PushRegisterIn(BaseModel):
    push_token: str
    platform: str = "harmony"


@router.post("/push/register")
def register_push_token(payload: PushRegisterIn, user: CurrentUser) -> dict:
    token = (payload.push_token or "").strip()
    platform = (payload.platform or "harmony").strip().lower()
    if not (8 <= len(token) <= 255):
        raise ApiError(1001, field="push_token", expected="8-255 字符")
    if platform not in PLATFORMS:
        raise ApiError(1002, field="platform", allowed=list(PLATFORMS))

    existing = db.fetch_one(
        "SELECT id FROM push_tokens WHERE user_id = :u AND token = :t", {"u": user["id"], "t": token}
    )
    if existing:
        # §6.19：同 token 幂等更新 updated_at（换绑设备/重装 App 会重复注册）
        db.execute(
            "UPDATE push_tokens SET platform = :p, updated_at = CURRENT_TIMESTAMP(3) WHERE id = :id",
            {"p": platform, "id": existing["id"]},
        )
    else:
        db.insert(
            "INSERT INTO push_tokens (user_id, token, platform) VALUES (:u, :t, :p)",
            {"u": user["id"], "t": token, "p": platform},
        )
    log.info("用户 %s 注册推送 token（%s）", user["username"], platform)
    return ok({"registered": True, "user_id": int(user["id"])})


@router.get("/health")
def health():
    """§9.1：DB 不可用时返回 503 且 `db:"down"`；否则 200 且 `ok:true`。

    这里不用 ApiError（它的响应体不含 data），因为 §6.20 要求 health 响应体里能看到 `db` 字段——
    运维与验收脚本靠它区分「服务挂了」和「数据库挂了」。
    """
    db_ok = ping()
    data: dict = {
        "ok": db_ok,
        "version": settings.version,
        # 取值按契约 §6.20 原文（"ok"）与 §9.1（"down"）；§9.1 明确 health 在 DB 不可用时 503
        "db": "ok" if db_ok else "down",
            # 契约 §0.2 / CAL-09：节点未校时前必须如实回报 server，不能假装是设备时钟
        "clock_source": settings.clock_source,
        "server_ts": now_ms(),
    }
    # 从未发生过的事件（IoTDA 令牌未取过、webhook 未收到过）按 §0.4 省略键而不是写 null，
    # 否则运维会把「null」误读成「刚失效」。
    if RUNTIME.get("iotda_token_expire_at") is not None:
        data["iotda_token_expire_at"] = RUNTIME["iotda_token_expire_at"]
    if RUNTIME.get("webhook_last_at") is not None:
        data["webhook_last_at"] = RUNTIME["webhook_last_at"]
    body = {
        "code": 0 if db_ok else 1500,
        "message": "ok" if db_ok else ERROR_TABLE[1500][0],
        "data": data,
        "ts": data["server_ts"],
    }
    return JSONResponse(status_code=200 if db_ok else 503, content=body)


@router.get("/time")
def server_time():
    """契约 v1.2 §6.21：网关取 UTC（免鉴权，先取时后握手，超时 ≤3 s）。

    字段口径（§6.21 定稿）：
    - `utc_epoch`/`utc_epoch_ms`：同一时刻的秒/毫秒表示，避免调用方猜单位（§0.3 时间统一 ms，秒值兼容 RTC/SNTP 用法）；
    - `iso8601`：秒级精度的 `YYYY-MM-DDTHH:MM:SSZ`，直接可用于 MQTT 鉴权时间戳与日志；
    - `tz`：固定 `UTC`，提醒网关写入时**不要**本地闰 8 小时（IoTDA event_time 才是 UTC 无时区标记格式）；
    - 不返回设备侧可信度：本接口只声明服务器自己现在几点，网关校准后必须走 /api/health 的 clock_source。

    响应头 `Cache-Control: no-store`（§6.21 定稿要求）：网关每次上线都按 §4.1.1 降级链重新取时，
    中间若挂一层缓存就会把**过期时间**当现在发给网关，鉴权时间戳与 event_time 会一起偏移。
    缓存语义必须显式声明（否则共享缓存默认可按启发式缓存 200 响应），所以这里写死在响应头上而不是靠
    「GET 一般不会被缓存」的默认行为。
    """
    now = now_ms()
    moment = datetime.fromtimestamp(now / 1000, tz=timezone.utc)
    body = ok(
        {
            "utc_epoch": int(now // 1000),
            "utc_epoch_ms": now,
            "iso8601": moment.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "tz": "UTC",
            "server_ts": now,
        }
    )
    return JSONResponse(content=body, headers={"Cache-Control": "no-store"})
