# IoTDA 规则引擎回调本服务器的前置清单（t41）

用途：P0 链路里，云端（华为云 IoTDA）要把设备属性/事件/消息**推**到本服务器，
所以服务器必须能从公网被访问。本文件列出「要满足什么」与「要用户给什么」。
平台侧的点选步骤见 `server/README.md` §6（那里是从「服务器已就绪」开始的）；本文件只管**部署与网络前置**。

---

## 1. 公网可达（硬前置）

| 项 | 要求 | 怎么验证 |
|---|---|---|
| 公网 IP 或域名 | 云服务器有公网 IP（NAT/EIP） | 控制台能看到；外部 `curl` 打得到 |
| 端口放通 | 云安全组 + 系统防火墙放通 `<PUBLIC_HTTP_PORT>`（默认 80；被占用就 8080） | 在家用手机热点：`curl -s -o /dev/null -w '%{http_code}' http://<公网IP>:<端口>/api/health` → `200` |
| 入口进程 | nginx 反代（compose 里的 `proxy`）唯一对外；api 只绑 `127.0.0.1` | `docker compose -f docker-compose.prod.yml ps` 中 proxy 有 `0.0.0.0:<端口>->80`，api 是 `127.0.0.1:3000->3000` |
| MySQL 不外露 | db 不发布端口 | `docker compose -f docker-compose.prod.yml port db` 无输出；`ss -ltn \| grep 3306` 无输出 |
| HTTPS（可选） | 平台允许 HTTP 转发即可；若要求 HTTPS，需要域名 + 证书（见 `deploy/nginx.prod.conf` 末尾注释） | `curl -I https://<域名>/api/health` |

- 回调地址固定为：`http(s)://<公网IP或域名>[:端口]/api/iotda/webhook?token=<WEBHOOK_TOKEN>`
  （路由在 `app/routers/webhook.py`，前缀 `/api/iotda` + `/webhook`；默认端口 80 时端口可省略）
- 外部脚本/平台可能被运营商拦 80：**演示前一定要从外网验证一次**，不要只在服务器上 curl `127.0.0.1`。

## 2. 鉴权与报文口径（服务器已经实现，配置时别搞错）

| 项 | 口径 | 出处 |
|---|---|---|
| 鉴权方式 | **只**用 URL 查询串 `?token=<WEBHOOK_TOKEN>`，常量时间比较 | 契约 §5.2；`app/routers/webhook.py:25-29` |
| 不加自定义 Header | 平台不方便带自定义头，别配 | 同上 |
| 失败响应 | 不匹配/未带 → `1502/401`，且响应体最小化（不告诉对方是「错了」还是「没带」） | `app/errors.py` |
| 方法 | 只接受 `POST`（`@router.post`）；用 GET 打会 405 | `app/routers/webhook.py:25` |
| 请求体 | UTF-8 JSON 对象，≤ 256 KB；`Content-Type: application/json` | `app/routers/webhook.py:31-42` |
| 必读字段 | 只用 `resource`、`notify_data.header.device_id`、`notify_data.body.services[]` | 契约 §5.2 |
| 无法识别 | 一律 **HTTP 200** + 日志 WARN（避免平台无限重推） | 契约 §5；`README.md` §6.4 |
| 观测 | `/api/health` 的 `webhook_last_at` 非空 ⇒ 有回调进来**且匹配到了设备**（没匹配上时它不会更新，日志里是 `webhook 无法匹配设备`） | `app/routers/push.py:83-84`、`app/services/telemetry.py:286-291` |
| 设备匹配 | 先按 `notify_data.header.device_id` = `devices.iotda_device_id` 查；查不到再用 `header.node_id` / 属性里的 `device_sn`、`gateway_sn` 按 `devices.sn` 兼容匹配 | `app/services/telemetry.py:48-64` |

**上线前自测一条**（把 token 换成真实值；这是平台会发的报文形状的简化版）：

```bash
curl -s -X POST "http://<公网IP>:<端口>/api/iotda/webhook?token=<WEBHOOK_TOKEN>" \
  -H 'Content-Type: application/json' \
  -d '{"resource":"device.property","notify_data":{"header":{"device_id":"<平台设备ID>","node_id":"M0-3F1A2B"},"body":{"services":[{"service_id":"gateway","properties":{"hb":1}}]}}}'
# 期望 {"code":0,...,"data":{"handled":N,"device_id":<id>,"sn":"M0-3F1A2B"}}；
# 随后 curl -s http://127.0.0.1:3000/api/health 里出现 webhook_last_at
# 若 code=0 但 warnings 里是 device_not_found：报文进来了但没匹配上设备
#   —— P0 的 devices.iotda_device_id 还是 NULL，改用 header.node_id = 网关 SN（本清单实测走通的就是这条兼容路径）
```

## 3. 平台侧要配的项（点选清单）

1. **触发资源**：`device.property`（属性上报）、`device.event`（事件）、`device.message`（消息）。
2. **动作**：选「推送至 HTTP/HTTPS」；URL 填 §1 那条；方法 `POST`；**不要**加自定义 Header。
3. **Token 轮换**：改 `.env` 的 `WEBHOOK_TOKEN` → `docker compose -f docker-compose.prod.yml up -d api` → 同步改规则 URL。
4. **设备侧映射**（真机接入时才需要）：把 `devices.iotda_device_id` 更新成平台设备 ID（P0 实测时它是 NULL，靠 SN 兼容匹配兜住）；一机一密 ClientId/UserName/Password 见 `README.md` §6.3。
5. **转发失败重试**：平台侧默认会重试，服务器对无法识别的报文一律回 200，所以**不要**在平台上把 200 之外的码当成功。

## 4. 需要用户提供的信息（Q8 + 部署细节）

| # | 要的东西 | 为什么要 |
|---|---|---|
| 1 | 云服务器**系统与版本**（Ubuntu 22.04 / Debian 12 / CentOS…） | 决定装 docker 的命令与防火墙工具（ufw / firewalld） |
| 2 | **SSH 方式**：公网 IP、端口、用户名、口令或密钥文件、是否允许 `sudo` | 上机执行 `deploy/部署施工单.md` |
| 3 | 公网 IP 是否已备案/是否被运营商封 80（如已封，用 8080） | 决定 `PUBLIC_HTTP_PORT` |
| 4 | 云安全组当前放通的端口清单 | 避免与别的项目抢端口，确认能放通我们的端口 |
| 5 | 这台机器上是否已有别的项目在用 80 / 3306 / 3000 | 硬约束：不干扰其它项目容器 |
| 6 | 是否有域名与证书（可选） | 若平台强制 HTTPS，需要域名 + 证书才能配 443 |
| 7 | 内存/磁盘可用量（`free -m` / `df -h`） | 本机 WSL 只有 7.4 GB，云服务器也要确认 ≥2 GB 内存 |
| 8 | IoTDA 侧：`device_id` 命名规则（是否 `mock-<sn>`）、规则引擎是否已建、是否有 CAL-07 的值（设备 ID/绑定码） | 真机联调与 `IOTDA_DISPATCH_MODE=real` 需要 |

> **本卡不做**（等用户确认后再做）：连他的公网服务器、改云安全组、在 IoTDA 控制台点规则、动 DNS/证书。
