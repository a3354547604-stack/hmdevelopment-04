"""§7 建表验收：11 张表 + 两条幂等/去重唯一键（契约 §7.13 指定的验收断言）。"""
from __future__ import annotations

from app import db
from app.config import settings

# 契约 §7.3 的 10 张计划内表 + §7.3 增补的 push_tokens
PLANNED_TABLES = {
    "users", "devices", "user_devices", "nodes", "sensor_readings",
    "alerts", "alert_dispositions", "activity_hourly", "activity_daily", "device_online",
}
EXTRA_TABLES = {"push_tokens"}


def _tables() -> set[str]:
    rows = db.fetch_all(
        "SELECT TABLE_NAME AS t FROM information_schema.TABLES WHERE TABLE_SCHEMA = :s", {"s": settings.db_name}
    )
    return {row["t"] for row in rows}


def _index_columns(table: str, index_name: str) -> list[str]:
    rows = db.fetch_all(
        "SELECT COLUMN_NAME AS c FROM information_schema.STATISTICS "
        "WHERE TABLE_SCHEMA = :s AND TABLE_NAME = :t AND INDEX_NAME = :i ORDER BY SEQ_IN_INDEX",
        {"s": settings.db_name, "t": table, "i": index_name},
    )
    return [row["c"] for row in rows]


def test_all_tables_exist():
    tables = _tables()
    assert PLANNED_TABLES <= tables, f"缺少计划内表：{PLANNED_TABLES - tables}"
    assert EXTRA_TABLES <= tables, f"缺少增补表：{EXTRA_TABLES - tables}"


def test_sensor_readings_dedupe_unique_key():
    """(device_id,node_sn,type,ts_ms) 唯一键：webhook 重试的入库幂等键（§7.4）。"""
    cols = _index_columns("sensor_readings", "uk_readings")
    assert cols == ["device_id", "node_sn", "type", "ts_ms"], f"uk_readings 列不符：{cols}"


def test_alerts_event_id_unique():
    cols = _index_columns("alerts", "uk_alerts_event")
    assert cols == ["event_id"], "alerts.event_id 必须是唯一键（§4.4 幂等）"


def test_time_columns_are_datetime3_and_utc_session():
    """时间列 DATETIME(3) + 会话时区 UTC：§0.2 的硬要求，错了会让报表日界整体偏移 8 小时。"""
    cols = db.fetch_all(
        "SELECT TABLE_NAME AS t, COLUMN_NAME AS c, DATETIME_PRECISION AS p FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA = :s AND DATA_TYPE = 'datetime' AND COLUMN_NAME IN ('ts', 'created_at', 'updated_at')",
        {"s": settings.db_name},
    )
    assert cols, "没有找到 DATETIME 时间列"
    assert all(int(row["p"] or 0) == 3 for row in cols), [row for row in cols if int(row["p"] or 0) != 3]
    tz = db.fetch_scalar("SELECT @@session.time_zone")
    assert tz in ("+00:00", "UTC"), f"会话时区必须是 UTC，实际 {tz}"
