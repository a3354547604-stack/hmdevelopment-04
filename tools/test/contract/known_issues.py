#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""known_issues.py -- 已登记问题的基线清单（复核报告里已经写明的问题）。

为什么要有它
    已登记的契约缺陷（例如 §6.9 少一个键）会让相关用例长期红着。红着不代表
    实现方做错了，只代表「契约还没修」。硬失败会让验收信号失效（狼来了）。
    所以：已登记问题 → SKIP 并在报告里带编号；**新问题** → FAIL。
    等 PM 修好契约，对应的 SKIP 会自然变 PASS；如果问题描述与现状不符，
    用例会以「基线过期」FAIL，提醒复核方更新本文件。
"""

from __future__ import annotations

from harness import SkipCase

KNOWN = {
    "B1": "P0 需 4 块 WS63（H1+M1+M2+M4），§0.1/接线表 §0.1 只列 2 块在手上 + 第 3 块待购",
    "B2": "§3.6 用 result=1502 表示「下发失败」，与 §9.2（1502=webhook 校验失败）/§9.3（0–7）冲突",
    "B3": "§4.1 未定义网关时间来源与容差（RK2206 无 RTC，GW_TIME 被排 P1）",
    "M1": "§3.7 前三条/第五条示例帧 payload 用省略号，无法逐字节复现",
    "M2": "§8.4「K 次 fall=0」未定义采样来源/周期（模块何时给 fall=0）",
    "M3": "契约 §2.5/§3.4/§6.5/§3.8/§8.4 的交叉引用编号错（settings/离线/处置）",
    "M4": "§6.9 settings items 缺 gas_l1_to_l2_s（§8.2 声明可配=是）",
    "M5": "§11 #2 声称 adc_mv 顺序 [ch2,ch3,ch4,ch5] 与 §2.4.1 冲突（P0 长度 1）",
    "M6": "§4.6 属性示例含 lpg/co，与 §4.3 标注「P0 未装→缺省」口径冲突",
    "M8": "§3.6 残余阈值写 2058，2×(10+1024)=2068 对不上",
    "M9": "接线表 §1 雷达供电写「LDO 模块」，5→3.3 V@600 mA 热耗 1.02 W 会热关断",
    "M10": "接线表 §4.2 分压标定以「发现偏低才改 20k/20k」为条件，实际是系统性误差",
    "M11": "MQ-2 RH 最坏 26 Ω 时 961.5 mW 超 950 mW 额定（需选低阻或标定）",
    "M12": "t1 汇报口径「10 topic / 5 事件」与正文（8 topic / 3 事件）不符",
    "M13": "§2.5 cmd 枚举（poll/set/demo/reboot）与 §3.4/§3.6（set/poll/demo/reboot_node/time_sync）及 §3.7 样例（set_threshold）三处不一致",
    "V11-1": "§6.11 响应示例缺 ts 字段（违反 §6.1 统一信封）",
    "V11-2": "§12 编号重复：既是「本地配网 API」又是「变更记录」",
    "V11-3": "契约标题仍写 v1 定版，正文却多处标「v1.1 新增」（版本口径）",
    "M14": "§12.6 password 同时写「必填 8–63 字节」与「开放网络传空串」，两条互斥（P0 只做 WPA2，按 8–63 实现）",
}


def known(issue_id: str, detail: str = "") -> None:
    """抛 SkipCase，把已知问题编号带进报告。"""
    if issue_id not in KNOWN:
        raise KeyError("未登记的 known issue id=%s（先加进 KNOWN，再在用例里引用）" % issue_id)
    msg = "[%s] %s" % (issue_id, KNOWN[issue_id])
    if detail:
        msg += "；本次观测：" + detail
    raise SkipCase(msg)


def is_known(issue_id: str) -> bool:
    return issue_id in KNOWN
