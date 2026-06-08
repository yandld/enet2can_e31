# MCXE31B 以太网转 6 路 CAN FD 网关

MCXE31B(Arm Cortex-M7F@160MHz)裸机超级循环 + lwIP(NO_SYS)的 Ethernet↔6×CAN/CAN FD 网关。对外主接口是 **Linux SocketCAN**:在你的 SoC(RK3588 / 树莓派等)上装一次,`can0..can5` 直接像本地 CAN 口用。

规格:

- MCU:**MCXE31B**(Arm Cortex-M7F@160MHz)
- 通道:**6 路 CAN(CAN0–CAN5)默认全开**(`CAN_ACTIVE_MASK=0x3F`),6 路配置完全一致
- CAN:**CAN FD**,仲裁 1 Mbps / 数据 5 Mbps / BRS on(也支持 CAN 2.0)
- 以太网:**RMII**(10/100M),**DHCP** 或静态 IP
- 端口:数据 UDP `50000`,控制 UDP `50001`;板内协议 `SCGW` v3(项目内 UDP tunnel,见 `docs/IMPLEMENTATION.md`)
- 板内转发延迟:udp→can / can→udp **微秒级**,板载 DWT 实测(`get_status` 的 `latency_us`)

> **RX 深度上板验证项**:每路 RX 邮箱组 `5` 深(64B FD 下最小实例 CAN3/4/5 的硬件上限,6 路取齐),已开 `MCR[IRMQ]` 使其真正级联成 5 深接收队列。`5` 深在 6×满载下是否够,**必须上板实测**(见 `docs/IMPLEMENTATION.md §3`)。

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

裸机**单上下文超级循环**:无 RTOS、无 CAN 中断、lwIP 跑 `NO_SYS`。一个 while 循环顺序轮询三件事,所有队列/邮箱/配置状态都在同一上下文,天然无锁、无竞态:

```c
while (1) {
    ethernet_lwip_poll();    // 收发以太网 / lwIP 定时器 / DHCP
    can_service_poll();      // 6 路 FlexCAN: 排空 RX、发 TX、轮询错误状态
    can_udp_gateway_poll();  // 把 CAN 收到的帧打包成 UDP 发回主机
}
```

模块(`source/`):

```text
main.c                   超级循环 + 故障处理(HardFault 等)+ 每段轮询耗时打点(DWT)
ethernet_lwip.c          lwIP/DHCP/静态IP、link 与 IP 状态
can_service.c            FlexCAN 边界: 6 路纯轮询、TX 双邮箱 / RX 邮箱组(IRMQ)、
                         运行时配置、硬件计数、总线错误→SocketCAN 错误帧
gateway_router.c         通道校验、UDP↔CAN 路由、CAN→UDP 轮询 peek/commit 背压、状态快照
can_udp_gateway.c        UDP 数据隧道(50000)+ JSON 控制面(50001)+ 延迟统计
can_gateway_protocol.h   SCGW v3 线格式(每帧带 channel 字节,变长记录)
latency_timer.h          DWT 周期计数器 + 延迟累加器(板内转发延迟,微秒级)
```

数据流是两个方向各一条流水线:

```text
下行 UDP→CAN:  以太网 ─▶ lwIP ─▶ can_udp_gateway 收回调 ─解析 v3─▶
               gateway_router_from_udp ─▶ can_service TX 队列 ─▶ FlexCAN TX 邮箱 ─▶ CAN 总线
上行 CAN→UDP:  CAN 总线 ─▶ FlexCAN RX 邮箱组 ─can_service 排空─▶ RX 环 ─▶
               gateway_router 轮询 peek ─▶ can_udp_gateway 打包 v3 ─▶ udp_sendto ─▶ 以太网
```

设计取舍:

- `fsl_flexcan` SDK driver 原样使用;`can_service.c` 只封装 init/re-init、TX/RX 队列、配置、计数、错误帧。
- 6 路代码路径完全一致便于验证;CAN0 的 Enhanced RX FIFO 刻意不用(全 6 路统一:纯轮询 + 独立邮箱组 + IRMQ 接收队列)。
- 无 ISR、单上下文:运行时改配(deinit→重配→重 init)不会和中断竞态;背压用"先占座再消费"(peek/commit)实现——堆不下就留在队列下轮重试,**有界延迟而非丢帧**。
- 延迟用 DWT 周期计数器实测,与主机时钟无关,通过 `get_status` 的 `latency_us` 暴露。

## 找到板子 IP

板子默认走 **DHCP**:从串口日志看 `Ethernet: DHCP bound <ip>`。下文示例用 `192.168.8.113`,替换成你的实际 IP。

