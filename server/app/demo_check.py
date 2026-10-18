"""演示数据自检（t49）：逐条断言 D1–D7 在真实 HTTP 上看得见，并读库验证「补传」与报表自洽。

为什么要单独一个模块而不是写在 shell 里：断言既要走 uvicorn（REST 口径，App 看到的就是这些响应），
又要读 MySQL（补传证据、报表自洽），写在 shell 里既脆又难复现；写成 Python 模块还能被 pytest 复用
（传 TestClient 而不是真栈），因此同一套判据在「本机 pytest」与「现场容器」里是同一份代码。

用法（栈已起 + 已跑过 `seed --demo`）：
    docker compose exec api python -m app.demo_check        # 或 server/tools/demo-check.sh
退出码：全绿 0；任一失败 1（输出可直接贴进证据报告）。
反向（没灌数据）一定是红（rc=1），条数取决于库里还剩什么，别把它当固定值：
  · 整库 TRUNCATE（连演示账号一起没了）→ 登录失败，整体退出，计 **0/7**；
  · 只清业务数据、账号与设备仍在 → 逐条跑，7 项全红，计 **0/7**；
  · 只删读数与活动、告警还在 → D1/D4 仍绿，计 **2/7**。

两条硬约束（t61 F-01/F-03 的教训）：
1. **所有读详情处一律写回**：§6.13 规定「打开 pending 详情即翻 confirmed 并清空 next_push_at」，
   自检不得改变被检对象，所以每一次 `GET /api/alerts/{id}` 都必须走 `_read_detail()`（读后按原值写回并自校验）。
2. **判据不得依赖运行钟点**：凡是「补传」这类判断，一律按数据内标记/关系自证（如 payload.origin=backfill、
   created_at 与 ts 的差值），不许拿「当前时间是否已过某钟点」当条件——否则同一份数据上午绿、晚上红。

判据来源：任务卡 t49 的 D1–D7 + 契约 §6.13（告警字段/文案）、§6.8（读数类型）、§6.16–§6.18（报表口径）、
§8.2（燃气二级阈值 2500 mV）、§8.6（门久开 180 s）、§8.7（网关离线→恢复）。
"""
from __future__ import annotations

import os
import sys
from typing import Any, Callable

from . import db
from .seed import (
    DEMO_ACTIVITY_DAYS,
    DEMO_BACKFILL_COUNT,
    DEMO_BACKFILL_DELAY_MIN,
    DEMO_BACKFILL_MARK,
    DEMO_OFFLINE_HOUR,
    DEMO_OFFLINE_MINUTES,
    DEMO_PASSWORD,
    DEMO_TODAY_ACTIVITY,
    DEMO_USERNAME,
    DEMO_YESTERDAY_ACTIVITY,
)
from .summary import TREND_TITLE
from .timeutil import cst_day_bounds_ms, ms_to_cst_date, now_ms

DEFAULT_BASE = os.environ.get("DEMO_CHECK_BASE", "http://127.0.0.1:3000")

GAS_T2_MV = 2500            # §8.2 gas_t2_mv：二级阈值，演示数据必须真的越过它（不是随便填个大数）
DOOR_LEFT_S = 180           # §8.6 门久开阈值
OFFLINE_GAP_MIN = 55        # D6 判据：网关属性快照的时间戳跳变至少 55 分钟
ALERT_KEYS = ("id", "device_id", "node_sn", "type", "level", "ts", "status", "push_count", "summary")
EXPECT_SUMMARY = {
    ("gas", 2): "厨房燃气泄漏报警",
    ("fall", 2): "卧室检测到跌倒",
    ("sos", 2): "收到紧急求助",
    ("no_motion", 1): "30 分钟未检测到活动",
    ("door_left", 1): "门已打开超过 3 分钟",
    ("device_offline", 1): "网关已离线",
}


# --------------------------------------------------------------------------- HTTP 薄封装

