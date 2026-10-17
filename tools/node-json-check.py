#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
node-json-check.py — 节点上行 JSON 的独立校验（主机侧，不需要硬件/SDK）

做三件事：
  1. 用主机 gcc 编译 tools/node-json-selftest.c + ws63-firmware/src/node_json.c（-std=c99 -Wall -Wextra -Werror）
  2. 跑它；自测失败即整体失败
  3. 把它打印的每一帧用 Python 的 json 解析器**真正解析一遍**，再逐条核对契约 v1.1：
     §2.2 信封键序与类型、§0.2 ts 13 位毫秒、§0.1 sn 字符集、§2.4 各 type 的必填/选填字段与量程、
     §2.1 单条 ≤512 字节、§0.3 一位小数精度

为什么要有第 3 步：C 代码只能断言「我期待的字符串出现了」，断不了「这串东西是不是合法 JSON」。
grep/子串匹配在 JSON 上非常容易假绿（少一个逗号、多一个引号都照样能匹配到子串）。
这里用真正的解析器，任何语法错误会直接抛异常。

退出码：0 = 全通过；1 = 有失败；2 = 环境缺编译器 / 源文件。

环境说明：主机 WSL 没有装 gcc（2026-10-17 实测 `gcc: command not found`），所以脚本会自动退回
到项目既有的 ws63wsl:latest 镜像里编译并运行 —— 那里有 gcc-11。两边的编译参数完全一致，
被测的是同一份 node_json.c。容器名带 hmprogram- 前缀（AGENTS.md 的容器命名规则）。
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
SELFTEST = os.path.join(ROOT, "tools", "node-json-selftest.c")
IMAGE = os.environ.get("WS63_IMAGE", "ws63wsl:latest")
# 主机与镜像里可用的 C 编译器（Ubuntu 20.04 镜像只有版本化的 gcc-11，没有无后缀 gcc）
CC_CANDIDATES = ["gcc", "gcc-11", "cc", "clang"]

# ---- 契约 v1.1 §2.4：每个 type 的字段表（必填 / 选填 / 类型 / 量程） ----
# 类型标记：i=int, f=一位小数, b=bool, s=str, i64=int64, ilist=int 数组
SPEC = {
    "gas": {
        "required": {"smoke": "i"},
        "optional": {"temp": "f", "humi": "f", "lpg": "i", "co": "i", "adc_mv": "ilist", "warmup": "b"},
        # 注意：§2.4.1 把 temp/humi 标为「必填」，故这里把它们移到 required（见下 _fix）
    },
    "fall": {
        "required": {"fall": "i", "presence": "i"},
        "optional": {"src": "s", "dist": "f"},
    },
    "door": {
        "required": {"door": "b", "window": "b"},
        "optional": {"ch": "i"},
    },
    "presence": {
        "required": {"presence": "b", "last_motion_ts": "i64"},
        "optional": {"count": "i", "ch": "i"},
    },
    "status": {
        "required": {"uptime_s": "i", "power_src": "s"},
        "optional": {"rssi": "i", "battery": "i", "err_cnt": "i", "warmup": "b"},
    },
}
# §2.4.1 表里 temp/humi 的「必填」列是「是」
SPEC["gas"]["required"].update({"temp": "f", "humi": "f"})

RANGES = {
    "smoke": (0, 5000), "lpg": (0, 5000), "co": (0, 5000),
    "temp": (-400, 1250), "humi": (0, 1000),          # 以 0.1 为单位的整数校验
    "dist": (0, 600),                                  # 0.0–6.0 m → 0–60 个 0.1
    "rssi": (-120, 0), "uptime_s": (0, None), "err_cnt": (0, None),
    "count": (0, 65535), "ch": (0, 7),
}
ENVELOPE_ORDER = ["sn", "ts", "type", "payload", "seq", "req_id", "fw"]
ENVELOPE_REQUIRED = ["sn", "ts", "type", "payload"]
SN_RE = re.compile(r"^[A-Z][A-Z0-9]{0,7}$")
MAX_LEN = 512

fails = []
checked = 0


def bad(msg):
    fails.append(msg)


