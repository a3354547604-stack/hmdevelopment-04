"""FastAPI 应用装配（契约 §0.6 前缀、§6.1 统一响应体、§9 HTTP 状态码）。

启动顺序刻意做成「先建库建表，再起调度器」：调度器第一轮就要查 `alerts`，
表还没建好会让日志里出现一串误导性的 500。数据库不可用时**不**让进程退出——
容器编排（compose restart）需要进程活着才能重试，而 `/api/health` 会如实报 `db:"down"`。
"""
from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from typing import Any

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from . import db, middleware
from .config import settings
from .errors import ApiError, ok
from .routers import alerts, auth, devices, push, reports, vendor, webhook
from .services.scheduler import scheduler
from .timeutil import now_ms

log = logging.getLogger("app")


def setup_logging() -> None:
    logging.basicConfig(
        level=getattr(logging, settings.log_level, logging.INFO),
        format="%(asctime)s %(levelname)-7s %(name)s | %(message)s",
    )


@asynccontextmanager
async def lifespan(_: FastAPI):
    setup_logging()
    log.info("启动 hmprogram-server v%s（APP_ENV=%s）", settings.version, settings.app_env)
    if settings.db_auto_migrate:
        try:
            db.ensure_database()
            log.info("建表语句执行完成：%d 条（DDL 见 db/schema.sql）", db.apply_schema())
        except Exception:  # noqa: BLE001 —— 数据库晚于 API 就绪时必须继续启动并如实报错
            log.exception("自动建表失败（DB 未就绪？）：/api/health 会报 db:down")
    if settings.scheduler_enabled:
        scheduler.start()
    try:
        yield
    finally:
        scheduler.stop()
        log.info("hmprogram-server 已停止")


app = FastAPI(
    title="独居老人居家安全监测与应急辅助系统 · 服务器",
    version=settings.version,
    description="契约见 docs/技术/接口契约.md；本服务实现 §5 webhook 与 §6 REST（18 个 + 补充）。",
    lifespan=lifespan,
    docs_url="/docs",
    openapi_url="/openapi.json",
)

app.add_middleware(middleware.IdempotencyMiddleware)
# §6.1：CORS 只放通本地调试来源（App 是原生客户端，不走浏览器 CORS）
app.add_middleware(
    CORSMiddleware,
    allow_origins=settings.cors_origins,
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# --------------------------------------------------------------------------- 异常 → 统一响应体

# 没有业务码的框架级错误（未知路由/方法不允许/…）按 HTTP 状态码归类，保证响应体仍是 {code,message,data,ts}
_HTTP_FALLBACK = {401: 1100, 403: 1104, 404: 1001, 405: 1001, 413: 1001, 429: 1105, 500: 1500, 502: 1501, 503: 1500}


@app.exception_handler(ApiError)
async def _api_error_handler(_: Request, exc: ApiError) -> JSONResponse:
    body = exc.body()
    if exc.code == 1502:
        # §9.2：1502 响应体最小化，回显细节会给扫描器免费信息
        body = {"code": exc.code, "message": exc.message}
    return JSONResponse(status_code=exc.http_status, content=body)


# 数值/长度/枚举类校验失败按 §9.2 归到 1002（超出范围，带 field + allowed），其余归 1001
_RANGE_ERRORS = {
    "less_than", "less_than_equal", "greater_than", "greater_than_equal",
    "string_too_short", "string_too_long", "too_short", "too_long", "enum",
}


@app.exception_handler(RequestValidationError)
async def _validation_handler(_: Request, exc: RequestValidationError) -> JSONResponse:
    """把 pydantic 校验失败翻译成 §9.2 的 1000/1001/1002（默认 422 不在契约状态码表里）。"""
    missing: list[str] = []
    field = None
    out_of_range = False
    allowed: Any = None
    for error in exc.errors():
        loc = [str(part) for part in error.get("loc", []) if part not in ("body", "query", "path")]
        name = ".".join(loc) or "body"
        etype = str(error.get("type") or "")
        if etype in ("missing", "value_error.missing"):
            missing.append(name)
        elif etype in _RANGE_ERRORS:
            if field is None:
                field = name
            out_of_range = True
            ctx = error.get("ctx") or {}
            for key in ("ge", "gt", "le", "lt", "min_length", "max_length", "expected"):
                if key in ctx:
                    allowed = ctx[key]
        elif field is None:
            field = name
    if missing:
        return JSONResponse(status_code=400, content=ApiError(1000, missing=missing).body())
    if out_of_range:
        extra = {"allowed": allowed} if allowed is not None else {}
        return JSONResponse(status_code=400, content=ApiError(1002, field=field, **extra).body())
    return JSONResponse(status_code=400, content=ApiError(1001, field=field).body())


@app.exception_handler(StarletteHTTPException)
async def _http_error_handler(_: Request, exc: StarletteHTTPException) -> JSONResponse:
    code = _HTTP_FALLBACK.get(exc.status_code, 1001)
    error = ApiError(code, http_status=exc.status_code, detail=str(exc.detail))
    return JSONResponse(status_code=exc.status_code, content=error.body())


@app.exception_handler(Exception)
async def _unhandled_handler(_: Request, exc: Exception) -> JSONResponse:
    log.exception("未处理异常：%s", exc)
    return JSONResponse(status_code=500, content=ApiError(1500).body())


# --------------------------------------------------------------------------- 路由

app.include_router(auth.router)
app.include_router(devices.router)
app.include_router(vendor.router)   # t152：厂商侧出厂注册（非用户 API，X-Product-Key 鉴权）
app.include_router(alerts.router)
app.include_router(reports.router)
app.include_router(push.router)
app.include_router(webhook.router)


@app.get("/", include_in_schema=False)
def root() -> dict:
    """给「浏览器直接打开 3000 端口」的人一个落点，避免看起来像服务挂了。"""
    return ok({"service": "hmprogram-server", "version": settings.version, "docs": "/docs", "ts": now_ms()})
