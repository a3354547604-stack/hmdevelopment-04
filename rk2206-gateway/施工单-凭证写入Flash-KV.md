# 施工单：给网关写入 IoTDA 凭证（不重编固件）

> 这张单子是给**现场操作的人**看的。你不需要装编译环境、不需要改代码、不需要重新烧固件 ——
> 只要一根 USB 串口线 + 一个串口终端，敲 3 步就把凭证写进板子。
>
> 涉及任务：t34（凭证 Flash KV + 串口命令 `gwkv`）。前置：板子已烧好带本功能的固件
> （`rk2206-gateway/out/gateway-Firmware.img`，t34 提交后编译产物）。
>
> ⚠ 未上板实测的那部分（本单里的串口交互）**以你回传的日志为准**；主机侧能自证的部分见第 6 节。

---

## 0. 先看一张表：每一步"你该看到什么"

| 步骤 | 你输入/动作 | 期望在串口里看到（原文） | 看不到时先看什么 |
|---|---|---|---|
| 1.1 | 打开串口终端，波特率 **115200**，8-N-1，**关流控** | 能看到启动日志里的 `[gw] ---- RK2206 网关 M0 启动 ELDERCARE-GW-M0-v1.2 (契约 v1.2) ----` | 串口号选错（换一个 COM）；终端没关流控；板子没复位 |
| 1.2 | 敲一次回车 | 出现 `OHOS #` 或 `#` 之类的 shell 提示符 | 固件不是 t34 之后的版本（重新烧 `gateway-Firmware.img`） |
| 1.3 | `gwkv show` + 回车 | `[kvcfg] 凭证来源：**编译期默认值**（KV 不可用：E_EMPTY(8) …）` | 若显示 `Flash KV`，说明之前已经写过凭证，可直接跳到第 3 步 |
| 2.1 | 在华为云控制台抄三个值 | 接入地址 / 设备 ID / 设备密钥（见 2.0） | 设备没建好：IoTDA → 设备 → 注册设备 → 选 MQTT |
| 2.2 | 主机上跑 `gw-kvcfg.py --line …` | 一行 `gwkv set host=… device=… secret=…` | 校验不通过时脚本会直接说哪个参数不对（host 带空格最常见） |
| 2.3 | 把那一行粘进串口 + 回车 | `[kvcfg] OK：已写入 Flash KV 0x6FF000（256 字节，读回逐字节一致）` + 摘要 | 见第 5 节错误码表 |
| 3.1 | 按板子上的**复位键** | 启动日志里 `凭证来源=Flash KV：host=… device=… time=…` | 若又回到"编译期默认值"，说明第 2 步其实没写成功（第 5 节 E_FLASH/E_VERIFY） |
| 3.2 | `gwkv show` | 接入地址/设备 ID/时间服务器 + `密钥 : 32 字节（指纹 XXXXXXXX，不回显）` | 指纹与第 2.2 步脚本打印的指纹**必须一致** |
| 3.3 | 等 30–60 s | 日志出现 `[gw] IoTDA 已连接 clientId=<设备ID>_0_0_<YYYYMMDDHH>` | 见 `施工单-网关上板与上云验证.md` 的失败表（网络/时间/密钥） |

**一句话**：第 1 步确认能进 shell，第 2 步写入，第 3 步复位后确认生效。

---

## 1. 第 1 步：接好线，进串口 shell（3 小步）

1. 用 USB 线把 **RK2206 小凌派**接到电脑；如果电脑上还没装驱动，先跑
   `device/rockchip/tools/windows/DriverAssitant/DriverInstall.exe`（SDK 里自带的那个）。
