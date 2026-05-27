# FRDM-MCXE31B CAN0 优先的 CAN FD 转以太网参考设计

这是面向客户参考设计的 MCXE31B 工程，目标是最终完成 6 路 CAN FD 与以太网之间的网关。当前阶段不直接冲 6 路满载，而是采用 CAN0-first：先把 DHCP/ping、CAN0 双向 UDP 数据通路、队列、状态和错误计数做稳，再逐步打开 CAN1..CAN5。

## 当前能做什么

| 模块 | 当前状态 |
|---|---|
| 以太网 | FRDM-MCXE31B 板载 LAN8741 RMII，默认 DHCP，已用于 ping 验证 |
| CAN0 | 默认启用，CAN FD 1 Mbps arbitration / 5 Mbps data，BRS on |
| CAN1..CAN5 | pin map 和 6 路协议映射保留，默认关闭 |
| 调试串口 | 只保留 debug UART，115200 8N1 |
| UDP data | MCU 监听 UDP `50000`，承载固定 84-byte CAN gateway frame |
| UDP status | MCU 监听 UDP `50001`，返回 compact JSON 状态快照 |
| Windows 测试工具 | `tools/win_can_udp_test.py` 可发帧、监听、查状态、做简单计数发送 |
| Linux SocketCAN | `tools/socketcan_udp_bridge.py` 是后续 Linux userspace SocketCAN bridge 起点 |

一句话用法：烧录固件，串口确认 DHCP IP，PC ping 通板子；再用 Windows 工具发 UDP 帧，PCANView 应能在 CAN0 看到 CAN frame；PCANView 发 CAN0 frame，Windows 工具应能收到 UDP 回包。

## 入门指南

### 1. 准备硬件

| 项目 | 用途 |
|---|---|
| FRDM-MCXE31B | 当前目标板 |
| 路由器 | FRDM-MCXE31B 和 PC 接到同一路由器 |
| USB 调试线 | 下载固件和查看 debug UART |
| PCANView 或 CAN FD peer node | 验证 CAN0 |
| CAN0 连接 | CAN0_TX=PTA7，CAN0_RX=PTA6，外接 transceiver 到 CANH/CANL |

串口参数：

```text
115200 8N1
```

PCANView 推荐配置：

| 参数 | 值 |
|---|---|
| CAN FD | Enabled |
| Arbitration bitrate | 1 Mbps |
| Data bitrate | 5 Mbps |
| BRS | Enabled |
| ID format | Standard |

### 2. 编译工程

Keil target：

```text
enet2can_e31 debug
```

命令行 rebuild：

```powershell
& 'C:\Keil_v5\UV4\UV4.exe' -b 'enet2can_e31.uvprojx' -t 'enet2can_e31 debug' -o 'debug\codex_build.log'
```

验收输出：

```text
"debug\enet2can_e31.out" - 0 Error(s), 0 Warning(s).
```

### 3. 下载并确认 DHCP

复位后 debug UART 应看到类似日志：

```text
========================================
  CAN0-first CAN FD gateway  -  MCXE31B
========================================
  Default active CAN mask: 0x1
  Gateway protocol: magic=0x43474644 data_port=50000 status_port=50001
========================================
Ethernet: DHCP enabled
CAN service: active_mask=0x1
CAN0: enabled CAN FD 1000kbps/5000kbps BRS on TX=0x100
CAN UDP gateway: data_port=50000 status_port=50001
Ethernet: link up
Ethernet: DHCP bound 192.168.1.xxx
```

如果未插网线或 DHCP 未完成，固件不会卡死，CAN0 仍会运行；串口只会低频提示 Ethernet 状态。

### 4. PC ping 验证

把串口里的 IP 填进去：

```powershell
ping -n 10 <board-ip>
```

示例：

```powershell
ping -n 10 192.168.1.123
```

连续收到回复说明 ENET/lwIP/DHCP/ICMP 基础框架可用。

### 5. Ethernet -> CAN0

Classic CAN：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --send-id 0x123 --data "11 22 33 44 55 66 77 88"
```

PCANView 应看到：

```text
ID=0x123, DLC=8, DATA=11 22 33 44 55 66 77 88
```

CAN FD + BRS：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --send-id 0x123 --fd --brs --data "00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F"
```

连续发送 100 帧：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --send-id 0x123 --fd --brs --data "11 22 33 44 55 66 77 88" --count 100 --interval-ms 10
```

### 6. CAN0 -> Ethernet

启动监听。工具会先发 1-byte learn packet，让 MCU 知道 CAN0 回包要发给哪台 PC：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --listen
```

