# FRDM-MCXE31B 以太网转 6 路 CAN FD 网关

MCXE31B(= S32K314,Cortex-M7F@160MHz)裸机超级循环 + lwIP(NO_SYS)的 Ethernet-to-6×CAN/CAN FD 网关参考工程。当前阶段目标:**先把 UDP 链路打通,6 路同构、稳定、不丢帧**。

当前范围:

- 目标板:FRDM-MCXE31B
- 通道:**6 路 CAN(CAN0–CAN5)默认全部使能**(`CAN_ACTIVE_MASK=0x3F`),配置完全相同
- 默认 CAN:CAN FD,仲裁段 1 Mbps,数据段 5 Mbps,BRS on
- 数据端口:UDP `50000`;控制端口:UDP `50001`
- 协议:`SCGW` v3,项目内 UDP tunnel(变长帧记录,见 `docs/IMPLEMENTATION.md`)
- CAN 驱动:**全 6 路统一,纯轮询(无 FlexCAN 中断),RX 用独立邮箱组 + IRMQ 接收队列**(不依赖 CAN0 的 Enhanced RX FIFO);评审/实现见 `docs/CODE_REVIEW.md` / `docs/IMPLEMENTATION.md`

> **RX 深度上板验证项**:每路 RX 邮箱组为 `5` 深(64B FD 下最小实例 CAN3/4/5 的硬件上限,6 路取齐)。已开启 `MCR[IRMQ]` 让邮箱组真正级联成 5 深接收队列(未开 IRMQ 时最低邮箱会被就地覆盖,有效深度仅 1)。`5` 深在 6×满载下是否足够,**必须上板实测**(见 `docs/IMPLEMENTATION.md §3`)。

## 架构

```text
main.c                  超级循环: ethernet_lwip_poll / can_service_poll / can_udp_gateway_poll
  ethernet_lwip.c       lwIP、DHCP/静态 IP、link/IP 状态
  can_service.c         FlexCAN 边界: 6 路纯轮询、TX/RX 队列、运行时配置、硬件计数
  gateway_router.c      通道校验、UDP↔CAN 路由、peek/commit 背压、状态快照
  can_udp_gateway.c     UDP data tunnel(50000) + JSON 控制面(50001)
  can_gateway_protocol.h  SCGW v3 线格式(MCU 与上位机共享定义)
  latency_timer.h         DWT 周期计数器 + 延迟统计(板内转发延迟测量)

tools/can_gateway_protocol.py   上位机共享协议 codec
tools/win_can_udp_test.py       Windows 配置 / 冒烟 / 多路压测 CLI
tools/socketcan_udp_bridge.py   Linux vcan/SocketCAN 双向桥
```

设计取舍:

- `fsl_flexcan` SDK driver 原样使用;`can_service.c` 只封装 init/re-init、TX/RX 队列、配置和硬件计数。
- 6 路代码路径完全一致,便于验证;CAN0 的 Enhanced RX FIFO 刻意不用。
- Linux 用户侧直接用 SocketCAN/can-utils 生态(`candump`/`cangen`/`canplayer`/`canbusload`)。
- `SCGW` v3 是项目内 tunnel 协议,不是行业标准;对外主接口是 Linux SocketCAN。

## 找到板子 IP

板子默认走 **DHCP**:从串口日志里看 `Ethernet: DHCP bound <ip>`。下面示例统一用 `192.168.8.107`,请替换成你的实际 IP。

无 DHCP 网络可改**静态 IP**:编译宏 `ETHERNET_LWIP_USE_DHCP=0`(默认 1),静态地址 `192.168.8.50/24`、网关 `192.168.8.1`(见 `source/ethernet_lwip.c`)。

## 控制面命令(JSON over UDP 50001)

Windows 工具是 6 个互斥动作(每次选一个):

| 动作 | 作用 |
|---|---|
| `--status` | 查状态(人类可读;加 `--json` 看原始);**已含各路运行配置** |
| `--config --channel N` | 运行时改某路 CAN 配置 |
| `--send --channel N` | 向某路发帧(配 `--id`) |
| `--listen` | 打印板子从 CAN 转发上来的帧 |
| `--pressure --channels ..` | 压测 + PASS/FAIL 汇总 |
| `--reset-stats` | 清零所有计数 |

