#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""契约 §12（网关本地 API）保真检查：证明前端只调 §12 的 5 个接口、且字段名逐个对齐契约。

为什么需要它（PM 口径 F-02）：P0 主路径是网关内置 H5 页面，安卓 App 是 P1「更漂亮的壳」。
两个前端**同源同字段**才叫同一条链路；一旦某一侧自己发明一个路径或把 `timeout_s` 写成 `timeoutS`，
现场就会出现「H5 能用、App 报 1602」这种最难查的偏差。而这类错误**编译期不报**：
Gson 解析不到字段只会得到默认值（§12.8 的节点会显示成 SN 未知、MAC 空），
所以必须用脚本对着契约正文核一遍。

两种模式：

  A) 默认（安卓 App，读 Kotlin 源码）——检查三件事：
     1) 接口集合与 HTTP 方法：契约 §12.4 总表 5 条 == GatewayApi.kt 里实际调用的 `http.get/post`；
     2) 请求字段名：§12.6 / §12.7 / §12.9 的请求表字段，必须在 GatewayApi.kt 里以 `put("同名")` 出现；
     3) 响应字段名：§12.5 / §12.6 / §12.7 / §12.8 / §12.9 的一级字段，
        必须在 ApiModels.kt 里有同名属性（Gson 靠字段名匹配，改名即静默丢数据）。

  B) `--js <file>`（网关 H5 页面，P0 主路径）——不需要 Kotlin、不需要 Android SDK：
     页面里没有编译器兜底，写错路径/字段名浏览器只会在控制台静默出错，
     所以从 HTML/内联 JS/本地外链 JS 里静态提取：
     1) `fetch("/api/...")` / `XMLHttpRequest.open()` / `axios.get("/api/...")` 的路径与动词；
     2) 请求体（`JSON.stringify({...})`、`body: {...}`、`.append("x")`、简写属性 `{ ssid, password }`）的键名；
     3) 响应字段访问（`json.x`、`data.y`、`r.z`、`data.nodes.forEach(n => n.sn)`，
        含可选链 `json?.data?.mode`）等。
     凡调用了 §12 之外的本地接口、或出现 §12.5–§12.9 里没有的字段名，一律报红并退出码 1。
     报错行号指到**真正的来源文件**：内联块算页面、外链脚本算它自己的文件。
     拿不准的情况（页面没调某接口、拼接路径、`json['data']` 这种下标写法）只打印提示不改退出码；
     但「一条 /api 路径都没提取到」会报红——那等于什么都没核，不能算「一致」。

不需要 Android SDK、不需要联网，只读契约与被检文件。

用法：
  python3 android-app/tools/check_local_api.py
  python3 android-app/tools/check_local_api.py --contract docs/技术/接口契约.md
  python3 android-app/tools/check_local_api.py --js rk2206-gateway/web/index.html   # H5 页面落地后的路径
  python3 android-app/tools/check_local_api.py --self-test     # 自带正/负样例自检（3 个样例）

退出码：0 = 一致；1 = 有差异（多调接口、字段名不一致、或调用 §12 之外的本地接口）
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

# 已登记、**有据可查**的例外（只打印提示，不报红）。登记规则：必须写明「为什么允许」
# 和「证据在哪」，否则下一个人无法判断它是例外还是残留。
EXTRA_ALLOWED: dict[tuple[str, str], str] = {
    ("GET", "/scan"): (
        "清单 F2 要求「SSID 下拉取自网关扫描缓存」；固件 t143 已在 "
        "rk2206-gateway/src/gw_prov_route.c 路由 6 实现（只读缓存，不受配网窗口限制），"
        "契约 §12.4 总表尚未补录，待 PM 补条目后删除本例外"
    ),
}

