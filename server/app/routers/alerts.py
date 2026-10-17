"""告警：列表 / 详情 / 处置（契约 §6.13–§6.15）。

状态机（§6.15）：`pending` --处置--> `resolved`；打开详情把 `pending` 置 `confirmed`（停推、未闭环）；
设备恢复只写 `recovered_at`，不改 `status`。
"""
from __future__ import annotations

import logging

from fastapi import APIRouter, Query
from pydantic import BaseModel

from ..deps import CurrentUser, device_context
from ..errors import ApiError, ok
from ..services import alerts as alert_svc
from ..services.alerts import device_settings

log = logging.getLogger("app.alerts")
router = APIRouter(prefix="/api/alerts", tags=["alerts"])

STATUSES = ("pending", "confirmed", "resolved", "all")


class DispositionIn(BaseModel):
    action: str
    note: str | None = None


@router.get("")
def list_alerts(
    user: CurrentUser,
    device_id: int = Query(...),
    status: str = Query(default="all"),
    type: str | None = Query(default=None),
    from_ms: int | None = Query(default=None, alias="from"),
    to_ms: int | None = Query(default=None, alias="to"),
    limit: int = Query(default=50, ge=1, le=200),
    offset: int = Query(default=0, ge=0),
) -> dict:
    device_context(user, device_id)
    if status not in STATUSES:
        raise ApiError(1002, field="status", allowed=list(STATUSES))
    if from_ms is not None and to_ms is not None and from_ms > to_ms:
        raise ApiError(1004, note="from 必须不晚于 to")
    return ok(
        alert_svc.list_alerts(
            device_id=device_id,
            status=status,
            alert_type=type,
            from_ms=from_ms,
            to_ms=to_ms,
            limit=limit,
            offset=offset,
        )
    )


@router.get("/{alert_id}")
def alert_detail(alert_id: int, user: CurrentUser) -> dict:
    row = alert_svc.get_alert_row(alert_id)
    device = device_context(user, int(row["device_id"]))
    data = alert_svc.detail(alert_id, device_settings(device))
    # §6.14：打开详情 = 已读；pending → confirmed（停推但不闭环），响应里体现最新状态
    if row["status"] == "pending":
        alert_svc.confirm_on_read(alert_id)
        data["status"] = "confirmed"
        data.pop("next_push_at", None)   # 停推 → 键省略（§0.4）
    return ok(data)


@router.post("/{alert_id}/disposition")
def dispose_alert(alert_id: int, payload: DispositionIn, user: CurrentUser) -> dict:
    row = alert_svc.get_alert_row(alert_id)
    device_context(user, int(row["device_id"]))
    note = (payload.note or "").strip() or None
    if note and len(note) > 200:
        raise ApiError(1002, field="note", allowed="≤200 字符")
    result = alert_svc.create_disposition(user, alert_id, (payload.action or "").strip(), note)
    log.info("告警 %s 由 %s 处置为 %s", alert_id, user["username"], payload.action)
    return ok(result)
