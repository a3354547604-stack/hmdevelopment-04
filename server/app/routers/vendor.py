"""厂商侧设备出厂注册（t152）。

**为什么需要这个接口**：`devices` 行原本只有 `seed` 会造，而公网服务器是 `APP_ENV=prod`、
跑不了 `seed --demo`（防止把手滑清库的演示数据灌进生产）。结果就是：网关经 IoTDA 转发上来的数据
`_resolve_device()` 查不到 `devices.sn` → 落库时标 `warnings: ["device_not_found"]`，App 也无从绑定。
本接口让「出厂即建档」成为可能：厂商在产线/发货前用产品密钥调一次，服务器就把 SN 记进 `devices`。

**权限模型**：这不是用户 API —— 不走 `Authorization: Bearer`，只认共享密钥头 `X-Product-Key`
（环境变量 `PRODUCT_KEY`）。未配置 `PRODUCT_KEY` 时整个路由按 **404** 处理（与「没有这个路由」
不可区分），避免暴露接口存在性；配置后错 key → 401/1501、SN 非法 → 400/1201。

**幂等**：同 SN 重复注册返回首次的 `device_id` 且 `created=false`，绝不新建第二行
（`devices.sn` 有唯一键，重复插入会 1062；先用 SELECT 再 INSERT 也挡不住并发，故两条路都兜）。
"""
from __future__ import annotations

import hmac
import logging
import re

from fastapi import APIRouter, Header, Request
from fastapi.exceptions import HTTPException
from pydantic import BaseModel

from .. import db
from ..config import settings
from ..errors import ApiError, ok
from ..security import activate_limiter

log = logging.getLogger("app.vendor")
router = APIRouter(prefix="/api/device", tags=["vendor"])

# 厂线 SN 规则：2–32。**用户侧**（`devices.py:28` 的 `SN_RE`）仍是 `{3,32}`——两处不是同一条规则：
# 网关真实出厂 SN 是短编号 `M0`（`rk2206-gateway/src/gw_config.h:14` `#define GW_SN "M0"`），
# 2 字符必须能过，否则网关第一次上云就 400/1201、`devices` 行永远建不出来（t157 修的就是这条）；
# 而用户侧绑的是**已出厂的设备**、靠 6 位绑定码，没有放宽的理由。
SN_RE = re.compile(r"^[A-Za-z0-9_-]{2,32}$")
SN_RULE_HINT = "^[A-Za-z0-9_-]{2,32}$"  # 回给调用方的 expected 串，必须与 SN_RE 同步
NAME_MAX = 64  # devices.name VARCHAR(64)


class ActivateIn(BaseModel):
    sn: str
    name: str | None = None


def _product_key() -> str:
    """当前配置的产品密钥；空串 = 接口关闭（404）。测试通过 monkeypatch 本函数切到关闭态。"""
    return settings.product_key


def _client_ip(request: Request) -> str:
    return (request.client.host if request.client else "unknown") or "unknown"


@router.post("/activate")
def activate_device(payload: ActivateIn, request: Request,
                    x_product_key: str | None = Header(default=None, alias="X-Product-Key")) -> dict:
    """出厂建档：`{sn, name?}` → `{device_id, sn, created}`。

    HTTP 状态码统一 200（成功体里用 `created` 区分新建/复用）：幂等重放时若首次回 201、
    重放回 200，调用方会误以为两次结果不同；契约 §6.1 也只用 `code=0` 表达成功。
    """
    expected = _product_key()
    if not expected:
        # 与未知路由完全一致（框架 404 → {code:1001,...}），不泄露「此接口存在但没配密钥」
        raise HTTPException(status_code=404, detail="Not Found")

    activate_limiter.check(_client_ip(request))

    supplied = x_product_key or ""
    # 定长比较：避免按字节提前返回泄露密钥前缀（与 webhook token 的校验口径一致）
    if not supplied or not hmac.compare_digest(supplied, expected):
        # 卡面（t152）指定 401/1501：1501 在 §9.2 的 message 是「上游 IoTDA 不可用」，
        # 这里借用它的码位表达「厂商侧鉴权失败」，语义细节等契约 r7 定稿后再收口
        raise ApiError(1501, http_status=401, field="X-Product-Key")

    sn = (payload.sn or "").strip()
    if not SN_RE.match(sn):
        raise ApiError(1201, http_status=400, field="sn", expected=SN_RULE_HINT)

    name = (payload.name or "").strip() or sn
    if len(name) > NAME_MAX:
        raise ApiError(1002, field="name", allowed=NAME_MAX)

    existing = db.fetch_one("SELECT id, sn FROM devices WHERE sn = :sn", {"sn": sn})
    if existing:
        log.info("出厂注册：SN=%s 已存在（device_id=%s），幂等返回", sn, existing["id"])
        return ok({"device_id": int(existing["id"]), "sn": existing["sn"], "created": False})

    try:
        device_id = db.insert(
            "INSERT INTO devices (sn, name, bind_code, bind_code_expire_at, iotda_device_id, owner_id) "
            "VALUES (:sn, :name, NULL, NULL, NULL, 0)",
            {"sn": sn, "name": name},
        )
    except Exception:  # noqa: BLE001 —— 并发重复注册：唯一键冲突时按幂等返回已有行
        raced = db.fetch_one("SELECT id, sn FROM devices WHERE sn = :sn", {"sn": sn})
        if raced:
            log.info("出厂注册：SN=%s 并发重复，按幂等返回 device_id=%s", sn, raced["id"])
            return ok({"device_id": int(raced["id"]), "sn": raced["sn"], "created": False})
        raise

    # `iotda_device_id` 留空：平台侧 device_id 由华为云控制台分配，出厂时还不知道；
    # webhook 侧 `_resolve_device()` 已支持 header.node_id = SN 的兼容匹配，故不阻塞落库
    log.info("出厂注册：SN=%s 新建 device_id=%s（owner_id=0 未绑定）", sn, device_id)
    return ok({"device_id": int(device_id), "sn": sn, "created": True})