# 同一接口的多个调用点：`submitWifi` 与 `submitCloudCreds` 都必须 POST /provision/wifi
# （§12.6 允许只带云凭据、不带 ssid），这是设计，不是复制粘贴残留。
DUP_ALLOWED: dict[tuple[str, str], str] = {
    ("POST", "/provision/wifi"): "submitWifi（家庭 Wi-Fi）与 submitCloudCreds（仅云凭据）两个方法共用，见 §12.6",
}


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
        # 切分点不能只认行首的「响应」：§12.8 写成「请求：无参数。响应 `data`：」，
        # 行首匹配会切空，导致 §12.8 的响应字段一个都核不到（这正是本工具要防的那类静默漏检）。
        cut = re.search(r"响应", body)
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
    ap = argparse.ArgumentParser(description="契约 §12 保真检查（安卓 App 源码 / 网关 H5 页面）")
    ap.add_argument("--contract", default=DEFAULT_CONTRACT)
    ap.add_argument("--gateway", default=DEFAULT_GATEWAY)
    ap.add_argument("--models", default=DEFAULT_MODELS)
    ap.add_argument("--js", metavar="FILE", help="核对网关 H5 页面：.html（含内联/本地外链 JS）或 .js")
    ap.add_argument("--self-test", action="store_true", help="跑内置正/负样例，验证 --js 会报绿/报红")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test(args.contract)
    if args.js:
        return check_js(args.js, args.contract)

    contract = read(args.contract)
    gateway_src = read(args.gateway)
    models_src = read(args.models)

    contract_eps = parse_endpoints(contract)
    app_eps = parse_gateway_calls(gateway_src)
    sections = parse_section_tables(contract)
    put_keys = parse_put_keys(gateway_src)
    props = parse_model_properties(models_src)

    problems: list[str] = []
    notes: list[str] = []

    # 1) 接口集合与 HTTP 方法
    want = {(m, _norm_path(p)) for m, p in contract_eps}
    have = set(app_eps)
    for m, p in sorted(want - have):
        problems.append(f"契约有、App 没调：{m} {p}")
    for m, p in sorted(have - want):
        why = EXTRA_ALLOWED.get((m, p))
        # 登记过的例外只打印提示：越界本身是事实，但已有人担保过，不该每次都报红。
        if why:
            notes.append(f"已登记例外：{m} {p} — {why}")
        else:
            problems.append(f"App 调了 §12 之外的接口：{m} {p}")
    # 路径对但动词不对，单独报出来（比「多一条 + 少一条」好定位）
    for _, p in sorted(want - have):
        wrong = sorted(m for m, q in have if q == p)
        if wrong:
            problems.append(f"{p} 的 HTTP 方法用错：App 用了 {', '.join(wrong)}")

    dup = [e for e in set(app_eps) if app_eps.count(e) > 1]
    for m, p in sorted(dup):
        why = DUP_ALLOWED.get((m, p))
        if why:
            notes.append(f"已登记的多调用点：{m} {p} 出现 {app_eps.count((m, p))} 次 — {why}")
        else:
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
    for n in notes:
        print("提示：" + n)
    if problems:
        print(f"!! 发现 {len(problems)} 处不一致：")
        for p in problems:
            print("   -", p)
        return 1
    print("一致：接口集合、请求字段名、响应字段名与契约 §12 逐条对齐（未发明接口或字段）")
    return 0


# ---------------------------------------------------------------------------
# 模式 B：网关 H5 页面（--js）
#
# 为什么把字段名检查也搬到 H5 上：H5 是 P0 主路径，但它是**纯解释执行**——路径写错只是 404、
# 字段名写错只是 `undefined`（页面上显示成空白或 NaN），没有任何编译期或运行期报错。
# 现场第一次暴露就是在评委面前。所以用同一份契约表静态核一遍。
#
# 静态分析必然保守：拿不准的地方只给「提示」不改退出码，只有**确定越界**（§12 之外的接口、
# 契约里没有的字段名）才报红。白名单 JS_BUILTINS 就是为了不让 `data.length`、
# `el.textContent` 这类 JS/DOM 内置成员变成假红。
# ---------------------------------------------------------------------------

JS_ROOT_LITERALS = ("json", "data")  # 契约信封：json.data.* 是最常见写法
JS_ENVELOPE = {"code", "message", "data", "ts"}
JS_ARRAY_PARENTS = ("nodes", "bound")
JS_NESTED_FIELDS = {
    "precheck": {"ap_found", "rssi", "ch"},
    "nodes": {"sn", "mac", "rssi", "bind", "role", "online", "last_seen"},
    "bound": {"sn", "mac", "alias"},
}

# JS 语言 / DOM / 数组的内置成员：出现在响应变量后面不算「发明字段」
JS_BUILTINS = {
    "length", "map", "filter", "forEach", "find", "findIndex", "some", "every", "reduce",
    "push", "pop", "shift", "unshift", "splice", "slice", "concat", "join", "reverse", "sort",
    "indexOf", "lastIndexOf", "includes", "keys", "values", "entries", "flat", "at",
    "toString", "valueOf", "toFixed", "toUpperCase", "toLowerCase", "trim", "trimStart",
    "trimEnd", "split", "replace", "replaceAll", "startsWith", "endsWith", "padStart",
    "padEnd", "charAt", "substring", "substr", "repeat", "match", "search", "localeCompare",
    "then", "catch", "finally", "json", "text", "blob", "arrayBuffer", "clone", "ok",
    "status", "statusText", "headers", "body", "url", "type", "name", "message", "stack",
    "hasOwnProperty", "constructor", "prototype", "call", "apply", "bind",
    "textContent", "innerText", "innerHTML", "outerHTML", "className", "classList", "style",
    "dataset", "value", "checked", "disabled", "selectedIndex", "options", "files",
    "href", "src", "alt", "title", "id", "tagName", "children", "parentNode", "parentElement",
    "firstChild", "lastChild", "nextSibling", "childNodes", "appendChild", "removeChild",
    "insertBefore", "setAttribute", "getAttribute", "removeAttribute", "addEventListener",
    "removeEventListener", "querySelector", "querySelectorAll", "closest", "matches",
    "getElementById", "getElementsByClassName", "createElement", "createTextNode",
    "insertAdjacentHTML", "scrollIntoView", "focus", "blur", "click", "submit", "reset",
    "preventDefault", "stopPropagation", "dispatchEvent", "remove", "contains", "replaceChildren",
    "toLocaleString", "toISOString", "getTime", "getFullYear", "getMonth", "getDate",
    "getHours", "getMinutes", "getSeconds", "now", "setTimeout", "setInterval", "clearTimeout",
    "clearInterval", "stringify", "parse", "assign", "freeze", "from", "of", "isArray",
    "getItem", "setItem", "removeItem", "clear", "protocol", "host", "hostname", "port",
    "pathname", "search", "hash", "origin", "searchParams", "textContent_", "localStorage",
    "sessionStorage", "location", "history", "reload", "replace_", "state", "error", "cause",
}


