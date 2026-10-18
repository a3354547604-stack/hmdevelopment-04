#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
node-radar-cache-check.py — M2 跌倒雷达「采样缓存」的独立校验（主机侧，不需要硬件/SDK）

t32 修的缺陷：node_sensor_radar.c 的采样缓存只被读、从没被写，于是「未到采样点」的那些
节拍永远回 0。采样 2 s、轮询 3 s，最小公倍数 6 s，**每 2 次 poll 必有 1 次**落在两次采样
之间 —— 这 1 次凭空造出「跌倒已恢复」的样本，正好喂给契约 §8.4 的「连续 K=3 次 fall=0
且 ≥30 s」退出计数，把仍躺在地上的老人判成已恢复。

本脚本做四件事：
  1. 生成两个最小替身头（soc_osal.h / tcxo.h）到临时目录，好让真实源码能在主机上编
  2. 编译 tools/node-radar-cache-selftest.c + **真实** ws63-firmware/src/node_sensor_radar.c
     + node_json.c（-std=c99 -Wall -Wextra -Werror）
  3. 跑 4 个场景，每个场景独立进程；把它打印的 fall 帧用 Python 的 json 解析器**真正解析**
     一遍，再按契约 §2.2/§2.4.2 逐字段核对（不是子串匹配 —— JSON 上子串匹配太容易假绿）
  4. **变异检验（断言有牙的反例证明）**：把真实源码里回填缓存的那两行删掉，编译成变异体，
     断言同一套自测在变异体上**必须失败**，且失败形态是「错报 fall=0 的次数 == 采样间隙
     poll 次数」。这一步才证明第 3 步的绿色不是「测了个寂寞」。

退出码：0 = 全通过；1 = 有失败；2 = 环境缺编译器 / 源文件。

环境说明：主机 WSL 没有装 gcc（2026-10-17 实测），所以自动退回既有 ws63wsl:latest 镜像里
编译运行 —— 那里有 gcc-11。两边编译参数完全一致，被测的是同一份源码。
容器名带 hmprogram- 前缀（AGENTS.md 的容器命名规则）。
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
SELFTEST = os.path.join(ROOT, "tools", "node-radar-cache-selftest.c")
RADAR_C = os.path.join(SRC, "node_sensor_radar.c")
IMAGE = os.environ.get("WS63_IMAGE", "ws63wsl:latest")
CC_CANDIDATES = ["gcc", "gcc-11", "cc", "clang"]

# 被测节点是 M2（卧室雷达），故按 M2 编：这样帧里的 sn 应当就是 "M2"
EXTRA_DEFS = ["-DNODE_ROLE=2"]

SCENARIOS = ["sustained", "boot", "silent", "demo"]

# t32 的回填语句。变异检验要求它们在真实源文件里各出现**恰好一次**，
# 否则说明源文件结构变了、这个检验已经失效 —— 那时应当报错而不是假装通过。
BACKFILL_LINES = ["g_cache_fall = fall;", "g_cache_pres = pres;"]

# ---- 契约 §2.2 信封 + §2.4.2 fall payload ----
ENVELOPE_ORDER = ["sn", "ts", "type", "payload", "seq", "req_id", "fw"]
ENVELOPE_REQUIRED = ["sn", "ts", "type", "payload"]
SN_RE = re.compile(r"^[A-Z][A-Z0-9]{0,7}$")
MAX_LEN = 512
# §2.4.2：fall 是 int 0/1（不是 bool），src 只能 uart/tx2，dist 单位为 0.1 m。
# presence 在 P0 **整键不出现**（PM 2026-10-18 裁定，t23）—— 所以它是 "absent" 不是 required，
# 而且是**禁止出现**：写 0 等于替硬件断言「确认无人」。开关见 node_cfg.h NODE_PRESENCE_ENABLE。
FALL_SPEC = {"required": {"fall": "i"}, "optional": {"src": "s", "dist": "f"},
             "absent": ["presence"]}
SRC_VALUES = ("uart", "tx2")

# 每个 tag 期望的 fall。tag 由**脚本模型**（不是被测代码报出来的值）决定，
# 否则代码错报时 tag 会跟着变、断言就成了自我实现。
# poll-lagging：模型说已起床、但节点还没采到的那一拍，报 1 是正确的（采样分辨率 2 s）。
# poll-normal：跌倒还没发生的那几拍，必须报 0。
TAG_EXPECT = {
    "gap-poll-fall": 1,
    "poll-normal": 0,
    "poll-lagging": 1,
    "poll-recovered": 0,
    "poll-boot": 0,
    "poll-unknown": 0,
    "poll-demo-gap": 1,
    "poll-demo-expired": 0,
}

