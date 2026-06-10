# MCXE31B 以太网转 6 路 CAN FD 网关

MCXE31B(Arm Cortex-M7F@160MHz)裸机超级循环 + lwIP(NO_SYS)的 Ethernet↔6×CAN/CAN FD 网关。对外主接口是 **Linux SocketCAN**:在你的 SoC(RK3588 / 树莓派等)上装一次,`can0..can5` 直接像本地 CAN 口用。

规格:

- MCU:**MCXE31B**(Arm Cortex-M7F@160MHz)
- 通道:**6 路 CAN(CAN0–CAN5)默认全开**(`CAN_ACTIVE_MASK=0x3F`),6 路配置完全一致
- CAN:**CAN FD**,仲裁 1 Mbps / 数据 5 Mbps / BRS on(也支持 CAN 2.0)
- 以太网:**RMII**(10/100M),**DHCP** 或静态 IP

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
- **一块板配一个桥**:板子把 CAN 帧回发给"最后一个跟它通信的主机",同一块板别同时起两个桥(会互相抢回程)。
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

`set_can_config` 可改项:`channel`(0–5,必填)、`enabled`、`fd`、`bitrate`(50k–1M)、`data_bitrate`(500k–5M)、`brs`、`loopback`(片内自环自测,免接线,见下方测试)、`filter`(`accept_all`/`id_mask`)、`filter_id`、`filter_mask`、`tx_drop_policy`。运行时改配安全(单上下文 deinit→重配→重 init,无竞态)。

## 测试:延迟 和 吞吐 

### 延迟 —— `latency.sh`(类似 ping,片内自环,免接线)

像 `ping`:每轮在**每条通道各发 1 帧**,板子把每帧在**片内 CAN 自环**(FlexCAN 自接收)后从**同一条通道**送回,打印这一轮各通道往返,默认每秒一次(`Ctrl-C` 停)。同一时刻每路只有 1 帧在飞,所以每行 RTT 是**真实往返**,不是排队。**不用接任何 CAN 线/终端电阻**——板子自环。

```bash
sudo ./scripts/latency.sh 192.168.8.113          # 6 路同时 ping,每秒一次,Ctrl-C 停
sudo ./scripts/latency.sh 192.168.8.113 0.2      # 第二参数=间隔秒(像 ping -i,调小更快)
sudo ./scripts/latency.sh 192.168.8.113 1 10     # 第三参数=次数(0=无限)
```

```
PING loopback x6 (Pi->board->CAN-loopback->board->Pi), 64B FD, interval 1s
seq=0     c0=1.072 c1=1.041 c2=1.058 c3=1.033 c4=1.049 c5=1.021  max=1.072 ms
seq=1     c0=1.054 c1=1.032 c2=1.040 c3=1.025 c4=1.031 c5=1.011  max=1.054 ms
^C
--- loopback ping statistics (6 channels) ---
12 transmitted, 12 received, 0% loss
  c0 rtt min/avg/max = 1.054/1.063/1.072 ms
  ...
board eth-to-eth latency (eth-in -> CAN loopback -> eth-out, real us):  MAX=340us  avg=210us
```

- 每行 **cN** 是该通道 Pi→板→CAN 片内自环→板→Pi 整条往返(含 Pi 用户态收发),`max` 是这一轮最慢的一路。
- 末尾 **board eth-to-eth** 才是**唯一对客户有意义的数**:板子**自己用 DWT(微秒级,与主机时钟无关)测的同一帧最长内部路径**——从**网卡收到帧(MAC RX 取帧、lwIP 解析前)** 算起,经 CAN 片内自环返回,到**重新组好包递交网卡发送(MAC TX handoff)** 为止,**已包含 lwIP 收/发协议栈(eth/IP/UDP 解析与组头)的开销**。**`MAX` 就是产品板内最坏延迟**,给客户看这一个就够(数字为样例,实测以你的板子为准)。
  - 仍在软件量程之外的只剩**纯硬件**那一点:MAC DMA + PHY 线上序列化(≈帧的线上时间),以及帧到达后在 RX 环里等下一次轮询的那段(上界 = 超级循环周期)。要真正"线上 SOF 到 SOF"得上 ENET 的 IEEE 1588 硬件打戳,可作后续选项。
- 想看时间花在哪一段(`udp->can` / `can->udp` 分段,现已含 lwIP 那段)只为工程定位用,对客户无意义、默认隐藏:`sudo DEBUG=1 ./scripts/latency.sh 192.168.8.113`。
- 只测部分通道:`sudo IFACES="can0 can1" ./scripts/latency.sh 192.168.8.113`(环境变量放 `sudo` 之后,否则被 sudo 丢弃)。脚本退出时会自动把 loopback 关掉,板子恢复正常总线转发。
- **自环开 BRS(真跑 5M 数据相位)**:回环通道会按 RM 要求把 TDC 关掉(NXP SDK 在回环里只是跳过 TDC 配置、没真正关,于是复位默认 `ETDCEN=1` + 延迟测量开着,没有真实收发器可测 → SSP 放错 → 自发的 BRS 帧收不回)。固件显式 `ETDC=0` 后,片内自发的 5M BRS 帧也能自收(已在硬件上验证:6 路、0 错误),所以自环测试就是真实 FD+BRS,RTT 含真实快速相位空中时间。注意:回环仍绕过了收发器/线缆/终端电阻,**真实总线信号质量仍需接真实节点验证**。

### 吞吐 / 丢帧 —— `stress.sh`

6 路打满,只看丢没丢 + 队列余量,**默认不报延迟 max**(满载下那个"max"是排队,不是延迟):

```bash
sudo ./scripts/stress.sh 192.168.8.113            # 默认 1000 fps/路、64B、10s
sudo ./scripts/stress.sh 192.168.8.113 2000       # 第二参数=速率/路,调高找无损天花板
sudo ./scripts/stress.sh 192.168.8.113 2000 30    # 第三参数=时长(秒)
```

```
can0..can5(6ch loopback): 60000 rx, 0 lost (0.00%)  -> PASS
board: drops tx=0 rx=0 overflow=0 queue_full=0  |  peak queue tx=11/64 rx=3/64
ALL PASS - 6 channels lossless at 1000 fps/ch
```

- **lost=0** 就是不丢帧;`peak queue` 还剩大把余量(满 64)说明扛得住。
- 同样走**片内 CAN 自环,免接线**:每路自发自收,等价客户现场每路一条独立总线满载。只压部分通道:`sudo IFACES="can0 can1" ./scripts/stress.sh 192.168.8.113`(环境变量放 `sudo` 之后,否则被 sudo 丢弃)。脚本退出时自动关 loopback。
- 只有丢包时才打印板子各通道计数做归因:`rx_fifo_overflow` → 板子 RX 封顶;`queue_full/tx_drop` → 板子 TX 封顶;自环下 `state` 不该是 error-passive(没有真实总线)。
