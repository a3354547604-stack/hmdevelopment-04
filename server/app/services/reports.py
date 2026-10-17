"""报表：活动分布 / 趋势 / 周月汇总（契约 §6.16–§6.18）。

归并口径（契约 §7.6 明文要求二选一并在注释写明）：**用 SQL 的
`CONVERT_TZ(ts,'+00:00','+08:00')` 在 MySQL 侧按 UTC+8 切分**，因此
`activity_hourly`/`activity_daily` 的 `date`/`hour` 一律是 UTC+8 语义；
服务器只做减法与拼装，不在 Python 里二次换算日历日（少一处漂移点）。
"""
from __future__ import annotations

from datetime import date, timedelta
from typing import Any

from .. import db
from ..summary import prev_compare_text, trend_compare
from ..timeutil import cst_day_bounds_ms, iso, ms_to_cst_date, now_ms

PERIOD_DAYS = {"day": 1, "week": 7, "month": 30}
PERIOD_PREV_LABEL = {"day": "昨日", "week": "上周", "month": "上月"}


# --------------------------------------------------------------------------- 数据读取

def _daily_counts(device_id: int, start: date, end: date) -> dict[str, int]:
    rows = db.fetch_all(
        "SELECT date, count FROM activity_daily WHERE device_id = :d AND date BETWEEN :a AND :b",
        {"d": device_id, "a": start, "b": end},
    )
    return {iso(row["date"]) if isinstance(row["date"], date) else str(row["date"]): int(row["count"]) for row in rows}


def _hourly_counts(device_id: int, start: date, end: date) -> dict[int, int]:
    rows = db.fetch_all(
        "SELECT hour, SUM(count) AS c FROM activity_hourly "
        "WHERE device_id = :d AND date BETWEEN :a AND :b GROUP BY hour",
        {"d": device_id, "a": start, "b": end},
    )
    return {int(row["hour"]): int(row["c"] or 0) for row in rows}


def _day_rows(device_id: int, start: date, end: date) -> list[tuple[date, int]]:
    counts = _daily_counts(device_id, start, end)
    days: list[tuple[date, int]] = []
    cursor = start
    while cursor <= end:
        days.append((cursor, counts.get(iso(cursor), 0)))
        cursor += timedelta(days=1)
    return days


def _period_bounds(period: str, ref: date) -> tuple[date, date]:
    """period 的日历窗口（含 end）——day=当天；week=近 7 天（含今天）；month=近 30 天。"""
    span = PERIOD_DAYS.get(period)
    if span is None:
        span = 1
    return ref - timedelta(days=span - 1), ref


# --------------------------------------------------------------------------- §6.16

def activity(device_id: int, period: str = "day", ref: date | None = None) -> dict[str, Any]:
    ref = ref or ms_to_cst_date(now_ms())
    start, end = _period_bounds(period, ref)
    from_ms, _ = cst_day_bounds_ms(start)
    _, to_ms = cst_day_bounds_ms(end)
    daily_rows = _day_rows(device_id, start, end)
    total = sum(count for _, count in daily_rows)

    data: dict[str, Any] = {
        "device_id": device_id,
        "period": period,
        "from": from_ms,
        "to": to_ms,
        "total": total,
        "daily": [{"date": iso(day), "count": count} for day, count in daily_rows],
    }
    if period == "day":
        hourly = _hourly_counts(device_id, start, end)
        # §6.16：period=day 时 hourly 24 项（UTC+8 小时），0 点也要出现
        data["hourly"] = [{"hour": hour, "count": hourly.get(hour, 0)} for hour in range(24)]
    # week/month 不返回 hourly（契约原文：hourly 省略）
    return data


# --------------------------------------------------------------------------- §6.17

def trend(device_id: int, days: int = 7) -> dict[str, Any]:
    today = ms_to_cst_date(now_ms())
    start = today - timedelta(days=days - 1)
    rows = _day_rows(device_id, start, today)
    today_count = rows[-1][1] if rows else 0
    yesterday_count = rows[-2][1] if len(rows) > 1 else 0
    delta_pct, text = trend_compare(today_count, yesterday_count)
    return {
        "device_id": device_id,
        "days": days,
        "series": [{"date": iso(day), "count": count} for day, count in rows],
        "compare": {
            "today": today_count,
            "yesterday": yesterday_count,
            "delta_pct": delta_pct,
            "text": text,
        },
    }


