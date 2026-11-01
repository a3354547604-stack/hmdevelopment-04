"""只读运维后台 `/admin`（P1；范围见 `赛题四-实施计划.md` §7.5）。

为什么要这个页面：t150 把阈值/演示/自检入口从 App 主界面移除了（规范：普通用户不调阈值），
当时的口径是「改由运维侧接口验证」——接口有了，入口没有。想看「设备在不在线、最后一条数据
是几分钟前」，此前只能连数据库或 curl。

三个设计取舍：

1. **挂在现有 FastAPI 上，不单开前端服务。** 服务器只有 2 核 2G（实施计划 §7.1），多一个容器
   就多一份内存和一份运维面。
2. **不进 OpenAPI、不占 `/api` 前缀。** 全部路由 `include_in_schema=False`：App 与验收脚本看到的
   契约面（`/docs`、`/openapi.json`）不会多出运维接口；而 `deploy/nginx.prod.conf` 的
   `location / { return 404; }` 只放通 `/api/`，所以 `/admin` **天然不可达公网**——安全边界是
   既有配置，不靠本文件额外声明。
3. **未配置即不存在。** `ADMIN_PASSWORD` 留空则所有 `/admin` 路由整体 404，沿用
   `routers/vendor.py` 里 `PRODUCT_KEY` 的口径：不给探测者任何「有东西但你没权限」的提示。

**只读。** 写操作（下发命令、改阈值、注入演示）在 App 高级区与 `/api` 已有出口，本页面再开一套
写入口，等于把写路径的事实源分裂成两份。
"""
from __future__ import annotations

import hashlib
import logging
import secrets
from datetime import date, datetime, timezone
from decimal import Decimal
from typing import Any

from fastapi import APIRouter, Form, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response

from . import db
from .config import settings
from .state import RUNTIME
from .timeutil import cst_day_bounds_ms, ms_to_cst_date, now_ms

log = logging.getLogger("app.admin")

router = APIRouter(include_in_schema=False)

COOKIE_NAME = "hm_admin"
_COOKIE_PATH = "/admin"

# 新鲜度分档（毫秒）：与契约 §8.7 的 180 s 网关离线判定不同——那是业务判定，
# 这里是「运维还能不能信这份数据」，阈值放得宽一些，免得 3 分钟没数据就报警色。
_FRESH_OK_MS = 5 * 60 * 1000
_FRESH_WARN_MS = 60 * 60 * 1000


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


# --------------------------------------------------------------------------- 数据


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


