#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s07_provision.py -- 本地配网/配对（契约 v1.1 §12 + §13 + §3.9 + §2.7）的契约核对。

为什么要单独一套
    配网是「没有公网也能把设备配起来」的兜底通路，跑在手机直连 SoftAP 的场景下，
    最容易出现「契约写了、实现没做、验收漏了」的三不管。这里钉住四件事：
      ① §12 的 5 个接口与字段；② 1600–1606 的语义（v1.1 改过口径，最容易抄错）；
      ③ 「先扫后绑」与 8 路白名单上限；④ §3.9 配网帧逐字节可复现。
"""

from __future__ import annotations

import json
import os
import re
import sys
import threading

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import known_issues as ki             # noqa: E402
import mock_server                    # noqa: E402
import reference_impl as ri           # noqa: E402
from harness import CaseFail, SkipCase, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402
from s05_rest import Client           # noqa: E402

C = cs.load()
CTX = {"httpd": None, "client": None, "off_httpd": None, "off_client": None,
       "expired_httpd": None, "expired_client": None}


def _serve(**kw):
    httpd = mock_server.create_server(port=0, **kw)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, Client("http://127.0.0.1:%d" % httpd.server_address[1])


def setup():
    if CTX["httpd"] is not None:
        return CTX["httpd"]
    CTX["httpd"], CTX["client"] = _serve(provision=True)
    CTX["off_httpd"], CTX["off_client"] = _serve(provision=False)                       # 不在配网模式
    CTX["expired_httpd"], CTX["expired_client"] = _serve(provision=True,
                                                         provision_timeout_s=0.0)       # 窗口已超时
    return CTX["httpd"]


def c():
    return CTX["client"]


def case_scope_and_security():
    eps = [e["path"] for e in C.provision_endpoints()]
    for need in ("/api/provision/status", "/api/provision/wifi", "/api/pair/scan",
                 "/api/pair/nodes", "/api/pair/bind"):
        require_in(need, eps, "§12.4 配网接口")
    text = C.section_text("12")
    checks = {
        "600": "600 s 配网窗口（§12.2）",
        "192.168.4.1": "SoftAP 地址（§12.3）",
        "WPA2": "WPA2-PSK（§12.3）",
        "长按": "长按配对键进入（§12.2）",
        "不回显": "口令不回显（§12.6）",
    }
    for needle, why in checks.items():
        require(needle in text, "§12 缺少：%s" % why)
    require("最多 8" in text or "上限 8" in text or "8 路" in text, "§12 应写明 8 路白名单上限")


def case_error_codes_semantics():
    """1600–1606 的文案与 HTTP 必须与 §9.2 一致（mock 自己的表最容易漂）。"""
    errs = C.biz_errors()
    for code in range(1600, 1607):
        require_in(code, errs, "§9.2 配网错误码")
        http = int(errs[code]["http"])
        require_eq(mock_server.HTTP_FOR[code], http, "mock 的 code=%d HTTP 与契约 §9.2" % code)
        for kw in re.findall(r"[A-Za-z\u4e00-\u9fff]{2,}", errs[code]["message"]):
            if kw in ("API", "本地"):
                continue
            if kw in mock_server.ERR[code]:
                break
        else:
            raise CaseFail("mock 的 code=%d 文案 %r 与契约 %r 对不上"
                           % (code, mock_server.ERR[code], errs[code]["message"]))


def case_not_in_provision_mode():
    st, payload = CTX["off_client"].call("GET", "/api/provision/status")
    require_eq(payload["code"], 1600, "未进入配网模式应 1600（409）")
    require_eq(st, 409, "1600 的 HTTP 状态")


def case_window_expired():
    st, payload = CTX["expired_client"].call("GET", "/api/provision/status")
    require_eq(payload["code"], 1601, "窗口超时应 1601（410）")
    require_eq(st, 410, "1601 的 HTTP 状态")


def case_status_shape():
    st, payload = c().call("GET", "/api/provision/status")
    require_eq(payload["code"], 0, "配网状态应可读（无 JWT）")
    data = payload["data"]
    for key in ("mode", "ssid", "device_sn", "firmware", "remain_s", "hub_online",
                "bound_count", "slots_free"):
        require_in(key, data, "§12.5 状态必填字段")
    require_eq(data["mode"], "ap", "窗口内 mode=ap")
    require_eq(data["ssid"], "ElderCare-1A2B", "SSID 应为 ElderCare-<SN 后 4 位>")
    require(0 < data["remain_s"] <= 600, "窗口剩余时间应在 (0,600]")
    require_eq(data["slots_free"], 8 - data["bound_count"], "slots_free = 8 − bound_count")


def case_wifi_precheck_and_errors():
    cli = CTX["client"]
    st, payload = cli.call("POST", "/api/provision/wifi", {"password": "P@ssw0rd8"})
    require_eq(payload["code"], 1000, "缺 ssid 应 1000")
    st, payload = cli.call("POST", "/api/provision/wifi", {"ssid": "HOME-2.4G", "password": "123"})
    require_eq(payload["code"], 1002, "口令长度应 1002（WPA2 8–63）")
    st, payload = cli.call("POST", "/api/provision/wifi", {"ssid": "NoSuchAP", "password": "P@ssw0rd8"})
    require_eq(payload["code"], 1602, "预检看不到热点应 1602")
    require_eq(payload["data"].get("reason"), "not_found", "1602 应带 reason=not_found")
    st, payload = cli.call("POST", "/api/provision/wifi", {"ssid": "HOME-2.4G", "password": "P@ssw0rd8"})
    require_eq(payload["code"], 0, "合法 Wi-Fi 应受理")
    data = payload["data"]
    for key in ("saved", "connecting", "ssid", "settle_s", "precheck"):
        require_in(key, data, "§12.6 响应字段")
    require_eq(data["settle_s"], 90, "settle_s 默认 90")
    require("password" not in json.dumps(payload, ensure_ascii=False).lower(), "响应不得回显口令")
    st, payload = cli.call("GET", "/api/provision/status")
    require_in(payload["data"]["mode"], ("connecting", "ap_sta"), "提交后 mode 进入 connecting")
    require_eq(payload["data"].get("sta_ssid"), "HOME-2.4G", "状态回显已配置的 Wi-Fi 名")


def case_scan_and_nodes():
    cli = CTX["client"]
    st, payload = cli.call("POST", "/api/pair/scan", {"duration_s": 999})
    require_eq(payload["code"], 1002, "duration_s 越界应 1002")
    st, payload = cli.call("POST", "/api/pair/scan", {"duration_s": 15})
    require_eq(payload["code"], 0, "发起扫描应受理（异步）")
    data = payload["data"]
    for key in ("started", "id", "duration_s", "estimate_s"):
        require_in(key, data, "§12.7 响应字段")
    require_eq(data["estimate_s"], data["duration_s"] + 3, "estimate_s = duration_s + 3（含重扫开销）")
    scan_id = data["id"]

    st, payload = cli.call("GET", "/api/pair/nodes")
    require_eq(payload["code"], 0, "扫描结果应可读")
    data = payload["data"]
    for key in ("id", "scan_state", "scanned_at", "truncated", "bound", "slots_free", "nodes"):
        require_in(key, data, "§12.8 响应字段")
    require_eq(data["id"], scan_id, "扫描 id 应回显")
    require_in(data["scan_state"], ("idle", "scanning", "done", "error"), "scan_state 枚举")
    require(data["nodes"], "扫描结果不应为空")
    n0 = data["nodes"][0]
    for key in ("sn", "mac", "bind", "role"):
        require_in(key, n0, "§12.8 节点字段")
    require(re.fullmatch(r"[0-9A-F]{2}(:[0-9A-F]{2}){5}", n0["mac"]), "MAC 应大写冒号分隔：%r" % n0["mac"])


def case_bind_preconditions():
    cli = CTX["client"]
    st, payload = cli.call("POST", "/api/pair/bind", {"op": "explode"})
    require_eq(payload["code"], 1002, "op 非法应 1002（枚举约束）")
    st, payload = cli.call("POST", "/api/pair/bind", {"op": "add", "sn": "M4"})
    require_eq(payload["code"], 1000, "add 缺 mac 应 1000")
    st, payload = cli.call("POST", "/api/pair/bind", {"op": "add", "sn": "M4", "mac": "11:22:33:44:55:EE"})
    require_eq(payload["code"], 1604, "mac 不在最近一次扫描结果里应 1604（先扫后绑）")
    st, payload = cli.call("POST", "/api/pair/bind", {"op": "clear"})
    require_eq(payload["code"], 1000, "clear 缺 confirm 应 1000")


def case_bind_add_and_rebuild():
    cli = CTX["client"]
    st, payload = cli.call("GET", "/api/pair/nodes")
    nodes = payload["data"]["nodes"]
    target = next(n for n in nodes if not n["bind"])
    before = payload["data"]["slots_free"]
    st, payload = cli.call("POST", "/api/pair/bind",
                           {"op": "add", "sn": "M4", "mac": target["mac"], "alias": "门厅"})
    require_eq(payload["code"], 0, "合法 add 应成功")
    data = payload["data"]
    for key in ("op", "sn", "bound", "slots_free", "connected"):
        require_in(key, data, "§12.9 响应字段")
    require_eq(data["slots_free"], before - 1, "add 后剩余槽位 −1")
    require_eq(data["connected"], True, "add 后应建立连接（mock 中汇聚在线）")
    require(any(b["mac"] == target["mac"] for b in data["bound"]), "白名单应含新 MAC")

    st, payload = cli.call("POST", "/api/pair/bind", {"op": "list"})
    require_eq(payload["code"], 0, "list 应成功")
    require(len(payload["data"]["bound"]) >= 3, "白名单应包含刚绑定的节点")

    st, payload = cli.call("POST", "/api/pair/bind", {"op": "del", "sn": "M4"})
    require_eq(payload["code"], 0, "del 应成功")
    require(not any(b["sn"] == "M4" for b in payload["data"]["bound"]), "del 后白名单不应还有该 SN")


def case_whitelist_full():
    """上限 8：把扫描到的节点全部绑满，再多一个必须 1606（防止把 8 路 SLE 连接写穿）。"""
    cli = CTX["client"]
    st, payload = cli.call("POST", "/api/pair/scan", {"duration_s": 15})
    require_eq(payload["code"], 0, "扫描应受理")
    st, payload = cli.call("GET", "/api/pair/nodes")
    nodes = payload["data"]["nodes"]
    require(len(nodes) > 8, "mock 扫描结果应 >8 条（用于触发截断与满额）")
    require_eq(payload["data"]["truncated"], True, ">8 条应标 truncated")
    for n in nodes:
        if n["bind"]:
            continue
        if payload["data"]["slots_free"] == 0:
            break
        st, payload = cli.call("POST", "/api/pair/bind", {"op": "add", "sn": "M5", "mac": n["mac"]})
        require_eq(payload["code"], 0, "未满时可继续 add")
    require_eq(payload["data"]["slots_free"], 0, "此时应已满 8")
    st, payload = cli.call("GET", "/api/pair/nodes")
    free = next(n for n in payload["data"]["nodes"] if not n["bind"])
    st, payload = cli.call("POST", "/api/pair/bind", {"op": "add", "sn": "M5", "mac": free["mac"]})
    require_eq(payload["code"], 1606, "白名单满应 1606")
    require_eq(st, 409, "1606 的 HTTP 状态")
    st, payload = cli.call("POST", "/api/pair/bind", {"op": "clear", "confirm": "CLEAR"})
    require_eq(payload["code"], 0, "clear（带确认串）应成功")
    require_eq(payload["data"]["bound"], [], "clear 后白名单为空")
    require_eq(payload["data"]["slots_free"], 8, "clear 后槽位全空")


def case_flow_end_to_end():
    """§13.2 的 App 顺序：status → wifi → scan → nodes → bind → status(bound_count+1)。"""
    httpd, cli = _serve(provision=True)
    try:
        st, payload = cli.call("GET", "/api/provision/status")
        require_eq(payload["code"], 0, "1) status")
        before = payload["data"]["bound_count"]
        st, payload = cli.call("POST", "/api/provision/wifi", {"ssid": "HOME-2.4G", "password": "P@ssw0rd8"})
        require_eq(payload["code"], 0, "2) wifi")
        st, payload = cli.call("POST", "/api/pair/scan", {"duration_s": 15})
        require_eq(payload["code"], 0, "3) scan")
        st, payload = cli.call("GET", "/api/pair/nodes")
        require_eq(payload["code"], 0, "4) nodes")
        free = next(n for n in payload["data"]["nodes"] if not n["bind"])
        st, payload = cli.call("POST", "/api/pair/bind", {"op": "add", "sn": "M4", "mac": free["mac"]})
        require_eq(payload["code"], 0, "5) bind")
        st, payload = cli.call("GET", "/api/provision/status")
        require_eq(payload["data"]["bound_count"], before + 1, "6) status.bound_count 应 +1")
    finally:
        httpd.shutdown()


def case_cloud_channel_relation():
    """§12.10：同一套 UART 命令要能被云端命令通道复用（P1），契约必须写明关系。"""
    text12 = C.section_text("12")
    require("pair_scan" in text12 and "pair_bind" in text12, "§12.10 应写明与 §4.5 命令通道的关系")
    cloud = {c.get("命令", "") for c in C.iotda_commands()}
    for cmd in ("pair_scan", "pair_bind"):
        require_in(cmd, cloud, "§4.5 云端命令（配对命令）")
    # 两条路最终落到同一组 UART 帧：§3.4 必须有 0x20 / 0x22
    types = C.uart_types()
    for t in (0x20, 0x22):
        require_in(t, types, "§3.4 配对帧 TYPE")


def case_pair_frames_roundtrip():
    """§3.9 的五条配网帧必须能被 §3.6 分帧器解出来，且 TYPE/cmd 与 §3.4 对得上。"""
    samples = C.uart_pair_samples()
    require(len(samples) >= 5, "§3.9 配网帧样例不足 5 条（实际 %d）" % len(samples))
    types = C.uart_types()
    cmds = {0x20: "sle_scan", 0x22: "pair_bind"}
    for smp in samples:
        raw = bytes.fromhex(re.sub(r"[^0-9A-Fa-f]", "", smp["hex"]))
        frames, errors, rest = ri.decode_frames(raw)
        require_eq(errors, [], "§3.9 %s 帧解码报错" % smp.get("frame", smp.get("seq")))
        require_eq(rest, b"", "§3.9 %s 帧有残余字节" % smp.get("frame", smp.get("seq")))
        require_eq(len(frames), 1, "§3.9 %s 应恰好一帧" % smp.get("frame", smp.get("seq")))
        f = frames[0]
        require_in(f.type_hex, types, "§3.9 TYPE 0x%02X 不在 §3.4" % f.type_hex)
        payload = f.json()
        if cmds.get(f.type_hex):
            require_eq(payload.get("cmd"), cmds[f.type_hex], "§3.9 TYPE 0x%02X 的 cmd" % f.type_hex)
        require_eq(ri.encode_frame(f.type_hex, f.seq, f.payload), raw, "§3.9 重编码应与样例一致")
        require_eq(f.raw, raw, "§3.9 原始字节应被整帧消费")


CASES = [
    ("§12 接口与安全边界声明", case_scope_and_security),
    ("§9.2 1600–1606 语义与 HTTP", case_error_codes_semantics),
    ("未进配网模式 → 1600", case_not_in_provision_mode),
    ("窗口超时 → 1601", case_window_expired),
    ("§12.5 状态字段完整", case_status_shape),
    ("§12.6 Wi-Fi 预检与错误码", case_wifi_precheck_and_errors),
    ("§12.7/§12.8 扫描与节点列表", case_scan_and_nodes),
    ("§12.9 绑定前置条件（先扫后绑）", case_bind_preconditions),
    ("§12.9 add/list/del 与槽位", case_bind_add_and_rebuild),
    ("§12.9 八路白名单上限 → 1606", case_whitelist_full),
    ("§13.2 端到端配网顺序", case_flow_end_to_end),
    ("§12.10 与云端命令通道的关系", case_cloud_channel_relation),
    ("§3.9 配网帧逐字节往返", case_pair_frames_roundtrip),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s07_provision", "本地配网/配对（§12 + §13 + §3.9 + §2.7）")
    setup()
    try:
        run_cases(suite, CASES, verbose)
    finally:
        for key in ("httpd", "off_httpd", "expired_httpd"):
            if CTX[key] is not None:
                CTX[key].shutdown()
            CTX[key] = None
        for key in ("client", "off_client", "expired_client"):
            CTX[key] = None
    suite.notes.append("被测服务：mock_server（provision=True/False/timeout=0 三个实例）")
    return suite


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