class Api:
    """既接受 requests.Session（真栈），也接受 Starlette TestClient（pytest，无需起服务）。"""

    def __init__(self, http: Any, base: str = DEFAULT_BASE):
        self.http = http
        self.base = base.rstrip("/")
        self.token: str | None = None

    def _headers(self) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.token}"} if self.token else {}

    def login(self, username: str = DEMO_USERNAME, password: str = DEMO_PASSWORD) -> None:
        resp = self.http.post(f"{self.base}/api/auth/login", json={"username": username, "password": password})
        body = resp.json()
        if resp.status_code != 200 or body.get("code") != 0:
            raise RuntimeError(f"登录失败（HTTP {resp.status_code}）：{str(body)[:200]}；先跑 `python -m app.seed --demo`")
        self.token = body["data"]["token"]

    def get(self, path: str, **params: Any) -> Any:
        resp = self.http.get(f"{self.base}{path}", params={k: v for k, v in params.items() if v is not None},
                             headers=self._headers())
        return _unwrap(resp, path)

    def post(self, path: str, payload: dict | None = None) -> Any:
        resp = self.http.post(f"{self.base}{path}", json=payload or {}, headers=self._headers())
        return _unwrap(resp, path)


def _unwrap(resp: Any, path: str) -> Any:
    try:
        body = resp.json()
    except Exception as exc:                                    # noqa: BLE001 - 自检要给出可读原因
        raise RuntimeError(f"{path} → HTTP {resp.status_code}，响应不是 JSON：{exc}") from exc
    if resp.status_code != 200 or body.get("code") != 0:
        raise RuntimeError(f"{path} → HTTP {resp.status_code} {str(body)[:200]}")
    return body["data"]


# --------------------------------------------------------------------------- 库内小工具

def _daily_count(device_id: int, day: Any) -> int:
    return int(db.fetch_scalar(
        "SELECT `count` FROM activity_daily WHERE device_id = :d AND `date` = :day", {"d": device_id, "day": day}, 0) or 0)


def _sum_daily(device_id: int, from_ms: int, to_ms: int) -> int:
    return int(db.fetch_scalar(
        "SELECT COALESCE(SUM(`count`), 0) FROM activity_daily WHERE device_id = :d AND `date` BETWEEN :a AND :b",
        {"d": device_id, "a": ms_to_cst_date(from_ms), "b": ms_to_cst_date(to_ms)}, 0) or 0)


def _alerts_in_window(device_id: int, from_ms: int, to_ms: int) -> int:
    return int(db.fetch_scalar(
        "SELECT COUNT(*) FROM alerts WHERE device_id = :d AND ts_ms BETWEEN :f AND :t",
        {"d": device_id, "f": from_ms, "t": to_ms}, 0) or 0)


def _readings(api: Api, device_id: int, **params: Any) -> dict:
    return api.get(f"/api/devices/{device_id}/readings", limit=200, **params)


# --------------------------------------------------------------------------- D1–D7

def _read_detail(api: Api, alert_id: int) -> dict:
    """读告警详情，但**不改变被检对象**——所有读详情处一律走这里（D1/D2/D3/D4）。

    为什么需要它：§6.13 规定「打开 pending 详情即翻成 confirmed 并清空 next_push_at」，
    而演示正要用那些待处置告警演「处置」。自检不得把它检查的状态吃掉，
    所以读完按原值写回（这是自检脚本的特权，产品代码里没有这个反向操作），
    并在返回前**自校验**写回确实成功——写回失败会静默污染演示态，必须当场炸出来（t61 F-01）。
    """
    before = db.fetch_one("SELECT status, next_push_at FROM alerts WHERE id = :i", {"i": alert_id})
    detail = api.get(f"/api/alerts/{alert_id}")
    if before and before["status"] == "pending":
        db.execute("UPDATE alerts SET status = 'pending', next_push_at = :np WHERE id = :i",
                   {"i": alert_id, "np": before["next_push_at"]})
    after = db.fetch_one("SELECT status, next_push_at FROM alerts WHERE id = :i", {"i": alert_id})
    if before is not None and after is not None:
        assert dict(before) == dict(after), f"读详情改变了被检行 id={alert_id}：{dict(before)} → {dict(after)}"
    return detail


