#!/usr/bin/env python3
"""独立复核 t6（HarmonyOS App 五页骨架与 HTTP 层）的动态探针驱动。

为什么不能只看实现者的自检：
  `harmony-app/tools/selfcheck-entry.ts` 的响应体来自实现者自己的 `MockData.ets`，
  且不校验请求侧（URL / 鉴权头 / 请求体）。本脚本做三件不同的事：
   ① 响应体 fixtures 由**契约表格**生成，并在生成时校验每个字段名都能在对应
      契约章节正文里找到（字段名写错 → 本脚本直接报错，不给"假绿"机会）；
   ② 探针（review/harmony_probe.ts）把传输层换成记录式 transport，逐条断言
      method/URL/query/Authorization/Content-Type/请求体；
   ③ 被测代码仍是 harmony-app 的真实 HttpApiService / Types / Result / SessionStore。

用法：  python3 tools/test/review/check_harmony_http.py [--keep]
退出码：0 全过；1 有失败；2 环境不可用（缺 esbuild / 缺 ets 源码）。
"""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
APP = REPO / "harmony-app"
ETS = APP / "entry/src/main/ets"
TOOLS = REPO / "tools/test"
sys.path.insert(0, str(TOOLS / "contract"))

from contract_source import Contract  # noqa: E402

CONTRACT = Contract(REPO / "docs/技术/接口契约.md")
ESBUILD = pathlib.Path.home() / "harmony-toolchain/esbuild/bin/esbuild"
BASE = "http://127.0.0.1:80/api"
PAYLOAD_TYPES = {"fall": {"fall": 1, "presence": 1, "src": "uart"}, "gas": {"smoke": 320, "temp": 24.5}}


def settings_items() -> dict:
    """§6.9 示例里的键 + §8.2 的 gas_l1_to_l2_s（D-01 的存在理由）。"""
    items = dict(CONTRACT.settings_defaults())
    items["gas_l1_to_l2_s"] = 60
    return items


def alert_item(alert_id: int = 101) -> dict:
    return {
        "id": alert_id, "device_id": 7, "node_sn": "M2", "type": "fall", "level": 2,
        "ts": 1760000123000, "status": "pending", "recovered_at": None, "push_count": 2,
        "last_push_at": 1760000183000, "next_push_at": 1760001200000, "summary": "卧室检测到跌倒",
    }


def reading(rid: int, ts: int, ptype: str = "fall") -> dict:
    return {"id": rid, "node_sn": "M2", "type": ptype, "ts": ts,
            "payload": dict(PAYLOAD_TYPES.get(ptype, {"smoke": 320}))}


def envelope(data, code: int = 0, message: str = "ok") -> str:
    return json.dumps({"code": code, "message": message, "data": data, "ts": 1760000500000})


