#!/usr/bin/env python3
"""双板星闪冒烟：把两块板的串口抓取原文判成「通过 / 不通过」。

为什么要有这个脚本：t12 第一次上板时，判读靠人眼翻 7700 行串口日志，
结论「rx=0」是对的、但「lost 无从谈起」这种话没法复算。这里把判据写成代码，
任何一次抓取都能用同一条命令重判，结论可复现、可回归。

用法：
    python3 tools/spike-check-notify.py <主端日志> <从端日志> [--seconds 90]
    python3 tools/spike-check-notify.py <主端日志> <从端日志> --recovery

日志格式 = tools/serial-capture.ps1 的产物：每行 `<epoch_ms>|<串口原文>`。

判据（全部满足才打印 PASS、退出码 0）：
  1) 防烧反：主端日志有 SPIKE-ROLE=MASTER 且没有 SPIKE-ROLE=SLAVE，从端反之；
  2) 主端 `tx seq=N` 从 0 起、逐个 +1、无断档，且 `notify FAILED` 为 0；
  3) 从端出现 `SUBSCRIBED`；
  4) 从端 `rx #N seq=M` 的 seq 从首帧起逐个 +1、无断档，`!! GAP`/`OUT-OF-ORDER` 为 0，
     最后一次 `lost=` 读数为 0；
  5) 从端收包速率：窗口 >= 请求秒数的 95%，rx >= 0.90 × 窗口秒数，rate ∈ [0.80, 1.20]/s；
  6) 周期抖动（t53 新增）：间隔 P95 <= 1.2× 标称周期、单次最坏间隔 <= 2.0× 标称周期。

为什么判据 6 只做「不超过两个周期」这么松：链路的用途是每秒一帧的告警/状态上报
（基线 ~1 帧/s），真正要防的是「卡住若干秒」而不是微秒级抖动。所以阈值按**周期数**
给：P95 不超过 1.2 个周期说明没有系统性漂移（漂移会体现为均值偏离 1000 ms），
最坏间隔不超过 2.0 个周期说明没有整拍丢失；超过就说明这一分钟里出现过停顿，
必须回头看固件而不是把它当成正常抖动。阈值与实测值都会打印，便于复核者换阈值复算。

--recovery 模式（判据 3 的加强版，对应 t53 验收项「至少观测一次断连恢复」）：
从端复位/断电后应能重新连上并重新订阅、继续计数，主端不崩：
  R1) 从端日志出现 >= 2 次冷启动横幅（= 真的复位过，不是没断开）；
  R2) 最后一次冷启动之后仍出现 `SUBSCRIBED`（= 重连后重新订阅成功）；
  R3) 复位后重新收帧：新一段 `rx #N` 从 1 起逐个 +1，且该段 payload 的 `seq=`
      越过复位前末帧的 seq、段内逐个 +1，帧数 >= --min-frames；
  R4) 主端日志只有 1 次冷启动横幅（= 主端全程没重启）且 `notify FAILED` 为 0；
  R5) 主端 `tx seq=N` 跨整段日志仍逐个 +1（= 主端发帧线程不受从端复位影响）；
  R6) 整段到达间隔只记录（含断连空档，不作判定）。

为什么 R3 拿 `seq=` 当「接回同一路数据流」的判据、而不是拿 `rx #N` 比大小：
`rx #N` 是**本端**的累计收帧号，复位就清零——复位的板子必然从 #1 重来（这本身
是「确实复位过」的旁证，不是丢包）；`seq=` 由**服务端**维护，复位不影响它，
所以「复位后首帧 seq > 复位前末帧 seq」才真正证明从端接回了同一条 notify 流，
而不是在自己的计数里自说自话。
"""

import argparse
import re
import sys

TS = re.compile(r"^(\d+)\|(.*)$")
TX = re.compile(r"\[SPIKE-SRV\] tx seq=(\d+) ")
TX_FAIL = re.compile(r"\[SPIKE-SRV\] notify FAILED seq=(\d+)")
RX = re.compile(r"\[SPIKE-CLI\] rx #(\d+) seq=(-?\d+) ")
GAP = re.compile(r"\[SPIKE-CLI\] !! GAP")
OOO = re.compile(r"\[SPIKE-CLI\] !! OUT-OF-ORDER")
LOST_FIELD = re.compile(r"lost=(\d+)")
DONE = re.compile(r"\[SPIKE-CLI\] structure discovery DONE type=(\d+)")
# 冷启动横幅：-Reset 复位后 ROM 必打这一行。判读只认**最后一次复位之后**的日志——
# 抓取脚本先打开串口再拉复位，所以抓到的头几毫秒里可能还夹着上一轮的残余帧
# （t16 复跑实测：主端第 1 行是上一轮的 tx seq=46，紧跟着才是 boot.）。不裁掉的话
# 会被判成“seq 断档 46 → 0”，那是抓取窗口的边界，不是丢包。
BOOT = re.compile(r"^boot\.?\s*$")

