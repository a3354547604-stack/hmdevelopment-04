# 验收基线报告（tools/test）

- 生成时间：2026-10-21 15:52:55Z
- 契约：接口契约.md · 170226 B · mtime 2026-10-21 23:50
- git：9212a3a
- 被测 REST：内置 mock（tools/test/mock_server.py）
- 环境：python 3.14.4 / docker Docker version 27.5.1, build 9f9e405

## 汇总

| 套件 | 通过 | 失败 | 跳过 | 用例数 |
|---|---:|---:|---:|---:|
| s01_schema 契约 schema 与示例一致性 | 12 | 0 | 0 | 12 |
| s02_codec UART 帧编解码与 CRC（§3） | 14 | 0 | 0 | 14 |
| s03_iotda IoTDA 物模型与鉴权（§4） | 13 | 0 | 0 | 13 |
| s04_models 阈值状态机与数据模型（§7/§8/§9） | 20 | 0 | 0 | 20 |
| s05_rest REST 契约冒烟（mock 内置） | 14 | 0 | 0 | 14 |
| s06_audit 契约自身一致性与交叉引用（§0–§12） | 19 | 0 | 0 | 19 |
| s07_provision 本地配网/配对（§12 + §13 + §3.9 + §2.7） | 13 | 0 | 0 | 13 |
| **构建检查** | 2 | 0 | 3 | 5 |

**总计：通过 105 · 失败 0 · 跳过 0**（只算 7 个契约套件；构建检查的跳过是环境门，见上一行）

## 跳过项（已知问题或环境缺失）

- `构建检查 / 网关构建检查`：主机没有 gcc/clang，无法做语法检查
- `构建检查 / HAP 构建检查`：主机没有 hvigorw（HarmonyOS SDK 未安装或未进 PATH），无法真编译 HAP；在装了 DevEco/HarmonyOS SDK 的机器上跑 hvigorw assembleHap；本机只做工程结构检查
- `构建检查 / WS63 固件构建检查`：默认只做静态检查；整包编译需 --full（容器 ws63wsl:latest + SDK）；验收构建时用：python3 tools/test/build_checks.py --full

## 构建检查证据

### 契约可解析（通过）

契约结构解析正常：biz_codes=33, db_tables=11, device_codes=13, endpoints=20, envelope_fields=6, fall=5, gas=8, motion=3

```
$ /usr/bin/python3 /mnt/c/Users/Administrator/Desktop/HMProgram/tools/test/contract/contract_source.py   # rc=0
{
  "path": "docs/技术/接口契约.md",
  "sections": 121,
  "types": [
    "gas",
    "fall",
    "door",
    "presence",
    "status",
    "env"
  ],
  "envelope_fields": [
    "字段",
    "类型",
    "必填",
    "单位",
    "说明",
    "示例"
  ],
  "uart_types": {
    "0x01": "NODE_DATA",
    "0x02": "HUB_STATUS",
    "0x03": "HUB_EVENT",
    "0x10": "GW_ACK",
    "0x11": "GW_CMD",
    "0x12": "GW_NACK",
    "0x13": "GW_TIME",
    "0x20": "SLE_SCAN_REQ",
    "0x21": "SLE_SCAN_RSP",
    "0x22": "PAIR_BIND",
    "0x23": "PAIR_BIND_ACK",
    "0x24": "NODE_LIST"
  },
  "endpoints": 20,
  "biz_codes": [
    0,
    1000,
    1001,
    1002,
    1003,
    1004,
    1100,
    1101,
    1102,
    1103,
    1104,
    1105,
    1200,
    1201,
    1202,
    1203,
    1204,
    1205,
    1206,
    1300,
    1301,
    1302,
    1400,
    1500,
    1501,
    1502,
    1600,
    1601,
    1602,
    1603,
    1604,
    1605,
    1606
  ],
  "device_codes": [
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12
  ],
  "db_tables": [
    "users",
    "devices",
    "user_devices",
    "nodes",
    "sensor_readings",
    "alerts",
    "alert_dispositions",
    "activity_hourly",
    "activity_daily",
    "device_online",
    "push_tokens"
  ],
  "settings": {
    "no_motion_min": 30,
    "presence_hold_s": 60,
    "gas_t1_mv": 1500,
    "gas_t2_mv": 2500,
    "gas_l1_hold_s": 10,
    "gas_l2_hold_s": 6,
    "gas_recover_mv": 200,
    "gas_recover_hold_s": 30,
    "away_af
```

### server 构建检查（通过）

Python 编译 + schema.sql 结构检查通过

```
$ /usr/bin/python3 -m compileall -q /mnt/c/Users/Administrator/Desktop/HMProgram/server   # rc=0
```

```
$ grep -c 'create table' server/db/schema.sql   # rc=0
11 张表
```

```
$ docker compose -f /mnt/c/Users/Administrator/Desktop/HMProgram/server/docker-compose.yml config --quiet   # rc=0
```

### 网关构建检查（跳过）

主机没有 gcc/clang，无法做语法检查

### HAP 构建检查（跳过）

主机没有 hvigorw（HarmonyOS SDK 未安装或未进 PATH），无法真编译 HAP

### WS63 固件构建检查（跳过）

默认只做静态检查；整包编译需 --full（容器 ws63wsl:latest + SDK）

```
$ find ws63-firmware/src -name '*.c'   # rc=0
20 个 .c 文件
```

## 复现命令

```bash
python3 tools/test/run_all.py --full        # 全套（含容器编译检查）
python3 tools/test/run_all.py               # 秒级（契约 + 构建静态检查）
python3 tools/test/suites/s02_codec.py      # 单跑某一套
python3 tools/test/mock_server.py --port 8801   # 起 mock 供 App/server 联调
```
