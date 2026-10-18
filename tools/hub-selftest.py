#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hub-selftest.py — 汇聚 H1 的独立校验（主机侧，不需要硬件，也不需要 SDK）

做四件事：
  1. 编译并运行 tools/hub-selftest.c + ws63-firmware/src 的纯 C 层
     （hub_frame/hub_json/hub_agg/hub_adv/hub_glink/hub_vnode + node_json），
     参数 -std=c99 -Wall -Wextra -Werror —— 板子上跑的就是这几份文件。
  2. 自测套件覆盖：§3.3 CRC 自检向量、§3.6 分帧容错与重传（result=12）、
     §2.7 身份与向后兼容、§2.6 聚合状态机（轮询/离线/rtt/事件）、§3.4 扫描会话与四种视图。
  3. **契约样例帧逐字节回归**：从 docs/技术/接口契约.md 里把 §3.7（6 条）与 §3.9（5 条）
     样例帧的十六进制抠出来，再把它们的 PAYLOAD 喂给同一份组帧代码，断言输出与契约完全一致
     （含 CRC 小端）。这一步是"帧编码正确"的硬证据 —— 不是"我打印了类似的东西"，是**逐字节相等**。
  4. 用 Python 的 json 解析器真正解析每一条样例 PAYLOAD，并核对 LEN 字段 = 实际长度
     （C 里的 strstr 断言不了"这串是不是合法 JSON"，少一个引号照样能匹配到子串）。
  5. **t59 订阅链路**：源码级断言 CCCD 句柄/类型写对、判据不看 write cfm；
     并把 hub_cfg.h 里订阅判定纯函数 hub_sub_decide() 的用例表编出来跑
     （含"写满重试次数后任何 cfm 都必须强制进 READY"这条 livelock 反例）。

退出码：0 = 全通过；1 = 有失败；2 = 环境缺编译器 / 缺源文件。

环境说明：主机 WSL 没装 gcc（2026-10-17 实测），脚本会自动退回项目既有镜像 ws63wsl:latest
（里面有 gcc-11）。容器名带 hmprogram- 前缀（AGENTS.md 容器命名规则）。
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "ws63-firmware", "src")
OUT = os.path.join(ROOT, "ws63-firmware", "out")
CONTRACT = os.path.join(ROOT, "docs", "技术", "接口契约.md")
SELFTEST = os.path.join(ROOT, "tools", "hub-selftest.c")
IMAGE = os.environ.get("WS63_IMAGE", "ws63wsl:latest")
CC_CANDIDATES = ["gcc", "gcc-11", "cc", "clang"]

# 被测的纯 C 层（hub_client.c 不在内：它 include SLE 协议栈头，只能在 SDK 里编译）
SOURCES = [
    "hub_frame.c", "hub_json.c", "hub_agg.c", "hub_adv.c",
    "hub_glink.c", "hub_vnode.c", "node_json.c",
]

# §3.7 六条 + §3.9 五条：都以 AA 55 01 开头（SOF1+SOF2+VER），正文十六进制用空格分隔
FRAME_RE = re.compile(r"(?:AA\s+55\s+01(?:\s+[0-9A-Fa-f]{2}){7,})")


def find_cc():
    for c in CC_CANDIDATES:
        p = shutil.which(c)
        if p:
            return c
    return None


def run_inside_docker(extra=()):
    name = "hmprogram-hubselftest-%d" % os.getpid()
    cmd = [
        "docker", "run", "--rm", "--name", name,
        "-v", "%s:/w" % ROOT, "-w", "/w",
        IMAGE, "python3", "tools/hub-selftest.py", "--inside-docker", *extra,
    ]
    print("$ " + " ".join(cmd))
    return subprocess.call(cmd)


def compile_and_run(cc):
    for s in SOURCES:
        if not os.path.isfile(os.path.join(SRC, s)):
            print("缺源文件 %s" % os.path.join(SRC, s), file=sys.stderr)
            return 2
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "hub-selftest")
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-I", SRC,
               "-o", exe, SELFTEST] + [os.path.join(SRC, s) for s in SOURCES]
        print("$ " + " ".join(cmd), file=sys.stderr)
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode != 0:
            print(cp.stdout)
            print(cp.stderr, file=sys.stderr)
            print("自测程序编译失败（-Werror 视为错误）", file=sys.stderr)
            return 2
        print("编译通过：%s" % cc, file=sys.stderr)
        rp = subprocess.run([exe], capture_output=True, text=True)
        print(rp.stdout, end="")
        if rp.stderr.strip():
            print(rp.stderr, file=sys.stderr)
        os.makedirs(OUT, exist_ok=True)
        with open(os.path.join(OUT, "hub-selftest.log"), "w", encoding="utf-8") as f:
            f.write(rp.stdout)
        # mock 聚合日志单独留一份，作为"HUB_MOCK 聚合正确"的证据
        keep = []
        inmock = False
        for line in rp.stdout.splitlines():
            if line.startswith("== mock"):
                inmock = True
            elif line.startswith("== ") and inmock:
                inmock = False
            if inmock:
                keep.append(line)
        with open(os.path.join(OUT, "hub-mock-agg.log"), "w", encoding="utf-8") as f:
            f.write("汇聚 HUB_MOCK 聚合日志（由 tools/hub-selftest.py 生成，可在主机复现）\n")
            f.write("\n".join(keep) + "\n")
        return rp.returncode


def frames_from_contract():
    """从契约里抠出 §3.7/§3.9 的样例帧：返回 [(十六进制, type, seq, payload_bytes), ...]"""
    with open(CONTRACT, encoding="utf-8") as f:
        text = f.read()
    # 只扫 §3.7 与 §3.9 两节，避免误抓别的十六进制串
    spans = []
    for marker in ("### 3.7", "### 3.9"):
        i = text.find(marker)
        if i < 0:
            continue
        j = text.find("\n### ", i + 4)
        spans.append((i, j if j > 0 else len(text)))
    out = []
    for a, b in spans:
        for m in FRAME_RE.finditer(text[a:b]):
            hexs = re.sub(r"\s+", " ", m.group(0).strip())
            raw = bytes(int(x, 16) for x in hexs.split())
            if len(raw) < 12:
                continue
            typ = raw[3]
            seq = raw[4] | (raw[5] << 8)
            ln = raw[6] | (raw[7] << 8)
            if 10 + ln != len(raw):
                out.append((hexs, typ, seq, None, "LEN=%d 与实际字节数 %d 不一致" % (ln, len(raw) - 10)))
                continue
            out.append((hexs, typ, seq, raw[8:8 + ln], None))
    return out


