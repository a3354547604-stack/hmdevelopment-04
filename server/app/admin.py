"""商家后台 `/admin`（P1；范围见 `赛题四-实施计划.md` §7.5）。

**这是给卖设备的人用的，不是给用户用的。** 用户端只有手机 App；这里管的是设备资产：
录了哪些台、哪台还没被绑、每台的绑定码是多少、服务器还活着没有。
所以：**用户家里的数据一概不进来**——没有读数、没有告警、没有在线节点数、没有房间名。

四个设计取舍：

1. **挂在现有 FastAPI 上，不单开前端服务。** 服务器只有 2 核 2G（实施计划 §7.1），多一个容器
   就多一份内存和一份运维面。
2. **不进 OpenAPI、不占 `/api` 前缀。** 全部路由 `include_in_schema=False`：App 与验收脚本看到的
   契约面（`/docs`、`/openapi.json`）不会多出后台接口；而 `deploy/nginx.prod.conf` 的
   `location / { return 404; }` 只放通 `/api/`，所以 `/admin` **天然不可达公网**——安全边界是
   既有配置，不靠本文件额外声明。
3. **未配置即不存在。** `ADMIN_PASSWORD` 留空则所有 `/admin` 路由整体 404，沿用
   `routers/vendor.py` 里 `PRODUCT_KEY` 的口径：不给探测者任何「有东西但你没权限」的提示。
4. **可写，但只写资产侧。** 录入设备、生成绑定码是商家自己的事；用户数据仍然只读。
   写入口只有 POST，且 cookie 是 `SameSite=Strict`，跨站表单带不上票。

绑定码为什么在后台生成：`routers/devices.py` 的 `new_bind_code()` 注释写着「生产由施工流程写入」，
而那条施工流程并不存在——出厂建档把 `bind_code` 写成 NULL，客户拿到机器绑不上。
本页面的「录入设备」就是那条缺失的施工流程：**录一台，发一个 6 位码，印在机器上随货走。**
"""
from __future__ import annotations

import hashlib
import logging
import re
import secrets
from datetime import date, datetime, timezone
from decimal import Decimal
from typing import Any

from fastapi import APIRouter, Form, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response

from . import db
from .config import settings
from .state import RUNTIME
from .timeutil import ms_to_dt, now_ms

log = logging.getLogger("app.admin")

router = APIRouter(include_in_schema=False)

COOKIE_NAME = "hm_admin"
_COOKIE_PATH = "/admin"

# SN 规则：与厂线接口（routers/vendor.py 的 SN_RE）同口径，额外放通 MAC 常见的冒号写法。
# 为什么放宽到允许 `:` —— 网关 SN 计划直接取网卡 MAC（形如 10-dc-b6-91-00-00 或 10:dc:b6:91:00:00），
# 两种写法现场都有人用；devices.sn 是 VARCHAR(32)，长度不构成风险。
_SN_RE = re.compile(r"^[A-Za-z0-9_.:-]{2,32}$")
_NAME_MAX = 64
_BULK_MAX = 200          # 一次最多录多少台：够产线一批，又不至于让一次请求跑几十秒


# --------------------------------------------------------------------------- 鉴权


def enabled() -> bool:
    return bool(settings.admin_password)


def _token() -> str:
    """由口令派生的固定会话令牌：改口令即让所有已登录浏览器失效，且服务端不存会话。"""
    return hashlib.sha256(f"hmprogram-admin-v1:{settings.admin_password}".encode()).hexdigest()


def _authed(request: Request) -> bool:
    if not enabled():
        return False
    got = request.cookies.get(COOKIE_NAME) or ""
    return secrets.compare_digest(got, _token())


def _guard(request: Request) -> Response | None:
    """未启用或未登录时统一的出口。返回 None 表示放行。"""
    if not enabled():
        return JSONResponse(status_code=404, content={"code": 1001, "message": "接口不存在", "data": None})
    if not _authed(request):
        return JSONResponse(status_code=401, content={"code": 1100, "message": "未登录", "data": None})
    return None


# --------------------------------------------------------------------------- 序列化