def check_d1(api: Api, device_id: int) -> str:
    """D1：燃气告警在列表里可见、字段完整，且详情里的读数真的越过了 §8.2 二级阈值。"""
    items = api.get("/api/alerts", device_id=device_id, type="gas", limit=200)["items"]
    assert items, "gas 告警一条都没有"
    level2 = [a for a in items if a["level"] == 2]
    assert level2, f"gas 告警没有 level=2（现有等级 {[a['level'] for a in items]}）"
    top = level2[0]
    missing = [k for k in ALERT_KEYS if k not in top]
    assert not missing, f"§6.13 必现字段缺失：{missing}"
    assert top["summary"] == EXPECT_SUMMARY[("gas", 2)], f"summary={top['summary']!r} 与 §6.13 文案不一致"
    assert top["status"] == "pending", f"头牌燃气报警应为待处置，实际 {top['status']}"
    detail = _read_detail(api, top["id"])
    adc = (detail.get("snapshot") or {}).get("adc_mv")
    assert adc is not None and adc >= GAS_T2_MV, f"snapshot.adc_mv={adc} 未越过二级阈值 {GAS_T2_MV}"
    after = db.fetch_scalar("SELECT status FROM alerts WHERE id = :i", {"i": top["id"]}, "")
    assert after == "pending", f"自检把待处置告警改成了 {after}（演示要靠它演处置）"
    return (f"gas {len(items)} 条，头牌 id={top['id']} level=2 status=pending summary={top['summary']}；"
            f"snapshot.adc_mv={adc} ≥ {GAS_T2_MV} mV（§8.2 gas_t2_mv）；读详情后仍为 pending")


def check_d2(api: Api, device_id: int) -> str:
    """D2：跌倒告警 + 跌倒读数（详情 snapshot 与读数两处都要有实物）。"""
    items = api.get("/api/alerts", device_id=device_id, type="fall", limit=200)["items"]
    level2 = [a for a in items if a["level"] == 2]
    assert level2, "没有 level=2 的跌倒告警"
    top = level2[0]
    assert top["summary"] == EXPECT_SUMMARY[("fall", 2)], f"summary={top['summary']!r}"
    detail = _read_detail(api, top["id"])
    assert (detail.get("snapshot") or {}).get("fall") == 1, f"snapshot={detail.get('snapshot')}"

    today = ms_to_cst_date(now_ms())
    day_from, day_to = cst_day_bounds_ms(today)
    readings = _readings(api, device_id, type="fall", **{"from": day_from, "to": day_to})["items"]
    assert any((r["payload"] or {}).get("fall") == 1 for r in readings), "今天的跌倒读数里没有 fall=1"
    return (f"fall {len(items)} 条（level=2 共 {len(level2)}），头牌 summary={top['summary']}；"
            f"今天跌倒读数 {len(readings)} 条且含 fall=1")


def check_d3(api: Api, device_id: int) -> str:
    """D3：无活动 + 门久开两类告警，以及门磁读数本身（P0 的活动判定数据源）。"""
    no_motion = api.get("/api/alerts", device_id=device_id, type="no_motion", limit=200)["items"]
    door_left = api.get("/api/alerts", device_id=device_id, type="door_left", limit=200)["items"]
    assert no_motion, "no_motion 告警一条都没有"
    assert no_motion[0]["summary"] == EXPECT_SUMMARY[("no_motion", 1)], f"summary={no_motion[0]['summary']!r}"
    assert door_left, "door_left 告警一条都没有"
    assert door_left[0]["summary"] == EXPECT_SUMMARY[("door_left", 1)], f"summary={door_left[0]['summary']!r}"
    snapshot = (_read_detail(api, door_left[0]["id"]).get("snapshot") or {})
    assert int(snapshot.get("open_s", 0)) >= DOOR_LEFT_S, f"door_left snapshot={snapshot} 未达 §8.6 的 {DOOR_LEFT_S} s"

    today = ms_to_cst_date(now_ms())
    day_from, day_to = cst_day_bounds_ms(today)
    doors = _readings(api, device_id, type="door", node_sn="M4", **{"from": day_from, "to": day_to})["items"]
    states = [bool((r["payload"] or {}).get("door")) for r in doors]
    assert True in states and False in states, f"今天的门磁读数没有跳变：{states}"
    return (f"no_motion id={no_motion[0]['id']} summary={no_motion[0]['summary']}；"
            f"door_left id={door_left[0]['id']} open_s={snapshot.get('open_s')} ≥ {DOOR_LEFT_S} s；"
            f"今天门磁读数 {len(doors)} 条，door 序列(新→旧)={states}")


