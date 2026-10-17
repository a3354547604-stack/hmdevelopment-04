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
    total_bad = (1 if rc != 0 else 0) + cfail + fwfail
    print("\n==== hub-selftest 总判定：%s ====" % ("PASS" if total_bad == 0 else "FAIL"))
    print("日志：ws63-firmware/out/hub-selftest.log；mock 聚合证据：ws63-firmware/out/hub-mock-agg.log")
    return 0 if total_bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
