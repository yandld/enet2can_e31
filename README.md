# MCXE31B 以太网转 6 路 CAN FD 网关

MCXE31B(Arm Cortex-M7F@160MHz)裸机超级循环 + lwIP(NO_SYS)的 Ethernet-to-6×CAN/CAN FD 网关。对外主接口是 **Linux SocketCAN**:在你的 SoC(RK3588 等)上装一次,`can0..can5` 直接像本地 CAN 口用。

规格:

- MCU:**MCXE31B**(Arm Cortex-M7F@160MHz)
- 通道:**6 路 CAN(CAN0–CAN5)默认全开**(`CAN_ACTIVE_MASK=0x3F`),6 路配置完全一致
- CAN:**CAN FD**,仲裁 1 Mbps / 数据 5 Mbps / BRS on(也支持 CAN 2.0)
- 以太网:**RMII**(10/100M),**DHCP** 或静态 IP
- 端口:数据 UDP `50000`,控制 UDP `50001`;板内协议 `SCGW` v3(项目内 UDP tunnel,见 `docs/IMPLEMENTATION.md`)

> **RX 深度上板验证项**:每路 RX 邮箱组 `5` 深(64B FD 下最小实例的硬件上限,6 路取齐),已开 `MCR[IRMQ]` 使其真正级联成 5 深接收队列。`5` 深在 6×满载下是否够,**必须上板实测**(见 `docs/IMPLEMENTATION.md §3`)。

## CAN 引脚(MCU)

固件当前为 6 路 FlexCAN 配置的 MCU 引脚(见 `board/pin_mux.c` 的 `BOARD_InitFlexCANPins`)。自行设计板子时把 CAN 收发器接到这些引脚即可;要换引脚就同步改 `pin_mux.c`。

| CAN | FlexCAN | MCU TX | MCU RX |
|---|---|---|---|
| CAN0 | FlexCAN_0 | PTA7 | PTA6 |
| CAN1 | FlexCAN_1 | PTA11 | PTA12 |
| CAN2 | FlexCAN_2 | PTE24 | PTE25 |
| CAN3 | FlexCAN_3 | PTC28 | PTC29 |
| CAN4 | FlexCAN_4 | PTC30 | PTC31 |
| CAN5 | FlexCAN_5 | PTC27 | PTC26 |

## 软件架构

```text
main.c                  超级循环: ethernet_lwip_poll / can_service_poll / can_udp_gateway_poll
  ethernet_lwip.c       lwIP、DHCP/静态 IP、link/IP 状态
  can_service.c         FlexCAN 边界: 6 路纯轮询、TX/RX 队列、运行时配置、硬件计数
  gateway_router.c      通道校验、UDP↔CAN 路由、peek/commit 背压、状态快照
  can_udp_gateway.c     UDP data tunnel(50000) + JSON 控制面(50001)
  can_gateway_protocol.h  SCGW v3 线格式定义
  latency_timer.h         DWT 周期计数器 + 延迟统计(板内转发延迟测量)
```

Linux 侧:`linux/mcxe31b-canbridge/`——用户态 C 桥,把板子一条 UDP 流 demux 成 6 个 `can0..can5`(见下文 Linux 小节)。

设计取舍:

- `fsl_flexcan` SDK driver 原样使用;`can_service.c` 只封装 init/re-init、TX/RX 队列、配置和硬件计数。
- 6 路代码路径完全一致,便于验证;CAN0 的 Enhanced RX FIFO 刻意不用(全 6 路统一,纯轮询 + 独立邮箱组 + IRMQ 接收队列)。
- Linux 用户侧直接用 SocketCAN/can-utils 生态;`SCGW` v3 是项目内 tunnel 协议,不是行业标准,对外主接口是 SocketCAN。

## 找到板子 IP

板子默认走 **DHCP**:从串口日志看 `Ethernet: DHCP bound <ip>`。下文示例用 `192.168.8.113`,替换成你的实际 IP。

无 DHCP 网络可改**静态 IP**:编译宏 `ETHERNET_LWIP_USE_DHCP=0`(默认 1),静态地址 `192.168.8.50/24`、网关 `192.168.8.1`(见 `source/ethernet_lwip.c`)。

## Linux SocketCAN(产品级:装一次即用)

`linux/mcxe31b-canbridge/` 是一个用户态 C 桥(libc、单二进制、**零内核模块、免编译内核**):把板子**一条 v3 UDP 流按每帧 channel 字节 demux** 成 6 个 `can0..can5`(用内核自带 `vcan` 实现),RK3588 / 树莓派 / 任意 aarch64 通吃。

### 安装(只做一次)