`--config` 可改项(只带要改的):`--channel`(0–5,必填)、`--enabled/--no-enabled`、`--fd/--no-fd`、`--bitrate`(50k–1M)、`--data-bitrate`(500k–5M)、`--brs/--no-brs`、`--filter accept_all|id_mask`、`--filter-id`、`--filter-mask`。运行时改配是安全的(单上下文 deinit→重配→重 init,无竞态)。

## Windows 配置与操作

配置某路(以 CAN3 为例,FD 1M/5M BRS):

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --config --channel 3 `
  --fd --bitrate 1000000 --data-bitrate 5000000 --brs
```

软件 ID 掩码过滤(只收 `0x100..0x10F`):

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --config --channel 0 `
  --filter id_mask --filter-id 0x100 --filter-mask 0x7F0
```

发一帧 CAN FD+BRS 到 CAN0(PCAN-View 直接可见;同总线的其它通道会收到并经 `--listen` 回显):

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --send --channel 0 --id 0x123 --fd --brs --data "11 22 33 44"
```

监听板子从 CAN 转发上来的帧(PCAN-View 发 → 板子 → 这里;另开一个终端):

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --listen --timeout 30
```

查状态 / 清计数:

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --status
python .\tools\win_can_udp_test.py --board 192.168.8.107 --reset-stats
```

收不到帧时按此顺序排查:

```text
tunnel.rx_frames 不增加            UDP/协议没进 MCU(IP/端口/防火墙)
router.rx 增加但 ch.tx_start 不增   配置、队列或 frame 校验问题
ch.tx_start/tx_done 增加            CAN 总线、对端配置或物理层问题
```

## Windows 压测

压测**开跑前自动 reset 计数**,所以汇总、各路增量、以及板内 `board DWT` 延迟(含只增不减的 max 高水位)都只反映**本次运行**——无需手动先 `--reset-stats`。结束打印一行汇总 + `PASS`/`FAIL`。同总线时建议**单路**压测(多路同总线 = 总线争用,不是带宽叠加):

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --pressure --channel 0 `
  --fd --brs --dlc 64 --duration 600 --rate 1000
```

各路独立总线时,可 round-robin 同时压 6 路。注意 `--rate` 是**所有通道合计**帧率(round-robin 平摊),所以 6 路各跑 1000 fps 需 `--rate 6000`:

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --pressure --channels 0 1 2 3 4 5 `
  --fd --brs --dlc 64 --duration 600 --rate 6000
```

6 路接**同一条总线**(如都接 PCAN):一路发,用 `--rx-watch` 校验其余各路是否每帧都收到:

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --pressure --channel 0 --rx-watch 1 2 3 4 5 `
  --fd --brs --dlc 64 --duration 30 --rate 1000
