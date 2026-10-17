#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""contract_source.py -- 把《接口契约》markdown 解析成可断言的常量表。

为什么要解析 md 而不是把常量抄进测试
    抄一遍 = 第二份事实源，契约改了测试不知道，验收就是假的。
    这里所有阈值/字段/错误码/样例帧都从 docs/技术/接口契约.md 现场读取：
    契约一改，测试要么跟着变（正常），要么直接报「表格结构变了」（提醒验收方复核）。

解析约定（与契约的 markdown 版式强相关，改版式要同步改这里）
    - 章节：`## 6. REST API` / `### 6.9 GET ...` / `#### 2.4.1 `gas`（...）`
    - 表格：连续的 `|` 行，第二行必须是 `|---|` 分隔行
    - 取值：Table.cell 返回**原样单元格**（保留反引号/加粗），Table.plain 去装饰
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
CONTRACT_PATH = os.path.join(REPO, "docs", "技术", "接口契约.md")
WIRING_PATH = os.path.join(REPO, "docs", "技术", "引脚与接线表.md")

HEADING_RE = re.compile(r"^(#{2,5})\s+(.+?)\s*$")
SECTION_NUM_RE = re.compile(r"^(\d+(?:\.\d+)*)\.?\s")


class ContractError(RuntimeError):
    """契约版式/内容不符合预期——测试应当直接失败，而不是猜。"""


def plain(cell: str) -> str:
    s = cell.replace("<br>", " ").replace("**", "").replace("`", "")
    return re.sub(r"\s+", " ", s).strip()


def tokens(cell: str) -> List[str]:
    return re.findall(r"`([^`]+)`", cell)


def num(text: str):
    """从单元格里取第一个数字；−/– 归一为 ASCII 减号；无数字返回 None。"""
    s = plain(text).replace("\u2212", "-").replace("\u2013", "-").replace("\u2014", "-")
    m = re.search(r"-?\d+(?:\.\d+)?", s)
    if not m:
        return None
    v = m.group(0)
    return float(v) if "." in v else int(v)


def fenced_blocks(text: str, lang: str = "") -> List[str]:
    out, cur, inside, tag = [], [], False, ""
    for line in text.splitlines():
        if line.strip().startswith("```"):
            if inside:
                if not lang or tag == lang:
                    out.append("\n".join(cur))
                cur, inside, tag = [], False, ""
            else:
                inside, tag = True, line.strip()[3:].strip().lower()
            continue
        if inside:
            cur.append(line)
    return out


def split_row(line: str) -> List[str]:
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|"):
        s = s[:-1]
    parts, buf, in_tick = [], [], False
    for ch in s:
        if ch == "`":
            in_tick = not in_tick
            buf.append(ch)
        elif ch == "|" and not in_tick:
            parts.append("".join(buf).strip())
            buf = []
        else:
            buf.append(ch)
    parts.append("".join(buf).strip())
    return parts


def parse_tables(text: str, section: str = "") -> List["Table"]:
    tables, block = [], []
    for line in list(text.splitlines()) + [""]:
        if line.strip().startswith("|"):
            block.append(line)
            continue
        if block:
            if len(block) >= 2 and re.match(r"^\|[\s:\-|]+\|?\s*$", block[1].strip()):
                header = [plain(c) for c in split_row(block[0])]
                rows = [split_row(l) for l in block[2:]]
                rows = [r for r in rows if any(c.strip() for c in r)]
                tables.append(Table(header, rows, section))
            block = []
    return tables


@dataclass
class Table:
    header: List[str]
    rows: List[List[str]]
    section: str = ""

    def col(self, name: str) -> int:
        for i, h in enumerate(self.header):
            if name in h:
                return i
        raise ContractError("§%s 表头 %r 里找不到列 %r" % (self.section, self.header, name))

    def cell(self, row: List[str], name: str, default: str = "") -> str:
        i = self.col(name)
        return row[i] if i < len(row) else default

    def p(self, row: List[str], name: str, default: str = "") -> str:
        return plain(self.cell(row, name, default))

    def n(self, row: List[str], name: str):
        return num(self.cell(row, name))

    def dicts(self) -> List[Dict[str, str]]:
        return [dict(zip(self.header, r)) for r in self.rows]

    def find(self, col: str, needle: str) -> Optional[List[str]]:
        for r in self.rows:
            if needle in self.cell(r, col):
                return r
        return None


