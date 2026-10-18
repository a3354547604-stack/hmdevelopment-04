# t39 主机侧端到端串测（无硬件）

一条命令把 P0 数据链路从「节点 JSON」串到「App 能读的 REST 响应」，用**各模块的真实代码路径**接力，
不是各自的 mock 自说自话：

```
bash tools/test/run_e2e_host.sh          # 全链（内部套 tools/build-lock.sh）
bash tools/test/run_e2e_host.sh --dry    # 只跑不需要构建/容器的检查（漂移守卫 + 样本生成）
```

## 四段与「真到哪一步」

| 段 | 从 → 到 | 用的真实代码 | 主机模拟的部分 | 必须真机的部分 |
|---|---|---|---|---|
| 1 | 节点 JSON → §3 UART 帧 | `ws63-firmware/src/hub_json.c`、`hub_frame.c`（容器里编译后运行） | 节点 JSON 取自 `ws63-firmware/out/node-uplink-sample.txt`（固件同一份序列化代码的产物） | SLE 空口本身、真实传感器取值 |
| 2 | §3 帧字节流 → §4 IoTDA payload | `rk2206-gateway/src/gw_proto.c`、`gw_rules.c`、`gw_iotda.c`（与 `tools/gw-host-test.sh` 同一套 objs） | `gw_main.c:393-500` 的板级分派胶水（static、依赖外设）由 `gw_e2e.c` 抄一份，跑前有漂移守卫比对 | UART 电气、433、声光、MQTT 真连 |
| 3 | §4 payload → webhook → MySQL → REST | 真 compose 的 db+api（`server/`，不是 `tools/test/mock_server.py`） | IoTDA 平台本身：本脚本按 §5 的信封直接 POST 到 webhook | IoTDA 真推、平台鉴权、规则引擎 |
| 4 | REST 响应 ↔ 契约表格 | `docs/技术/接口契约.md` 现场解析（`tools/test/contract/contract_source.py`） | 无 | — |

结论：段 1/2/3 都是「主机上能跑的真实实现」，段 4 是「实测响应 vs 契约表格」。
真机才有的环节在报告里标「待用户实测」：IoTDA 真连、SLE 空口、LD6002C/MQ-2 实际读数、网关 MQTT。

## 为什么段 3 不用现成的 mock_server.py

`tools/test/mock_server.py` 是**我们按契约手写的替身**。用替身验替身只能证明「我们两次抄得一样」，
证明不了「服务器真的能收」。所以这一段起 compose 的 db+api，真发 webhook、真查 MySQL、真走 REST。

## 漂移守卫（为什么敢抄那 20 行）

`gw_e2e.c` 里的 `k_fall/k_mv/k_door/k_pir/k_temp/k_humi` 候选键表是从 `gw_main.c:400-408` 抄的。
`--dry` 阶段会把两边的字面量逐项比对，不一致直接失败——宁可在主机上红，也不要等上板才发现两端口径不同。

## 负向注入（防假绿）

| 注入 | 期望 |
|---|---|
| N1 改坏帧尾 CRC | 段 2 解析器报 `reason=4`，且该帧**不计入** frames_ok |
| N2 只改 LEN 字段不改实长 | 段 2 报 `reason=5` |
| N3 未知 TYPE 0x99（CRC 正确） | 段 2 报 `reason=6` |
| N4 webhook 带未绑定的 `device_id` | 传输层仍 200（§5.2 从宽），但 `handled=0` 且**告警行数不变**（不静默造数） |
| N5 webhook token 错误 | 401 或 `code=1502` |

## 现场怎么用（30 秒定位断点）

1. `GET /api/health` 的 `db` 不是 `ok` → 段 3 的服务器/数据库没起来，看 `docker compose logs api`。
2. 段 1 帧数对但段 2 `parse_error` → 帧格式问题（CRC/LEN/TYPE），字段名不对不会在这里报错，看下一步。
3. 段 2 有 props 但**没有 event** → 状态机没进阈值：先看 `smoke_mv/gas_level/fall_alarm`（这些在 props 的 extra 里），
   再看是不是保持窗口（`l1_hold_ms/l2_hold_ms/recover_hold_ms`）没过。
4. 段 2 有 event 但段 3 `handled=0` → 服务器没认出来：核对 `notify_data.header.device_id/node_id` 是否为已绑定 SN，
   以及 `services[0].event_type` 是不是 `alert|recover|sos`（§4.4）。
5. 段 3 有告警但段 4 报缺字段 → 契约表格与响应字段名不一致（**这是接缝缺陷**，报 PM）。
6. 报告：`tools/test/reports/e2e-<UTC>.md`（逐项三列 + 失败项）与同名 `.json`（含每段原始日志）。

## 范围与边界

- 只新增 `tools/test/` 下的文件；不改任何实现目录、不改契约。
- 需要重活（编译/起容器）时按 AGENTS.md 占构建槽——直接跑 `bash tools/test/run_e2e_host.sh` 即可（内部已加锁）。
- 段 3 跑完默认 `docker compose stop`（不删卷、不动别的项目容器）。
