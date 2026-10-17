"""下发通道：服务器 → 华为云 IoTDA → 网关（契约 §4.2/§4.5/§6.11/§6.12）。

两种模式（`IOTDA_DISPATCH_MODE`）：
- `mock`（默认）：不真的调华为云，只把命令写进 `state.RUNTIME["commands"]` 流水并记日志。
  离线开发、pytest、答辩演示都在这个模式；**mock 下 `demo_trigger` 由 `app/mocks/virtual.py`
  生成的虚拟网关把结果回灌到真实 webhook 处理链路**，因此告警/报表仍然走 §5 的真代码。
- `real`：用 IAM 账号取 token，再调 IoTDA 应用侧「下发设备命令」接口。
  ⚠️ 待用户实测（CAL-04）：仓库内没有真实凭证，本路径未联调，仅按官方接口约定实现。

契约空缺登记（已报 PM，不自行发明语义之外的字段）：
§4.5 的命令表没有「下发阈值配置」的命令名，而 §6.11 要求把 settings 下发到设备。
本实现取 §3.4 `0x11` set 帧里的 `cmd` 名：`report_interval_s` 用 §4.5 已定义
`set_report_interval`，其余键逐条用 `set_threshold {"key":k,"value":v}`（与 UART 帧一一对应）。
"""
from __future__ import annotations

import logging
import time
import uuid

import requests

from ..config import settings
from ..errors import ApiError
from ..state import RUNTIME, record_command

log = logging.getLogger("app.iotda")

COMMAND_TOPIC = "$oc/devices/{device_id}/sys/commands/request_id={request_id}"
COMMAND_RESPONSE_TOPIC = "$oc/devices/{device_id}/sys/commands/response/request_id={request_id}"
IAM_TOKEN_PATH = "/v3/auth/tokens"
COMMAND_TIMEOUT_S = 10  # §6.11 响应里的 timeout_s
PAIR_COMMAND_TIMEOUT_S = 20  # §4.5：pair_* 命令超时按 20 s

# 契约 §4.5 里官方命名的命令 → 使用的下发方式
_OFFICIAL_COMMANDS = {
    "report_interval_s": ("set_report_interval", lambda value: {"seconds": int(value)}),
}


def new_request_id() -> str:
    """§6.11 样例 request_id 为 6 位十六进制。"""
    return uuid.uuid4().hex[:6]


def command_topic(device: dict, request_id: str) -> str:
    """§4.2：topic 里 request_id 是**路径一部分**（`.../commands/request_id={id}`），两个占位符都要填。"""
    device_id = device.get("iotda_device_id") or device["sn"]
    return COMMAND_TOPIC.format(device_id=device_id, request_id=request_id)


def build_command(command: str, paras: dict, command_id: str) -> dict:
    """物模型命令报文（§4.6 的下行结构：服务 `gateway` + 命令名 + paras）。"""
    return {"service_id": "gateway", "command_name": command, "command_id": command_id, "paras": paras}


def _iam_token() -> str:
    """IAM 取 token（企业账号用 domain 名，账号名与用户名一致时同样适用）。"""
    if not (settings.iotda_endpoint and settings.iotda_iam_user and settings.iotda_iam_password):
        raise ApiError(1501, note="未配置 IOTDA_ENDPOINT/IOTDA_IAM_*，real 模式无法下发")
    domain = settings.iotda_iam_domain or settings.iotda_iam_user
    # IoTDA 与 IAM 同区域：https://iotda.cn-north-4.myhuaweicloud.com → https://iam.cn-north-4.myhuaweicloud.com
    iam_endpoint = settings.iotda_endpoint.replace("//iotda.", "//iam.")
    url = f"{iam_endpoint.rstrip('/')}{IAM_TOKEN_PATH}"
    payload = {
        "auth": {
            "identity": {
                "methods": ["password"],
                "password": {
                    "user": {
                        "name": settings.iotda_iam_user,
                        "password": settings.iotda_iam_password,
                        "domain": {"name": domain},
                    }
                },
            },
            "scope": {"project": {"name": settings.iotda_project_id}},
        }
    }
    try:
        resp = requests.post(url, json=payload, timeout=8)
    except requests.RequestException as exc:
        raise ApiError(1501, note=f"IAM 请求失败：{exc}") from exc
    if resp.status_code not in (200, 201):
        raise ApiError(1501, note=f"IAM 返回 {resp.status_code}")
    token = resp.headers.get("X-Subject-Token", "")
    if not token:
        raise ApiError(1501, note="IAM 未返回 X-Subject-Token")
    RUNTIME["iotda_token_expire_at"] = int(time.time() * 1000) + 3600 * 1000
    return token


def _publish_real(device: dict, command: str, paras: dict, request_id: str, command_id: str) -> None:
    if not (settings.iotda_endpoint and settings.iotda_project_id and settings.iotda_app_id):
        raise ApiError(1501, note="未配置 IOTDA_ENDPOINT/PROJECT_ID/APP_ID")
    token = _iam_token()
    device_id = device.get("iotda_device_id")
    if not device_id:
        raise ApiError(1205, note="设备缺少 iotda_device_id，无法下发")
    url = (
        f"{settings.iotda_endpoint.rstrip('/')}/v5/iot/{settings.iotda_project_id}"
        f"/devices/{device_id}/commands"
    )
    body = {
        "service_id": "gateway",
        "command_name": command,
        "command_id": command_id,
        "paras": paras,
    }
    try:
        resp = requests.post(
            url,
            json=body,
            headers={"X-Auth-Token": token, "Content-Type": "application/json"},
            timeout=8,
        )
    except requests.RequestException as exc:
        raise ApiError(1501, note=f"IoTDA 请求失败：{exc}") from exc
    if resp.status_code >= 300:
        # 契约 §9.2：IoTDA 应用侧返回错误 → 1205（502）
        raise ApiError(1205, note=f"IoTDA 返回 {resp.status_code}: {resp.text[:200]}")


def dispatch(device: dict, command: str, paras: dict, timeout_s: int = COMMAND_TIMEOUT_S) -> dict:
    """下发一条命令；返回 {request_id, command_id, topic, paras, mode, dispatched_at, timeout_s}。

    只要不在 real 模式或 mock 模式成功，都**不**返回「已成功」字样（§6.11 的硬要求）。
    """
    request_id = new_request_id()
    command_id = f"cmd-{int(time.time() * 1000)}-{request_id[:1]}"
    topic = command_topic(device, request_id)
    mode = settings.iotda_dispatch_mode
    if mode == "real":
        _publish_real(device, command, paras, request_id, command_id)
    else:
        log.info(
            "[mock] 下发 %s → %s topic=%s paras=%s request_id=%s",
            command,
            device["sn"],
            topic,
            paras,
            request_id,
        )
    record_command(command, request_id, device["sn"], topic, paras, mode)
    return {
        "command_id": command_id,
        "request_id": request_id,
        "topic": topic,
        "command": command,
        "paras": paras,
        "mode": mode,
        "timeout_s": timeout_s,
        "dispatched_at": int(time.time() * 1000),
    }


def dispatch_settings_key(device: dict, key: str, value) -> dict:
    """单个设置项 → 一条命令（与 §3.4 `0x11` set 帧一一对应）。"""
    if key in _OFFICIAL_COMMANDS:
        command, build = _OFFICIAL_COMMANDS[key]
        return dispatch(device, command, build(value))
    return dispatch(device, "set_threshold", {"cmd": "set_threshold", "key": key, "value": value})
