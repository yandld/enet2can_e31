# MCXE31B 以太网转 6 路 CAN FD 网关

MCXE31B(Arm Cortex-M7F@160MHz)裸机超级循环 + lwIP(NO_SYS)的 Ethernet↔6×CAN/CAN FD 网关。对外主接口是 **Linux SocketCAN**:在你的 SoC(RK3588 / 树莓派等)上装一次,`vcan-gw0..vcan-gw5` 直接像本地 CAN 口用。

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

板子默认 **DHCP 优先、静态兜底**:link up 后最多等 10s 拿 DHCP 租约,拿到即用;超时则回退静态 `192.168.8.113/24`、网关 `192.168.8.1`(DHCP 仍在后台跑,之后拿到租约会自动切回)。从串口那行 `Ethernet: IP = <ip>  (DHCP|static)` 读实际地址。下文示例统一用 `192.168.8.113`。

纯静态部署(无 DHCP、免那 10s 等待):编译宏 `ETHERNET_LWIP_USE_DHCP=0`,开机直接上静态。地址与超时见 `source/ethernet_lwip.c` 顶部宏。

---

# Linux 端 —— SocketCAN 桥

`linux/` 是一套用户态 C 工具(libc、单二进制、**零内核模块、免编译内核**),把板子**一条 v3 UDP 流按每帧 channel 字节 demux** 成 6 个标准 SocketCAN 口 `vcan-gw0..vcan-gw5`(用内核自带 `vcan` 实现)。RK3588 / 树莓派 / 任意 aarch64 通吃,同一份源码 `make` 即可。

## 架构

```text
 你的App / candump / cangen          canbridge 守护进程              MCXE31B 板子
  (标准 SocketCAN API)              (单 epoll + recvmmsg)
        │                                   │
  vcan-gw0..5 (vcan) ◀── 注入 ────┐        │
        │                          ├──▶ 一条 UDP 流 (50000) ◀───▶ FlexCAN0..5 ─ CAN 总线
        └─── 读 vcan-gwX ─ 聚合 ───┘        │   每帧带 channel 字节
                                            │
   canbridge_ctl ───── JSON 控制 (50001) ───┴──────────────────▶  设比特率 / 查状态
```

- **下行**(板→主机):板子 UDP 帧 → canbridge 按 channel 字节注入对应 `vcan-gwX`,`candump vcan-gwX` 就能看到。
- **上行**(主机→板):`vcan-gwX` 上的帧 → canbridge 聚合成 ≤16 帧的 v3 datagram → 发给板子 → 板子在对应 FlexCAN 口发出。
- **一块板配一个桥**:板子把 CAN 帧回发给"最后一个跟它通信的主机",同一块板别同时起两个桥(会互相抢回程)。
- 单 epoll 线程 + `recvmmsg` 一次排空积压;`SO_RXQ_OVFL` 让桥能**自证有没有丢**(区分"桥丢"还是"板丢")。
- **实时调度**:桥跑在 `SCHED_FIFO` + `mlockall`(systemd 配置 + 代码兜底),避免被普通调度卡住后甩出突发——那是板内延迟尾巴的主因,详见末节。

## 安装(只做一次)

```bash
cd linux
make
sudo ./install.sh 192.168.8.113        # 板子 IP;编译、装 systemd 服务、开机自起、配置实时调度
```

