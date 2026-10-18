# legacy/ —— 不再参与构建、但有溯源价值的文件

这个目录只回答一个问题：**「这个东西以前是干什么的、被谁取代了、现在还能拿来对照什么？」**
凡是仍然参与构建、或被文档当依据引用的文件，都**不**放在这里（见文末那节）。

本目录的设立来自 t52（t43 复核 t3 的 finding F-02：`ws63-firmware/src/` 里有未跟踪散落文件）。

## HsTest.cs

| 项 | 值 |
|---|---|
| 原名/原路径 | `ws63-firmware/src/HsTest.cs` |
| 现在 | `ws63-firmware/legacy/HsTest.cs` |
| 体积 / 时间 | 4984 B，2026-10-16 01:29 |
| sha256 | `3d8c5d4781a4568618c8487dfc8f7937ed057f775dcbce36fed7840b5aa59f2f` |

**是什么**：WS63 ROM bootrom「高速握手」（`START_FLAG = 0xDEADBEEF`、`TYPE_ROMBOOT_HS = 0xF0`、
`INTER_DATA_LEN = 0x12`、CRC16-CCITT）在 PC 侧（.NET + `System.IO.Ports`）的**首次验证实现**。
2026-10-16 那次「单板直连、把自研固件烧进去」的通宵实验就是用它跑出来的。

**被谁取代**：两个**仍在维护**的实现沿用了它验证过的时序，并且都写明出处：

- `ws63-firmware/flasher/ws63flash.cs:98`（`f[7] = 0x00; // verified: the ROM accepts pad=0 (HsTest)`）
  与 `:478`（`// --- reset the board (mirrors the verified HsTest sequence exactly) ---`）；
- `tools/ws63flash-port.cs:109`、`:493`（同两句注释）。

**为什么不是"删掉就完了"**：它是有出处的"最初那份能跑通的实现"，
烧录时序一旦以后要改（`pad` 字节、复位序列），对照它能分清"原厂就这么要求"和"我们后来试出来的"。
所以搬进 `legacy/` 而不是删除；同时它**不是固件源码**，不该躺在 `src/` 里。

## HsTest.exe（已删除，只留指纹）

| 项 | 值 |
|---|---|
| 原名/原路径 | `ws63-firmware/src/HsTest.exe` |
| 体积 / 时间 | 7680 B，2026-10-16 01:29 |
| sha256（删除前） | `f90eb97e7363a3e7b0506f78b1895886c4764627e7a5e36aeffa82c6414ef692` |
| 处置 | **t52 删除** |

删它的理由（三条，缺一条我都会选择留下它）：

1. 它是 `HsTest.cs` 的编译产物，**没有独立信息**；同一个文件用 `csc HsTest.cs` 就能重建；
2. 全仓 grep **没有任何脚本或文档以文件路径引用它**——`ws63flash.cs` / `ws63flash-port.cs` 里出现的
   "HsTest" 指的是**那段时序**，不是这个可执行文件；
3. 仓库里不放二进制构建产物（这正是复核 finding 点名的那件事：`.exe` 躺在源码树会干扰复核与打包）。

sha256 留档的作用：`ws63-firmware/doc/2-烧录成功日志-2026-10-16.txt` 一类历史日志是它跑出来的，
若以后要确认某份日志出自哪个二进制，可以拿这个哈希比对（重建产物字节不一定相同，
所以这里留的是"当时那一份"的指纹，不是可复现性判据）。

## 没有搬到这里、而是"就地入库"的两个文件

`src/sensor_hub.c`、`src/wifi_hub.c` 一开始也在 F-02 的名单里，但 grep 之后**判定不能搬**：

**① 它们被文档当依据引用**（搬走会让引用失效）：

| 引用处 | 引的是什么 |
|---|---|
| `docs/技术/接口契约.md:1309` | 采样周期 2 s「沿用 `sensor_hub.c` `LOOP_INTERVAL_MS=2000`」 |
| `docs/技术/接口契约.md:1520` | §11 一致性比对第 2 行：`sensor_hub.c:46` `FW_ADC_CH_FIRST=2` 只采 ch2–ch5 |
| `docs/技术/接口契约.md:1521` | §11 第 3 行：`sensor_hub.c:119` 的 `ADC ch%d (GPIO_%02d)` 通道↔引脚映射 |
| `docs/技术/接口契约.md:1522` | §11 第 4 行：`sensor_hub.c:129-131` 只探 `GPIO_00~GPIO_04`（05/06 已让给 WS2812B 与雷达 TX2） |
| `docs/技术/接口契约.md:1528` | §11 第 10 行：`wifi_hub.c` 仅扫描验证，量产固件不需要 WiFi（D-03） |
| `docs/技术/引脚与接线表.md:74`、`:539` | 「依据清单（全部可查）」直接点名 `ws63-firmware/src/{sensor_hub.c,radar_hub.c,sle_hub.c}` |
| `docs/技术笔记/串口查看与日志判读.md:86` | 串口日志里的固件横幅 `WS63 SELF-BUILT FIRMWARE (sensor_hub)` |
| `README.md:63` | `src/` 的目录说明 |

**② `src/CMakeLists.txt:24-26` 明确写了「保留文件不动」**：

```
#   sensor_hub.c / wifi_hub.c  —— 早期诊断固件（ADC/GPIO/UART/WiFi 扫描）。各自带 app_run，
#                                 编进来会多起两个任务刷屏。保留文件不动，需要时单个加回清单。
```

搬走它们等于**背着构建清单改语义**（那句注释会说谎），而本卡不允许改 `src/` 下的文件。

**所以对这两个文件的处置是「就地入库」**：内容一字未改，只是从"未跟踪"变成"已跟踪"。
这样 F-02 的实质问题（未跟踪散落、会被复核/打包误判）消失，而文档引用与构建清单注释全部保持有效。

| 文件 | 体积 | sha256（入库时） |
|---|---|---|
| `ws63-firmware/src/sensor_hub.c` | 6565 B | `831852b5d09a1da9ab38a5518a094eda00a844d68811f7cb0aa34058a678d2d3` |
| `ws63-firmware/src/wifi_hub.c` | 4612 B | `fae3bb412d6f08fcddedae8340e80b27ce9ff046881383d896c6ef5628009de3` |

**它们现在还会被编进固件吗**：不会。节点/汇聚固件按 `ws63-firmware/scripts/node-targets.txt` 的**显式清单**
拷贝源文件（`node-build.sh` 只 `cp "$SRC_DIR"/*.h` + 清单里列出的 `.c`），
这两个文件不在任何清单里 —— 与 `src/CMakeLists.txt:23-26` 的说法一致。
