"""口令哈希、JWT 与登录限流（契约 §6.1、§6.4、§9.1）。

JWT 载荷：{sub, username, iat, exp}；HS256；有效期 7 天（契约 §6.1）。
`sub` 按 JWT 规范写成字符串（契约示例里是数字，序列化差异在此说明：读回时 int() 还原）。
"""
from __future__ import annotations

import logging
import time
from collections import deque
from typing import Any

import bcrypt
import jwt

from .config import settings
from .errors import ApiError
from .timeutil import now_ms

log = logging.getLogger("app.security")

BCRYPT_ROUNDS = 10  # 契约 §7.2：bcrypt cost=10
_BCRYPT_MAX_BYTES = 72  # bcrypt 输入上限，超长口令先截断（防静默异常）


def hash_password(password: str) -> str:
    raw = password.encode("utf-8")[:_BCRYPT_MAX_BYTES]
    return bcrypt.hashpw(raw, bcrypt.gensalt(rounds=BCRYPT_ROUNDS)).decode("ascii")


def verify_password(password: str, password_hash: str) -> bool:
    if not password_hash:
        return False
    raw = password.encode("utf-8")[:_BCRYPT_MAX_BYTES]
    try:
        return bcrypt.checkpw(raw, password_hash.encode("ascii"))
    except ValueError:
        # 哈希格式不合法（历史数据/脏数据）→ 直接判定失败，不抛给调用方
        return False


def create_token(user_id: int, username: str) -> tuple[str, int]:
    """返回 (token, expires_in_seconds)。"""
    expires_in = settings.jwt_expire_days * 86400
    now = int(time.time())
    payload = {
        "sub": str(user_id),
        "username": username,
        "iat": now,
        "exp": now + expires_in,
    }
    return jwt.encode(payload, settings.jwt_secret, algorithm=settings.jwt_algorithm), expires_in


def decode_token(token: str) -> dict[str, Any]:
    try:
        return jwt.decode(token, settings.jwt_secret, algorithms=[settings.jwt_algorithm])
    except jwt.ExpiredSignatureError as exc:
        raise ApiError(1101) from exc
    except jwt.PyJWTError as exc:
        # 签名错误/结构错误同样按「凭证不可用」处理：App 收到 1101 即回登录页（契约 §9.2）
        log.info("JWT 校验失败：%s", exc)
        raise ApiError(1101) from exc


class LoginRateLimiter:
    """§9.1：同一 IP 5 分钟内 >100 次登录尝试 → 429/1105。

    进程内滑动窗口（单进程部署足够；多副本时需换 Redis，P1）。
    """

    def __init__(self, limit: int, window_s: int) -> None:
        self.limit = limit
        self.window_ms = window_s * 1000
        self._hits: dict[str, deque[int]] = {}

    def check(self, key: str) -> None:
        now = now_ms()
        bucket = self._hits.setdefault(key, deque())
        while bucket and now - bucket[0] > self.window_ms:
            bucket.popleft()
        if len(bucket) >= self.limit:
            raise ApiError(1105)
        bucket.append(now)

    def reset(self) -> None:
        self._hits.clear()


login_limiter = LoginRateLimiter(settings.login_rate_limit, settings.login_rate_window_s)
# 厂商侧出厂注册（t152）单独一个桶：它和登录是两种攻击面，混用会让厂商改产线时把用户登录打成 429
activate_limiter = LoginRateLimiter(settings.product_key_rate_limit, settings.product_key_rate_window_s)
