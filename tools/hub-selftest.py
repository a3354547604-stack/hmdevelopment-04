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
    if "sub-confirmed by first frame" not in text:
        print("  FAIL  没有 `sub-confirmed by first frame` 日志 —— 上板时无法确认订阅何时生效")
        fails += 1

    if fails == 0:
        print("  PASS  handle=c->prop_handle / type=SSAP_DESCRIPTOR_CLIENT_CONFIGURATION(0x02)"
              " / 判据不看 cfm / 判定集中且强制 READY / 首帧确认")
    return fails


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


def main():
    in_docker = "--inside-docker" in sys.argv
    cc = find_cc()
    if cc is None:
        if in_docker:
            print("容器里也没有 C 编译器（试过 %s）" % ", ".join(CC_CANDIDATES), file=sys.stderr)
            return 2
        if shutil.which("docker") is None:
            print("主机与容器都没有 C 编译器，无法自测", file=sys.stderr)
            return 2
        return run_inside_docker()

    rc = compile_and_run(cc)
    if rc == 2:
        return 2
    cfail = check_contract_frames(cc)
    fwfail = check_fw_version_consistency()
    hfail = check_cccd_handle()
    sfail = check_sub_decide(cc)
    total_bad = (1 if rc != 0 else 0) + cfail + fwfail + hfail + sfail
    print("\n==== hub-selftest 总判定：%s ====" % ("PASS" if total_bad == 0 else "FAIL"))
    print("日志：ws63-firmware/out/hub-selftest.log；mock 聚合证据：ws63-firmware/out/hub-mock-agg.log")
    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