def strip_js_comments(src: str) -> str:
    """删掉 JS 注释（字符串内部的 `//` 不能动，否则会把 URL 切掉）。

    注释必须删：注释里常写着 `// fetch("/api/pair/nodes")` 之类示例，留着会造假红。
    """
    out: list[str] = []
    i, n = 0, len(src)
    while i < n:
        ch = src[i]
        if ch in "\"'`":
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                if src[i] == "\\":
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                if src[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if ch == "/" and i + 1 < n and src[i + 1] == "*":
            end = src.find("*/", i + 2)
            stop = n if end < 0 else end + 2
            # 块注释里可能有换行：删掉正文但把换行原样补回来，否则注释之后的报错行号会整体前移。
            out.append("\n" * src.count("\n", i, stop))
            i = stop
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def balanced(text: str, open_idx: int) -> str:
    """返回 text[open_idx] 这一对括号/花括号内部的原文（假定注释已删、字符串已按引号跳过）。"""
    opener = text[open_idx]
    closer = {"(": ")", "{": "}", "[": "]"}[opener]
    depth, i, n = 0, open_idx, len(text)
    while i < n:
        ch = text[i]
        if ch in "\"'`":
            quote = ch
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == quote:
                    break
                i += 1
            i += 1
            continue
        if ch in "({[":
            depth += 1
        elif ch in ")}]":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i]
        i += 1
    return text[open_idx + 1:]


def line_of(text: str, idx: int) -> int:
    return text.count("\n", 0, idx) + 1


def top_level_keys(body: str) -> list[str]:
    """取对象字面量第一层的键名（`{ssid, password: p, nested: {a:1}}` → ssid, password, nested）。

    只取第一层：嵌套对象（如 §12.6 的 precheck）在契约里是独立字段，混在一起会把
    `precheck.ap_found` 误判成请求字段。
    """
    keys: list[str] = []
    depth, i, n = 0, 0, len(body)
    while i < n:
        ch = body[i]
        if ch in "{[":
            depth += 1
            i += 1
            continue
        if ch in "}]":
            depth -= 1
            i += 1
            continue
        if ch in "\"'`":
            quote = ch
            i += 1
            while i < n and body[i] != quote:
                i += 2 if body[i] == "\\" else 1
            i += 1
            continue
        if depth == 1 and (ch.isalpha() or ch in "_$"):
            m = re.match(r"[A-Za-z_$][\w$]*", body[i:])
            word = m.group(0)
            j = i + len(word)
            while j < n and body[j] in " \t\r\n":
                j += 1
            if j < n and body[j] == ":" and body[j:j + 2] != "::":
                if word not in keys:
                    keys.append(word)
                i = j + 1
                continue
            if j < n and body[j] in ",}":
                # 简写属性：`JSON.stringify({ ssid, password })` 里两个键不带冒号，
                # 只抓 `key:` 的写法会把整份请求体看成「没有字段」——正是最危险的假绿。
                # 用「前一个非空白字符是 { 或 ,」把「冒号后的值」和展开 `{...payload}` 排除掉。
                k = i - 1
                while k >= 0 and body[k] in " \t\r\n":
                    k -= 1
                if k >= 0 and body[k] in "{," and word not in keys:
                    keys.append(word)
            i = j
            continue
        i += 1
    return keys


