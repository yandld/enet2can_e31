# E2CF MCXE31B CAN FD 网关交付说明

本仓库交付 MCXE31B 以太网到 6 路 CAN FD 网关方案。Linux 侧加载 `eth2can.ko` 后，会看到标准 SocketCAN 设备 `eth2can0..5`，客户可以继续使用 `ip link`、`candump`、`cansend` 和上层 CAN 工具。

默认交付配置为 CAN FD `bitrate 1000000` / `dbitrate 5000000`，即 1M/5M BRS。

## 交付内容

| 路径 | 用途 |
|---|---|
| `source/` | MCXE31B 网关固件源码 |
| `linux/` | Linux SocketCAN 驱动、安装脚本和测试工具 |
| `linux/can_testcase/` | `canperf` 延迟与双向带宽测试 |
| `docs/` | 协议、Linux 驱动、MCU 固件和 EQOS TX 环设计说明 |
| `tools/imx95/` | 内部 i.MX95 bench 构建/部署脚本，不作为客户入口 |

## 固件

交付板若已预烧录固件，可直接上电。需要自行构建时，使用 Keil MDK 打开 `e2cf_mcxe31.uvprojx` 构建 MCXE31B 固件。烧录工具按客户现有 MCXE31B/SWD 流程执行。

## 最短 Bring-up

1. 烧录或确认 MCXE31B 固件已预装，然后上电。
2. Linux 主机连接到网关以太网口。
3. 从仓库根目录安装驱动：

```sh
sh linux/scripts/install_driver.sh
```

4. 确认 6 个 SocketCAN 设备存在并已配置为 1M/5M：

```sh
ip -details link show type can
```

5. 按默认线束连接 3 组 CAN 总线：

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

每组是独立 CAN FD 总线：总线两端各 120Ω 终端，CANH/CANL 不能接反，收发器需要正确供电并与网关共地。

## 状态灯

| LED | GPIO | 状态 | 含义 |
|---|---|---|---|
| SYS | PTC16 | 约 1 Hz 闪烁 | 固件主循环运行中 |
| SYS | PTC16 | 常亮 | 固件停在 fault/fatal path |
| NET | PTB22 | 熄灭 | PHY link down |
| NET | PTB22 | 闪烁 | PHY link up，但尚未建立 E2CF peer heartbeat |
| NET | PTB22 | 常亮 | Ethernet/E2CF 链路 ready |
| CAN | PTC14 | 熄灭 | 无 CAN 流量 |
| CAN | PTC14 | 闪烁 | 有 CAN RX/TX 流量 |
| CAN | PTC14 | 常亮 | 至少一路 active CAN 处于 error-passive 或 bus-off |

## 基础自检

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

如果 `eth2can4` 能收到帧，说明驱动、网关、物理 CAN 线束至少一组链路可用。

## 交付测试

构建客户测试工具：

```sh
cd linux/can_testcase
make
```

延迟测试：

```sh
./canperf latency --count 10000
```

双向最大可持续带宽测试：

```sh
./canperf bandwidth
```

验收时先看最后的 `RESULT` 行：`PASS` 表示 zero-loss，延迟给出 p50/p99/p99.9，带宽给出 MSR。`EVIDENCE` 行用于保存复验证据；`counters=clean` 表示驱动/网关丢帧、溢出、拒绝等计数器未增长。p99.9 即 p999；当前文档不固定 p99 或带宽硬阈值，实际阈值应由客户线束、主机性能和系统负载共同确认。

## MCXE31B 引脚分配

下表列出 CAN、Ethernet、Debug UART 主要连接，来自 `board/pin_mux.c` 的 MCUXpresso pin mux 配置，封装为 `MCXE31BMPB`。

| 功能 | 外设信号 | MCU 管脚 | 封装脚号 | 连接建议 |
|---|---|---|---:|---|
| CAN0 | FLEXCAN_0_TX | PTA7 | 100 | 接 CAN0 收发器 TXD |
| CAN0 | FLEXCAN_0_RX | PTA6 | 102 | 接 CAN0 收发器 RXD |
| CAN1 | FLEXCAN_1_TX | PTA11 | 160 | 接 CAN1 收发器 TXD |
| CAN1 | FLEXCAN_1_RX | PTA12 | 159 | 接 CAN1 收发器 RXD |
| CAN2 | FLEXCAN_2_TX | PTE24 | 157 | 接 CAN2 收发器 TXD |
| CAN2 | FLEXCAN_2_RX | PTE25 | 158 | 接 CAN2 收发器 RXD |
| CAN3 | FLEXCAN_3_TX | PTC28 | 96 | 接 CAN3 收发器 TXD |
| CAN3 | FLEXCAN_3_RX | PTC29 | 99 | 接 CAN3 收发器 RXD |
| CAN4 | FLEXCAN_4_TX | PTC30 | 101 | 接 CAN4 收发器 TXD |
| CAN4 | FLEXCAN_4_RX | PTC31 | 103 | 接 CAN4 收发器 RXD |
| CAN5 | FLEXCAN_5_TX | PTC27 | 93 | 接 CAN5 收发器 TXD |
| CAN5 | FLEXCAN_5_RX | PTC26 | 91 | 接 CAN5 收发器 RXD |
| Ethernet | EMAC_MII_RMII_MDIO | PTB4 | 48 | RMII PHY MDIO |
| Ethernet | EMAC_MII_RMII_MDC | PTB5 | 47 | RMII PHY MDC |
| Ethernet | EMAC_MII_RMII_TXD0 | PTC2 | 50 | RMII PHY TXD0 |
| Ethernet | EMAC_MII_RMII_TXD1 | PTD7 | 51 | RMII PHY TXD1 |
| Ethernet | EMAC_MII_RMII_TX_EN | PTD12 | 54 | RMII PHY TX_EN |
| Ethernet | EMAC_MII_RMII_RX_DV | PTC17 | 65 | RMII PHY CRS_DV/RX_DV |
| Ethernet | EMAC_MII_RMII_RXD0 | PTC1 | 61 | RMII PHY RXD0 |
| Ethernet | EMAC_MII_RMII_RXD1 | PTC0 | 62 | RMII PHY RXD1 |
| Ethernet | EMAC_MII_RMII_TX_CLK | PTD11 | 55 | RMII 50 MHz reference clock |
| Ethernet | ENET_PHY_RST GPIO | PTC3 | 49 | PHY reset |
| Debug UART | LPUART_5_RX | PTE3 | 27 | Debug console RX |
| Debug UART | LPUART_5_TX | PTE14 | 26 | Debug console TX |

6 路 CAN FD 外部收发器建议使用 CAN SIC 等级器件。默认线束测试为 `CAN0<->CAN4`、`CAN1<->CAN2`、`CAN3<->CAN5`。

## 常见入口

- Linux 驱动安装和故障排查：`linux/README.md`
- canperf 编译和结果解读：`linux/can_testcase/README.md`
- i.MX95 内部 bench 工具：`tools/imx95/README.md`