def check_contract_frames(cc):
    frames = frames_from_contract()
    print("\n== 契约样例帧逐字节回归（§3.7 / §3.9）==")
    if not frames:
        print("  FAIL  没从契约里抠到样例帧（正则或文档结构变了？）")
        return 1
    fails = 0
    lines = []
    for hexs, typ, seq, payload, err in frames:
        tag = "0x%02X SEQ=%d" % (typ, seq)
        if err:
            print("  FAIL  %s  %s" % (tag, err))
            fails += 1
            continue
        try:
            js = json.loads(payload.decode("utf-8"))
        except Exception as e:                      # noqa: BLE001
            print("  FAIL  %s  PAYLOAD 不是合法 JSON: %s" % (tag, e))
            fails += 1
            continue
        if not isinstance(js, dict):
            print("  FAIL  %s  PAYLOAD 顶层不是 JSON 对象（§3.5 要求）" % tag)
            fails += 1
            continue
        lines.append("%x %d %s" % (typ, seq, payload.decode("utf-8")))
    if fails:
        return fails

    # 把契约里的 PAYLOAD 喂给同一份组帧代码，逐字节比较
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "hub-selftest")
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-I", SRC,
               "-o", exe, SELFTEST] + [os.path.join(SRC, s) for s in SOURCES]
        subprocess.run(cmd, check=True, capture_output=True)
        rp = subprocess.run([exe, "-build"], input="\n".join(lines) + "\n",
                            capture_output=True, text=True)
        got = [re.sub(r"\s+", " ", x.strip()) for x in rp.stdout.splitlines() if x.strip()]
    if len(got) != len(frames):
        print("  FAIL  取回的帧数 %d != 契约样例数 %d" % (len(got), len(frames)))
        return 1
    for (hexs, typ, seq, _payload, _err), g in zip(frames, got):
        tag = "0x%02X SEQ=%d" % (typ, seq)
        if g == "ERR":
            print("  FAIL  %s  组帧失败" % tag)
            fails += 1
        elif g.upper() != hexs.upper():
            print("  FAIL  %s  与契约不一致" % tag)
            print("        契约: %s" % hexs)
            print("        实测: %s" % g)
            fails += 1
        else:
            print("  PASS  %s  逐字节一致（%d 字节）" % (tag, len(hexs.split())))
    print("  样例帧：%d 条，失败 %d 条" % (len(frames), fails))
    return fails


def check_fw_version_consistency():
    """mock 帧与真帧的 `fw` 字段必须同源同值。

    hub_vnode.c 刻意不 include hub_cfg.h（后者带 SDK 宏，会把"纯 C、主机可编译"拖没），
    所以它硬写了一份 fw 字符串；那条注释承诺"hub-selftest.py 会抠出来逐条断言"。
    本函数就是兑现这条注释：两处宏必须相等，否则 mock 聚合日志里的 fw 与上板帧不一致，
    而"日志证明 mock 节点聚合正确"这个验收项就失去意义。
    """
    print("\n== mock fw 版本一致性（hub_vnode.c 的 VNODE_FW vs hub_cfg.h 的 HUB_FW_VERSION）==")

    def grab(name, macro):
        path = os.path.join(SRC, name)
        if not os.path.isfile(path):
            return None
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = re.match(r'\s*#define\s+%s\s+"([^"]*)"' % re.escape(macro), line)
                if m:
                    return m.group(1)
        return None

    a = grab("hub_vnode.c", "VNODE_FW")
    b = grab("hub_cfg.h", "HUB_FW_VERSION")
    if a is None or b is None:
        print("  FAIL  抠不到宏（hub_vnode.c VNODE_FW=%r，hub_cfg.h HUB_FW_VERSION=%r）"
              % (a, b))
        return 1
    if a != b:
        print('  FAIL  两处 fw 不一致：hub_vnode.c VNODE_FW="%s" != hub_cfg.h HUB_FW_VERSION="%s"' % (a, b))
        print("        同一个固件里 mock 帧与真帧的 fw 字段必须同值（契约 §2.2）")
        return 1
    print('  PASS  VNODE_FW == HUB_FW_VERSION == "%s"' % a)
    return 0


SUBDECIDE_SRC = r"""
/* 由 tools/hub-selftest.py 生成：订阅判定纯函数的用例表（t59）。
 * 被测代码就是 ws63-firmware/src/hub_cfg.h 里的 hub_sub_decide() —— 直接 include 真源码，
 * 不复制一份逻辑到测试里（复制品永远会通过它自己的测试）。 */
#include <stdio.h>
#include "hub_cfg.h"

static int g_fails;

static void ck(const char *name, int got, int want)
{
    if (got == want) {
        return;
    }
    printf("  FAIL  %s: got=%d want=%d\n", name, got, want);
    g_fails++;
}

int main(void)
{
    /* 1. 窗口未到、没有首帧 -> 什么都不做（不是失败，只是还在等） */
    ck("wait/cfm0", hub_sub_decide(1u, 0x0, 0, 0), HUB_SUB_WAIT);
    ck("wait/no-cfm", hub_sub_decide(1u, -1, 0, 0), HUB_SUB_WAIT);
    ck("wait/cfm7", hub_sub_decide(2u, 0x7, 0, 0), HUB_SUB_WAIT);

    /* 2. 本 SDK 对有效 CCCD 写的 cfm 恒非 0（spike 实测 0x7）：**不能**当失败。
     *    窗口到了就该重写，而不是回退重发现（旧实现正是这里死循环）。 */
    ck("retry/cfm7", hub_sub_decide(1u, 0x7, 0, 1), HUB_SUB_RETRY);
    ck("retry/cfm0", hub_sub_decide(2u, 0x0, 0, 1), HUB_SUB_RETRY);
    ck("retry/no-cfm", hub_sub_decide(1u, -1, 0, 1), HUB_SUB_RETRY);

    /* 3. 只有 0x4（本机协议栈拒绝 / 候选无效）值得立刻重写，不必等满一个窗口 */
    ck("retry/rejected", hub_sub_decide(1u, (int)HUB_CCCD_STATUS_REJECTED, 0, 0), HUB_SUB_RETRY);

    /* 4. 首帧到了就是订阅生效 —— 优先于任何 cfm 值 */
    ck("confirmed/cfm7", hub_sub_decide(1u, 0x7, 1, 0), HUB_SUB_CONFIRMED);
    ck("confirmed/rejected", hub_sub_decide(1u, (int)HUB_CCCD_STATUS_REJECTED, 1, 1),
       HUB_SUB_CONFIRMED);

    /* 5. 上界：写满 HUB_SUB_RETRY_MAX 次后，**任何** cfm 组合都必须强制进 READY。
     *    这条是 livelock 的反例证明 —— 只要窗口到了，就不存在"永远 WAIT"的组合。 */
    {
        const int cfms[4] = { -1, 0x0, (int)HUB_CCCD_STATUS_REJECTED, 0x7 };
        int i;

        for (i = 0; i < 4; i++) {
            ck("force@bound/deadline", hub_sub_decide(HUB_SUB_RETRY_MAX, cfms[i], 0, 1), HUB_SUB_FORCE);
            ck("never-wait@bound", hub_sub_decide(HUB_SUB_RETRY_MAX, cfms[i], 0, 1) != HUB_SUB_WAIT, 1);
        }
        ck("force@rejected-immediate",
           hub_sub_decide(HUB_SUB_RETRY_MAX, (int)HUB_CCCD_STATUS_REJECTED, 0, 0), HUB_SUB_FORCE);
    }

    /* 6. 也不能提前放弃：还差一次时窗口到了应当重写，不是 FORCE */
    ck("not-early-force", hub_sub_decide((uint8_t)(HUB_SUB_RETRY_MAX - 1u), 0x7, 0, 1), HUB_SUB_RETRY);

    /* 7. 上界与文档声明一致：3 次、4.5 s，且远小于任务给的 15 s 上限 */
    ck("bound==3", (HUB_SUB_RETRY_MAX == 3u) ? 1 : 0, 1);
    ck("bound<=15s", ((unsigned)HUB_SUB_CONFIRM_MS * (unsigned)HUB_SUB_RETRY_MAX <= 15000u) ? 1 : 0, 1);

    printf("  %s  hub_sub_decide 用例表：%s（fails=%d）\n",
           g_fails ? "FAIL" : "PASS", g_fails ? "FAIL" : "OK", g_fails);
    return g_fails ? 1 : 0;
}
"""


