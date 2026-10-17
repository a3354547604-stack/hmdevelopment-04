#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""node-compile-check.py -- 不开 Docker 的节点固件编译自检（秒级）

为什么需要它
    一次容器全量编译要好几分钟，而节点固件改一行就得知道「编不编得过」。
    这个脚本把 SDK 上次构建留下的 compile_commands.json 当“官方编译参数表”，
    借用 SDK 自己的 include 路径 / 宏 / 参数，用 SDK 自带的 riscv32-linux-musl-gcc
    对每个目标（m1/m2/m4/mock）的**确切文件清单**真编译出 .o（落在临时目录，
    不进仓库）。文件清单来自 ws63-firmware/scripts/node-targets.txt —— 与
    node-build.sh 同源，避免「脚本里的列表和真正编进去的不一致」。

    血泪教训（与 tools/spike-compile-check.py 相同）：必须真出 .o，不能只用
    -fsyntax-only —— 那个只跑编译器前端，中端警告（SDK 开了 -Werror，典型是
    -Werror=unused-function）一个都不报。t8 就是因此漏掉「回调定义了但没注册」。

它不能替代什么
    这里只查编译。镜像里真的有这段代码、广播真的起来了、通知真的发出去了，
    都只能靠容器构建 + 上板实测（node-build.sh 与施工单负责）。

用法
    python3 tools/node-compile-check.py              # 四个目标全查
    python3 tools/node-compile-check.py m2 mock      # 只查指定目标
    SDK_DIR=~/ws63/fbb_ws63-master python3 tools/node-compile-check.py
    退出码 0 = 全过；1 = 有文件没通过（输出里带 FAIL）；2 = 环境/清单不满足