def read_js_source(path: str) -> tuple[list[tuple[str, str]], list[str], list[str]]:
    """读被检文件 → ([(来源文件, JS 正文), …], 提示, 问题)。

    带上「来源文件」是为了报错行号指对人：外链脚本里的错误若统一按页面文件名报，
    现场会拿着错误的行号去看 HTML（`bad.html:3` 其实是 `app.js:3`）。

    .js 直接读；.html 取 <script> 内联块 + 同目录的本地外链（契约 §12.11 约束 1 要求单文件
    内联，所以外链本身记一条提示，但内容照样核，避免「只有一个 CDN 引用」时漏检）。
    """
    src = read(path)
    notes: list[str] = []
    problems: list[str] = []
    ext = os.path.splitext(path)[1].lower()
    if ext in (".js", ".mjs", ".cjs"):
        return [(path, src)], [f"输入按 JS 源文件处理（{os.path.basename(path)}）"], problems

    base = os.path.dirname(os.path.abspath(path))
    # §12.11 #1/#2：单文件、无 CDN/外网依赖（AP 模式下取不到任何外部资源，页面会缺样式/图标）
    for m in re.finditer(r"<link\b[^>]*href\s*=\s*[\"'](https?:)?//([^\"']+)", src, re.I):
        problems.append(
            f"页面引用了外部资源 {m.group(2)}：§12.11 要求单文件内联、全离线（AP 下取不到会缺样式）"
        )
    for m in re.finditer(r"@import\s+(?:url\()?[\"']?(https?:)?//([^\"')]+)", src, re.I):
        problems.append(f"CSS 里 @import 了外部样式 {m.group(2)}：§12.11 #2 要求全离线")
    for m in re.finditer(r"url\(\s*[\"']?(https?:)?//([^\"')]+)", src, re.I):
        problems.append(f"样式里引用了外部资源 {m.group(2)}（字体/图标库）：§12.11 #1 要求内联")
    blocks: list[tuple[str, str]] = []
    for m in re.finditer(r"<script\b([^>]*)>(.*?)</script\s*>", src, re.S | re.I):
        attrs, body = m.group(1), m.group(2)
        type_m = re.search(r"type\s*=\s*[\"']([^\"']+)[\"']", attrs, re.I)
        if type_m and not re.search(r"javascript|module|^$", type_m.group(1), re.I):
            continue  # 模板/json 块不是 JS
        src_m = re.search(r"src\s*=\s*[\"']([^\"']+)[\"']", attrs, re.I)
        if src_m:
            ref = src_m.group(1)
            if re.match(r"https?://|//", ref):
                problems.append(
                    f"页面引用了外部脚本 {ref}：§12.11 要求单文件内联、全离线，去掉 CDN/外链"
                )
                continue
            local = os.path.normpath(os.path.join(base, ref))
            if os.path.isfile(local):
                blocks.append((local, read(local)))
                notes.append(f"已读入外链脚本 {ref}（§12.11 要求单文件内联，建议内联进页面）")
            else:
                notes.append(f"外链脚本 {ref} 不在仓库里，本次没核到它的接口/字段")
            continue
        blocks.append((path, body))

    if not blocks:
        # 没有 <script> 标签：可能被检的是片段或纯 JS，整份按 JS 处理（宁可多核一遍）
        notes.append("页面里没有 <script> 块，按整份文本处理")
        blocks = [(path, src)]
    return blocks, notes, problems


def enclosing_call(text: str, idx: int) -> dict | None:
    """找到下标 idx 处字面量所属的那一层调用（`api('/api/x', {...})` → name=api, args=`'/api/x', {...}`）。

    为什么要往上找一层：真页面几乎都会把 fetch 收进一个 `api()`/`request()` 小函数，
    路径字面量出现在**包装函数的调用点**，只认 `fetch(` 会一条路径都提取不到（假绿）。
    """
    i, depth = idx, 0
    while i >= 0:
        ch = text[i]
        if ch in ")]}":
            depth += 1
        elif ch in "([{":
            if depth == 0 and ch == "(":
                j = i - 1
                while j >= 0 and text[j] in " \t\r\n":
                    j -= 1
                k = j
                while k >= 0 and (text[k].isalnum() or text[k] in "_$.[]"):
                    k -= 1
                callee = text[k + 1:j + 1]
                return {
                    "name": callee.split(".")[-1].strip("[]"),
                    "start": k + 1,
                    "args": balanced(text, i),
                }
            depth -= 1
        i -= 1
    return None


def parse_js_calls(text: str) -> list[dict]:
    """提取调用的本地接口 → [{kind,method,path,start,end,line}]。

    主路径是**按 `/api/...` 字面量反查所属调用**（这样包装函数、fetch、XHR 三种写法都能覆盖）；
    method 从同一层调用里的 `method: "POST"` / `.open("POST", …)` / `.post(…)` 推。
    推不出来的记 None —— 宁可说「动词未定」，也不要假设成 GET 造成假红/假绿。
    """
    calls: list[dict] = []
    for pm in re.finditer(r"[\"'`](/api/[^\"'`\s]+)[\"'`]", text):
        path = pm.group(1).split("?")[0]
        call = enclosing_call(text, pm.start())
        method: str | None = None
        kind = "wrapper"  # 默认：路径字面量写在本地包装函数的调用点（H5 最常见的写法）
        start, end = pm.start(), pm.end()
        if call:
            args = call["args"]
            name = call["name"]
            if name == "fetch":
                kind = "fetch"
            elif name == "open":  # xhr.open("POST", "/api/…")
                kind = "xhr"
            elif name.lower() in ("get", "post", "put", "delete"):
                kind = "call"
            found = re.findall(r"method\s*:\s*[\"']([A-Za-z]+)[\"']", args)
            if found:
                method = found[0].upper()
            elif kind == "call":
                method = name.upper()
            elif kind == "xhr":
                m2 = re.match(r"\s*[\"']([A-Za-z]+)[\"']", args)
                method = m2.group(1).upper() if m2 else None
            elif kind == "fetch":
                method = "GET"  # fetch 不给 method 就是 GET，这条是契约 §12.4 的 GET 接口
            start, end = call["start"], call["start"] + len(args) + 1
        calls.append({
            "kind": kind, "method": method, "path": path,
            "start": start, "end": end, "line": line_of(text, pm.start()),
        })
    return calls


