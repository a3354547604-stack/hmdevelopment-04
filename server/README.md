# server/ — 云端与服务器（FastAPI + MySQL 8）

独居老人居家安全监测与应急辅助系统的服务器：接收 IoTDA 规则引擎/webhook 推送的网关数据，
落库、判级、重推告警，并向 App 提供 REST 接口。

- **唯一技术事实源**：`docs/技术/接口契约.md`（本目录所有字段、错误码、表结构以它为准）
- **任务依据**：任务卡 t5「服务器骨架、数据库与模拟 API」
- **契约依据的行号**：接口 §6、表 §7、阈值/状态机 §8、错误码 §9、webhook 信封 §5、物模型 §4

## 1. 技术栈与目录

| 项 | 选择 | 说明 |
|---|---|---|
| 框架 | FastAPI 0.115 + Uvicorn | 任务卡指定；契约 §1 的示意图写的是 Node.js，差异与处置见 §9 |
| 数据库 | MySQL 8.0（InnoDB / utf8mb4_0900_ai_ci） | 表结构见 `db/schema.sql`（契约 §7.13：DDL 的唯一维护点） |
| DB 访问 | SQLAlchemy Core + PyMySQL | 只用 Core（SQL 显式可读），不引 ORM 隐式行为 |
| 鉴权 | JWT HS256（PyJWT）+ bcrypt(cost=10) | 契约 §6.1/§6.4 |
| 容器 | docker compose（项目名 `hmprogram`） | 容器/网络/卷统一 `hmprogram-` 前缀，端口只绑 127.0.0.1 |

```
server/
├── app/
│   ├── main.py            FastAPI 应用、异常处理器、lifespan（建表 + 调度器）
│   ├── config.py          环境变量 → Settings（prod 下占位符密钥拒绝启动）
│   ├── db.py              engine/连接（会话时区强制 UTC）、schema 执行、查询助手
│   ├── errors.py          契约 §9.2 错误码表、统一响应体 {code,message,data,ts}
│   ├── payload.py         响应清洗：最外层 None 省略（§0.4）
│   ├── security.py        bcrypt、JWT、登录限流（§9.1 429）
│   ├── deps.py            鉴权依赖、设备归属校验（1104/1200）
│   ├── settings_keys.py   21 个可配置阈值（§6.9 字段表）+ 校验与「能否下发」判定
│   │                      （§8.2 的 gas_l1_to_l2_s 是算法参数，刻意不在 items 里，见 D-01）
│   ├── summary.py         告警文案（§6.13 逐字表）、趋势/环比文案（§6.17/§6.18）
│   ├── state.py           进程内运行态（命令流水、webhook 最近时刻、调度计数）
│   ├── middleware.py      Idempotency-Key 回放（§6.1，5 分钟）
│   ├── seed.py            mock 种子数据（演示账号/网关/节点/读数/告警/报表）
│   ├── mocks/virtual.py   虚拟网关：把 demo 事件按 §5 信封打回自己的 webhook
│   ├── routers/           auth / devices / alerts / reports / push / webhook
│   └── services/          telemetry（入库）/ alerts（判级重推）/ reports / scheduler / iotda
├── db/schema.sql          11 张表 DDL（首次初始化容器时自动执行，启动时也会幂等执行）
├── db/init/15-test-db.sh  建 `<DB_NAME>_test` 并授权，让 pytest 不污染演示库
├── tests/                 pytest 契约测试（真 MySQL，102 例）
├── deploy/nginx.conf      可选反代（profile=proxy，80 → 127.0.0.1:3000）
└── .env.example           环境变量样例（**不含任何真实密钥**）
```

## 2. 快速开始

```bash
cd server
cp .env.example .env            # 可选：不建 .env 也能零配置启动（全部有 dev 兜底）
docker compose up -d --build    # 起 db + api（首次会建库建表）
docker compose ps               # 两个容器都应为 healthy
docker compose exec api python -m app.seed          # 灌 mock 种子（幂等）
docker compose exec api python -m app.seed --bind-code   # 额外打印一个 6 位绑定码（演示绑定用）
curl -s http://127.0.0.1:3000/api/health
```

- 后端监听 `127.0.0.1:${API_PORT:-3000}`（契约 §0.8：Nginx/App 走 80 → 127.0.0.1:3000）
- MySQL 监听 `127.0.0.1:${DB_PORT_HOST:-3306}`
- 可选反代：`docker compose --profile proxy up -d`（`127.0.0.1:${HTTP_PORT:-8080}` → api）
- 停止/清理：`docker compose down`（保留数据卷 `hmprogram-server-db-data`）；`docker compose down -v` 连数据一起删

