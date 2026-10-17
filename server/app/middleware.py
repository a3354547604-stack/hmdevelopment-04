"""横切中间件：`Idempotency-Key` 重放（契约 §6.1）。

为什么放在中间件而不是每个 handler：契约要求「所有写接口」都支持该头。中间件里做一处，
既不会漏接口，也不用每个 handler 记一遍。缓存落在进程内存（只服务单实例 P0）：
- 命中 → 直接回放首次响应体，并加 `Idempotent-Replay: true` 便于验收脚本区分；
- 键包含调用者身份（Authorization 摘要或来源 IP）+ 方法 + 路径 + 该头，避免不同用户互撞；
- 只缓存 2xx（失败允许客户端换参数重试），TTL 默认 300 s（= 契约 5 分钟）。
"""
from __future__ import annotations

import hashlib
import logging

from starlette.middleware.base import BaseHTTPMiddleware
from starlette.requests import Request
from starlette.responses import Response

from .config import settings
from .timeutil import now_ms

log = logging.getLogger("app.idempotency")

_MAX_ENTRIES = 512
_CACHE: dict[str, dict] = {}
_WRITE_METHODS = {"POST", "PUT", "PATCH"}


def _identity(request: Request) -> str:
    auth = request.headers.get("authorization") or ""
    if auth:
        return hashlib.sha256(auth.encode("utf-8")).hexdigest()[:16]
    return request.client.host if request.client else "anonymous"


def _cache_key(request: Request, key: str) -> str:
    raw = f"{_identity(request)}|{request.method}|{request.url.path}|{request.url.query}|{key}"
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def _prune(now: int) -> None:
    ttl_ms = settings.idempotency_ttl_s * 1000
    for cache_key, entry in list(_CACHE.items()):
        if now - entry["at"] > ttl_ms:
            _CACHE.pop(cache_key, None)
    while len(_CACHE) > _MAX_ENTRIES:  # 极端情况兜底：丢最早的一条
        _CACHE.pop(next(iter(_CACHE)), None)


def reset() -> None:
    """测试用：清空重放缓存。"""
    _CACHE.clear()


class IdempotencyMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        key = request.headers.get("idempotency-key")
        if request.method not in _WRITE_METHODS or not key:
            return await call_next(request)

        now = now_ms()
        cache_key = _cache_key(request, key)
        cached = _CACHE.get(cache_key)
        if cached and now - cached["at"] <= settings.idempotency_ttl_s * 1000:
            log.info("幂等命中：%s %s key=%s", request.method, request.url.path, key)
            return Response(
                content=cached["body"],
                status_code=cached["status"],
                media_type=cached["media_type"],
                headers={"Idempotent-Replay": "true"},
            )

        response = await call_next(request)
        if not 200 <= response.status_code < 300:
            return response  # 失败不缓存：允许客户端换参数重试

        chunks = [chunk async for chunk in response.body_iterator]
        body = b"".join(chunks)
        _prune(now)
        _CACHE[cache_key] = {
            "at": now,
            "status": response.status_code,
            "body": body,
            "media_type": response.media_type,
        }
        headers = {k: v for k, v in response.headers.items() if k.lower() != "content-length"}
        return Response(content=body, status_code=response.status_code, headers=headers, media_type=response.media_type)
