"""时间工具：全系统内部一律 UTC，只有报表的「日/小时」按 UTC+8 切分（契约 §0.2、§7.6）。

约定：传给 MySQL 的 datetime 一律是 naive UTC（连接初始化 `SET time_zone='+00:00'`，见 db.py），
这样读回来的 datetime 也一定是 UTC，不会因为容器时区不同而漂移。
"""
from __future__ import annotations

from datetime import date, datetime, timedelta, timezone

UTC = timezone.utc
# 中国标准时间没有夏令时，用固定偏移比 tzinfo 数据库依赖更可靠（容器内不一定有 tzdata）
CST = timezone(timedelta(hours=8))
CST_OFFSET = "+08:00"


def now_ms() -> int:
    return int(datetime.now(tz=UTC).timestamp() * 1000)


def ms_to_dt(ms: int) -> datetime:
    """Unix ms → naive UTC datetime（写库用）。"""
    return datetime.fromtimestamp(ms / 1000, tz=UTC).replace(tzinfo=None)


def dt_to_ms(dt: datetime | None) -> int | None:
    """库里的 naive UTC datetime → Unix ms（出参用）。"""
    if dt is None:
        return None
    return int(dt.replace(tzinfo=UTC).timestamp() * 1000)


def ms_to_cst_date(ms: int) -> date:
    return datetime.fromtimestamp(ms / 1000, tz=CST).date()


def cst_day_bounds_ms(day: date) -> tuple[int, int]:
    """UTC+8 日历日 [00:00:00.000, 23:59:59.999] → (from_ms, to_ms) 闭区间。"""
    start = datetime(day.year, day.month, day.day, tzinfo=CST)
    return int(start.timestamp() * 1000), int((start + timedelta(days=1)).timestamp() * 1000) - 1


def cst_hour(ms: int) -> int:
    return datetime.fromtimestamp(ms / 1000, tz=CST).hour


def parse_date(text: str | None, default: date | None = None) -> date:
    if not text:
        return default or ms_to_cst_date(now_ms())
    return datetime.strptime(text, "%Y-%m-%d").date()


def iso(day: date) -> str:
    return day.strftime("%Y-%m-%d")