def _collect() -> dict[str, Any]:
    """一次查询把页面要的东西全取出来。行数都在百级以内（P0 = 1 网关 4 节点）。"""
    now = now_ms()
    today_start, _ = cst_day_bounds_ms(ms_to_cst_date(now))

    gateways = db.fetch_all(
        "SELECT d.id AS device_id, d.sn, d.name, d.fw_ver, d.iotda_device_id, d.owner_id, "
        "       o.online, o.last_heartbeat_ms, o.alarm_state, o.last_properties_json "
        "FROM devices d LEFT JOIN device_online o ON o.device_id = d.id ORDER BY d.id"
    )
    for row in gateways:
        props = db.jload(row.pop("last_properties_json", None), None)
        row["properties"] = props if isinstance(props, dict) else None
    gateway_online = {row["device_id"]: bool(row.get("online")) for row in gateways}

    nodes = db.fetch_all(
        "SELECT device_id, sn, type, location, online, last_seen_ms, rssi, battery "
        "FROM nodes ORDER BY device_id, sn"
    )
    alerts = db.fetch_all(
        # recovered_at 是 DATETIME(3)：直接 select 出来是 datetime 对象，JSONResponse 序列化不了，
        # 所以一律在 SQL 里换成毫秒（前端也只认毫秒，时区换算交给浏览器）。
        "SELECT id, device_id, node_sn, type, level, ts_ms, status, push_count, "
        "       ROUND(UNIX_TIMESTAMP(recovered_at) * 1000) AS recovered_at_ms "
        "FROM alerts ORDER BY ts_ms DESC LIMIT 30"
    )
    # 走主键倒序取最后一条，而不是 MAX(ts_ms) 扫全表：sensor_readings 是本项目最大的表。
    last_reading = db.fetch_one(
        "SELECT id, device_id, node_sn, type, ts_ms FROM sensor_readings ORDER BY id DESC LIMIT 1"
    )
    counts = db.fetch_one(
        "SELECT COUNT(*) AS total, "
        "       SUM(ts_ms >= :today) AS today, "
        "       SUM(status = 'pending') AS pending "
        "FROM alerts",
        {"today": today_start},
    ) or {}

    last_ts = int(last_reading["ts_ms"]) if last_reading and last_reading.get("ts_ms") else None
    age = (now - last_ts) if last_ts else None
    if age is None:
        freshness = "none"
    elif age <= _FRESH_OK_MS:
        freshness = "ok"
    elif age <= _FRESH_WARN_MS:
        freshness = "warn"
    else:
        freshness = "stale"

    return {
        "server_ts": now,
        "today_start_ms": today_start,
        "service": {
            "version": settings.version,
            "app_env": settings.app_env,
            "db": "ok" if db.ping() else "down",
            "clock_source": settings.clock_source,
            # 进程内值：重启即清零，所以只作参考，不能当「多久没数据」的判据
            "webhook_last_at": RUNTIME.get("webhook_last_at"),
            "started_at": RUNTIME.get("started_at"),
            "scheduler_ticks": RUNTIME.get("scheduler_ticks"),
        },
        "freshness": {
            "last_reading_ms": last_ts,
            "age_ms": age,
            "state": freshness,
            "reading": last_reading,
        },
        "counts": {
            "alerts_total": int(counts.get("total") or 0),
            "alerts_today": int(counts.get("today") or 0),
            "alerts_pending": int(counts.get("pending") or 0),
            "gateways": len(gateways),
            "gateways_online": sum(1 for g in gateways if g.get("online")),
            "nodes": len(nodes),
            # nodes.online 是「节点最后一次上报时说的状态」，网关离线期间它不会变成 0
            # （只有 node_offline 事件才写 0）。网关离线时节点状态**未知**，不能算在线，
            # 否则会出现「网关 0/2 在线、节点 8/8 在线」这种自相矛盾的看板。
            "nodes_online": sum(
                1 for n in nodes if n.get("online") and gateway_online.get(n["device_id"])
            ),
        },
        "gateways": gateways,
        "nodes": nodes,
        "alerts": alerts,
    }


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
        log.warning("运维后台登录失败（来源 %s）", request.client.host if request.client else "?")
        html = _LOGIN_HTML.replace("__VERSION__", settings.version).replace(
            "__ERROR__", '<p class="err">口令不对</p>'
        )
        return HTMLResponse(html, status_code=401)
    resp = RedirectResponse(url="/admin", status_code=303)
    # 不设 max-age：关浏览器即失效；HttpOnly 让页面脚本碰不到；SameSite 断掉跨站自动带票
    resp.set_cookie(
        COOKIE_NAME, _token(), path=_COOKIE_PATH, httponly=True, samesite="strict"
    )
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
        log.exception("运维后台取数失败")
        return JSONResponse(
            status_code=503,
            content={"code": 1500, "message": f"取数失败：{exc.__class__.__name__}", "data": None},
        )
    return JSONResponse(content=_jsonable(payload))


# --------------------------------------------------------------------------- 页面

_LOGIN_HTML = """<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>运维后台 · 登录</title>
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
  <h1>居家安全监测 · 运维后台</h1>
  <p class="sub">只读页面 · 不对外网开放</p>
  __ERROR__
  <input type="password" name="password" placeholder="运维口令" autofocus autocomplete="current-password">
  <button type="submit">进入</button>
  <footer>v__VERSION__ · 通过 SSH 隧道或局域网访问</footer>
</form></body></html>"""


