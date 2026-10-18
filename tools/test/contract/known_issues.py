#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""known_issues.py -- 已登记问题的基线清单（复核报告里已经写明的问题）。

为什么要有它
    已登记的契约缺陷（例如 §6.9 少一个键）会让相关用例长期红着。红着不代表
    实现方做错了，只代表「契约还没修」。硬失败会让验收信号失效（狼来了）。
    所以：已登记问题 → SKIP 并在报告里带编号；**新问题** → FAIL。

三档口径（t64 收口，2026-10-18）——「已登记」必须是有牙的
    RESOLVED   契约修订已消解（依据：契约「变更记录」v1.2/v1.3 逐条点名）。
               用例必须改成硬断言；若还有用例引用它，known() 直接 FAIL，
               否则 SKIP 会永久掩盖这条链路的回归。
    PENDING    确属未收口（实现面未做 / 契约面未修）。SKIP，但必须带
               「拒绝原因 + 负责人卡号」，报告里能直接看到谁收口。
    HISTORICAL 早期复核登记、当前无任何用例引用。仅留档；引用前先核实现状。

    s06 的 case_no_silent_skip 守着这条线：任何 known() 引用都必须在 PENDING 里，
    且 PENDING 里不许有没人引用的死条目——否则验收报告里的 SKIP 会变成永久盲区。
"""

from __future__ import annotations

from harness import CaseFail, SkipCase

# --- 已被契约修订消解：再被引用即 FAIL（防「静默 SKIP」掩盖回归）-------------------------
# 依据：docs/技术/接口契约.md「变更记录」v1.2（2026-10-17）/ v1.3 逐条点名，
#       对应复核报告 复核-契约v1.1-2026-10-15 / 复核-t36-契约v1.2-2026-10-18。
RESOLVED = {
    "B2": "§3.6 重发失败已改 result=12，1502 只留 webhook（变更记录 v1.2 ①）",
    "B3": "§4.1.1 取时与降级 + §6.21 GET /api/time（变更记录 v1.2 ②）",
    "M1": "§3.7 六条样例帧全部补成完整字节（变更记录 v1.2 ④）",
    "M3": "交叉引用 6 类 7 处已修正（变更记录 v1.2 ⑤）",
    "M4": "gas_l1_to_l2_s 明示为 §8.2 算法参数、不进 §6.9（变更记录 v1.2 ⑥）",
    "M5": "§11 #2 adc_mv 改「P0 单路 [ch2]」并修行序（变更记录 v1.2 ⑦）",
    "M6": "§4.6 示例改为 P0 真实上报，省略 presence/lpg/co（变更记录 v1.2 ⑧）",
    "M7": "全文毫秒样例统一到 2026-10-17（变更记录 v1.2 ⑨，CRC 锁定帧与鉴权夹具除外）",
    "M8": "§3.6 残余阈值 2058→2068（变更记录 v1.2 ③）",
    "M11": "§2.4.1/§8.2 补 RH 下限功率边界（变更记录 v1.2 ⑩）",
    "M13": "cmd 枚举统一为 poll/set_threshold/demo/reboot/time_sync（变更记录 v1.2 ⑪）",
    "M14": "§12.6 口令 8–63 且不支持开放网络（变更记录 v1.2 ⑫）",
    "V11-1": "§6.11 响应示例补 ts（变更记录 v1.2）",
    "V11-2": "§12 编号唯一（本地配网 API 12.1–12.11，无重号）",
    "V11-3": "标题版本与「变更记录」一致（现为 v1.3）",
}

# --- 确属未收口：SKIP 但必须带原因与负责人卡号 ---------------------------------------
PENDING = {
    "COV-1": ("§6.21 `GET /api/time`（免鉴权取时，PM D2）未纳入 mock 基线，"
              "s05 覆盖用例对不上（mock_server.py 未实现该路由）",
              "t64 —— 需 PM 扩 inScope 至 tools/test/mock_server.py 才能转硬断言"),
    "M3-0.2": ("§0.2「报表的日/小时按 UTC+8 日历切分（§7.6）」引用的是 sensor_readings；"
               "同口径的 §5.3 已指向 §7.9/§7.10，此处权威位置应同为 §7.9/§7.10",
               "PM 契约修订（t64 已报，待开卡）"),
}

# --- 早期复核登记、当前无任何用例引用：仅留档，引用前先核实现状 -----------------------
HISTORICAL = {
    "B1": "P0 需 4 块 WS63（H1+M1+M2+M4），手上 2 块 + 第 3 块待购（硬件面，非契约面）",
    "M2": "§8.4「K 次 fall=0」的采样来源/周期（实现面口径，当时未定）",
    "M9": "接线表 §1 雷达供电「LDO 模块」热耗（硬件面，待上板实测）",
    "M10": "接线表 §4.2 分压标定条件写法（硬件面，CAL 标定记录跟进）",
    "M12": "t1 汇报口径「10 topic / 5 事件」与正文不符（已随文档修订消失）",
}


def known(issue_id: str, detail: str = "") -> None:
    """抛 SkipCase，把已知问题编号带进报告。

    RESOLVED / HISTORICAL 一律不放过：前者说明用例该改硬断言了，后者说明这条编号
    已经没人维护、不能再用来换一个 SKIP。
    """
    if issue_id in RESOLVED:
        raise CaseFail(
            "基线过期：%s 已被契约修订消解（%s）——请把用例改成硬断言，不要再 SKIP"
            % (issue_id, RESOLVED[issue_id])
        )
    if issue_id in PENDING:
        reason, owner = PENDING[issue_id]
        msg = "[%s] 待收口：%s；负责人：%s" % (issue_id, reason, owner)
        if detail:
            msg += "；本次观测：" + detail
        raise SkipCase(msg)
    if issue_id in HISTORICAL:
        raise CaseFail(
            "HISTORICAL 编号 %s 不能再换 SKIP：%s（引用前先核实现状，再决定改硬断言或转 PENDING）"
            % (issue_id, HISTORICAL[issue_id])
        )
    raise KeyError("未登记的 known issue id=%s（先加进 PENDING，再在用例里引用）" % issue_id)


def is_known(issue_id: str) -> bool:
    return issue_id in PENDING


def pending(issue_id: str, detail: str = "") -> None:
    """显式登记一处「待收口」并 SKIP（编号必须在 PENDING 里，自带原因与负责人卡号）。

    与 known() 的区别只是语义：known() 读起来像「老问题照旧」，pending() 读起来像
    「这里确实还没做，登记在案」。两者对 RESOLVED/HISTORICAL 都不放行。
    """
    known(issue_id, detail)


def pending_ids() -> list:
    return sorted(PENDING)


def resolved_ids() -> list:
    return sorted(RESOLVED)
