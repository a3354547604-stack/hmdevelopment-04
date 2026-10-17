#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""spike-compile-check.py -- 不开 Docker 的冒烟固件编译自检（秒级）

为什么需要它
    冒烟固件一改就要验证，而 ws63wsl 容器里一次全量编译要几分钟。
    这个脚本把 SDK 上次构建留下的 compile_commands.json 当成“官方编译参数表”：
    借用 SDK 自己的头文件路径 / 宏定义 / 参数，换成本地工具链对我们的
    spike_*.c 做真正编译（出 .o，落在临时目录，不进仓库）。拼写错误、结构体
    成员写错、漏头文件、函数签名对不上、定义了却没用的函数，都会在 1 秒内
    暴露，不用等容器。

    血泪教训：这里必须真出 .o，不能用 -fsyntax-only。-fsyntax-only 只跑编译器
    前端，中端的警告（典型是 -Werror=unused-function，SDK 是开 -Werror 的）
    一个都不报——第一版就是这么漏掉「回调函数定义了但忘了注册」的 bug：
    主机上全绿，容器里直接编译失败。

它不能替代什么
    这里只查编译，不产出也不校验镜像。链路对不对（能不能连上、能不能收到
    通知）只能上板实测；“两套固件真的编得出来”仍必须用 tools/spike-build.sh
    在容器里跑一遍，并检查 fwpkg 里的角色标记。

前提
    1) SDK 至少成功全量构建过一次（否则没有 compile_commands.json）
    2) 主机上能直接调用 SDK 里的 riscv32-linux-musl-gcc
       （在 SDK 目录下 `find . -name 'riscv32-linux-musl-gcc'` 找）

用法
    python3 tools/spike-compile-check.py                # 两种角色都查
    python3 tools/spike-compile-check.py server         # 只查主节点
    python3 tools/spike-compile-check.py client
    SDK_DIR=~/ws63/fbb_ws63-master python3 tools/spike-compile-check.py
    退出码 0 = 全过；1 = 有文件没通过（输出里带 FAIL）；2 = 环境不满足前提
"""

import glob
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SPIKE = os.path.join(REPO, "ws63-firmware", "spike")

SDK = os.environ.get("SDK_DIR", os.path.expanduser("~/ws63/fbb_ws63-master"))
CCJ = os.path.join(SDK, "src/output/ws63/acore/ws63-liteos-app/compile_commands.json")

# 这些参数是为“写在仓库里的 .o + 依赖文件”准备的，我们必须丢掉：
# 依赖文件会污染仓库，-o 由脚本自己指定到临时目录
DROP_WITH_ARG = {"-o", "-MF", "-MT", "-MQ", "-MD", "-MMD", "-MP"}

ROLE_DEFINE = {"server": "-DSPIKE_ROLE=1", "client": "-DSPIKE_ROLE=2"}


def find_reference_entry(entries):
    """挑一条 custom 组件的编译命令当模板（文件名随意）。"""
    for e in entries:
        if "/samples/custom/" in e.get("file", ""):
            return e
    for e in entries:
        if "ws63-liteos-app" in e.get("file", ""):
            return e
    return None


def relocalize(arg):
    """把参数里的 /sdk/ 前缀还原成本机 SDK 路径。

    只换第一处：'-I/sdk/src/protocol/bt/host/bt/sdk/common' 里第二个 /sdk/ 是
    目录名的一部分，str.replace 会把它一起换掉，结果是一个不存在的 -I 路径
    ——真踩过，主机上就会报出容器里没有的错。
    """
    idx = arg.find("/sdk/")
    if idx < 0:
        return arg
    return arg[:idx] + SDK.rstrip("/") + "/" + arg[idx + len("/sdk/"):]


def build_template(command):
    """把模板命令改写成本机可执行的参数表：SDK 路径还原 + 去掉输出/依赖参数。"""
    args = shlex.split(command)
    out = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in DROP_WITH_ARG:
            i += 1  # 连同它的参数一起丢掉
        elif a.endswith(".c"):
            pass    # 丢掉模板自己的源文件，稍后换成 spike_*.c
        elif a.startswith("-DTHIS_FILE_ID="):
            pass    # 这个宏 SDK 是“按文件”生成的，稍后按实际源文件名补回来
        elif a.startswith("-DSPIKE_ROLE="):
            pass    # 模板条目里已经烤进了上一次构建的角色，我们的角色自己传
        else:
            out.append(relocalize(a))
        i += 1
    if "-c" not in out:
        out.append("-c")
    out.append("-Wall")
    return out


def this_file_id(src):
    """SDK 给每个 .c 生成的 THIS_FILE_ID 形如 SPIKE_SERVER_C。"""
    return "-DTHIS_FILE_ID=" + os.path.basename(src).upper().replace(".", "_")


def brief_command(cmd):
    """失败时打印的命令：-I/-D 有几百个，全打出来没人看，只留有用的部分。"""
    shown = []
    dropped = 0
    for a in cmd:
        if a.startswith("-I") or a.startswith("-D"):
            dropped += 1
        else:
            shown.append(a)
    return " ".join(shlex.quote(a) for a in shown) + ("  (+%d 个 -I/-D，见 compile_commands.json)" % dropped)


def main():
    roles = sys.argv[1:] or ["server", "client"]
    bad_roles = [r for r in roles if r not in ROLE_DEFINE]
    if bad_roles:
        print("角色只能是 server / client，收到：%s" % " ".join(bad_roles))
        return 2

    if not os.path.isfile(CCJ):
        print("找不到 %s" % CCJ)
        print("先按 ws63-firmware/doc/1-通宵进展与烧录方法.md 在容器里成功构建一次。")
        return 2

    with open(CCJ, encoding="utf-8") as fh:
        entries = json.load(fh)
    ref = find_reference_entry(entries)
    if ref is None:
        print("compile_commands.json 里没有可用的 ws63-liteos-app 条目")
        return 2
    template = build_template(ref["command"])
    print("模板条目：%s" % ref["file"])
    print("SDK：%s" % SDK)

    files = sorted(glob.glob(os.path.join(SPIKE, "spike_*.c")))
    if not files:
        print("没找到 %s/spike_*.c" % SPIKE)
        return 2

    tmpdir = tempfile.mkdtemp(prefix="spike-cc-")
    fails = []
    try:
        for role in roles:
            for src in files:
                obj = os.path.join(tmpdir, "%s-%s.o" % (role, os.path.basename(src)))
                cmd = list(template) + [ROLE_DEFINE[role], this_file_id(src), "-o", obj, src]
                r = subprocess.run(cmd, capture_output=True, text=True)
                tag = os.path.basename(src)
                if r.returncode == 0:
                    warn = r.stderr.strip()
                    print("OK    [%s] %s%s" % (role, tag, "  (有警告，看下面)" if warn else ""))
                    if warn:
                        print("      " + warn.replace("\n", "\n      ")[-2000:])
                else:
                    fails.append((role, tag))
                    print("FAIL  [%s] %s" % (role, tag))
                    print((r.stdout + r.stderr)[-4000:])
                    print("复现：\n  %s\n" % brief_command(cmd))
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("共 %d 个源文件 × %d 个角色，失败 %d 个" % (len(files), len(roles), len(fails)))
    if fails:
        print("失败清单：" + " ".join("%s/%s" % t for t in fails))
        return 1
    print("编译自检通过（含 SDK 的 -Werror 级别警告）。出镜像仍须 tools/spike-build.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