SUBSTATE_SRC = r"""
/* t85 断言用台架：直接编真实 hub_cfg.h 的 hub_sub_state_next()（不复制被测逻辑）。
 * 覆盖 t60 F-02 的核心：FORCE（放行）与 CONFIRMED（真收到数据）必须互不覆盖。
 * 状态取值按 PENDING(0) < FORCED(1) < CONFIRMED(2) 排列，"不回退"= 结果 >= 当前。 */
#include <stdio.h>
#include "hub_cfg.h"

static int g_fails;

static void ck(const char *what, int got, int want)
{
    if (got != want) {
        g_fails++;
        printf("  FAIL %s: got=%d want=%d\n", what, got, want);
    }
}

int main(void)
{
    /* 1. 没有事件：状态不动 */
    ck("pending/idle", hub_sub_state_next(HUB_SUBSTATE_PENDING, 0, 0), HUB_SUBSTATE_PENDING);
    ck("forced/idle", hub_sub_state_next(HUB_SUBSTATE_FORCED, 0, 0), HUB_SUBSTATE_FORCED);
    ck("confirmed/idle", hub_sub_state_next(HUB_SUBSTATE_CONFIRMED, 0, 0), HUB_SUBSTATE_CONFIRMED);

    /* 2. 首帧是唯一可信证据：从任何状态都要收敛到 CONFIRMED */
    ck("frame/from-pending", hub_sub_state_next(HUB_SUBSTATE_PENDING, 1, 0), HUB_SUBSTATE_CONFIRMED);
    ck("frame/from-forced", hub_sub_state_next(HUB_SUBSTATE_FORCED, 1, 0), HUB_SUBSTATE_CONFIRMED);
    ck("frame/from-confirmed", hub_sub_state_next(HUB_SUBSTATE_CONFIRMED, 1, 0), HUB_SUBSTATE_CONFIRMED);

    /* 3. t60 F-02 的回归点：放行 ≠ 确认，放行也绝不能覆盖确认。
     *    一手证据（spike/logs/f70-hub-COM15.log:81/83）：force READY 之后 562 ms
     *    才来第一条 0x01，"FORCED + 首帧 → CONFIRMED" 就是"订阅真生效"的取证迁移。 */
    ck("force/does-not-confirm", hub_sub_state_next(HUB_SUBSTATE_PENDING, 0, 1), HUB_SUBSTATE_FORCED);
    ck("force/keeps-forced", hub_sub_state_next(HUB_SUBSTATE_FORCED, 0, 1), HUB_SUBSTATE_FORCED);
    ck("force/cannot-downgrade-confirmed",
       hub_sub_state_next(HUB_SUBSTATE_CONFIRMED, 0, 1), HUB_SUBSTATE_CONFIRMED);
    ck("late-confirm/after-force", hub_sub_state_next(HUB_SUBSTATE_FORCED, 1, 1), HUB_SUBSTATE_CONFIRMED);

    /* 4. 三条性质穷举（这是"行为断言"的本体，不是文本匹配） */
    {
        int cur, frame, forced;

        for (cur = 0; cur <= (int)HUB_SUBSTATE_CONFIRMED; cur++) {
            for (frame = 0; frame <= 1; frame++) {
                for (forced = 0; forced <= 1; forced++) {
                    int r = hub_sub_state_next(cur, frame, forced);

                    if (r < cur) {
                        g_fails++;
                        printf("  FAIL 单调性: cur=%d frame=%d forced=%d -> %d（回退了）\n",
                               cur, frame, forced, r);
                    }
                    if (frame != 0 && r != (int)HUB_SUBSTATE_CONFIRMED) {
                        g_fails++;
                        printf("  FAIL 首帧必须确认: cur=%d forced=%d -> %d\n", cur, forced, r);
                    }
                    if (cur == (int)HUB_SUBSTATE_CONFIRMED && r != (int)HUB_SUBSTATE_CONFIRMED) {
                        g_fails++;
                        printf("  FAIL CONFIRMED 是吸收态: forced=%d frame=%d -> %d\n", forced, frame, r);
                    }
                    if (frame == 0 && forced == 0 && r != cur) {
                        g_fails++;
                        printf("  FAIL 无事件不得改状态: cur=%d -> %d\n", cur, r);
                    }
                }
            }
        }
    }

    printf("  %s  hub_sub_state_next 用例表：%s（fails=%d）\n",
           g_fails ? "FAIL" : "PASS", g_fails ? "FAIL" : "OK", g_fails);
    return g_fails ? 1 : 0;
}
"""