```bash
cd linux/mcxe31b-canbridge
make
sudo ./install.sh 192.168.8.113        # 板子 IP;装 systemd 服务并开机自起
```

`install.sh` 会:`modprobe vcan` → 建好 `can0..can5` 并设 **`mtu 72`**(CAN-FD 必需,漏设会把 64B FD 帧静默丢)→ 装服务并启动。验证:

```bash
systemctl status mcxe31b-canbridge
candump can0                            # 应能看到 CAN0 转上来的帧
```

### 日常使用

`can0..can5` 就是标准 SocketCAN 接口,整个 can-utils / python-can 生态直接可用:

```bash
candump can0                            # 看收到的帧
cangen  can0 -g 1 -L 64 -f -b           # 往 CAN0 发 CAN-FD 帧
```

### 设 CAN 比特率 / 看状态(经板子下发)

`can0..can5` 是虚拟口,`ip link set canX type can bitrate ...` **不可用**;真实比特率发给板子,用 `canbridge_ctl`(控制面 = UDP 50001 上的 JSON):

```bash
# 改某路 CAN 配置(只带要改的字段)
canbridge_ctl --board 192.168.8.113 set_can_config channel=0 fd=true bitrate=1000000 data_bitrate=5000000 brs=true
# 查状态(各路运行配置、计数、板内延迟 latency_us)
canbridge_ctl --board 192.168.8.113 get_status
# 清零所有计数
canbridge_ctl --board 192.168.8.113 reset_stats
```

`set_can_config` 可改项:`channel`(0–5,必填)、`enabled`、`fd`、`bitrate`(50k–1M)、`data_bitrate`(500k–5M)、`brs`、`filter`(`accept_all`/`id_mask`)、`filter_id`、`filter_mask`。运行时改配安全(单上下文 deinit→重配→重 init,无竞态)。

### 压测(一条命令)

测客户场景:6 路各 **1kHz 收发、64B FD**。先把 6 路两两接成 3 条独立总线 `(can0,can1)(can2,can3)(can4,can5)`,各 120Ω×2,然后:

```bash
sudo ./scripts/stress.sh 192.168.8.113            # 默认 1000 fps/路、64B、10s
sudo ./scripts/stress.sh 192.168.8.113 2000       # 第二个参数 = 速率/路,调高找无损天花板
```

每条总线一行结果,全过就一句话:

```
can0<->can1: 20000 rx, 0 lost (0.00%), latency avg=0.77ms max=4.84ms  -> PASS
can2<->can3: 20000 rx, 0 lost (0.00%), latency avg=0.75ms max=4.86ms  -> PASS
can4<->can5: 20000 rx, 0 lost (0.00%), latency avg=0.71ms max=4.60ms  -> PASS
ALL PASS - 6 channels lossless at 1000 fps/ch (raise rate via 2nd arg to find the ceiling)
```

- **lost** 是唯一要盯的数:`0` 就是不丢帧。
- **latency** 是 Pi→板子→Pi 整条往返,含 Pi 用户态(非实时内核)调度抖动,**不是产品自身延迟**。产品板内转发延迟(微秒级)看 `get_status` 的 `latency_us`(见下节)。
- 只有丢包时才打印板子各通道计数做归因:`state=error-passive` → 总线接线/终端;`rx_fifo_overflow` → 板子 RX 封顶;`queue_full/tx_drop` → 板子 TX 封顶;桥 `rxq_ovfl=0` → 瓶颈不在 Pi。

> 「两两接」只是台架接法——拿邻路当对端设备,负载与客户现场「6 条独立总线」完全等价。接线不够可先只接一条:`sudo IFACES="can0 can1" ./scripts/stress.sh 192.168.8.113`(环境变量要放 `sudo` 之后,否则被 sudo 丢弃)。

### 注意

- **一块板只能跑一个桥**:板子把 CAN 帧回发给「最后一个跟它通信的对端」,别同时起两个桥。
- 比特率只能经板子设(见上),不能在 vcan 接口上设。

### 兼容性

| 能力 | RK3588(vendor 6.1) | 树莓派 OS | Ubuntu aarch64 |
|---|---|---|---|
| `vcan` + `CAN_RAW` | ✅ | ✅ | ✅ |
| `CAN_RAW_FD_FRAMES` + mtu 72 | ✅ | ✅ | ✅ |

> 同一份源码 `make` 即可,vcan 内核自带、免编译。目录文件:`install.sh`(一键安装)、`src/canbridge.c`(桥本体)、`src/canbridge_ctl.c`(控制客户端)、`src/canmesh.c`(压测工具)、`scripts/stress.sh`(压测)、`scripts/setup-vcan.sh`(建网口)。