STUB_SOC_OSAL = """/* 替身（由 tools/node-radar-cache-check.py 生成）：只提供被测源码用到的那一个符号。
 * 真机上是 SDK 的 soc_osal.h；这里不引入任何 SDK 依赖，好让真实驱动源码能在主机上编。 */
#ifndef STUB_SOC_OSAL_H
#define STUB_SOC_OSAL_H

void osal_printk(const char *fmt, ...);

#endif
"""

STUB_TCXO = """/* 替身（由 tools/node-radar-cache-check.py 生成）：被测源码只用 uapi_tcxo_get_ms()，
 * 由自测按脚本化时间线给值，于是整条采样路径在主机上是确定性的。 */
#ifndef STUB_TCXO_H
#define STUB_TCXO_H

#include <stdint.h>

uint32_t uapi_tcxo_get_ms(void);

#endif
"""

fails = []
checked = 0


def bad(msg):
    fails.append(msg)


def write_stubs(incdir):
    os.makedirs(incdir, exist_ok=True)
    with open(os.path.join(incdir, "soc_osal.h"), "w", encoding="utf-8") as f:
        f.write(STUB_SOC_OSAL)
    with open(os.path.join(incdir, "tcxo.h"), "w", encoding="utf-8") as f:
        f.write(STUB_TCXO)


def compile_one(cc, incdir, exe, radar_source, extra=()):
    """编译自测 + 给定的 node_sensor_radar.c（真实源码或变异体）+ node_json.c。"""
    cmd = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1",
           "-I", SRC, "-I", incdir, *EXTRA_DEFS, *extra,
           "-o", exe, SELFTEST, radar_source, os.path.join(SRC, "node_json.c")]
    print("$ " + " ".join(cmd))
    cp = subprocess.run(cmd, capture_output=True, text=True)
    if cp.returncode != 0:
        print(cp.stdout)
        print(cp.stderr, file=sys.stderr)
        bad("编译失败：" + " ".join(cmd))
        return False
    return True


def run_scenario(exe, name):
    rp = subprocess.run([exe, name], capture_output=True, text=True)
    out = rp.stdout
    print(out, end="")
    if rp.stderr.strip():
        print(rp.stderr, file=sys.stderr)
    return rp.returncode, out


def check_fall_frame(tag, raw):
    """用真 JSON 解析器复检一帧 fall 上行报文（§2.2 信封 + §2.4.2 payload）。"""
    global checked
    checked += 1

    nbytes = len(raw.encode("utf-8"))
    if nbytes > MAX_LEN:
        bad(f"[{tag}] 长度 {nbytes} > §2.1 上限 {MAX_LEN}")

    try:
        obj = json.loads(raw)
    except Exception as e:                                     # noqa: BLE001
        bad(f"[{tag}] 不是合法 JSON: {e}\n    {raw}")
        return
    if not isinstance(obj, dict):
        bad(f"[{tag}] 顶层不是对象")
        return

    keys = list(obj.keys())
    expect = [k for k in ENVELOPE_ORDER if k in obj]
    if keys != expect:
        bad(f"[{tag}] 信封键序与 §2.2 不一致: {keys} 应为 {expect}")
    for k in ENVELOPE_REQUIRED:
        if k not in obj:
            bad(f"[{tag}] 缺必填字段 {k}")
    for k in keys:
        if k not in ENVELOPE_ORDER:
            bad(f"[{tag}] 出现契约外字段 {k!r}")

    if obj.get("sn") != "M2" or not SN_RE.match(str(obj.get("sn", ""))):
        bad(f"[{tag}] sn 应为 M2（§2.7），实为 {obj.get('sn')!r}")
    if obj.get("type") != "fall":
        bad(f"[{tag}] type 应为 fall，实为 {obj.get('type')!r}")
    ts = obj.get("ts")
    if not isinstance(ts, int) or isinstance(ts, bool) or len(str(ts)) != 13:
        bad(f"[{tag}] ts 不是 13 位毫秒（§0.2）: {ts!r}")
    if "seq" in obj and (not isinstance(obj["seq"], int) or not 0 <= obj["seq"] <= 65535):
        bad(f"[{tag}] seq 越界: {obj['seq']!r}")

    pl = obj.get("payload")
    if not isinstance(pl, dict):
        bad(f"[{tag}] payload 不是对象")
        return
    for k in FALL_SPEC["required"]:
        if k not in pl:
            bad(f"[{tag}] fall 缺必填字段 {k}")
    # 「禁止出现」的键（t23：presence 在 P0 整键不出现）。单独一条消息，
    # 免得它掉进下面「契约外字段」里被说得含糊；写 "presence":0 同样不允许。
    for k in FALL_SPEC.get("absent", ()):
        if k in pl:
            bad(f"[{tag}] P0 不得出现 {k!r}（PM 2026-10-18 裁定，见 node_cfg.h "
                f"NODE_PRESENCE_ENABLE）: {pl[k]!r}")
    for k, v in pl.items():
        kind = FALL_SPEC["required"].get(k) or FALL_SPEC["optional"].get(k)
        if kind is None:
            if k in FALL_SPEC.get("absent", ()):
                continue
            bad(f"[{tag}] fall 出现契约外字段 {k!r}")
            continue
        if kind == "i":
            # §2.4.2 明确 fall 是 int 0/1 —— 不是 bool，也不能是别的数
            if not isinstance(v, int) or isinstance(v, bool) or v not in (0, 1):
                bad(f"[{tag}] {k} 应为 int 0/1，实为 {v!r}")
        elif kind == "s":
            if not isinstance(v, str):
                bad(f"[{tag}] {k} 应为 string: {v!r}")
            elif k == "src" and v not in SRC_VALUES:
                bad(f"[{tag}] src 只能 {SRC_VALUES}: {v!r}")
        elif kind == "f":
            if isinstance(v, bool) or not isinstance(v, (int, float)):
                bad(f"[{tag}] {k} 应为数字: {v!r}")

    want = TAG_EXPECT.get(tag)
    if want is not None:
        if pl.get("fall") != want:
            bad(f"[{tag}] fall = {pl.get('fall')!r}，期望 {want}（ts={ts}）")


