"""FastAPI 依赖：登录态与设备权限。

权限判定一律查 `user_devices`（契约 §7.4：不看 devices.owner_id，它只是快照）。
"""
from __future__ import annotations

from typing import Annotated, Any

from fastapi import Depends, Header

from . import db
from .errors import ApiError
from .security import decode_token

USER_COLUMNS = "id, username, phone, created_at"


def current_user(authorization: str | None = Header(default=None)) -> dict[str, Any]:
    if not authorization or not authorization.strip():
        raise ApiError(1100)
    parts = authorization.split()
    if len(parts) != 2 or parts[0].lower() != "bearer":
        raise ApiError(1100)
    claims = decode_token(parts[1])
    try:
        user_id = int(claims.get("sub", ""))
    except (TypeError, ValueError) as exc:
        raise ApiError(1101) from exc
    user = db.fetch_one(f"SELECT {USER_COLUMNS} FROM users WHERE id = :uid", {"uid": user_id})
    if not user:
        # token 合法但用户已被删：等同于凭证失效
        raise ApiError(1101)
    return user


def device_context(user: dict[str, Any], device_id: int, need_owner: bool = False) -> dict[str, Any]:
    """返回设备行；非成员 1104/403，不存在 1200/404。"""
    device = db.fetch_one(
        "SELECT id, sn, name, bind_code, bind_code_expire_at, iotda_device_id, owner_id, fw_ver, "
        "settings_json, created_at "
        "FROM devices WHERE id = :did",
        {"did": device_id},
    )
    if not device:
        raise ApiError(1200)
    member = db.fetch_one(
        "SELECT role FROM user_devices WHERE user_id = :uid AND device_id = :did AND deleted_at IS NULL",
        {"uid": user["id"], "did": device_id},
    )
    if not member:
        raise ApiError(1104)
    if need_owner and member["role"] != "owner":
        raise ApiError(1104)
    device["role"] = member["role"]
    return device


def online_row(device_id: int) -> dict[str, Any] | None:
    return db.fetch_one("SELECT * FROM device_online WHERE device_id = :did", {"did": device_id})


# 路由签名里统一写 `user: CurrentUser`，避免每个 handler 重复 Depends(...)
CurrentUser = Annotated[dict[str, Any], Depends(current_user)]
