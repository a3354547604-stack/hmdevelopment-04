"""统一响应体与错误码（契约 §6.1 响应体、§9.1 HTTP 状态码、§9.2 业务错误码）。

硬约束（契约 §6.1）：**不允许出现 HTTP 200 而 `code != 0`**。
因此 ApiError 自带 HTTP 状态码，由异常处理器同时写进状态码与响应体。
"""
from __future__ import annotations

from typing import Any

from .timeutil import now_ms

# (message, http_status) —— 与契约 §9.2 表格逐行对应
ERROR_TABLE: dict[int, tuple[str, int]] = {
    0: ("ok", 200),
    1000: ("缺少必填参数", 400),
    1001: ("参数格式错误", 400),
    1002: ("参数超出允许范围", 400),
    1003: ("未知参数名", 400),
    1004: ("时间范围非法", 400),
    1100: ("未提供访问凭证", 401),
    1101: ("凭证已过期", 401),
    1102: ("账号或密码错误", 401),
    1103: ("用户名已存在", 409),
    1104: ("无权操作该设备", 403),
    1105: ("请求过于频繁", 429),
    1200: ("设备不存在", 404),
    1201: ("绑定码错误", 400),
    1202: ("设备已被其他账号绑定", 409),
    1203: ("绑定码已过期", 410),
    1204: ("设备离线，无法下发", 409),
    1205: ("云端下发失败", 502),
    1206: ("演示事件非法", 400),
    1300: ("告警不存在", 404),
    1301: ("告警已处置", 409),
    1302: ("处置动作非法", 400),
    1400: ("报表无数据", 200),
    1500: ("数据库错误", 500),
    1501: ("上游 IoTDA 不可用", 502),
    1502: ("webhook 校验失败", 401),
}


class ApiError(Exception):
    """业务错误：code 决定 message 与 HTTP 状态码，extra 进响应体（如 missing / field / allowed）。"""

    def __init__(self, code: int, http_status: int | None = None, **extra: Any) -> None:
        message, table_status = ERROR_TABLE.get(code, ("未知错误", 500))
        self.code = code
        self.message = message
        self.http_status = http_status or table_status
        self.extra = {k: v for k, v in extra.items() if v is not None}
        super().__init__(f"{code} {message}")

    def body(self) -> dict[str, Any]:
        """错误体：细节（missing/field/allowed 等）一律放进 `data`，与成功响应同构。

        `data` 槽位保留（无细节时为 null），这样 App 只需一套解析路径：先看 code，再看 data。
        §0.4「不用 null 占位」约束的是业务可选字段（缺省即省略键），不是统一信封的载荷槽位。
        """
        payload: dict[str, Any] = {
            "code": self.code,
            "message": self.message,
            "data": dict(self.extra) or None,
            "ts": now_ms(),
        }
        return payload


def ok(data: Any = None) -> dict[str, Any]:
    return {"code": 0, "message": "ok", "data": data, "ts": now_ms()}