def parse_gap_line(out):
    """抠出 '错报 fall=0 的 K 次' 与 '落在采样间隙 M 次'。返回 (poll_fall, poll_gap, bad_k) 或 None。"""
    m = re.search(r"poll (\d+) 次，其中落在采样间隙 (\d+) 次，错报 fall=0 的 (\d+) 次", out)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def extract_frames(out):
    frames = []
    lines = out.splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith("FRAME ") and i + 1 < len(lines):
            frames.append((ln[len("FRAME "):], lines[i + 1]))
    return frames


def check_fixed_build(cc, incdir, td):
    """正例：真实源码。4 个场景必须全绿，帧必须逐字段合契约。"""
    exe = os.path.join(td, "radar-selftest")
    if not compile_one(cc, incdir, exe, RADAR_C):
        return None

    all_frames = []
    for name in SCENARIOS:
        rc, out = run_scenario(exe, name)
        if rc != 0 or "SELFTEST OK" not in out:
            bad(f"场景 {name} 未通过（rc={rc}）")
            continue
        if f"SCENARIO {name}" not in out:
            bad(f"场景 {name} 没打出 SCENARIO 行")
        frags = extract_frames(out)
        if not frags:
            bad(f"场景 {name} 没有产出任何 FRAME")
        all_frames.extend(frags)

        if name == "sustained":
            got = parse_gap_line(out)
            if got is None:
                bad("sustained 场景没打出计数行 —— 断言没跑到，别当通过")
            else:
                poll_fall, poll_gap, k_bad = got
                # 场景本身要够真：否则那条核心断言等于没测
                if poll_fall < 10:
                    bad(f"跌倒窗口 poll 次数太少（{poll_fall}），场景不够真")
                if poll_gap < 5:
                    bad(f"落在采样间隙的 poll 太少（{poll_gap}），缺陷路径没被走到")
                if k_bad != 0:
                    bad(f"修复后仍错报 fall=0 {k_bad} 次")

    for tag, raw in all_frames:
        check_fall_frame(tag, raw)

    n_by_tag = {}
    for tag, _ in all_frames:
        n_by_tag[tag] = n_by_tag.get(tag, 0) + 1
    print("# 帧统计: " + ", ".join(f"{k}={v}" for k, v in sorted(n_by_tag.items())))
    for tag, least in (("gap-poll-fall", 10), ("poll-normal", 1), ("poll-lagging", 1),
                       ("poll-recovered", 3), ("poll-boot", 1), ("poll-unknown", 1),
                       ("poll-demo-gap", 1), ("poll-demo-expired", 1)):
        if n_by_tag.get(tag, 0) < least:
            bad(f"tag {tag} 只出现 {n_by_tag.get(tag, 0)} 次，少于期望的 {least}")
    return n_by_tag


