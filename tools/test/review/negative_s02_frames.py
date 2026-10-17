#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""negative_s02_frames.py -- s02「§3.7 六条样例帧」硬断言的负向验证（t24）。

做法：把 tools/test 与 docs/技术/接口契约.md 复制到 /tmp 沙箱，在**副本**上注入 7 种
坏帧/坏字段，逐一跑 s02_codec.py，要求 exit=1 且失败文本命中预期关键词；最后跑一次
未变异的副本作为基线（必须 exit=0）。仓库文件不被修改。

用法：python3 tools/test/review/negative_s02_frames.py
退出码：0=全部按预期报红且基线为绿；1=有变异没被抓到或基线不绿。
"""

import json
import os
import re
import shutil
import struct
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
SAND = "/tmp/hm-t24-neg"
CONTRACT = "docs/技术/接口契约.md"

sys.path.insert(0, os.path.join(REPO, "tools/test"))
sys.path.insert(0, os.path.join(REPO, "tools/test/contract"))
import contract_source as cs   # noqa: E402
import reference_impl as ri    # noqa: E402

C = cs.load()

def samples():
    out = []
    for s in C.uart_samples():
        out.append({"scene": s["scene"], "hex": cs.plain(s["hex"])})
    return out

def frame_of(idx):
    raw = samples()[idx]["hex"]
    data = bytes.fromhex(re.sub(r"[^0-9A-Fa-f]", "", raw))
    _v, t, seq, length = struct.unpack("<BBHH", data[2:8])
    return raw, data, t, seq, length

def reencode(raw, data, mutate_json):
    _v, t, seq, _l = struct.unpack("<BBHH", data[2:8])
    obj = json.loads(data[8:8 + _l].decode("utf-8"))
    mutate_json(obj)
    payload = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    new = ri.encode_frame(t, seq, payload)
    return " ".join(f"{b:02X}" for b in new)

def build_sandbox():
    if os.path.isdir(SAND):
        shutil.rmtree(SAND)
    os.makedirs(os.path.join(SAND, "docs/技术"), exist_ok=True)
    shutil.copytree(os.path.join(REPO, "tools/test"), os.path.join(SAND, "tools/test"),
                    ignore=shutil.ignore_patterns("__pycache__", "reports"))
    shutil.copy(os.path.join(REPO, CONTRACT), os.path.join(SAND, CONTRACT))

def run_s02():
    p = subprocess.run([sys.executable, "tools/test/suites/s02_codec.py"],
                       cwd=SAND, capture_output=True, text=True)
    out = p.stdout + p.stderr
    fails = [l.strip() for l in out.splitlines() if l.strip().startswith("FAIL")]
    return p.returncode, fails, out

def mutate(name, fn, expect_kw):
    build_sandbox()
    path = os.path.join(SAND, CONTRACT)
    text = open(path, encoding="utf-8").read()
    new_text = fn(text)
    assert new_text != text, "%s：变异没生效" % name
    open(path, "w", encoding="utf-8").write(new_text)
    code, fails, out = run_s02()
    hit = any(expect_kw in f for f in fails)
    print(f"[{'OK ' if (code == 1 and hit) else 'BAD'}] {name}")
    print(f"      exit={code} 失败行={fails if fails else '（无 FAIL 行）'}")
    if not hit and code != 1:
        print("      ---- 尾部输出 ----")
        print("\n".join(out.strip().splitlines()[-6:]))
    return code == 1 and hit

def mut_ellipsis(text):
    raw, _d, _t, _s, _l = frame_of(0)
    assert raw in text
    return text.replace(raw, raw[:29] + " … " + raw[29:], 1)

def mut_drop_crc_byte(text):
    raw, _d, _t, _s, _l = frame_of(0)
    assert raw in text
    return text.replace(raw, raw[:-2], 1)          # 去掉最后 1 字节

def mut_stale_crc(text):
    raw, data, _t, _s, _l = frame_of(0)
    bad = bytearray(data)
    bad[9] = bad[9] ^ 0xFF                          # 改 payload 首字节，CRC 不动
    new = " ".join(f"{b:02X}" for b in bad)
    assert raw in text
    return text.replace(raw, new, 1)

def mut_undeclared_key(text):
    # 用 0x03（fall）帧：它不受「逐字段等于 §2.2 示例」约束，只能被字段集断言抓住
    raw, data, _t, _s, _l = frame_of(1)
    new = reencode(raw, data, lambda o: o["payload"].__setitem__("smoke_x", 1))
    return text.replace(raw, new, 1)

def mut_missing_required(text):
    raw, data, _t, _s, _l = frame_of(1)
    new = reencode(raw, data, lambda o: o["payload"].pop("fall"))
    return text.replace(raw, new, 1)

def mut_undeclared_key_gas(text):
    # 0x01（gas）帧多键：被更强的「逐字段等于 §2.2 示例」先抓住，同样必须报红
    raw, data, _t, _s, _l = frame_of(0)
    new = reencode(raw, data, lambda o: o["payload"].__setitem__("smoke_x", 1))
    return text.replace(raw, new, 1)

def mut_envelope_extra(text):
    raw, data, _t, _s, _l = frame_of(0)
    new = reencode(raw, data, lambda o: o.__setitem__("extra", 1))
    return text.replace(raw, new, 1)

def mut_example_mismatch(text):
    # §2.2 示例 JSON 块里的 temp 改掉 → 0x01 帧应与之逐字段相等
    old = '"payload":{"smoke":320,"temp":24.5,"humi":56.2}'
    assert old in text, "§2.2 示例块文本与预期不同"
    return text.replace(old, '"payload":{"smoke":320,"temp":24.6,"humi":56.2}', 1)

MUTATIONS = [
    ("①省略号（payload 用 … 省略）", mut_ellipsis, "省略"),
    ("②整帧少一个字节（len != 10+LEN）", mut_drop_crc_byte, "整帧长度"),
    ("③payload 改字节但 CRC 未更新", mut_stale_crc, "CRC 复算"),
    ("④0x03 帧 payload 出现 §2.4.2 未声明的键", mut_undeclared_key, "未声明"),
    ("⑤0x03 帧 payload 缺 §2.4.2 必填字段 fall", mut_missing_required, "必填"),
    ("④b 0x01 帧多键（由 §2.2 逐字段断言兜住）", mut_undeclared_key_gas, "§2.2 示例"),
    ("⑥envelope 多一个键", mut_envelope_extra, "envelope"),
    ("⑦§2.2 示例与 0x01 帧不一致", mut_example_mismatch, "§2.2 示例"),
]

ok = True
for name, fn, kw in MUTATIONS:
    ok = mutate(name, fn, kw) and ok

# 基线：未变异的副本必须全绿
build_sandbox()
code, fails, _out = run_s02()
print(f"[{'OK ' if code == 0 and not fails else 'BAD'}] ⓪基线（未变异的副本）exit={code} 失败={fails}")
ok = (code == 0 and not fails) and ok
print("\n负向验证结论：", "全部按预期报红 ✅" if ok else "存在未按预期报红/基线不绿 ❌")
sys.exit(0 if ok else 1)