def check_d4(api: Api, device_id: int) -> str:
    """D4：SOS 有来源区分（button=物理按键 / app=App 触发），两条都在。

    读详情走 `_read_detail()`：其中一条 SOS 是待处置的，直接读会把演示要用的状态吃掉（t61 F-01）。
    """
    items = api.get("/api/alerts", device_id=device_id, type="sos", limit=200)["items"]
    assert items, "sos 告警一条都没有"
    sources = []
    for item in items:
        snapshot = (_read_detail(api, item["id"]).get("snapshot") or {})
        sources.append(str(snapshot.get("source") or "?"))
    assert "button" in sources, f"SOS 缺少 source=button（现有 {sources}）"
    assert "app" in sources, f"SOS 缺少 source=app（现有 {sources}）"
    pending = [i["id"] for i in items if i["status"] == "pending"]
    for alert_id in pending:                       # 写回是否成功由 _read_detail 自校验，这里再点一次名
        row = db.fetch_one("SELECT status FROM alerts WHERE id = :i", {"i": alert_id})
        assert row and row["status"] == "pending", f"自检把待处置 SOS id={alert_id} 改成了 {row and row['status']}"
    return f"sos {len(items)} 条，来源={sources}（button 与 app 都在），待处置 {pending} 读后仍为 pending"


def check_d5(api: Api, device_id: int) -> str:
    """D5：离家/回家的判定输入与参数可查。

    诚实边界：契约 §4.3 的 14 个属性里**没有**独立的 away 状态字段（home/away 是网关本地状态机，
    §8.5），服务器能查到的只有「判定输入」（门磁跳变）与「参数」（away_after_open_s），
    因此这里断言的是这两者，而不是一个不存在的字段。
    """
    today = ms_to_cst_date(now_ms())
    day_from, day_to = cst_day_bounds_ms(today)
    doors = _readings(api, device_id, type="door", node_sn="M4", **{"from": day_from, "to": day_to})["items"]
    ordered = sorted(doors, key=lambda r: r["ts"])
    transitions = [(r["ts"], bool((r["payload"] or {}).get("door"))) for r in ordered]
    assert len(transitions) >= 3, f"今天的门磁读数不足以看出「关→开→关」：{transitions}"
    assert [state for _, state in transitions[:3]] == [False, True, False], f"门磁序列不是 关→开→关：{transitions[:3]}"

    settings = api.get(f"/api/devices/{device_id}/settings")
    away = (settings.get("items") or {}).get("away_after_open_s")
    assert away, f"§6.9 items 里没有 away_after_open_s：{sorted((settings.get('items') or {}))[:5]}…"

    status = api.get(f"/api/devices/{device_id}/status")
    props = status.get("properties") or {}
    assert "door" in props, f"/status properties 里没有 door：{sorted(props)}"
    assert props.get("bound_nodes"), "/status properties 里没有 bound_nodes（§4.3）"
    m4 = [n for n in status.get("nodes") or [] if n["sn"] == "M4"]
    assert m4 and m4[0]["online"] is True, f"M4 不在线：{m4}"
    return (f"今天门磁 关→开→关（{transitions[0][0]}→{transitions[2][0]}）；away_after_open_s={away}；"
            f"properties.door={props.get('door')} bound_nodes={props.get('bound_nodes')} M4 online=True"
            "（注：契约无独立 away 字段，见函数注释）")


