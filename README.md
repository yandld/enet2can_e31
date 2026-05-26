# FRDM-MCXE31B 6x FlexCAN Demo

基于 NXP **FRDM-MCXE31B** 评估板，演示 6 路独立 Classic CAN 及 UART smoke test 的基本收发功能。

---

## 硬件平台

| 项目 | 参数 |
|---|---|
| 评估板 | FRDM-MCXE31B |
| MCU | NXP MCXE31B，ARM Cortex-M7，160 MHz |
| SDK | MCUXpresso SDK 2025.12.00 |
| 开发工具 | Keil MDK / ARM Compiler 6 |

---

## 功能概览

| 外设 | 功能 |
|---|---|
| FlexCAN 0 / 1 / 2 / 3 / 4 / 5 | 默认 Classic CAN 500 kbps，每路独立收发，每秒发送一帧计数帧，接收所有标准帧并打印 |
| LPUART2（MIKROE） | 每秒发送一条计数字符串；收到字节后原样回显并打印到调试串口 |
| UART smoke test | 保留工程现有 UART smoke 测试逻辑 |
| LPUART5（调试串口） | 115200 bps，打印运行日志 |

---

## Classic CAN 默认参数

所有 FlexCAN 通道默认使用相同参数：

| 参数 | 默认值 |
|---|---|
| Bitrate | 500 kbps |
| Payload | 8 bytes |
| CAN FD | Disabled |
| BRS | Disabled |
| Frame format | Standard ID data frame |
| Self reception | Disabled |
| TX period | 1000 ms |
| TX timeout | 200 ms |

发送 ID：

| 通道 | TX ID |
|---|---|
| CAN0 | `0x100` |
| CAN1 | `0x101` |
| CAN2 | `0x102` |
| CAN3 | `0x103` |
| CAN4 | `0x104` |
| CAN5 | `0x105` |

---

## 引脚分配

CAN 引脚依据 `MCXE31B_IOMUX.xlsx` 和 `board/pin_mux.c` 配置。当前编译工程以 `board/pin_mux.c` 为准，没有重新生成 `.mex`。

| 通道 | 外设 | TX 引脚 | TX package pin | RX 引脚 | RX package pin |
|---|---|---:|---:|---:|---:|
| CAN0 | FLEXCAN_0 | PTA7 | 100 | PTA6 | 102 |
| CAN1 | FLEXCAN_1 | PTA11 | 160 | PTA12 | 159 |
| CAN2 | FLEXCAN_2 | PTE24 | 157 | PTE25 | 158 |
| CAN3 | FLEXCAN_3 | PTC28 | 96 | PTC29 | 99 |
| CAN4 | FLEXCAN_4 | PTC30 | 101 | PTC31 | 103 |
| CAN5 | FLEXCAN_5 | PTC27 | 93 | PTC26 | 91 |

### LPUART2（MIKROE 接口）

| 方向 | 引脚 | Package pin |
|---|---:|---:|
| TX | PTE12 | 29 |
| RX | PTD17 | 31 |

### 调试串口

| 方向 | 引脚 | Package pin |
|---|---:|---:|
| TX | PTE14 | 26 |
| RX | PTE3 | 27 |

调试串口参数：**115200 bps，8N1**。

---

## 参数配置

常用 CAN 参数集中在 `source/main.c` 顶部：

```c
#define CAN_CHANNEL_COUNT       6U
#define CAN_TX_ID_BASE          0x100U
#define CAN_BITRATE             500000U
#define CAN_USE_CANFD           0
#define CAN_FD_BITRATE          2000000U
#define TX_PERIOD_MS            1000U
#define TX_TIMEOUT_MS           200U
#define FD_PAYLOAD_SIZE         kFLEXCAN_64BperMB
#define FD_DLC                  15
```

通道实例、时钟和 ID 由 `s_canConfig[]` 配置：

```c
static const can_ch_config_t s_canConfig[CAN_CHANNEL_COUNT] =
{
    { FLEXCAN_0, kCLOCK_Flexcan0Clk, CAN_TX_ID_BASE + 0U },
    { FLEXCAN_1, kCLOCK_Flexcan1Clk, CAN_TX_ID_BASE + 1U },
    { FLEXCAN_2, kCLOCK_Flexcan2Clk, CAN_TX_ID_BASE + 2U },
    { FLEXCAN_3, kCLOCK_Flexcan3Clk, CAN_TX_ID_BASE + 3U },
    { FLEXCAN_4, kCLOCK_Flexcan4Clk, CAN_TX_ID_BASE + 4U },
    { FLEXCAN_5, kCLOCK_Flexcan5Clk, CAN_TX_ID_BASE + 5U }
};
```