2. 打开串口终端（MobaXterm / 串口助手 / `tools/serial-capture.ps1` 都行），选**COM 口**、
   波特率 **115200**、8-N-1、**数据流控关闭**。
   - 不知道是哪个 COM：PowerShell 里跑
     `Get-CimInstance Win32_PnPEntity | Where-Object {$_.Name -match '\(COM\d+\)'} | Select-Object Name`
   - 也可以在 WSL 里直接抓 20 秒原文：
     `powershell.exe -File tools/serial-capture.ps1 -Port COM20 -Seconds 20 -Out rk2206-gateway/out/boot-COM20.log`
3. 敲一次**回车**，看到 shell 提示符（形如 `OHOS #`）就说明通了。

> 为什么要有 shell：`gwkv` 命令是挂在 SDK 自带 shell 上的，走的是**调试串口**，
> 与契约 §3 里汇聚↔网关那条二进制 UART 是**两个不同的口/方向**，互不干扰。

## 2. 第 2 步：写入凭证（3 小步）

0. **去哪里抄这三个值**：华为云控制台 → IoTDA（设备接入）→ 左侧「设备」→ 点你建的那台设备 →
   「详情」里有 **设备 ID** 与 **设备密钥**（密钥只在创建时完整显示，丢了就点「重置密钥」）；
   **接入地址**在「总览」页的"接入信息"里（形如 `xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com`）。
1. 在**电脑**（WSL 或装了 Python 的机器）上跑一次工具，让它帮你把命令拼好、顺手校验：
   ```bash
   python3 rk2206-gateway/tools/gw-kvcfg.py --line \
       host=<接入地址> device=<设备ID> secret=<设备密钥> time=<网关所在局域网的服务器IP>
   ```
   - `time=` 可以不给：不给就走公网 NTP；给了就用你自建的时间服务器（默认 `192.168.1.100:8000`）。
   - 脚本会打印 **校验结果 + 一行可直接粘贴的命令 + 密钥指纹**。指纹记下来，第 3 步要对。
2. 把那一行**原样粘贴**进串口终端（注意不要换行/多空格）并回车：
   ```
   gwkv set host=xxxxxx.iot-mqtts.cn-north-4.myhuaweicloud.com device=<设备ID> secret=<设备密钥> time=192.168.1.100
   ```
3. 看到 `[kvcfg] OK：已写入 Flash KV 0x6FF000（256 字节，读回逐字节一致）` 就算写成功。
   命令是**部分更新**：只想改设备 ID 时，`gwkv set device=新ID` 就行，host/secret 会沿用板子里已有的值。

> 密钥**不会**回显：串口只会打印 `密钥 : N 字节（指纹 XXXXXXXX，不回显）`。
> 也正因如此，`gwkv` 只存在于物理串口上，不走 t10 的 Wi-Fi 配网 HTTP/H5（那里只配 Wi-Fi 与节点白名单）。

## 3. 第 3 步：复位并确认生效（3 小步）

1. 按一下板子上的**复位键**（凭证只在启动时读一次）。
2. 启动日志里应当出现：
   ```
   [gw] 凭证来源=Flash KV：host=<接入地址> device=<设备ID> time=<时间服务器>
   ```
   如果看到的是 `⚠ 凭证来源=编译期默认值（KV 不可用：…）`，说明凭证没生效 → 回第 2 步。
3. 敲 `gwkv show` 复核：**指纹要和第 2 步脚本打印的一致**（指纹一致 = 密钥一字不差）。

---

## 4. 复核与"和主机对齐"（可选，但推荐）

| 命令 | 用途 |
|---|---|
| `gwkv show` | 看当前生效凭证 + 来源（Flash KV / 编译期默认值） |
| `gwkv dump` | 打印 KV 块 256 字节十六进制 —— 与主机工具 `--block` 的十六进制**逐字节比对**，证明两种实现布局一致 |
| `gwkv clear` | 擦掉凭证；重启后回落编译期默认值（**连不上云**，这是有意的） |
| `gwkv help` | 用法提示 |

主机侧对照：
```bash
python3 rk2206-gateway/tools/gw-kvcfg.py --block host=<接入地址> device=<设备ID> secret=<设备密钥> time=<时间服务器>
```