def check_frame(name, raw):
    """raw 是 C 侧打印的那一行 JSON（不含变量名）。"""
    global checked
    checked += 1

    nbytes = len(raw.encode("utf-8"))
    if nbytes > MAX_LEN:
        bad(f"[{name}] 长度 {nbytes} > §2.1 上限 {MAX_LEN}")

    try:
        obj = json.loads(raw)
    except Exception as e:                                    # noqa: BLE001
        bad(f"[{name}] 不是合法 JSON: {e}\n    {raw}")
        return None

    if not isinstance(obj, dict):
        bad(f"[{name}] 顶层不是对象")
        return None

    # --- 信封：键序（§2.2 表顺序）与必填 ---
    keys = list(obj.keys())
    if keys[:5] != ENVELOPE_ORDER[:5] and keys[:4] != ENVELOPE_REQUIRED:
        bad(f"[{name}] 信封键序不对: {keys}")
    expect_order = [k for k in ENVELOPE_ORDER if k in obj]
    if keys != expect_order:
        bad(f"[{name}] 键序与 §2.2 不一致: {keys} 应为 {expect_order}")
    for k in ENVELOPE_REQUIRED:
        if k not in obj:
            bad(f"[{name}] 缺必填字段 {k}")
    for k in keys:
        if k not in ENVELOPE_ORDER:
            bad(f"[{name}] 出现契约外字段 {k!r}")

    if not SN_RE.match(str(obj.get("sn", ""))):
        bad(f"[{name}] sn 不符合 §0.1: {obj.get('sn')!r}")
    ts = obj.get("ts")
    if not isinstance(ts, int) or isinstance(ts, bool):
        bad(f"[{name}] ts 必须是整数: {ts!r}")
    elif len(str(ts)) != 13:
        bad(f"[{name}] ts 不是 13 位毫秒: {ts}")
    if "seq" in obj and (not isinstance(obj["seq"], int) or not 0 <= obj["seq"] <= 65535):
        bad(f"[{name}] seq 越界: {obj['seq']!r}")
    if "req_id" in obj and not isinstance(obj["req_id"], int):
        bad(f"[{name}] req_id 必须是整数")

    # --- payload：§2.4 ---
    t = obj.get("type")
    if t not in SPEC:
        bad(f"[{name}] type 不在 §2.3 表内: {t!r}")
        return obj
    spec = SPEC[t]
    pl = obj.get("payload")
    if not isinstance(pl, dict):
        bad(f"[{name}] payload 不是对象")
        return obj

    for k, kind in spec["required"].items():
        if k not in pl:
            bad(f"[{name}] {t} 缺必填字段 {k}")
    for k, v in pl.items():
        if k not in spec["required"] and k not in spec["optional"]:
            bad(f"[{name}] {t} 出现契约外字段 {k!r}")
            continue
        kind = spec["required"].get(k) or spec["optional"][k]
        if kind == "i":
            if not isinstance(v, int) or isinstance(v, bool):
                bad(f"[{name}] {k} 应为 int，实为 {type(v).__name__}: {v!r}")
        elif kind == "i64":
            if not isinstance(v, int) or isinstance(v, bool) or v < 0:
                bad(f"[{name}] {k} 应为 ≥0 的 int64: {v!r}")
        elif kind == "b":
            if not isinstance(v, bool):
                bad(f"[{name}] {k} 应为 bool，实为 {type(v).__name__}: {v!r}")
        elif kind == "s":
            if not isinstance(v, str):
                bad(f"[{name}] {k} 应为 string: {v!r}")
        elif kind == "f":
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                bad(f"[{name}] {k} 应为数字: {v!r}")
            elif round(float(v) * 10) != float(v) * 10:
                bad(f"[{name}] {k}={v} 超过 §0.3 的 1 位小数精度")
        elif kind == "ilist":
            if not isinstance(v, list) or not all(isinstance(x, int) and not isinstance(x, bool) for x in v):
                bad(f"[{name}] {k} 应为 int 数组: {v!r}")
            elif len(v) < 1:
                bad(f"[{name}] {k} 数组长度至少 1")

    # --- 量程（§2.4 的「范围」列） ---
    for k, (lo, hi) in RANGES.items():
        if k not in pl:
            continue
        v = pl[k]
        scaled = int(round(float(v) * 10)) if k in ("temp", "humi") else int(v)
        if lo is not None and scaled < lo:
            bad(f"[{name}] {k}={v} 低于下限")
        if hi is not None and scaled > hi:
            bad(f"[{name}] {k}={v} 高于上限")
    if pl.get("battery") not in (None, 255) and not 0 <= pl["battery"] <= 100:
        bad(f"[{name}] battery 必须 0–100 或 255: {pl['battery']!r}")
    if "src" in pl and pl["src"] not in ("uart", "tx2"):
        bad(f"[{name}] src 只能 uart/tx2: {pl['src']!r}")
    return obj


def find_cc():
    for c in CC_CANDIDATES:
        p = shutil.which(c)
        if p:
            return c
    return None


def docker_available():
    if shutil.which("docker") is None:
        return False
    r = subprocess.run(["docker", "image", "inspect", IMAGE], capture_output=True, text=True)
    return r.returncode == 0