```

- `--channel N` / `--channels ...`:单路 / 多路 round-robin 发送。`--rate` 为**合计**帧率,N 路平摊(每路 ≈ rate/N);想每路 R fps 用 `--rate N×R`。
- 压测出现 `SATURATED`(`queue_full>0` 或有帧未上总线)即判 **FAIL**——表示该速率板子无法无损承受,应降低 `--rate` 找到可持续上限。
- `--rx-watch N ...`:同总线上**应当收到**这些帧的通道;`rx < 上总线数` 即判 **FAIL**(抓拔线/丢帧)。即使不加,任何**非发送通道收到一部分**(<上总线数)也会自动 FAIL。
- 汇总比对压测前后:发送数 vs 板子 `rx_frames`、`tunnel.drop`/`loss`、`router.drop`、各路 `rx_drop`/`tx_drop`/`rx_fifo_overflow`/`error`/`bus-off`,并打印 `board DWT us`(板内延迟,见下节)。
- 加 `--json` 打印压测后完整状态。

**验收判据(任一不满足即不达标):**

```text
压测汇总 = PASS
tunnel.loss = 0          UDP 序列无丢包
router.drop / queue_full = 0
每路 rx_drop = 0         RX 环满丢帧
每路 rx_fifo_overflow = 0  硬件邮箱 overrun(被覆盖)丢帧
每路 error = 0           tx/rx 错误 + tx_timeout 聚合
每路 state ≠ bus-off
tx_done 跟得上 tx_start
watermark 不长期顶满
```

> 注:`rx_fifo_overflow` 现统计**硬件邮箱 overrun**(一帧在被读走前被覆盖=丢帧);`error` 聚合里也含它。两者都为 0 才算 RX 无损。

## 延迟测量(板内,DWT 真值)

固件用 Cortex-M7 的 **DWT 周期计数器**(160MHz,6.25ns 分辨率)给每帧打戳,得到**不依赖任何上位机时钟**的板内转发延迟。状态里多了 `latency_us` 字段,`--status` 与 `--pressure` 都会显示:

```text
latency_us (board, DWT): udp->can avg=.. /max=.. can->udp avg=.. /max=.. loop avg=.. /max=..
```

| 字段 | 含义(打戳点) | 与 CAN 负载 |
|---|---|---|
| `udp_to_can` | UDP 包到达 → 帧写入 CAN 发送邮箱(以太网→CAN) | **有关**:总线忙时含等空闲邮箱的排队(每帧上线约 140us) |
| `can_to_udp` | 从 CAN 收邮箱读出 → 交给以太网发(CAN→以太网) | **基本无关**,约几十 us |
| `loop` | 一圈超级循环耗时(轮询粒度) | 有关但有界;查 status 时因拼 JSON 会短时变大,与数据转发无关 |

**注意累计语义**:`--status` 看到的 `latency_us` 是**自上次 `--reset-stats` 起累计**,其中 `max` 是**只增不减的高水位线**——不 reset 的话它会一直保留历史最坏值(容易误以为当前还很差)。`--pressure` **开跑前已自动 reset**,所以它输出的 `board DWT` 就是**本次运行**的值(含干净的 max);独立用 `--status` 看某段窗口时,先 `--reset-stats`。

**用户级"以太网→CAN"完整延迟 ≈ `loop`(几十 us) + `udp_to_can` + ~140us 上线物理**;轻载下约 200–300us,远 < 1ms。

量**与负载无关的纯处理开销**:看 `can_to_udp`,或用**逐帧发**(一帧一个 UDP 包,邮箱总空闲)测 `udp_to_can`:

```powershell
python .\tools\win_can_udp_test.py --board 192.168.8.107 --reset-stats
python .\tools\win_can_udp_test.py --board 192.168.8.107 --send --channel 0 --id 0x100 `
  --fd --brs --data "11 22 33 44 55 66 77 88" --count 5000 --interval-ms 1
python .\tools\win_can_udp_test.py --board 192.168.8.107 --status
```

> `udp_to_can` 在"压测一次塞 16 帧"时会偏大——那是 16 帧共用一条总线的串行化(物理决定,非板子慢),不是网关处理慢。要"应用感知"的真实值,就让上位机一帧一个 UDP 包发(如上)。

## Linux SocketCAN

创建 6 个 vcan 并启桥(接口按位置映射到 channel 0..5):

```bash
sudo modprobe vcan
for i in 0 1 2 3 4 5; do sudo ip link add dev vcan$i type vcan; sudo ip link set up vcan$i; done
python3 tools/socketcan_udp_bridge.py --remote-host 192.168.8.107 \
  --can vcan0 vcan1 vcan2 vcan3 vcan4 vcan5 --stats-interval 1
```

也可 `--setup-vcan` 让脚本自动创建并拉起 `--can` 指定的接口。标准工具:

```bash
candump vcan3                  # 看 CAN3→以太网过来的帧
cangen vcan3 -g 1 -L 64 -f     # 向 CAN3 灌 FD 帧(压上位机→CAN)
canplayer vcan0=can0 -I trace.log
```

## 构建

```powershell
& 'C:\Keil_v5\UV4\UV4.exe' -b 'enet2can_e31.uvprojx' -t 'enet2can_e31 debug' -o 'debug\codex_build.log'
```

期望结果:

```text
"debug\enet2can_e31.out" - 0 Error(s), 0 Warning(s).
```

> 改了**强制包含**的头(如 `source/mcux_config.h`,经 `-include` 注入、不被依赖跟踪)后,用 `-r`(全量重建)而非 `-b`,否则改动可能不生效。
