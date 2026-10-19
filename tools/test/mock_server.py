#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""mock_server.py -- 按《接口契约》§6（+§12 可选）实现的最小参考服务。

用途
    1. 验收基线自证：验收脚本自身要「跑得通」，不能只在真空里断言。
    2. 实现方对接：server 员工可以直接对拍字段/错误码；App 员工在服务器没起时也能联调。
    3. 契约探针：mock 与契约冲突时，s05_rest 会红——按契约改 mock 或改契约，不许和稀泥。

不是产品代码：内存存储、无并发保护、不做鉴权强度、不写库。
命令行：
    python3 tools/test/mock_server.py --port 8801 [--provision]
"""

from __future__ import annotations

import argparse
import json
import re
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional, Tuple

NOW_MS = lambda: int(time.time() * 1000)  # noqa: E731

DEFAULT_SETTINGS = {
    "no_motion_min": 30, "presence_hold_s": 60, "gas_t1_mv": 1500, "gas_t2_mv": 2500,
    "gas_l1_hold_s": 10, "gas_l2_hold_s": 6, "gas_recover_mv": 200, "gas_recover_hold_s": 30,
    "away_after_open_s": 600, "door_open_alarm_s": 180, "report_interval_s": 60,
    "node_offline_timeout_s": 9, "gateway_offline_timeout_s": 180,
    "temp_high_c": 35.0, "temp_low_c": 5.0, "humi_high_pct": 85.0, "humi_low_pct": 20.0,
    "env_hold_min": 30, "alert_repush_s": [0, 60, 300], "alert_suppress_s": 60,
    "fall_suppress_s": 30,
}

RANGES = {
    "no_motion_min": (5, 120), "presence_hold_s": (10, 600), "gas_t1_mv": (200, 4000),
    "gas_t2_mv": (500, 5000), "gas_l1_hold_s": (1, 600), "gas_l2_hold_s": (1, 600),
    "gas_recover_mv": (0, 1000), "gas_recover_hold_s": (1, 3600),
    "away_after_open_s": (60, 3600), "door_open_alarm_s": (10, 3600),
    "report_interval_s": (10, 3600), "node_offline_timeout_s": (3, 120),
    "gateway_offline_timeout_s": (30, 3600), "temp_high_c": (-40, 125), "temp_low_c": (-40, 125),
    "humi_high_pct": (0, 100), "humi_low_pct": (0, 100), "env_hold_min": (1, 1440),
    "alert_suppress_s": (0, 3600), "fall_suppress_s": (0, 3600),
}

# §6.11：这些键由服务器判定、不经过设备（与 server/app/settings_keys.py 的 dispatchable=False 同集合）
# → 出现在 config 请求里时落 rejected 并带 reasons，其余落 accepted。
SERVER_SIDE_KEYS = {
    "node_offline_timeout_s", "gateway_offline_timeout_s", "temp_high_c", "temp_low_c",
    "humi_high_pct", "humi_low_pct", "env_hold_min", "alert_repush_s", "alert_suppress_s",
    "door_open_alarm_s",
}

ERR = {
    0: "ok", 1000: "缺少必填参数", 1001: "参数格式错误", 1002: "参数超出允许范围",
    1003: "未知参数名", 1004: "时间范围非法", 1100: "未提供凭证", 1101: "凭证过期",
    1102: "账号或密码错误", 1103: "用户名已存在", 1104: "无权操作该设备",
    1105: "请求过于频繁", 1200: "设备不存在", 1201: "绑定码错误",
    1202: "设备已被其他账号绑定", 1203: "绑定码已过期", 1204: "设备离线无法下发",
    1205: "云端下发失败", 1206: "演示事件非法", 1300: "告警不存在", 1301: "告警已处置",
    1302: "处置动作非法", 1400: "报表无数据", 1500: "数据库错误", 1501: "上游 IoTDA 不可用",
    1502: "webhook 校验失败", 1600: "网关不在配网模式", 1601: "配网窗口已超时",
    1602: "Wi-Fi 连接失败", 1603: "已有任务在进行", 1604: "节点不在扫描列表 / MAC 非法",
    1605: "汇聚未连接或离线", 1606: "白名单已满",
}

HTTP_FOR = {
    0: 200, 1000: 400, 1001: 400, 1002: 400, 1003: 400, 1004: 400, 1100: 401, 1101: 401,
    1102: 401, 1103: 409, 1104: 403, 1105: 429, 1200: 404, 1201: 400, 1202: 409, 1203: 410,
    1204: 409, 1205: 502, 1206: 400, 1300: 404, 1301: 409, 1302: 400, 1400: 200,
    1500: 500, 1501: 502, 1502: 401, 1600: 409, 1601: 410, 1602: 400, 1603: 409, 1604: 400,
    1605: 409, 1606: 409,
}

WEBHOOK_TOKEN = "mock-iotda-token"


class State:
    def __init__(self, provision_timeout_s: float = 600.0) -> None:
        self.users: Dict[str, Dict[str, Any]] = {}
        self.tokens: Dict[str, str] = {}
        self.devices: Dict[str, Dict[str, Any]] = {
            "dev-1": {"id": "dev-1", "sn": "M0-3F1A2B", "name": "爷爷家网关", "role": "owner",
                      "online": True, "last_heartbeat": NOW_MS(), "alarm_state": 0,
                      "unread_alerts": 1, "bind_code": "123456", "bind_code_ts": NOW_MS(),
                      "iotda_device_id": "6a1b2c3d4e5f"},
        }
        self.user_devices: List[Tuple[str, str]] = []          # (username, device_id)
        self.settings = {"dev-1": dict(DEFAULT_SETTINGS)}
        self.settings_source = {"dev-1": "local"}
        self.alerts: List[Dict[str, Any]] = [
            {"id": 1, "device_id": "dev-1", "node_sn": "M2", "type": "fall", "level": 2,
             "ts": NOW_MS() - 60000, "status": "pending", "recovered_at": None, "push_count": 1,
             "last_push_at": NOW_MS() - 60000, "next_push_at": NOW_MS() + 540000,
             "summary": "卧室检测到跌倒"},
            {"id": 3, "device_id": "dev-1", "node_sn": "M4", "type": "sos", "level": 2,
             "ts": NOW_MS() - 30000, "status": "pending", "recovered_at": None, "push_count": 0,
             "last_push_at": None, "next_push_at": NOW_MS() + 60000,
             "summary": "收到紧急求助"},
            {"id": 2, "device_id": "dev-1", "node_sn": "M1", "type": "gas", "level": 1,
             "ts": NOW_MS() - 120000, "status": "resolved", "recovered_at": NOW_MS() - 60000,
             "push_count": 3, "last_push_at": NOW_MS() - 60000, "next_push_at": None,
             "summary": "厨房燃气浓度偏高"},
        ]
        self.dispositions: List[Dict[str, Any]] = []
        self.readings: List[Dict[str, Any]] = [
            {"id": i, "node_sn": "M1", "type": "gas", "ts": NOW_MS() - i * 2000,
             "payload": {"smoke": 300 + i, "lpg": 100, "co": 5, "temp": 24.5, "humi": 55.0}}
            for i in range(1, 6)
        ]
        self.pushes: Dict[str, Dict[str, Any]] = {}
        # ---- §12 本地配网状态 ----
        self.provision_mode = True          # 网关是否处于配网模式（长按进入）
        self.provision_timeout_s = provision_timeout_s
        self.provision_deadline = NOW_MS() + int(provision_timeout_s * 1000)
        self.wifi: Dict[str, Any] = {}      # 家庭 Wi-Fi 配置（不回显口令）
        self.mode = "ap"                    # idle/ap/connecting/sta/ap_sta
        self.busy = ""
        self.scan = {"id": 0, "state": "idle", "scanned_at": 0, "duration_s": 0, "nodes": []}
        self.bound: List[Dict[str, str]] = [
            {"sn": "M1", "mac": "11:22:33:44:55:01", "alias": "厨房"},
            {"sn": "M2", "mac": "11:22:33:44:55:02", "alias": "卧室"},
        ]
        self.scan_catalog = [
            {"sn": "M1", "mac": "11:22:33:44:55:01", "rssi": -45, "role": 1},
            {"sn": "M2", "mac": "11:22:33:44:55:02", "rssi": -62, "role": 1},
            {"sn": "M3", "mac": "11:22:33:44:55:03", "rssi": -66, "role": 1},
            {"sn": "M4", "mac": "11:22:33:44:55:04", "rssi": -58, "role": 1},
            {"sn": "M5", "mac": "11:22:33:44:55:05", "rssi": -70, "role": 1},
            {"sn": "?", "mac": "11:22:33:44:55:06", "rssi": -73, "role": 1},
            {"sn": "?", "mac": "11:22:33:44:55:07", "rssi": -75, "role": 1},
            {"sn": "?", "mac": "11:22:33:44:55:08", "rssi": -77, "role": 1},
            {"sn": "?", "mac": "11:22:33:44:55:99", "rssi": -81, "role": 1},
        ]
        self.hub_online = True
        self.scan_ready = True
        self.nodes = [
            {"sn": "M1", "type": "gas", "location": "厨房", "online": True,
             "last_seen": NOW_MS() - 30000, "rssi": -62, "battery": 255},
            {"sn": "M2", "type": "fall", "location": "卧室", "online": False,
             "last_seen": NOW_MS() - 90000, "rssi": -71, "battery": 88},
        ]
        self.webhook_last_at = NOW_MS() - 5000


def envelope(code: int, data: Any = None, message: str = "") -> Tuple[int, Dict[str, Any]]:
    return HTTP_FOR.get(code, 500), {
        "code": code, "message": message or ERR.get(code, "error"),
        "data": data if data is not None else {}, "ts": NOW_MS(),
    }


def _limit(params: Dict[str, str], default: int = 50) -> Tuple[Optional[int], Optional[Tuple[int, Dict[str, Any]]]]:
    raw = params.get("limit")
    if raw is None:
        return default, None
    try:
        val = int(raw)
    except ValueError:
        return None, envelope(1001, {"field": "limit"})
    if val < 1 or val > 200:
        return None, envelope(1002, {"field": "limit", "allowed": [1, 200]})
    return val, None


class Handler(BaseHTTPRequestHandler):
    server_version = "ElderCareMock/0.1"
    state: State
    provision: bool = False

    # ---------- 工具 ----------
    def log_message(self, fmt, *args):        # 验收跑测试时不要把访问日志刷满屏
        if getattr(self.server, "verbose", False):
            BaseHTTPRequestHandler.log_message(self, fmt, *args)

    def _user(self) -> Optional[str]:
        auth = self.headers.get("Authorization", "")
        m = re.fullmatch(r"Bearer (.+)", auth.strip())
        if not m:
            return None
        return self.state.tokens.get(m.group(1))

    def _body(self) -> Dict[str, Any]:
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        raw = self.rfile.read(length)
        try:
            val = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return {}
        return val if isinstance(val, dict) else {}

    def _params(self) -> Dict[str, str]:
        if "?" not in self.path:
            return {}
        from urllib.parse import parse_qs, urlparse
        return {k: v[0] for k, v in parse_qs(urlparse(self.path).query).items()}

    def _send(self, status: int, payload: Dict[str, Any],
              headers: Optional[Dict[str, str]] = None) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _route(self, method: str) -> None:
        from urllib.parse import urlparse
        path = urlparse(self.path).path
        st = self.state
        params = self._params()
        body = self._body()

        # ---- §12 本地配网 API（无 JWT，仅配网窗口内）----
        if path.startswith("/api/provision") or path.startswith("/api/pair"):
            if not self.provision or not st.provision_mode:
                return self._send(*envelope(1600))          # 409 网关不在配网模式
            if NOW_MS() > st.provision_deadline:
                return self._send(*envelope(1601))          # 410 配网窗口已超时
            if path == "/api/provision/status":
                data = {"mode": st.mode, "ssid": "ElderCare-1A2B", "device_sn": "M0-3F1A2B",
                        "firmware": "0.1.0",
                        "remain_s": max(0, int((st.provision_deadline - NOW_MS()) // 1000)),
                        "hub_online": st.hub_online, "bound_count": len(st.bound),
                        "slots_free": max(0, 8 - len(st.bound))}
                if st.wifi.get("ssid"):
                    data["sta_ssid"] = st.wifi["ssid"]      # 永不回显口令
                if st.busy:
                    data["busy"] = st.busy
                return self._send(*envelope(0, data))
            if path == "/api/provision/wifi":
                ssid, pwd = body.get("ssid"), body.get("password")
                if not ssid or pwd is None:
                    return self._send(*envelope(1000, {"field": "ssid" if not ssid else "password"}))
                if not 1 <= len(ssid.encode("utf-8")) <= 32:
                    return self._send(*envelope(1002, {"field": "ssid", "allowed": [1, 32]}))
                if not 8 <= len(pwd.encode("utf-8")) <= 63:
                    return self._send(*envelope(1002, {"field": "password", "allowed": [8, 63]}))
                if st.busy == "connect":
                    return self._send(*envelope(1603))       # 409 已有任务在进行
                if ssid not in ("HOME-2.4G", "eldercare-ap"):
                    # 预检看不到该热点：不落盘，direct 1602 reason=not_found
                    return self._send(*envelope(1602, {"reason": "not_found"}))
                st.wifi = {"ssid": ssid}
                st.mode = "connecting"
                st.busy = "connect"
                st.provision_deadline = min(st.provision_deadline, NOW_MS() + 90000)
                return self._send(*envelope(0, {"saved": True, "connecting": True, "ssid": ssid,
                                                "settle_s": 90,
                                                "precheck": {"ap_found": True, "rssi": -52, "ch": 6}}))
            if path == "/api/pair/scan":
                if not st.hub_online:
                    return self._send(*envelope(1605))       # 409 汇聚离线
                if st.busy == "scan":
                    return self._send(*envelope(1603))       # 409 扫描进行中
                duration = body.get("duration_s", 15)
                if not isinstance(duration, int) or not 3 <= duration <= 60:
                    return self._send(*envelope(1002, {"field": "duration_s", "allowed": [3, 60]}))
                st.scan = {"id": st.scan["id"] + 1, "state": "scanning",
                           "scanned_at": 0, "duration_s": duration, "nodes": []}
                return self._send(*envelope(0, {"started": True, "id": st.scan["id"],
                                                "duration_s": duration, "estimate_s": duration + 3}))
            if path == "/api/pair/nodes":
                if st.scan["state"] == "scanning":           # mock 里扫描立刻完成：这里先落结果
                    st.scan["state"] = "done"
                    st.scan["scanned_at"] = NOW_MS()
                    st.scan["nodes"] = [dict(n) for n in st.scan_catalog]
                nodes = []
                for n in st.scan["nodes"]:
                    item = {"sn": n["sn"], "mac": n["mac"], "bind": any(b["mac"] == n["mac"] for b in st.bound)}
                    if n.get("rssi") is not None:
                        item["rssi"] = n["rssi"]
                    item["role"] = n.get("role", 1)
                    if item["bind"]:
                        item["online"] = st.hub_online
                        item["last_seen"] = NOW_MS() - 30000
                    nodes.append(item)
                return self._send(*envelope(0, {"id": st.scan["id"], "scan_state": st.scan["state"],
                                                "scanned_at": st.scan["scanned_at"] or NOW_MS(),
                                                "truncated": len(nodes) > 8,
                                                "bound": st.bound,
                                                "slots_free": max(0, 8 - len(st.bound)),
                                                "nodes": nodes}))
            if path == "/api/pair/bind":
                op = body.get("op")
                if op not in ("add", "del", "clear", "list"):
                    return self._send(*envelope(1002, {"field": "op",
                                                       "allowed": ["add", "del", "clear", "list"]}))
                if op == "list":
                    return self._send(*envelope(0, {"op": "list", "bound": st.bound,
                                                    "slots_free": max(0, 8 - len(st.bound))}))
                if op == "clear":
                    if body.get("confirm") != "CLEAR":
                        return self._send(*envelope(1000, {"field": "confirm"}))
                    st.bound = []
                    return self._send(*envelope(0, {"op": "clear", "bound": [], "slots_free": 8}))
                sn, mac = body.get("sn"), body.get("mac")
                if not sn or (op == "add" and not mac):
                    return self._send(*envelope(1000, {"field": "sn" if not sn else "mac"}))
                if op == "del":
                    st.bound = [b for b in st.bound if b["sn"] != sn]
                    return self._send(*envelope(0, {"op": "del", "sn": sn, "bound": st.bound,
                                                    "slots_free": max(0, 8 - len(st.bound))}))
                # add：先扫后绑（§12.9）
                scanned = {n["mac"] for n in st.scan["nodes"]}
                if st.scan["state"] != "done" or mac not in scanned or \
                        (st.scan["scanned_at"] and NOW_MS() - st.scan["scanned_at"] > 300000):
                    return self._send(*envelope(1604, {"mac": mac}))
                if any(b["mac"] == mac for b in st.bound):
                    pass                                     # 幂等：重复 add 同一 MAC 不报错
                elif len(st.bound) >= 8:
                    return self._send(*envelope(1606, {"slots_free": 0}))
                else:
                    st.bound.append({"sn": sn, "mac": mac, "alias": body.get("alias", "")})
                if not st.hub_online:
                    return self._send(*envelope(1605))
                return self._send(*envelope(0, {"op": "add", "sn": sn, "bound": st.bound,
                                                "slots_free": max(0, 8 - len(st.bound)),
                                                "connected": st.hub_online}))
        # ---- §6.20 health（无鉴权）----
        if path == "/api/health":
            return self._send(*envelope(0, {"ok": True, "version": "mock-0.1", "db": "ok",
                                            "iotda_token_expire_at": NOW_MS() + 3600000,
                                            "webhook_last_at": st.webhook_last_at,
                                            "clock_source": "device", "server_ts": NOW_MS()}))

        # ---- §6.21 取时（免鉴权、无参数、Cache-Control: no-store；消费方是网关）----
        # t97 补：原先 mock 没这条路由，s05「§6.2 总表逐条可达」只能对 /api/time 记
        # COV-1 待收口 → 长期 SKIP。补上后该用例转硬断言（SKIP=0）。
        if path == "/api/time":
            now = int(time.time())
            return self._send(*envelope(0, {
                "utc_epoch": now,
                "iso8601": datetime.fromtimestamp(now, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            }), headers={"Cache-Control": "no-store"})

        # ---- §5 webhook（token 校验，不是 JWT）----
        if path == "/api/iotda/webhook":
            token = self.headers.get("X-Auth-Token") or params.get("token")
            if token != WEBHOOK_TOKEN:
                return self._send(*envelope(1502))
            st.webhook_last_at = NOW_MS()
            return self._send(*envelope(0, {"accepted": True}))

        # ---- §6.3/§6.4 认证（无 JWT）----
        if path == "/api/auth/register" and method == "POST":
            username = body.get("username")
            password = body.get("password")
            if not username or not password:
                return self._send(*envelope(1000, {"fields": ["username", "password"]}))
            if not re.fullmatch(r"[A-Za-z0-9_]{3,32}", str(username)):
                return self._send(*envelope(1001, {"field": "username"}))
            if not 8 <= len(str(password)) <= 64:
                return self._send(*envelope(1002, {"field": "password"}))
            if username in st.users:
                return self._send(*envelope(1103))
            st.users[username] = {"user_id": len(st.users) + 1, "username": username,
                                  "password": password, "created_at": NOW_MS()}
            return self._send(201, envelope(0, {"user_id": st.users[username]["user_id"],
                                                "username": username,
                                                "created_at": NOW_MS()})[1])

        if path == "/api/auth/login" and method == "POST":
            username = body.get("username")
            password = body.get("password")
            u = st.users.get(username)
            if not u or u["password"] != password:
                return self._send(*envelope(1102))
            token = "tok-%d" % (len(st.tokens) + 1)
            st.tokens[token] = username
            return self._send(*envelope(0, {"token": token, "expires_in": 604800,
                                            "user": {"id": u["user_id"], "username": username,
                                                     "phone": u.get("phone", "")}}))

        # ---- 其余接口一律要求 JWT ----
        user = self._user()
        if user is None:
            return self._send(*envelope(1100))

        def device_ok(dev_id: str):
            if dev_id not in st.devices:
                return envelope(1200, {"device_id": dev_id})
            if (user, dev_id) not in st.user_devices:
                return envelope(1104, {"device_id": dev_id})
            return None

        if path == "/api/devices" and method == "GET":
            ids = [d for u, d in st.user_devices if u == user]
            items = [st.devices[i] for i in ids]
            return self._send(*envelope(0, {"total": len(items), "items": items}))

        if path == "/api/devices/bind" and method == "POST":
            sn = body.get("sn")
            code = str(body.get("bind_code") or "")
            target = next((d for d in st.devices.values()), None)
            if code != target["bind_code"]:
                age = NOW_MS() - target["bind_code_ts"]
                return self._send(*envelope(1201 if age <= 600000 else 1203))
            if NOW_MS() - target["bind_code_ts"] > 600000:
                return self._send(*envelope(1203))
            if (user, target["id"]) not in st.user_devices:
                st.user_devices.append((user, target["id"]))
            return self._send(201, envelope(0, {"device_id": target["id"], "sn": target["sn"],
                                                "role": "owner"})[1])

        m = re.fullmatch(r"/api/devices/([^/]+)/(status|readings|settings|config|demo)", path)
        if m:
            dev_id, action = m.group(1), m.group(2)
            bad = device_ok(dev_id)
            if bad:
                return self._send(*bad)
            dev = st.devices[dev_id]
            if action == "status" and method == "GET":
                return self._send(*envelope(0, {
                    "device_id": dev_id, "online": dev["online"],
                    "last_heartbeat": dev["last_heartbeat"], "alarm_state": dev["alarm_state"],
                    "properties": {"presence": True, "last_motion_ts": NOW_MS() - 60000,
                                   "door": False, "window": False, "temp": 24.5, "humi": 55.0,
                                   "smoke": 320, "nodes_online": '{"M1":true,"M2":false}',
                                   "battery": 255, "alarm_state": dev["alarm_state"]},
                    "nodes": st.nodes, "pending_alerts": len([a for a in st.alerts if a["status"] == "pending"]),
                    "server_ts": NOW_MS()}))
            if action == "readings" and method == "GET":
                limit, err = _limit(params)
                if err:
                    return self._send(*err)
                typ = params.get("type")
                node = params.get("node_sn")
                items = [r for r in st.readings if (not typ or r["type"] == typ)
                         and (not node or r["node_sn"] == node)]
                return self._send(*envelope(0, {"total": len(items), "items": items[:limit]}))
            if action == "settings":
                if method == "GET":
                    return self._send(*envelope(0, {"source": st.settings_source[dev_id],
                                                    "updated_at": NOW_MS(),
                                                    "items": st.settings[dev_id]}))
                if method == "PUT":
                    items = body.get("items")
                    if not isinstance(items, dict) or not items:
                        return self._send(*envelope(1000, {"field": "items"}))
                    merged = dict(st.settings[dev_id])
                    for key, val in items.items():
                        if key not in merged:
                            return self._send(*envelope(1003, {"key": key}))
                        lo, hi = RANGES.get(key, (None, None))
                        if isinstance(val, (int, float)) and lo is not None and not (lo <= val <= hi):
                            return self._send(*envelope(1002, {"field": key, "allowed": [lo, hi]}))
                        if key == "alert_repush_s":
                            if not isinstance(val, list) or len(val) != 3:
                                return self._send(*envelope(1001, {"field": key}))
                        merged[key] = val
                    st.settings[dev_id] = merged
                    st.settings_source[dev_id] = "device"
                    return self._send(*envelope(0, {"source": "device", "updated_at": NOW_MS(),
                                                    "items": merged}))
            if action in ("config", "demo") and method == "POST":
                if not dev["online"]:
                    return self._send(*envelope(1204))
                if action == "demo" and body.get("event") not in ("fall", "gas", "sos"):
                    return self._send(*envelope(1206, {"event": body.get("event")}))
                # §6.11（PM 最终裁定）：accepted/rejected 是键名字符串数组，rejected 可带 reasons。
                # 服务器判定键（不经过设备）落 rejected，与 server 的 dispatch_targets 同口径。
                wanted = (["demo_trigger"] if action == "demo"
                          else (body.get("keys") or list(st.settings[dev_id].keys())))
                accepted = [k for k in wanted if k not in SERVER_SIDE_KEYS]
                rejected = {k: "服务器判定参数，不经过设备" for k in wanted if k in SERVER_SIDE_KEYS}
                result = {"command_id": "cmd-%d-7" % NOW_MS(), "request_id": "a1b2c3",
                          "accepted": accepted, "rejected": sorted(rejected),
                          "dispatched_at": NOW_MS(), "timeout_s": 10}
                if rejected:
                    result["reasons"] = {k: rejected[k] for k in sorted(rejected)}
                return self._send(*envelope(0, result))

        if path == "/api/alerts" and method == "GET":
            limit, err = _limit(params)
            if err:
                return self._send(*err)
            status = params.get("status", "all")
            typ = params.get("type")
            items = [a for a in st.alerts
                     if (status == "all" or a["status"] == status) and (not typ or a["type"] == typ)]
            return self._send(*envelope(0, {"total": len(items), "items": items}))

        m = re.fullmatch(r"/api/alerts/(\d+)", path)
        if m and method == "GET":
            for a in st.alerts:
                if a["id"] == int(m.group(1)):
                    if a["status"] == "pending":
                        a["status"] = "confirmed"
                    detail = dict(a)
                    detail["dispositions"] = [d for d in st.dispositions if d["alert_id"] == a["id"]]
                    detail["readings_before"] = st.readings[:5]
                    detail["readings_after"] = []
                    return self._send(*envelope(0, detail))
            return self._send(*envelope(1300))

        m = re.fullmatch(r"/api/alerts/(\d+)/disposition", path)
        if m and method == "POST":
            action = body.get("action")
            if action not in ("contacted", "checked", "false_alarm", "other"):
                return self._send(*envelope(1302, {"action": action}))
            for a in st.alerts:
                if a["id"] == int(m.group(1)):
                    if a["status"] == "resolved":
                        return self._send(*envelope(1301))
                    a["status"] = "resolved"
                    a["next_push_at"] = None
                    did = len(st.dispositions) + 1
                    st.dispositions.append({"alert_id": a["id"], "action": action,
                                            "note": body.get("note", ""), "ts": NOW_MS()})
                    return self._send(*envelope(0, {"alert_id": a["id"], "status": "resolved",
                                                    "action": action, "disposition_id": did,
                                                    "ts": NOW_MS()}))
            return self._send(*envelope(1300))

        if path in ("/api/reports/activity", "/api/reports/trend", "/api/reports/summary") and method == "GET":
            if not params.get("device_id"):
                return self._send(*envelope(1000, {"field": "device_id"}))
            if path.endswith("/activity"):
                period = params.get("period", "day")
                if period == "day":
                    hourly = [{"hour": h, "count": 0} for h in range(24)]
                    daily = [{"date": "2026-10-17", "count": 0}]
                elif period == "week":
                    hourly = []
                    daily = [{"date": "2026-10-%02d" % d, "count": 0} for d in range(11, 18)]
                elif period == "month":
                    hourly = []
                    daily = [{"date": "2026-10-%02d" % d, "count": 0} for d in range(1, 31)]
                else:
                    return self._send(*envelope(1001, {"field": "period"}))
                return self._send(*envelope(0, {"device_id": params["device_id"], "period": period,
                                                "from": NOW_MS() - 86400000, "to": NOW_MS(),
                                                "total": 0, "hourly": hourly, "daily": daily}))
            if path.endswith("/trend"):
                days = int(params.get("days", 7))
                # 序列取非零：既有今日也有昨日基线，才能覆盖 §6.17 的「有基线」形状；
                # days=1 时没有昨天 → 自动走「无基线只留 title/today」分支（s05_rest 两个场景都跑）。
                series = [{"date": "2026-10-%02d" % d, "count": 100 + d} for d in range(11, 11 + days)]
                today = series[-1]["count"] if series else 0
                yesterday = series[-2]["count"] if len(series) > 1 else 0
                compare = {"title": "今日活动频次", "today": today}  # v1.2：title 必现
                if yesterday:
                    delta = int(round((today - yesterday) / yesterday * 100))  # §6.17 整数百分比
                    compare["yesterday"] = yesterday
                    compare["delta_pct"] = delta
                    if delta <= -10:
                        compare["text"] = "较昨日下降 %d%%" % abs(delta)
                    elif delta >= 10:
                        compare["text"] = "较昨日上升 %d%%" % abs(delta)
                    else:
                        compare["text"] = "与昨日基本持平"
                return self._send(*envelope(0, {"device_id": params["device_id"], "days": days,
                                                "series": series, "compare": compare}))
            return self._send(*envelope(0, {"device_id": params["device_id"], "period": "week",
                                            "from": NOW_MS() - 604800000, "to": NOW_MS(),
                                            "activity": {"total": 0, "active_days": 0, "avg_daily": 0,
                                                         "peak_hour": None, "min_hour": None,
                                                         "compare_prev": 0},
                                            "alerts": {"total": 2, "by_type": {"fall": 1, "gas": 1},
                                                       "resolved": 1, "pending": 1, "false_alarm": 0},
                                            "env": {"temp_avg": 24.5, "temp_min": 22.0,
                                                    "temp_max": 26.0, "humi_avg": 55.0}}))

        if path == "/api/push/register" and method == "POST":
            token = body.get("push_token")
            if not token or body.get("platform") != "harmony":
                return self._send(*envelope(1001, {"field": "push_token/platform"}))
            st.pushes[token] = {"user": user, "updated_at": NOW_MS()}
            return self._send(*envelope(0, {"registered": True, "user_id": user}))

        return self._send(*envelope(1000, {"path": path, "method": method}))

    def do_GET(self):
        self._route("GET")

    def do_POST(self):
        self._route("POST")

    def do_PUT(self):
        self._route("PUT")


def create_server(port: int = 0, provision: bool = False, verbose: bool = False,
                  provision_timeout_s: float = 600.0) -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {"state": State(provision_timeout_s),
                                                "provision": provision})
    httpd = ThreadingHTTPServer(("127.0.0.1", port), handler)
    httpd.verbose = verbose        # type: ignore[attr-defined]
    return httpd


def serve_forever(port: int, provision: bool) -> None:
    httpd = create_server(port, provision, verbose=True)
    print("mock server on http://127.0.0.1:%d/api%s" % (httpd.server_address[1],
                                                        "  (+§12 provision)" if provision else ""))
    httpd.serve_forever()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8801)
    ap.add_argument("--provision", action="store_true", help="同时暴露 §12 本地配网 API")
    args = ap.parse_args()
    serve_forever(args.port, args.provision)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