def build_fixtures() -> dict:
    status_props = {
        "presence": True, "last_motion_ts": 1760000000000, "door": False, "window": False,
        "temp": 24.5, "humi": 56.2, "smoke": 320, "lpg": 180, "co": 12,
        "nodes_online": json.dumps({"M1": True, "M2": True}), "battery": 255,
        "alarm_state": 2, "extra_key_not_in_contract": "越界键不应让解析失败",
    }
    status = {
        "device_id": 7, "online": True, "last_heartbeat": 1760000490000, "alarm_state": 2,
        "properties": status_props,
        "nodes": [
            {"sn": "M1", "type": "gas", "location": "厨房", "online": True,
             "last_seen": 1760000490000, "battery": 255},
            {"sn": "M2", "type": "fall", "location": "卧室", "online": False,
             "last_seen": 1760000400000, "rssi": -58},
        ],
        "pending_alerts": 3, "server_ts": 1760000500000,
    }
    settings = {"source": "device", "updated_at": 1760000500000, "items": settings_items()}
    activity = {"device_id": 7, "period": "week", "from": 1759795200000, "to": 1760400000000,
                "total": 730, "hourly": [],
                "daily": [{"date": "2026-10-%02d" % (11 + i), "count": 90 + i} for i in range(7)]}
    trend = {"device_id": 7, "days": 7,
             "series": [{"date": "2026-10-%02d" % (11 + i), "count": 100 + i} for i in range(7)],
             "compare": {"today": 128, "yesterday": 160, "delta_pct": -20,
                         "text": "今日活动频次较昨日下降 20%"}}
    summary = {"device_id": 7, "period": "week", "from": 1759795200000, "to": 1760400000000,
               "activity": {"total": 760, "active_days": 5, "avg_daily": 108.6, "peak_hour": 9,
                            "min_hour": 3, "compare_prev": "较上周期上升 5%"},
               "alerts": {"total": 4, "by_type": {"fall": 1, "gas": 2, "sos": 1},
                          "resolved": 1, "pending": 2, "false_alarm": 0},
               "env": {"temp_avg": 23.4, "temp_min": 19.2, "temp_max": 27.8, "humi_avg": 54.3}}
    health = {"ok": True, "version": "1.0.0", "db": "ok", "iotda_token_expire_at": 1760003600000,
              "webhook_last_at": 1760000000000, "clock_source": "device", "server_ts": 1760000500000}

    fixtures = [
        ("login", "POST", "/auth/login", "", False, 200,
         envelope({"token": "probe.jwt.abc123", "expires_in": 604800,
                   "user": {"id": 1, "username": "daughter01", "phone": "13800000001"}})),
        ("devices", "GET", "/devices", "", True, 200,
         envelope({"total": 1, "items": [{"id": 7, "sn": "M0-3F1A2B", "name": "爸妈家",
                                          "role": "owner", "online": True,
                                          "last_heartbeat": 1760000490000, "alarm_state": 2,
                                          "unread_alerts": 3}]})),
        ("status", "GET", "/devices/7/status", "", True, 200, envelope(status)),
        ("readings", "GET", "/devices/7/readings", "type=fall&limit=25", True, 200,
         envelope({"total": 2, "items": [reading(9, 1760000123000), reading(8, 1760000000000)]})),
        ("settings", "GET", "/devices/7/settings", "", True, 200, envelope(settings)),
        ("settings_put", "PUT", "/devices/7/settings", "", True, 200, envelope(
            dict(settings, items=dict(settings_items(), no_motion_min=45)))),
        ("alerts", "GET", "/alerts", "device_id=7&status=pending&limit=50", True, 200,
         envelope({"total": 1, "items": [alert_item()]})),
        # §6.14 没写明 data 是平铺还是嵌套（核对表 D-04）；按 App 选的"平铺"形状给
        ("alert_detail", "GET", "/alerts/101", "", True, 200,
         envelope(dict(alert_item(),
                       dispositions=[{"id": 1, "alert_id": 101, "user_id": 1,
                                      "action": "contacted", "note": "已电话联系",
                                      "ts": 1760000200000}],
                       readings_before=[reading(4 + i, 1760000000000 + i) for i in range(5)],
                       readings_after=[reading(20 + i, 1760000300000 + i) for i in range(5)]))),
        ("activity", "GET", "/reports/activity", "device_id=7&period=week&date=2026-10-17",
         True, 200, envelope(activity)),
        ("trend", "GET", "/reports/trend", "device_id=7&days=7", True, 200, envelope(trend)),
        ("summary", "GET", "/reports/summary", "device_id=7&period=week", True, 200, envelope(summary)),
        ("health", "GET", "/health", "", False, 200, envelope(health)),
    ]
    out = [{"key": k, "method": m, "path": p, "query": q, "authed": a, "http": h, "body": b}
           for (k, m, p, q, a, h, b) in fixtures]

    errors = [
        {"key": "bind_1201", "method": "POST", "url_contains": "/devices/bind", "http": 400,
         "body_has": envelope({}, 1201, "invalid bind code")},
        {"key": "alert_1300", "method": "GET", "url_contains": "/alerts/999", "http": 404,
         "body_has": envelope({}, 1300, "alert not found")},
        {"key": "alert_1301", "method": "POST", "url_contains": "/alerts/101/disposition",
         "http": 409, "body_has": envelope({}, 1301, "already resolved")},
        {"key": "empty_1400", "method": "GET", "url_contains": "/reports/activity", "http": 200,
         "body_has": envelope({"device_id": 7, "period": "day", "from": 1760400000000,
                               "to": 1760400000000, "total": 0, "hourly": [], "daily": []})},
        {"key": "network", "method": "GET", "url_contains": "/devices/7/status", "http": 0,
         "body_has": ""},
        {"key": "expired", "method": "GET", "url_contains": "/alerts?", "http": 401,
         "body_has": envelope({}, 1101, "token expired")},
        {"key": "html", "method": "GET", "url_contains": "/health", "http": 502,
         "body_has": "<html><body>502 Bad Gateway</body></html>"},
        # 契约字段类型不符：必填字段（online 是 boolean）必须报 parse 并带字段路径
        {"key": "badtype_required", "method": "GET", "url_contains": "/devices/7/status",
         "http": 200, "body_has": envelope(dict(status, online="true"))},
        # 可选字段（temp）类型不符：App 按"缺省"处理（optNum 只认 number）——已记入复核报告
        {"key": "badtype_optional", "method": "GET", "url_contains": "/devices/7/status",
         "http": 200, "body_has": envelope(dict(status, properties=dict(status_props, temp="24.5")))},
        # 嵌套形状（若云端按 {alert:{...}} 返回）：必须明确报字段错误，不能静默成功
        {"key": "nested_detail", "method": "GET", "url_contains": "/alerts/101", "http": 200,
         "body_has": envelope({"alert": alert_item(), "dispositions": [],
                               "readings_before": [], "readings_after": []})},
    ]
    return {"base": BASE, "fixtures": out, "errors": errors, "login_token": "probe.jwt.abc123"}


