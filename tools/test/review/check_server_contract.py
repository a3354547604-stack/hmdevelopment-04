#!/usr/bin/env python3
"""t18 独立复核：server/（t5 交付）的契约一致性检查。

设计原则（与实现者自证分离）：
1. 不导入 server 的测试代码、不读它的断言——自己发 HTTP 请求、自己解析契约；
2. 字段名对照的"契约侧"来自 `tools/test/contract/contract_source.py` 对 docs/技术/接口契约.md 的解析，
   而不是我手抄的清单，避免"我抄错、实现也跟着错"；
3. 用**独立账号**走查：自己 register/login，用 seed 打印的绑定码绑定备用网关，
   不去蹭 demo 账号（那样会掩盖绑定/归属校验的问题）；
4. 每个检查项都打印 OK / DIFF / NOTE；DIFF=与契约不符（需人工定级），NOTE=契约未规定或已知待办。

用法：
    VERIFIER_BIND_CODE=123456 python3 tools/test/review/check_server_contract.py
    VERIFIER_BIND_CODE=123456 python3 tools/test/review/check_server_contract.py --base http://127.0.0.1:3000

退出码：0=无 DIFF；1=有 DIFF（需复核人判定）；2=环境不可用（服务未起/绑定码缺失）。
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import secrets
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any

HERE = pathlib.Path(__file__).resolve()
REPO = HERE.parents[3]
sys.path.insert(0, str(REPO / "tools/test/contract"))

from contract_source import Contract  # noqa: E402

C = Contract(REPO / "docs/技术/接口契约.md")

OK, DIFF, NOTE = [], [], []

# 契约示例里列出、但 §0.4/PM 裁定允许"无值就省略键"的字段：
# 出现在响应里是加分，缺席不算缺陷（只记 NOTE）。
# 契约 §4.3 标注 P1/本阶段不接的物模型属性：不上报不算缺陷。
P1_PROPERTIES = {"lpg", "co"}

OPTIONAL_EXAMPLE_KEYS = {
    "recovered_at", "next_push_at", "iotda_token_expire_at", "webhook_last_at",
    "snapshot_json", "last_push_at",
}

# 只在"非空子数组"里出现的键：契约示例中 dispositions[] 的元素字段与处置人 username。
# 数组为空（详情刚打开、还没处置）时按整段示例键比对会误报缺键，故单列出来交给元素级比对。
CONDITIONAL_KEYS = {"action", "note", "user_id", "ts", "at", "username"}
# §6.17 同理由：compare 的 yesterday/delta_pct/text 只在"有昨日基线"时才出现（v1.2 明文），
# 复核设备往往没有活动数据 → 用整段示例键比对会误报缺键，改由下方按基线场景分支核对。
CONDITIONAL_KEYS |= {"yesterday", "delta_pct", "text"}


def ok(msg: str) -> None:
    OK.append(msg)
    print("  [OK]   %s" % msg)


def diff(msg: str) -> None:
    DIFF.append(msg)
    print("  [DIFF] %s" % msg)


def note(msg: str) -> None:
    NOTE.append(msg)
    print("  [NOTE] %s" % msg)


def section(title: str) -> None:
    print("\n" + title)


# --------------------------------------------------------------------------- HTTP

class Http:
    def __init__(self, base: str):
        self.base = base.rstrip("/")
        self.seen: list[tuple[str, str, int, int]] = []   # method, path, http, code

    def req(self, method: str, path: str, body: Any = None, token: str | None = None,
            headers: dict | None = None) -> tuple[int, dict, Any]:
        url = self.base + path
        data = None
        hdrs = dict(headers or {})
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            hdrs["Content-Type"] = "application/json"
        if token:
            hdrs["Authorization"] = "Bearer " + token
        request = urllib.request.Request(url, data=data, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(request, timeout=20) as resp:
                status, raw, hdrs_out = resp.status, resp.read(), dict(resp.headers)
        except urllib.error.HTTPError as exc:            # 4xx/5xx 也是"正常响应"，要检查 code
            status, raw, hdrs_out = exc.code, exc.read(), dict(exc.headers)
        try:
            parsed = json.loads(raw.decode("utf-8"))
        except Exception:  # noqa: BLE001
            parsed = {"_raw": raw[:200].decode("utf-8", "replace")}
        code = parsed.get("code") if isinstance(parsed, dict) else None
        self.seen.append((method, path.split("?")[0], status, int(code) if isinstance(code, int) else -999))
        return status, hdrs_out, parsed


def envelope_bad(parsed: Any) -> str | None:
    """§6.1：响应必须是 {code,message,data,ts}；code=0 才算成功。"""
    if not isinstance(parsed, dict):
        return "响应不是 JSON 对象"
    for key in ("code", "message", "data", "ts"):
        if key not in parsed:
            return "缺信封字段 %s" % key
    return None


def data_keys(parsed: Any) -> set:
    return set(parsed.get("data", {}).keys()) if isinstance(parsed.get("data"), dict) else set()


def live_keys(node: Any, keys: set | None = None) -> set:
    """响应 JSON 里出现过的全部键名（递归：含 hourly[]/daily[]/items[] 这类嵌套列表元素）。"""
    keys = set() if keys is None else keys
    if isinstance(node, dict):
        for key, value in node.items():
            keys.add(key)
            live_keys(value, keys)
    elif isinstance(node, list):
        for value in node:
            live_keys(value, keys)
    return keys


def example_keys(section_num: str) -> set:
    """从契约某节的 JSON 示例里取"data 层"的键（含列表元素键），作为字段名对照的基准。"""
    keys: set[str] = set()

    # properties / by_type 的**内层键**是数据模型或动态映射（§4.3 属性名、告警类型名），
    # 不作为「字段名必须出现」的判据：前者由 §4.3 表核对，后者按数据动态生成。
    DYNAMIC = {"properties", "by_type"}

    def walk(node: Any) -> None:
        if isinstance(node, dict):
            for k, v in node.items():
                keys.add(k)
                if k not in DYNAMIC:
                    walk(v)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    for block in C.json_blocks(section_num):
        if isinstance(block, dict) and "data" in block:
            walk(block["data"])
        else:
            walk(block)
    return keys


def contract_sub_keys(section_num: str, key: str) -> set:
    """契约某节示例里 `data[key]`（数组）**第一个元素的键集合**。

    为什么单独要它：`example_keys()` 把整段示例的键揉成一个集合，再与响应的递归键集对比；
    当数组为空（如未处置时的 dispositions[]）时对比必然失败，会把"元素字段"误报成"缺键"。
    复核 dispositions[] 这类"只有处置后才非空"的数组必须按元素取键。
    """
    out: set[str] = set()
    for block in C.json_blocks(section_num):
        data = block.get("data") if isinstance(block, dict) else None
        if isinstance(data, dict):
            arr = data.get(key)
            if isinstance(arr, list) and arr and isinstance(arr[0], dict):
                out |= set(arr[0])
    return out


def null_paths(node: Any, path: str = "data") -> list[str]:
    """§0.4 空值扫描：列出 JSON 里所有值为 null 的键路径。"""
    out: list[str] = []
    if isinstance(node, dict):
        for key, value in node.items():
            sub = "%s.%s" % (path, key)
            out.append(sub) if value is None else out.extend(null_paths(value, sub))
    elif isinstance(node, list):
        for idx, value in enumerate(node):
            sub = "%s[%d]" % (path, idx)
            out.append(sub) if value is None else out.extend(null_paths(value, sub))
    return out


def items_shape(data: Any) -> dict:
    """列表项字段：items[0] 的键；用于对齐 §6.x 的 items 元素字段。"""
    if isinstance(data, dict) and isinstance(data.get("items"), list) and data["items"]:
        first = data["items"][0]
        if isinstance(first, dict):
            return first
    return {}


# --------------------------------------------------------------------------- A. 端点清单

def routes_from_source() -> dict[tuple[str, str], str]:
    """从 server/app/routers/*.py 静态解析路由（复核对象是"代码里真实存在的路由"）。"""
    out: dict[tuple[str, str], str] = {}
    root = REPO / "server/app/routers"
    for path in sorted(root.glob("*.py")):
        text = path.read_text(encoding="utf-8")
        prefix = re.search(r'APIRouter\(prefix="([^"]*)"', text)
        prefix_str = prefix.group(1) if prefix else ""
        for method, sub in re.findall(r'@router\.(get|post|put|delete)\("([^"]*)"', text):
            out[(method.upper(), prefix_str + sub)] = path.name
    return out


def norm_path(path: str) -> str:
    return re.sub(r"\{[^}]*\}", "{}", path.rstrip("/"))


def section_check_endpoints() -> None:
    section("A. 契约 §6.2 端点清单 ↔ 代码路由清单")
    rows = C.endpoints()
    contract_paths = []
    for row in rows:
        method = row.get("method", "").strip().upper()
        path = row.get("path", "").strip().split("?")[0]
        if method and path:
            contract_paths.append((method, path))
    src = routes_from_source()
    src_norm = {(m, norm_path(p)): f for (m, p), f in src.items()}
    print("  契约 §6.2：%d 条；代码路由：%d 条" % (len(contract_paths), len(src)))
    missing = [(m, p) for (m, p) in contract_paths if (m, norm_path(p)) not in src_norm]
    if missing:
        diff("契约有、代码没有的端点：%s" % ", ".join("%s %s" % t for t in missing))
    else:
        ok("契约 §6.2 的 %d 条端点（18 个 App + webhook）在代码里逐条存在" % len(contract_paths))
    contract_norm = {(m, norm_path(p)) for (m, p) in contract_paths}
    extra = [(m, p) for (m, p) in src_norm if (m, norm_path(p)) not in contract_norm]
    if extra:
        note("代码超集（契约未列，需 README §9 登记）：%s" % ", ".join("%s %s" % t for t in sorted(extra)))


# --------------------------------------------------------------------------- B/C. 动态走查

class Walk:
    def __init__(self, http: Http, bind_code: str, bind_sn: str):
        self.http = http
        self.bind_code = bind_code
        self.bind_sn = bind_sn
        self.user: dict = {}
        self.token: str = ""
        self.device_id: int = 0
        self.device_sn: str = ""
        self.alert_id: int = 0

    # --- 账号与绑定
    def setup_account(self) -> None:
        section("B. 独立账号：注册 → 登录 → 绑定备用网关（不蹭 demo 账号）")
        username = "verifier_" + secrets.token_hex(4)
        status, _, parsed = self.http.req("POST", "/api/auth/register",
                                         {"username": username, "password": "verify-pass-123"})
        expect_keys = example_keys("6.3")
        if status != 201:
            diff("register 期望 201（契约 §6.3/§9.1），实际 %d" % status)
        elif envelope_bad(parsed):
            diff("register 响应信封异常：%s" % envelope_bad(parsed))
        else:
            missing = expect_keys - set(parsed["data"].keys())
            if missing:
                diff("register data 缺契约字段：%s" % sorted(missing))
            else:
                ok("register 201，data 键与 §6.3 示例一致：%s" % sorted(parsed["data"].keys()))
        status, _, parsed = self.http.req("POST", "/api/auth/login",
                                         {"username": username, "password": "verify-pass-123"})
        if status != 200 or parsed.get("code") != 0:
            diff("login 失败：HTTP %d code=%s" % (status, parsed.get("code")))
            return
        self.token = parsed["data"]["token"]
        self.user = {"username": username}
        missing = example_keys("6.4") - set(parsed["data"].keys())
        (diff if missing else ok)("login data 键%s：%s"
                                 % ("缺 " + str(sorted(missing)) if missing else "与 §6.4 示例一致",
                                    sorted(parsed["data"].keys())))
        if "user" in parsed["data"]:
            sub_missing = {"id", "username"} - set(parsed["data"]["user"].keys())
            if sub_missing:
                diff("login.data.user 缺键：%s" % sorted(sub_missing))

        status, _, parsed = self.http.req("POST", "/api/devices/bind",
                                         {"sn": self.bind_sn, "bind_code": self.bind_code, "name": "verifier-备用网关"},
                                         token=self.token)
        if status == 201 and parsed.get("code") == 0:
            self.device_id = int(parsed["data"]["device_id"])
            self.device_sn = parsed["data"]["sn"]
            role = parsed["data"].get("role")
            if role != "owner":
                # 备用网关若已被**上一次复核**绑走，本次会以 family 身份加入：bind 照样 201，
                # 但 §6.10–§6.12 会全部 1104（非 owner），把"环境脏"伪装成一堆契约 DIFF。
                # 这里显式判 owner，让环境问题在最早一步暴露。
                diff("bind 返回 role=%s（期望 owner）：备用网关 %s 已有 owner，"
                     "请先清库（docker compose down -v 重建卷，或 DELETE FROM user_devices WHERE device_id=%d）"
                     % (role, self.bind_sn, self.device_id))
            else:
                ok("bind 201：device_id=%s sn=%s role=owner（§6.6）" % (self.device_id, self.device_sn))
        else:
            diff("bind 失败：HTTP %d code=%s message=%s" % (status, parsed.get("code"), parsed.get("message")))

    # --- 18 端点
    def walk_endpoints(self) -> None:
        section("C. 18 端点走查（字段名对照契约 §6.x 示例；G=按 §6.1 信封规则）")
        if not self.token:
            diff("没有 token，跳过端点走查")
            return
        dev = self.device_id

        # §6.5 列表
        status, _, parsed = self.http.req("GET", "/api/devices", token=self.token)
        self._check("§6.5 GET /api/devices", status, 200, parsed, "6.5",
                    extra_checks=lambda d: self._list_has_total(d))
        # §6.7 状态
        status, _, parsed = self.http.req("GET", "/api/devices/%d/status" % dev, token=self.token)
        status_parsed = parsed
        self._check("§6.7 GET /{id}/status", status, 200, parsed, "6.7")
        if isinstance(parsed.get("data"), dict):
            props = parsed["data"].get("properties")
            want = {row["name"] for row in C.iotda_properties()}
            if isinstance(props, dict) and not props:
                note("自建设备从未上报，properties 为空 → 改用 demo 设备查 §4.3 快照")
                props = self._demo_properties()
            if isinstance(props, dict) and props:
                missing = want - set(props.keys())
                if missing:
                    # properties 是「网关最后上报值」的透传（§6.7 + §0.4）：缺键往往只是还没人上报。
                    # 因此补一帧"全量属性上报"，再取一次快照，才能判定服务端是否真的丢了字段。
                    props = self._probe_properties_passthrough(set(want))
                    missing = want - set(props.keys())
                if missing & P1_PROPERTIES:
                    note("P1 属性未上报（契约 §4.3 标注 lpg/co 为 P1/本阶段不接）：%s"
                         % sorted(missing & P1_PROPERTIES))
                    missing = missing - P1_PROPERTIES
                if missing:
                    diff("status.properties 即使全量上报后仍缺 §4.3 属性：%s" % sorted(missing))
                else:
                    ok("status.properties 在全量上报后覆盖 §4.3 全部 %d 项属性（服务端为透传，不裁剪）"
                       % len(want))

        # §6.8 读数
        status, _, parsed = self.http.req("GET", "/api/devices/%d/readings?limit=5" % dev, token=self.token)
        self._check("§6.8 GET /{id}/readings", status, 200, parsed, "6.8",
                    extra_checks=lambda d: self._list_has_total(d))

        # §6.9 读设置
        status, _, set_parsed = self.http.req("GET", "/api/devices/%d/settings" % dev, token=self.token)
        self._check("§6.9 GET /{id}/settings", status, 200, set_parsed, "6.9")
        defaults = C.settings_defaults()
        items = (set_parsed.get("data") or {}).get("items")
        if isinstance(items, dict):
            missing = set(defaults) - set(items)
            if missing:
                diff("settings.items 缺 §6.9 键：%s" % sorted(missing))
            else:
                ok("settings.items 覆盖 §6.9 全部 %d 键" % len(defaults))
            # 契约 v1.2 §6.9 注（PM D-01）：gas_l1_to_l2_s 是 §8.2 算法参数，**不在 items 里**。
            # 若 GET 回它、而 §6.10 又要求拒它，App 就拿到一个"读得到、写不了"的幽灵键。
            extra = set(items) - set(defaults)
            if extra:
                diff("settings.items 多出 §6.9 之外的键：%s（v1.2 D-01：gas_l1_to_l2_s 属 §8.2 算法参数、不进 items）"
                     % sorted(extra))
            else:
                ok("settings.items 无契约外键（D-01：不含 gas_l1_to_l2_s）")
            if "source" not in (set_parsed.get("data") or {}):
                diff("settings 响应缺 source（§6.9）")
        else:
            diff("settings 响应 data.items 不是对象")

        # §6.10 写设置（局部 + 越界 + 未知键）
        status, _, put_parsed = self.http.req("PUT", "/api/devices/%d/settings" % dev,
                                             {"items": {"no_motion_min": 45}}, token=self.token)
        self._check("§6.10 PUT /{id}/settings", status, 200, put_parsed, "6.10",
                    extra_checks=lambda d: self._settings_source(d))
        status, _, bad = self.http.req("PUT", "/api/devices/%d/settings" % dev,
                                      {"items": {"no_motion_min": 1}}, token=self.token)
        self._expect_error("§6.10 越界", status, bad, 1002)
        status, _, bad = self.http.req("PUT", "/api/devices/%d/settings" % dev,
                                      {"items": {"not_a_key": 1}}, token=self.token)
        self._expect_error("§6.10 未知键", status, bad, 1003)
        # §6.10 明文点名：gas_l1_to_l2_s 属 §8.2 算法参数，经本接口提交必须回 1003（v1.2 D-01）
        status, _, bad = self.http.req("PUT", "/api/devices/%d/settings" % dev,
                                      {"items": {"gas_l1_to_l2_s": 30}}, token=self.token)
        if status == 400 and bad.get("code") == 1003:
            ok("§6.10 gas_l1_to_l2_s → 1003（v1.2 D-01/§6.10）")
        else:
            diff("§6.10 提交 gas_l1_to_l2_s 期望 HTTP 400/1003，实际 HTTP %d code=%s"
                 "（v1.2 §6.9 注 + §6.10：它是 §8.2 算法参数，不可经本接口下发）"
                 % (status, bad.get("code")))
            # 复核者不改 server：值若被接受，就地回写默认值，避免污染 demo 数据
            self.http.req("PUT", "/api/devices/%d/settings" % dev,
                          {"items": {"gas_l1_to_l2_s": 60}}, token=self.token)

        # §6.11 下发
        status, _, cfg = self.http.req("POST", "/api/devices/%d/config" % dev,
                                      {"keys": ["no_motion_min"]}, token=self.token)
        self._check("§6.11 POST /{id}/config", status, 200, cfg, "6.11",
                    extra_checks=lambda d: self._accepted_rejected_ints(d))
        # §6.12 演示
        status, _, demo = self.http.req("POST", "/api/devices/%d/demo" % dev,
                                       {"event": "fall"}, token=self.token)
        self._check("§6.12 POST /{id}/demo", status, 200, demo, "6.12",
                    extra_checks=lambda d: self._accepted_rejected_ints(d))
        if isinstance(demo.get("data"), dict) and demo["data"].get("alert"):
            note("demo 响应带附加键 alert（契约 §6.12 未列，README §5 已说明为便于验收）")
        status, _, bad = self.http.req("POST", "/api/devices/%d/demo" % dev,
                                      {"event": "earthquake"}, token=self.token)
        self._expect_error("§6.12 非法事件", status, bad, 1206)

        # §6.13 列表（含 status=confirmed 过滤）
        status, _, alerts = self.http.req("GET", "/api/alerts?device_id=%d" % dev, token=self.token)
        self._check("§6.13 GET /api/alerts", status, 200, alerts, "6.13",
                    extra_checks=lambda d: self._list_has_total(d))
        item = items_shape(alerts.get("data"))
        if item:
            want = example_keys("6.13") & set()
        status, _, confirmed = self.http.req("GET", "/api/alerts?device_id=%d&status=confirmed" % dev,
                                            token=self.token)
        if status == 200 and confirmed.get("code") == 0:
            bad = [it for it in (confirmed["data"] or {}).get("items", []) if it.get("status") != "confirmed"]
            (diff if bad else ok)("status=confirmed 筛选：%s（%d 条）"
                                  % ("出现非 confirmed 项 %s" % bad if bad else "只返回 confirmed",
                                     len((confirmed["data"] or {}).get("items", []))))
        else:
            diff("status=confirmed 筛选请求失败：HTTP %d code=%s" % (status, confirmed.get("code")))
        # §6.13 明文写死：status 未知取值 → 1001（不是"参数越界"1002）
        status, _, bogus = self.http.req("GET", "/api/alerts?device_id=%d&status=bogus" % dev,
                                         token=self.token)
        if status == 400 and bogus.get("code") == 1001:
            ok("§6.13 status 未知取值 → 1001")
        else:
            diff("§6.13 status 未知取值期望 1001/400，实际 HTTP %d code=%s"
                 "（契约 §6.13 明文「未知取值 → 1001」；实现按 §9.2 的 1002 越界处理）"
                 % (status, bogus.get("code")))

        # §6.13 找一条 pending 告警用于详情/处置
        listed = ((alerts.get("data") or {}).get("items") or [])
        pending = next((it for it in listed if it.get("status") == "pending"), None)
        if pending is None:
            note("当前没有 pending 告警（demo fall 走后 §8.8 去重窗口），详情/处置用例改看已处置告警")
            pending = listed[0] if listed else None
        if pending:
            self.alert_id = int(pending["id"])
            extra = set(pending) - (example_keys("6.13") | {"summary"})
            if extra:
                note("alert 列表项含契约未列字段：%s" % sorted(extra))
            nulls = [k for k, v in pending.items() if v is None]
            if nulls:
                note("alert 列表项里值为 null 的键：%s（§0.4：可选字段应省略键）" % nulls)

        # §6.14 详情（平铺）
        if self.alert_id:
            status, _, detail = self.http.req("GET", "/api/alerts/%d" % self.alert_id, token=self.token)
            self._check("§6.14 GET /api/alerts/{id}", status, 200, detail, "6.14")
            d = detail.get("data") if isinstance(detail.get("data"), dict) else {}
            if "alert" in d:
                diff("§6.14 详情 data 出现嵌套键 alert：PM 裁定为**平铺**")
            elif "id" in d and "type" in d and "status" in d:
                ok("§6.14 详情 data 平铺（顶层含 id/type/level/status）")
            else:
                diff("§6.14 详情 data 既不平铺也没有预期字段：%s" % sorted(d))
            for key in ("dispositions", "readings_before", "readings_after"):
                if key not in d:
                    diff("§6.14 详情缺 %s（契约 §6.14）" % key)
            if d.get("status") == "confirmed":
                if "next_push_at" in d:
                    diff("§6.14 打开详情后 next_push_at 应省略（PM 裁定：可选字段缺省省略键）")
                else:
                    ok("§6.14 打开详情 → pending 置 confirmed 且省略 next_push_at（§0.4/§6.14）")
            if "recovered_at" in d and d["recovered_at"] is None:
                diff("§6.14 详情 recovered_at 为 null（PM 裁定：可选字段缺省省略键）")

            # §6.15 处置
            status, _, disp = self.http.req("POST", "/api/alerts/%d/disposition" % self.alert_id,
                                           {"action": "checked", "note": "verifier 复核"}, token=self.token)
            self._check("§6.15 POST /{id}/disposition", status, 200, disp, "6.15")
            status, _, again = self.http.req("POST", "/api/alerts/%d/disposition" % self.alert_id,
                                            {"action": "checked"}, token=self.token)
            self._expect_error("§6.15 重复处置", status, again, 1301)
            status, _, bad = self.http.req("POST", "/api/alerts/%d/disposition" % self.alert_id,
                                          {"action": "teleport"}, token=self.token)
            self._expect_error("§6.15 非法动作", status, bad, 1302)

            # 处置后回看详情：dispositions[] 的元素字段是 App 详情页时间轴的解析依据。
            # 未处置时数组为空，上面按"整段示例键"做的对照根本摸不到 action/note/ts，
            # 因此必须在这里按**元素键**单独核一次（附录 D-05 记录的 `at` vs `ts` 就是这里发现的）。
            status, _, detail2 = self.http.req("GET", "/api/alerts/%d" % self.alert_id, token=self.token)
            d2 = detail2.get("data") if isinstance(detail2.get("data"), dict) else {}
            recs = d2.get("dispositions") or []
            if not recs:
                diff("§6.14 处置成功后 dispositions[] 仍为空（§6.15 应回写处置记录）")
            else:
                want_keys = contract_sub_keys("6.14", "dispositions") or {"id", "action", "note", "user_id", "ts"}
                got_keys = live_keys(recs[0])
                lost = want_keys - got_keys
                if lost:
                    diff("§6.14 dispositions[] 元素缺契约字段：%s（实际键 %s）"
                         % (sorted(lost), sorted(got_keys)))
                else:
                    ok("§6.14 dispositions[] 元素键与契约 §6.14 示例一致：%s" % sorted(got_keys))
                nulls = [k for k, v in recs[0].items() if v is None]
                if nulls:
                    diff("§6.14 dispositions[] 出现 null 值键：%s（§0.4：无值应省略键）" % nulls)
        status, _, miss = self.http.req("GET", "/api/alerts/99999999", token=self.token)
        self._expect_error("§6.14 告警不存在", status, miss, 1300)

        # §6.16–§6.18 报表
        for num, path, query in (("6.16", "/api/reports/activity", "?device_id=%d&period=day" % dev),
                                 ("6.17", "/api/reports/trend", "?device_id=%d&days=7" % dev),
                                 ("6.18", "/api/reports/summary", "?device_id=%d&period=week" % dev)):
            status, _, rep = self.http.req("GET", path + query, token=self.token)
            self._check("§%s GET %s" % (num, path), status, 200, rep, num)
        # §6.17 compare：契约 v1.2 规定 title 必现、delta_pct 整数四舍五入、无基线省略
        # yesterday/delta_pct/text。这里逐条落判据，避免"看着差不多就过"。
        status, _, trend = self.http.req("GET", "/api/reports/trend?device_id=%d&days=7" % dev, token=self.token)
        cmp_data = ((trend.get("data") or {}).get("compare") or {})
        if "title" not in cmp_data:
            diff("§6.17 trend.compare 缺 title（v1.2：必现「今日活动频次」）")
        else:
            ok("§6.17 trend.compare 带 title")
        # 判据要看"键在不在"，不能只看 .get() 的返回值：省略键与写 null 都会得到 None，
        # 但前者合规（§0.4），后者违规。差别就在这里。
        if "delta_pct" in cmp_data:
            delta = cmp_data["delta_pct"]
            if isinstance(delta, float):
                diff("§6.17 trend.compare.delta_pct=%r 为浮点（v1.2：整数百分比，四舍五入）" % delta)
            elif delta is None:
                diff("§6.17 trend.compare.delta_pct 为 null（§0.4 + v1.2：无基线时应省略该键）")
            else:
                ok("§6.17 trend.compare.delta_pct=%r 为整数（v1.2）" % delta)
        else:
            note("§6.17 days=7 且该设备无活动数据 → compare 省略 delta_pct（无昨日基线，符合 v1.2）")
        # 无基线场景：days=1 时没有"昨日"可比
        status, _, t1 = self.http.req("GET", "/api/reports/trend?device_id=%d&days=1" % dev, token=self.token)
        c1 = ((t1.get("data") or {}).get("compare") or {})
        if status == 200 and t1.get("code") == 0:
            leaked = [k for k in ("yesterday", "delta_pct", "text") if k in c1]
            if leaked:
                diff("§6.17 无基线时 compare 不应出现 %s（v1.2 十六项：无基线省略该三键）" % leaked)
            nulls = [k for k, v in c1.items() if v is None]
            if nulls:
                diff("§6.17 无基线时 compare 出现 null 值键 %s（§0.4：应省略键）" % nulls)
            if not leaked and not nulls:
                ok("§6.17 无基线时 compare 只保留 today/title")
        status, _, bad = self.http.req("GET", "/api/reports/activity", token=self.token)
        self._expect_error("§6.16 缺 device_id", status, bad, 1000)

        # §6.19 推送注册
        status, _, push = self.http.req("POST", "/api/push/register",
                                       {"push_token": "verifier-token-" + secrets.token_hex(4),
                                        "platform": "harmony"}, token=self.token)
        self._check("§6.19 POST /api/push/register", status, 200, push, "6.19")
        # §6.20 健康
        status, _, health = self.http.req("GET", "/api/health")
        self._check("§6.20 GET /api/health（免鉴权）", status, 200, health, "6.20")
        status, _, back = self.http.req("GET", "/api/devices/%d/status" % dev)
        self._expect_error("§6.1 无 token 访问受保护端点", status, back, 1100, http_want=401)

    # --- 检查小工具
    def _check(self, label: str, status: int, want_status: int, parsed: Any, section_num: str,
               extra_checks=None) -> None:
        bad = envelope_bad(parsed)
        if bad:
            diff("%s：%s" % (label, bad))
            return
        if status != want_status:
            diff("%s：HTTP 期望 %d，实际 %d（code=%s）" % (label, want_status, status, parsed.get("code")))
            return
        if parsed.get("code") != 0:
            note("%s：HTTP %d 但 code=%s（非成功语义）" % (label, status, parsed.get("code")))
            return
        want = example_keys(section_num)
        got = live_keys(parsed.get("data"))
        missing = {k for k in want if k not in got and k not in ("services",)}
        optional_missing = {k for k in missing if k in OPTIONAL_EXAMPLE_KEYS}
        # "只在非空子数组里出现"的键：契约 §6.14 示例的 dispositions[] 元素字段（action/note/
        # user_id/ts）与处置人 username。详情刚打开时 dispositions 为空数组，用整段示例键比对
        # 必然误报"缺键"——这类键改由 contract_sub_keys() 按元素单独核对。
        conditional = {k for k in missing if k in CONDITIONAL_KEYS}
        missing -= conditional
        missing -= optional_missing
        msg = "%s：HTTP %d code=0" % (label, status)
        if missing:
            diff("%s；data 缺契约示例键：%s" % (msg, sorted(missing)))
        else:
            ok("%s；data 键覆盖 §%s 示例" % (msg, section_num))
        if optional_missing:
            note("%s；按 §0.4 省略的可选键：%s（有值时会出现）" % (label, sorted(optional_missing)))
        if conditional:
            note("%s；嵌套数组元素键（%s）由元素级比对核对，此处不计"
                 % (label, sorted(conditional)))
        if extra_checks:
            extra_checks(parsed.get("data"))

    def _expect_error(self, label: str, status: int, parsed: Any, want_code: int,
                      http_want: int | None = None) -> None:
        bad = envelope_bad(parsed)
        if bad:
            diff("%s：%s" % (label, bad))
            return
        got = parsed.get("code")
        if got != want_code:
            diff("%s：期望业务码 %d，实际 %s（HTTP %d）" % (label, want_code, got, status))
            return
        http_want = http_want if http_want is not None else self._http_for(want_code)
        if http_want and status != http_want:
            diff("%s：业务码 %d 期望 HTTP %d，实际 %d（§9.1/§9.2）" % (label, want_code, http_want, status))
        else:
            ok("%s：code=%d HTTP %d（§9.2）" % (label, got, status))
        if want_code in (1002,):
            if not isinstance(parsed.get("data"), dict) or "field" not in parsed["data"]:
                note("%s：data 未带 field/allowed（契约 §9.2 说 1002 带 field 与 allowed）" % label)

    def _http_for(self, code: int) -> int | None:
        rows = C.biz_errors()
        row = rows.get(code)
        if row and row.get("HTTP"):
            try:
                return int(str(row["HTTP"]).strip())
            except ValueError:
                return None
        return None

    def _demo_properties(self) -> dict:
        """demo 设备（seed 的 device 1）有真实上报数据，用它核对 §6.7 的 properties 快照。"""
        status, _, parsed = self.http.req("POST", "/api/auth/login",
                                         {"username": "demo", "password": "demo-pass-123"})
        if status != 200 or parsed.get("code") != 0:
            diff("demo 账号登录失败（README §2 的演示账号）：HTTP %d" % status)
            return {}
        token = parsed["data"]["token"]
        status, _, lst = self.http.req("GET", "/api/devices", token=token)
        for item in ((lst.get("data") or {}).get("items") or []):
            status, _, st = self.http.req("GET", "/api/devices/%d/status" % int(item["id"]), token=token)
            props = (st.get("data") or {}).get("properties") if st.get("code") == 0 else None
            if isinstance(props, dict) and props:
                ok("demo 设备 %s 的 properties 非空（%d 键）" % (item.get("sn"), len(props)))
                return props
        note("demo 设备也没有 properties 快照：§4.3 属性全链路未打通（需 webhook 上报后才有）")
        return {}

    def _probe_properties_passthrough(self, want: set) -> dict:
        """补一帧全量属性上报（§4.6 形状，README §5 curl 21），再取一次 status.properties。

        用途：区分"网关还没上报该属性"与"服务端把字段丢了"——前者不是缺陷，后者是。
        """
        status, _, parsed = self.http.req("POST", "/api/auth/login",
                                         {"username": "demo", "password": "demo-pass-123"})
        if status != 200 or parsed.get("code") != 0:
            diff("demo 账号登录失败，无法做属性透传探针")
            return {}
        token = parsed["data"]["token"]
        status, _, lst = self.http.req("GET", "/api/devices", token=token)
        items = (lst.get("data") or {}).get("items") or []
        if not items:
            diff("demo 账号下没有设备，无法做属性透传探针")
            return {}
        device = items[0]
        # 契约 §4.3 的物模型属性；lpg/co 属 P1，这里照报但允许缺省
        props = {
            "presence": True, "last_motion_ts": int(time.time() * 1000) - 5000, "door": False,
            "window": False, "temp": 24.8, "humi": 55.0, "smoke": 180, "battery": 255,
            "alarm_state": 1, "nodes_online": '{"M1":true,"M2":true,"M4":true,"H1":true}',
            # v1.1 新增属性：服务端若是纯透传，报上来就应该出现在快照里
            "bound_nodes": '["M1","M2"]', "pair_state": 0,
        }

        body = {"resource": "device.property", "event": "report",
                "event_time": "20261017T140000Z",
                "notify_data": {"header": {"device_id": device["sn"], "node_id": device["sn"]},
                                "body": {"services": [{"service_id": "gateway",
                                                       "event_time": "20261017T140000Z",
                                                       "properties": props}]}}}
        status, _, got = self.http.req("POST", "/api/iotda/webhook?token=" + os.environ.get(
            "VERIFIER_WEBHOOK_TOKEN", "dev-webhook-token-change-me"), body)
        if status != 200 or got.get("code") != 0:
            diff("属性上报 webhook 失败：HTTP %d code=%s" % (status, got.get("code")))
            return {}
        handled = (got.get("data") or {}).get("handled")
        ok("属性上报 webhook 受理（handled=%s，设备 %s）" % (handled, device["sn"]))
        status, _, st = self.http.req("GET", "/api/devices/%d/status" % int(device["id"]), token=token)
        snap = (st.get("data") or {}).get("properties") if st.get("code") == 0 else {}
        if isinstance(snap, dict) and snap:
            ok("上报后 status.properties 有 %d 键：%s" % (len(snap), sorted(snap)))
        return snap if isinstance(snap, dict) else {}

    def _list_has_total(self, data: Any) -> None:
        if not isinstance(data, dict) or "total" not in data:
            diff("列表响应缺 total（§6.1 分页）")
        elif not isinstance(data.get("items"), list):
            diff("列表响应 items 不是数组")

    def _settings_source(self, data: Any) -> None:
        if not isinstance(data, dict) or data.get("source") not in ("local", "device"):
            diff("settings.source 取值非法：%s（§6.9）" % (data or {}).get("source"))

    def _accepted_rejected_ints(self, data: Any) -> None:
        if not isinstance(data, dict):
            return
        for key in ("accepted", "rejected"):
            value = data.get(key)
            if not isinstance(value, int) or isinstance(value, bool):
                diff("PM 裁定：%s 必须是 data 内的整型计数，实际 %r" % (key, value))
        if isinstance(data.get("accepted"), int) and isinstance(data.get("rejected"), int):
            ok("PM 裁定 accepted/rejected 为整型：accepted=%s rejected=%s"
               % (data["accepted"], data["rejected"]))


# --------------------------------------------------------------------------- D. webhook

def recent_same_type(http: "Http", token: str | None, device_sn: str, alert_type: str,
                     window_s: int = 600) -> bool:
    """该设备最近 window_s 秒内是否已有同类告警（判断 §8.8 去重窗口是否遮蔽了本次上报）。

    复核脚本复跑时，上一次自己造的告警会落在窗口内，让"首次应入库"的断言失真——
    这时要能区分"服务端没入库（缺陷）"和"窗口内去重（设计）"。
    """
    if not token:
        return False
    status, _, parsed = http.req("GET", "/api/alerts", token=token)
    if status != 200 or parsed.get("code") != 0:
        return False
    now = int(time.time() * 1000)
    for item in ((parsed.get("data") or {}).get("items") or []):
        if item.get("type") != alert_type:
            continue
        ts = item.get("ts")
        if isinstance(ts, int) and 0 <= now - ts <= window_s * 1000:
            return True
    return False

def section_webhook(http: Http, token: str, device_sn: str) -> None:
    section("D. webhook（§5）：鉴权、事件入库、event_id 幂等")
    status, _, parsed = http.req("POST", "/api/iotda/webhook?token=wrong-token", {"resource": "device.property"})
    if status == 401 and parsed.get("code") == 1502:
        ok("webhook 错 token → 1502/401（§9.2）")
    else:
        diff("webhook 错 token 期望 1502/401，实际 HTTP %d code=%s" % (status, parsed.get("code")))
    status, _, unknown = http.req("POST", "/api/iotda/webhook?token=" + os.environ.get("VERIFIER_WEBHOOK_TOKEN",
                                                                                       "dev-webhook-token-change-me"),
                                 {"resource": "device.property", "notify_data": {"header": {"device_id": "NOPE-XXXX"},
                                                                                 "body": {"services": []}}})
    if status == 200:
        ok("webhook 未匹配设备 → 200（§5：不重推），data=%s" % unknown.get("data"))
    else:
        diff("webhook 未匹配设备期望 200，实际 %d" % status)

    # 用 sos/M4（demo 设备上没有 sos，去重窗口不会遮蔽），ts 取现在：
    # 第一次应新建（handled=1），第二次带同一 event_id 应幂等（handled=0），DB 里只应有一行。
    event_id = "M0-VERIFY-%d" % secrets.randbelow(10 ** 6)
    ts_now = int(time.time() * 1000)
    body = {"resource": "device.event", "event_time": "20261017T140000Z",
            "notify_data": {"header": {"device_id": device_sn, "node_id": device_sn},
                            "body": {"services": [{"service_id": "gateway", "event_type": "sos",
                                                   "event_time": "20261017T140000Z",
                                                   "paras": {"source": "M4", "event_id": event_id,
                                                             "ts": ts_now}}]}}}
    handled = []
    for attempt in (1, 2):
        status, _, got = http.req("POST", "/api/iotda/webhook?token=" + os.environ.get(
            "VERIFIER_WEBHOOK_TOKEN", "dev-webhook-token-change-me"), body)
        if status != 200 or got.get("code") != 0:
            diff("webhook 事件（第 %d 次）失败：HTTP %d code=%s" % (attempt, status, got.get("code")))
            continue
        data = got.get("data") or {}
        handled.append(data.get("handled"))
        ok("webhook 事件第 %d 次 → HTTP 200 code=0，handled=%s（event_id=%s）"
           % (attempt, data.get("handled"), event_id))
    if handled == [1, 0]:
        ok("同一 event_id 重投：首次入库、第二次幂等（handled 1→0，§4.4）")
    elif handled == [0, 0] and recent_same_type(http, token, device_sn, "sos"):
        # §8.8 去重窗口：同设备同类型告警在窗口内不重复入库。**复核脚本自己复跑**就会撞上窗口
        # （上一次跑的那条 sos 还在窗口内），此时 handled=0 是设计行为，不是幂等/入库缺陷。
        note("首次 handled=0：同设备同类型（sos）告警仍在 §8.8 去重窗口内（上一次复核留下的告警），"
             "本次不计为缺陷；若要看 1→0，请等窗口过期或清掉该告警后重跑")
    elif handled:
        diff("同 event_id 重投的 handled 序列异常：%s（期望 [1, 0]）" % handled)
    print("  [EVIDENCE] event_id=%s（用 DB 复核：该 event_id 应恰有 1 行告警）" % event_id)
    return event_id


# --------------------------------------------------------------------------- E. PM 裁定静态核对

def section_pm_rulings() -> None:
    section("E. PM 裁定与背景口径的静态核对")
    src = (REPO / "server/app/routers/devices.py").read_text(encoding="utf-8")
    if re.search(r'"accepted":\s*len\(', src) or re.search(r'"accepted":\s*1', src):
        ok("config/demo 的 accepted/rejected 由计数（int）构造")
    else:
        diff("未在 devices.py 找到 accepted 整型计数的构造")
    if "set_threshold" in src or "set_threshold" in (REPO / "server/app/services/iotda.py").read_text(
            encoding="utf-8"):
        ok("阈值下发命令名 set_threshold（§3.4 0x11 set 帧）")
    else:
        diff("未找到 set_threshold 下发")
    iotda_src = (REPO / "server/app/services/iotda.py").read_text(encoding="utf-8")
    if re.search(r'"set_threshold",\s*\{"cmd":\s*"set_threshold",\s*"key":', iotda_src):
        ok("set_threshold 载荷为 {cmd,key,value}")
    else:
        diff("set_threshold 载荷形状与 PM 裁定 {cmd,key,value} 不符")
    cfg = (REPO / "server/app/config.py").read_text(encoding="utf-8")
    if 'iotda_dispatch_mode: str = "mock"' in cfg and 'IOTDA_DISPATCH_MODE", "mock"' in cfg:
        ok("IOTDA_DISPATCH_MODE 默认 mock（config.py）")
    else:
        diff("IOTDA_DISPATCH_MODE 默认值不是 mock")
    compose = (REPO / "server/docker-compose.yml").read_text(encoding="utf-8")
    if "IOTDA_DISPATCH_MODE: ${IOTDA_DISPATCH_MODE:-mock}" in compose:
        ok("compose 里 IOTDA_DISPATCH_MODE 兜底 mock")
    else:
        diff("compose 里 IOTDA_DISPATCH_MODE 没有 mock 兜底")
    if "APP_ENV=prod 但 JWT_SECRET/WEBHOOK_TOKEN 仍是占位符" in cfg:
        ok("prod 用占位符密钥会拒绝启动（config.py 显式检查）")
    else:
        diff("config.py 没有 prod 占位符密钥的拒绝逻辑")
    keys_src = (REPO / "server/app/settings_keys.py").read_text(encoding="utf-8")
    key_names = set(re.findall(r'^\s{4}"([a-z_0-9]+)":\s*KeySpec', keys_src, re.M))
    defaults = set(C.settings_defaults())
    if defaults - key_names:
        diff("settings_keys.py 缺契约 §6.9 的键：%s" % sorted(defaults - key_names))
    else:
        ok("settings_keys.py 覆盖 §6.9 全部 %d 键（静态核对；契约外多出的 gas_l1_to_l2_s 见 C 段 D-01 DIFF）"
           % len(defaults))


# --------------------------------------------------------------------------- F. 密钥扫描

SECRET_PAT = re.compile(
    r'(?i)\b(pass(word|wd)?|secret|token|api[_-]?key|access[_-]?key|secret[_-]?key|ak|sk)\b\s*[=:]\s*["\']([^"\']{6,})["\']')
ALLOW_VALUES = {
    "hmprogram-dev-pass", "hmprogram-dev-root", "dev-only-change-me", "dev-webhook-token-change-me",
    "demo-pass-123", "test-pass-123", "verify-pass-123", "mock-iotda-token", "eldercare",
    "change-me", "changeme", "secret", "password", "dummy", "placeholder", "not-a-secret",
    "utf-8", "application/json", "GET", "POST", "Bearer",
    "wrong-token",   # server/tests 里的负例夹具，不是真实凭据
}
SKIP_DIRS = {".git", "__pycache__", ".pytest_cache", "node_modules"}


def section_secret_scan() -> None:
    section("F. 密钥/口令扫描（server/ + compose；.env.example 允许占位符）")
    hits: list[str] = []
    scanned = 0
    for path in sorted((REPO / "server").rglob("*")):
        if not path.is_file():
            continue
        if any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.suffix not in {".py", ".yml", ".yaml", ".sql", ".sh", ".sh", ".md", ".example", ".ini", ".conf", ".txt", ".json"} \
                and path.name not in {"Dockerfile"}:
            continue
        scanned += 1
        try:
            text = path.read_text(encoding="utf-8")
        except Exception:  # noqa: BLE001
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("#"):
                continue
            for match in SECRET_PAT.finditer(line):
                value = match.group(3)
                if value in ALLOW_VALUES or value.startswith("${") or value.startswith("<"):
                    continue
                if len(value) < 8:
                    continue
                hits.append("%s:%d: %s=%r" % (path.relative_to(REPO), lineno, match.group(1), value))
    print("  扫描文件：%d" % scanned)
    if hits:
        diff("疑似硬编码凭据 %d 处：%s" % (len(hits), " | ".join(hits[:10])))
    else:
        ok("未发现硬编码真实口令/secret（仅 dev 兜底值与占位符）")
    compose = (REPO / "server/docker-compose.yml").read_text(encoding="utf-8")
    if ".env" in (REPO / "server/.gitignore").read_text(encoding="utf-8"):
        ok(".env 在 server/.gitignore 中（真实凭据不进仓库）")
    else:
        diff(".env 未被 .gitignore 忽略")
    if "hmprogram-dev-pass" in compose:
        note("compose 里 DB/JWT/WEBHOOK 有 dev 兜底值（README 与 .env.example 已声明；prod 会被 config.py 拒绝）")


# --------------------------------------------------------------------------- G. /api/time

def section_time(http: Http) -> None:
    section("G. /api/time（PM 裁定：免鉴权、UTC、秒 + iso8601 + no-store）")
    status, hdrs, parsed = http.req("GET", "/api/time")
    if status != 200 or parsed.get("code") != 0:
        diff("/api/time 不可用：HTTP %d code=%s" % (status, parsed.get("code")))
        return
    data = parsed.get("data") or {}
    need = {"utc_epoch", "utc_epoch_ms", "iso8601", "tz"}
    missing = need - set(data)
    (diff if missing else ok)("/api/time data 键：%s%s" % (sorted(data),
                                                        "（缺 %s）" % sorted(missing) if missing else ""))
    if isinstance(data.get("utc_epoch"), int) and not isinstance(data.get("utc_epoch"), bool):
        ok("/api/time.utc_epoch 是整数秒：%s" % data["utc_epoch"])
    else:
        diff("/api/time.utc_epoch 不是整数秒：%r" % data.get("utc_epoch"))
    iso = str(data.get("iso8601", ""))
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", iso):
        ok("/api/time.iso8601 是 UTC（Z 结尾）：%s" % iso)
    else:
        diff("/api/time.iso8601 形状不符：%r" % iso)
    cache = hdrs.get("Cache-Control") or hdrs.get("cache-control")
    if cache and "no-store" in cache.lower():
        ok("/api/time 带 Cache-Control: %s" % cache)
    else:
        note("/api/time 无 Cache-Control: no-store（t19 待落地的已知待办，不计入 t5）")


# --------------------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default=os.environ.get("CONTRACT_TEST_BASE_URL", "http://127.0.0.1:3000"))
    args = parser.parse_args()
    bind_code = os.environ.get("VERIFIER_BIND_CODE", "")
    bind_sn = os.environ.get("VERIFIER_BIND_SN", "M0-9C2D4E")
    if not bind_code:
        print("环境不可用：需要 VERIFIER_BIND_CODE（seed --bind-code 打印的值）")
        return 2
    http = Http(args.base)
    try:
        status, _, _ = http.req("GET", "/api/health")
        if status != 200:
            print("环境不可用：%s/api/health HTTP %d" % (args.base, status))
            return 2
    except Exception as exc:  # noqa: BLE001
        print("环境不可用：连不上 %s（%s）" % (args.base, exc))
        return 2

    print("被复核服务：%s（契约：docs/技术/接口契约.md）" % args.base)
    section_check_endpoints()
    walk = Walk(http, bind_code, bind_sn)
    walk.setup_account()
    walk.walk_endpoints()
    event_id = section_webhook(http, walk.token, walk.device_sn)
    section_pm_rulings()
    section_secret_scan()
    section_time(http)

    section("全局一致性")
    bad = [row for row in http.seen if row[0] and row[2] == 200 and row[3] not in (0, -999)]
    if bad:
        diff("出现「HTTP 200 且 code≠0」：%s（契约 §6.1 禁止）" % bad[:5])
    else:
        ok("全部 %d 次请求都没有「HTTP 200 且 code≠0」" % len(http.seen))

    print("\n结论：%d OK / %d DIFF / %d NOTE" % (len(OK), len(DIFF), len(NOTE)))
    for item in DIFF:
        print("  DIFF: %s" % item)
    return 1 if DIFF else 0


if __name__ == "__main__":
    sys.exit(main())
