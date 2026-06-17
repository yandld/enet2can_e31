# e2cf_mcxe31 — E2CF 网关固件(MCXE31B,裸机 v1)

iMX95(RT Edge Linux)↔ L2 switch ↔ MCXE31B 的 ETH↔6×CANFD 网关 MCU 侧实现。
协议与架构依据 `eth2can_design/` 三份设计文档(01 协议规范 / 02 Linux 侧 / 03 MCXE 侧)。

> **命名约定**:**E2CF** 是线上协议名(EtherType 0x88B5 帧格式、代码中
> `e2cf_*`/`E2CF_*` 符号、规范文档 01);**eth2can** 是实现该协议的 Linux
> 驱动/设备名(`eth2can.ko`、`eth2can0..5`);**canperf** 是配套测试工具。

基于 `enet2can_e31` 工程派生:**board/BSP/驱动完全相同**(FRDM-MCXE31B,
LAN8741 RMII PHY,FlexCAN PE 时钟已切 80 MHz AIPS_PLAT_CLK),删除了 lwIP/UDP
整套中间件,数据面改为纯 L2 EtherType 0x88B5(E2CF 协议)+ 分级中断。

## 构建

两种方式,同一编译器(Arm Compiler 6):

```sh
# 命令行(本机已验证:armclang 6.24,编译+链接零警告)
make            # 产出 build/e2cf_mcxe31.axf / .bin
# 或 Keil MDK:打开 e2cf_mcxe31.uvprojx
```

## 源码结构(source/)

| 文件 | 职责 |
|---|---|
| `e2cf_proto.h` | E2CF 线格式(与 Linux 驱动共享,小端 packed 结构) |
| `e2cf_config.h` | 全部编译期可调项(通道掩码、autostart、日志级别等) |
| `dbg_log.[ch]` | **全局可控调试打印**:编译期级别裁剪 + 运行期级别/模块掩码;ISR 安全(格式化进 8KB 环形缓冲,主循环限额泄给 UART,数据面零阻塞) |
| `gw_time.[ch]` | 网关单调时钟:DWT@160MHz 64 位扩展,ts_base 回绕连续 |
| `eth_raw.[ch]` | 裸 ENET_QOS:零拷贝收(EMAC ISR prio1,与 CAN 同级、无互抢)/发(DTCM 面直接 DMA) |
| `can_hw.[ch]` | 6 路 FlexCAN:全部走 RX MB bank(CAN0/1/2=8RX MB、CAN3-5=4RX MB;IRMQ 接收队列,按 16 位时戳排序上送);每通道**单活跃 TX MB** 严格保序 + sw_txfifo[16](=协议窗口);stuck-TX 看门狗 |
| `e2cf_core.[ch]` | 协议引擎:DATA/TXC 自适应聚合(满 17/32 条或 T_agg=50µs 死线到期即发;旧"egress 空闲即发"规则已删)、CFG token 幂等执行、HB 500ms 链路监督→安全态、EVT/TIME/STATS 周期上报 |
| `main.c` | 超循环:聚合死线、错误轮询 10ms、链路 100ms、日志/统计 |

## 调试打印(设计要求:全局开关)

- 编译期:`E2CF_LOG_BUILD_LEVEL`(高于此级别的语句整体剔除)
- 运行期:UART 单键控制台 —— `0..5` 设级别(NONE..TRACE)、`m` 轮换模块掩码、
  `s` 立即打印全部统计、`?` 帮助;也可代码调 `dbg_log_set_level/mask()`
- 周期统计:`E2CF_STATS_PERIOD_MS`(默认 **0/关闭**;统计改走带内 STATS 1Hz,见下方偏差表。设如 3000 可在单板台架开 UART 周期打印)
- 所有 LOG 调用 ISR 安全,绝不阻塞数据面;环满丢行计数可见

## 上电自检(bring-up 默认)

`E2CF_AUTOSTART_CHANNELS=1`:6 通道 1M/8M BRS 自动 START(协议要求的
"上电 STOP 等 CFG" 由置 0 恢复)。`E2CF_AUTOSTART_LOOPBACK=1` 可走片内回环
(无需收发器/接线,复用原工程验证过的 TDC-off 方案)。
安全态(HB 超时停发)在**收到第一个 HB 之前不武装**,便于单板调试。

## v1 与设计文档 03 的已记录偏差(均为阶段化,不改协议)

| 项 | 设计 | v1 实现 | 升级路径 |
|---|---|---|---|
| CAN0 eFIFO 搬运 | eFIFO+eDMA 水位4 | **RX MB bank**(8 MB,同 CAN1/2):eFIFO 使能后 RX 中断在本片上从未触发(疑似标志路由到未服务的 MB32-63 线),弃用 | Phase 2 恢复 eFIFO+eDMA(加 fsl_edma/dmamux,需先解决中断线路由) |
| 时基 | STM@80M + EMAC 1588 对齐 | DWT@160M(API 已隔离在 gw_time) | 换 gw_time 实现即可 |
| RX 时戳 | FlexCAN HR timestamp | MB 16 位时戳仅用于**组内排序**;ts_off 取 ISR 读取时刻(2µs 单位内偏差可接受) | Phase 2 配 HR 时戳时基 |
| SET_FILTER | 硬件 RXIMR/eFIFO 表 | 仅 accept-all(非全通返回 ENOTSUP,不静默) | Phase 2 |
| one-shot | CFG mode_flags[4] | ENOTSUP | Phase 2 |
| 非 ISO FD | CFG mode_flags[1] | ENOTSUP(控制器始终 ISO;Linux 驱动不广告 FD_NON_ISO) | 有需求时 FDInit 后清 ISOCANFDEN(约 6 行) |
| 看门狗 SWT | main 喂狗 | 未接入 | Phase 2 |
| 统计可观测性 | UART 3s 周期打印 | UART 周期打印**默认关闭**(`E2CF_STATS_PERIOD_MS=0`,'s' 键保留);统计改走带内 STATS 消息(类型 7,1 Hz,规范 v1.0-draft3 §4.9),Linux 经 `ethtool -S` / debugfs `eth2can/{stats,clear_stats}` / `ip -d link` berr 查看;CFG op=9 非破坏清零 | —(本项即协议正式机制) |
| **SDK 红线例外** | drivers/ 不修改 | `fsl_enet_qos.c` `ENET_QOS_SendFrame` 的环回绕 tail 指针改为环内回绕(stmmac 风格,标注 `E2CF WORKAROUND`):抓包证实原写法在回绕沿偶发"0 号槽帧丢失 + 旧描述符逐字节重发"(2/20 万帧);配套 TX 环 8→64、eth_raw 永久保留一个空槽。证据与机理见 `eth2can_design/eqos_tx_ring_wrap_report.md` | SDK 官方修复后回退此例外 |

## 内存布局

- DTCM 非 cache 区(16KB):EQOS 描述符环 + 8×1536B TX 聚合面(零拷贝、免维护;面 12KB,整个 ncache 区 = 8×1536+1024+128 = 13440B)
- SRAM(cache):EMAC RX 缓冲池(驱动做 invalidate 维护)
- 镜像:Code 46.6KB / ZI 67KB(512KB RAM 余量充足)

## Linux 侧

见 `linux/README.md`(第一阶段 EtherType 截留驱动 eth2can.ko)。
