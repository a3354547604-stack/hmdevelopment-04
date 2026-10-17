#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""契约 §12（网关本地 API）保真检查：证明本 App 只调 §12 的 5 个接口、且字段名逐个对齐契约。

为什么需要它（PM 口径 F-02）：P0 主路径是网关内置 H5 页面，安卓 App 是 P1「更漂亮的壳」。
两个前端**同源同字段**才叫同一条链路；一旦 App 自己发明一个路径或把 `timeout_s` 写成 `timeoutS`，
现场就会出现「H5 能用、App 报 1602」这种最难查的偏差。而这类错误**编译期不报**：
Gson 解析不到字段只会得到默认值（§12.8 的节点会显示成 SN 未知、MAC 空），
所以必须用脚本对着契约正文核一遍。

检查三件事：
  1) 接口集合与 HTTP 方法：契约 §12.4 总表 5 条 == GatewayApi.kt 里实际调用的 `http.get/post`；
  2) 请求字段名：§12.6 / §12.7 / §12.9 的请求表字段，必须在 GatewayApi.kt 里以 `put("同名")` 出现；
  3) 响应字段名：§12.5 / §12.6 / §12.7 / §12.8 / §12.9 的一级字段，
     必须在 ApiModels.kt 里有同名属性（Gson 靠字段名匹配，改名即静默丢数据）。

不需要 Android SDK、不需要联网，只读三个文件。

用法：
  python3 android-app/tools/check_local_api.py
  python3 android-app/tools/check_local_api.py --contract docs/技术/接口契约.md