def _strip_comments(text):
    """去掉 C 注释（保留字符串字面量）。

    为什么不直接对全文做正则：t59 修的就是"注释里写着错误做法、代码里跟着写"这种事故 ——
    hub_client.c 的注释里必须能出现 `prop_handle+1` 来说明"这是错的"，而断言只该盯代码。
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":                    # 字符串/字符字面量整段保留
            q = c
            out.append(c)
            i += 1
            while i < n:
                out.append(text[i])
                if text[i] == "\\" and i + 1 < n:
                    out.append(text[i + 1])
                    i += 2
                    continue
                if text[i] == q:
                    i += 1
                    break
                i += 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        out.append(c)
        i += 1
    return "".join(out)


def _func_body(text, name):
    """抠出 `<name>(...)` 那个函数的函数体（从名字到下一个行首 '}'）。"""
    i = text.find(name)
    if i < 0:
        return None
    j = text.find("\n}", i)
    return text[i:j + 2] if j > 0 else text[i:]


def check_cccd_handle():
    """t59 三条：CCCD 句柄/类型、订阅判据不看 cfm、判定集中在纯函数里。"""
    print("\n== CCCD 订阅链路（t59）==")
    path = os.path.join(SRC, "hub_client.c")
    if not os.path.isfile(path):
        print("  FAIL  没有 %s" % path)
        return 1
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    text = _strip_comments(raw)          # 断言只盯代码；注释里出现错误做法是**必须允许**的
    fails = 0

    if re.search(r"prop_handle\s*\+\s*1", text):
        print("  FAIL  hub_client.c 里还有 `prop_handle + 1` —— BLE 的「描述符 = 特征值 + 1」"
              "对 SLE 不成立（见 hub_cfg.h 的 CCCD 注释）")
        fails += 1
    if not re.search(r"param\.handle\s*=\s*c->prop_handle\s*;", text):
        print("  FAIL  cli_subscribe() 没写成 `param.handle = c->prop_handle;`")
        fails += 1
    if not re.search(r"param\.type\s*=\s*SSAP_DESCRIPTOR_CLIENT_CONFIGURATION\s*;", text):
        print("  FAIL  CCCD 写的 type 必须是 SSAP_DESCRIPTOR_CLIENT_CONFIGURATION(0x02)")
        fails += 1

    body = _func_body(text, "hub_write_cfm_cbk(")
    if body is None:
        print("  FAIL  找不到 hub_write_cfm_cbk()")
        fails += 1
    else:
        if "ERRCODE_SLE_SUCCESS" in body:
            print("  FAIL  hub_write_cfm_cbk() 又拿 cfm 当订阅成功判据了"
                  "（本 SDK 那笔 cfm 恒非 0 → 永远进不了 READY）")
            fails += 1
        if "HUB_STAGE_DONE" in body:
            print("  FAIL  hub_write_cfm_cbk() 里直接置 DONE —— 判定应当集中在 hub_sub_decide()")
            fails += 1
        if "last_cfm_status" not in body:
            print("  FAIL  hub_write_cfm_cbk() 没有把 cfm 记进 last_cfm_status（只该记录事实）")
            fails += 1

    if not re.search(r"hub_sub_decide\s*\(", text):
        print("  FAIL  hub_client.c 没有调用 hub_sub_decide()（订阅判定必须走这个纯函数，"
              "否则没法在主机上覆盖它）")
        fails += 1
    if "HUB_SUB_FORCE" not in text:
        print("  FAIL  hub_client.c 没有处理 HUB_SUB_FORCE —— 没有这条就还是可能卡在 CCCD")
        fails += 1
    if not re.search(r"hub_sub_confirm_on_frame\s*\(", text):
        print("  FAIL  没有 hub_sub_confirm_on_frame()（订阅生效只能由节点首帧确认）")
        fails += 1
    # t85：这里**不再**断言日志文本（t60 F-02 的教训：文本断言抓不住"这条路径其实
    # 走不到"）。判据本身的行为由 check_sub_state() 编真源码跑用例表覆盖；
    # 这里只断言"接线"——确认必须走那个纯函数、FORCE 必须写状态而不是冒充确认、
    # got_frame 必须来自真实状态（t59 曾把它硬编码成 0，于是 CONFIRMED 分支永不执行）。
    confirm_body = _func_body(text, "hub_sub_confirm_on_frame(")
    if confirm_body is None:
        print("  FAIL  找不到 hub_sub_confirm_on_frame()")
        fails += 1
    else:
        if "hub_sub_state_next(" not in confirm_body:
            print("  FAIL  hub_sub_confirm_on_frame() 没有走 hub_sub_state_next()"
                  "（状态迁移必须集中在可主机单测的纯函数里）")
            fails += 1
        if "== HUB_STAGE_DONE" in confirm_body:
            print("  FAIL  hub_sub_confirm_on_frame() 里又有 `== HUB_STAGE_DONE` 的判断"
                  "（t60 F-02：FORCE 置 DONE 之后到来的首帧会被早退吃掉 → 确认永远取不到）")
            fails += 1
    disc_body = _func_body(text, "hub_discovery_manage(")
    if disc_body is None:
        print("  FAIL  找不到 hub_discovery_manage()")
        fails += 1
    else:
        if "hub_sub_state_next(" not in disc_body:
            print("  FAIL  FORCE 分支没有写 sub_state（只置 DONE = 又变成"
                  "「放行冒充确认」，t60 F-02）")
            fails += 1
        if "HUB_SUBSTATE_CONFIRMED" not in disc_body:
            print("  FAIL  hub_sub_decide() 的 got_frame 不是从真实状态取的"
                  "（t59 硬编码 0 → CONFIRMED 分支永不执行）")
            fails += 1

    if fails == 0:
        print("  PASS  handle=c->prop_handle / type=SSAP_DESCRIPTOR_CLIENT_CONFIGURATION(0x02)"
              " / 判据不看 cfm / 判定集中且强制 READY / 确认走纯状态机（行为断言见"
              " check_sub_state）")
    return fails


def check_sub_state(cc):
    """t85 行为断言：编**真源码**的 hub_sub_state_next()（hub_cfg.h）跑状态机用例表。

    为什么必须行为化：t59 对"以首帧为准"只有一句源码文本断言（日志字符串在不在），
    而 t60 复核证明那条路径在正常固件里根本走不到（FORCE 置 DONE → 早退）——
    文本在、行为假，是典型的假绿灯。这里改成喂状态迁移并检查结果。
    """
    with tempfile.TemporaryDirectory() as td:
        csrc = os.path.join(td, "substate.c")
        exe = os.path.join(td, "substate")
        with open(csrc, "w", encoding="utf-8") as f:
            f.write(SUBSTATE_SRC)
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-I", SRC, "-o", exe, csrc]
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode != 0:
            print("  FAIL  订阅状态机用例表编译失败")
            print(cp.stdout)
            print(cp.stderr, file=sys.stderr)
            return 1
        rp = subprocess.run([exe], capture_output=True, text=True)
    print(rp.stdout, end="")
    if rp.stderr.strip():
        print(rp.stderr, file=sys.stderr)
    return 1 if rp.returncode != 0 else 0


def check_sub_decide(cc):
    """把 hub_sub_decide() 的用例表编出来跑（被测的是真源码，不是复制品）。"""
    with tempfile.TemporaryDirectory() as td:
        csrc = os.path.join(td, "subdecide.c")
        exe = os.path.join(td, "subdecide")
        with open(csrc, "w", encoding="utf-8") as f:
            f.write(SUBDECIDE_SRC)
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-I", SRC, "-o", exe, csrc]
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode != 0:
            print("  FAIL  订阅判定用例表编译失败")
            print(cp.stdout)
            print(cp.stderr, file=sys.stderr)
            return 1
        rp = subprocess.run([exe], capture_output=True, text=True)
    print(rp.stdout, end="")
    if rp.stderr.strip():
        print(rp.stderr, file=sys.stderr)
    return 1 if rp.returncode != 0 else 0



POLL_ACCT_SRC = r"""
/* t70 断言用台架：直接编真实 hub_agg.c，模拟「节点应答复到得比 200 ms 晚」这条路径。
 * 不复制被测逻辑，只喂时间线与上行帧。 */
#include <stdio.h>
#include <string.h>
#include "hub_agg.h"

static int fails;
static const uint8_t MAC1[6] = {0x11u,0x22u,0x33u,0x44u,0x55u,0x01u};