# --------------------------------------------------------------------------- §6.18

def _env_stats(device_id: int, from_ms: int, to_ms: int) -> dict[str, Any]:
    """环境统计取自网关属性快照（`type='gateway'`，payload 里有 temp/humi）。"""
    row = db.fetch_one(
        "SELECT "
        "ROUND(AVG(CAST(payload_json->>'$.temp' AS DECIMAL(6,2))),1) AS temp_avg, "
        "ROUND(MIN(CAST(payload_json->>'$.temp' AS DECIMAL(6,2))),1) AS temp_min, "
        "ROUND(MAX(CAST(payload_json->>'$.temp' AS DECIMAL(6,2))),1) AS temp_max, "
        "ROUND(AVG(CAST(payload_json->>'$.humi' AS DECIMAL(6,2))),1) AS humi_avg "
        "FROM sensor_readings WHERE device_id = :d AND type = 'gateway' AND ts_ms BETWEEN :f AND :t",
        {"d": device_id, "f": from_ms, "t": to_ms},
    ) or {}
    return {key: (float(row[key]) if row.get(key) is not None else None)
            for key in ("temp_avg", "temp_min", "temp_max", "humi_avg")}


def summary(device_id: int, period: str = "week", ref: date | None = None) -> dict[str, Any]:
    ref = ref or ms_to_cst_date(now_ms())
    start, end = _period_bounds(period, ref)
    from_ms, _ = cst_day_bounds_ms(start)
    _, to_ms = cst_day_bounds_ms(end)

    daily_rows = _day_rows(device_id, start, end)
    total = sum(count for _, count in daily_rows)
    active_days = sum(1 for _, count in daily_rows if count > 0)
    hourly = _hourly_counts(device_id, start, end)
    peak_hour = min_hour = None
    if total > 0:
        peak_hour = max(range(24), key=lambda h: hourly.get(h, 0))
        non_zero = [h for h in range(24) if hourly.get(h, 0) > 0]
        min_hour = min(non_zero, key=lambda h: hourly.get(h, 0)) if non_zero else None

    # 上一周期（等长窗口）用于「较上周/上月」
    prev_end = start - timedelta(days=1)
    prev_start = prev_end - timedelta(days=(end - start).days)
    prev_total = sum(count for _, count in _day_rows(device_id, prev_start, prev_end))

    alert_row = db.fetch_one(
        "SELECT COUNT(*) AS total FROM alerts WHERE device_id = :d AND ts_ms BETWEEN :f AND :t",
        {"d": device_id, "f": from_ms, "t": to_ms},
    ) or {}
    by_type_rows = db.fetch_all(
        "SELECT type, COUNT(*) AS c FROM alerts WHERE device_id = :d AND ts_ms BETWEEN :f AND :t GROUP BY type",
        {"d": device_id, "f": from_ms, "t": to_ms},
    )
    status_rows = db.fetch_all(
        "SELECT status, COUNT(*) AS c FROM alerts WHERE device_id = :d AND ts_ms BETWEEN :f AND :t GROUP BY status",
        {"d": device_id, "f": from_ms, "t": to_ms},
    )
    status_map = {row["status"]: int(row["c"]) for row in status_rows}
    false_alarm = int(db.fetch_scalar(
        "SELECT COUNT(*) FROM alert_dispositions d JOIN alerts a ON a.id = d.alert_id "
        "WHERE a.device_id = :d AND a.ts_ms BETWEEN :f AND :t AND d.action = 'false_alarm'",
        {"d": device_id, "f": from_ms, "t": to_ms},
        0,
    ) or 0)

    alerts_total = int(alert_row.get("total") or 0)
    return {
        "device_id": device_id,
        "period": period,
        "from": from_ms,
        "to": to_ms,
        "activity": {
            "total": total,
            "active_days": active_days,
            "avg_daily": round(total / max(1, len(daily_rows)), 1),
            "peak_hour": peak_hour,
            "min_hour": min_hour,
            "compare_prev": prev_compare_text(total, prev_total, PERIOD_PREV_LABEL.get(period, "上一周期")),
        },
        "alerts": {
            "total": alerts_total,
            "by_type": {row["type"]: int(row["c"]) for row in by_type_rows},
            "resolved": status_map.get("resolved", 0),
            "pending": status_map.get("pending", 0),
            "false_alarm": false_alarm,
        },
        "env": _env_stats(device_id, from_ms, to_ms),
    }