def parse_js_request_keys(text: str, calls: list[dict]) -> list[tuple[str, int, int]]:
    """请求体键名（key, 行号, 字符下标）。只认 fetch/XHR 调用内部、`body: {...}` 与命名像请求体的变量，
    否则页面上任何 `JSON.stringify({a:1})`（比如写 localStorage）都会变成假红。"""
    out: list[tuple[str, int, int]] = []
    spans = [(c["start"], c["end"]) for c in calls]

    def inside_call(idx: int) -> bool:
        return any(s <= idx <= e for s, e in spans)

    for m in re.finditer(r"JSON\.stringify\s*\(", text):
        if not inside_call(m.start()):
            continue
        for k in top_level_keys(balanced(text, m.end() - 1)):
            out.append((k, line_of(text, m.start()), m.start()))
    for m in re.finditer(r"body\s*:\s*\{", text):
        # balanced() 返回的是**括号里面**的文本；top_level_keys 靠「第 1 层」判顶层键，
        # 少了外层花括号就永远停在深度 0 —— 这会让 `body: {...}` 里的字段一个都核不到（假绿）。
        for k in top_level_keys("{" + balanced(text, text.index("{", m.start())) + "}"):
            out.append((k, line_of(text, m.start()), m.start()))
    for m in re.finditer(r"\.append\s*\(\s*[\"'](\w+)[\"']", text):
        out.append((m.group(1), line_of(text, m.start()), m.start()))
    for m in re.finditer(r"(?:const|let|var)\s+(\w+)\s*=\s*\{", text):
        name = m.group(1)
        if not re.search(r"body|payload|req|param", name, re.I):
            continue
        for k in top_level_keys("{" + balanced(text, text.index("{", m.start())) + "}"):
            out.append((k, line_of(text, m.start()), m.start()))
    return out


def parse_js_response_roots(text: str) -> tuple[set[str], set[str], list[tuple[str, int]]]:
    """响应根变量：`const json = await res.json()`、`JSON.parse(...)`、`.then(j => ...)`、
    以及本地小函数 `return json.data` 的结果变量（H5 常见的 `api()` 包装）。

    返回 (根变量集合, 包装函数集合, 解构出来的 (字段名, 字符下标) 列表)。
    """
    roots: set[str] = set(JS_ROOT_LITERALS)
    destructured: list[tuple[str, int]] = []
    for m in re.finditer(r"(?:const|let|var)\s+(\w+)\s*=\s*[^;\n]*?\.json\s*\(", text):
        # `[^;\n]*?` 而不是 `[\w$.\[\]]*`：真页面几乎都写 `await (await fetch(...)).json()`，
        # 中间有括号和引号，旧模式匹配不到 → 那个变量不被当成响应根 → 它的字段访问全部漏检（假绿）。
        roots.add(m.group(1))
    for m in re.finditer(r"(?:const|let|var)\s+(\w+)\s*=\s*JSON\.parse\s*\(", text):
        roots.add(m.group(1))
    for m in re.finditer(r"\.then\s*\(\s*(\w+)\s*=>", text):
        roots.add(m.group(1))
    for m in re.finditer(r"(?:const|let|var)\s*\{([^}]*)\}\s*=\s*(?:await\s+)?[\w$.\[\]]*\.json\s*\(", text):
        destructured += [(w, m.start()) for w in re.findall(r"[A-Za-z_$][\w$]*", m.group(1))]
    wrappers: set[str] = set()
    for m in re.finditer(
        r"(?:function\s+(\w+)|(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s*)?\(?[^()]*\)?\s*=>)", text
    ):
        name = m.group(1) or m.group(2)
        # 只看这个函数**自己的函数体**：早先取「头之后 600 字符」，会把后面那个真包装函数
        # 的 `return x.data` 算到前一个函数头上，于是每个普通函数都被标成包装函数。
        brace = text.find("{", m.end())
        semi = text.find(";", m.end())
        if brace != -1 and (semi == -1 or brace < semi) and brace - m.end() < 80:
            body = balanced(text, brace)
        else:  # 表达式体箭头函数：`p => fetch(p).then(r => r.json())`
            body = text[m.end(): semi if semi != -1 else m.end() + 200]
        if re.search(r"return\s+[\w$?.\[\]]*\.\s*data\b", body):
            wrappers.add(name)
    for m in re.finditer(r"(?:const|let|var)\s+(\w+)\s*=\s*(?:await\s+)?([\w$]+)\s*\(", text):
        if m.group(2) in wrappers:
            roots.add(m.group(1))
    return roots, wrappers, destructured