def check_d6(api: Api, device_id: int) -> str:
    """D6：网关离线→恢复，且断网窗口的证据在库里查得到（跳变 + 补传 + 恢复告警）。

    判据为什么按「标记」而不是按入库时刻：补传样本由 seed 打上 `payload.origin=backfill`，
    窗口里也刻意不写任何实时读数，所以「窗口内读数 = 补传样本」在任何钟点都成立。
    旧判据用「created_at 晚于窗口结束」认补传：13:00 CST 之后实时写入的行也满足该条件，
    同一份数据在晚上会数出 14 条（设计 11 条）——判据随墙钟退化（t61 F-03）。

    在线瞬时值同样不作判据：seed 写的是真实心跳，灌数 180 s 后调度器会按 §8.7 合法地判离线，
    那时「离线→恢复」的证据（device_offline 带 recovered_at）仍在。证据行如实打印当前 online（t61 F-02）。
    """
    today = ms_to_cst_date(now_ms())
    day_from, _ = cst_day_bounds_ms(today)
    off_start = day_from + DEMO_OFFLINE_HOUR * 3600 * 1000
    off_end = off_start + DEMO_OFFLINE_MINUTES * 60 * 1000
    off_last = off_end - 1              # 闭区间：[12:00, 13:00)，13:00 整点那条不算窗口内
    backfill_created = off_end + DEMO_BACKFILL_DELAY_MIN * 60 * 1000

    inside = _readings(api, device_id, type="gateway", **{"from": off_start, "to": off_last})["items"]
    assert not inside, f"断网窗口内不该有网关属性上报，实际 {len(inside)} 条"

    around = _readings(api, device_id, type="gateway",
                       **{"from": off_start - 3 * 3600 * 1000, "to": off_end + 3 * 3600 * 1000})["items"]
    stamps = sorted(r["ts"] for r in around)
    gap = max((b - a for a, b in zip(stamps, stamps[1:])), default=0)
    assert gap >= OFFLINE_GAP_MIN * 60 * 1000, f"网关属性时间戳最大跳变只有 {gap / 60000:.0f} 分钟"

    # 补传证据 = 窗口内带 origin=backfill 标记的行；再自证「采集在断网期、入库在恢复后」：
    # 全部样本共享同一个入库时刻（窗口结束后 3 分钟）且都晚于自己的采集时刻。两条都与运行钟点无关。
    window_rows = _readings(api, device_id, **{"from": off_start, "to": off_last})["items"]
    rows = db.fetch_all(
        "SELECT node_sn, type, ts_ms, payload_json->>'$.origin' AS origin, "
        "UNIX_TIMESTAMP(created_at) * 1000 AS created_ms FROM sensor_readings "
        "WHERE device_id = :d AND ts_ms BETWEEN :f AND :t ORDER BY ts_ms",
        {"d": device_id, "f": off_start, "t": off_last})
    marked = [r for r in rows if (r.get("origin") or "") == DEMO_BACKFILL_MARK]
    unmarked = [r for r in rows if (r.get("origin") or "") != DEMO_BACKFILL_MARK]
    assert len(rows) == len(window_rows), f"窗口内读数条数不一致：库 {len(rows)} 条 vs REST {len(window_rows)} 条"
    assert not unmarked, ("断网窗口内出现了非补传读数（实时写入），演示口径「窗口内只有补传样本」不成立："
                          f"{[(r['node_sn'], r['type'], int(r['ts_ms'])) for r in unmarked]}")
    assert len(marked) == DEMO_BACKFILL_COUNT, (
        f"窗口内带 origin={DEMO_BACKFILL_MARK} 的补传样本 {len(marked)} 条，设计值 {DEMO_BACKFILL_COUNT} 条")
    kinds = sorted({r["type"] for r in marked})
    created_values = sorted({int(r["created_ms"]) for r in marked})
    assert created_values == [backfill_created], (
        f"补传样本的入库时刻应为窗口结束后 {DEMO_BACKFILL_DELAY_MIN} 分钟（{backfill_created}），实际 {created_values}")
    early = [r for r in marked if int(r["created_ms"]) <= int(r["ts_ms"])]
    assert not early, f"补传样本必须「入库晚于采集」，这些行不满足：{[(r['node_sn'], int(r['ts_ms'])) for r in early]}"

    offline = api.get("/api/alerts", device_id=device_id, type="device_offline", limit=200)["items"]
    assert offline, "没有 device_offline 告警"
    recovered = [a for a in offline if a.get("recovered_at")]
    assert recovered, f"device_offline 告警没有 recovered_at：{offline}"
    status = api.get(f"/api/devices/{device_id}/status")
    online = status.get("online")
    return (f"窗口 {off_start}–{off_end} 内网关上报 0 条，前后时间戳跳变 {gap / 60000:.0f} 分钟（≥{OFFLINE_GAP_MIN}）；"
            f"窗口内读数 {len(rows)} 条且全部带 origin={DEMO_BACKFILL_MARK} 标记（type={kinds}），"
            f"入库时刻均为窗口结束后 {DEMO_BACKFILL_DELAY_MIN} 分钟（{backfill_created}）且晚于各自采集时刻"
            f"——「采集在断网期、入库在恢复后」由数据自证，与运行钟点无关；"
            f"device_offline {len(offline)} 条，其中 {len(recovered)} 条带 recovered_at；"
            f"当前 /status online={online}（灌数 180 s 后调度器会合法判离线，属预期，在线瞬时值不作判据）")


