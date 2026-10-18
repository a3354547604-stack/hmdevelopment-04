#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s05_rest.py -- REST 契约冒烟（§6/§9.1/§9.2），默认打 tools/test/mock_server.py。

两种跑法：
    python3 tools/test/suites/s05_rest.py                     # 起内存 mock（自证基线可跑）
    CONTRACT_TEST_BASE_URL=http://127.0.0.1:3000 python3 ...   # 打真实服务器（实现方验收）

验收口径（都是契约明写的）：
    - §6.1：HTTP 200 且 code≠0 不允许；信封四要素；分页 total/limit 上限 200
    - §9.2：业务码与 HTTP 状态码一一对应
    - §6.10/§6.15：写接口的参数校验和状态机副作用
"""

from __future__ import annotations

import json
import os
import sys
import threading
import urllib.error
import urllib.request

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import known_issues as ki             # noqa: E402          # noqa: E402
import mock_server                    # noqa: E402
from harness import CaseFail, SkipCase, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402

C = cs.load()
BASE = os.environ.get("CONTRACT_TEST_BASE_URL", "").rstrip("/")

# 运行期上下文：mock 端口、登录后的 token
CTX = {"base": BASE, "token": "", "username": "verifier_%d" % (os.getpid() % 100000)}
_SEEN: list = []


class Client:
    def __init__(self, base: str):
        self.base = base

    def call(self, method: str, path: str, body=None, token=None, headers=None):
        url = self.base + path
        data = None
        hdrs = {"Content-Type": "application/json"}
        if body is not None:
            data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        if token:
            hdrs["Authorization"] = "Bearer " + token
        if headers:
            hdrs.update(headers)
        req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=10) as resp:
                status, raw = resp.status, resp.read()
        except urllib.error.HTTPError as e:
            status, raw = e.code, e.read()
        try:
            payload = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            payload = {"_raw": raw[:200].decode("utf-8", "replace")}
        _SEEN.append((method, path, status, payload.get("code")))
        return status, payload


CLIENT: Client = None  # type: ignore[assignment]


def setup_module():
    global CLIENT
    if CTX["base"]:
        CLIENT = Client(CTX["base"])
        return None
    httpd = mock_server.create_server(port=0, provision=True)
    port = httpd.server_address[1]
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    CTX["base"] = "http://127.0.0.1:%d" % port
    CLIENT = Client(CTX["base"])
    return httpd


def _login():
    if CTX["token"]:
        return CTX["token"]
    c = CLIENT
    u = CTX["username"]
    st, payload = c.call("POST", "/api/auth/register",
                         {"username": u, "password": "verifier-pass-1"})
    if payload.get("code") not in (0, 1103):
        raise CaseFail("注册失败：HTTP %s %r" % (st, payload))
    st, payload = c.call("POST", "/api/auth/login", {"username": u, "password": "verifier-pass-1"})
    if payload.get("code") != 0:
        raise CaseFail("登录失败：HTTP %s %r" % (st, payload))
    CTX["token"] = payload["data"]["token"]
    return CTX["token"]


def _current_device():
    token = _login()
    st, payload = CLIENT.call("GET", "/api/devices", token=token)
    if payload.get("code") == 0 and payload["data"].get("items"):
        return payload["data"]["items"][0]["id"]
    # 新账号还没绑定：用 mock 的固定绑定码完成绑定（真实服务器上等价于用户扫码绑定）
    st, payload = CLIENT.call("POST", "/api/devices/bind",
                              {"sn": "M0-3F1A2B", "bind_code": "123456", "name": "测试网关"},
                              token=token)
    if payload.get("code") != 0:
        raise CaseFail("拿不到设备且绑定失败：HTTP %s %r" % (st, payload))
    return payload["data"]["device_id"]


# ---------------- 用例 ----------------
def case_no_http200_with_error_code():
    """§6.1：绝不允许 HTTP 200 配非 0 code。"""
    bad = [(m, p, s, c) for m, p, s, c in _SEEN if s == 200 and c not in (0,)]
    require(not bad, "出现 HTTP 200 且 code≠0：%r" % bad[:5])


def case_unauthorized():
    st, payload = CLIENT.call("GET", "/api/devices")
    require_eq(st, 401, "无 token 访问 /api/devices 的 HTTP 码")
    require_eq(payload["code"], 1100, "无 token 的业务码")


def case_envelope_shape():
    st, payload = CLIENT.call("GET", "/api/health")
    require_eq(st, 200, "health 应 200")
    for key in ("code", "message", "data", "ts"):
        require_in(key, payload, "§6.1 信封字段")
    require_eq(payload["code"], 0, "health code")


def case_endpoints_all_reachable():
    """§6.2 的每个接口都必须被服务识别（不是 404/1000 未知路径）。"""
    token = _login()
    dev = _current_device()
    mapping = [
        ("GET", "/api/health", None, None),
        ("POST", "/api/auth/login", {"username": CTX["username"], "password": "verifier-pass-1"}, None),
        ("POST", "/api/auth/register", {"username": CTX["username"], "password": "verifier-pass-1"}, None),
        ("GET", "/api/devices", None, token),
        ("POST", "/api/devices/bind", {"sn": "M0-3F1A2B", "bind_code": "000000", "name": "x"}, token),
        ("GET", "/api/devices/%s/status" % dev, None, token),
        ("GET", "/api/devices/%s/readings" % dev, None, token),
        ("GET", "/api/devices/%s/settings" % dev, None, token),
        ("PUT", "/api/devices/%s/settings" % dev, {"items": {"no_motion_min": 31}}, token),
        ("POST", "/api/devices/%s/config" % dev, {"keys": ["no_motion_min"]}, token),
        ("POST", "/api/devices/%s/demo" % dev, {"event": "fall"}, token),
        ("GET", "/api/alerts?device_id=%s" % dev, None, token),
        ("GET", "/api/alerts/1", None, token),
        ("POST", "/api/alerts/2/disposition", {"action": "checked"}, token),
        ("GET", "/api/reports/activity?device_id=%s" % dev, None, token),
        ("GET", "/api/reports/trend?device_id=%s" % dev, None, token),
        ("GET", "/api/reports/summary?device_id=%s" % dev, None, token),
        ("POST", "/api/push/register", {"push_token": "tok-abc", "platform": "harmony"}, token),
        ("POST", "/api/iotda/webhook", {"resource": "device.property"}, {"X-Auth-Token": mock_server.WEBHOOK_TOKEN}),
    ]
    declared = {(e["method"], e["path"]) for e in C.endpoints()}
    missing = []
    for method, path, body, tok in mapping:
        if body is None and method == "GET" and tok is None:
            headers = None
        else:
            headers = None
        if isinstance(tok, dict):
            st, payload = CLIENT.call(method, path, body, headers=tok)
        else:
            st, payload = CLIENT.call(method, path, body, token=tok)
        code = payload.get("code")
        if code == 1000 and str(payload.get("data", {}).get("path", "")).startswith("/api/"):
            missing.append((method, path, "服务未实现该路由"))
        # 契约总表里声明的接口，mock 必须能对上（允许尚未实现 → 报 SKIP 而不是静默通过）
    known_paths = {p.split("?")[0] for _m, p, _b, _t in mapping}
    for method, path in sorted(declared):
        base = path.split("{")[0].rstrip("/")
        if not any(k.startswith(base.split("?")[0][:14]) or base.startswith(k.split("?")[0][:14])
                   for k in known_paths):
            missing.append((method, path, "验收基线未覆盖该接口"))
    if missing:
        # 只有「已登记待收口」的接口才允许 SKIP（带原因 + 负责人卡号），其余一律 FAIL：
        # 静默 SKIP 会把「mock 少一个路由」长期藏起来。
        paths = {p for _m, p, _why in missing}
        if paths <= {"/api/time"}:
            ki.pending("COV-1", "未覆盖：%r" % sorted(paths))   # 抛 SkipCase，见下
        else:
            raise CaseFail("以下接口未覆盖且未登记（mock 未实现或基线未写）：%r" % missing[:6])


def case_register_and_login_errors():
    st, payload = CLIENT.call("POST", "/api/auth/register",
                              {"username": CTX["username"], "password": "verifier-pass-1"})
    require_eq(payload["code"], 1103, "重名注册")
    require_eq(st, 409, "1103 的 HTTP 码")
    st, payload = CLIENT.call("POST", "/api/auth/login",
                              {"username": CTX["username"], "password": "wrong-password"})
    require_eq(payload["code"], 1102, "密码错误")
    require_eq(st, 401, "1102 的 HTTP 码")


def case_bind_code_error():
    token = _login()
    st, payload = CLIENT.call("POST", "/api/devices/bind",
                              {"sn": "M0-3F1A2B", "bind_code": "999999", "name": "x"}, token)
    require_eq(payload["code"], 1201, "绑定码错误")
    require_eq(st, 400, "1201 的 HTTP 码")


def case_readings_limit_validation():
    token = _login()
    dev = _current_device()
    st, payload = CLIENT.call("GET", "/api/devices/%s/readings?limit=500" % dev, token=token)
    require_eq(payload["code"], 1002, "limit=500 应报超范围")
    require_eq(st, 400, "1002 的 HTTP 码")
    st, payload = CLIENT.call("GET", "/api/devices/%s/readings?limit=abc" % dev, token=token)
    require_eq(payload["code"], 1001, "limit=abc 应报格式错误")
    st, payload = CLIENT.call("GET", "/api/devices/%s/readings?limit=2" % dev, token=token)
    require_eq(payload["code"], 0, "limit=2 应成功")
    require_in("total", payload["data"], "分页应带 total")
    require(len(payload["data"]["items"]) <= 2, "返回条数应受 limit 约束")


def case_settings_validation_and_effects():
    token = _login()
    dev = _current_device()
    st, payload = CLIENT.call("PUT", "/api/devices/%s/settings" % dev,
                              {"items": {"not_a_key": 1}}, token=token)
    require_eq(payload["code"], 1003, "未知键应 1003")
    st, payload = CLIENT.call("PUT", "/api/devices/%s/settings" % dev,
                              {"items": {"no_motion_min": 1}}, token=token)
    require_eq(payload["code"], 1002, "越界值应 1002")
    require_in("allowed", payload["data"], "1002 应带 allowed")
    st, payload = CLIENT.call("PUT", "/api/devices/%s/settings" % dev,
                              {"items": {"no_motion_min": 45}}, token=token)
    require_eq(payload["code"], 0, "合法写入应成功")
    require_eq(payload["data"]["source"], "device", "写入后 source 应为 device")
    st, payload = CLIENT.call("GET", "/api/devices/%s/settings" % dev, token=token)
    require_eq(payload["data"]["items"]["no_motion_min"], 45, "写入生效")
    keys = set(payload["data"]["items"])
    declared = set(C.settings_defaults())
    require(not (declared - keys), "GET settings 缺 §6.9 声明的键：%r" % sorted(declared - keys))
    # v1.2 D-01：items 是闭集，gas_l1_to_l2_s 属 §8.2 算法参数、**不进 items**（多出即 FAIL）
    require(not (keys - declared),
            "GET settings 多出 §6.9 之外的键：%r（v1.2 D-01：gas_l1_to_l2_s 不进 items）"
            % sorted(keys - declared))


def case_demo_and_config_semantics():
    token = _login()
    dev = _current_device()
    st, payload = CLIENT.call("POST", "/api/devices/%s/demo" % dev, {"event": "earthquake"}, token=token)
    require_eq(payload["code"], 1206, "非法演示事件")
    st, payload = CLIENT.call("POST", "/api/devices/%s/demo" % dev, {"event": "fall"}, token=token)
    require_eq(payload["code"], 0, "合法演示应受理")
    for key in ("command_id", "request_id", "accepted", "dispatched_at", "timeout_s"):
        require_in(key, payload["data"], "§6.11 命令投递结构")
    blob = json.dumps(payload["data"], ensure_ascii=False)
    require("已成功" not in blob and "success" not in blob.lower(),
            "§6.11：下发接口绝不能返回“已成功”")
    # §6.11（PM 最终裁定，契约 L942）：accepted/rejected 是键名字符串数组，不是整型计数
    for key in ("accepted", "rejected"):
        value = payload["data"][key]
        require(isinstance(value, list) and all(isinstance(x, str) for x in value),
                "§6.11 %s 必须是键名字符串数组，实际 %r" % (key, value))
    require_eq(payload["data"]["accepted"], ["demo_trigger"], "demo 的 accepted 应为 demo_trigger")
    require_eq(payload["data"]["rejected"], [], "demo 无拒绝项")
    # config：一个可下发键 + 一个服务器判定键 → accepted/rejected 分流，reasons 键 ⊆ rejected
    st, payload = CLIENT.call("POST", "/api/devices/%s/config" % dev,
                              {"keys": ["no_motion_min", "alert_repush_s"]}, token=token)
    require_eq(payload["code"], 0, "config 受理")
    data = payload["data"]
    require_eq(data["accepted"], ["no_motion_min"], "可下发键进 accepted（元素=键名）")
    require_eq(data["rejected"], ["alert_repush_s"], "服务器判定键进 rejected（元素=键名）")
    require_eq(set(data.get("reasons", {})), {"alert_repush_s"}, "§6.11 reasons 键 ⊆ rejected")
    require(all(isinstance(v, str) and v for v in data["reasons"].values()),
            "§6.11 reasons 文案必须为非空字符串")
    # 全量下发：accepted + rejected 恰覆盖 §6.9 全部 21 键（不重不漏、无整型计数）
    st, payload = CLIENT.call("POST", "/api/devices/%s/config" % dev, {}, token=token)
    data = payload["data"]
    require_eq(sorted(data["accepted"] + data["rejected"]), sorted(C.settings_defaults()),
               "§6.11 全量下发的 accepted+rejected 应恰为 §6.9 的键集")


def case_alerts_detail_and_disposition():
    token = _login()
    dev = _current_device()
    st, payload = CLIENT.call("GET", "/api/alerts?device_id=%s&status=pending" % dev, token=token)
    require_eq(payload["code"], 0, "告警列表")
    items = payload["data"]["items"]
    require(items, "mock 应有一条 pending 告警")
    aid = items[0]["id"]
    for key in ("id", "device_id", "node_sn", "type", "level", "ts", "status", "push_count",
                "last_push_at", "next_push_at", "summary"):
        require_in(key, items[0], "§6.13 列表字段")
    st, payload = CLIENT.call("GET", "/api/alerts/%d" % aid, token=token)
    require_eq(payload["code"], 0, "告警详情")
    require_in("dispositions", payload["data"], "§6.14 详情应带 dispositions")
    st, payload = CLIENT.call("POST", "/api/alerts/%d/disposition" % aid,
                              {"action": "checked", "note": "已电话确认"}, token=token)
    require_eq(payload["code"], 0, "处置应成功")
    require_eq(payload["data"]["status"], "resolved", "§6.15 处置后状态")
    st, payload = CLIENT.call("POST", "/api/alerts/%d/disposition" % aid,
                              {"action": "checked"}, token=token)
    require_eq(payload["code"], 1301, "重复处置应 1301")
    st, payload = CLIENT.call("POST", "/api/alerts/99999/disposition",
                              {"action": "checked"}, token=token)
    require_eq(payload["code"], 1300, "不存在的告警应 1300")


def case_reports_shapes():
    token = _login()
    dev = _current_device()
    st, payload = CLIENT.call("GET", "/api/reports/activity?device_id=%s&period=day" % dev, token=token)
    require_eq(payload["code"], 0, "activity day")
    data = payload["data"]
    require_eq(len(data["hourly"]), 24, "day 应有 24 条 hourly")
    require_eq(len(data["daily"]), 1, "day 应有 1 条 daily")
    st, payload = CLIENT.call("GET", "/api/reports/activity?device_id=%s&period=week" % dev, token=token)
    require_eq(len(payload["data"]["daily"]), 7, "week 应有 7 条 daily")
    require_eq(payload["data"].get("hourly", []), [], "week 不应返回 hourly")
    st, payload = CLIENT.call("GET", "/api/reports/activity", token=token)
    require_eq(payload["code"], 1000, "缺 device_id 应 1000")
    st, payload = CLIENT.call("GET", "/api/reports/trend?device_id=%s&days=7" % dev, token=token)
    require_in("compare", payload["data"], "trend 应带 compare")
    cmp7 = payload["data"]["compare"]
    require_eq(cmp7.get("title"), "今日活动频次", "§6.17 compare.title 固定文案（必现）")
    require_in("today", cmp7, "§6.17 compare.today 必现")
    if "yesterday" in cmp7:
        require(not isinstance(cmp7.get("delta_pct"), bool) and isinstance(cmp7.get("delta_pct"), int),
                "§6.17 有基线时 delta_pct 必须是整数百分比，实际 %r" % cmp7.get("delta_pct"))
        require_eq(cmp7["delta_pct"],
                   int(round((cmp7["today"] - cmp7["yesterday"]) / cmp7["yesterday"] * 100)),
                   "§6.17 delta_pct 规则：round((today-yesterday)/yesterday*100)")
        require(isinstance(cmp7.get("text"), str) and cmp7["text"], "§6.17 有基线必须有 text 规则行")
    else:
        require("delta_pct" not in cmp7 and "text" not in cmp7,
                "§6.17 无昨日基线时省略 delta_pct/text，只留 title/today")
    # 无基线场景：days=1 → 没有昨天，compare 只留 title/today（v1.2 明文）
    st, payload = CLIENT.call("GET", "/api/reports/trend?device_id=%s&days=1" % dev, token=token)
    cmp1 = payload["data"]["compare"]
    require_eq(sorted(cmp1), ["title", "today"], "§6.17 无基线只留 title/today")
    require_eq(cmp1["title"], "今日活动频次", "§6.17 无基线时 title 仍必现")
    st, payload = CLIENT.call("GET", "/api/reports/summary?device_id=%s" % dev, token=token)
    for key in ("activity", "alerts", "env"):
        require_in(key, payload["data"], "§6.18 summary 结构")


def case_push_idempotent():
    token = _login()
    st, payload = CLIENT.call("POST", "/api/push/register",
                              {"push_token": "tok-dup", "platform": "harmony"}, token=token)
    require_eq(payload["code"], 0, "注册推送 token")
    require_eq(payload["data"]["registered"], True, "§6.19 registered")
    st, payload = CLIENT.call("POST", "/api/push/register",
                              {"push_token": "tok-dup", "platform": "harmony"}, token=token)
    require_eq(payload["code"], 0, "同 token 重复注册应幂等")
    st, payload = CLIENT.call("POST", "/api/push/register",
                              {"push_token": "tok-dup", "platform": "android"}, token=token)
    require_eq(payload["code"], 1001, "platform 只允许 harmony")


def case_webhook_token():
    st, payload = CLIENT.call("POST", "/api/iotda/webhook", {"resource": "device.property"})
    require_eq(payload["code"], 1502, "webhook token 错误应 1502")
    require_eq(st, 401, "1502 的 HTTP 码")
    st, payload = CLIENT.call("POST", "/api/iotda/webhook", {"resource": "device.property"},
                              headers={"X-Auth-Token": mock_server.WEBHOOK_TOKEN})
    require_eq(payload["code"], 0, "webhook 正常接收")


def case_no_http200_with_error_code_final():
    case_no_http200_with_error_code()


CASES = [
    ("§6.1 信封与 health 无鉴权", case_envelope_shape),
    ("§6.1 无 token 401/1100", case_unauthorized),
    ("§6.2 接口总表逐条可达", case_endpoints_all_reachable),
    ("§6.3/§6.4 注册登录错误码", case_register_and_login_errors),
    ("§6.6 绑定码错误 1201", case_bind_code_error),
    ("§6.8 读数分页与越界", case_readings_limit_validation),
    ("§6.9/§6.10 settings 校验与生效", case_settings_validation_and_effects),
    ("§6.11/§6.12 命令投递语义", case_demo_and_config_semantics),
    ("§6.13–§6.15 告警与处置状态机", case_alerts_detail_and_disposition),
    ("§6.16–§6.18 报表结构", case_reports_shapes),
    ("§6.19 推送注册幂等", case_push_idempotent),
    ("§5 webhook token 校验", case_webhook_token),
    ("§6.1 HTTP 200 不得配非 0 code", case_no_http200_with_error_code_final),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s05_rest", "REST 契约冒烟（mock %s）" % (BASE or "内置"))
    httpd = setup_module()
    try:
        run_cases(suite, CASES, verbose)
    finally:
        if httpd is not None:
            httpd.shutdown()
    suite.notes.append("被测服务：%s" % CTX["base"])
    suite.notes.append("请求数：%d" % len(_SEEN))
    return suite


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