def _jsonable(value: Any) -> Any:
    """把数据库直出的类型收成 JSON 能表达的类型。

    MySQL 的 SUM/ROUND/COUNT 一律回 Decimal，DATETIME 回 datetime——两者都过不了 JSONResponse
    的序列化。与其在每个 SELECT 里逐个 CAST（漏一个就是一次 500），不如在出口统一收口。
    时间列一律 UTC（`db.py` 的 connect 钩子设了 `SET time_zone='+00:00'`），所以直接按 UTC 换算。
    """
    if isinstance(value, dict):
        return {k: _jsonable(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_jsonable(v) for v in value]
    if isinstance(value, Decimal):
        return int(value) if value == value.to_integral_value() else float(value)
    if isinstance(value, datetime):
        return int(value.replace(tzinfo=timezone.utc).timestamp() * 1000)
    if isinstance(value, date):
        return value.isoformat()
    if isinstance(value, (bytes, bytearray)):
        return value.decode("utf-8", "replace")
    return value


# --------------------------------------------------------------------------- 数据


def _collect() -> dict[str, Any]:
    """一次查询把页面要的东西全取出来。行数都在百级以内（P0 = 1 网关 4 节点）。"""
    now = now_ms()

    devices = db.fetch_all(
        "SELECT d.id AS device_id, d.sn, d.name, d.fw_ver, d.iotda_device_id, d.owner_id, "
        "       d.bind_code, d.bind_code_expire_at, d.created_at, "
        "       o.online, o.last_heartbeat_ms "
        "FROM devices d LEFT JOIN device_online o ON o.device_id = d.id "
        "ORDER BY d.id DESC"
    )
    for row in devices:
        # 状态只有两种（用户裁定）：已绑定 / 未激活。激活时间与在线状态另列，不混进状态里。
        row["bound"] = int(row.get("owner_id") or 0) != 0
        row["activated"] = row.get("online") is not None
        row["status"] = "bound" if row["bound"] else "inactive"

    # 库大小：信息模式里查，不扫业务表
    size_row = db.fetch_one(
        "SELECT ROUND(SUM(data_length + index_length) / 1048576, 1) AS mb "
        "FROM information_schema.tables WHERE table_schema = DATABASE()"
    ) or {}
    # 读数总量与最后一条：走主键倒序取最后一条，而不是 MAX(ts_ms) 扫全表
    readings_total = db.fetch_scalar("SELECT COUNT(*) FROM sensor_readings", default=0)
    last_reading = db.fetch_one("SELECT ts_ms FROM sensor_readings ORDER BY id DESC LIMIT 1")
    last_ts = int(last_reading["ts_ms"]) if last_reading and last_reading.get("ts_ms") else None

    return {
        "server_ts": now,
        "service": {
            "version": settings.version,
            "app_env": settings.app_env,
            "db": "ok" if db.ping() else "down",
            "clock_source": settings.clock_source,
            # 进程内值：重启即清零，所以只作参考，不能当「多久没数据」的判据
            "webhook_last_at": RUNTIME.get("webhook_last_at"),
            "started_at": RUNTIME.get("started_at"),
        },
        "stats": {
            "devices_total": len(devices),
            "devices_bound": sum(1 for d in devices if d["bound"]),
            "db_size_mb": size_row.get("mb"),
            "readings_total": int(readings_total or 0),
            "last_reading_ms": last_ts,
            "last_reading_age_ms": (now - last_ts) if last_ts else None,
        },
        "devices": devices,
    }


def _new_bind_code() -> tuple[str, datetime]:
    """6 位数字码 + 到期时刻。码按台发，不需要全库唯一（绑定靠 SN+码 两个条件）。"""
    code = f"{secrets.randbelow(1_000_000):06d}"
    return code, ms_to_dt(now_ms() + settings.bind_code_ttl_days * 24 * 60 * 60 * 1000)


# --------------------------------------------------------------------------- 路由


@router.get("/admin")
def page(request: Request) -> Response:
    if not enabled():
        return JSONResponse(status_code=404, content={"code": 1001, "message": "接口不存在", "data": None})
    if not _authed(request):
        return HTMLResponse(
            _LOGIN_HTML.replace("__VERSION__", settings.version).replace("__ERROR__", ""),
            status_code=200,
        )
    return HTMLResponse(_PAGE_HTML.replace("__VERSION__", settings.version))


@router.post("/admin/login")
def login(request: Request, password: str = Form("")) -> Response:
    if not enabled():
        return JSONResponse(status_code=404, content={"code": 1001, "message": "接口不存在", "data": None})
    if not secrets.compare_digest(password, settings.admin_password):
        log.warning("商家后台登录失败（来源 %s）", request.client.host if request.client else "?")
        html = _LOGIN_HTML.replace("__VERSION__", settings.version).replace(
            "__ERROR__", '<p class="err">口令不对</p>'
        )
        return HTMLResponse(html, status_code=401)
    resp = RedirectResponse(url="/admin", status_code=303)
    # 不设 max-age：关浏览器即失效；HttpOnly 让页面脚本碰不到；SameSite 断掉跨站自动带票
    resp.set_cookie(COOKIE_NAME, _token(), path=_COOKIE_PATH, httponly=True, samesite="strict")
    return resp


@router.post("/admin/logout")
def logout() -> Response:
    resp = RedirectResponse(url="/admin", status_code=303)
    resp.delete_cookie(COOKIE_NAME, path=_COOKIE_PATH)
    return resp


@router.get("/admin/data")
def data(request: Request) -> Response:
    denied = _guard(request)
    if denied is not None:
        return denied
    try:
        payload = _collect()
    except Exception as exc:  # noqa: BLE001 —— 库挂了要在页面上看见原因，而不是白屏
        log.exception("商家后台取数失败")
        return JSONResponse(
            status_code=503,
            content={"code": 1500, "message": f"取数失败：{exc.__class__.__name__}", "data": None},
        )
    return JSONResponse(content=_jsonable(payload))


@router.post("/admin/devices")
async def create_devices(request: Request) -> Response:
    """出厂录入：新建设备并生成 6 位绑定码。已存在的 SN 不报错，缺码就补一个。

    幂等口径与 `routers/vendor.py` 的 `POST /api/device/activate` 一致：
    重复提交同一 SN 不算失败，返回已有记录 + 当前绑定码。产线上多点一次不该是错误。
    """
    denied = _guard(request)
    if denied is not None:
        return denied
    try:
        payload = await request.json()
    except Exception:  # noqa: BLE001
        return JSONResponse(status_code=400, content={"code": 1001, "message": "请求体不是合法 JSON", "data": None})

    raw_items = payload.get("items") if isinstance(payload, dict) else None
    if not isinstance(raw_items, list) or not raw_items:
        return JSONResponse(status_code=400, content={"code": 1001, "message": "items 不能为空", "data": None})
    if len(raw_items) > _BULK_MAX:
        return JSONResponse(
            status_code=400,
            content={"code": 1001, "message": f"一次最多 {_BULK_MAX} 台", "data": None},
        )

    created: list[dict[str, Any]] = []
    existing: list[dict[str, Any]] = []
    failed: list[dict[str, Any]] = []

    for item in raw_items:
        if not isinstance(item, dict):
            failed.append({"sn": None, "reason": "条目格式不对"})
            continue
        sn = str(item.get("sn") or "").strip()
        if not _SN_RE.match(sn):
            failed.append({"sn": sn, "reason": "SN 只能 2–32 位字母数字与 _ . : -"})
            continue
        name = str(item.get("name") or "").strip() or sn
        if len(name) > _NAME_MAX:
            failed.append({"sn": sn, "reason": f"名称超长（≤{_NAME_MAX}）"})
            continue

        row = db.fetch_one(
            "SELECT id, sn, bind_code, owner_id FROM devices WHERE sn = :sn", {"sn": sn}
        )
        if row is not None:
            # 已存在：能补码就补（出厂建档写的是 NULL），不能补就把现状报回去
            if not row["bind_code"] and int(row["owner_id"] or 0) == 0:
                code, expire = _new_bind_code()
                db.execute(
                    "UPDATE devices SET bind_code = :c, bind_code_expire_at = :e WHERE id = :d",
                    {"c": code, "e": expire, "d": row["id"]},
                )
                existing.append({"sn": row["sn"], "device_id": int(row["id"]), "bind_code": code, "filled": True})
            else:
                existing.append(
                    {
                        "sn": row["sn"],
                        "device_id": int(row["id"]),
                        "bind_code": row["bind_code"],
                        "filled": False,
                    }
                )
            continue

        code, expire = _new_bind_code()
        try:
            device_id = db.insert(
                "INSERT INTO devices (sn, name, bind_code, bind_code_expire_at, iotda_device_id, owner_id) "
                "VALUES (:sn, :name, :code, :expire, NULL, 0)",
                {"sn": sn, "name": name, "code": code, "expire": expire},
            )
        except Exception:  # noqa: BLE001 —— 并发重复录入：唯一键冲突按「已存在」处理
            raced = db.fetch_one("SELECT id, sn, bind_code FROM devices WHERE sn = :sn", {"sn": sn})
            if raced:
                existing.append(
                    {"sn": raced["sn"], "device_id": int(raced["id"]), "bind_code": raced["bind_code"], "filled": False}
                )
                continue
            raise
        created.append({"sn": sn, "device_id": int(device_id), "bind_code": code})

    log.info("商家后台录入：新建 %d，已存在 %d，失败 %d", len(created), len(existing), len(failed))
    return JSONResponse(
        content=_jsonable({"created": created, "existing": existing, "failed": failed})
    )


@router.post("/admin/devices/{device_id}/bind-code")
def reset_bind_code(request: Request, device_id: int) -> Response:
    """重发绑定码：贴错标签、码被泄露、客户弄丢了机器上的贴纸时用。"""
    denied = _guard(request)
    if denied is not None:
        return denied
    row = db.fetch_one("SELECT id, sn, owner_id FROM devices WHERE id = :d", {"d": device_id})
    if row is None:
        return JSONResponse(status_code=404, content={"code": 1201, "message": "设备不存在", "data": None})
    if int(row["owner_id"] or 0) != 0:
        # 已绑定的设备重发码没有意义：绑定码是一次性的，绑完就清空；
        # 真要换主人得走「解绑」，那是另一件事，不在本页面范围。
        return JSONResponse(
            status_code=409,
            content={"code": 1202, "message": "这台已绑定，重发绑定码无效（需要先解绑）", "data": None},
        )
    code, expire = _new_bind_code()
    db.execute(
        "UPDATE devices SET bind_code = :c, bind_code_expire_at = :e WHERE id = :d",
        {"c": code, "e": expire, "d": device_id},
    )
    log.info("商家后台重发绑定码：SN=%s", row["sn"])
    return JSONResponse(content=_jsonable({"sn": row["sn"], "bind_code": code}))


# --------------------------------------------------------------------------- 页面

_LOGIN_HTML = """<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>商家后台 · 登录</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; min-height:100vh; display:grid; place-items:center;
         background:#0b0f14; color:#e6edf3;
         font:15px/1.5 system-ui,-apple-system,"Segoe UI","Noto Sans SC",sans-serif; }
  .card { width:320px; padding:28px 26px; border:1px solid #1e2936; border-radius:14px;
          background:#121821; box-shadow:0 18px 48px rgba(0,0,0,.5); }
  h1 { margin:0 0 4px; font-size:17px; font-weight:600; letter-spacing:.3px; }
  p.sub { margin:0 0 20px; color:#7d8b9c; font-size:12.5px; }
  input { width:100%; padding:10px 12px; margin-bottom:14px; border-radius:9px;
          border:1px solid #26313f; background:#0d131a; color:#e6edf3; font-size:14px; }
  input:focus { outline:none; border-color:#3ddc84; }
  button { width:100%; padding:10px; border:0; border-radius:9px; cursor:pointer;
           background:#3ddc84; color:#08120c; font-weight:600; font-size:14px; }
  button:hover { background:#4de894; }
  .err { margin:0 0 12px; color:#ff7b7b; font-size:13px; }
  footer { margin-top:18px; color:#54616f; font-size:11.5px; line-height:1.6; }
</style></head>
<body><form class="card" method="post" action="/admin/login">
  <h1>居家安全监测 · 商家后台</h1>
  <p class="sub">设备资产台账 · 不对外网开放</p>
  __ERROR__
  <input type="password" name="password" placeholder="后台口令" autofocus autocomplete="current-password">
  <button type="submit">进入</button>
  <footer>v__VERSION__ · 通过 SSH 隧道或局域网访问</footer>
</form></body></html>"""


_PAGE_HTML = """<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>商家后台 · 居家安全监测</title>
<style>
  :root {
    color-scheme: dark;
    --bg:#0b0f14; --panel:#121821; --line:#1e2936; --ink:#e6edf3; --dim:#7d8b9c; --faint:#54616f;
    --ok:#3ddc84; --warn:#ffb020; --bad:#ff5c5c; --info:#4aa8ff;
  }
  * { box-sizing:border-box; }
  body { margin:0; padding:22px 26px 40px; background:var(--bg); color:var(--ink);
         font:14px/1.55 system-ui,-apple-system,"Segoe UI","Noto Sans SC",sans-serif; }
  .mono { font-family:ui-monospace,SFMono-Regular,"Cascadia Mono",Consolas,monospace;
          font-variant-numeric:tabular-nums; }
  header { display:flex; align-items:baseline; gap:14px; flex-wrap:wrap; margin-bottom:18px; }
  header h1 { margin:0; font-size:17px; font-weight:600; letter-spacing:.3px; }
  header .meta { color:var(--faint); font-size:12px; }
  header .spacer { flex:1; }
  .pill { padding:3px 9px; border-radius:999px; font-size:11.5px; border:1px solid var(--line); color:var(--dim); }
  .pill b { color:var(--ink); font-weight:600; }
  .btn { padding:5px 11px; border-radius:8px; border:1px solid var(--line); background:#161d27;
         color:var(--dim); font-size:12px; cursor:pointer; text-decoration:none; }
  .btn:hover { border-color:#2c3a4c; color:var(--ink); }
  .btn.primary { background:#3ddc84; border-color:#3ddc84; color:#08120c; font-weight:600; }
  .btn.primary:hover { background:#4de894; }
  .btn:disabled { opacity:.5; cursor:default; }

  .kpis { display:grid; grid-template-columns:repeat(auto-fit,minmax(160px,1fr)); gap:12px; margin-bottom:16px; }
  .kpi { padding:14px 16px; border:1px solid var(--line); border-radius:12px; background:var(--panel); }
  .kpi .k { color:var(--dim); font-size:12px; margin-bottom:6px; }
  .kpi .v { font-size:22px; font-weight:600; }
  .kpi .v small { font-size:13px; color:var(--faint); font-weight:400; }

  section { margin-bottom:16px; border:1px solid var(--line); border-radius:12px;
            background:var(--panel); overflow:hidden; }
  section > h2 { margin:0; padding:11px 16px; font-size:12.5px; font-weight:600; color:var(--dim);
                 letter-spacing:.6px; border-bottom:1px solid var(--line); background:#0f151d;
                 display:flex; gap:10px; align-items:baseline; flex-wrap:wrap; }
  section > h2 span { font-weight:400; color:var(--faint); font-size:11.5px; letter-spacing:0; }
  table { width:100%; border-collapse:collapse; }
  th, td { padding:9px 14px; text-align:left; font-size:13px; border-bottom:1px solid #161e28;
           white-space:nowrap; }
  th { color:var(--faint); font-weight:500; font-size:11.5px; letter-spacing:.4px; background:#0d131a; }
  tr:last-child td { border-bottom:0; }
  tbody tr:hover { background:#151d27; }
  .on { color:var(--ok); } .off { color:var(--bad); } .idle { color:var(--faint); }
  .tag { display:inline-block; padding:1px 8px; border-radius:6px; font-size:11.5px;
         border:1px solid var(--line); color:var(--dim); }
  .tag.bound { color:var(--ok); border-color:#1d4d36; }
  .tag.inactive { color:var(--faint); }
  .code { font-size:15px; font-weight:600; letter-spacing:1.5px; color:var(--info); }
  .code.none { color:var(--faint); font-weight:400; letter-spacing:0; font-size:12.5px; }
  .empty { padding:16px; color:var(--faint); font-size:13px; }

  .entry { padding:16px; display:flex; gap:12px; flex-wrap:wrap; align-items:flex-start; }
  .entry textarea { flex:1 1 320px; min-height:92px; padding:10px 12px; border-radius:9px;
                    border:1px solid #26313f; background:#0d131a; color:var(--ink);
                    font:13px/1.5 ui-monospace,SFMono-Regular,Consolas,monospace; resize:vertical; }
  .entry textarea:focus { outline:none; border-color:#3ddc84; }
  .entry .side { display:flex; flex-direction:column; gap:8px; min-width:180px; }
  .entry input { padding:9px 11px; border-radius:9px; border:1px solid #26313f;
                 background:#0d131a; color:var(--ink); font-size:13px; }
  .entry input:focus { outline:none; border-color:#3ddc84; }
  .hint { color:var(--faint); font-size:12px; line-height:1.6; }
  .result { padding:0 16px 16px; font-size:13px; }
  .result table { margin-top:8px; border:1px solid var(--line); border-radius:9px; overflow:hidden; }
  .result .ok { color:var(--ok); } .result .bad { color:var(--bad); } .result .warn { color:var(--warn); }
</style></head>
<body>
<header>
  <h1>居家安全监测 · 商家后台</h1>
  <span class="meta mono" id="clock">—</span>
  <span class="spacer"></span>
  <span class="pill">v__VERSION__ · <b id="env">—</b> · 库 <b id="db">—</b></span>
  <button class="btn" id="btn-refresh" type="button">刷新</button>
  <form method="post" action="/admin/logout" style="display:inline">
    <button class="btn" type="submit">退出</button>
  </form>
</header>

<div class="kpis">
  <div class="kpi"><div class="k">设备总数</div><div class="v" id="k-total">—</div></div>
  <div class="kpi"><div class="k">已绑定</div><div class="v" id="k-bound">—</div></div>
  <div class="kpi"><div class="k">未激活</div><div class="v" id="k-inactive">—</div></div>
  <div class="kpi"><div class="k">最后收到数据</div><div class="v" id="k-last" style="font-size:15px">—</div></div>
  <div class="kpi"><div class="k">库大小</div><div class="v" id="k-size">—</div></div>
  <div class="kpi"><div class="k">读数总量</div><div class="v" id="k-rows">—</div></div>
</div>

<section>
  <h2>出厂录入 <span>一行一个 SN；录入即生成 6 位绑定码，印在机器上随货发</span></h2>
  <div class="entry">
    <textarea id="sn-input" placeholder="10-dc-b6-91-00-00&#10;10-dc-b6-91-00-01"></textarea>
    <div class="side">
      <input id="name-input" placeholder="名称（可空，默认同 SN）" autocomplete="off">
      <button class="btn primary" id="btn-create" type="button">录入并生成绑定码</button>
      <div class="hint">已存在的 SN 不报错：缺码会补一个，有码就原样返回。</div>
    </div>
  </div>
  <div class="result" id="create-result"></div>
</section>

<section>
  <h2>设备台账 <span id="dev-note"></span></h2>
  <table><thead><tr>
    <th>SN</th><th>名称</th><th>状态</th><th>绑定码</th><th>在线</th>
    <th>最后活跃</th><th>固件</th><th>出厂时间</th><th></th>
  </tr></thead><tbody id="dev-body"><tr><td colspan="9" class="empty">—</td></tr></tbody></table>
</section>

<script>
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g,
  (c) => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[c]));
const pad = (n) => String(n).padStart(2, '0');

function fmtTime(ms) {
  if (!ms) return '—';
  const d = new Date(Number(ms));
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}
function fmtAge(ms) {
  if (ms == null) return '—';
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return s + ' 秒前';
  const m = Math.round(s / 60);
  if (m < 60) return m + ' 分钟前';
  const h = Math.floor(m / 60);
  if (h < 24) return h + ' 小时前';
  return Math.floor(h / 24) + ' 天前';
}

async function load() {
  let d;
  try {
    const r = await fetch('/admin/data', { cache: 'no-store' });
    if (r.status === 401) { location.reload(); return; }
    d = await r.json();
    if (!r.ok) throw new Error(d.message || ('HTTP ' + r.status));
  } catch (e) {
    $('clock').textContent = '取数失败：' + e.message;
    return;
  }

  const now = Number(d.server_ts);
  $('clock').textContent = '服务器时间 ' + fmtTime(now);
  $('env').textContent = esc(d.service.app_env);
  $('db').textContent = esc(d.service.db);
  $('db').className = d.service.db === 'ok' ? 'on' : 'off';

  const s = d.stats;
  $('k-total').textContent = s.devices_total;
  $('k-bound').textContent = s.devices_bound;
  $('k-inactive').textContent = s.devices_total - s.devices_bound;
  $('k-last').textContent = s.last_reading_ms
    ? fmtAge(s.last_reading_age_ms) + ' · ' + fmtTime(s.last_reading_ms)
    : '从未收到';
  $('k-size').innerHTML = s.db_size_mb == null ? '—' : s.db_size_mb + '<small> MB</small>';
  $('k-rows').textContent = Number(s.readings_total).toLocaleString();

  const rows = d.devices.map((g) => {
    const bound = !!g.bound;
    const online = g.online === null || g.online === undefined ? null : !!g.online;
    const onlineCell = online === null
      ? '<td class="idle">—</td>'
      : `<td class="${online ? 'on' : 'off'}">${online ? '在线' : '离线'}</td>`;
    const code = g.bind_code
      ? `<td><span class="code">${esc(g.bind_code)}</span></td>`
      : `<td><span class="code none">${bound ? '已用完' : '未生成'}</span></td>`;
    const action = bound
      ? '<td class="idle">—</td>'
      : `<td><button class="btn" data-reset="${Number(g.device_id)}" data-sn="${esc(g.sn)}" type="button">重发码</button></td>`;
    return `<tr>
      <td class="mono">${esc(g.sn)}</td>
      <td>${esc(g.name)}</td>
      <td><span class="tag ${bound ? 'bound' : 'inactive'}">${bound ? '已绑定' : '未激活'}</span></td>
      ${code}
      ${onlineCell}
      <td class="mono">${fmtAge(g.last_heartbeat_ms ? now - Number(g.last_heartbeat_ms) : null)}</td>
      <td class="mono">${esc(g.fw_ver) || '—'}</td>
      <td class="mono">${fmtTime(g.created_at)}</td>
      ${action}
    </tr>`;
  });
  $('dev-body').innerHTML = rows.length
    ? rows.join('')
    : '<tr><td colspan="9" class="empty">还没有录入任何设备：在上面填 SN 录一台</td></tr>';
  $('dev-note').textContent = `共 ${s.devices_total} 台 · 在线列只看得到「是否连着服务器」，不涉及用户数据`;
}

async function postJSON(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (r.status === 401) { location.reload(); throw new Error('未登录'); }
  const d = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(d.message || ('HTTP ' + r.status));
  return d;
}

$('btn-create').addEventListener('click', async () => {
  const lines = $('sn-input').value.split('\\n').map((x) => x.trim()).filter(Boolean);
  if (!lines.length) { $('create-result').innerHTML = '<p class="bad">先填至少一个 SN</p>'; return; }
  const name = $('name-input').value.trim();
  const items = lines.map((sn, i) => ({ sn, name: i === 0 ? name : '' }));
  $('btn-create').disabled = true;
  try {
    const d = await postJSON('/admin/devices', { items });
    const parts = [];
    const line = (list, cls, label, codeOf) => list.forEach((x) => {
      parts.push(`<tr><td class="${cls}">${label}</td><td class="mono">${esc(x.sn)}</td>` +
                 `<td><span class="code">${esc(codeOf(x) || '—')}</span></td></tr>`);
    });
    line(d.created, 'ok', '新建', (x) => x.bind_code);
    line(d.existing, 'warn', '已存在', (x) => x.bind_code);
    d.failed.forEach((x) => parts.push(
      `<tr><td class="bad">失败</td><td class="mono">${esc(x.sn) || '—'}</td>` +
      `<td class="bad">${esc(x.reason)}</td></tr>`));
    $('create-result').innerHTML = parts.length
      ? `<table><thead><tr><th>结果</th><th>SN</th><th>绑定码</th></tr></thead><tbody>${parts.join('')}</tbody></table>`
      : '<p class="bad">没有任何结果</p>';
    $('sn-input').value = '';
    load();
  } catch (e) {
    $('create-result').innerHTML = `<p class="bad">录入失败：${esc(e.message)}</p>`;
  } finally {
    $('btn-create').disabled = false;
  }
});

$('dev-body').addEventListener('click', async (ev) => {
  const btn = ev.target.closest('button[data-reset]');
  if (!btn) return;
  const id = btn.getAttribute('data-reset');
  const sn = btn.getAttribute('data-sn');
  if (!confirm('给 ' + sn + ' 重新生成绑定码？旧码立即失效。')) return;
  btn.disabled = true;
  try {
    const d = await postJSON('/admin/devices/' + id + '/bind-code', {});
    $('create-result').innerHTML =
      `<table><thead><tr><th>结果</th><th>SN</th><th>绑定码</th></tr></thead><tbody>` +
      `<tr><td class="ok">重发</td><td class="mono">${esc(d.sn)}</td>` +
      `<td><span class="code">${esc(d.bind_code)}</span></td></tr></tbody></table>`;
    load();
  } catch (e) {
    $('create-result').innerHTML = `<p class="bad">重发失败：${esc(e.message)}</p>`;
    btn.disabled = false;
  }
});

load();
setInterval(load, 10000);
$('btn-refresh').addEventListener('click', load);
</script>
</body></html>"""