无 DHCP 网络可改**静态 IP**:编译宏 `ETHERNET_LWIP_USE_DHCP=0`(默认 1),静态地址 `192.168.8.50/24`、网关 `192.168.8.1`(见 `source/ethernet_lwip.c`)。

---

# Linux 端 —— SocketCAN 桥

`linux/` 是一套用户态 C 工具(libc、单二进制、**零内核模块、免编译内核**),把板子**一条 v3 UDP 流按每帧 channel 字节 demux** 成 6 个标准 SocketCAN 口 `can0..can5`(用内核自带 `vcan` 实现)。RK3588 / 树莓派 / 任意 aarch64 通吃,同一份源码 `make` 即可。

## 架构

```text
 你的App / candump / cangen          canbridge 守护进程              MCXE31B 板子
  (标准 SocketCAN API)              (单 epoll + recvmmsg)
        │                                   │
   can0..can5 (vcan)  ◀── 注入 ────┐        │
        │                          ├──▶ 一条 UDP 流 (50000) ◀───▶ FlexCAN0..5 ─ CAN 总线
        └──── 读 canX ─── 聚合 ────┘        │   每帧带 channel 字节
                                            │
   canbridge_ctl ───── JSON 控制 (50001) ───┴──────────────────▶  设比特率 / 查状态
```

- **下行**(板→主机):板子 UDP 帧 → canbridge 按 channel 字节注入对应 `canX`,`candump canX` 就能看到。
- **上行**(主机→板):`canX` 上的帧 → canbridge 聚合成 ≤16 帧的 v3 datagram → 发给板子 → 板子在对应 FlexCAN 口发出。
- 单 epoll 线程 + `recvmmsg` 一次排空积压;`SO_RXQ_OVFL` 让桥能**自证有没有丢**(区分"桥丢"还是"板丢")。
- **实时调度**:桥跑在 `SCHED_FIFO` + `mlockall`(systemd 配置 + 代码兜底),避免被普通调度卡住后甩出突发——那是板内延迟尾巴的主因,详见末节。

## 安装(只做一次)

```bash
cd linux
make
sudo ./install.sh 192.168.8.113        # 板子 IP;编译、装 systemd 服务、开机自起、配置实时调度
```

`install.sh` 会:编译 → `modprobe vcan` 建好 `can0..can5` 并设 **`mtu 72`**(CAN FD 必需,漏设会把 64B FD 帧静默丢)→ 装服务并(重)启动。验证:

```bash
systemctl status mcxe31b-canbridge
chrt -p $(pgrep -x canbridge)           # 应显示 SCHED_FIFO priority 50(实时生效)
candump can0                            # 应能看到 CAN0 转上来的帧
```

## 日常使用

`can0..can5` 就是标准 SocketCAN 接口,整个 can-utils / python-can 生态直接可用:

```bash
candump can0                            # 看收到的帧
cangen  can0 -g 1 -L 64 -f -b           # 往 CAN0 发 CAN-FD 帧
```

## 设 CAN 比特率 / 看状态(经板子下发)

`can0..can5` 是虚拟口,`ip link set canX type can bitrate ...` **不可用**;真实比特率发给板子,用 `canbridge_ctl`(控制面 = UDP 50001 上的 JSON):

```bash
# 改某路 CAN 配置(只带要改的字段)
canbridge_ctl --board 192.168.8.113 set_can_config channel=0 fd=true bitrate=1000000 data_bitrate=5000000 brs=true
# 查状态(各路运行配置、计数、板内延迟 latency_us)
canbridge_ctl --board 192.168.8.113 get_status
# 清零所有计数
canbridge_ctl --board 192.168.8.113 reset_stats
```

`set_can_config` 可改项:`channel`(0–5,必填)、`enabled`、`fd`、`bitrate`(50k–1M)、`data_bitrate`(500k–5M)、`brs`、`filter`(`accept_all`/`id_mask`)、`filter_id`、`filter_mask`、`tx_drop_policy`。运行时改配安全(单上下文 deinit→重配→重 init,无竞态)。

## 测试:延迟 和 吞吐 是两个命令

延迟和吞吐是两个不同问题,像 `ping` 和 `iperf` 一样**分开测**——把满载下的排队当延迟会冤枉产品。

### 延迟 —— `latency.sh`(像 ping)

低速率(默认 100 fps/路,**~1 帧在飞**),CAN 总线不排队,量到的是**真实转发延迟**:

```bash
sudo ./scripts/latency.sh 192.168.8.113
```

```
=== product latency (lightly loaded -- the REAL forwarding latency) ===
  board (on-chip UDP<->CAN, DWT):    udp->can avg=38us max=120us   can->udp avg=15us max=60us
  roundtrip (Pi<->board<->Pi):      avg=0.42 ms  max=1.15 ms
  network+host (roundtrip - board): avg=0.37 ms  max=0.97 ms
  => board forwarding ~53 us (<< 1 ms); the rest is Ethernet + the Pi.
```