#define CK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL %s\n", (msg)); } } while (0)

/* 跑到状态机推出一条 POLL 动作为止；返回 poll id（没有则 -1），并记录是否见过离线事件 */
static int run_to_poll(hub_agg_t *a, hub_act_t *act, int64_t *now, int64_t limit_ms, int *saw_off)
{
    int64_t end = *now + limit_ms;
    while (*now < end) {
        uint8_t i;
        *now += 50;
        hub_act_clear(act);
        hub_agg_tick(a, *now, act);
        for (i = 0; i < act->n; i++) {
            if (act->q[i].kind == HUB_AK_EVENT_OFF) { *saw_off = 1; }
        }
        for (i = 0; i < act->n; i++) {
            if (act->q[i].kind == HUB_AK_POLL) { return (int)act->q[i].aux; }
        }
    }
    return -1;
}

int main(void)
{
    hub_agg_t a;
    hub_act_t act;
    hub_uplink_t u;
    int64_t now = 100000;
    int poll_id, saw_off = 0;

    hub_agg_init(&a, "H1", now);
    CK(hub_wl_add(&a, "M1", MAC1, now) == 0, "白名单加入 M1");
    CK(hub_agg_on_connected(&a, 0x1000u, MAC1, now) == 0, "连接 M1");
    hub_agg_set_proc(&a, 0x1000u, HUB_PROC_DONE, 0x1122u);
    CK(a.node[0].state == HUB_SLOT_READY, "节点进 READY");

    poll_id = run_to_poll(&a, &act, &now, 6000, &saw_off);
    CK(poll_id > 0, "状态机应发出 poll");
    CK(a.node[0].poll_pending == 1u, "poll 在途");
    {
        int64_t poll_at = now;
        memset(&u, 0, sizeof(u));
        strcpy(u.sn, "M1");
        strcpy(u.type, "status");
        u.req_id = poll_id;
        /* 关键：中间要真的跑节拍 —— 上板是 50 ms 一拍，超时判定发生在 tick 里。
         * 直接调 on_uplink 会绕过判定，那是自欺欺人的测法。 */
        {
            int64_t rt = poll_at;
            while (rt < poll_at + 250) {
                rt += 50;
                hub_act_clear(&act);
                hub_agg_tick(&a, rt, &act);
            }
            (void)hub_agg_on_uplink(&a, &u, rt, &act);
        }
        CK(a.poll_ok == 1u, "250ms 到应答必须结算 poll_ok（旧 200ms 上界下恒 0）");
        CK(a.poll_miss == 0u, "250ms 到应答不得记 poll_miss");
        CK(a.node[0].misses == 0u, "有应答则连续 miss 清零");
        CK(a.node[0].poll_missed_id == 0u, "有应答则清掉已判死的 poll id");
        CK(a.node[0].rtt_ms == 250u, "rtt 应为实测 250ms");
        CK(a.last_rtt_ms == 250u, "STAT 用的 last_rtt 应同步");
        CK(a.node[0].online == 1u, "有应答即在线");
    }
    now += 3000;
    (void)run_to_poll(&a, &act, &now, 6000, &saw_off);
    (void)run_to_poll(&a, &act, &now, 6000, &saw_off);
    (void)run_to_poll(&a, &act, &now, 6000, &saw_off);
    {
        int missed_id = (int)a.node[0].poll_id;
        (void)run_to_poll(&a, &act, &now, 6000, &saw_off);
        CK(a.poll_miss >= 3u, "静默至少记 3 次 miss");
        CK(a.node[0].online == 0u, "3 次 miss 判离线");
        CK(saw_off == 1, "离线须推 0x03 事件（EVENT_OFF）");
        {
            uint32_t ok_before = a.poll_ok;
            memset(&u, 0, sizeof(u));
            strcpy(u.sn, "M1");
            strcpy(u.type, "status");
            u.req_id = missed_id;
            hub_act_clear(&act);
            (void)hub_agg_on_uplink(&a, &u, now, &act);
            CK(a.poll_late == 1u, "迟到应答应计入 poll_late");
            CK(a.poll_ok == ok_before, "迟到应答不得补记 poll_ok");
        }
    }
    if (fails == 0) { printf("  poll 结算用例表：OK\n"); }
    return fails == 0 ? 0 : 1;
}
"""

UPLINK_FWD_SRC = r"""
/* t70 断言用台架：直接编真实 hub_glink.c + hub_frame.c，证明「取出上行的人真的把它发到
 * uart_write」且正文逐字节等于节点原 JSON（t59 上板 [GLINK] -> 0x01 为 0 条的那条链）。 */
#include <stdio.h>
#include <string.h>
#include "hub_glink.h"
#include "hub_frame.h"

static int fails;
static uint8_t tx[4096];
static uint32_t tx_len;
static int64_t g_now = 5000;

#define CK(cond, msg) do { if (!(cond)) { fails++; printf("  FAIL %s\n", (msg)); } } while (0)

static int32_t fake_write(void *ctx, const uint8_t *buf, uint32_t len)
{
    (void)ctx;
    if (tx_len + len > sizeof(tx)) { return -1; }
    memcpy(tx + tx_len, buf, len);
    tx_len += len;
    return (int32_t)len;
}
static int32_t fake_read(void *ctx, uint8_t *buf, uint32_t cap) { (void)ctx; (void)buf; (void)cap; return 0; }
static int64_t fake_now(void *ctx) { (void)ctx; return g_now; }
static void fake_log(void *ctx, const char *fmt, ...) { (void)ctx; (void)fmt; }