> **演示提示**：种子只写入一次心跳，之后没有真实上报时，服务器会在 180 s（`gateway_offline_timeout_s`，§8.7）
> 后按契约把网关判为离线并生成 `device_offline` 告警——这是**预期行为**，不是故障。演示前重跑一次 `app.seed`，
> 或用 README §5 示例 21 的属性 webhook 重新上报一次，即可让 `online=true`。

`app.seed` 会创建：演示账号 `demo / demo-pass-123`（**仅 dev**）、网关 `M0-3F1A2B`（已绑定，含 H1/M1/M2/M4 四个节点）、
未绑定网关 `M0-9C2D4E`（用于走绑定流程）、最近 3 天读数（gateway 属性快照 / presence 活动 / gas 背景值 / 一组跌倒）、
3 条不同状态的告警，并按 §7.9 归并出 `activity_hourly`/`activity_daily`。

## 3. 环境变量

完整清单与注释见 [`.env.example`](.env.example)。必须关注的几项：

| 变量 | 默认（dev） | 说明 |
|---|---|---|
| `APP_ENV` | `dev` | `prod` 时若 `JWT_SECRET`/`WEBHOOK_TOKEN` 仍是占位符，进程**拒绝启动** |
| `CLOCK_SOURCE` | `server` | CAL-09 未完成前必须为 `server`（§0.2）；会如实出现在 `/api/health` |
| `JWT_SECRET` | `dev-only-change-me` | 生产必须替换；泄露=可伪造任意用户 token |
| `WEBHOOK_TOKEN` | `dev-webhook-token-change-me` | 32 字节随机 hex；IoTDA 转发 URL 的 `?token=`（§5.2） |
| `IOTDA_DISPATCH_MODE` | `mock` | `mock` 只记命令流水（离线可演示）；`real` 走华为云 IoTDA 应用侧接口 |
| `SCHEDULER_ENABLED` | `1` | 重推/网关离线/报表归并后台线程；pytest 里置 0 |
| `DB_*` | `eldercare/eldercare/hmprogram-dev-pass` | 生产必须改口令，并保持仅监听 127.0.0.1 |

**降级行为**：`IOTDA_DISPATCH_MODE=real` 但缺 `IOTDA_PROJECT_ID`/`APP_ID`/`APP_SECRET` 时，
下发接口返回 `1205/502`（云端下发失败）而不是抛 500——App 能明确区分「参数没生效」与「服务器炸了」。

## 4. 接口总表

统一响应体 `{"code":0,"message":"ok","data":{...},"ts":<ms>}`；除 `/api/auth/*`、`/api/health`、`/api/time` 外都要
`Authorization: Bearer <JWT>`；**不允许 HTTP 200 而 code!=0**（§6.1）。

| # | 方法 | 路径 | 契约 | 说明 |
|---|---|---|---|---|
| 1 | POST | `/api/auth/register` | §6.3 | 201；用户名重复 1103 |
| 2 | POST | `/api/auth/login` | §6.4 | token 7 天；不区分「用户不存在/密码错」均为 1102 |
| — | GET | `/api/auth/me` | 补充 | 校验本地 token 是否仍有效（不在 18 个之内） |
| 3 | GET | `/api/devices` | §6.5 | 我绑定的设备 + 未读告警数 |
| 4 | POST | `/api/devices/bind` | §6.6 | 201；1201/1202/1203（见 §6 表） |
| 5 | GET | `/api/devices/{id}/status` | §6.7 | 属性快照 + 节点在线 + 待处置告警数 |
| 6 | GET | `/api/devices/{id}/readings` | §6.8 | 按 ts 倒序，limit≤200 |
| 7 | GET | `/api/devices/{id}/settings` | §6.9 | `{source,updated_at,items}` |
| 8 | PUT | `/api/devices/{id}/settings` | §6.10 | 局部更新；仅 owner；1002/1003 |
| 9 | POST | `/api/devices/{id}/config` | §6.11 | 下发命令；`accepted`/`rejected` 为**字符串数组**（元素=键名，PM 裁定 D-05），可选 `reasons` 给被拒原因 |
| 10 | POST | `/api/devices/{id}/demo` | §6.12 | 演示事件，走真实链路（虚拟网关 → webhook） |
| 11 | GET | `/api/alerts` | §6.13 | `device_id` 必填；`status=pending|confirmed|resolved|all` |
| 12 | GET | `/api/alerts/{id}` | §6.14 | data **平铺**（PM 裁定①）；打开即 pending→confirmed |
| 13 | POST | `/api/alerts/{id}/disposition` | §6.15 | 处置 → `resolved` + 停推；重复 1301 |
| 14 | GET | `/api/reports/activity` | §6.16 | day 出 24 小时分布，week/month 出日粒度 |
| 15 | GET | `/api/reports/trend` | §6.17 | 近 N 天 + 与昨日同比文案 |
| 16 | GET | `/api/reports/summary` | §6.18 | 活动/告警/环境三段汇总 |
| 17 | POST | `/api/push/register` | §6.19 | Push Kit token 幂等注册（P1，mock 只落库） |
| 18 | GET | `/api/health` | §6.20 | 免鉴权；DB 不可用 → 503 且 `db:"down"` |
| — | GET | `/api/time` | v1.2 | 免鉴权；网关联网后先取 UTC 再连 IoTDA |
| — | POST | `/api/iotda/webhook?token=` | §5 | 内部接口：IoTDA 规则引擎/转发目标调用 |

