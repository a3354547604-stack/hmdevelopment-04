"""IoTDA 规则引擎 → 服务器 webhook（契约 §5）。

三条硬约束（§5.2）：
1. 鉴权只用 URL 查询串 `?token=`（平台不方便带自定义 Header），常量时间比较；
2. **只依赖** `resource` + `notify_data.header.device_id` + `notify_data.body.services[]`，
   平台哪天新增字段都不会把这条链路改坏；
3. 无法识别/无法匹配一律 **HTTP 200**（否则平台会不停重推），只在日志里 WARN。
"""
from __future__ import annotations

import hmac
import json
import logging

from fastapi import APIRouter, Query, Request

from ..config import settings
from ..errors import ApiError, ok
from ..services import telemetry

log = logging.getLogger("app.webhook")
router = APIRouter(prefix="/api/iotda", tags=["iotda"])


@router.post("/webhook")
async def iotda_webhook(request: Request, token: str | None = Query(default=None)) -> dict:
    if not token or not hmac.compare_digest(token, settings.webhook_token):
        # §9.2 1502：响应体最小化，不告诉对方「token 错了还是没带」
        raise ApiError(1502)

    raw = await request.body()
    if len(raw) > 256 * 1024:
        raise ApiError(1001, note="body 超过 256 KB")
    try:
        body = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        log.warning("webhook body 不是合法 JSON：%s（前 200 字节：%r）", exc, raw[:200])
        raise ApiError(1001, note="body 必须是 UTF-8 JSON 对象") from exc
    if not isinstance(body, dict):
        raise ApiError(1001, note="body 必须是 JSON 对象")

    summary = telemetry.handle_webhook(body)
    return ok(summary)
