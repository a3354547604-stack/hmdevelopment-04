"""报表：活动时段 / 趋势 / 汇总（契约 §6.16–§6.18）。

三个接口都不返回 1400 错误：无数据时 `items`/`hourly`/`daily` 为空数组、`total=0`（§9.2 明文）。
"""
from __future__ import annotations

from fastapi import APIRouter, Query

from ..deps import CurrentUser, device_context
from ..errors import ApiError, ok
from ..services import reports as report_svc
from ..timeutil import parse_date

router = APIRouter(prefix="/api/reports", tags=["reports"])

PERIODS = ("day", "week", "month")


def _period(period: str) -> str:
    value = (period or "day").lower()
    if value not in PERIODS:
        raise ApiError(1002, field="period", allowed=list(PERIODS))
    return value


@router.get("/activity")
def activity(
    user: CurrentUser,
    device_id: int = Query(...),
    period: str = Query(default="day"),
    date: str | None = Query(default=None),
) -> dict:
    device_context(user, device_id)
    ref = parse_date(date)  # 非法日期 → 1001
    return ok(report_svc.activity(device_id, _period(period), ref))


@router.get("/trend")
def trend(
    user: CurrentUser,
    device_id: int = Query(...),
    days: int = Query(default=7, ge=1, le=90),
) -> dict:
    device_context(user, device_id)
    return ok(report_svc.trend(device_id, days))


@router.get("/summary")
def summary(
    user: CurrentUser,
    device_id: int = Query(...),
    period: str = Query(default="week"),
    date: str | None = Query(default=None),
) -> dict:
    device_context(user, device_id)
    value = _period(period)
    if value == "day":
        raise ApiError(1002, field="period", allowed=["week", "month"], note="summary 只支持 week/month")
    return ok(report_svc.summary(device_id, value, parse_date(date)))
