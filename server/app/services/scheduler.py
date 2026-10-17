"""后台调度器（契约 §7.7 重推、§8.7 网关离线、§7.9/§7.10 报表归并）。

三个作业，各自可被测试单独调用（`tick_*`），线程只是把它们按周期串起来：

| 作业 | 周期 | 契约依据 |
|---|---|---|
| 告警重推 | 10 s | §7.7「每 10 s 扫 status='pending' AND recovered_at IS NULL AND next_push_at<=now」 |
| 网关离线 | 10 s | §8.7「180 s 无上报 → online=0 + device_offline 告警」 |
| 活动归并 | 5 min（每天 00:10 补算前一天） | §7.9「每 5 分钟重算今天，每日 00:10 重算前一天」 |

为什么用进程内线程而不是 Celery/APScheduler：单进程单实例部署，作业是幂等的短事务；
引入外部队列会让 `docker compose up` 多两个组件，对竞赛交付没有收益（P1 再拆）。
"""
from __future__ import annotations

import logging
import threading
from datetime import date, datetime, timedelta
from typing import Any

from . import alerts
from ..config import settings
from .. import db, state
from ..timeutil import cst_day_bounds_ms, CST, iso, ms_to_dt, ms_to_cst_date, now_ms

log = logging.getLogger("app.scheduler")

REPUSH_INTERVAL_S = 10
ACTIVITY_INTERVAL_S = 300


# --------------------------------------------------------------------------- 作业

def tick_repush() -> int:
    count = alerts.run_repush_once()
    if count:
        log.info("重推 %d 条告警（§7.7 节奏 0/60/300 s）", count)
    return count


def _offline_timeout_s(device: dict[str, Any]) -> int:
    cfg = alerts.device_settings(device)
    return int(cfg.get("gateway_offline_timeout_s") or settings.gateway_offline_timeout_s)


def tick_gateway_offline() -> list[int]:
    """返回本轮新置为离线的设备 id。"""
    now = now_ms()
    rows = db.fetch_all(
        "SELECT o.device_id, o.last_heartbeat_ms, d.* FROM device_online o "
        "JOIN devices d ON d.id = o.device_id WHERE o.online = 1"
    )
    offlined: list[int] = []
    for row in rows:
        last = row.get("last_heartbeat_ms")
        if not last:
            continue
        if now - int(last) <= _offline_timeout_s(row) * 1000:
            continue
        db.execute("UPDATE device_online SET online = 0 WHERE device_id = :d", {"d": row["device_id"]})
        # event_id 用「最后一次心跳」而不是当前时刻：同一次离线只产生一条告警，
        # 网关恢复后又离线（心跳时间变了）才会新增（§8.7）。
        alerts.raise_alert(
            row,
            "device_offline",
            1,
            None,
            {"last_heartbeat_ms": int(last), "timeout_s": _offline_timeout_s(row)},
            ts_ms=now,
            event_id=f"{row['sn']}-device_offline-{int(last)}",
        )
        offlined.append(int(row["device_id"]))
        log.warning("设备 %s 判定离线（最后心跳 %s）", row["sn"], ms_to_dt(int(last)))
    return offlined


def recompute_activity(day: date) -> dict[str, int]:
    """按 UTC+8 重算某日的活动分布（契约 §7.6 明确用 CONVERT_TZ 在 SQL 侧切分）。

    P0 简化（已登记给 PM）：读数只增不减，因此重算是「补齐/更新」而不是「清零重算」；
    若要支持删除读数后的回算，需要先 DELETE 当日的行再 INSERT。
    """
    from_ms, to_ms = cst_day_bounds_ms(day)
    hourly = db.execute(
        "INSERT INTO activity_hourly (device_id, `date`, `hour`, `count`) "
        "SELECT device_id, DATE(CONVERT_TZ(ts,'+00:00','+08:00')) AS d, "
        "HOUR(CONVERT_TZ(ts,'+00:00','+08:00')) AS h, COUNT(*) AS c "
        "FROM sensor_readings "
        "WHERE type = 'presence' AND payload_json->>'$.presence' = 'true' "
        "AND ts_ms BETWEEN :f AND :t "
        "GROUP BY device_id, d, h "
        "ON DUPLICATE KEY UPDATE `count` = VALUES(`count`)",
        {"f": from_ms, "t": to_ms},
    )
    daily = db.execute(
        "INSERT INTO activity_daily (device_id, `date`, `count`, `active_minutes`) "
        "SELECT device_id, DATE(CONVERT_TZ(ts,'+00:00','+08:00')) AS d, COUNT(*) AS c, "
        "COUNT(DISTINCT FLOOR(UNIX_TIMESTAMP(ts) / 60)) AS m "
        "FROM sensor_readings "
        "WHERE type = 'presence' AND payload_json->>'$.presence' = 'true' "
        "AND ts_ms BETWEEN :f AND :t "
        "GROUP BY device_id, d "
        "ON DUPLICATE KEY UPDATE `count` = VALUES(`count`), `active_minutes` = VALUES(`active_minutes`)",
        {"f": from_ms, "t": to_ms},
    )
    return {"day": iso(day), "hourly": hourly, "daily": daily}


def tick_activity() -> dict[str, Any]:
    today = ms_to_cst_date(now_ms())
    now_local = datetime.now(tz=CST)
    result: dict[str, Any] = {"today": recompute_activity(today)}
    if now_local.hour == 0 and now_local.minute >= 10:
        # 00:10 之后补算前一天（契约 §7.9）
        result["yesterday"] = recompute_activity(today - timedelta(days=1))
    return result


def tick_once() -> dict[str, Any]:
    """一轮全部作业；测试与 `/api/health` 的调度器探针都用它。"""
    state.RUNTIME["scheduler_ticks"] = int(state.RUNTIME["scheduler_ticks"]) + 1
    state.RUNTIME["scheduler_last_at"] = now_ms()
    result = {
        "repushed": tick_repush(),
        "offlined": tick_gateway_offline(),
    }
    if state.RUNTIME["scheduler_ticks"] == 1 or int(state.RUNTIME["scheduler_ticks"]) % 6 == 0:
        result["activity"] = tick_activity()
    return result


# --------------------------------------------------------------------------- 线程

class Scheduler:
    def __init__(self, interval_s: int = REPUSH_INTERVAL_S) -> None:
        self.interval_s = interval_s
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    @property
    def running(self) -> bool:
        return bool(self._thread and self._thread.is_alive())

    def _loop(self) -> None:
        log.info("调度器启动：周期 %d s", self.interval_s)
        while not self._stop.wait(self.interval_s):
            try:
                tick_once()
            except Exception:  # 单个作业失败不能拖垮进程
                log.exception("调度器本轮执行失败")
        log.info("调度器已停止")

    def start(self) -> None:
        if self.running:
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._loop, name="hmprogram-scheduler", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=timeout)
            self._thread = None


scheduler = Scheduler()