UART2 参数在 `source/uart2.c` 顶部：

```c
#define UART2_BAUDRATE      115200U
#define UART2_TX_PERIOD_MS  1000U
```

---

## CAN FD 可选模式

默认不启用 CAN FD。如果需要切回 CAN FD，将 `CAN_USE_CANFD` 改为 `1`，并确保对端也配置为 500 kbps arbitration / 2 Mbps data / BRS enabled。

---

## 调试串口输出

上电后会先打印 6 路 Classic CAN 配置摘要，随后持续输出收发日志。

### 启动信息示例

```text
========================================
  6x FlexCAN Demo  -  MCXE31B
========================================
  CAN0: 500kbps  Classic CAN  TX=0x100  period=1000ms
  CAN1: 500kbps  Classic CAN  TX=0x101  period=1000ms
  CAN2: 500kbps  Classic CAN  TX=0x102  period=1000ms
  CAN3: 500kbps  Classic CAN  TX=0x103  period=1000ms
  CAN4: 500kbps  Classic CAN  TX=0x104  period=1000ms
  CAN5: 500kbps  Classic CAN  TX=0x105  period=1000ms
  RX: all standard frames
========================================

[CAN0] ready
[CAN1] ready
[CAN2] ready
[CAN3] ready
[CAN4] ready
[CAN5] ready
[UART2] init  115200bps  PTE12=TX  PTD17=RX
```

### 运行日志格式

| 日志 | 说明 |
|---|---|
| `[CAN3] TX start  id=0x103  data=0x5` | CAN3 启动发送，`data` 为递增计数值；这不代表总线已 ACK |
| `[CAN3] TX timeout, abort pending frame` | 当前帧未完成发送，常见原因是总线无 ACK 或对端未就绪，demo 会 abort 后在下个周期继续尝试 |
| `[CAN4] RX  id=0x123  len=8  data: ... ts=1234` | CAN4 收到 Classic CAN 帧，显示 ID、字节长度、数据和硬件时间戳 |
| `[UART2] RX: 0x41 'A'` | UART2 收到字符 `A` |

> CAN 默认关闭自接收（`disableSelfReception = true`），各通道不会收到自己发出的帧。

---

## UART2 收发说明

### 定时发送

MCU 每隔 `UART2_TX_PERIOD_MS`（默认 1 s）通过 PTE12 发送一条字符串：

```text
UART2 cnt=0
UART2 cnt=1
...
```

### 接收与回显

PTD17 收到任意字节后：

1. 通过 PTE12 原样回发（echo）
2. 在调试串口打印 `[UART2] RX: 0xXX 'c'`

### 回环自测方法

用跳线短接 **PTE12 -> PTD17**，MCU 会收到自己发出的字符串并在调试串口打印。测试完毕后拔除跳线即可。

---

## 工程文件结构

```text
enet2can_e31/
├── source/
│   ├── main.c          # CAN main logic and channel configuration
│   ├── uart2.c         # LPUART2 RX/TX implementation
│   ├── uart2.h
│   ├── uart_smoke.c    # UART smoke test implementation
│   └── uart_smoke.h
├── board/
│   ├── hardware_init.c # Peripheral initialization entry
│   ├── pin_mux.c       # Pin mux configuration
│   ├── clock_config.c  # Clock configuration
│   └── board.h
└── drivers/
    ├── fsl_flexcan.c/h
    └── fsl_lpuart.c/h
```

`enet2can_e31.uvprojx` 已包含当前 demo 所需源文件。

---

## 时钟配置说明

| 时钟域 | 频率 | 用途 |
|---|---:|---|
| Core CLK | 160 MHz | CPU |
| AIPS_PLAT_CLK | 80 MHz | FlexCAN PE clock source |
| FLEXCAN0/1/2_PE_CLK | 80 MHz | FLEXCAN_0/1/2 |
| FLEXCAN3/4/5_PE_CLK | 80 MHz | FLEXCAN_3/4/5 |
| AIPS_SLOW_CLK | 40 MHz | LPUART clock source |

`board/hardware_init.c` 会显式配置 `FLEXCAN012_PE` 和 `FLEXCAN345_PE` 时钟。PCAN 或其他对端默认按 Classic CAN 500 kbps 配置即可。
