# FRDM-MCXE31B 外设综合 Demo

基于 NXP **FRDM-MCXE31B** 评估板，演示 3 路独立 FlexCAN 及 1 路 LPUART2（MIKROE 接口）的基本收发功能。

---

## 硬件平台

| 项目 | 参数 |
|---|---|
| 评估板 | FRDM-MCXE31B |
| MCU | NXP MCXE31B，ARM Cortex-M7，160 MHz |
| SDK | MCUXpresso SDK 2025.12.00 |
| 开发工具 | Keil MDK |

---

## 功能概览

| 外设 | 功能 |
|---|---|
| FlexCAN 0 / 1 / 2 | 每路独立收发，每秒发送一帧计数帧，接收所有标准帧并打印 |
| LPUART2（MIKROE） | 每秒发送一条计数字符串；收到字节后原样回显并打印到调试串口 |
| LPUART5（调试串口） | 115200 bps，打印所有运行日志 |

---

## 引脚分配

### FlexCAN

| 通道 | 外设 | TX 引脚 | RX 引脚 |
|---|---|---|---|
| CAN0 | FLEXCAN_0 | PTA7 | PTA6 |
| CAN1 | FLEXCAN_1 | PTA11 | PTA12 |
| CAN2 | FLEXCAN_2 | PTE24 | PTE25 |

### LPUART2（MIKROE 接口）

| 方向 | 引脚 |
|---|---|
| TX | PTE12（LPUART2_TX-MIKROE） |
| RX | PTD17（LPUART2_RX-MIKROE） |

### 调试串口

| 方向 | 引脚 |
|---|---|
| TX | PTE14 |
| RX | PTE3 |

波特率：**115200 bps，8N1**

---

## 参数配置

所有常用参数集中在 `source/main.c` 顶部，修改后重新编译即可，无需改动其他文件。

### CAN 参数（每路独立）

```c
/* CAN0 */
#define CAN0_TX_ID      0x100U      // 发送帧 ID
#define CAN0_BITRATE    500000U     // 仲裁段波特率，单位 bps
#define CAN0_USE_CANFD  0           // 0=Classic CAN，1=CAN FD
#define CAN0_FD_BITRATE 2000000U    // FD 数据段波特率（仅 CAN FD 模式有效）

/* CAN1、CAN2 同理，修改对应前缀即可 */
```

### 公共参数

```c
#define TX_PERIOD_MS    1000U   // 所有 CAN 通道的发送周期，单位 ms
```

### UART2 参数（`source/uart2.c` 顶部）

```c
#define UART2_BAUDRATE      115200U     // 波特率
#define UART2_TX_PERIOD_MS  1000U       // 定时发送周期，单位 ms
```

---

## 调试串口输出说明

上电后调试串口会首先打印各通道配置摘要，随后持续输出收发日志。

### 启动信息示例

```
========================================
  3x FlexCAN Demo  —  MCXE31B
========================================
  CAN0: 500kbps  Classic CAN  TX=0x100  period=1000ms
  CAN1: 500kbps  Classic CAN  TX=0x101  period=1000ms
  CAN2: 500kbps  Classic CAN  TX=0x102  period=1000ms
  RX: all standard frames
========================================

[CAN0] ready
[CAN1] ready
[CAN2] ready
[UART2] init  115200bps  PTE12=TX  PTD17=RX
```

### 运行日志格式

| 日志 | 说明 |
|---|---|
| `[CAN0] TX  id=0x100  data=0x00000005` | CAN0 发送，data 为递增计数值 |
| `[CAN1] RX  id=0x100  len=8   data: 00 00 00 05 00 00 00 00  ts=1234` | CAN1 收到帧，显示 ID、字节长度、逐字节数据、硬件时间戳 |
| `[UART2] RX: 0x41 'A'` | UART2 收到字符 `A` |

> **注意**：CAN 默认关闭自接收（`disableSelfReception = true`），各通道不会收到自己发出的帧。

---

## CAN FD 使用方法

将对应通道的 `CAN_USE_CANFD` 改为 `1`，并确认总线对端设备也支持 CAN FD：

```c
#define CAN2_USE_CANFD      1
#define CAN2_BITRATE        500000U     // 仲裁段 500 kbps
#define CAN2_FD_BITRATE     2000000U    // 数据段 2 Mbps
```

CAN FD 模式下发送帧固定为 64 字节有效载荷（DLC=15），数据第 0 字节填入发送计数值，其余为 0。

---

## UART2 收发说明

### 定时发送
MCU 每隔 `UART2_TX_PERIOD_MS`（默认 1 s）通过 PTE12 发送一条字符串：
```
UART2 cnt=0
UART2 cnt=1
...
```

### 接收与回显
PTD17 收到任意字节后：
1. 通过 PTE12 原样回发（echo）
2. 在调试串口打印 `[UART2] RX: 0xXX 'c'`

### 回环自测方法
用跳线短接 **PTE12 → PTD17**，MCU 会收到自己发出的字符串并在调试串口打印。测试完毕后拔除跳线即可。

---

## 工程文件结构

```
enet2can_e31/
├── source/
│   ├── main.c          # CAN 主逻辑 + 参数配置宏
│   ├── uart2.c         # LPUART2 收发实现
│   └── uart2.h
├── board/
│   ├── hardware_init.c # 外设初始化入口
│   ├── pin_mux.c       # 引脚复用配置
│   ├── clock_config.c  # 时钟配置（MCUXpresso 生成）
│   └── board.h
└── drivers/
    ├── fsl_flexcan.c/h # FlexCAN 驱动
    └── fsl_lpuart.c/h  # LPUART 驱动
```

> Keil 工程中需手动将 `source/main.c` 和 `source/uart2.c` 加入编译列表。

---

## 时钟配置说明

| 时钟域 | 频率 | 用途 |
|---|---|---|
| Core CLK | 160 MHz | CPU |
| AIPS_PLAT_CLK | 80 MHz | FlexCAN PE 时钟（经 div=1 后为 80 MHz → 支持最高 8 Mbps FD） |
| AIPS_SLOW_CLK | 40 MHz | LPUART2 时钟源 |

FlexCAN 时钟由 `hardware_init.c` 中显式配置；LPUART2 时钟由驱动内部在 `LPUART_Init()` 时自动使能，无需额外操作。