def run_inside_docker(extra=()):
    """把整份自测搬进容器跑：主机没有 gcc 时的唯一出路（镜像里有 gcc-11）。"""
    name = f"hmprogram-nodejson-{os.getpid()}"
    cmd = [
        "docker", "run", "--rm", "--name", name,
        "-v", f"{ROOT}:/w", "-w", "/w",
        IMAGE, "python3", "tools/node-json-check.py", "--inside-docker", *extra,
    ]
    print("$ " + " ".join(cmd))
    return subprocess.call(cmd)


def run_sample(cc):
    """--sample：编译 tools/node-json-sample.c 并打印节点上行样例（doc 11 的证据来源）。"""
    sample = os.path.join(ROOT, "tools", "node-json-sample.c")
    for p in (sample, os.path.join(SRC, "node_json.c")):
        if not os.path.isfile(p):
            print(f"缺源文件 {p}", file=sys.stderr)
            return 2
    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "node-json-sample")
        cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1",
               "-I", SRC, "-o", exe, sample, os.path.join(SRC, "node_json.c")]
        print("$ " + " ".join(cmd), file=sys.stderr)
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode != 0:
            print(cp.stdout)
            print(cp.stderr, file=sys.stderr)
            print("样例程序编译失败", file=sys.stderr)
            return 1
        rp = subprocess.run([exe] + [a for a in sys.argv[1:] if a in ("m1", "m2", "m4")],
                            capture_output=True, text=True)
        print(rp.stdout, end="")
        if rp.stderr.strip():
            print(rp.stderr, file=sys.stderr)
        return rp.returncode


def main():
    in_docker = "--inside-docker" in sys.argv
    sample_mode = "--sample" in sys.argv
    extra = ["--sample"] if sample_mode else []
    cc = find_cc()
    if cc is None:
        if in_docker:
            print("容器里也没有找到 C 编译器（试过 %s）" % ", ".join(CC_CANDIDATES), file=sys.stderr)
            return 2
        if docker_available():
            print(f"主机无 C 编译器 → 退回容器 {IMAGE} 执行（等价编译参数）")
            return run_inside_docker(extra)
        print("环境缺 gcc 且没有可用的 docker 镜像 %s" % IMAGE, file=sys.stderr)
        return 2
    print(f"使用编译器: {cc}")
    if sample_mode:
        return run_sample(cc)

    for p in (SELFTEST, os.path.join(SRC, "node_json.c")):
        if not os.path.isfile(p):
            print(f"缺源文件 {p}", file=sys.stderr)
            return 2

    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "node-json-selftest")
        cmd = [
            cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1",
            "-I", SRC, "-o", exe, SELFTEST, os.path.join(SRC, "node_json.c"),
        ]
        print("$ " + " ".join(cmd))
        cp = subprocess.run(cmd, capture_output=True, text=True)
        if cp.returncode != 0:
            print(cp.stdout)
            print(cp.stderr, file=sys.stderr)
            print("编译失败", file=sys.stderr)
            return 1

        rp = subprocess.run([exe], capture_output=True, text=True)
        out = rp.stdout
        print(out, end="")
        if rp.stderr.strip():
            print(rp.stderr, file=sys.stderr)
        if rp.returncode != 0 or "SELFTEST OK" not in out:
            print("C 侧自测失败", file=sys.stderr)
            return 1

    # ---- 逐帧用真 JSON 解析器复检 ----
    frames = []
    lines = out.splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith("FRAME ") and i + 1 < len(lines):
            frames.append((ln[len("FRAME "):], lines[i + 1]))
    if not frames:
        print("没有从自测里取到任何帧", file=sys.stderr)
        return 1

    for name, raw in frames:
        if name in ("set_ack", "nack"):
            try:
                o = json.loads(raw)
            except Exception as e:                            # noqa: BLE001
                bad(f"[{name}] 不是合法 JSON: {e}")
                continue
            if name == "set_ack" and (o.get("cmd") != "set_ack" or "key" not in o or "result" not in o):
                bad(f"[set_ack] 字段不符 §2.5: {o}")
            if name == "nack" and (o.get("cmd") != "nack" or "reason" not in o):
                bad(f"[nack] 字段不符 §2.5: {o}")
            continue
        check_frame(name, raw)

    print(f"\n用 JSON 解析器复检了 {len(frames)} 帧，字段级断言 {checked} 条")
    if fails:
        print(f"\n❌ 失败 {len(fails)} 条：")
        for f in fails:
            print("   - " + f)
        return 1
    print("✅ 全部通过：信封键序/类型、§2.4 必填与量程、§0.1 sn、§0.2 ts、§0.3 精度、§2.1 长度")
    return 0


if __name__ == "__main__":
    sys.exit(main())
