"""进程内运行态（只读接口用，不落库）。

放这里的都是「重启即失效、无需持久化」的观测值：mock 下发的命令流水、webhook 最近时刻、
IoTDA 应用侧 token 过期时刻。这样 `/api/health` 与演示排查不需要为观测再造一张表。
"""
from __future__ import annotations

from typing import Any

from .timeutil import now_ms

_MAX_COMMANDS = 100  # 流水只留最近 100 条，避免长时间运行吃内存

RUNTIME: dict[str, Any] = {
    "started_at": now_ms(),
    "webhook_last_at": None,
    "webhook_last_resource": None,
    "iotda_token_expire_at": None,
    "commands": [],  # mock/real 下发流水：{command, request_id, device_sn, topic, paras, at, mode}
    "scheduler_ticks": 0,
    "scheduler_last_at": None,
}


def reset() -> None:
    """测试用：清空运行态。"""
    RUNTIME.update(
        started_at=now_ms(),
        webhook_last_at=None,
        webhook_last_resource=None,
        iotda_token_expire_at=None,
        commands=[],
        scheduler_ticks=0,
        scheduler_last_at=None,
    )


def record_webhook(resource: str | None) -> None:
    RUNTIME["webhook_last_at"] = now_ms()
    RUNTIME["webhook_last_resource"] = resource


def record_command(command: str, request_id: str, device_sn: str, topic: str, paras: dict, mode: str) -> None:
    commands = RUNTIME["commands"]
    commands.append(
        {
            "command": command,
            "request_id": request_id,
            "device_sn": device_sn,
            "topic": topic,
            "paras": paras,
            "mode": mode,
            "at": now_ms(),
        }
    )
    del commands[:-_MAX_COMMANDS]


def last_command() -> dict | None:
    return RUNTIME["commands"][-1] if RUNTIME["commands"] else None