## 5. curl 示例（实测输出）

以下示例在 `docker compose up` + `python -m app.seed` 之后本机实测得到（`TOKEN` 用登录返回的 JWT 替换，
`BIND_CODE` 用 `seed --bind-code` 打印的值）。为便于阅读省略了部分长响应。

#### 1 健康检查（免鉴权）

```bash
$ curl -s http://127.0.0.1:3000/api/health
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "ok": true,
    "version": "1.0.0",
    "db": "ok",
    "clock_source": "server",
    "server_ts": 1792246174414
  },
  "ts": 1792246174414
}
```

`HTTP 200`

#### 2 网关校时（免鉴权，契约 §6.21）

```bash
$ curl -s -i http://127.0.0.1:3000/api/time
```

```
HTTP/1.1 200 OK
cache-control: no-store            # §6.21 定稿：网关每次上线都重新取时，中间不许有缓存
content-type: application/json
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "utc_epoch": 1792246174,
    "utc_epoch_ms": 1792246174458,
    "iso8601": "2026-10-17T14:09:34Z",
    "tz": "UTC",
    "server_ts": 1792246174458
  },
  "ts": 1792246174458
}
```

`HTTP 200`

#### 3 注册（HTTP 201）

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/auth/register -H 'Content-Type: application/json' -d '{"username":"demo_1792246174","password":"pass-123456","phone":"13800000000"}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "user_id": 2,
    "username": "demo_1792246174",
    "created_at": 1792246174542
  },
  "ts": 1792246174547
}
```

`HTTP 201`

#### 4 登录（演示账号 demo/demo-pass-123）

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/auth/login -H 'Content-Type: application/json' -d '{"username":"demo","password":"demo-pass-123"}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "token": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.<载荷>.<签名>（示例已脱敏）",
    "expires_in": 604800,
    "user": {
      "id": 1,
      "username": "demo"
    }
  },
  "ts": 1792246174702
}
```

`HTTP 200`

#### 5 未带 token → 1100/401

```bash
$ curl -s http://127.0.0.1:3000/api/devices
```

```json
{
  "code": 1100,
  "message": "未提供访问凭证",
  "data": null,
  "ts": 1792246174745
}
```

`HTTP 401`

#### 6 我的设备列表

```bash
$ curl -s http://127.0.0.1:3000/api/devices -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "total": 1,
    "items": [
      {
        "id": 1,
        "sn": "M0-3F1A2B",
        "name": "客厅网关",
        "role": "owner",
        "online": true,
        "last_heartbeat": 1792246157104,
        "alarm_state": 1,
        "unread_alerts": 1
      }
    ]
  },
  "ts": 1792246174792
}
```

`HTTP 200`