def check_d7(api: Api, device_id: int) -> str:
    """D7：周/月/趋势三份报表口径互相自洽（都与库里的 activity_daily 对得上）。"""
    today = ms_to_cst_date(now_ms())
    trend = api.get("/api/reports/trend", device_id=device_id, days=7)
    compare = trend["compare"]
    assert compare.get("title") == TREND_TITLE, f"compare.title={compare.get('title')!r}"
    assert compare.get("today") == _daily_count(device_id, today), "compare.today 与 activity_daily 不一致"
    assert compare.get("today") == DEMO_TODAY_ACTIVITY, f"今日活动应为设计值 {DEMO_TODAY_ACTIVITY}（冻结时钟）"
    assert compare.get("yesterday") == DEMO_YESTERDAY_ACTIVITY, f"昨日活动应为 {DEMO_YESTERDAY_ACTIVITY}"
    expected_delta = round((DEMO_TODAY_ACTIVITY - DEMO_YESTERDAY_ACTIVITY) / DEMO_YESTERDAY_ACTIVITY * 100)
    assert compare.get("delta_pct") == expected_delta, f"delta_pct={compare.get('delta_pct')} 期望 {expected_delta}"
    assert isinstance(compare.get("delta_pct"), int), "delta_pct 必须是整数（§6.17）"
    assert compare.get("text") == "较昨日下降 25%", f"compare.text={compare.get('text')!r}"
    assert len(trend["series"]) == 7, f"series 长度 {len(trend['series'])}"
    for item in trend["series"]:
        assert item["count"] == _daily_count(device_id, item["date"]), f"series[{item['date']}] 与库里不一致"

    day = api.get("/api/reports/activity", device_id=device_id, period="day")
    assert len(day["hourly"]) == 24 and len(day["daily"]) == 1, "period=day 应为 24 小时 + 1 天"
    assert day["total"] == DEMO_TODAY_ACTIVITY, f"日报 total={day['total']}"
    assert sum(h["count"] for h in day["hourly"]) == day["total"], "hourly 合计与 total 不一致"

    week = api.get("/api/reports/summary", device_id=device_id, period="week")
    month = api.get("/api/reports/summary", device_id=device_id, period="month")
    details = []
    for period, data in (("week", week), ("month", month)):
        assert data["activity"]["total"] == _sum_daily(device_id, data["from"], data["to"]), \
            f"{period} activity.total 与 activity_daily 不一致"
        assert data["alerts"]["total"] == _alerts_in_window(device_id, data["from"], data["to"]), \
            f"{period} alerts.total 与 alerts 表不一致"
        assert data["env"]["temp_avg"] is not None, f"{period} env.temp_avg 为空（网关快照没进统计窗口）"
        assert data["activity"]["compare_prev"], f"{period} 缺少 compare_prev"
        details.append(f"{period} 活动 {data['activity']['total']} 次/{data['activity']['active_days']} 天、"
                       f"告警 {data['alerts']['total']} 条（误报 {data['alerts']['false_alarm']}）、"
                       f"环境 {data['env']['temp_avg']}℃、{data['activity']['compare_prev']}")
    return (f"趋势 today={compare['today']} yesterday={compare['yesterday']} delta_pct={compare['delta_pct']} "
            f"text={compare['text']!r}；日报 total={day['total']}；" + "；".join(details))