int main(void)
{
    static hub_glink_t g;
    hub_glink_port_t port;
    hub_glink_up_t up;
    const char *json = "{\"sn\":\"M1\",\"ts\":1760000000000,\"type\":\"gas\",\"payload\":{\"smoke_mv\":123}}";
    uint8_t expect[HUB_FRAME_MAX];
    int n, m;

    memset(&port, 0, sizeof(port));
    port.uart_write = fake_write;
    port.uart_read = fake_read;
    port.now_ms = fake_now;
    port.log = fake_log;
    port.ctx = NULL;
    hub_glink_init(&g, &port);

    memset(&up, 0, sizeof(up));
    strcpy(up.sn, "M1");
    up.rx_ms = g_now;
    up.len = (uint16_t)strlen(json);
    memcpy(up.json, json, up.len + 1u);

    tx_len = 0;
    n = hub_glink_send_uplink(&g, &up);
    CK(n > 0, "send_uplink 应写出帧");
    CK(tx_len == (uint32_t)n, "写出的字节数应等于返回的帧长");
    CK(g.up_sent == 1u, "up_sent 应 +1");

    m = hub_frame_build_node(1u, json, (uint16_t)strlen(json), expect, sizeof(expect));
    CK(m == n, "帧长应与 hub_frame_build_node 一致");
    CK(m > 0 && memcmp(tx, expect, (size_t)m) == 0, "帧应与 hub_frame_build_node 逐字节一致");
    {
        hub_parser_t pp;
        hub_frame_t fr;
        uint8_t scratch[HUB_PAYLOAD_MAX + 4u];
        hub_parser_init(&pp);
        CK(hub_parser_push(&pp, tx, tx_len) == 0u, "分帧器应吃下整帧（返回值是丢弃字节数）");
        CK(hub_parser_next(&pp, &fr, scratch, sizeof(scratch)) == 1, "应解出一帧");
        CK(fr.type == HUB_T_NODE_DATA, "TYPE 必须是 0x01");
        CK(fr.payload_len == (uint16_t)strlen(json), "正文长度不变");
        CK(memcmp(fr.payload, json, fr.payload_len) == 0, "正文必须逐字节等于节点原 JSON");
    }
    tx_len = 0;
    (void)hub_glink_send_uplink(&g, &up);
    CK(tx[4] == 2u && tx[5] == 0u, "第二帧 SEQ 应为 2（LE）");
    tx_len = 0;
    strcpy(up.json, "not-json");
    up.len = 8u;
    CK(hub_glink_send_uplink(&g, &up) < 0, "非 JSON 正文应被拒");
    CK(tx_len == 0u, "被拒时不得写出任何字节");
    if (fails == 0) { printf("  上行转发用例表：OK\n"); }
    return fails == 0 ? 0 : 1;
}
"""


def _compile_one(cc, name, src, extra_sources, td):
    """把一段内嵌 C 源码 + 若干真实源文件编成可执行文件，返回 (exe, err)。"""
    csrc = os.path.join(td, name + ".c")
    exe = os.path.join(td, name)
    with open(csrc, "w", encoding="utf-8") as f:
        f.write(src)
    cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-I", SRC,
           "-o", exe, csrc] + [os.path.join(SRC, s) for s in extra_sources]
    cp = subprocess.run(cmd, capture_output=True, text=True)
    if cp.returncode != 0:
        print(cp.stdout)
        print(cp.stderr, file=sys.stderr)
        return None, "编译失败：" + " ".join(extra_sources)
    return exe, None


def check_poll_accounting(cc):
    """t70：poll 结算必须与到达时延无关（旧代码用固定 200 ms 判死 -> poll_ok 恒 0）。"""
    with tempfile.TemporaryDirectory() as td:
        exe, err = _compile_one(cc, "pollacct", POLL_ACCT_SRC, ["hub_agg.c", "hub_json.c"], td)
        if exe is None:
            print("  FAIL  poll 结算用例表：" + err)
            return 1
        rp = subprocess.run([exe], capture_output=True, text=True)
    print(rp.stdout, end="")
    if rp.stderr.strip():
        print(rp.stderr, file=sys.stderr)
    return 0 if rp.returncode == 0 else 1


def check_pump_forwards():
    """t70：**取出上行的人**必须负责把它转发出去。

    只测 hub_glink_send_uplink() 本身不够 —— t59 的缺陷正是「有人把队列取空却没人发」。
    所以这里做源码级断言：hub_pump_uplinks() 的函数体里必须有转发调用。
    （断言跑在去注释后的文本上：注释里要能出现反面写法来说明问题。）
    """
    src = os.path.join(SRC, "hub_client.c")
    with open(src, encoding="utf-8") as f:
        text = _strip_comments(f.read())
    body = _func_body(text, "hub_pump_uplinks")
    if body is None:
        print("  FAIL  找不到 hub_pump_uplinks()")
        return 1
    bad = 0
    if "hub_glink_send_uplink(" not in body:
        print("  FAIL  队列取出方（hub_pump_uplinks）没有转发调用 —— 帧会被取走即丢")
        bad += 1
    if "hub_glink_pop_uplink(" not in body:
        print("  FAIL  hub_pump_uplinks 没在出队？语义已变，请同步本断言")
        bad += 1
    if bad == 0:
        print("  PASS  hub_pump_uplinks 取出后调用 hub_glink_send_uplink 转发（t70 P0 修复点）")
    return bad


def check_uplink_forward(cc):
    """t70：取出上行的人必须真的把 0x01 发到 uart_write，正文逐字节等于节点原 JSON。"""
    with tempfile.TemporaryDirectory() as td:
        exe, err = _compile_one(cc, "uplinkfwd", UPLINK_FWD_SRC,
                                 ["hub_glink.c", "hub_frame.c", "hub_json.c"], td)
        if exe is None:
            print("  FAIL  上行转发用例表：" + err)
            return 1
        rp = subprocess.run([exe], capture_output=True, text=True)
    print(rp.stdout, end="")
    if rp.stderr.strip():
        print(rp.stderr, file=sys.stderr)
    return 0 if rp.returncode == 0 else 1


def check_uart_tx_path():
    """t76（F-70-01）：UART 写路径必须按「实际写出的字节数」判成功，且失败要看得见。

    SDK 语义（已取证，见 doc/12 §11）：uapi_uart_write() 返回**实际写出的字节数**
    （uart.c `return write_count;`），只有 uapi_uart_param_check 失败才返回负 errcode。
    t59 的代码拿这个返回值跟 ERRCODE_SUCC(0) 比，于是写成功反被判失败、up_sent 恒 0。
    本断言守三件事（变异检验：把超时改回 0、或把判据改回 ERRCODE_SUCC、或删掉分级计数，
    这里必须变红）：
      1) 超时常量存在且非零；
      2) 成功判据是 n == len，写路径里不再出现 ERRCODE_SUCC；
      3) 失败分级计数（总数 / 短写 / 参数错）都在写路径里。
    """
    cfg = os.path.join(SRC, "hub_cfg.h")
    with open(cfg, encoding="utf-8") as f:
        cfg_text = _strip_comments(f.read())
    m = re.search(r"#define\s+HUB_UART_TX_TIMEOUT_MS\s+(\d+)u?", cfg_text)

    src = os.path.join(SRC, "hub_client.c")
    with open(src, encoding="utf-8") as f:
        text = _strip_comments(f.read())
    body = _func_body(text, "hub_port_write")
    if body is None:
        print("  FAIL  找不到 hub_port_write()")
        return 1

    bad = 0
    if m is None or int(m.group(1)) == 0:
        print("  FAIL  HUB_UART_TX_TIMEOUT_MS 缺失或为 0（非零超时是卡面要求）")
        bad += 1
    if "HUB_UART_TX_TIMEOUT_MS" not in body:
        print("  FAIL  写路径没用超时常量（还是硬编码 0？）")
        bad += 1
    if "ERRCODE_SUCC" in body:
        print("  FAIL  写路径又拿返回值跟 ERRCODE_SUCC 比 —— 返回值是字节数，不是 errcode")
        bad += 1
    if "(int32_t)len" not in body:
        print("  FAIL  成功判据不是 n == len（实际写出字节数）")
        bad += 1
    for cnt in ("g_uart_tx_err++", "g_uart_tx_short++", "g_uart_tx_param++"):
        if cnt not in body:
            print("  FAIL  失败分级计数缺少 %s —— 写失败会被吞掉" % cnt)
            bad += 1
    if bad == 0:
        print("  PASS  UART 写路径：非零超时 + 按实际写出字节数判成功 + 失败分级计数（F-70-01 修复点）")
    return bad


BOARD_UPLINK_RE = re.compile(r"\[GLINK\] -> 0x01 sn=(\S+) seq=(\d+) len=(\d+) body=(.*)$")
BOARD_NODE_TX_RE = re.compile(r"\[(\S+)\] tx (\d+) bytes: (\{.*\})\s*$")
BOARD_TS_RE = re.compile(r'"ts":(\d+)')
BOARD_STAT_KEYS = {
    "up_sent": r"up_sent=(\d+)",
    "poll_ok": r"poll_ok=(\d+)",
    "poll_miss": r"poll_miss=(\d+)",
    "late": r"late=(\d+)",
    "ev_off": r"ev_off=(\d+)",
}
BOARD_STAT_TX_RE = re.compile(r"tx\(ok=(\d+) err=(\d+) short=(\d+) param=(\d+)\)")


def _board_parse_stat(text):
    """把一条 STAT 行拆成 dict；行被截断（缺键）时返回 None，由调用方往前找上一条。"""
    out = {}
    for key, pat in BOARD_STAT_KEYS.items():
        m = re.search(pat, text)
        if m is None:
            return None
        out[key] = int(m.group(1))
    m = BOARD_STAT_TX_RE.search(text)
    if m is None:
        return None
    out["tx_ok"], out["tx_err"], out["tx_short"], out["tx_param"] = (int(g) for g in m.groups())
    return out


def _board_strip(line):
    """串口抓包每行前缀 `<epoch_ms>|`（tools/serial-capture.ps1），剥掉它再匹配。"""
    _, sep, text = line.partition("|")
    return text if sep else line


def _board_node_frames(path):
    """读节点板日志（COM14）：`[M1] tx 154 bytes: {…}` → {ts: [(字节数, JSON 原文), …]}。

    节点板自己打印的是**它发给汇聚的原文**，所以它可以给汇聚侧的转发结果当"真值"。
    同一个 ts 可能对应两帧（poll 一次问两问：status + 主类型），故用列表。
    """
    frames = {}
    if not path or not os.path.isfile(path):
        return frames
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = BOARD_NODE_TX_RE.search(_board_strip(line))
            if m is None:
                continue
            js = m.group(3)
            tsm = BOARD_TS_RE.search(js)
            if tsm is None:
                continue
            frames.setdefault(tsm.group(1), []).append((int(m.group(2)), js))
    return frames


def check_board_log(path, node_log=None):
    """上板实跑日志 → 机械判定（t70 验收 1/2；不传 --board-log 时 SKIP）。

    日志来源与格式（都在仓库里，不用猜）：
      `[GLINK] -> 0x01 sn=%s seq=%u len=%d body=%s` —— hub_glink.c 的 hub_glink_send_uplink()
      `STAT … up_sent=… poll_ok=… poll_miss=… ev_off=… tx(ok=… err=… short=… param=…)` —— hub_client.c 的 hub_stat()
      `[M1] tx <N> bytes: {…}`（节点板，给 --node-log 做逐字节真值）

    判据：
      A. 透传真的发生：`-> 0x01` 行数 > 0 且**等于最后一条 STAT 之前**出现的条数 == 该 STAT 的 up_sent
         （STAT 每 30 s 一条，快照之后还会有新帧，所以只比快照之前的）；
         每条完整行的 len == body 的 UTF-8 字节数 + 10（hub_frame.h: HUB_FRAME_MAX = HUB_PAYLOAD_MAX + 10）；
      B. 可被网关侧解析：完整行的 body 用 Python json 真解析，信封带 sn/ts/type/payload 且 sn 与帧头一致；
      C. 板端日志截断要认得出来，不能混过关：串口行上限实测 ≈198 字符（SDK console 行缓冲），
         超过它的打印会把行尾吃掉 —— 这类行必须**恰好等于**本文件里的行上限、且仍以该 sn 的信封开头；
         既解析不了又没顶到行上限的，判 FAIL（那才是真的被改坏）；
      D. seq 单调递增（16 位回绕算递增；不要求连续 —— 0x02/0x03 也吃 seq）；
      E. poll 结算：poll_ok ≥ 10、poll_miss ≤ 2、ev_off == 0；UART 写路径 tx(err=0 short=0 param=0)；
      F. 给了 --node-log 时逐字节对账：汇聚转发的每一帧都要能对上节点板打印的同 ts 帧 ——
         `节点字节数 + 10 == 帧头 len`，且汇聚侧正文是节点正文的**前缀**（完整行则要求完全相等）。
    """
    if not path:
        print("  SKIP  上板日志断言（未提供 --board-log <日志文件>）")
        return 0
    if not os.path.isfile(path):
        print("  FAIL  --board-log 指向的文件不存在：%s" % path)
        return 1

    ups, stats, last_stat_idx = [], [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            text = _board_strip(line)
            m = BOARD_UPLINK_RE.search(text)
            if m:
                ups.append((m.group(1), int(m.group(2)), int(m.group(3)), m.group(4), len(text)))
            elif "STAT up=" in text:
                stats.append(text)
                last_stat_idx = len(ups)

    bad = 0
    print("  上板日志：%s（-> 0x01 %d 条，STAT %d 条）" % (path, len(ups), len(stats)))
    if not ups:
        print("  FAIL  日志里没有任何 [GLINK] -> 0x01 —— 上行透传没发生（t59 的原始病）")
        return 1

    upcap = max(u[4] for u in ups)
    stat, stat_line = None, None
    for text in reversed(stats):
        stat = _board_parse_stat(text)
        if stat is not None:
            stat_line = text
            break
    if stat is None:
        print("  FAIL  日志里没有一条完整的 STAT 行 —— 判不了 poll 结算与 UART 写路径")
        bad += 1

    node_frames = _board_node_frames(node_log)
    complete, truncated = [], []
    json_bad, key_bad, sn_bad, len_bad, cut_bad = [], [], [], [], []
    for sn, _seq, flen, body, linelen in ups:
        try:
            env = json.loads(body)
        except ValueError as exc:
            if linelen == upcap and body.startswith('{"sn":"%s","ts":' % sn):
                truncated.append((sn, flen, body, linelen))
                continue
            json_bad.append("%s（行 %d 字符）" % (exc, linelen))
            continue
        complete.append((sn, flen, body))
        missing = [k for k in ("sn", "ts", "type", "payload") if k not in env]
        if missing:
            key_bad.append("%s 缺 %s" % (body[:50], ",".join(missing)))
        if env.get("sn") != sn:
            sn_bad.append("帧头 sn=%s / body sn=%s" % (sn, env.get("sn")))
        expect = len(body.encode("utf-8")) + 10
        if flen != expect:
            len_bad.append("len=%d ≠ body %d B + 10" % (flen, len(body.encode("utf-8"))))
    for sn, flen, body, linelen in truncated:
        if linelen != upcap:
            cut_bad.append("sn=%s 行 %d ≠ 上限 %d" % (sn, linelen, upcap))

    def _report(name, items, hint=""):
        nonlocal bad
        if items:
            bad += len(items)
            print("  FAIL  %s：%d 条，首条 %s%s" % (name, len(items), items[0], hint))
        else:
            print("  PASS  %s" % name)

    print("  INFO  行上限 %d 字符：完整行 %d 条、板端截断行 %d 条（截断是串口日志行上限，不是帧被改）"
          % (upcap, len(complete), len(truncated)))
    _report("0x01 完整行正文是合法 JSON（%d 条）" % len(complete), json_bad)
    _report("0x01 完整行信封带 sn/ts/type/payload（%d 条）" % len(complete), key_bad)
    _report("0x01 帧头 sn 与正文一致（%d 条）" % len(complete), sn_bad)
    _report("0x01 帧长 == 正文 + 10（%d 条）" % len(complete), len_bad, " —— 正文被改过？")
    _report("板端截断行恰好顶到行上限（%d 条）" % len(truncated), cut_bad)

    seq_bad, prev_all = [], [u[1] for u in ups]
    for prev, cur in zip(prev_all, prev_all[1:]):
        if not (cur > prev or prev > 60000):
            seq_bad.append("%d → %d" % (prev, cur))
    _report("0x01 seq 单调递增（%d 条，跳号正常）" % len(ups), seq_bad)

    if node_log:
        if not node_frames:
            print("  FAIL  --node-log 给了但没解析出任何 `[sn] tx N bytes: {…}` 行：%s" % node_log)
            bad += 1
        else:
            full, prefix, unmatched, mismatch = 0, 0, [], []
            for _sn, _seq, flen, body, linelen in ups:
                tsm = BOARD_TS_RE.search(body)
                cands = [f for f in node_frames.get(tsm.group(1) if tsm else "", [])
                         if f[0] + 10 == flen]
                if not cands:
                    unmatched.append("ts=%s len=%d" % (tsm.group(1) if tsm else "?", flen))
                    continue
                js = cands[0][1]
                if body == js:
                    full += 1
                elif js.startswith(body) and linelen == upcap:
                    prefix += 1
                else:
                    mismatch.append("ts=%s len=%d" % (tsm.group(1) if tsm else "?", flen))
            print("  与节点板逐字节对账：完全相等 %d 条、截断前缀一致 %d 条、对不上 %d 条、找不到对应帧 %d 条"
                  % (full, prefix, len(mismatch), len(unmatched)))
            if mismatch:
                bad += len(mismatch)
                print("  FAIL  汇聚转发内容与节点发出内容不一致：%d 条，首条 %s" % (len(mismatch), mismatch[0]))
            else:
                print("  PASS  每一帧都能对上节点板原文（字节数 +10 == len，正文前缀/全等）")
            if unmatched:
                bad += len(unmatched)
                print("  FAIL  有 %d 帧在节点日志里找不到对应帧（ts+len 对不上）：首条 %s"
                      % (len(unmatched), unmatched[0]))
            else:
                print("  PASS  所有 %d 帧都能在节点日志里定位" % len(ups))

    if stat is not None:
        print("  STAT 末条：up_sent=%d poll_ok=%d poll_miss=%d late=%d ev_off=%d "
              "tx(ok=%d err=%d short=%d param=%d)" % (
                  stat["up_sent"], stat["poll_ok"], stat["poll_miss"], stat["late"],
                  stat["ev_off"], stat["tx_ok"], stat["tx_err"], stat["tx_short"], stat["tx_param"]))
        before = len(ups) if last_stat_idx is None else last_stat_idx
        if stat["up_sent"] != before:
            print("  FAIL  末条 STAT 的 up_sent=%d ≠ 该行之前出现的 0x01 条数 %d"
                  % (stat["up_sent"], before))
            bad += 1
        else:
            print("  PASS  up_sent == 该 STAT 之前出现的 0x01 条数（%d）：队列取出后确实发了" % before)
        for cond, ok_msg, fail_msg in (
            (stat["poll_ok"] >= 10, "  PASS  poll_ok=%d ≥ 10" % stat["poll_ok"],
             "  FAIL  poll_ok=%d < 10 —— 轮询应答没被结算" % stat["poll_ok"]),
            (stat["poll_miss"] <= 2, "  PASS  poll_miss=%d ≤ 2（无系统性误判）" % stat["poll_miss"],
             "  FAIL  poll_miss=%d > 2 —— 还在按固定时延误判丢应答（t70 的病）" % stat["poll_miss"]),
            (stat["ev_off"] == 0, "  PASS  ev_off=0（无在线/离线反复）",
             "  FAIL  ev_off=%d —— 出现节点在线/离线反复" % stat["ev_off"]),
            (not (stat["tx_err"] or stat["tx_short"] or stat["tx_param"]),
             "  PASS  UART 写路径无失败（err=0 short=0 param=0）",
             "  FAIL  UART 写路径有失败：err=%d short=%d param=%d"
             % (stat["tx_err"], stat["tx_short"], stat["tx_param"])),
        ):
            print(ok_msg if cond else fail_msg)
            if not cond:
                bad += 1
    return bad


def main():
    in_docker = "--inside-docker" in sys.argv
    board_log, node_log = None, None
    for flag, name in (("--board-log", "board_log"), ("--node-log", "node_log")):
        if flag in sys.argv:
            k = sys.argv.index(flag)
            if k + 1 >= len(sys.argv):
                print("%s 后面要跟日志文件路径" % flag, file=sys.stderr)
                return 2
            if name == "board_log":
                board_log = sys.argv[k + 1]
            else:
                node_log = sys.argv[k + 1]
    cc = find_cc()
    if cc is None:
        if in_docker:
            print("容器里也没有 C 编译器（试过 %s）" % ", ".join(CC_CANDIDATES), file=sys.stderr)
            return 2
        if shutil.which("docker") is None:
            print("主机与容器都没有 C 编译器，无法自测", file=sys.stderr)
            return 2
        return run_inside_docker(extra=sys.argv[1:])

    rc = compile_and_run(cc)
    if rc == 2:
        return 2
    cfail = check_contract_frames(cc)
    fwfail = check_fw_version_consistency()
    hfail = check_cccd_handle()
    sfail = check_sub_decide(cc)
    ssfail = check_sub_state(cc)
    pafail = check_poll_accounting(cc)
    upfail = check_uplink_forward(cc)
    pffail = check_pump_forwards()
    txfail = check_uart_tx_path()
    blfail = check_board_log(board_log, node_log)
    total_bad = ((1 if rc != 0 else 0) + cfail + fwfail + hfail + sfail + ssfail
                 + pafail + upfail + pffail + txfail + blfail)
    print("\n==== hub-selftest 总判定：%s ====" % ("PASS" if total_bad == 0 else "FAIL"))
    print("日志：ws63-firmware/out/hub-selftest.log；mock 聚合证据：ws63-firmware/out/hub-mock-agg.log")
    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
