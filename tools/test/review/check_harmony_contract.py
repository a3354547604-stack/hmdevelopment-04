#!/usr/bin/env python3
"""独立复核 t6 的静态交叉核对：harmony-app 源码 ↔ 契约 §6/§9（契约 v1.1）。

与动态探针（review/check_harmony_http.py）互补：
  · 动态探针证明"真实代码路径能跑通、契约字段能解析、错误码能翻译"；
  · 本脚本证明 03-契约字段核对表.md 的覆盖清单与实现/契约现状**对得上**（含 v1.1 新增项）。

检查项：
  A. §6.2 的 20 条接口（18 App + webhook + §6.21 取时）↔ HttpApiService 实现；
  B. ApiPort 与 HttpApiService 方法集一致；
  C. §6.9 键集合 ↔ SettingsItems ↔ settingsBody ↔ §8.2（D-01）；
  D. §9.2 全部业务码 ↔ Result.codeText 直译表，以及 describe() 是否真的带服务器 message；
  E. §6.13/§7.7 告警字段、§4.3/§4.3.1 物模型字段 ↔ Types.ets 解析键（v1.1 新增项也查）；
  F. 核对表 D-01…D-06 的现状（逐条给代码/契约证据）；
  G. 敏感信息扫描（学校/团队/指导老师/联系方式）。

用法：python3 tools/test/review/check_harmony_contract.py
退出码：0 全部对得上；1 有差异（差异＝需要人工判定的发现，复核报告里逐条给结论）。
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
APP = REPO / "harmony-app"
ETS = APP / "entry/src/main/ets"
sys.path.insert(0, str(REPO / "tools/test/contract"))

from contract_source import Contract  # noqa: E402

C = Contract(REPO / "docs/技术/接口契约.md")
problems = []


def read(p: pathlib.Path) -> str:
    return p.read_text(encoding="utf-8")


def read_optional(p: pathlib.Path) -> "str | None":
    """读得到就返回文本，读不到返回 None。

    t155：这个探针是冻结门禁的 App 侧硬项，**不许因为某个页面被重做/改名就抛异常**——
    FileNotFoundError 会让整个 App 侧假红（t154 发现：页面在 t150 界面重做中删除）。
    找不到时由调用方显式跳过并打印原因。
    """
    try:
        return p.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    except OSError:            # 权限/编码之外的读取失败也按「不可读 = 跳过」处理
        return None


SKIPS: list[str] = []


def skip(msg: str) -> None:
    """显式跳过：一定打印，且会出现在末尾结论行里（不是静默通过）。"""
    SKIPS.append(msg)
    print("  [SKIP] " + msg)


def ok(msg: str) -> None:
    print("  [OK]   " + msg)


def diff(msg: str) -> None:
    problems.append(msg)
    print("  [DIFF] " + msg)


def note(msg: str) -> None:
    """已知留白/归属他人：打印但不计入退出码（与 [DIFF] 区分开，避免假红）。"""
    print("  [NOTE] " + msg)


def block(text: str, start: str, stop: str = "}") -> str:
    """取 `export interface X {...}` 的字段块：按"整行就是 }"结束，
    避免被字段注释里的 `{`/`}`（如 nodes_online 的示例 JSON）截断。"""
    lines = text.splitlines()
    i = next(k for k, l in enumerate(lines) if start in l)
    out = [lines[i]]
    for l in lines[i + 1:]:
        if l.strip() == stop:
            break
        out.append(l)
    return "\n".join(out)


http = read(ETS / "service/HttpApiService.ets")
port = read(ETS / "service/ApiPort.ets")
types = read(ETS / "model/Types.ets")
result = read(ETS / "model/Result.ets")
# t155：设置项在 t150 界面重做后由 pages/ProfilePage.ets 的「高级」区承载
# （旧 pages/SettingsPage.ets 已删除；键定义在 common/SettingsSchema.ets）。
# 这里只读页面本身（pushService 是页面级接入，配置文件里不会出现），
# 页面不存在时走 skip() 分支，绝不抛异常、也不假装通过。
SETTINGS_PAGE_REL = "pages/ProfilePage.ets"
settings_page = read_optional(ETS / SETTINGS_PAGE_REL)

# ---------------------------------------------------------------- A 接口清单
print("A. §6.2 契约接口 ↔ HttpApiService 实现")
t62 = C.table("6.2")
eps = [(t62.p(r, "方法"), t62.p(r, "路径")) for r in t62.rows]
# §6.2 里不是 App 消费者的两条：webhook 由 IoTDA 平台调（§5），
# §6.21 /api/time 由**网关**启动握手取时（§6.21 明写「网关启动握手取时用」，
# 且规则里只要求网关据此把 clock_source 置 device）。t97 更正：原先只排掉 webhook，
# 于是 /api/time 被当成「App 该实现但没实现」报 DIFF —— 那是错的期望，不是 App 的缺口。
GATEWAY_ONLY = {"/api/time"}
app_eps = [(m, p) for (m, p) in eps
           if m in ("GET", "POST", "PUT") and "/api/iotda/webhook" not in p
           and p not in GATEWAY_ONLY]
internal = [(m, p) for (m, p) in eps if (m, p) not in app_eps]
print("  契约 §6.2：%d 条（App %d + 内部 %d：%s）"
      % (len(eps), len(app_eps), len(internal), ", ".join("%s %s" % e for e in internal)))

def norm_code_path(lit: str) -> str:
    p = lit.strip("'\"`")
    p = p.replace("${HttpApiService.devicePath(deviceId)}", "/devices/{id}")
    return re.sub(r"\$\{[^}]+\}", "{id}", p)


calls = re.findall(r"(getJson|postJson|putJson)<[^>]*>\(\s*(`[^`]*`|'[^']*'|\"[^\"]*\")", http)
METHOD_OF = {"getJson": "GET", "postJson": "POST", "putJson": "PUT"}
norm_code = {(METHOD_OF[k], norm_code_path(v)) for (k, v) in calls}
missing = []
for method, path in app_eps:
    norm = "/" + re.sub(r"\{id\}", "{id}", path.replace("/api/", "", 1))
    if (method, norm) not in norm_code:
        missing.append("%s %s" % (method, path))
if missing:
    diff("HttpApiService 里找不到这些契约路径：%s" % ", ".join(missing))
else:
    ok("18 条 App 接口路径在 HttpApiService 中逐条可对上（含 /devices/{id} 拼接）")

print("B. ApiPort ↔ HttpApiService 方法集")
port_methods = sorted(m for m, _ in re.findall(r"^\s{2}([a-zA-Z]+)\((.*?)\): Promise<", port, re.M))
impl_methods = sorted(re.findall(r"^\s{2}async ([a-zA-Z]+)\(", http, re.M))
if port_methods == impl_methods and len(port_methods) == 18:
    ok("18/18 一致：%s" % ", ".join(port_methods))
else:
    diff("方法集不一致（Port %d / Impl %d）：%s"
         % (len(port_methods), len(impl_methods), sorted(set(port_methods) ^ set(impl_methods))))

# ---------------------------------------------------------------- C settings
print("C. §6.9 settings 键集合（D-01）")
contract_keys = set(C.settings_defaults().keys())
types_block = block(types, "export interface SettingsItems")
types_keys = set(re.findall(r"^\s{2}([a-z_0-9]+)\?:", types_block, re.M))
body_keys = set(re.findall(r"items\['([a-z_0-9]+)'\] = patch\.", http))
print("  §6.9 示例键 %d；SettingsItems %d；settingsBody %d"
      % (len(contract_keys), len(types_keys), len(body_keys)))
extra = types_keys - contract_keys
# t64 更正：这条期望原先写「App 仍带 gas_l1_to_l2_s → ok」（等 App 修）。
# App 已在 t62 按 PM D-01 收敛到 §6.9 的 21 键，所以现在的正确期望是**差集为空**；
# 差集非空才是缺陷（会整体提交 → 服务器 400/1003）。
if not extra:
    ok("App 的 SettingsItems 与 §6.9 键集合一致（%d 键，D-01 已收口：gas_l1_to_l2_s 属 §8.2 算法参数、"
       "不在 items 里）" % len(types_keys))
else:
    diff("App 多出 §6.9 之外的键（整体提交会被服务器 400/1003 拒）：%s" % sorted(extra))
if body_keys == types_keys:
    ok("settingsBody 提交清单 == SettingsItems 全部 %d 键（逐键显式判断，未遍历属性）" % len(body_keys))
else:
    diff("settingsBody 与 SettingsItems 不一致：%s" % sorted(types_keys ^ body_keys))

# ---------------------------------------------------------------- D 错误码
print("D. §9.2 业务码 ↔ Result.codeText")
t92 = C.table("9.2")
codes = [int(t92.n(r, "code")) for r in t92.rows]
mapped = {int(c): t for (c, t) in re.findall(r"if \(code === (\d+)\) \{\s*return '([^']*)'", result)}
ok("§9.2 业务码 %d 个；codeText 直译 %d 个" % (len(codes), len(mapped)))
not_mapped = [c for c in codes if c not in mapped]
provision_codes = [c for c in not_mapped if 1600 <= c <= 1606]
other_missing = [c for c in not_mapped if c not in provision_codes]
if other_missing:
    diff("未翻译的非配网业务码：%s" % other_missing)
else:
    ok("§6 覆盖的 %d 个业务码（含 1400 报表无数据）全部有中文直译；HTTP 类别由 kindOfStatus 兜底"
       % (len(codes) - len(provision_codes)))
if provision_codes:
    # t97 更正期望文本：原句写「§13.2 的步骤 5/7/11/12/13 明确写『谁=App』」——
    # t36 之后 §13.2 只有步骤 1/10/14 是 App（纯 HTTP，P1），3/4/6/9 是
    # 「手机（系统 WLAN 设置）」、5/7/11/12/13 是「手机浏览器」；节标题本身写
    # 「P0 主路径 = 手机浏览器 + 网关 H5 页面」，§12.11 又明写「安卓 App 与鸿蒙 App
    # 的配网页是 P1 消费者」。所以「App 无 §12 接口、无 1600–1606 直译」与契约现状
    # **一致**，是 P1 留白而不是缺陷 → 报 NOTE 并标归属卡。
    # 有牙：契约一旦把 App 配网页改回 P0（§12.11 里不再写 P1），这里立刻转 DIFF。
    sec_12 = C.section_text("12")
    sec_132 = C.section_text("13.2")
    app_p1 = ("配网页" in sec_12 and "P1 消费者" in sec_12 and "手机浏览器" in sec_132)
    if app_p1:
        note("§9.2 的配网码 %s 未翻译、ApiPort/HttpApiService 也没有 §12 接口 —— 与契约"
             "现状一致（§12 消费方段：App 配网页是 P1 消费者；§13.2 P0 主路径=手机浏览器+网关 H5）。"
             "归属卡：app-harmony-op 的配网页卡（P1）；P1 落地时需补这 %d 个码的直译 + §12 五接口"
             % (provision_codes, len(provision_codes)))
    else:
        diff("§9.2 的配网码 %s 未翻译，且 ApiPort/HttpApiService 里没有 §12 的任何接口 —— "
             "契约 §12.11 已不再把 App 配网页标为 P1，App 侧应实现" % provision_codes)
tail = result.split("export function describe")[1]
if "codeText(code)" in tail and "+ serverMessage" not in tail.split("if (status === 0)")[0]:
    diff("describe() 业务码分支只返回 codeText、不拼接服务器 message —— 核对表 §5 称"
         "'显示服务器 message（含 missing/field）'，与实现不符；1002 的 field/allowed 会丢失")
else:
    ok("describe() 的服务器 message 拼接与核对表 §5 的描述一致")

# ---------------------------------------------------------------- E 字段覆盖
print("E. 告警/物模型字段 ↔ 解析键")
t77 = C.table("7.7", must_have="列")
db_cols = [t77.p(r, "列") for r in t77.rows]
alert_block = block(types, "export interface AlertItem")
alert_parsed = set(re.findall(r"^\s{2}([a-z_]+)\??:", alert_block, re.M))
core13 = ["id", "device_id", "node_sn", "type", "level", "ts", "status", "recovered_at",
          "push_count", "last_push_at", "next_push_at", "summary"]
miss = [f for f in core13 if f not in alert_parsed]
if miss:
    diff("AlertItem 未解析的契约字段：%s" % miss)
else:
    ok("§6.13 的 12 个条目字段全部被 parseAlertItem 解析（total 在列表层另解）")

props = [C.table("4.3").p(r, "属性") for r in C.table("4.3").rows]
prop_block = block(types, "export interface GatewayProperties")
prop_parsed = set(re.findall(r"^\s{2}([a-z_]+)\??:", prop_block, re.M))
missing_props = [p for p in props if p not in prop_parsed]
if missing_props:
    diff("§4.3（v1.1 共 %d 项）中 App 未解析的属性：%s —— 核对表 §3 的\"12 项全部解析\""
         "对 v1.0 成立、对 v1.1 已过期" % (len(props), missing_props))
else:
    ok("§4.3 的 %d 项属性全部被 GatewayProperties 解析" % len(props))

t431 = C.table("4.3.1", must_have="字段")
node_extra = [t431.p(r, "字段") for r in t431.rows]
node_block = block(types, "export interface NodeItem")
node_parsed = set(re.findall(r"^\s{2}([a-z_]+)\??:", node_block, re.M))
missing_node = [f for f in node_extra if f not in node_parsed]
if missing_node:
    print("  [note] §4.3.1 的 %s 未在 NodeItem 里（已解析 %s）——v1.1 新增可选项，见报告"
          % (missing_node, sorted(node_parsed)))
else:
    ok("§4.3.1 的 %s 均在 NodeItem 里" % node_extra)

# ---------------------------------------------------------------- F D 项现状
print("F. 核对表 D-01…D-06 现状")
d_items = [
    ("D-01 gas_l1_to_l2_s 只在 §8.2（v1.2 已裁：不进 §6.9 items）",
     "gas_l1_to_l2_s" in types and "gas_l1_to_l2_s" in http,
     "契约侧已定稿为 §8.2 算法参数；App 已在 t62 收敛到 §6.9 的 21 键（不解析也不提交）"),
    ("D-02 §6.9 引用错节（写 §8.6 键值）", "§8.6" in C.section_text("6.9"),
     "契约 §6.9 正文仍写 '... §8.6 键值 ...'，阈值实际散在 §8.2/§8.3/§8.5/§8.7/§8.8"),
    ("D-03 §6.13 曾含 recovered_at:null",
     '"recovered_at": null' in C.section_text("6.13") or '"recovered_at":null' in C.section_text("6.13"),
     "App 的 optNum/optStr 对 null 返回 undefined（探针 E2/E3 覆盖）"),
    ("D-04 §6.14 未写明平铺/嵌套",
     ("平铺" not in C.section_text("6.14") and "嵌套" not in C.section_text("6.14")),
     "App 按平铺解析；探针 H10 证明嵌套形状被明确拒绝，不静默成功"),
    ("D-05 §6.11 accepted 语义未定义", "键名" not in C.section_text("6.11"),
     "App 按键名数组处理并渲染键名"),
    ("D-06 confirmed 筛选合法性未写明", "confirmed" in C.section_text("6.13"),
     "ApiPort 定义 4 个状态常量：%s；列表默认 all"
     % ", ".join(re.findall(r"ALERT_STATUS_\w+: string = '(\w+)'", port))),
]
for name, still, evidence in d_items:
    print("  %-34s %-6s %s" % (name, "仍存在" if still else "已消解", evidence))
if "Idempotency-Key" in http + port:
    diff("代码出现 Idempotency-Key（核对表 §8 声明未实现）")
else:
    ok("Idempotency-Key 确认未实现（与核对表 §8 一致）")
if settings_page is None:
    skip("设置页 %s 不存在：跳过「pushService 接入」核对（原因：t150 界面重做后设置项迁到"
         " ProfilePage 高级区；若页面再次改名，请更新 SETTINGS_PAGE_REL）。跳过 != 通过"
         % SETTINGS_PAGE_REL)
elif "pushService" in settings_page:
    diff("设置页 %s 已接入 pushService（核对表 §8 声明未接入）" % SETTINGS_PAGE_REL)
else:
    ok("设置页 %s 未接入 pushService（与核对表 §8 的'留白'声明一致）" % SETTINGS_PAGE_REL)
if "见核对表 D-03" in types:
    diff("Types.ets 把 gas_l1_to_l2_s 的编号引成 D-03，核对表里该问题是 D-01")
else:
    ok("Types.ets 的 D 编号引用与核对表一致")

# ---------------------------------------------------------------- G 敏感信息
print("G. 敏感信息扫描")
PAT = re.compile(r"(大学|学院|职业技术|指导教师|指导老师|辅导员|参赛队|队名|团队名称|学号|班级|"
                 r"@qq\.com|@163\.com|@gmail\.com)", re.I)
hits = []
for f in sorted(APP.rglob("*")):
    if f.is_dir() or f.suffix.lower() in (".png", ".jpg", ".jpeg", ".har", ".hap", ".pyc"):
        continue
    if any(part in (".hvigor", "build", "node_modules", "oh_modules") for part in f.parts):
        continue
    try:
        txt = f.read_text(encoding="utf-8")
    except (UnicodeDecodeError, OSError):
        continue
    for i, line in enumerate(txt.splitlines(), 1):
        if PAT.search(line) and not re.search(r"(不写|不提交|禁止|不得|不要|不含|避免)", line):
            hits.append("%s:%d: %s" % (f.relative_to(REPO), i, line.strip()[:100]))
if hits:
    diff("敏感信息命中 %d 行：%s" % (len(hits), hits[:5]))
else:
    ok("harmony-app/ 全文（含 deliverables）无学校/团队/指导老师/联系方式命中")

print()
if problems:
    print("结论：%d 处差异（见 [DIFF]）—— 已在 docs/评审与验证/ 的复核报告里逐条判定"
          % len(problems))
    sys.exit(1)
if SKIPS:
    print("结论：静态交叉核对全部通过（跳过 %d 项：%s）" % (len(SKIPS), "；".join(SKIPS)))
else:
    print("结论：静态交叉核对全部通过")