# 抖动判据的标称周期（ms）。链路是 1 帧/s 的 notify，所以标称 1000 ms；
# 允许比例与绝对上限分开写，便于复核者一眼看出判据松紧。
NOMINAL_PERIOD_MS = 1000.0
JITTER_P95_MAX = 1.2      # × 标称周期
JITTER_WORST_MAX = 2.0    # × 标称周期


def percentile(vals, p):
    """线性插值分位数（与 numpy 默认口径一致），vals 单位与 p 无关。"""
    if not vals:
        return 0.0
    s = sorted(vals)
    if len(s) == 1:
        return float(s[0])
    k = (len(s) - 1) * (p / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def boot_count(lines):
    return sum(1 for _, t in lines if BOOT.match(t.strip()))


def trim_to_boot(lines):
    """裁到最后一次冷启动横幅之后；返回 (裁剪后行, 裁掉的行数)。没有横幅则原样返回。"""
    idx = None
    for i, (_, t) in enumerate(lines):
        if BOOT.match(t.strip()):
            idx = i
    if idx is None:
        return lines, 0
    return lines[idx:], idx


def load(path):
    """→ [(epoch_ms|None, text)]"""
    out = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            raw = raw.rstrip("\n").rstrip("\r")
            m = TS.match(raw)
            if m:
                out.append((int(m.group(1)), m.group(2)))
            else:
                out.append((None, raw))
    return out


def monotonic_by_one(seqs):
    """返回 (是否逐个 +1, 第一处断档的人话描述)。seqs 应为 int 列表。"""
    for i in range(1, len(seqs)):
        if seqs[i] != seqs[i - 1] + 1:
            return False, f"第 {i} 处断档：{seqs[i-1]} → {seqs[i]}（第 {i+1} 帧）"
    return True, ""


def intervals_ms(lines, pat):
    """按**主机侧时间戳**算相邻匹配行的时间差（ms）。返回 (间隔表, 首个时间戳)。"""
    ts = [t for t, txt in lines if t is not None and pat.search(txt)]
    return [ts[i] - ts[i - 1] for i in range(1, len(ts))], (ts[0] if ts else None)


def jitter_line(iv):
    """把间隔表压成一行可复核的量化结论。"""
    if not iv:
        return None
    p50, p95, worst = percentile(iv, 50), percentile(iv, 95), max(iv)
    worst_at = iv.index(worst) + 1
    over1 = sum(1 for x in iv if x > 1.5 * NOMINAL_PERIOD_MS)
    over2 = sum(1 for x in iv if x > JITTER_WORST_MAX * NOMINAL_PERIOD_MS)
    return {
        "n": len(iv), "p50": p50, "p95": p95, "worst": worst, "worst_at": worst_at,
        "mean": sum(iv) / len(iv), "over1": over1, "over2": over2,
        "text": (f"n={len(iv)} 间隔，P50={p50:.1f}ms P95={p95:.1f}ms 最坏={worst:.1f}ms"
                 f"（第 {worst_at}/{len(iv)} 处）均值={sum(iv)/len(iv):.1f}ms；"
                 f">1.5 周期 {over1} 次、>2.0 周期 {over2} 次"),
    }


def check(path_master, path_slave, seconds):
    lines_m, lines_s = load(path_master), load(path_slave)
    lines_m, drop_m = trim_to_boot(lines_m)
    lines_s, drop_s = trim_to_boot(lines_s)
    text_m = "\n".join(t for _, t in lines_m)
    text_s = "\n".join(t for _, t in lines_s)
    results = []          # (ok, 项目, 说明)
    fatal = []

    # ---- 1 角色（防烧反）
    m_own = "SPIKE-ROLE=MASTER" in text_m
    m_other = "SPIKE-ROLE=SLAVE" in text_m
    s_own = "SPIKE-ROLE=SLAVE" in text_s
    s_other = "SPIKE-ROLE=MASTER" in text_s
    ok = m_own and not m_other and s_own and not s_other
    results.append((ok, "角色（防烧反）",
                    f"主端 MASTER={m_own}/SLAVE={m_other}，从端 SLAVE={s_own}/MASTER={s_other}"))
    if not ok:
        fatal.append("烧反了或日志取错：两块板的角色标记对不上")

    # ---- 2 主端 tx
    tx_seq = [int(x) for x in TX.findall(text_m)]
    tx_fail = [int(x) for x in TX_FAIL.findall(text_m)]
    if tx_seq:
        ok_seq, why = monotonic_by_one(tx_seq)
        results.append((ok_seq and not tx_fail, "主端 tx seq 逐个 +1 且无发送失败",
                        f"{len(tx_seq)} 帧，首={tx_seq[0]} 末={tx_seq[-1]}，"
                        f"notify FAILED={len(tx_fail)}" + (f"，{why}" if why else "")))
        if not ok_seq:
            fatal.append(f"主端 tx seq 有断档：{why}")
        if tx_fail:
            fatal.append(f"主端有 {len(tx_fail)} 次 notify FAILED（seq={tx_fail[:5]}）")
    else:
        results.append((False, "主端 tx seq 逐个 +1 且无发送失败", "主端一帧都没发（tx seq= 出现 0 次）"))
        fatal.append("主端没发通知——先看发包线程的守卫条件与连接状态标志")

    # ---- 3 从端订阅
    sub = "SUBSCRIBED" in text_s
    n_attempt = text_s.count("subscribe: write CCCD")
    n_cccd_fail = text_s.count("CCCD write failed")
    n_done = DONE.findall(text_s)
    # 对端（服务端）侧的独立证据：write_request_cb 真的收到了 CCCD 写。
    peer_cccd = text_m.count("CCCD written by client")
    results.append((sub, "从端出现 SUBSCRIBED",
                    f"CCCD 写尝试={n_attempt} 次、失败={n_cccd_fail} 次、"
                    f"discovery DONE={len(n_done)} 次（type 序列前 6 个={n_done[:6]}）；"
                    f"对端收到 CCCD 写={peer_cccd} 次（服务端 write_request_cb）"))
    if not sub:
        fatal.append("从端从未订阅成功（SUBSCRIBED 缺失）")

    # ---- 4 从端 rx 连续性
    rx = [(int(n), int(s)) for n, s in RX.findall(text_s)]
    gaps, ooo = len(GAP.findall(text_s)), len(OOO.findall(text_s))
    lost_last = [int(x) for x in LOST_FIELD.findall(text_s)]
    if rx:
        seqs = [s for _, s in rx]
        ok_seq, why = monotonic_by_one(seqs)
        lost_last_v = lost_last[-1] if lost_last else -1
        ok = ok_seq and gaps == 0 and ooo == 0 and lost_last_v == 0
        results.append((ok, "从端 rx seq 逐个 +1、GAP=0、lost=0",
                        f"{len(rx)} 帧，首 seq={seqs[0]} 末 seq={seqs[-1]}，"
                        f"!! GAP={gaps} OUT-OF-ORDER={ooo}，最后 lost={lost_last_v}"
                        + (f"，{why}" if why else "")))
        if not ok_seq:
            fatal.append(f"从端 seq 断档：{why}")
        if gaps or ooo:
            fatal.append(f"从端出现 GAP={gaps} OUT-OF-ORDER={ooo}（真丢包）")
        if lost_last_v != 0:
            fatal.append(f"从端最后 lost={lost_last_v} ≠ 0")
    else:
        results.append((False, "从端 rx seq 逐个 +1、GAP=0、lost=0", "从端一帧都没收到（rx # 出现 0 次）"))
        fatal.append("从端没收到通知")

    # ---- 5 速率
    ts_s = [t for t, _ in lines_s if t]
    if rx and ts_s and len(rx) >= 2:
        span_ms = ts_s[-1] - ts_s[0]
        span_s = span_ms / 1000.0
        rate = len(rx) / span_s if span_s > 0 else 0.0
        ok = span_s >= seconds * 0.95 and len(rx) >= 0.90 * span_s and 0.80 <= rate <= 1.20
        results.append((ok, "从端收包速率 ≈1 帧/秒",
                        f"窗口 {span_s:.1f}s（要求 ≥{seconds * 0.95:.1f}s），"
                        f"收到 {len(rx)} 帧，速率 {rate:.3f} 帧/秒"))
        if span_s < seconds * 0.95:
            fatal.append(f"抓取窗口只有 {span_s:.1f}s，不足 {seconds}s")
        if rate < 0.80 or rate > 1.20:
            fatal.append(f"收包速率 {rate:.3f} 帧/秒 偏离 1 帧/秒")
    else:
        results.append((False, "从端收包速率 ≈1 帧/秒", "无时间戳或帧数不足，无法算速率"))

    # ---- 6 周期抖动（t53：量化而不是只给平均帧率）
    iv_rx, _ = intervals_ms(lines_s, RX)
    j = jitter_line(iv_rx)
    if j:
        ok = j["p95"] <= JITTER_P95_MAX * NOMINAL_PERIOD_MS and j["worst"] <= JITTER_WORST_MAX * NOMINAL_PERIOD_MS
        results.append((ok, "从端到达间隔抖动（P50/P95/最坏）",
                        j["text"] + f"；判据 P95≤{JITTER_P95_MAX:g}×标称 "
                                    f"最坏≤{JITTER_WORST_MAX:g}×标称（标称 {NOMINAL_PERIOD_MS:.0f}ms）"))
        if not ok:
            fatal.append(f"到达间隔超出抖动阈值：P95={j['p95']:.1f}ms 最坏={j['worst']:.1f}ms")
    else:
        results.append((False, "从端到达间隔抖动（P50/P95/最坏）", "可算间隔的接收行不足 2 条"))
        fatal.append("无法量化抖动（接收行不足）")

    print(f"主端日志：{path_master}（{len(lines_m)} 行，冷启动横幅前裁掉 {drop_m} 行）")
    print(f"从端日志：{path_slave}（{len(lines_s)} 行，冷启动横幅前裁掉 {drop_s} 行）")
    print("-" * 78)
    for ok, item, detail in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {item}")
        print(f"       {detail}")
    print("-" * 78)
    if fatal:
        print("结论：不通过")
        for f in fatal:
            print(f"  - {f}")
        return 1
    print("结论：通过（主端持续发、从端每秒收到、seq 连续、lost=0、抖动在阈值内）")
    return 0


def check_recovery(path_master, path_slave, min_frames=20):
    """判「从端复位后重连恢复」：见文件头 R1–R6。"""
    lines_m_raw, lines_s_raw = load(path_master), load(path_slave)
    boots_s, boots_m = boot_count(lines_s_raw), boot_count(lines_m_raw)
    # 恢复场景**不裁**到第一次启动：要的就是「复位前 + 复位后」两段。
    text_m = "\n".join(t for _, t in lines_m_raw)
    text_s = "\n".join(t for _, t in lines_s_raw)
    last_boot = max((i for i, (_, t) in enumerate(lines_s_raw) if BOOT.match(t.strip())), default=None)
    after = lines_s_raw[last_boot:] if last_boot is not None else []
    text_after = "\n".join(t for _, t in after)

    results, fatal = [], []

    # R1 从端确实复位过
    ok = boots_s >= 2
    results.append((ok, "R1 从端复位过（>=2 次冷启动横幅）", f"从端冷启动横幅 {boots_s} 次"))
    if not ok:
        fatal.append("从端只启动一次，这次抓取不构成「断连恢复」观测")

    # R2 复位后重新订阅成功
    sub_after = "SUBSCRIBED" in text_after
    results.append((sub_after, "R2 复位后重新 SUBSCRIBED",
                    f"最后一次冷启动（第 {last_boot + 1 if last_boot is not None else '-'} 行）"
                    f"之后 SUBSCRIBED={'出现' if sub_after else '缺失'}，"
                    f"该段 CCCD 写尝试={text_after.count('subscribe: write CCCD')} 次"))
    if not sub_after:
        fatal.append("复位后没有再订阅成功（SUBSCRIBED 缺失）")

    # R3 复位后重新收帧（本卡最核心的一条证据）
    # 注意不能拿 `rx #N` 跟复位前比大小：那是**本端**的累计收帧号，复位就清零，
    # 复位的板子必然从 #1 重新开始（这正是「确实复位过」的旁证，不是丢包）。
    # 能证明「接回了同一路数据流」的是 payload 里的 `seq=` —— 由**服务端**维护，
    # 复位不影响它，所以复位后首帧的 seq 必须 > 复位前末帧的 seq，且段内逐个 +1。
    before_text = "\n".join(t for _, t in lines_s_raw[:last_boot or 0])
    rx_before = [(int(n), int(s)) for n, s in RX.findall(before_text)]
    rx_after = [(int(n), int(s)) for n, s in RX.findall(text_after)]
    rb_txt = (f"复位前 {len(rx_before)} 条（末 seq={rx_before[-1][1]}，rx #{rx_before[-1][0]}）"
              if rx_before else "复位前 0 条")
    if rx_after and rx_before:
        n_ok = all(rx_after[i][0] == rx_after[i - 1][0] + 1 for i in range(1, len(rx_after)))
        seq_ok = all(rx_after[i][1] == rx_after[i - 1][1] + 1 for i in range(1, len(rx_after)))
        cont = rx_after[0][1] > rx_before[-1][1]
        enough = len(rx_after) >= min_frames
        ok = n_ok and seq_ok and cont and enough
        # 恢复时延：最后一次冷启动 → 复位后第一帧
        boot_ts = next((t for t, txt in lines_s_raw[last_boot:] if BOOT.match(txt.strip())), None)
        first_ts = next((t for t, txt in after if t is not None and RX.search(txt)), None)
        lat = f"，恢复时延 {(first_ts - boot_ts) / 1000.0:.1f}s" if boot_ts and first_ts else ""
        results.append((ok, "R3 复位后重新收帧（seq 越过复位点、段内逐个 +1）",
                        f"{rb_txt} → 复位后 {len(rx_after)} 条：首 seq={rx_after[0][1]} "
                        f"末 seq={rx_after[-1][1]}（rx #1..# {rx_after[-1][0]}）{lat}；"
                        f"段内 rx#+1={n_ok} seq+1={seq_ok} 越过复位点={cont} "
                        f"帧数≥{min_frames}={enough}"))
        if not ok:
            fatal.append(f"复位后没有恢复收帧：seq {rx_before[-1][1]} → {rx_after[0][1]}，"
                         f"段内 rx#+1={n_ok} seq+1={seq_ok} 帧数={len(rx_after)}")
    else:
        results.append((False, "R3 复位后重新收帧（seq 越过复位点、段内逐个 +1）",
                        f"{rb_txt} → 复位后 {len(rx_after)} 条，无法证明恢复"))
        fatal.append("复位前后缺一段收帧记录，无法证明恢复")

    # R4 主端全程没重启、没有发送失败
    tx_fail = len(TX_FAIL.findall(text_m))
    ok = boots_m <= 1 and tx_fail == 0
    results.append((ok, "R4 主端全程未重启且 notify FAILED=0",
                    f"主端冷启动横幅 {boots_m} 次，notify FAILED={tx_fail} 次"))
    if not ok:
        fatal.append(f"主端不干净：横幅 {boots_m} 次 / notify FAILED {tx_fail} 次")

    # R5 主端 tx seq 跨整段仍逐个 +1
    tx_seq = [int(x) for x in TX.findall(text_m)]
    if tx_seq:
        ok_seq, why = monotonic_by_one(tx_seq)
        results.append((ok_seq, "R5 主端 tx seq 跨整段逐个 +1",
                        f"{len(tx_seq)} 帧，首={tx_seq[0]} 末={tx_seq[-1]}" + (f"，{why}" if why else "")))
        if not ok_seq:
            fatal.append(f"主端 tx seq 断档：{why}")
    else:
        results.append((False, "R5 主端 tx seq 跨整段逐个 +1", "主端一帧都没发"))
        fatal.append("主端没发通知")

    iv_rx, _ = intervals_ms([l for l in lines_s_raw if l[0]], RX)
    j = jitter_line(iv_rx)
    if j:
        results.append((True, "R6 整段到达间隔（含断连空档，仅记录不判定）", j["text"]))

    print(f"主端日志：{path_master}（{len(lines_m_raw)} 行，未裁剪）")
    print(f"从端日志：{path_slave}（{len(lines_s_raw)} 行，含 {boots_s} 次冷启动）")
    print("-" * 78)
    for ok, item, detail in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {item}")
        print(f"       {detail}")
    print("-" * 78)
    if fatal:
        print("结论：不通过（断连恢复未观测到）")
        for f in fatal:
            print(f"  - {f}")
        return 1
    print("结论：通过（从端复位后重新连上并重新订阅、继续收帧；主端全程未重启）")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("master_log")
    ap.add_argument("slave_log")
    ap.add_argument("--seconds", type=int, default=90, help="本次要求的抓取窗口（默认 90）")
    ap.add_argument("--recovery", action="store_true",
                    help="按「从端复位→重连→重新订阅→继续收帧」判（判据 R1–R6）")
    ap.add_argument("--min-frames", type=int, default=20,
                    help="--recovery 模式下要求复位后至少收到多少帧（默认 20）")
    a = ap.parse_args()
    if a.recovery:
        return check_recovery(a.master_log, a.slave_log, a.min_frames)
    return check(a.master_log, a.slave_log, a.seconds)


if __name__ == "__main__":
    sys.exit(main())