#### 7 绑定设备（HTTP 201）

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/devices/bind -H 'Content-Type: application/json' -H 'Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIyIiwidXNlcm5hbWUiOiJkZW1vXzE3OTIyNDYxNzQiLCJpYXQiOjE3OTIyNDYxNzQsImV4cCI6MTc5Mjg1MDk3NH0.gjNhkfdDfo22H5NLNJefmNfdG1RQxyIq07adTuqsUbM' -d '{"sn":"M0-9C2D4E","bind_code":"923383"}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "device_id": 2,
    "sn": "M0-9C2D4E",
    "role": "owner"
  },
  "ts": 1792246174847
}
```

`HTTP 201`

#### 8 设备实时状态

```bash
$ curl -s http://127.0.0.1:3000/api/devices/1/status -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "device_id": 1,
    "online": true,
    "last_heartbeat": 1792246157104,
    "alarm_state": 1,
    "properties": {
      "humi": 51.2,
      "temp": 23.4,
      "smoke": 0,
      "alarm_state": 1,
      "nodes_online": {
        "H1": true,
        "M1": true,
        "M2": true,
        "M4": true
      }
    },
    "nodes": [
      {
        "sn": "H1",
        "type": "hub",
        "location": "客厅",
        "online": true,
  // （响应共 63 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 9 读数（倒序 limit=2）

```bash
$ curl -s http://127.0.0.1:3000/api/devices/1/readings?limit=2 -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "total": 145,
    "items": [
      {
        "id": 429,
        "node_sn": "M1",
        "type": "gas",
        "ts": 1792245633000,
        "payload": {
          "lpg": 229,
          "humi": 55.1,
          "temp": 23.3,
          "smoke": 186,
          "adc_mv": 489,
          "warmup": false
        }
      },
      {
        "id": 381,
        "node_sn": "M0",
        "type": "gateway",
        "ts": 1792245600000,
        "payload": {
  // （响应共 42 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 10 读取阈值设置

```bash
$ curl -s http://127.0.0.1:3000/api/devices/1/settings -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "source": "local",
    "updated_at": null,
    "items": {
      "no_motion_min": 30,
      "presence_hold_s": 60,
      "away_after_open_s": 600,
      "gas_t1_mv": 1500,
      "gas_t2_mv": 2500,
      "gas_l1_hold_s": 10,
      "gas_l2_hold_s": 6,
      "gas_recover_mv": 200,
      "gas_recover_hold_s": 30,
      "fall_suppress_s": 30,
      "report_interval_s": 60,
      "node_offline_timeout_s": 9,
      "gateway_offline_timeout_s": 180,
      "temp_high_c": 35.0,
      "temp_low_c": 5.0,
      "humi_high_pct": 85.0,
      "humi_low_pct": 20.0,
      "env_hold_min": 30,
  // （响应共 37 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 11 修改阈值设置（局部）

```bash
$ curl -s -X PUT http://127.0.0.1:3000/api/devices/1/settings -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"items":{"no_motion_min":45,"gas_t1_mv":1400}}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "source": "local",
    "updated_at": 1792246175023,
    "items": {
      "no_motion_min": 45,
      "presence_hold_s": 60,
      "away_after_open_s": 600,
      "gas_t1_mv": 1400,
      "gas_t2_mv": 2500,
      "gas_l1_hold_s": 10,
      "gas_l2_hold_s": 6,
      "gas_recover_mv": 200,
      "gas_recover_hold_s": 30,
      "fall_suppress_s": 30,
      "report_interval_s": 60,
      "node_offline_timeout_s": 9,
      "gateway_offline_timeout_s": 180,
      "temp_high_c": 35.0,
      "temp_low_c": 5.0,
      "humi_high_pct": 85.0,
      "humi_low_pct": 20.0,
      "env_hold_min": 30,
  // （响应共 37 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 12 下发配置（可下发/不可下发分开报）

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/devices/1/config -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"keys":["report_interval_s","temp_high_c"]}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "command_id": "cmd-1792246175067-1",
    "request_id": "9435d7",
    "accepted": ["report_interval_s"],
    "rejected": ["temp_high_c"],
    "reasons": {"temp_high_c": "环境阈值 P1，P0 不远程下发"},
    "dispatched_at": 1792246175067,
    "timeout_s": 10
  },
  "ts": 1792246175088
}
```

`HTTP 200`

#### 13 演示事件 fall（走真实 webhook 链路）

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/devices/1/demo -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"event":"fall"}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "command_id": "cmd-1792246175127-0",
    "request_id": "0ebfd2",
    "accepted": ["demo_trigger"],
    "rejected": [],
    "dispatched_at": 1792246175127,
    "timeout_s": 10,
    "event": "fall",
    "alert": {
      "id": 4,
      "device_id": 1,
      "node_sn": "M2",
      "type": "fall",
      "level": 2,
      "ts": 1792246175127,
      "status": "pending",
      "push_count": 1,
      "last_push_at": 1792246175190,
      "next_push_at": 1792246235127,
      "summary": "卧室检测到跌倒"
    },
    "source": "mock-virtual-gateway"
  },
  // （响应共 28 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 14 告警列表

```bash
$ curl -s http://127.0.0.1:3000/api/alerts?device_id=1\&limit=3 -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "total": 4,
    "items": [
      {
        "id": 4,
        "device_id": 1,
        "node_sn": "M2",
        "type": "fall",
        "level": 2,
        "ts": 1792246175127,
        "status": "pending",
        "push_count": 1,
        "last_push_at": 1792246175190,
        "next_push_at": 1792246235127,
        "summary": "卧室检测到跌倒"
      },
      {
        "id": 2,
        "device_id": 1,
        "node_sn": "M1",
        "type": "gas",
        "level": 1,
        "ts": 1792240457108,
  // （响应共 48 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 15 告警详情（打开即 pending→confirmed）

```bash
$ curl -s http://127.0.0.1:3000/api/alerts/1 -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "id": 1,
    "device_id": 1,
    "node_sn": "M2",
    "type": "fall",
    "level": 2,
    "ts": 1792152557108,
    "event_id": "M0-3F1A2B-fall-1792152557108",
    "status": "resolved",
    "recovered_at": 1792153757108,
    "snapshot": {
      "fall": 1,
      "presence": 1
    },
    "push_count": 1,
    "last_push_at": 1792152557108,
    "created_at": 1792246157110,
    "summary": "卧室检测到跌倒",
    "dispositions": [
      {
        "id": 1,
        "user_id": 1,
        "username": "demo",
  // （响应共 193 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 16 处置告警

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/alerts/2/disposition -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"action":"checked","note":"已电话确认老人安好"}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "alert_id": 2,
    "status": "resolved",
    "action": "checked",
    "disposition_id": 3,
    "ts": 1792246175348
  },
  "ts": 1792246175356
}
```

`HTTP 200`

#### 16b 重复处置 → 1301/409

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/alerts/2/disposition -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"action":"checked"}'
```