def check_mutation(cc, incdir, td):
    """反例：删掉回填缓存的两行 → 同一套自测必须失败，且失败形态是「错报次数 == 间隙 poll 次数」。

    这一步是整套材料的关键：正例全绿只能说明「没报错」，只有反例失败才能说明
    「那几条断言真的在测这个不变量」，而不是顺手绿了。
    """
    with open(RADAR_C, encoding="utf-8") as f:
        src = f.read()

    mutant = src
    for line in BACKFILL_LINES:
        n = mutant.count(line)
        if n != 1:
            bad(f"变异检验失效：{RADAR_C} 里 {line!r} 出现 {n} 次（应为 1 次），请更新本脚本")
            return
        mutant = mutant.replace(line, f"/* 变异检验：故意删掉 {line} */")

    mpath = os.path.join(td, "node_sensor_radar_mutant.c")
    with open(mpath, "w", encoding="utf-8") as f:
        f.write(mutant)

    mex = os.path.join(td, "radar-selftest-mutant")
    if not compile_one(cc, incdir, mex, mpath):
        return

    rc, out = run_scenario(mex, "sustained")
    got = parse_gap_line(out)
    print(f"# 变异体（删掉两行回填）sustained: rc={rc}, 计数行={got}")

    if rc == 0 or "SELFTEST OK" in out:
        bad("变异检验失败：删掉回填缓存的两行后自测仍然全绿 —— 那几条断言没有牙")
        return
    if got is None:
        bad("变异体没打出计数行，无法核对失败形态")
        return
    poll_fall, poll_gap, k_bad = got
    if k_bad <= 0:
        bad(f"变异体的错报次数为 {k_bad}，期望 > 0")
    if k_bad != poll_gap:
        bad(f"变异体的错报次数 {k_bad} 应恰好等于采样间隙 poll 次数 {poll_gap}"
            f"（修复前「间隙回 0」就是唯一的错报来源）")
    print(f"# 反例成立：修复前错报 fall=0 {k_bad} 次 = 采样间隙 poll {poll_gap} 次"
          f"（共 poll {poll_fall} 次）")


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
    name = f"hmprogram-radarcache-{os.getpid()}"
    cmd = ["docker", "run", "--rm", "--name", name,
           "-v", f"{ROOT}:/w", "-w", "/w",
           IMAGE, "python3", "tools/node-radar-cache-check.py", "--inside-docker", *extra]
    print("$ " + " ".join(cmd))
    return subprocess.call(cmd)


def main():
    in_docker = "--inside-docker" in sys.argv

    for p in (SELFTEST, RADAR_C, os.path.join(SRC, "node_json.c")):
        if not os.path.isfile(p):
            print(f"缺源文件 {p}", file=sys.stderr)
            return 2

    cc = find_cc()
    if cc is None:
        if in_docker:
            print("容器里也没有找到 C 编译器（试过 %s）" % ", ".join(CC_CANDIDATES), file=sys.stderr)
            return 2
        if docker_available():
            print(f"主机无 C 编译器 → 退回容器 {IMAGE} 执行（等价编译参数）")
            return run_inside_docker()
        print("环境缺 gcc 且没有可用的 docker 镜像 %s" % IMAGE, file=sys.stderr)
        return 2

    print(f"使用编译器: {cc}")
    with tempfile.TemporaryDirectory() as td:
        incdir = os.path.join(td, "stub")
        write_stubs(incdir)

        n_by_tag = check_fixed_build(cc, incdir, td)
        check_mutation(cc, incdir, td)

    print(f"\n场景 {len(SCENARIOS)} 个，字段级断言 {checked} 条"
          + (f"，帧统计 {sum(n_by_tag.values())} 帧" if n_by_tag else ""))
    if fails:
        print(f"\n❌ 失败 {len(fails)} 条：")
        for f in fails:
            print("   - " + f)
        return 1
    print("✅ 全部通过：4 场景 SELFTEST OK、fall 帧逐字段合契约 §2.2/§2.4.2、"
          "持续跌倒窗口 0 次错报，且变异体（删掉回填两行）必失败")
    return 0


if __name__ == "__main__":
    sys.exit(main())