CHECKS: list[tuple[str, str, Callable[[Api, int], str]]] = [
    ("D1", "燃气二级报警（列表字段 + 详情读数越阈值）", check_d1),
    ("D2", "跌倒告警（告警 + 读数两处实物）", check_d2),
    ("D3", "无活动 / 门久开告警 + 门磁读数", check_d3),
    ("D4", "SOS 求助（button 与 app 两种来源）", check_d4),
    ("D5", "离家/回家判定输入与参数可查", check_d5),
    ("D6", "网关离线→恢复（跳变 + 补传 + 恢复告警）", check_d6),
    ("D7", "周/月/趋势报表口径自洽", check_d7),
]


def run(http: Any, base: str = DEFAULT_BASE, device_id: int | None = None, quiet: bool = False) -> int:
    """跑 D1–D7，返回进程退出码（0=全绿）。`http` 可以是 requests.Session 或 TestClient。"""
    api = Api(http, base)
    try:
        api.login()
        if device_id is None:
            devices = api.get("/api/devices")["items"]
            assert devices, "账号下没有设备（先跑 seed --demo）"
            device_id = devices[0]["id"]
    except Exception as exc:                                      # noqa: BLE001 - 自检要给出可读原因
        print(f"[FAIL] 自检无法开始：{exc}")
        print(f"演示数据自检：0/{len(CHECKS)} 通过")
        return 1
    if not quiet:
        print(f"演示数据自检：device_id={device_id} base={api.base} 共 {len(CHECKS)} 项（判据见 app/demo_check.py 头注释）")

    passed, failed = 0, []
    for code, title, fn in CHECKS:
        try:
            evidence = fn(api, device_id)
        except Exception as exc:                                  # noqa: BLE001 - 自检要把原因原样打出来
            failed.append(code)
            print(f"[FAIL] {code} {title}\n       {exc}")
            continue
        passed += 1
        print(f"[PASS] {code} {title}\n       {evidence}")

    print(f"演示数据自检：{passed}/{len(CHECKS)} 通过" + ("" if not failed else f"，失败项 {' '.join(failed)}"))
    return 0 if not failed else 1


def main(argv: list[str] | None = None) -> int:
    import argparse
    import requests

    parser = argparse.ArgumentParser(description="演示数据自检 D1–D7（需先 seed --demo 且栈已起）")
    parser.add_argument("--base", default=DEFAULT_BASE, help=f"API 基址（默认 {DEFAULT_BASE}）")
    parser.add_argument("--device-id", type=int, default=None, help="指定设备（默认取账号下第一台）")
    args = parser.parse_args(argv)

    session = requests.Session()
    try:
        return run(session, base=args.base, device_id=args.device_id)
    except SystemExit:
        raise
    except Exception as exc:                                      # noqa: BLE001
        print(f"自检无法开始：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