```json
{
  "code": 1301,
  "message": "告警已处置",
  "data": null,
  "ts": 1792246175408
}
```

`HTTP 409`

#### 17 活动报表 day（hourly 24 项）

```bash
$ curl -s http://127.0.0.1:3000/api/reports/activity?device_id=1\&period=day -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "device_id": 1,
    "period": "day",
    "from": 1792166400000,
    "to": 1792252799999,
    "total": 36,
    "daily": [
      {
        "date": "2026-10-17",
        "count": 36
      }
    ],
    "hourly": [
      {
        "hour": 0,
        "count": 0
      },
      {
        "hour": 1,
        "count": 0
      },
      {
        "hour": 2,
  // （响应共 116 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 18 活动趋势 7 天

```bash
$ curl -s http://127.0.0.1:3000/api/reports/trend?device_id=1\&days=7 -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "device_id": 1,
    "days": 7,
    "series": [
      {
        "date": "2026-10-11",
        "count": 0
      },
      {
        "date": "2026-10-12",
        "count": 0
      },
      {
        "date": "2026-10-13",
        "count": 0
      },
      {
        "date": "2026-10-14",
        "count": 0
      },
      {
        "date": "2026-10-15",
        "count": 37
  // （响应共 45 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 19 汇总报表 week

```bash
$ curl -s http://127.0.0.1:3000/api/reports/summary?device_id=1\&period=week -H 'Authorization: Bearer $TOKEN'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "device_id": 1,
    "period": "week",
    "from": 1791648000000,
    "to": 1792252799999,
    "activity": {
      "total": 108,
      "active_days": 3,
      "avg_daily": 15.4,
      "peak_hour": 17,
      "min_hour": 11,
      "compare_prev": "上周无活动记录"
    },
    "alerts": {
      "total": 4,
      "by_type": {
        "fall": 2,
        "gas": 1,
        "no_motion": 1
      },
      "resolved": 3,
      "pending": 1,
      "false_alarm": 1
  // （响应共 36 行，此处截断；完整字段见 docs/技术/接口契约.md §6）
```

`HTTP 200`

#### 20 注册推送 token

```bash
$ curl -s -X POST http://127.0.0.1:3000/api/push/register -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"push_token":"demo-push-token-0123456789","platform":"harmony"}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "registered": true,
    "user_id": 1
  },
  "ts": 1792246175597
}
```

`HTTP 200`

#### 21 模拟 IoTDA 属性上报 webhook

```bash
$ curl -s -X POST 'http://127.0.0.1:3000/api/iotda/webhook?token=dev-webhook-token-change-me' -H 'Content-Type: application/json' -d '{"resource":"device.property","event":"report","event_time":"20261017T135000Z","notify_data":{"header":{"device_id":"mock-M0-3F1A2B","node_id":"M0-3F1A2B"},"body":{"services":[{"service_id":"gateway","event_time":"20261017T135000Z","properties":{"temp":26.5,"humi":58.2,"smoke":210,"presence":true,"door":false,"window":false,"battery":255,"alarm_state":1,"nodes_online":"{\"M1\":true,\"M2\":true,\"M4\":true,\"H1\":true}"}}]}}}'
```

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "handled": 4,
    "resource": "device.property",
    "device_id": 1,
    "sn": "M0-3F1A2B",
    "warnings": []
  },
  "ts": 1792246175686
}
```

`HTTP 200`

#### 22 越界参数 → 1002

```bash
$ curl -s -X PUT http://127.0.0.1:3000/api/devices/1/settings -H 'Content-Type: application/json' -H 'Authorization: Bearer $TOKEN' -d '{"items":{"no_motion_min":999}}'
```

```json
{
  "code": 1002,
  "message": "参数超出允许范围",
  "data": {
    "field": "no_motion_min",
    "allowed": [
      5,
      120
    ]
  },
  "ts": 1792246175728
}
```

`HTTP 400`

#### 23 webhook token 错 → 1502/401（响应体最小化）

```bash
$ curl -s -X POST 'http://127.0.0.1:3000/api/iotda/webhook?token=wrong-token' -H 'Content-Type: application/json' -d '{}'
```

```json
{
  "code": 1502,
  "message": "webhook 校验失败"
}
```

`HTTP 401`

## 6. IoTDA 平台侧配置（契约 §5）

1. **数据转发 → 规则**：触发资源勾选 `device.property`（属性上报）、`device.event`（事件）、`device.message`（消息）；
   动作选「推送至 HTTP/HTTPS」，URL 填
   `https://<server_host>/api/iotda/webhook?token=<WEBHOOK_TOKEN>`；**不要**加自定义 Header（§5.2 规定鉴权走查询串）。
