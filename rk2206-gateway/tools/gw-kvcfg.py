#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gw-kvcfg.py — 网关凭证 KV 的主机侧工具（与固件 src/gw_kvcfg.c **两份独立实现**，互为对照）

它做四件事：
  1) --line    ：把凭证拼成一行可以直接粘进串口终端的 `gwkv set ...` 命令（用户不用自己拼空格/顺序）
  2) --block   ：按 gw_kvcfg.h 的布局算出 256 字节凭证块（十六进制 + CRC + 密钥指纹），
                 板上 `gwkv dump` 打出来的十六进制应当与它逐字节一致 —— 这是"布局没写错"的交叉证据
  3) --decode  ：反解一段 256 字节（比如从 dump 粘回来的），校验 magic/版本/CRC 并打印字段
  4) --gen/--check-vectors：生成/校验 test/gw_kvcfg_vectors.inc（固件自测拿它逐字节比对）

为什么要有这个工具：
  - 验收要求"不依赖板子即可自证"。参数校验、布局、CRC 在主机上是可判定的；
    固件侧同一套规则由 test/gw_selftest.c 的 test_kvcfg() 断言，两边用同一组向量对齐。
  - 用户现场只需要一行命令，拼错了会被校验先挡住（不是等连不上云才发现）。

注意：本工具**不**读也不写仓库里的任何密钥文件；secret 由命令行传入，只用于算块与指纹。
"""
import argparse
import io
import os
import sys

MAGIC = 0x47574B56           # "GWKV"
VERSION = 2
BLOCK_SIZE = 256
CRC_OFF = 252
OFF_HOST, LEN_HOST = 8, 64
OFF_ID, LEN_ID = 72, 64
OFF_SECRET, LEN_SECRET = 136, 64
OFF_TIME, LEN_TIME = 200, 48
OFF_MPORT = 248
OFF_TPORT = 250

E_PARAM, E_LEN, E_CHAR, E_PORT, E_SECRET = 1, 2, 3, 4, 5

# 与 gw_config.h 的编译期占位值一致（--defaults 用；也是"没写 KV 时板子用的那份"）
DEFAULTS = {
    "host": "117.78.16.25",
    "port": 1883,
    "device": "6a1b2c3d4e5f",
    "secret": "00000000000000000000000000000000",
    "time": "192.168.1.100",
    "timeport": 8000,
}


def fnv1a32(data: bytes) -> int:
    crc = 0x811C9DC5
    for b in data:
        crc ^= b
        crc = (crc * 0x01000193) & 0xFFFFFFFF
    return crc


def _host_charset_ok(v: str) -> bool:
    return all(c.isalnum() and c.isascii() or c in ".-" for c in v)


def _time_charset_ok(v: str) -> bool:
    return all(c.isalnum() and c.isascii() or c in ".-:" for c in v)


def _id_charset_ok(v: str) -> bool:
    return all(c.isalnum() and c.isascii() or c in ".-_:" for c in v)


def _secret_charset_ok(v: str) -> bool:
    return all(0x21 <= ord(c) <= 0x7E for c in v)


def validate(cfg: dict):
    """返回 (code, message)；code=0 通过。规则与 src/gw_kvcfg.c 的 gw_kvcfg_validate 一一对应。"""
    host, dev, sec = cfg["host"], cfg["device"], cfg["secret"]
    tim = cfg.get("time", "")
    if not host:
        return E_PARAM, "缺少 host=（IoTDA 接入地址，控制台给的那个域名）"
    if not dev:
        return E_PARAM, "缺少 device=（设备 ID）"
    if not sec:
        return E_PARAM, "缺少 secret=（设备密钥）"
    if len(host) >= LEN_HOST:
        return E_LEN, "host 超长：最多 %d 字节" % (LEN_HOST - 1)
    if len(dev) >= LEN_ID:
        return E_LEN, "device 超长：最多 %d 字节" % (LEN_ID - 1)
    if len(sec) >= LEN_SECRET:
        return E_LEN, "secret 超长：最多 %d 字节" % (LEN_SECRET - 1)
    if len(tim) >= LEN_TIME:
        return E_LEN, "time 超长：最多 %d 字节" % (LEN_TIME - 1)
    if not _host_charset_ok(host):
        return E_CHAR, "host 含非法字符（只允许 A-Z a-z 0-9 . -）"
    if not _id_charset_ok(dev):
        return E_CHAR, "device 含非法字符（只允许 A-Z a-z 0-9 . - _ :）"
    if len(sec) < 8:
        return E_SECRET, "secret 太短：至少 8 字节"
    if not _secret_charset_ok(sec):
        return E_CHAR, "secret 含非法字符（需可见 ASCII 且无空格）"
    if tim and not _time_charset_ok(tim):
        return E_CHAR, "time 含非法字符（只允许 A-Z a-z 0-9 . - :）"
    for key, name in (("port", "port"), ("timeport", "timeport")):
        v = cfg.get(key, 0)
        if not (1 <= int(v) <= 65535):
            return E_PORT, "%s 非法：范围 1..65535" % name
    return 0, "OK"


def encode(cfg: dict) -> bytes:
    code, msg = validate(cfg)
    if code != 0:
        raise ValueError("%s(%d): %s" % (errname(code), code, msg))
    blk = bytearray(BLOCK_SIZE)

    def put(off, raw):
        blk[off:off + len(raw)] = raw[:off + len(raw)]

    blk[0:4] = MAGIC.to_bytes(4, "little")
    blk[4:6] = VERSION.to_bytes(2, "little")
    blk[6:8] = BLOCK_SIZE.to_bytes(2, "little")
    put(OFF_HOST, cfg["host"].encode("ascii"))
    put(OFF_ID, cfg["device"].encode("ascii"))
    put(OFF_SECRET, cfg["secret"].encode("ascii"))
    put(OFF_TIME, cfg.get("time", "").encode("ascii"))
    blk[OFF_MPORT:OFF_MPORT + 2] = int(cfg["port"]).to_bytes(2, "little")
    blk[OFF_TPORT:OFF_TPORT + 2] = int(cfg["timeport"]).to_bytes(2, "little")
    blk[CRC_OFF:CRC_OFF + 4] = fnv1a32(bytes(blk[:CRC_OFF])).to_bytes(4, "little")
    return bytes(blk)


def errname(code: int) -> str:
    return {0: "OK", 1: "E_PARAM", 2: "E_LEN", 3: "E_CHAR", 4: "E_PORT",
            5: "E_SECRET", 6: "E_FLASH", 7: "E_VERIFY", 8: "E_EMPTY", 9: "E_CRC", 10: "E_VER"}.get(code, "E_UNKNOWN")


def decode(blk: bytes):
    """返回 (code, cfg_or_message)。与 gw_kvcfg_decode 同序检查。"""
    if len(blk) < BLOCK_SIZE:
        return 10, "块长度不足 %d（收到 %d）" % (BLOCK_SIZE, len(blk))
    if all(b == 0xFF for b in blk[:BLOCK_SIZE]):
        return 8, "KV 区全 0xFF（从未写入或已被 clear）"
    if int.from_bytes(blk[0:4], "little") != MAGIC:
        return 10, "magic 不符：这块不是本工具写的"
    if int.from_bytes(blk[4:6], "little") != VERSION:
        return 10, "版本不符（块内 %d，本工具 %d）" % (int.from_bytes(blk[4:6], "little"), VERSION)
    if int.from_bytes(blk[6:8], "little") != BLOCK_SIZE:
        return 10, "块长不符（块内 %d）" % int.from_bytes(blk[6:8], "little")
    want = int.from_bytes(blk[CRC_OFF:CRC_OFF + 4], "little")
    got = fnv1a32(blk[:CRC_OFF])
    if want != got:
        return 9, "CRC 不符（块内 0x%08X，实际 0x%08X）" % (want, got)

    def get(off, ln):
        raw = blk[off:off + ln]
        return raw.split(b"\x00", 1)[0].decode("ascii", "replace")

    cfg = {
        "host": get(OFF_HOST, LEN_HOST),
        "device": get(OFF_ID, LEN_ID),
        "secret": get(OFF_SECRET, LEN_SECRET),
        "time": get(OFF_TIME, LEN_TIME),
        "port": int.from_bytes(blk[OFF_MPORT:OFF_MPORT + 2], "little"),
        "timeport": int.from_bytes(blk[OFF_TPORT:OFF_TPORT + 2], "little"),
    }
    code, msg = validate(cfg)
    if code != 0:
        return code, "块内容非法：%s" % msg
    return 0, cfg


def secret_fp(secret: str) -> str:
    """与固件 `gwkv show` 打印的密钥指纹一致（FNV-1a 32，大写 8 位十六进制）。"""
    return "%08X" % fnv1a32(secret.encode("ascii", "replace"))


# ---------------------------------- 命令行 ----------------------------------
def parse_kv(args):
    cfg = dict(DEFAULTS) if args.defaults else {"host": "", "port": 1883, "device": "",
                                                "secret": "", "time": "", "timeport": 8000}
    alias = {"device_id": "device", "time_host": "time", "host": "host", "device": "device",
             "secret": "secret", "time": "time", "port": "port", "timeport": "timeport"}
    for item in args.kv:
        if "=" not in item:
            sys.exit("参数 `%s` 不是 key=value（形如 host=xxx.iot-mqtts.cn-north-4.myhuaweicloud.com）" % item)
        k, v = item.split("=", 1)
        if k not in alias:
            sys.exit("不认识的键 `%s`（可用 host/port/device/secret/time/timeport）" % k)
        if k in ("port", "timeport"):
            if not v.isdigit():
                sys.exit("%s=%s 不是数字" % (k, v))
            cfg[alias[k]] = int(v)
        else:
            cfg[alias[k]] = v
    return cfg


def cmd_line(cfg):
    code, msg = validate(cfg)
    if code != 0:
        print("❌ 参数不合法：%s(%d) %s" % (errname(code), code, msg))
        return 1
    parts = ["gwkv", "set", "host=%s" % cfg["host"], "device=%s" % cfg["device"],
             "secret=%s" % cfg["secret"]]
    if cfg["port"] != 1883:
        parts.append("port=%d" % cfg["port"])
    if cfg.get("time"):
        parts.append("time=%s" % cfg["time"])
    if cfg["timeport"] != 8000:
        parts.append("timeport=%d" % cfg["timeport"])
    print("✅ 参数校验通过。把下面这一行原样粘进串口终端（115200）并回车：\n")
    print(" ".join(parts))
    print()
    print("写入完成后按一下板子的复位键；启动日志里应出现 `凭证来源=Flash KV`。")
    print("密钥指纹（应与板上 `gwkv show` 打印的指纹一致）：%s" % secret_fp(cfg["secret"]))
    return 0


def cmd_block(cfg):
    blk = encode(cfg)
    print("块长 %d B，CRC=0x%08X，密钥指纹=%s" % (len(blk), int.from_bytes(blk[CRC_OFF:CRC_OFF + 4], "little"),
                                                 secret_fp(cfg["secret"])))
    print(blk.hex().upper())
    return 0


def cmd_decode(args):
    raw = args.decode
    if raw.startswith("@"):
        raw = io.open(raw[1:], encoding="utf-8").read()
    raw = "".join(raw.split())
    if len(raw) % 2:
        sys.exit("十六进制长度必须是偶数")
    try:
        blk = bytes.fromhex(raw)
    except ValueError as exc:
        sys.exit("不是合法十六进制：%s" % exc)
    code, out = decode(blk)
    if code != 0:
        print("❌ %s(%d) %s" % (errname(code), code, out))
        return 1
    print("✅ 块合法：")
    print("  接入地址  : %s:%d" % (out["host"], out["port"]))
    print("  设备 ID   : %s" % out["device"])
    print("  密钥      : %d 字节（指纹 %s）" % (len(out["secret"]), secret_fp(out["secret"])))
    print("  时间服务器: %s:%d" % (out["time"] or "(未配置)", out["timeport"]))
    return 0


VECTOR_CASES = [
    ("默认占位值（与 gw_config.h 一致）",
     "117.78.16.25", 1883, "6a1b2c3d4e5f", "00000000000000000000000000000000", "192.168.1.100", 8000),
    ("现场示例：华为云域名 + 32 位十六进制密钥",
     "abcdefg.iot-mqtts.cn-north-4.myhuaweicloud.com", 1883, "6a1b2c3d4e5f60718293a4b5",
     "0123456789abcdef0123456789abcdef", "192.168.1.50", 8000),
    ("边界：host/device/secret 各 63 字节、time 47 字节",
     "a" * 63, 65535, "b" * 63, "c" * 63, "d" * 47, 1),
    ("无时间服务器（板上走 NTP 备选）",
     "10.0.0.9", 1883, "M0-TEST-0001", "s3cret-0123456789", "", 8000),
]


def vector_block():
    out = []
    for name, host, port, dev, sec, tim, tport in VECTOR_CASES:
        cfg = {"host": host, "port": port, "device": dev, "secret": sec, "time": tim, "timeport": tport}
        blk = encode(cfg)
        out.append((name, cfg, int.from_bytes(blk[CRC_OFF:CRC_OFF + 4], "little"), blk.hex().upper()))
    return out


def vectors_text():
    lines = [
        "/*",
        " * gw_kvcfg_vectors.inc — 生成物，别手改。",
        " *",
        " * 真源：rk2206-gateway/tools/gw-kvcfg.py 的 VECTOR_CASES（Python 侧独立实现）。",
        " * 再生成：python3 rk2206-gateway/tools/gw-kvcfg.py --gen-vectors rk2206-gateway/test/gw_kvcfg_vectors.inc",
        " * 校验一致：python3 rk2206-gateway/tools/gw-kvcfg.py --check-vectors rk2206-gateway/test/gw_kvcfg_vectors.inc",
        " *",
        " * 固件自测（test/gw_selftest.c 的 test_kvcfg）拿它做逐字节比对：",
        " * 同一个 cfg 经 C 的 gw_kvcfg_encode() 得到的 256 字节必须与这里的 block_hex 完全相同，",
        " * 否则说明两种实现的布局/CRC 有分叉 —— 这比『跑一遍看输出像不像』硬。",
        " */",
        "typedef struct {",
        "    const char *name;",
        "    const char *host;",
        "    uint16_t    mqtt_port;",
        "    const char *device_id;",
        "    const char *secret;",
        "    const char *time_host;",
        "    uint16_t    time_port;",
        "    uint32_t    crc;",
        "    const char *block_hex;",
        "} gw_kvcfg_vector_t;",
        "",
        "static const gw_kvcfg_vector_t k_kvcfg_vectors[] = {",
    ]
    for name, cfg, crc, hexs in vector_block():
        lines.append("    {")
        lines.append('        "%s",' % name.replace('"', "'"))
        lines.append('        "%s", %d,' % (cfg["host"], cfg["port"]))
        lines.append('        "%s",' % cfg["device"])
        lines.append('        "%s",' % cfg["secret"])
        lines.append('        "%s", %d,' % (cfg["time"], cfg["timeport"]))
        lines.append("        0x%08Xu," % crc)
        lines.append('        "%s",' % hexs)
        lines.append("    },")
    lines.append("};")
    lines.append("")
    lines.append("#define GW_KVCFG_VECTOR_COUNT (sizeof(k_kvcfg_vectors) / sizeof(k_kvcfg_vectors[0]))")
    lines.append("")
    return "\n".join(lines)


def cmd_gen_vectors(path):
    text = vectors_text()
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)
    io.open(path, "w", encoding="utf-8", newline="\n").write(text)
    print("[gw-kvcfg] 已生成 %s（%d 条向量）" % (path, len(VECTOR_CASES)))
    return 0


def cmd_check_vectors(path):
    if not os.path.exists(path):
        print("[gw-kvcfg] ❌ 缺少 %s，先跑 --gen-vectors" % path)
        return 2
    cur = io.open(path, encoding="utf-8").read()
    if cur != vectors_text():
        print("[gw-kvcfg] ❌ %s 与 VECTOR_CASES 不一致：重新跑 --gen-vectors" % path)
        return 1
    print("[gw-kvcfg] ✅ 向量文件与生成器一致：%s（%d 条）" % (path, len(VECTOR_CASES)))
    return 0


def cmd_selftest():
    fails = []

    def ck(cond, what):
        if not cond:
            fails.append(what)

    ck(fnv1a32(b"") == 0x811C9DC5, "FNV-1a 空串初值")
    ck(fnv1a32(b"a") == 0xE40C292C, "FNV-1a 'a'")
    for name, cfg, crc, hexs in vector_block():
        blk = encode(cfg)
        ck(len(blk) == BLOCK_SIZE, "%s: 块长 256" % name)
        ck(blk.hex().upper() == hexs, "%s: 编码稳定" % name)
        code, back = decode(blk)
        ck(code == 0 and back["host"] == cfg["host"] and back["secret"] == cfg["secret"],
           "%s: 解码往返" % name)
        bad = bytearray(blk)
        bad[OFF_HOST] ^= 0xFF
        ck(decode(bytes(bad))[0] == 9, "%s: 篡改字段 → E_CRC" % name)
        bad = bytearray(blk)
        bad[0] ^= 0xFF
        ck(decode(bytes(bad))[0] in (9, 10), "%s: 篡改 magic → 拒绝" % name)
        ck(decode(b"\xFF" * BLOCK_SIZE)[0] == 8, "%s: 全 0xFF → E_EMPTY" % name)
    bad_cases = [
        ({"host": "", "port": 1883, "device": "d", "secret": "s3cret-01", "time": "", "timeport": 8000}, E_PARAM),
        ({"host": "h", "port": 1883, "device": "", "secret": "s3cret-01", "time": "", "timeport": 8000}, E_PARAM),
        ({"host": "h", "port": 1883, "device": "d", "secret": "", "time": "", "timeport": 8000}, E_PARAM),
        ({"host": "a" * 64, "port": 1883, "device": "d", "secret": "s3cret-01", "time": "", "timeport": 8000}, E_LEN),
        ({"host": "bad host", "port": 1883, "device": "d", "secret": "s3cret-01", "time": "", "timeport": 8000}, E_CHAR),
        ({"host": "h", "port": 0, "device": "d", "secret": "s3cret-01", "time": "", "timeport": 8000}, E_PORT),
        ({"host": "h", "port": 1883, "device": "d", "secret": "short", "time": "", "timeport": 8000}, E_SECRET),
        ({"host": "h", "port": 1883, "device": "d", "secret": "s3cret-01", "time": "bad time", "timeport": 8000}, E_CHAR),
    ]
    for cfg, want in bad_cases:
        got, _ = validate(cfg)
        ck(got == want, "负向用例 code=%d（期望 %d）：%r" % (got, want, cfg))
    ck(secret_fp("00000000000000000000000000000000") == secret_fp("0" * 32), "指纹稳定")

    if fails:
        print("[gw-kvcfg] ❌ 自测失败 %d 项：" % len(fails))
        for f in fails:
            print("   - %s" % f)
        return 1
    print("[gw-kvcfg] ✅ 自测通过（%d 条向量 + 8 条负向 + CRC 初值）" % len(VECTOR_CASES))
    return 0


def main():
    ap = argparse.ArgumentParser(description="网关凭证 KV 主机工具（与固件 gw_kvcfg.c 互为对照）",
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kv", nargs="*", help="host=... device=... secret=... [port=... time=... timeport=...]")
    ap.add_argument("--line", action="store_true", help="打印可直接粘贴的 gwkv set 命令行")
    ap.add_argument("--block", action="store_true", help="打印 256 字节凭证块（十六进制）")
    ap.add_argument("--decode", metavar="HEX", help="反解 256 字节（可写 @文件名）")
    ap.add_argument("--defaults", action="store_true", help="以 gw_config.h 的编译期默认值为基线（部分更新用）")
    ap.add_argument("--secret-fp", metavar="SECRET", help="只算密钥指纹（与板上 gwkv show 对齐）")
    ap.add_argument("--gen-vectors", metavar="PATH", help="生成 test/gw_kvcfg_vectors.inc")
    ap.add_argument("--check-vectors", metavar="PATH", help="校验 .inc 与生成器一致")
    ap.add_argument("--selftest", action="store_true", help="主机自测（不依赖板子）")
    args = ap.parse_args()

    if args.selftest:
        return cmd_selftest()
    if args.gen_vectors:
        return cmd_gen_vectors(args.gen_vectors)
    if args.check_vectors:
        return cmd_check_vectors(args.check_vectors)
    if args.secret_fp:
        print(secret_fp(args.secret_fp))
        return 0
    if args.decode:
        return cmd_decode(args)
    cfg = parse_kv(args)
    if args.line:
        return cmd_line(cfg)
    if args.block:
        return cmd_block(cfg)
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
