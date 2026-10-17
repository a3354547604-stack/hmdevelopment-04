#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""reference_impl.py -- 契约的「可执行参考实现」（纯标准库，不依赖任何固件/服务器）。

用途（这是验收基线的一部分，不是产品代码）：
    1. 把契约里的帧编解码、CRC、燃气分级、跌倒迟滞、无活动/外出、离线判定、
       重推节奏写成可运行的参考，实现方（网关/汇聚/服务器）在自测时可直接 import
       比对，省去「各人一套口径」。
    2. 验收时用同一组用例跑实现方提供的适配器（见 tools/test/README 的 adapter 约定）。
    3. 参考实现本身也被 s02/s04 用例断言（防止参考实现自己跑偏）。

任何数值都不在这里硬编码——构造函数参数由 contract_source 从契约读出后传入。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

SOF = b"\xAA\x55"
VER = 0x01
MAX_LEN = 1024
HEADER_LEN = 8
CRC_LEN = 2


def crc16_modbus(data: bytes) -> int:
    """CRC-16/MODBUS：poly 0x8005 反射成 0xA001，init 0xFFFF，refin/refout，xorout 0。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc & 0xFFFF


def payload_is_object(payload: bytes) -> bool:
    """§3.5：PAYLOAD 必须是完整 JSON **对象**（禁止数组/裸值作顶层）。"""
    import json
    try:
        return isinstance(json.loads(payload.decode("utf-8")), dict)
    except (UnicodeDecodeError, ValueError):
        return False


def encode_frame(type_hex: int, seq: int, payload: bytes, ver: int = VER) -> bytes:
    """按 §3.2 组帧：AA 55 VER TYPE SEQ(LE) LEN(LE) PAYLOAD CRC16(LE)，CRC 覆盖 VER..PAYLOAD。"""
    if len(payload) > MAX_LEN:
        raise ValueError("payload %d > %d" % (len(payload), MAX_LEN))
    head = struct.pack("<BBHH", ver, type_hex, seq, len(payload))
    body = head + payload
    return SOF + body + struct.pack("<H", crc16_modbus(body))


@dataclass
class Frame:
    type_hex: int
    seq: int
    payload: bytes
    raw: bytes
    ver: int = VER

    def json(self):
        import json
        return json.loads(self.payload.decode("utf-8"))


@dataclass
class FrameError:
    kind: str          # crc / len / type / sof
    reason: int        # §9.3 设备侧错误码
    at: int
    bytes_skipped: int = 0


def decode_frames(buf: bytes) -> Tuple[List[Frame], List[FrameError], bytes]:
    """返回 (帧列表, 错误列表, 残余)。规则完全按 §3.6：
    - 粘包按 LEN 连续切；不足一个整帧就保留残余；
    - 找不到 SOF 时逐字节滑动丢弃；LEN>1024 丢弃首字节继续找；
    - CRC 错丢弃首字节继续找；残余 > 2058 清空。
    """
    frames: List[Frame] = []
    errors: List[FrameError] = []
    i = 0
    n = len(buf)
    while i + 1 < n:
        if buf[i:i + 2] != SOF:
            i += 1
            continue
        if i + HEADER_LEN > n:
            break
        ver, type_hex, seq, length = struct.unpack("<BBHH", buf[i + 2:i + HEADER_LEN])
        if length > MAX_LEN:
            errors.append(FrameError("len", 5, i, 1))
            i += 1
            continue
        total = HEADER_LEN + length + CRC_LEN
        if i + total > n:
            break  # 半包，等下一批
        body = buf[i + 2:i + HEADER_LEN + length]
        crc_rx = struct.unpack("<H", buf[i + HEADER_LEN + length:i + total])[0]
        if crc16_modbus(body) != crc_rx:
            errors.append(FrameError("crc", 4, i, 1))
            i += 1
            continue
        raw = buf[i:i + total]
        frames.append(Frame(type_hex, seq, buf[i + HEADER_LEN:i + HEADER_LEN + length], raw, ver))
        i += total
    rest = buf[i:] if i < n else b""
    # 契约 §3.6 写「残余 > 2058 清空」。2058 与「2×最大帧」对不上（2×(10+1024)=2068），
    # 这是已登记的口径冲突（复核报告 M8）：参考实现按公式取 2068，审计套件负责暴露差异。
    if len(rest) > 2 * (HEADER_LEN + MAX_LEN + CRC_LEN):
        rest = b""
    return frames, errors, rest


@dataclass
class Event:
    kind: str                  # alert / recover / level
    level: int = 1
    detail: str = ""
    at: float = 0.0


class GasDetector:
    """§8.2 燃气分级状态机（网关 algo_task）。

    采样周期 dt 秒（契约默认 2 s）；一级需连续 T1_HOLD，二级需连续 T2_HOLD 或一级滞留
    L1_TO_L2；恢复到 <T1−RECOVER_MV 连续 RECOVER_HOLD 才回正常。
    """

    def __init__(self, t1_mv, t2_mv, l1_hold_s, l2_hold_s, l1_to_l2_s,
                 recover_mv, recover_hold_s, dt_s=2.0):
        self.t1, self.t2 = t1_mv, t2_mv
        self.l1_hold, self.l2_hold = l1_hold_s, l2_hold_s
        self.l1_to_l2, self.recover_mv, self.recover_hold = l1_to_l2_s, recover_mv, recover_hold_s
        self.dt = dt_s
        self.level = 0
        self._over1 = 0.0
        self._over2 = 0.0
        self._clean = 0.0
        self._l1_since: Optional[float] = None
        self.now = 0.0

    def feed(self, mv: float) -> List[Event]:
        events: List[Event] = []
        self.now += self.dt
        if self.level == 0:
            self._clean = 0.0
            if mv >= self.t1:
                self._over1 += self.dt
                if self._over1 >= self.l1_hold:
                    self.level = 1
                    self._l1_since = self.now
                    events.append(Event("alert", 1, "gas level 1", self.now))
            else:
                self._over1 = 0.0
        elif self.level == 1:
            if mv >= self.t2:
                self._over2 += self.dt
            else:
                self._over2 = 0.0
            if self._over2 >= self.l2_hold or (self.now - (self._l1_since or self.now)) >= self.l1_to_l2:
                self.level = 2
                events.append(Event("alert", 2, "gas level 2", self.now))
            if mv < self.t1 - self.recover_mv:
                self._clean += self.dt
                if self._clean >= self.recover_hold:
                    self.level = 0
                    self._over1 = self._over2 = self._clean = 0.0
                    self._l1_since = None
                    events.append(Event("recover", 0, "gas normal", self.now))
            else:
                self._clean = 0.0
        else:  # level 2
            if mv < self.t1 - self.recover_mv:
                self._clean += self.dt
                if self._clean >= self.recover_hold:
                    self.level = 0
                    self._over1 = self._over2 = self._clean = 0.0
                    self._l1_since = None
                    events.append(Event("recover", 0, "gas normal", self.now))
            else:
                self._clean = 0.0
        return events


class FallLatch:
    """§8.4 跌倒迟滞：fall=1 立即 ALARM；退出须 K 次 fall=0 且距最后一次 fall=1 ≥ T_hold。"""

    def __init__(self, k_fall_zero: int, t_hold_s: float, suppress_s: float):
        self.k, self.t_hold, self.suppress = k_fall_zero, t_hold_s, suppress_s
        self.state = "IDLE"
        self.zero_count = 0
        self.last_trigger: Optional[float] = None

    def feed(self, fall: bool, now: float) -> List[Event]:
        events: List[Event] = []
        if self.state == "IDLE":
            if fall:
                self.state = "ALARM"
                self.last_trigger = now
                self.zero_count = 0
                events.append(Event("alert", 2, "fall", now))
        else:
            if fall:
                self.last_trigger = now          # ALARM 内重复触发：不产生新告警
                self.zero_count = 0
            else:
                self.zero_count += 1
                if self.zero_count >= self.k and self.last_trigger is not None \
                        and now - self.last_trigger >= self.t_hold:
                    self.state = "IDLE"
                    self.zero_count = 0
                    events.append(Event("recover", 0, "fall cleared", now))
        return events

    def suppress_until(self) -> float:
        return (self.last_trigger or 0.0) + self.suppress


@dataclass
class MotionState:
    state: str = "home"           # home / away
    last_motion: float = 0.0
    no_motion_alerted: bool = False
    away_since: Optional[float] = None
    door_was_open: bool = False
    alerts: List[Event] = field(default_factory=list)


class PresenceTracker:
    """§8.5 无活动 + 外出模式。door_open=true 表示门被打开（门磁触发）。"""

    def __init__(self, no_motion_min_s: float, away_after_open_s: float, presence_hold_s: float):
        self.no_motion_s = no_motion_min_s * 60.0
        self.away_after = away_after_open_s
        self.presence_hold = presence_hold_s
        self.st = MotionState()

    def feed(self, now: float, motion: bool = False, door_open: bool = False) -> List[Event]:
        events: List[Event] = []
        door_edge = door_open and not self.st.door_was_open
        self.st.door_was_open = door_open
        if motion or door_edge:
            self.st.last_motion = now
            self.st.no_motion_alerted = False
            if self.st.state == "away":
                self.st.state = "home"
                self.st.away_since = None
        if door_edge and self.st.state == "home":
            self.st.state = "away"
            self.st.away_since = now
        if self.st.state == "away" and self.st.away_since is not None \
                and now - self.st.away_since >= self.away_after and now - self.st.last_motion >= self.away_after:
            # 外出窗口内一直无活动 → 发 no_motion（文案「30 分钟未检测到活动」由服务器生成）
            if not self.st.no_motion_alerted:
                self.st.no_motion_alerted = True
                events.append(Event("alert", 1, "no_motion", now))
        elif self.st.state == "home" and now - self.st.last_motion >= self.no_motion_s:
            if not self.st.no_motion_alerted:
                self.st.no_motion_alerted = True
                events.append(Event("alert", 1, "no_motion", now))
        return events


class OfflineTracker:
    """§8.7 两条独立规则：节点 3 s × 3 = 9 s（汇聚判）；网关 60 s × 3 = 180 s（服务器判）。"""

    def __init__(self, node_offline_s: float, gateway_offline_s: float):
        self.node_s = node_offline_s
        self.gateway_s = gateway_offline_s

    def node_offline(self, last_seen: float, now: float) -> bool:
        return now - last_seen >= self.node_s

    def gateway_offline(self, last_report: float, now: float) -> bool:
        return now - last_report >= self.gateway_s


class RepushScheduler:
    """§8.8 重推节奏 [0,60,300]，最多 3 次；停推：status≠pending / recovered_at 非空 / count≥3。"""

    def __init__(self, schedule: List[int], max_push: int = 3):
        self.schedule = list(schedule)
        self.max_push = max_push

    def should_push(self, *, now: float, created_at: float, push_count: int, status: str,
                    recovered_at: Optional[float]) -> bool:
        if status != "pending" or recovered_at is not None or push_count >= self.max_push:
            return False
        if push_count >= len(self.schedule):
            return False
        return now - created_at >= self.schedule[push_count]

    def next_at(self, *, created_at: float, push_count: int) -> Optional[float]:
        if push_count >= min(self.max_push, len(self.schedule)):
            return None
        return created_at + self.schedule[push_count]


def business_error(http_status: int, biz_code: int) -> bool:
    """§6.1：code≠0 时 HTTP 必须是错误码，绝不允许 HTTP 200 配非 0 code。"""
    if biz_code == 0:
        return http_status in (200, 201)
    return http_status >= 400