- **board** 是产品自身转发延迟(板载 DWT,微秒级,与主机时钟无关)——**这才是给客户看的延迟数**。
- **roundtrip** 是 Pi→板→Pi 整条往返(含 Pi 用户态收发);两者之差即"网络+主机"。

### 吞吐 / 丢帧 —— `stress.sh`

6 路打满,只看丢没丢 + 队列余量,**默认不报延迟 max**(满载下那个"max"是排队,不是延迟):

```bash
sudo ./scripts/stress.sh 192.168.8.113            # 默认 1000 fps/路、64B、10s
sudo ./scripts/stress.sh 192.168.8.113 2000       # 第二参数=速率/路,调高找无损天花板
```

```
can0<->can1: 20000 rx, 0 lost (0.00%)  -> PASS
can2<->can3: 20000 rx, 0 lost (0.00%)  -> PASS
can4<->can5: 20000 rx, 0 lost (0.00%)  -> PASS
board: drops tx=0 rx=0 overflow=0 queue_full=0  |  peak queue tx=11/64 rx=3/64
ALL PASS - 6 channels lossless at 1000 fps/ch
```

- **lost=0** 就是不丢帧;`peak queue` 还剩大把余量(满 64)说明扛得住。
- 台架把 6 路两两接成 3 条独立总线 `(can0,can1)(can2,can3)(can4,can5)`(各 120Ω×2),拿邻路当对端,等价客户现场 6 条独立总线。接线不够先接一条:`sudo IFACES="can0 can1" ./scripts/stress.sh 192.168.8.113`(环境变量放 `sudo` 之后,否则被 sudo 丢弃)。
- 只有丢包时才打印板子各通道计数做归因:`state=error-passive` → 总线接线/终端;`rx_fifo_overflow` → 板子 RX 封顶;`queue_full/tx_drop` → 板子 TX 封顶。
- 调优用隐藏开关:`sudo DEBUG=1 ./scripts/stress.sh 192.168.8.113` 才会吐出板内 DWT 分解 + 每段轮询耗时(`poll-leg max`)。平时不给客户看。

## 为什么要实时调度

板子自身转发只要 ~几十µs(见 `latency.sh`)。满载下偶发的毫秒级延迟尾巴,根因是 **Pi 非实时调度**把桥卡住后一次性甩出突发,板子被迫在 1M/5M 的 CAN 线上逐帧串行(~140µs/帧)。把桥(和压测工具 `canmesh`)提到 `SCHED_FIFO` + `mlockall` 后,突发消失、尾巴大幅下降(实测往返 max 34→14ms、板 udp→can max 8.9→3.7ms)。若仍需更紧,下一步是 CPU 隔离(`isolcpus`)+ 网卡中断绑核。

## 注意

- **一块板只能跑一个桥**:板子把 CAN 帧回发给"最后一个跟它通信的对端",别同时起两个桥。
- 比特率只能经板子设(`canbridge_ctl`),不能在 vcan 口上 `ip link set`。
- 本仓库在 Windows 上用 `core.autocrlf=true` 编辑;`.sh` 必须保持 **LF**(已用 `.gitattributes` 锁定)。否则拷到 Linux 上 shebang 带 `\r`,执行会报 `No such file or directory`。

## 兼容性

| 能力 | RK3588(vendor 6.1) | 树莓派 OS | Ubuntu aarch64 |
|---|---|---|---|
| `vcan` + `CAN_RAW` | ✅ | ✅ | ✅ |
| `CAN_RAW_FD_FRAMES` + mtu 72 | ✅ | ✅ | ✅ |
| `SCHED_FIFO` 实时(需 root,systemd 已配) | ✅ | ✅ | ✅ |

> 同一份源码 `make` 即可,vcan 内核自带、免编译。

## 目录

```text
linux/
  install.sh                一键编译 + 安装 + 起服务(含实时调度)
  Makefile
  src/canbridge.c           桥本体(UDP↔vcan demux,SCHED_FIFO 实时)
  src/canbridge_ctl.c       控制客户端(JSON: get_status / set_can_config / reset_stats)
  src/canmesh.c             压测 / 延迟测量工具(共享总线 mesh,自带实时调度)
  src/can_gateway_wire.h    v3 线格式(与固件 source/can_gateway_protocol.h 对齐)
  scripts/latency.sh        延迟测试(ping 式,低速)
  scripts/stress.sh         丢帧 / 吞吐测试(满载;DEBUG=1 出详细)
  scripts/setup-vcan.sh     建 can0..5(mtu 72)
  systemd/                  服务单元 + 默认配置
```