2. **Token 轮换**：改 `.env` 的 `WEBHOOK_TOKEN` → `docker compose up -d api` → 同步改转发 URL。
   不匹配会得到 `1502/401`，且响应体最小化（不泄露规则信息）。
3. **一机一密**：网关侧 ClientId `{device_id}_0_1_{YYYYMMDDHH}`、UserName=device_id、
   Password=HMACSHA256(secret, UTC 时间戳) 的 64 位小写 hex（§4.1）。服务器侧用 `devices.iotda_device_id` 反查设备；
   P0 演示用 `mock-<sn>` 形式（`app/mocks/virtual.py`），真机接入时改 `devices.iotda_device_id` 为平台设备 ID。
4. **入库映射**（§5.3）：属性 → `device_online` 快照 + `nodes.online` + `sensor_readings(node_sn='M0',type='gateway')`；
   事件 → `alerts`（`event_id` 唯一键幂等）；消息 → `messages/up` 的 `content` 数组逐条展开成 `sensor_readings`。
   无法识别的 `resource` 一律 **200 + WARN**（不重试、不报错），避免平台无限重推。

## 7. 数据模型与报表口径

- 11 张表：`users`、`devices`、`user_devices`、`nodes`、`sensor_readings`、`alerts`、`alert_dispositions`、
  `activity_hourly`、`activity_daily`、`device_online`、`push_tokens`（契约 §7，DDL = `db/schema.sql`）
- 时间列统一 `DATETIME(3)` 存 **UTC**（连接建立时 `SET time_zone='+00:00'`，compose 里 `--default-time-zone=+00:00`），
  另有 `*_ms BIGINT` 冗余列供 App 直接使用；报表日/小时按 **UTC+8** 切分（SQL 侧 `CONVERT_TZ`）
- 幂等键：`sensor_readings(device_id,node_sn,type,ts_ms)`、`alerts(event_id)` —— 平台重推/重试不会产生重复行
- 报表归并：调度器每 5 分钟增量刷新当天 `activity_hourly/daily`，并在 UTC+8 00:10 后重算前一天（§7.9）；
  `activity_daily.active_minutes` = 当天有过活动的**不同分钟数**
- 告警重推：`[0,60,300]` 秒三档（设备可通过 `alert_repush_s` 覆盖），`status!='pending'`、已恢复、已推 3 次都会停推（§7.7）

## 8. 验证

**契约测试（真 MySQL，不用 SQLite：触发器级别的差异会掩盖问题）**

```bash
# 推荐：容器内跑（依赖、时区、MySQL 版本都与运行环境一致）
docker compose run --rm --no-deps -e DB_HOST=db api python -m pytest

# 宿主机跑（需 python3.12 venv + pip install -r requirements-dev.txt，且 DB_HOST=127.0.0.1）
cd server && python -m pytest
```

- 共 **102 例**：表结构/唯一键（`test_00`）、健康与校时（`test_10`）、注册登录与限流（`test_20`）、
  设备/绑定/阈值/下发/演示（`test_30`）、告警列表/详情/处置/重推（`test_40`）、报表（`test_50`）、
  webhook 三条链路与幂等（`test_60`）、mock 虚拟网关与调度器（`test_70`）