def js_field_accesses(text: str, roots: set[str]) -> tuple[list[tuple[str, str, int, int]], list[tuple[int, int]]]:
    """响应字段访问 → ([(容器说明, 字段名, 行号, 字符下标)], 已归属数组元素的字符区间)。

    容器说明用于报错时指路（`json.x` / `nodes[].sn`）。第二项让调用方知道哪些位置已经
    按「数组元素」归属过了：同一个局部变量名（如 `b`）既可能是响应根、又可能是 `bound`
    的元素，不排除就会把 `b.mac` 当成响应根字段报假红。第四项（字符下标）用于把错误
    指到**真正的来源文件**——页面内联块与外链脚本混在一起时行号会张冠李戴。
    """
    out: list[tuple[str, str, int, int]] = []
    attributed: list[tuple[int, int]] = []
    # 可选链 `json?.data?.mode` 在真页面里很常见：所有「点号」都要写成 `\s*\??\.` 才匹配得到。
    # 漏掉它的后果不是报红而是**假绿**——整份页面提取出 0 处字段访问，却照样打印「一致」。
    # 顺序要紧：先做「数组元素」归属（§12.8 的 nodes[]/bound[]），再扫响应根。
    # §12.6 的 precheck 子对象：`data.precheck.ap_found`
    for m in re.finditer(r"\bprecheck\s*\??\.\s*([A-Za-z_$][\w$]*)", text):
        out.append(("precheck", m.group(1), line_of(text, m.start()), m.start()))
    # §12.8 的数组元素：`data.nodes.forEach(n => n.sn)` / `nodes.map(({sn, mac}) => ...)` / `nodes?.[0]?.sn`
    for parent in JS_ARRAY_PARENTS:
        for m in re.finditer(
            rf"\.{parent}\s*\??\.\s*(?:forEach|map|filter|find|some|every|flatMap)\s*\(",
            text,
        ):
            open_idx = text.index("(", m.end() - 1)
            region = balanced(text, open_idx)
            head = re.match(r"\s*\(?\s*(\w+)", region)
            if not head:
                continue
            alias = head.group(1)
            if alias in ("function", "async", "new") or alias in JS_BUILTINS:
                continue  # `nodes.forEach(function (n) {...})`：别把关键字当元素变量
            # 只在回调体里查这个别名：页面上另外几处同名局部变量（比如另一个接口的响应 `b`）
            # 不属于 nodes[]/bound[]，扫全文会把它们误判成 `bound[].bound` 这种假红。
            for am in re.finditer(rf"\b{re.escape(alias)}\s*\??\.\s*([A-Za-z_$][\w$]*)", region):
                idx = open_idx + 1 + am.start()
                out.append((f"{parent}[]", am.group(1), line_of(text, idx), idx))
                attributed.append((idx, idx + len(am.group(0))))
        for m in re.finditer(rf"\.{parent}\s*\??\.\s*(?:forEach|map)\s*\(\s*\(\s*\{{([^}}]*)\}}", text):
            for field in re.findall(r"[A-Za-z_$][\w$]*", m.group(1)):
                out.append((f"{parent}[]", field, line_of(text, m.start()), m.start()))
        for m in re.finditer(rf"\.{parent}\s*\??\.?\s*\[[^\]]*\]\s*\??\.\s*([A-Za-z_$][\w$]*)", text):
            out.append((f"{parent}[]", m.group(1), line_of(text, m.start()), m.start()))
    for root in sorted(roots):
        for m in re.finditer(rf"\b{re.escape(root)}\s*\??\.\s*([A-Za-z_$][\w$]*)", text):
            if any(s <= m.start() < e for s, e in attributed):
                continue  # 这处已经按数组元素归属过了，别当成响应根字段再报一次
            out.append((root, m.group(1), line_of(text, m.start()), m.start()))
    return out, attributed