保持这个窗口打开，然后在 PCANView 发送一帧：

```text
ID=0x321, DATA=AA BB CC DD
```

Windows 工具应打印类似：

```text
192.168.1.xxx:50000 ch=0 id=0x321 dlc=4 flags=Classic status=ok data=[AA BB CC DD]
```

### 7. 查询状态

查询一次：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --status
```

循环查询：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --watch-status --interval-ms 1000
```

输出示例：

```text
link=True dhcp=True ip=192.168.1.123 active_mask=0x1 peer=192.168.1.10:50000 can0.rx=2 can0.tx_start=3 can0.tx_queue=0 can0.rx_drop=0 can0.tx_drop=0 can0.error=0
```

如需看原始 JSON：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --status --json
```

JSON 中重点看：

| 字段 | 含义 |
|---|---|
| `link` / `dhcp` / `ip` | 以太网链路、DHCP 和 IPv4 |
| `active_mask` | 当前启用的 CAN channel，默认 `0x1` |
| `peer` | CAN0 -> Ethernet 的 UDP 回包目标 |
| `udp.rx/tx/drop/parse_error/no_peer` | UDP 层计数 |
| `router.disabled/queue_full` | 协议/路由拒绝原因 |
| `can[0].rx/tx_start/tx_done` | CAN0 收发计数 |
| `can[0].tx_queue/rx_queue` | 当前队列深度 |
| `can[0].watermark` | 队列历史最高水位 |

### Enhanced Rx FIFO mode

当前 CAN0 FD 接收路径使用 SDK 的 `Enhanced Rx FIFO` 中断模式，不改 SDK driver，也不引入 eDMA。

默认参数：
```c
#define CAN_SERVICE_USE_ENHANCED_RX_FIFO 1U
#define CAN_SERVICE_EFIFO_BATCH_SIZE 1U
#define CAN_SERVICE_EFIFO_WATERMARK 0U
```

这组默认值先保低延迟和可观测性。`--status` 里重点看 `rx_fifo_overflow`、`rx_fifo_warning`、`tx_err_counter`、`rx_err_counter`、`state`。

如果 `rx_fifo_overflow` 开始增长，下一步再把 batch 提到 4、watermark 提到 3，做对比压测。

每 5 秒 debug UART 也会打印一行简短 counter，不打印每帧：

```text
CAN0: status rx=1 tx_start=1 tx_done=1 txq=0 rxq=0 tx_drop=0 rx_drop=0 err=0 last=0x0
```

## 当前默认配置

CAN 配置在 `source/can_service.h`：

```c
#define CAN_SERVICE_CHANNEL_COUNT 6U
#define CAN_ACTIVE_MASK 0x01U
#define CAN_SERVICE_RX_RING_SIZE 32U
#define CAN_SERVICE_TX_QUEUE_SIZE 32U
#define CAN_BITRATE 1000000U
#define CAN_USE_CANFD 1
#define CAN_FD_BITRATE 5000000U
#define CAN_CLASSIC_DLC 8U
#define CAN_FD_DLC 15U
```

含义：

| 配置 | 说明 |
|---|---|
| `CAN_SERVICE_CHANNEL_COUNT 6U` | 协议和架构保留 6 路 CAN |
| `CAN_ACTIVE_MASK 0x01U` | 当前只启用 CAN0 |
| `CAN_SERVICE_RX_RING_SIZE 32U` | 每路 CAN RX ring 深度 |
| `CAN_SERVICE_TX_QUEUE_SIZE 32U` | 每路 CAN TX queue 深度 |
| `CAN_USE_CANFD 1` | 默认 CAN FD |
| `CAN_BITRATE 1000000U` | arbitration phase 1 Mbps |
| `CAN_FD_BITRATE 5000000U` | data phase 5 Mbps |

后续要打开 CAN1，可以改成 `CAN_ACTIVE_MASK 0x03U`；打开 6 路可以改成 `0x3FU`。不要在 CAN0 UDP bridge 和 counters 稳定前直接打开 6 路压测。

## CAN 引脚

CAN pins follow `docs/MCXE31B_IOMUX.xlsx` and `board/pin_mux.c`.

| 通道 | 外设 | TX pin | TX package pin | RX pin | RX package pin |
|---|---|---:|---:|---:|---:|
| CAN0 | FLEXCAN_0 | PTA7 | 100 | PTA6 | 102 |
| CAN1 | FLEXCAN_1 | PTA11 | 160 | PTA12 | 159 |
| CAN2 | FLEXCAN_2 | PTE24 | 157 | PTE25 | 158 |
| CAN3 | FLEXCAN_3 | PTC28 | 96 | PTC29 | 99 |
| CAN4 | FLEXCAN_4 | PTC30 | 101 | PTC31 | 103 |
| CAN5 | FLEXCAN_5 | PTC27 | 93 | PTC26 | 91 |

Debug UART：

| 功能 | TX | RX | 波特率 |
|---|---:|---:|---:|
| Debug UART, LPUART5 | PTE14 | PTE3 | 115200 |

## 协议和架构

UDP data protocol 定义在 `source/can_gateway_protocol.h`：

```c
#define CAN_GATEWAY_UDP_DATA_PORT 50000U
#define CAN_GATEWAY_UDP_STATUS_PORT 50001U
```

v1 数据帧固定 84 bytes，不改变 layout：

```text
magic/version/channel/flags/dlc/can_id/timestamp_ms/status/data[64]
```

架构边界：

| 文件 | 责任 |
|---|---|
| `source/main.c` | 初始化和主循环调度 |
| `source/ethernet_lwip.c/.h` | ENET/lwIP DHCP bring-up 边界 |
| `source/can_service.c/.h` | FlexCAN、per-channel RX/TX queue、CAN counters |
| `source/gateway_router.c/.h` | 协议校验、active mask、路由、统一快照 |
| `source/can_udp_gateway.c/.h` | lwIP UDP data/status endpoint、peer learning、pbuf 收发 |
| `source/can_gateway_protocol.h` | UDP wire protocol |
| `tools/win_can_udp_test.py` | Windows UDP/CAN0/status 测试工具 |
| `tools/socketcan_udp_bridge.py` | Linux userspace SocketCAN bridge starter |
| `docs/bringup/6canfd_ethernet_reference_design.md` | bring-up 和里程碑记录 |

主循环只做调度：

```c
ethernet_lwip_poll();
can_service_poll();
can_udp_gateway_poll();
```

Linux starter：

```bash
python3 tools/socketcan_udp_bridge.py --remote-host <board-ip> --can can0
```

当前仍优先验证 Windows + PCANView + CAN0；Linux SocketCAN bridge 后续再做 `vcan0/can0` 回归验证。

## 故障排查

### 没有 DHCP bound

检查：

- FRDM-MCXE31B 和 PC 是否接到同一路由器。
- 网线是否连接，串口是否有 `Ethernet: link up`。
- 路由器是否开启 DHCP。
- PC 是否也能从同一路由器获取 IP。

### ping 不通

检查：

- ping 的 IP 是否来自最新串口日志 `Ethernet: DHCP bound x.x.x.x`。
- PC 和板子是否在同一网段。
- 企业网络策略或防火墙是否拦截 ICMP。

### Ethernet -> CAN0 没反应

检查：

- PCANView 是否为 CAN FD、1M/5M、BRS enabled。
- CAN0 transceiver 是否供电并退出 standby/silent。
- CANH/CANL/GND/终端电阻是否正确。
- `--status` 中 `can0.tx_queue` 是否堆积，`tx_drop` 或 `error` 是否增加。

### CAN0 -> Ethernet 没回包

先运行：

```powershell
python tools\win_can_udp_test.py --board <board-ip> --listen
```

如果未发送 learn packet，MCU 不知道 peer，`--status` 中 `udp.no_peer` 会增加。

### 为什么只开 CAN0

当前目标是先把以太网框架、CAN0 双向链路、buffer、counter、错误模型跑通。6 路是最终目标，但不应在 CAN0 队列/状态模型未稳定前直接打开。

## 已知限制

- 当前是最小 CAN0 UDP data path 加 status path；还没有运行时配置 API。
- UDP v1 不承诺重传、顺序和强可靠，只通过 counters 暴露丢包/拒绝原因。
- TCP server/client、HTTP/JSON config、Modbus TCP、CANopen、PTP/1588、远程升级尚未实现。
- bitrate、active mask 仍是编译期配置。
- CAN1..CAN5 默认关闭。
- 120 ohm 终端电阻开关需要硬件支持；如果板上没有可控终端，固件无法软件实现。

## 推荐下一步

1. 保持 CAN0-only。
2. 用 PCANView + `tools\win_can_udp_test.py` 验证 Ethernet -> CAN0。
3. 用 PCANView + `tools\win_can_udp_test.py --listen` 验证 CAN0 -> Ethernet。
4. 用 `--status` 记录 RX/TX/drop/error/watermark counters。
5. CAN0 20% 负载稳定后，再逐路打开 CAN1..CAN5。
