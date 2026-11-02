"""IoTDA 规则引擎 → 服务器 webhook（契约 §5）。

三条硬约束（§5.2）：
1. 鉴权只用 URL 查询串 `?token=`（平台不方便带自定义 Header），常量时间比较；
2. **只依赖** `resource` + `notify_data.header.device_id` + `notify_data.body.services[]`，
   平台哪天新增字段都不会把这条链路改坏；
3. 无法识别/无法匹配一律 **HTTP 200**（否则平台会不停重推），只在日志里 WARN。

排查开关：`WEBHOOK_DUMP=1` 时把**原始报文前 1200 字节**以 `WEBHOOK_RAW` 前缀打到 WARNING 日志。
它默认关闭、只读不写（不入库、不改响应、不动业务分支），用于「平台实际发来的形状」与
契约假设不一致时取证——t196 就是靠它发现真实报文是 `device.property` + `body.services[]`。
token 在 URL 查询串里，不在 body，故不会被打进日志。
"""
from __future__ import annotations

import hmac
import json
import logging
import os

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
    if os.environ.get("WEBHOOK_DUMP") == "1":
        # 取证用：默认关。截断到 1200 字节——足够看出 resource/header/body 三层形状，
        # 又不至于把一条 3 节点明细的整包（>4 KB）灌进日志。
        log.warning("WEBHOOK_RAW %s", raw[:1200].decode("utf-8", "replace"))
    try:
        body = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        log.warning("webhook body 不是合法 JSON：%s（前 200 字节：%r）", exc, raw[:200])
        raise ApiError(1001, note="body 必须是 UTF-8 JSON 对象") from exc
    if not isinstance(body, dict):
        raise ApiError(1001, note="body 必须是 JSON 对象")

    summary = telemetry.handle_webhook(body)
    return ok(summary)