"""

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SRC = os.path.join(REPO, "ws63-firmware", "src")
TARGETS = os.path.join(REPO, "ws63-firmware", "scripts", "node-targets.txt")

SDK = os.environ.get("SDK_DIR", os.path.expanduser("~/ws63/fbb_ws63-master"))
CCJ = os.path.join(SDK, "src/output/ws63/acore/ws63-liteos-app/compile_commands.json")

DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ", "-MD", "-MMD", "-MP"}
# 模板条目是“上一次构建”留下的，里面烤进了上一次目标的按文件宏，必须丢掉自己补
# 汇聚目标（hub/hubmock）由 t3 加入同一份清单，因此 -DHUB_MOCK= 也要丢：
# 模板条目是「上一次构建」的，若上次编的是 hubmock，模板里就烤着 -DHUB_MOCK=1，
# 再叠上目标自己的 -DHUB_MOCK=0 会得到「重复定义」警告（值取后来的，但日志很脏）。
DROP_DEFINE_PREFIX = ("-DTHIS_FILE_ID=", "-DSPIKE_ROLE=", "-DNODE_ROLE=", "-DNODE_MOCK=",
                      "-DNODE_LEGACY_SPEED_TEST=", "-DHUB_MOCK=")


def read_targets():
    """读 node-targets.txt → [(name, [build args], [src files])]"""
    out = []
    with open(TARGETS, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = [p.strip() for p in line.split("|")]
            if len(parts) != 3:
                raise ValueError("清单行格式应为「目标名 | 参数 | 文件」，实际：%s" % line)
            out.append((parts[0], parts[1].split(), parts[2].split()))
    return out


def defs_from_build_args(args):
    """-def=A=1,B=2 → ['-DA=1', '-DB=2']"""
    res = []
    for a in args:
        if not a.startswith("-def="):
            continue
        for kv in a[len("-def="):].split(","):
            kv = kv.strip()
            if kv:
                res.append("-D" + kv)
    return res


def find_reference_entry(entries):
    for e in entries:
        if "/samples/custom/" in e.get("file", ""):
            return e
    for e in entries:
        if "ws63-liteos-app" in e.get("file", ""):
            return e
    return None


def relocalize(arg):
    """把 /sdk/ 换成本机 SDK 路径，只换第一处。

    '-I/sdk/src/protocol/bt/host/bt/sdk/common' 里第二个 /sdk/ 是目录名的一部分，
    str.replace 会一起换掉、产生不存在的 -I 路径（t8 踩过）。
    """
    idx = arg.find("/sdk/")
    if idx < 0:
        return arg
    return arg[:idx] + SDK.rstrip("/") + "/" + arg[idx + len("/sdk/"):]


def build_template(command):
    args = shlex.split(command)
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in DROP_WITH_ARG:
            i += 1
        elif a.endswith(".c"):
            pass
        elif a.startswith(DROP_DEFINE_PREFIX):
            pass
        else:
            out.append(relocalize(a))
        i += 1
    if "-c" not in out:
        out.append("-c")
    out.append("-Wall")
    return out


def this_file_id(src):
    return "-DTHIS_FILE_ID=" + os.path.basename(src).upper().replace(".", "_")


def brief_command(cmd):
    shown = []
    dropped = 0
    for a in cmd:
        if a.startswith("-I") or a.startswith("-D"):
            dropped += 1
        else:
            shown.append(a)
    return " ".join(shlex.quote(a) for a in shown) + \
        ("  (+%d 个 -I/-D，见 compile_commands.json)" % dropped)


def main():
    if not os.path.isfile(CCJ):
        print("找不到 %s" % CCJ)
        print("先按 ws63-firmware/doc/1-通宵进展与烧录方法.md 在容器里成功构建一次。")
        return 2
    try:
        targets = read_targets()
    except (OSError, ValueError) as exc:
        print("读目标清单失败：%s" % exc)
        return 2

    want = sys.argv[1:]
    if want:
        names = [t[0] for t in targets]
        bad = [w for w in want if w not in names]
        if bad:
            print("未知目标 %s；可选：%s" % (" ".join(bad), " ".join(names)))
            return 2
        targets = [t for t in targets if t[0] in want]

    with open(CCJ, encoding="utf-8") as fh:
        entries = json.load(fh)
    ref = find_reference_entry(entries)
    if ref is None:
        print("compile_commands.json 里没有可用的 ws63-liteos-app 条目")
        return 2
    template = build_template(ref["command"])
    print("模板条目：%s" % ref["file"])
    print("SDK：%s" % SDK)

    total = 0
    fails = []
    tmpdir = tempfile.mkdtemp(prefix="node-cc-")
    try:
        for name, args, files in targets:
            defs = defs_from_build_args(args)
            print("\n=== 目标 %s（%s）===" % (name, " ".join(defs) or "无额外宏"))
            for src in files:
                path = os.path.join(SRC, src)
                total += 1
                if not os.path.isfile(path):
                    fails.append((name, src))
                    print("FAIL  [%s] %s —— 文件不存在（清单与实际源码不一致）" % (name, src))
                    continue
                obj = os.path.join(tmpdir, "%s-%s.o" % (name, src))
                cmd = list(template) + defs + [this_file_id(src), "-o", obj, path]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode == 0:
                    warn = r.stderr.strip()
                    print("OK    [%s] %s%s" % (name, src, "  (有警告，见下)" if warn else ""))
                    if warn:
                        print("      " + warn.replace("\n", "\n      ")[-2000:])
                else:
                    fails.append((name, src))
                    print("FAIL  [%s] %s" % (name, src))
                    print((r.stdout + r.stderr)[-4000:])
                    print("复现：\n  %s\n" % brief_command(cmd))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("\n共 %d 个（目标 × 文件）编译，失败 %d 个" % (total, len(fails)))
    if fails:
        print("失败清单：" + " ".join("%s/%s" % t for t in fails))
        return 1
    print("编译自检通过（含 SDK 的 -Werror 级别警告）。出镜像仍须 ws63-firmware/scripts/node-build.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