`install.sh` 会:编译 → `modprobe vcan` 建好 `vcan-gw0..vcan-gw5` 并设 **`mtu 72`**(CAN FD 必需,漏设会把 64B FD 帧静默丢)→ 装服务并(重)启动。**默认用 `vcan-gw0..5`(刻意避开客户已有的物理 `can0/can1`),详见下节[接口命名与碰撞规避](#接口命名与碰撞规避)。** 验证:

```bash
systemctl status mcxe31b-canbridge
chrt -p $(pgrep -x canbridge)           # 应显示 SCHED_FIFO priority 50(实时生效)
candump vcan-gw0                        # 应能看到 CAN0 转上来的帧
```

## 接口命名与碰撞规避

本桥的接口是用内核 `vcan` 建的**虚拟口,不是物理 CAN 控制器**。客户主机常常**已经有物理 `can0`/`can1`**(板载收发器、别的以太网-CAN 网关),若我们也用 `can0..can5` 就会撞名:`ip link add` 失败,或更糟——桥**绑到错误的物理口**,把以太网流量误注入真实 CAN 总线,静默串线。

为此本桥**默认就用 `vcan-gw0..vcan-gw5`**——刻意避开 `can0/can1`,开箱即不撞名,无需任何额外操作。要换名字(比如连 `vcan-gw*` 都被占了),用 `--prefix`:

```bash
sudo ./install.sh 192.168.8.113 --prefix mybus    # 改建 mybus0..mybus5
```

前缀限字母数字 / `-` / `_`,且 ≤14 字符(SocketCAN 接口名上限 15)。**注意**:`install.sh` 每次运行都会按 `--prefix`(默认 `vcan-gw`)重写 `IFACES`,所以持久自定义名字请用 `--prefix`;直接手改 `/etc/default/mcxe31b-canbridge` 的 `IFACES=` 只在下次重装前有效(改完 `sudo systemctl restart mcxe31b-canbridge` 生效)。

两条**不能破坏**的规则:

- **顺序即通道**:`IFACES` 的顺序就是固件通道号 0..5,重排会把帧路由到**错通道**。
- **不能减项**:固件始终发 6 路;`IFACES` 少列几个,缺的通道会被**静默丢帧**。

**若主机的 `can0` 是真实 CAN 控制器**(而非别的 vcan):改名只在 vcan 命名空间内有意义,救不了真实总线撞名——应在装桥**之前**用 udev 给物理口改名(用你的驱动/路径/`ifindex` 等稳定属性匹配真实口),例如:

```
# /etc/udev/rules.d/90-can-rename.rules
SUBSYSTEM=="net", ACTION=="add", DRIVERS=="<your-can-driver>", NAME="can-phys0"
```

## 日常使用

`vcan-gw0..vcan-gw5` 就是标准 SocketCAN 接口,整个 can-utils / python-can 生态直接可用:

```bash
candump vcan-gw0                        # 看收到的帧
cangen  vcan-gw0 -g 1 -L 64 -f -b       # 往 CAN0 发 CAN-FD 帧
```

## 设 CAN 比特率 / 看状态(经板子下发)

`vcan-gw0..vcan-gw5` 是虚拟口,`ip link set <iface> type can bitrate ...` **不可用**;真实比特率发给板子,用 `canbridge_ctl`(控制面 = UDP 50001 上的 JSON):

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
- 末尾 **board eth-to-eth** 才是唯一对客户有意义的数:板子用 DWT(微秒级、与主机时钟无关)测的同一帧最长内部路径——网卡收帧 → CAN 片内自环 → 重新递交网卡发送,已含 lwIP 收发栈开销。**`MAX` 就是板内最坏延迟**,给客户看这一个即可(数字为样例)。
  - 软件量程外只剩纯硬件:MAC DMA + PHY 线上序列化,及 RX 环等下次轮询(上界=超级循环周期);要"线上 SOF→SOF"需上 ENET IEEE 1588 硬件打戳。
- 想看时间花在哪一段(`udp->can` / `can->udp` 分段,现已含 lwIP 那段)只为工程定位用,对客户无意义、默认隐藏:`sudo DEBUG=1 ./scripts/latency.sh 192.168.8.113`。
- 只测部分通道:`sudo IFACES="vcan-gw0 vcan-gw1" ./scripts/latency.sh 192.168.8.113`(环境变量放 `sudo` 之后,否则被 sudo 丢弃)。脚本退出时会自动把 loopback 关掉,板子恢复正常总线转发。
- **自环开 BRS(真跑 5M 数据相位)**:固件按 RM 在回环通道显式关掉 TDC(NXP SDK 漏关会导致自发 BRS 帧收不回),所以自环测的是真实 FD+BRS、RTT 含快速相位空中时间(已在硬件验证:6 路 0 错误)。仍绕过收发器/线缆/终端,真实总线信号质量需接真实节点验证。

### 吞吐 / 丢帧 —— `stress.sh`

6 路打满,只看丢没丢 + 队列余量,**默认不报延迟 max**(满载下那个"max"是排队,不是延迟):

```bash
sudo ./scripts/stress.sh 192.168.8.113            # 默认 1000 fps/路、64B、10s
sudo ./scripts/stress.sh 192.168.8.113 2000       # 第二参数=速率/路,调高找无损天花板
sudo ./scripts/stress.sh 192.168.8.113 2000 30    # 第三参数=时长(秒)
```

```
vcan-gw0..5(6ch loopback): 60000 rx, 0 lost (0.00%)  -> PASS
board: drops tx=0 rx=0 overflow=0 queue_full=0  |  peak queue tx=11/64 rx=3/64
ALL PASS - 6 channels lossless at 1000 fps/ch
```

- **lost=0** 就是不丢帧;`peak queue` 还剩大把余量(满 64)说明扛得住。
- 同样走**片内 CAN 自环,免接线**:每路自发自收,等价客户现场每路一条独立总线满载。只压部分通道:`sudo IFACES="vcan-gw0 vcan-gw1" ./scripts/stress.sh 192.168.8.113`(环境变量放 `sudo` 之后,否则被 sudo 丢弃)。脚本退出时自动关 loopback。
- 只有丢包时才打印板子各通道计数做归因:`rx_fifo_overflow` → 板子 RX 封顶;`queue_full/tx_drop` → 板子 TX 封顶;自环下 `state` 不该是 error-passive(没有真实总线)。