## 5. 失败对照表（串口里会直接打印错误码）

| 看到 | 含义 | 怎么办 |
|---|---|---|
| `E_PARAM(1)` 缺少 host=/device=/secret= | 必填项没给 | 三项都要给（或先用 `gwkv show` 确认板子里已有） |
| `E_PARAM(1)` 不认识的键 | 键名写错 | 只认 `host/port/device/secret/time/timeport` |
| `E_LEN(2)` 某个值超长 | host/device/secret ≤63 字节，time ≤47 | 抄错了，回去核对控制台 |
| `E_CHAR(3)` 含非法字符 | 值里有空格、引号、中文标点 | 最常见：复制时带了空格。用工具的 `--line` 生成命令 |
| `E_PORT(4)` | `port=`/`timeport=` 不在 1..65535 | IoTDA 明文 MQTT 是 `1883` |
| `E_SECRET(5)` 密钥太短 | 少于 8 字节 | 华为云密钥通常是 32 位十六进制，确认没漏抄 |
| `E_FLASH(6)` 擦除/写入失败 | Flash 失效或扇区被占用 | 重试一次；仍然失败就是硬件/分区问题，回传完整日志 |
| `E_VERIFY(7)` 读回校验不符 | 写进去的和读出来的不一致（介质坏） | 重试；仍失败回传日志 |
| `E_EMPTY(8)`（`show` 里） | KV 区从未写过或刚 `clear` | 正常，按第 2 步写一次 |
| `E_CRC(9)`（启动日志里） | 块被写坏（掉电中断） | 重新 `gwkv set` 一次 |
| `E_VER(10)` | 不是本版本固件写的块 | 用当前固件的 `gwkv set` 重写 |

### 三条"坏情况"的确定行为（验收要求）

1. **缺凭证**：`gwkv show`/启动日志显示来源=**编译期默认值** + 原因码；板子照常启动、照常跑本地规则与缓存，
   只是连不上云（默认值里的设备 ID/密钥是占位值）。**不会**出现"用半截凭证去连、失败得莫名其妙"。
2. **写失败（E_FLASH）**：`set` 打印失败原因 + `影响：Flash 里仍是上一份凭证（没写成功就不算改过）`；
   重启后 `gwkv show` 仍是上一份。
3. **读回校验不符（E_VERIFY）**：`set` 报失败，**不会**打印 OK；板子下次启动如果块已损坏，
   走第 1 条（回落默认值 + 打印原因）。

---

## 6. 不需要板子就能验的部分（本任务已自证）

```bash
# ① 主机侧工具自测（与固件两份独立实现，同一组向量）
python3 rk2206-gateway/tools/gw-kvcfg.py --selftest
python3 rk2206-gateway/tools/gw-kvcfg.py --check-vectors rk2206-gateway/test/gw_kvcfg_vectors.inc

# ② 固件纯逻辑自测（含 test_kvcfg：布局/CRC/落盘/命令/三条负向）
bash tools/build-lock.sh bash tools/gw-host-test.sh

# ③ 整条固件构建（断言：17+ 个源文件全部编译、符号与锚点串进镜像）
bash tools/build-lock.sh bash tools/gw-build.sh gateway
```

**只有板上才能验的**：真 Flash 的擦写（4 KB 扇区对齐）、串口 shell 里敲 `gwkv` 的实际交互、
复位后凭证是否真的生效（也就是本单第 1–3 步）。

## 7. 现场前提

- 板子必须**已经联网**（Wi-Fi 账密走 SDK 的 `config_network.h`，编译期决定；见
  `施工单-配网上板与本地API验证.md` 的现场前提）。
- 电脑侧"网关所在局域网的服务器 IP"需要提前确定（用于 `time=`）；不给就用公网 NTP。
- 本单**不涉及**烧录；凭证写入不改变固件，所以反复改不用重新烧。