def iter_headings(text: str) -> List[Tuple[str, int, int, str]]:
    """遍历标题，返回 (章节号, 行号(1-based), 层级, 标题原文)。供审计用例核对交叉引用。"""
    out = []
    for i, line in enumerate(text.splitlines()):
        m = HEADING_RE.match(line)
        if not m:
            continue
        level, title = len(m.group(1)), m.group(2)
        mm = SECTION_NUM_RE.match(title)
        out.append((mm.group(1) if mm else "", i + 1, level, title))
    return out


class Contract:
    def __init__(self, path: str = CONTRACT_PATH):
        self.path = path
        with open(path, "r", encoding="utf-8") as f:
            self.text = f.read()
        self.lines = self.text.splitlines()
        self._headings = self._scan_headings()
        self._cache: Dict[str, object] = {}

    # ---------- 结构 ----------
    def _scan_headings(self) -> List[Tuple[int, int, str, str]]:
        out = []
        for i, line in enumerate(self.lines):
            m = HEADING_RE.match(line)
            if not m:
                continue
            level, title = len(m.group(1)), m.group(2)
            mm = SECTION_NUM_RE.match(title)
            out.append((level, i, mm.group(1) if mm else "", title))
        return out

    def section_text(self, num: str) -> str:
        start = None
        for level, i, n, _t in self._headings:
            if n == num:
                start = (level, i)
                break
        if start is None:
            raise ContractError("契约里找不到章节 §%s" % num)
        level, i0 = start
        i1 = len(self.lines)
        for lv, i, _n, _t in self._headings:
            if i > i0 and lv <= level:
                i1 = i
                break
        return "\n".join(self.lines[i0:i1])

    def heading_line(self, num: str) -> int:
        for _l, i, n, _t in self._headings:
            if n == num:
                return i + 1
        raise ContractError("契约里找不到章节 §%s" % num)

    def sections_matching(self, prefix: str) -> List[Tuple[str, str]]:
        return [(n, t) for _l, _i, n, t in self._headings if n.startswith(prefix)]

    def tables(self, num: str) -> List[Table]:
        key = "tables:" + num
        if key not in self._cache:
            self._cache[key] = parse_tables(self.section_text(num), num)
        return self._cache[key]  # type: ignore[return-value]

    def table(self, num: str, nth: int = 0, must_have: Iterable[str] = ()) -> Table:
        want = list(must_have)
        seen = 0
        for t in self.tables(num):
            if all(any(w in h for h in t.header) for w in want):
                if seen == nth:
                    return t
                seen += 1
        raise ContractError("§%s 找不到第 %d 个包含列 %r 的表格" % (num, nth, want))

    def json_blocks(self, num: str) -> List[object]:
        key = "json:" + num
        if key not in self._cache:
            vals = []
            for body in fenced_blocks(self.section_text(num), "json"):
                try:
                    vals.append(json.loads(body))
                except json.JSONDecodeError:
                    continue
            self._cache[key] = vals
        return self._cache[key]  # type: ignore[return-value]

    # ---------- §2 SLE ----------
    def envelope_fields(self) -> Table:
        return self.table("2.2", must_have=["字段", "必填"])

    def types(self) -> List[str]:
        t = self.table("2.3", must_have=["type"])
        return [t.p(r, "type") for r in t.rows]

    def payload_section(self, type_name: str) -> str:
        for _lv, _i, n, title in self._headings:
            if n.startswith("2.4.") and ("`%s`" % type_name) in title:
                return n
        raise ContractError("找不到 type=%s 的 payload 段（§2.4.x 标题需含 `%s`）" % (type_name, type_name))

    def payload_fields(self, type_name: str) -> Table:
        return self.table(self.payload_section(type_name), must_have=["字段", "必填"])

    def downlink_fields(self) -> Table:
        return self.table("2.5", must_have=["字段"])

    def hub_behavior(self) -> str:
        return self.section_text("2.6")

    # ---------- §3 UART ----------
    def uart_types(self) -> Dict[int, Dict[str, str]]:
        t = self.table("3.4", must_have=["TYPE", "方向"])
        out = {}
        for r in t.rows:
            m = re.search(r"0x([0-9A-Fa-f]{2})", t.cell(r, "TYPE"))
            if not m:
                continue
            out[int(m.group(1), 16)] = {
                "name": t.p(r, "名称"),
                "dir": t.p(r, "方向"),
                "purpose": t.p(r, "用途"),
                "payload": t.cell(r, "PAYLOAD"),
            }
        return out

    def uart_cmd_enum(self, type_hex: int = 0x11) -> List[str]:
        """§3.4 的 cmd 枚举写在 TYPE 表下方的字段子表里（说明列用 `a / b / c` 列出）。

        只取「多选项」的那张表：0x22 的 cmd 固定为 pair_bind，不是枚举。
        """
        for t in self.tables("3.4"):
            for r in t.rows:
                if t.p(r, t.header[0]) == "cmd":
                    parts = [p.strip() for p in t.p(r, "说明").split("/") if p.strip()]
                    if len(parts) > 1:
                        return parts
        raise ContractError("§3.4 找不到 TYPE=0x%02X 的 cmd 枚举子表" % type_hex)

    def uart_subtable(self, first_col_value: str, col: str = "cmd"):
        """按子表首行值找 §3.4 的字段子表（例如 cmd=sle_scan / pair_bind）。"""
        for t in self.tables("3.4"):
            for r in t.rows:
                if t.p(r, t.header[0]) == col and t.p(r, "说明").strip() == first_col_value:
                    return t
        raise ContractError("§3.4 找不到 %s=%s 的子表" % (col, first_col_value))

    def uart_samples(self) -> List[Dict[str, str]]:
        t = self.table("3.7", must_have=["场景", "帧"])
        has_note = any("备注" in h for h in t.header)
        return [{"scene": t.p(r, "场景"), "hex": t.cell(r, "帧"),
                 "note": t.p(r, "备注") if has_note else ""} for r in t.rows]

    def framing_text(self) -> str:
        return self.section_text("3.6")

    # ---------- §4 IoTDA ----------
    def auth_params(self) -> Dict[str, str]:
        t = self.table("4.1", must_have=["项", "值"])
        return {t.p(r, "项"): t.cell(r, "值") for r in t.rows}

    def iotda_topics(self) -> List[Dict[str, str]]:
        t = self.table("4.2", must_have=["topic"])
        return [{"topic": t.cell(r, "topic"), "方向": t.p(r, "方向"),
                 "stage": t.p(r, "本契约") if any("本契约" in h for h in t.header) else "",
                 "purpose": t.p(r, "用途")} for r in t.rows]

    def iotda_properties(self) -> List[Dict[str, str]]:
        t = self.table("4.3", must_have=["属性", "物模型类型"])
        out = []
        for r in t.rows:
            out.append({
                "name": t.p(r, "属性"),
                "type": t.p(r, "物模型类型"),
                "range": t.p(r, "取值") if any("取值" in h for h in t.header) else "",
                "required": t.p(r, "上报") if any("上报" in h for h in t.header) else "",
                "note": t.p(r, "数据来源") if any("数据来源" in h for h in t.header) else "",
            })
        return out

    def iotda_events(self) -> List[Dict[str, str]]:
        t = self.table("4.4", must_have=["event_type", "paras"])
        return [{"event_type": t.p(r, "event_type"), "paras": t.cell(r, "paras"),
                 "level": t.p(r, "级别") if any("级别" in h for h in t.header) else "",
                 "trigger": t.p(r, "触发")} for r in t.rows]

    def iotda_commands(self) -> List[Dict[str, str]]:
        t = self.table("4.5", must_have=["命令"])
        return [dict(zip(t.header, [plain(c) for c in r])) for r in t.rows]

    def iotda_examples(self) -> List[object]:
        return self.json_blocks("4.6")

    # ---------- §5 webhook ----------
    def webhook_fields(self) -> Table:
        return self.table("5.2", must_have=["字段"])

    # ---------- §6 REST ----------
    def endpoints(self) -> List[Dict[str, str]]:
        t = self.table("6.2", must_have=["方法", "路径"])
        out = []
        for r in t.rows:
            out.append({
                "no": t.p(r, "#") if any(h.strip() == "#" for h in t.header) else "",
                "method": t.p(r, "方法"),
                "path": t.cell(r, "路径").replace("`", "").strip(),
                "purpose": t.p(r, "用途"),
                "stage": t.p(r, "优先级"),
            })
        return out

    def settings_defaults(self) -> Dict[str, object]:
        for val in self.json_blocks("6.9"):
            if isinstance(val, dict) and isinstance(val.get("items"), dict):
                return val["items"]
        raise ContractError("§6.9 找不到含 items 的 JSON 示例块")

    def alert_summaries(self) -> Dict[Tuple[str, int], str]:
        t = self.table("6.13", must_have=["type", "summary"])
        out = {}
        for r in t.rows:
            lv = t.n(r, "level")
            if lv is None:
                continue
            out[(t.p(r, "type"), int(lv))] = t.p(r, "summary")
        return out

    # ---------- §7 数据表 ----------
    def tables_in_db(self) -> List[str]:
        out = []
        for _lv, _i, n, title in self._headings:
            if n.startswith("7.") and n.count(".") == 1:
                m = re.search(r"`([a-z_]+)`", title)
                if m:
                    out.append(m.group(1))
        return out

    def db_columns(self, table_name: str) -> List[str]:
        for _lv, _i, n, title in self._headings:
            if n.startswith("7.") and ("`%s`" % table_name) in title:
                for col in ("列", "字段"):
                    try:
                        t = self.table(n, must_have=[col])
                    except ContractError:
                        continue
                    return [t.p(r, col) for r in t.rows]
                raise ContractError("§%s 找不到列定义表" % n)
        raise ContractError("§7 找不到表 %s" % table_name)

    # ---------- §8 阈值 ----------
    def gas_params(self) -> Dict[str, object]:
        t = self.table("8.2", must_have=["参数", "默认值"])
        out: Dict[str, object] = {}
        for r in t.rows:
            name = t.p(r, "参数")
            if not (name.startswith("gas_") or "采样" in name):
                continue
            out[name] = t.n(r, "默认值")
        return out

    def fall_params(self) -> Dict[str, object]:
        t = self.table("8.4", must_have=["参数", "默认值"])
        out: Dict[str, object] = {}
        for r in t.rows:
            name = t.p(r, "参数")
            raw = plain(t.cell(r, "默认值"))
            if name.startswith("fall_suppress"):
                out["fall_suppress_s"] = t.n(r, "默认值")
            if name.startswith("进入条件"):
                out["enter_delay_s"] = 0
            if name.startswith("退出条件"):
                m = re.search(r"K\s*=\s*(\d+)", raw)
                if m:
                    out["k_fall_zero"] = int(m.group(1))
                m = re.search(r"T_hold\s*=\s*(\d+)", raw)
                if m:
                    out["t_hold_s"] = int(m.group(1))
                if "≥" in raw or ">=" in raw:
                    out["exit_needs_last_trigger_age"] = True
        if "k_fall_zero" not in out:
            raise ContractError("§8.4 退出条件里解析不到 K 值：%r" % t.header)
        return out

    def motion_params(self) -> Dict[str, object]:
        t = self.table("8.5", must_have=["参数", "默认值"])
        return {t.p(r, "参数"): t.n(r, "默认值") for r in t.rows}

    def offline_params(self) -> Dict[str, object]:
        t = self.table("8.7", must_have=["对象"])
        out: Dict[str, object] = {}
        for r in t.rows:
            obj = t.p(r, "对象")
            blob = " ".join(plain(c) for c in r)
            m = re.search(r"=\s*(\d+)\s*s", blob)
            if obj.startswith("节点") and m:
                out["node_offline_s"] = int(m.group(1))
            if obj.startswith("网关") and m:
                out["gateway_offline_s"] = int(m.group(1))
        return out

    def repush_params(self) -> Dict[str, object]:
        text = self.section_text("8.8")
        out: Dict[str, object] = {}
        m = re.search(r"\[(\s*\d+\s*,\s*\d+\s*,\s*\d+\s*)\]", text)
        if m:
            out["alert_repush_s"] = [int(x) for x in m.group(1).split(",")]
        m = re.search(r"alert_suppress_s`?\s*[（(](\d+)\s*s", text)
        if m:
            out["alert_suppress_s"] = int(m.group(1))
        m = re.search(r"fall[：:\s]*(\d+)\s*s\s*窗口", text)
        if m:
            out["fall_suppress_s"] = int(m.group(1))
        return out

    # ---------- §9 错误码 ----------
    def biz_errors(self) -> Dict[int, Dict[str, str]]:
        t = self.table("9.2", must_have=["code", "message"])
        out = {}
        for r in t.rows:
            code = t.n(r, "code")
            if code is None:
                continue
            out[int(code)] = {"message": t.p(r, "message"), "http": t.p(r, "HTTP"),
                              "note": t.p(r, "说明")}
        return out

    def http_codes(self) -> Dict[int, str]:
        t = self.table("9.1", must_have=["含义"])
        out = {}
        for r in t.rows:
            code = t.n(r, t.header[0])
            if code is not None:
                out[int(code)] = t.p(r, "含义")
        return out

    def device_errors(self) -> Dict[int, str]:
        t = self.table("9.3", must_have=["含义"])
        out = {}
        for r in t.rows:
            v = t.n(r, t.header[0])
            if v is not None:
                out[int(v)] = t.p(r, "含义")
        return out

    # ---------- §0 通用 / §12 配网 ----------
    def sn_table(self) -> List[Dict[str, str]]:
        t = self.table("0.7", must_have=["sn", "角色"])
        return [{"sn": t.p(r, "sn"), "role": t.p(r, "角色"), "stage": t.p(r, "优先级"),
                 "status": t.p(r, "状态"), "sensors": t.p(r, "搭载传感器")} for r in t.rows]

    def ports(self) -> Dict[str, str]:
        t = self.table("0.8", must_have=["项", "值"])
        return {t.p(r, "项"): t.cell(r, "值") for r in t.rows}

    def provision_endpoints(self) -> List[Dict[str, str]]:
        t = self.table("12.4", must_have=["方法", "路径"])
        return [{"no": t.p(r, "#"), "method": t.p(r, "方法"), "path": t.p(r, "路径"),
                 "purpose": t.p(r, "用途"), "blocking": t.p(r, "阻塞?")} for r in t.rows]

    def provision_params(self) -> Dict[str, str]:
        t = self.table("12.3", must_have=["项", "值"])
        return {t.p(r, "项"): t.cell(r, "值") for r in t.rows}

    def uart_pair_samples(self) -> List[Dict[str, str]]:
        """§3.9 配网/配对帧示例（v1.1）。"""
        t = self.table("3.9", must_have=["帧", "字节"])
        return [{"frame": t.cell(r, "帧"), "seq": t.n(r, "SEQ"), "len": t.n(r, "LEN"),
                 "hex": t.cell(r, "字节")} for r in t.rows]

    # ---------- 自检 ----------
    def duplicate_section_numbers(self) -> List[str]:
        """同一编号出现多次（例如 §12 既是配网 API 又是变更记录）——版式风险。"""
        seen: Dict[str, int] = {}
        for _l, _i, n, _t in self._headings:
            if n:
                seen[n] = seen.get(n, 0) + 1
        return sorted(k for k, v in seen.items() if v > 1)

    def all_section_numbers(self) -> List[str]:
        return [n for _l, _i, n, _t in self._headings if n]

    def dump_snapshot(self) -> Dict[str, object]:
        return {
            "path": os.path.relpath(self.path, REPO),
            "sections": len(self.all_section_numbers()),
            "types": self.types(),
            "envelope_fields": self.envelope_fields().header,
            "uart_types": {("0x%02X" % k): v["name"] for k, v in self.uart_types().items()},
            "endpoints": len(self.endpoints()),
            "biz_codes": sorted(self.biz_errors()),
            "device_codes": sorted(self.device_errors()),
            "db_tables": self.tables_in_db(),
            "settings": self.settings_defaults(),
            "gas": self.gas_params(),
            "fall": self.fall_params(),
            "motion": self.motion_params(),
            "offline": self.offline_params(),
            "repush": self.repush_params(),
        }


def load(path: str = CONTRACT_PATH) -> Contract:
    return Contract(path)


if __name__ == "__main__":
    print(json.dumps(load().dump_snapshot(), ensure_ascii=False, indent=2))