- **测试库隔离**：pytest 会 `TRUNCATE` 业务表，因此 conftest 强制把 `DB_NAME` 指向 `<DB_NAME>_test`
  （库与授权由 `db/init/15-test-db.sh` 首次初始化时建好）。跑测试不会再清掉演示数据；
  若用的是**已存在**的数据卷，执行一次：
  ```bash
  docker compose exec db mysql -uroot -p"$MYSQL_ROOT_PASSWORD" -e \
    "CREATE DATABASE IF NOT EXISTS eldercare_test DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci; \
     GRANT ALL PRIVILEGES ON eldercare_test.* TO 'eldercare'@'%'; FLUSH PRIVILEGES;"
  ```
- **无 docker 时的退路**：`pip install -r requirements-dev.txt` 后照常 `pytest`（需可达的 MySQL 8）；
  纯离线冒烟可 `uvicorn app.main:app` + `curl /api/health`（DB 不可用时 health 返回 503 是预期行为）
- 契约的机器可读断言（验收员工维护）：`tools/test/contract/contract_source.py` 逐条解析契约；
  本目录的 DDL/字段命名按它可被断言（表名、唯一键名见 `tests/test_00_schema.py`）

## 9. 契约口径与差异（已登记，待 PM 确认）

| # | 事项 | 现状与理由 |
|---|---|---|
| 1 | 技术栈 FastAPI ≠ 契约 §1 示意图的 Node.js | 任务卡 t5 指定 FastAPI；实施计划 §「后端」写「Node.js 或 Python（FastAPI）」，取 FastAPI |
| 2 | `devices.settings_json` 是**增补列** | §7 未列该列；阈值（§6.9）需要持久化，用 JSON 列最贴合「键值可扩展」的需求 |
| 3 | `/api/time` 带**附加**字段 | 契约 §6.21 只声明必现两键（`utc_epoch`/`iso8601`）；实现另返回 `utc_epoch_ms`/`tz`/`server_ts`（同一时刻的毫秒、时区提示、响应时刻），不改必现键的语义。`Cache-Control: no-store` 按 §6.21 定稿写死 |
| 4 | `/api/reports/trend` 文案（v1.2 已定稿，**不再是差异**） | §6.17 v1.2 拆成 `compare.title`（固定「今日活动频次」）+ `compare.text`（规则行「较昨日下降/上升 N%」「与昨日基本持平」）；无昨日基线时省略 `yesterday`/`delta_pct`/`text`，只留 `title`+`today`。边界值算升降（恰好 ±10% 说升降），`delta_pct` 为**整数**（四舍五入，非银行家舍入） |
| 5 | `/api/reports/summary` 无数据时 `compare_prev` | 契约只给示例「较上周下降 5%」；无上期数据时返回「上周无活动记录」 |
| 6 | PUT settings 用 `{items:Ellipsis}` 包裹 | §6.10 请求体示例为 `{"items":{...}}`；也兼容裸 dict 与 `{settings:{...}}` |
| 7 | 下发阈值的命令名 | §4.5 只有 `set_report_interval`，没有「下发阈值」命令；现用 `set_threshold{cmd,key,value}`（对应 §3.4 `0x11` set 帧）逐键下发 |
| 8 | 错误响应体里的 `data` | 无细节时为 `null`（信封结构稳定，App 一套解析路径）；业务可选字段（如 `recovered_at`）则**省略键**（§0.4） |
| 9 | `/api/health.db` 取值 | 按 §6.20 字面 `ok`（§9.1 只说不可用时 `down`） |
| 10 | webhook 收到但匹配不到设备 | 返回 200 + WARN（§5 规定无法识别一律 200），靠 `webhook_last_at` 与日志排查 |
| 11 | §6.9 `items` 为 **21 项**，`gas_l1_to_l2_s` 不在其中 | PM D-01 裁定：该键是 §8.2 算法参数（P0 固定 60 s），不进 `items`、PUT 即 `1003` 未知键；要开放运行时配置须走契约变更。列为 `settings_keys.ALGORITHM_PARAMS` 备查 |
| 12 | §6.11 `accepted`/`rejected` 为**字符串数组**（元素=键名） | PM D-05 裁定（契约 939 行示例 + 942 行注）；被拒原因另放可选 `reasons`（键名→原因），无拒绝项则省略该键。App 侧 `Types.ets` 已按 `string[]` 实现，服务器对齐 |
| 13 | §6.13 `status` 未知取值 → `1001`；§6.14 `dispositions[].ts` | 均按契约字面收紧（原先分别是 1002 与 `at`，见 t18 复核 F-02/F-03）；App 详情页按 `ts` 取处置时间 |

## 10. 安全与交付要求