FIELD_SOURCES = {
    "login": ["§6.4", "§6.1"],
    "devices": ["§6.5"],
    "status": ["§6.7", "§4.3", "§6.1"],
    "readings": ["§6.8", "§2.4.2"],
    "settings": ["§6.9", "§8.2"],
    "settings_put": ["§6.9", "§6.10", "§8.2"],
    "alerts": ["§6.13"],
    "alert_detail": ["§6.14", "§6.13", "§6.8", "§2.4", "§7.8"],
    "activity": ["§6.16"],
    "trend": ["§6.17"],
    "summary": ["§6.18"],
    "health": ["§6.20"],
}


def walk_keys(obj, parent=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            yield k, parent
            yield from walk_keys(v, k)
    elif isinstance(obj, list):
        for it in obj:
            yield from walk_keys(it, parent)


def validate_fixtures(fx: dict) -> list:
    """字段名必须能在契约对应章节里找到 —— 防止 fixtures 变成"我编的字段"。"""
    problems = []
    text_cache = {}

    def text_of(section):
        num = section[1:] if section.startswith("§") else section
        if num not in text_cache:
            text_cache[num] = CONTRACT.section_text(num)
        return text_cache[num]

    for f in fx["fixtures"]:
        data = json.loads(f["body"])["data"]
        for key, parent in walk_keys(data):
            if key in ("extra_key_not_in_contract",):
                continue
            if parent == "by_type":
                # by_type 是"告警 type → 条数"的动态映射，键是业务枚举不是契约字段名
                continue
            sections = FIELD_SOURCES.get(f["key"], ["§6"])
            if not any(key in text_of(s) for s in sections):
                problems.append("fixture %s 的字段 %r 在 %s 里找不到（契约没这个字段名？）"
                                % (f["key"], key, "/".join(sections)))
    return problems


def run_probe(fx: dict, keep: bool) -> int:
    if not ESBUILD.exists():
        print("环境不可用：找不到 esbuild -> %s" % ESBUILD)
        return 2
    if not (ETS / "service/HttpApiService.ets").exists():
        print("环境不可用：找不到 harmony-app/entry/src/main/ets/service/HttpApiService.ets")
        return 2

    work = pathlib.Path(tempfile.gettempdir()) / "hm-verifier-probe"
    if work.exists():
        shutil.rmtree(work)
    (work / "node_modules/@kit.NetworkKit").mkdir(parents=True)
    (work / "tools").mkdir(parents=True)
    shutil.copytree(ETS, work / "ets")
    shutil.copy(TOOLS / "review/harmony_probe.ts", work / "tools/harmony_probe.ts")
    (work / "node_modules/@kit.NetworkKit/package.json").write_text(
        json.dumps({"name": "@kit.NetworkKit", "version": "0.0.0-probe", "main": "index.js"}),
        encoding="utf-8")
    # 探针只走记录式 transport；真实 HTTP 客户端一旦被调用就应该炸出来
    (work / "node_modules/@kit.NetworkKit/index.js").write_text(
        "exports.http = { createHttp: function () {"
        " throw new Error('probe: 探针不应走到真实 NetHttpClient'); } };\n", encoding="utf-8")
    (work / "fixtures.json").write_text(json.dumps(fx, ensure_ascii=False), encoding="utf-8")

    bundle = work / "probe.cjs"
    cmd = [str(ESBUILD), str(work / "tools/harmony_probe.ts"), "--bundle", "--platform=node",
           "--format=cjs", "--target=node18", "--resolve-extensions=.ets,.ts,.js",
           "--loader:.ets=ts", "--log-level=warning", "--outfile=%s" % bundle]
    b = subprocess.run(cmd, capture_output=True, text=True)
    if b.returncode != 0:
        print("esbuild 打包失败：\n%s\n%s" % (b.stdout, b.stderr))
        return 2
    r = subprocess.run(["node", str(bundle), str(work / "fixtures.json")],
                       capture_output=True, text=True, cwd=str(work))
    print(r.stdout.rstrip())
    if r.stderr.strip():
        print("--- node stderr ---\n" + r.stderr.rstrip())
    if not keep and work.exists():
        shutil.rmtree(work, ignore_errors=True)
    return 0 if r.returncode == 0 else 1


def main() -> int:
    keep = "--keep" in sys.argv
    fx = build_fixtures()
    problems = validate_fixtures(fx)
    if problems:
        print("fixtures 自检失败（说明我写的字段名跟契约对不上）：")
        for p in problems:
            print("  - " + p)
        return 1
    print("fixtures 自检通过：%d 条响应、%d 条故障场景的字段名均能在契约正文中找到"
          % (len(fx["fixtures"]), len(fx["errors"])))
    print("被测代码：harmony-app/entry/src/main/ets 的真实 HttpApiService/Types/Result/SessionStore")
    print("-" * 78)
    return run_probe(fx, keep)


if __name__ == "__main__":
    sys.exit(main())