_PAGE_HTML = """<!doctype html>
<html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>运维后台 · 居家安全监测</title>
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

  /* 新鲜度条：整页最重要的一个数 */
  .fresh { display:flex; align-items:center; gap:20px; flex-wrap:wrap;
           padding:16px 20px; margin-bottom:16px; border-radius:13px;
           border:1px solid var(--line); background:var(--panel); }
  .fresh .dot { width:11px; height:11px; border-radius:50%; background:var(--faint); flex:0 0 auto; }
  .fresh.ok    { border-color:#1d4d36; background:linear-gradient(90deg,#0f1d16,var(--panel) 55%); }
  .fresh.warn  { border-color:#4d3a15; background:linear-gradient(90deg,#1d1708,var(--panel) 55%); }
  .fresh.stale { border-color:#4d2020; background:linear-gradient(90deg,#1f0f0f,var(--panel) 55%); }
  .fresh.ok    .dot { background:var(--ok); box-shadow:0 0 12px var(--ok); }
  .fresh.warn  .dot { background:var(--warn); box-shadow:0 0 12px var(--warn); }
  .fresh.stale .dot { background:var(--bad); box-shadow:0 0 12px var(--bad); }
  .fresh .label { color:var(--dim); font-size:12px; letter-spacing:.4px; }
  .fresh .big { font-size:26px; font-weight:600; line-height:1.15; }
  .fresh.ok .big { color:var(--ok); } .fresh.warn .big { color:var(--warn); }
  .fresh.stale .big { color:var(--bad); } .fresh.none .big { color:var(--faint); }
  .fresh .detail { color:var(--faint); font-size:12px; }

  .kpis { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:12px; margin-bottom:16px; }
  .kpi { padding:14px 16px; border:1px solid var(--line); border-radius:12px; background:var(--panel); }
  .kpi .k { color:var(--dim); font-size:12px; margin-bottom:6px; }
  .kpi .v { font-size:22px; font-weight:600; }
  .kpi .v small { font-size:13px; color:var(--faint); font-weight:400; }

  section { margin-bottom:16px; border:1px solid var(--line); border-radius:12px;
            background:var(--panel); overflow:hidden; }
  section > h2 { margin:0; padding:11px 16px; font-size:12.5px; font-weight:600; color:var(--dim);
                 letter-spacing:.6px; border-bottom:1px solid var(--line); background:#0f151d;
                 display:flex; gap:10px; align-items:baseline; }
  section > h2 span { font-weight:400; color:var(--faint); font-size:11.5px; letter-spacing:0; }
  table { width:100%; border-collapse:collapse; }
  th, td { padding:9px 16px; text-align:left; font-size:13px; border-bottom:1px solid #161e28; }
  th { color:var(--faint); font-weight:500; font-size:11.5px; letter-spacing:.4px; background:#0d131a; }
  tr:last-child td { border-bottom:0; }
  tbody tr:hover { background:#151d27; }
  .on { color:var(--ok); } .off { color:var(--bad); } .idle { color:var(--faint); }
  .tag { display:inline-block; padding:1px 7px; border-radius:6px; font-size:11.5px;
         border:1px solid var(--line); color:var(--dim); }
  .tag.l1 { color:var(--warn); border-color:#4d3a15; }
  .tag.l2 { color:var(--bad); border-color:#4d2020; }
  .tag.st-pending { color:var(--warn); border-color:#4d3a15; }
  .tag.st-confirmed { color:var(--info); border-color:#16344d; }
  .tag.st-resolved { color:var(--ok); border-color:#1d4d36; }
  .kv { display:grid; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); gap:10px; padding:14px 16px; }
  .kv div { border:1px solid #18212c; border-radius:9px; padding:8px 11px; background:#0d131a; }
  .kv .k { color:var(--faint); font-size:11px; margin-bottom:2px; }
  .kv .v { font-size:15px; }
  .empty { padding:16px; color:var(--faint); font-size:13px; }
  .stale-banner { display:none; margin-bottom:16px; padding:10px 16px; border-radius:10px;
                  border:1px solid #4d2020; background:#1f0f0f; color:#ff9d9d; font-size:13px; }

  /* 大屏模式（?fullscreen=1）：只留能一眼看懂的，字号放大、去掉可操作元素 */
  body.fullscreen { padding:34px 40px; }
  body.fullscreen .hide-fs { display:none !important; }
  body.fullscreen .fresh .big { font-size:52px; }
  body.fullscreen .fresh .label { font-size:16px; }
  body.fullscreen .fresh .detail { font-size:15px; }
  body.fullscreen .kpi .v { font-size:40px; }
  body.fullscreen .kpi .k { font-size:15px; }
  body.fullscreen th, body.fullscreen td { font-size:17px; padding:13px 20px; }
  body.fullscreen header h1 { font-size:24px; }
</style></head>
<body>
<header>
  <h1>居家安全监测 · 运维后台</h1>
  <span class="meta mono" id="clock">—</span>
  <span class="spacer"></span>
  <span class="pill">v__VERSION__ · <b id="env">—</b> · 库 <b id="db">—</b></span>
  <button class="btn hide-fs" id="btn-refresh" type="button">刷新</button>
  <a class="btn hide-fs" id="btn-fs" href="?fullscreen=1">大屏</a>
  <form class="hide-fs" method="post" action="/admin/logout" style="display:inline">
    <button class="btn" type="submit">退出</button>
  </form>
</header>

<div class="stale-banner" id="banner"></div>

<div class="fresh none" id="fresh">
  <span class="dot"></span>
  <div>
    <div class="label">最后一条数据</div>
    <div class="big" id="fresh-big">—</div>
    <div class="detail" id="fresh-detail">正在读取…</div>
  </div>
</div>

<div class="kpis">
  <div class="kpi"><div class="k">网关在线</div><div class="v" id="k-gw">—</div></div>
  <div class="kpi"><div class="k">节点在线</div><div class="v" id="k-node">—</div></div>
  <div class="kpi"><div class="k">今日告警 / 未处置</div><div class="v" id="k-alert">—</div></div>
  <div class="kpi"><div class="k">进程内最近 webhook</div><div class="v" id="k-hook" style="font-size:15px">—</div></div>
</div>

<section class="hide-fs">
  <h2>网关 <span id="gw-note"></span></h2>
  <table><thead><tr>
    <th>SN</th><th>名称</th><th>在线</th><th>最近心跳</th><th>距现在</th><th>固件</th><th>IoTDA 设备</th>
  </tr></thead><tbody id="gw-body"><tr><td colspan="7" class="empty">—</td></tr></tbody></table>
</section>

<section class="hide-fs">
  <h2>节点 <span id="node-note"></span></h2>
  <table><thead><tr>
    <th>SN</th><th>网关</th><th>类型</th><th>房间</th><th>在线</th><th>最近心跳</th><th>距现在</th><th>RSSI</th><th>电量</th>
  </tr></thead><tbody id="node-body"><tr><td colspan="9" class="empty">—</td></tr></tbody></table>
</section>

<section class="hide-fs">
  <h2>最近属性快照 <span>device_online.last_properties_json，网关最后一次上报的全部属性</span></h2>
  <div id="props"><div class="empty">—</div></div>
</section>

<section>
  <h2>告警流水 <span>最近 30 条，按发生时刻倒序</span></h2>
  <table><thead><tr>
    <th>发生时刻</th><th>距现在</th><th>类型</th><th>级别</th><th>节点</th><th>状态</th><th>推送</th>
  </tr></thead><tbody id="alert-body"><tr><td colspan="7" class="empty">—</td></tr></tbody></table>
</section>

<script>
const FS = new URLSearchParams(location.search).has('fullscreen');
if (FS) document.body.classList.add('fullscreen');
if (FS) document.getElementById('btn-fs').textContent = '';

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s == null ? '' : s);
const pad = (n) => String(n).padStart(2, '0');

function fmtTime(ms) {
  if (!ms) return '—';
  const d = new Date(Number(ms));
  return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
}
function fmtAge(ms) {
  if (ms == null) return '—';
  const s = Math.max(0, Math.round(ms / 1000));
  if (s < 60) return s + ' 秒前';
  const m = Math.round(s / 60);
  if (m < 60) return m + ' 分钟前';
  const h = Math.floor(m / 60);
  if (h < 24) return h + ' 小时 ' + (m % 60) + ' 分前';
  return Math.floor(h / 24) + ' 天 ' + (h % 24) + ' 小时前';
}
const TYPE_CN = { gas:'燃气', fall:'跌倒', sos:'紧急求助', no_motion:'长时间无活动',
                  node_offline:'节点离线', device_offline:'网关离线', door_left:'开门离家', env:'环境' };
const NODE_CN = { hub:'汇聚', gas:'燃气', fall:'跌倒', door:'门磁', presence:'人体' };
const ST_CN = { pending:'未处置', confirmed:'已确认', resolved:'已处置' };

function setRow(tbody, rows, cols, emptyText) {
  if (!rows.length) { tbody.innerHTML = `<tr><td colspan="${cols}" class="empty">${emptyText}</td></tr>`; return; }
  tbody.innerHTML = rows.join('');
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

  const f = d.freshness;
  const box = $('fresh');
  box.className = 'fresh ' + f.state;
  const stateCn = { ok:'数据在流动', warn:'数据延迟', stale:'数据断流', none:'从未收到数据' }[f.state] || '—';
  $('fresh-big').textContent = f.age_ms == null ? stateCn : (fmtAge(f.age_ms) + ' · ' + stateCn);
  $('fresh-detail').textContent = f.reading
    ? `最近一条读数 ${fmtTime(f.last_reading_ms)} · ${esc(f.reading.node_sn)} / ${esc(f.reading.type)}`
    : 'sensor_readings 表为空：设备侧从未上报';

  const banner = $('banner');
  if (f.state === 'stale' || f.state === 'none') {
    banner.style.display = 'block';
    banner.textContent = f.state === 'none'
      ? '⚠ 数据库里没有任何读数：网关没有在往云上推数据，先查设备侧与 IoTDA 规则引擎。'
      : '⚠ 最后一条数据已是 ' + fmtAge(f.age_ms) + '：先查网关在线状态与 IoTDA 规则引擎，不要先怀疑页面。';
  } else {
    banner.style.display = 'none';
  }

  const c = d.counts;
  $('k-gw').innerHTML = `${c.gateways_online}<small> / ${c.gateways}</small>`;
  $('k-node').innerHTML = `${c.nodes_online}<small> / ${c.nodes}</small>`;
  $('k-alert').innerHTML = `${c.alerts_today}<small> / ${c.alerts_pending}</small>`;
  $('k-hook').textContent = d.service.webhook_last_at
    ? fmtTime(d.service.webhook_last_at) + '（重启即清零）'
    : '本进程尚未收到 webhook';

  const gw = d.gateways.map((g) => `<tr>
      <td class="mono">${esc(g.sn)}</td>
      <td>${esc(g.name)}</td>
      <td class="${g.online ? 'on' : 'off'}">${g.online ? '在线' : '离线'}</td>
      <td class="mono">${fmtTime(g.last_heartbeat_ms)}</td>
      <td class="mono">${fmtAge(g.last_heartbeat_ms ? now - Number(g.last_heartbeat_ms) : null)}</td>
      <td class="mono">${esc(g.fw_ver) || '—'}</td>
      <td class="mono" style="color:var(--faint)">${esc(g.iotda_device_id) || '未注册'}</td>
    </tr>`);
  setRow($('gw-body'), gw, 7, 'devices 表为空：还没出厂注册任何网关');
  $('gw-note').textContent = `共 ${c.gateways} 台，在线 ${c.gateways_online}`;

  // 节点 SN 只在同一网关内唯一：两台网关各有一份 H1/M1/M2/M4，所以必须标出归属网关，
  // 否则页面上会出现两行一模一样的 SN。
  const gwSn = {};
  const gwOnline = {};
  d.gateways.forEach((g) => { gwSn[g.device_id] = g.sn; gwOnline[g.device_id] = !!g.online; });
  const nd = d.nodes.map((n) => {
    const gwOn = !!gwOnline[n.device_id];
    return `<tr>
      <td class="mono">${esc(n.sn)}</td>
      <td class="mono">${esc(gwSn[n.device_id] || '#' + n.device_id)}</td>
      <td>${esc(NODE_CN[n.type] || n.type)}</td>
      <td>${esc(n.location) || '—'}</td>
      <td class="${gwOn ? (n.online ? 'on' : 'off') : 'idle'}">${gwOn ? (n.online ? '在线' : '离线') : '未知'}</td>
      <td class="mono">${fmtTime(n.last_seen_ms)}</td>
      <td class="mono">${fmtAge(n.last_seen_ms ? now - Number(n.last_seen_ms) : null)}</td>
      <td class="mono">${n.rssi == null ? '—' : esc(n.rssi) + ' dBm'}</td>
      <td class="mono">${n.battery == null ? '—' : (Number(n.battery) === 255 ? '市电' : esc(n.battery) + '%')}</td>
    </tr>`;
  });
  setRow($('node-body'), nd, 9, 'nodes 表为空：汇聚还没上报过节点列表');
  $('node-note').textContent = `共 ${c.nodes} 个，在线 ${c.nodes_online}` +
    (c.nodes_online < c.nodes ? '（网关离线期间节点状态未知，不计入在线）' : '');

  const props = (d.gateways.find((g) => g.properties) || {}).properties;
  if (props) {
    const items = Object.entries(props).map(([k, v]) => `<div>
        <div class="k mono">${esc(k)}</div>
        <div class="v mono">${esc(typeof v === 'object' ? JSON.stringify(v) : v)}</div>
      </div>`);
    $('props').innerHTML = `<div class="kv">${items.join('')}</div>`;
  } else {
    $('props').innerHTML = '<div class="empty">还没有属性快照：网关未上报过 properties</div>';
  }

  const al = d.alerts.map((a) => `<tr>
      <td class="mono">${fmtTime(a.ts_ms)}</td>
      <td class="mono">${fmtAge(now - Number(a.ts_ms))}</td>
      <td>${esc(TYPE_CN[a.type] || a.type)}</td>
      <td><span class="tag l${a.level}">${Number(a.level) === 2 ? '报警' : '预警'}</span></td>
      <td class="mono">${esc(a.node_sn) || '—'}</td>
      <td><span class="tag st-${esc(a.status)}">${esc(ST_CN[a.status] || a.status)}</span></td>
      <td class="mono" style="color:var(--faint)">${esc(a.push_count)} 次</td>
    </tr>`);
  setRow($('alert-body'), al, 7, 'alerts 表为空：还没有产生过告警');
}

load();
setInterval(load, 5000);
$('btn-refresh').addEventListener('click', load);
</script>
</body></html>"""