def check_js(path: str, contract_path: str) -> int:
    if not os.path.isfile(path):
        print(f"!! 找不到被检文件：{path}")
        return 1
    contract = read(contract_path)
    endpoints = parse_endpoints(contract)
    sections = parse_section_tables(contract)
    want = {(m, _norm_path(p)) for m, p in endpoints}
    resp_fields = {f for sec in RESPONSE_SECTIONS for f in sections[sec]["response"]}
    req_fields = {f for sec in REQUEST_SECTIONS for f in sections[sec]["request"]}
    # 嵌套字段的父名（契约表里只有 `precheck.ap_found` 这种行，父名本身不是一行）也要放行
    nested_parents = {"precheck", "nodes", "bound"}
    allowed_top = resp_fields | nested_parents | JS_ENVELOPE | JS_BUILTINS

    blocks, notes, problems = read_js_source(path)
    # 逐块去注释后再拼：去注释会删掉字符，先拼再删会让「字符下标 → 行号」整体错位。
    parts: list[str] = []
    segs: list[tuple[int, str]] = []  # (该块在拼接文本里的起点, 来源文件)
    pos = 0
    for origin, text in blocks:
        segs.append((pos, origin))
        stripped = strip_js_comments(text)
        parts.append(stripped)
        pos += len(stripped) + 1  # "\n".join 的分隔符
    js = "\n".join(parts)

    def loc(idx: int) -> str:
        """字符下标 → 「来源文件:行号」。

        外链脚本必须指到它自己的文件：否则 `app.js` 里的错会报成 `index.html:3`，
        现场拿着页面文件找不到那一行（比不报还费时间）。
        """
        seg = segs[0]
        for s in segs:
            if s[0] <= idx:
                seg = s
            else:
                break
        line = line_of(js, idx) - line_of(js, seg[0]) + 1
        origin = os.path.relpath(seg[1], ROOT) if os.path.isabs(seg[1]) else seg[1]
        return f"{origin}:{line}"

    calls = parse_js_calls(js)
    # 按路径归并：同一条路径上出现过的动词集合（推不出动词就记 None）
    by_path: dict[str, dict] = {}
    for c in calls:
        p = _norm_path(c["path"])
        rec = by_path.setdefault(p, {"methods": set(), "call": c})
        if c["method"]:
            rec["methods"].add(c["method"])
        if rec["call"]["method"] is None and c["method"] is not None:
            rec["call"] = c  # 报行号时优先用能看出动词的那一处
    want_by_path = {_norm_path(p): m for m, p in endpoints}
    unknown_method = sorted(p for p, r in by_path.items() if not r["methods"])
    spliced = sorted(p for p in by_path if p.endswith("/"))

    for p in sorted(by_path):
        rec = by_path[p]
        where = loc(rec["call"]["start"])
        if p not in want_by_path:
            shown = "/".join(sorted(rec["methods"])) or "动词未定"
            if not re.match(r"^/(provision|pair)\b", p):
                # 首段不是 §12 的 provision/pair → 基本就是 §6 云端 REST（§12.11 #3 禁止 H5 调）
                problems.append(
                    f"页面调了 §12 之外的接口（很可能是 §6 云端 REST，§12.11 #3 禁止 H5 调用）："
                    f"{shown} {p}（{where}）"
                )
            else:
                problems.append(f"调用了 §12 之外的本地接口：{shown} {p}（{where}）")
            close = [q for q in want_by_path if q.startswith(p.rstrip("/"))]
            if close:
                problems.append(f"   ↳ 最接近的契约路径：{'/api' + close[0]}（是不是想调它？）")
        else:
            wrong = sorted(m for m in rec["methods"] if m != want_by_path[p])
            if wrong:
                problems.append(
                    f"动词与契约不符：{p} 契约是 {want_by_path[p]}，页面写 {'/'.join(wrong)}（{where}）"
                )
    missing = sorted(p for p in want_by_path if p not in by_path)
    if not by_path:
        # 一条路径都提不到 = 这条链路根本没核过，必须报红，不能悄悄打印「一致」。
        problems.append(
            "没有从被检文件里提取到任何 /api/ 路径：本工具只做静态核对，"
            "若页面用变量/拼接构造 URL，请改成字面量，或人工核对"
        )

    put_keys = parse_js_request_keys(js, calls)
    bad_req = sorted({(k, idx) for k, _line, idx in put_keys if k not in req_fields})
    for k, idx in bad_req:
        problems.append(f"页面请求字段 `{k}` 不在 §12 请求表里（{loc(idx)}）")
        similar = [f for f in req_fields if f.lower() == k.lower() or f.replace("_", "") == k.replace("_", "").lower()]
        if similar:
            problems.append(f"   ↳ 是不是想写 `{similar[0]}`？")

    roots, wrappers, destructured = parse_js_response_roots(js)
    accesses, _elem_spans = js_field_accesses(js, roots)
    unknown: dict[tuple[str, str], int] = {}
    for container, field, _line, idx in accesses:
        if container == "precheck":
            if field not in JS_NESTED_FIELDS["precheck"] | JS_BUILTINS:
                unknown.setdefault((container, field), idx)
            continue
        if container.endswith("[]"):
            parent = container[:-2]
            if field not in JS_NESTED_FIELDS[parent] | JS_BUILTINS:
                unknown.setdefault((container, field), idx)
            continue
        if field not in allowed_top:
            unknown.setdefault((container, field), idx)
    for field, idx in destructured:
        if field not in allowed_top and field not in JS_BUILTINS:
            unknown.setdefault(("解构", field), idx)
    for (container, field), idx in sorted(unknown.items()):
        problems.append(
            f"响应字段 `{container}.{field}` 不在契约 §12.5–§12.9 里（{loc(idx)}）"
        )
        similar = [f for f in resp_fields if f.lower() == field.lower() or f.replace("_", "") == field.replace("_", "").lower()]
        if similar:
            problems.append(f"   ↳ 是不是想写 `{similar[0]}`？")

    print(f"H5 核对：{os.path.relpath(path, ROOT) if os.path.isabs(path) else path}")
    for n in notes:
        print(f"  · {n}")
    print(f"契约 §12 接口 {len(endpoints)} 个：{', '.join(m + ' ' + p for m, p in endpoints)}")
    kinds = sorted({c["kind"] for c in calls})
    have_shown = [f"{'/'.join(sorted(by_path[p]['methods'])) or '?'} {p}" for p in sorted(by_path)]
    print(f"页面调用 {len(by_path)} 条路径（{'/'.join(kinds) or '无'}）：{', '.join(have_shown) or '无'}")
    if missing:
        print("提示（不算失败，页面可以只实现其中几步）：契约有、页面没调："
              + ", ".join(f"{want_by_path[p]} {p}" for p in missing))
    if unknown_method:
        print(f"提示：以下路径的动词静态推不出（包装函数里没写 method），只核了路径："
              f"{', '.join(unknown_method)}")
    if spliced:
        print(f"提示：以下路径由字符串拼接而成，静态核对只看得到前缀：{', '.join(spliced)}")
    if wrappers:
        print(f"提示：识别到本地包装函数 {', '.join(sorted(wrappers))}（其返回值按响应根处理）")
    req_names = sorted({k for k, _line, _idx in put_keys})
    print(f"请求字段 {len(req_names)} 个：{', '.join(req_names) or '无'}")
    print(f"响应字段访问 {len(accesses)} 处，覆盖 {len({f for _, f, _line, _idx in accesses})} 个字段名")
    if by_path and not accesses:
        # 防「工具自己坏了却报绿」：一处字段都没核到就不该让人以为核过了。
        print("提示（不算失败）：页面调了本地接口，但一处响应字段访问都没提取到——"
              "要么页面只发请求不看返回，要么用了本工具不认识的写法（如 json['data']），请人工再核一遍")
    if problems:
        print(f"!! 发现 {len(problems)} 处不一致：")
        for p in problems:
            print("   -", p)
        return 1
    print(f"一致：页面只调 §12 的本地接口，请求/响应字段名与契约 §12.4–§12.9 对齐"
          f"（已核请求字段 {len(req_names)} 个、响应字段访问 {len(accesses)} 处）")
    return 0