- **不提交密钥**：仓库只有 `.env.example`（占位符）；`.env` 在 `.gitignore` 里；真实 IoTDA/Push 凭据只放部署机环境变量
- 口令库：bcrypt cost=10（§7 users.password_hash）；JWT 泄露等价于账号泄露，生产必须换成随机 32 字节以上密钥
- 登录限流：同 IP 5 分钟 >100 次 → `1105/429`（§9.1）；`/api/auth/login` 不区分用户是否存在（防枚举）
- 设备越权：所有设备相关接口都过 `user_devices` 归属校验，非成员 `1104/403`，无设备 `1200/404`
- webhook：`hmac.compare_digest` 常量时间比较 token；失败响应体最小化（不返回任何设备/规则信息）
- 容器：不用 host 网络、不用 privileged、端口只绑 127.0.0.1、项目名 `hmprogram`（AGENTS.md 约定）

## 11. 待办与需要用户/PM 提供的信息

| 项 | 说明 |
|---|---|
| CAL-04 | IoTDA 应答 topic 与规则引擎 HTTP 推送报文需**真机抓一条**：首次联调时把原始 webhook body 存到 `docs/评审与验证/`，回填 §5.3 |
| CAL-07 | 网关 SN / IoTDA device_id / 绑定码由 PM 分配，写入部署机 `.env`（不进仓库） |
| CAL-09 | 节点 `ts` 可信前 `/api/health.clock_source` 必须为 `server`；固件实现 §3.4 `0x13` 校时后改 `CLOCK_SOURCE=device` |
| Push Kit | P1：`PUSH_MODE=real` 需要 App 侧 client_id/secret（用户提供） |
| 真机联调 | 当前下行命令为 `mock` 模式：命令流水可见、不会真正到达网关；接真机后置 `IOTDA_DISPATCH_MODE=real` |
| 公网部署 | 服务器公网部署需要用户先给 **Q8**（云服务器系统版本 / SSH 方式 / 公网 IP / 安全组端口）——准备与施工单见 §12 |

## 12. 公网部署（生产口径）

给 App 提供 REST、并接收 **IoTDA 规则引擎 webhook 回调**，服务器必须在公网可达。生产用一份独立的 compose：

```bash
cd server
cp .env.example .env                            # 填 5 个必填项：DB_PASSWORD / MYSQL_ROOT_PASSWORD / JWT_SECRET / WEBHOOK_TOKEN / PUBLIC_HTTP_PORT
docker compose -f docker-compose.prod.yml up -d --build
docker compose -f docker-compose.prod.yml ps    # db+api healthy、proxy Up
curl -s http://127.0.0.1:3000/api/health        # {"code":0,... "db":"ok"}
```

| 文件 | 作用 |
|---|---|
| [`docker-compose.prod.yml`](docker-compose.prod.yml) | 生产栈：**db 不发布任何宿主机端口**（公网碰不到 3306）、api 只绑 `127.0.0.1:3000`、只有 nginx 反代对外、每服务有内存上限与日志轮转 |
| [`deploy/nginx.prod.conf`](deploy/nginx.prod.conf) | 唯一对外入口（80）：`/api/` → api:3000，`/` 一律 404；`server_tokens off` |
| [`deploy/hmprogram-server.service`](deploy/hmprogram-server.service) | 可选 systemd unit：开机自启 + `systemctl status/restart`（不装它则靠 compose 的 `restart: unless-stopped`） |
| [`deploy/部署施工单.md`](deploy/部署施工单.md) | 从零到 `/api/health` 通的施工单（每段 ≤3 步、含失败对策与**回滚**步骤） |
| [`deploy/IoTDA回调前置清单.md`](deploy/IoTDA回调前置清单.md) | 公网可达/端口/路径/鉴权口径 + 平台侧配置项 + 需要用户提供的信息（Q8） |
| [`tools/db-backup.sh`](tools/db-backup.sh) / [`tools/db-restore.sh`](tools/db-restore.sh) / [`tools/db-rows.sh`](tools/db-rows.sh) | 备份（mysqldump 在 db 容器内执行）→ 清库恢复 → 逐表行数比对一致；产物落 `server/backups/`（已 gitignore） |

- 与开发 compose **同项目名 `hmprogram`**（同一套容器名与数据卷）：切换时先 `docker compose down` 再起另一份，不要同时跑。
- 脚本用 `bash tools/db-backup.sh` 调用（仓库里模式位 644，克隆后没有执行权限）；备份产物在 `server/backups/`，默认留 14 份。
- 硬约束：只动本项目容器；不 `docker system prune`、不改系统自带 nginx、不用 host 网络。
- 真实公网部署需要用户确认（Q8）后再做；本机只做本地实测。