退出码：0 = 一致；1 = 有差异（多调/漏调接口、字段名不一致）
"""

from __future__ import annotations

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_CONTRACT = os.path.join(ROOT, "docs", "技术", "接口契约.md")
DEFAULT_GATEWAY = os.path.join(
    ROOT, "android-app", "app", "src", "main", "java",
    "com", "hmprogram", "eldercare", "provision", "data", "api", "GatewayApi.kt",
)
DEFAULT_MODELS = os.path.join(
    ROOT, "android-app", "app", "src", "main", "java",
    "com", "hmprogram", "eldercare", "provision", "data", "model", "ApiModels.kt",
)

# §12.5–§12.9：哪个小节是请求表、哪个是响应表
REQUEST_SECTIONS = {"12.6", "12.7", "12.9"}
RESPONSE_SECTIONS = {"12.5", "12.6", "12.7", "12.8", "12.9"}


def read(path: str) -> str:
    with open(path, encoding="utf-8") as f:
        return f.read()


def parse_endpoints(contract: str) -> list[tuple[str, str]]:
    """§12.4 接口总表 → [(METHOD, /api/...)]。"""
    table = re.search(
        r"###\s*12\.4.*?\n(.*?)(?=\n###\s*12\.5)",
        contract,
        re.S,
    )
    if not table:
        raise SystemExit("!! 契约里找不到 §12.4 接口总表，结构可能变了")
    out: list[tuple[str, str]] = []
    for line in table.group(1).splitlines():
        m = re.match(r"^\|\s*\d+\s*\|\s*(GET|POST)\s*\|\s*`([^`]+)`", line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def parse_section_tables(contract: str) -> dict[str, dict[str, list[str]]]:
    """§12.5–§12.9 → {小节: {"request": [...], "response": [...]}}，只收「一级」字段。

    每个小节都有两张表（请求 / 响应），用「响应」这个标记切分——不能把两张表混在一起，
    否则会把响应字段（如 `saved`）误判成 App 少发了参数。
    只收一级字段：`nodes[].sn`、`precheck.ap_found` 这类嵌套字段跳过，
    它们在 Kotlin 侧是另一个 data class 的属性（§12.8 的 nodes 由节点小节自己核对）。
    """
    sections: dict[str, dict[str, list[str]]] = {}
    for sec in sorted(REQUEST_SECTIONS | RESPONSE_SECTIONS):
        m = re.search(rf"###\s*{sec}\s.*?\n(.*?)(?=\n###\s*12\.\d+)", contract, re.S)
        if not m:
            raise SystemExit(f"!! 契约里找不到 §{sec}，结构可能变了")
        body = m.group(1)
        cut = re.search(r"^\s*响应", body, re.M)
        req_part = body[: cut.start()] if cut else ""
        resp_part = body[cut.start():] if cut else ""
        sections[sec] = {
            "request": _table_fields(req_part),
            "response": _table_fields(resp_part),
        }
    return sections


def _table_fields(body: str) -> list[str]:
    """从 markdown 表格里取第一列的字段名（去重、保序）。"""
    names: list[str] = []
    for line in body.splitlines():
        if line.startswith("|---") or not line.startswith("|"):
            continue
        cell = line.strip().strip("|").split("|")[0].strip().strip("`").strip()
        cell = cell.rstrip("?").strip()
        if not re.fullmatch(r"[a-z][a-z0-9_]*", cell):
            continue  # 表头、嵌套字段（含 . 或 []）、非字段行
        if cell in ("字段", "name", "参数"):
            continue
        if cell not in names:
            names.append(cell)
    return names


def parse_gateway_calls(src: str) -> list[tuple[str, str]]:
    """GatewayApi.kt 里实际的 http.get/post 调用 → [(METHOD, /path)]。

    路径统一去掉 `/api` 前缀再比：契约写的是完整路径 `/api/provision/status`，
    而 HttpClient 的基址自带 `/api`，Kotlin 里只能写 `"/provision/status"`。
    """
    return [
        (m.group(1).upper(), _norm_path(m.group(2)))
        for m in re.finditer(r"http\.(get|post)\(\s*\"([^\"]+)\"", src)
    ]


def _norm_path(path: str) -> str:
    return re.sub(r"^/api(?=/|$)", "", path)


def parse_put_keys(src: str) -> set[str]:
    return set(re.findall(r'put\(\s*"([^"]+)"', src))


def parse_model_properties(src: str) -> set[str]:
    """ApiModels.kt 的全部属性名（data class 的 val 名）。"""
    return set(re.findall(r"\bval\s+([a-z][a-z0-9_]*)\s*:", src))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--contract", default=DEFAULT_CONTRACT)
    ap.add_argument("--gateway", default=DEFAULT_GATEWAY)
    ap.add_argument("--models", default=DEFAULT_MODELS)
    args = ap.parse_args()

    contract = read(args.contract)
    gateway_src = read(args.gateway)
    models_src = read(args.models)

    contract_eps = parse_endpoints(contract)
    app_eps = parse_gateway_calls(gateway_src)
    sections = parse_section_tables(contract)
    put_keys = parse_put_keys(gateway_src)
    props = parse_model_properties(models_src)

    problems: list[str] = []

    # 1) 接口集合与 HTTP 方法
    want = {(m, _norm_path(p)) for m, p in contract_eps}
    have = set(app_eps)
    for m, p in sorted(want - have):
        problems.append(f"契约有、App 没调：{m} {p}")
    for m, p in sorted(have - want):
        problems.append(f"App 调了 §12 之外的接口：{m} {p}")
    # 路径对但动词不对，单独报出来（比「多一条 + 少一条」好定位）
    for _, p in sorted(want - have):
        wrong = sorted(m for m, q in have if q == p)
        if wrong:
            problems.append(f"{p} 的 HTTP 方法用错：App 用了 {', '.join(wrong)}")

    dup = [e for e in set(app_eps) if app_eps.count(e) > 1]
    for m, p in sorted(dup):
        problems.append(f"同一接口被调用了多次（可能是复制粘贴残留）：{m} {p}")

    # 2) 请求字段名
    for sec in sorted(REQUEST_SECTIONS):
        for name in sections[sec]["request"]:
            if name not in put_keys:
                problems.append(f"§{sec} 请求字段 `{name}` 没有出现在 GatewayApi.kt 的 put(\"...\") 里")

    # 3) 响应字段名
    for sec in sorted(RESPONSE_SECTIONS):
        for name in sections[sec]["response"]:
            if name not in props:
                problems.append(f"§{sec} 响应字段 `{name}` 在 ApiModels.kt 里没有同名属性")

    print(f"契约 §12 接口 {len(contract_eps)} 个：{', '.join(m + ' ' + p for m, p in contract_eps)}")
    req_n = sum(len(v["request"]) for v in sections.values())
    resp_n = sum(len(v["response"]) for v in sections.values())
    print(f"契约字段：请求 {req_n} 个、响应 {resp_n} 个（嵌套字段由各自小节核对）")
    print(f"GatewayApi.kt 实际调用 {len(app_eps)} 处；请求字段 {len(put_keys)} 个；模型属性 {len(props)} 个")
    if problems:
        print(f"!! 发现 {len(problems)} 处不一致：")
        for p in problems:
            print("   -", p)
        return 1
    print("一致：接口集合、请求字段名、响应字段名与契约 §12 逐条对齐（未发明接口或字段）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
