#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""s02_codec.py -- UART 帧编解码契约测试（§3）。

金丝雀来自契约自己：§3.7 的完整示例帧与 §3.9 的配网/配对帧（都是逐字节给出 + CRC）。
任何一方（汇聚固件 / 网关固件 / 参考实现）字节序或 CRC 口径不同，这里必红。
"""

from __future__ import annotations

import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")))
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "contract")))

import contract_source as cs          # noqa: E402
import reference_impl as ri           # noqa: E402
from harness import CaseFail, SkipCase, SuiteResult, require, require_eq, require_in, run_cases  # noqa: E402

C = cs.load()


def _hex_bytes(cell: str) -> bytes:
    cleaned = re.sub(r"[^0-9A-Fa-f]", "", cs.plain(cell))
    if len(cleaned) % 2:
        raise CaseFail("十六进制串长度为奇数：%r" % cell[:60])
    return bytes.fromhex(cleaned)


def _complete_samples():
    out = []
    for s in C.uart_samples():
        raw = cs.plain(s["hex"])
        if "…" in raw or "..." in raw:
            continue
        out.append(("§3.7 " + s["scene"][:40], raw))
    for s in C.uart_pair_samples():
        out.append(("§3.9 " + s["frame"], cs.plain(s["hex"])))
    return out


def case_crc_selfcheck():
    require_eq(ri.crc16_modbus(b"123456789"), 0x4B37, "§3.3 CRC 自检 crc16(\"123456789\")")
    text = C.section_text("3.3")
    require("4B37" in text.upper(), "§3.3 正文应写明自检值 4B37")


def case_golden_frames_byte_exact():
    samples = _complete_samples()
    require(len(samples) >= 6, "完整示例帧只有 %d 条（§3.7 两条 + §3.9 五条至少 6 条）" % len(samples))
    for label, raw in samples:
        data = _hex_bytes(raw)
        require(data[:2] == b"\xAA\x55", "%s 帧头不是 AA 55" % label)
        ver, type_hex, seq, length = struct.unpack("<BBHH", data[2:8])
        require_eq(ver, 0x01, "%s VER" % label)
        require(length <= ri.MAX_LEN, "%s LEN=%d 超过 1024" % (label, length))
        require_eq(len(data), 10 + length, "%s 整帧长度应为 10+LEN" % label)
        crc_rx = struct.unpack("<H", data[8 + length:10 + length])[0]
        require_eq(ri.crc16_modbus(data[2:8 + length]), crc_rx, "%s CRC" % label)
        frames, errors, rest = ri.decode_frames(data)
        require_eq(errors, [], "%s 解码不应报错" % label)
        require_eq(len(frames), 1, "%s 应解出 1 帧" % label)
        require_eq(frames[0].type_hex, type_hex, "%s TYPE" % label)
        require_eq(frames[0].seq, seq, "%s SEQ" % label)
        require_eq(ri.encode_frame(type_hex, seq, frames[0].payload), data, "%s 重新编码应字节一致" % label)


def case_pair_samples_match_declared_len():
    for s in C.uart_pair_samples():
        data = _hex_bytes(s["hex"])
        _ver, type_hex, seq, length = struct.unpack("<BBHH", data[2:8])
        require_eq(length, int(s["len"]), "§3.9 %s LEN 列" % s["frame"])
        require_eq(seq, int(s["seq"]), "§3.9 %s SEQ 列" % s["frame"])
        labels = re.findall(r"`0x([0-9A-Fa-f]{2})`", s["frame"])
        if labels:
            require_eq(type_hex, int(labels[0], 16), "§3.9 %s TYPE" % s["frame"])


def case_pair_frames_are_declared_types():
    declared = C.uart_types()
    for s in C.uart_pair_samples():
        data = _hex_bytes(s["hex"])
        type_hex = data[3]
        require_in(type_hex, declared, "§3.9 %s 的 TYPE 0x%02X 应在 §3.4 TYPE 表" % (s["frame"], type_hex))


def case_truncated_samples_are_known():
    """§3.7 六条样例帧必须逐字节完整，字段集与 §2.2/§2.4.x 对齐。

    v1.2 已补齐复核 M1（原先两条样例帧 payload 用「…」省略），本用例随之从「登记缺陷」
    改为硬断言：省略帧数必须为 0；逐帧 `len == 10 + LEN` 且 CRC 复算一致；携带节点 JSON
    的帧（0x01 NODE_DATA / 0x03 HUB_EVENT）其 envelope 键集与 §2.2 完全一致、内层 payload
    键集 ⊆ §2.4.x 声明字段且覆盖全部必填字段（P1 键允许缺省，如 §2.4.2 的 `presence`）。
    """
    samples = C.uart_samples()
    require_eq(len(samples), 6, "§3.7 样例帧条数（v1.2 的六条实测帧）")
    truncated = [s["scene"][:24] for s in samples
                 if "…" in cs.plain(s["hex"]) or "..." in cs.plain(s["hex"])]
    require_eq(truncated, [], "§3.7 仍有 payload 省略的帧（M1 回退：v1.2 要求逐字节完整）")

    envelope = [cs.plain(C.envelope_fields().cell(r, "字段")) for r in C.envelope_fields().rows]
    example = C.json_blocks("2.2")
    require_eq(len(example), 1, "§2.2 应有一条完整示例 JSON")

    node_json_frames = 0
    for s in samples:
        label = "§3.7 " + s["scene"][:20]
        data = _hex_bytes(s["hex"])
        require(data[:2] == b"\xAA\x55", "%s 帧头不是 AA 55" % label)
        _ver, type_hex, _seq, length = struct.unpack("<BBHH", data[2:8])
        require_eq(len(data), 10 + length, "%s 整帧长度应为 10+LEN" % label)
        crc_rx = struct.unpack("<H", data[8 + length:10 + length])[0]
        require_eq(ri.crc16_modbus(data[2:8 + length]), crc_rx, "%s CRC 复算" % label)
        frames, errors, _rest = ri.decode_frames(data)
        require_eq(errors, [], "%s 解码不应报错" % label)
        if type_hex not in (0x01, 0x03):       # 其余帧的 payload 不是节点 JSON
            continue
        node_json_frames += 1
        obj = json.loads(frames[0].payload.decode("utf-8"))
        require_eq(sorted(obj.keys()), sorted(envelope), "%s envelope 键集应与 §2.2 一致" % label)
        if type_hex == 0x01:
            require_eq(obj, example[0], "%s 应逐字段等于 §2.2 示例" % label)
        table = C.payload_fields(obj["type"])
        section = C.payload_section(obj["type"])
        declared = [cs.plain(table.cell(r, "字段")) for r in table.rows]
        required = [cs.plain(table.cell(r, "字段")) for r in table.rows
                    if table.cell(r, "必填").startswith("是")]
        inner = sorted(obj["payload"].keys())
        undeclared = sorted(set(inner) - set(declared))
        require_eq(undeclared, [], "%s payload 有 §%s 未声明的键" % (label, section))
        require_eq(sorted(set(required) - set(inner)), [],
                   "%s payload 缺 §%s 的必填字段" % (label, section))
    require_eq(node_json_frames, 2, "§3.7 应有 2 条携带节点 JSON 的帧（0x01/0x03）")


def case_coalesced_frames():
    a = ri.encode_frame(0x11, 4, b'{"cmd":"poll","id":7}')
    b = ri.encode_frame(0x10, 6, b'{"ack_seq":1,"result":0}')
    frames, errors, rest = ri.decode_frames(a + b)
    require_eq(errors, [], "粘包不应报错")
    require_eq([f.seq for f in frames], [4, 6], "粘包应解出两帧且顺序不变")
    require_eq(rest, b"", "粘包应无残余")


def case_half_frame_kept_as_residual():
    frame = ri.encode_frame(0x01, 1, b'{"sn":"M1"}')
    frames, errors, rest = ri.decode_frames(frame[:-3])
    require_eq(frames, [], "半包不应解出帧")
    require_eq(rest, frame[:-3], "半包应原样保留为残余")


def case_garbage_prefix_is_skipped():
    frame = ri.encode_frame(0x11, 9, b'{"cmd":"poll","id":1}')
    junk = b"\x00\xAA\x11\x55\xFF" + frame
    frames, errors, rest = ri.decode_frames(junk)
    require_eq(len(frames), 1, "假帧头前缀后应仍能解出真帧")
    require_eq(frames[0].seq, 9, "解出的帧 SEQ")
    require_eq(rest, b"", "残余")


def case_len_overflow_reported():
    bad = b"\xAA\x55\x01\x01\x01\x00" + struct.pack("<H", 2000) + b"\x00" * 10
    frames, errors, _rest = ri.decode_frames(bad)
    require_eq(frames, [], "LEN>1024 不应解出帧")
    require(any(e.kind == "len" and e.reason == 5 for e in errors),
            "LEN>1024 应报 reason=5（§9.3），实际 %r" % errors)


def case_crc_error_reported():
    frame = bytearray(ri.encode_frame(0x01, 2, b'{"sn":"M2","fall":true}'))
    frame[10] ^= 0xFF
    frames, errors, _rest = ri.decode_frames(bytes(frame))
    require_eq(frames, [], "CRC 错不应解出帧")
    require(any(e.kind == "crc" and e.reason == 4 for e in errors),
            "CRC 错应报 reason=4（§9.3），实际 %r" % errors)


def case_crc_covers_ver_to_payload():
    """CRC 必须覆盖 VER..PAYLOAD：改 payload 任一字节，帧尾 CRC 必须变。"""
    a = ri.encode_frame(0x01, 3, b'{"sn":"M1","mv":1000}')
    b = ri.encode_frame(0x01, 3, b'{"sn":"M1","mv":1001}')
    crc_a = struct.unpack("<H", a[-2:])[0]
    crc_b = struct.unpack("<H", b[-2:])[0]
    require(crc_a != crc_b, "payload 变化后 CRC 未变（说明 CRC 没覆盖 payload）")
    # 反向：改 VER 字段也应改变 CRC（覆盖范围含 VER）
    c = bytearray(a)
    c[2] = 0x02
    require(ri.crc16_modbus(bytes(c[2:8 + 13])) != crc_a, "VER 变化后 CRC 未变（说明 CRC 没覆盖 VER）")

def case_endianness_small():
    """SEQ/LEN/CRC 都是小端：手工拼一个已知值的帧做交叉验证。"""
    payload = b"{}"
    frame = ri.encode_frame(0x12, 0x1234, payload)
    require_eq(frame[4:6], b"\x34\x12", "SEQ 小端")
    require_eq(frame[6:8], b"\x02\x00", "LEN 小端")
    expect_crc = ri.crc16_modbus(frame[2:8 + len(payload)])
    require_eq(frame[8 + len(payload):], struct.pack("<H", expect_crc), "CRC 小端")


def case_payload_must_be_object():
    """§3.5：PAYLOAD 顶层必须是 JSON 对象，数组/裸值不允许。"""
    require(ri.payload_is_object(b'{"cmd":"poll","id":1}'), "对象 payload 应判为合法")
    for raw in (b'[1,2]', b'"x"', b"5", b"not-json"):
        require(not ri.payload_is_object(raw), "§3.5 顶层 %r 不应判为合法 payload" % raw)


def case_strict_framing_limits_match_contract():
    text = C.section_text("3.6")
    require("1024" in C.section_text("3.2"), "§3.2 应写明 LEN 上限 1024")
    require("2058" in text or "2068" in text, "§3.6 应写明残余清空阈值")
    if "2058" in text:
        raise SkipCase("§3.6 残余阈值写 2058，而 2×(10+1024)=2068 对不上（复核报告 M8）")


CASES = [
    ("§3.3 CRC 自检向量 0x4B37", case_crc_selfcheck),
    ("§3.7/§3.9 完整示例帧逐字节复现", case_golden_frames_byte_exact),
    ("§3.9 SEQ/LEN 列与字节一致", case_pair_samples_match_declared_len),
    ("§3.9 帧 TYPE 都在 §3.4 中", case_pair_frames_are_declared_types),
    ("§3.7 六条样例帧完整且字段集对齐（M1 已修）", case_truncated_samples_are_known),
    ("§3.6 粘包按 LEN 切分", case_coalesced_frames),
    ("§3.6 半包保留残余", case_half_frame_kept_as_residual),
    ("§3.6 假帧头滑动重找", case_garbage_prefix_is_skipped),
    ("§3.6 LEN 越界 reason=5", case_len_overflow_reported),
    ("§3.6 CRC 错 reason=4", case_crc_error_reported),
    ("§3.2 CRC 覆盖 VER..PAYLOAD", case_crc_covers_ver_to_payload),
    ("§3.2 SEQ/LEN/CRC 小端", case_endianness_small),
    ("§3.5 PAYLOAD 必须是 JSON 对象", case_payload_must_be_object),
    ("§3.6 残余阈值口径", case_strict_framing_limits_match_contract),
]


def build(verbose: bool = False) -> SuiteResult:
    suite = SuiteResult("s02_codec", "UART 帧编解码与 CRC（§3）")
    return run_cases(suite, CASES, verbose)


if __name__ == "__main__":
    s = build(True)
    sys.exit(0 if s.status != "failed" else 1)