SAMPLES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "h5_samples")


def run_self_test(contract_path: str) -> int:
    """负向验证：故意写错的样例必须报红，正确样例必须报绿。

    这个自检防的是「工具自己坏了却一直报绿」——比页面写错更危险，
    因为它会让后面所有人都以为已经核过了。三个样例分别盯：
    good.html 正确页面、bad.html 路径/动词/字段名写错、bad2.html 可选链与简写属性写法。
    """
    good = os.path.join(SAMPLES_DIR, "good.html")
    bad = os.path.join(SAMPLES_DIR, "bad.html")
    bad2 = os.path.join(SAMPLES_DIR, "bad2.html")
    print("== 自检 1/3：正确样例（期望退出码 0）")
    good_rc = check_js(good, contract_path)
    print("\n== 自检 2/3：故意写错的样例（期望退出码 1）")
    bad_rc = check_js(bad, contract_path)
    print("\n== 自检 3/3：可选链 + 简写属性的错误样例（期望退出码 1）")
    bad2_rc = check_js(bad2, contract_path)

    # 再直接问一遍解析器：正确样例必须真的核出东西。否则「一致」可能只是「什么都没提取到」，
    # 而这不依赖任何样例内容，纯靠断言兜住。
    blocks, _notes, _problems = read_js_source(good)
    js = strip_js_comments("\n".join(text for _origin, text in blocks))
    roots, _wrappers, _destructured = parse_js_response_roots(js)
    calls = parse_js_calls(js)
    accesses, _spans = js_field_accesses(js, roots)
    req_keys = parse_js_request_keys(js, calls)
    n_resp = len({f for _, f, _line, _idx in accesses})
    n_req = len({k for k, _line, _idx in req_keys})
    extracted_ok = n_resp >= 10 and n_req >= 8
    print(f"\n自检附加断言：正确样例提取到请求字段 {n_req} 个（期望 ≥8）、响应字段 {n_resp} 个（期望 ≥10）"
          f" → {'通过' if extracted_ok else '失败（提取为空，会假绿）'}")

    ok = good_rc == 0 and bad_rc == 1 and bad2_rc == 1 and extracted_ok
    print(f"自检结果：正确样例 rc={good_rc}（期望 0）、错误样例 rc={bad_rc}（期望 1）、"
          f"可选链样例 rc={bad2_rc}（期望 1）→ {'通过' if ok else '失败'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
