# MCXE 侧详细设计:6 路 CANFD ↔ Ethernet 实时网关固件

**版本:** v1.0-draft1
**日期:** 2026-06-11
**目标硬件:** NXP MCXE31B(Cortex-M7F @160 MHz)
**SDK:** MCUXpresso SDK(`fsl_flexcan` + `fsl_enet_qos`)
**配套文档:** 《01_E2CF协议规范》《02_Linux侧详细设计》
**校准:** 2026-06-17 已与 v1 固件源码对齐 —— 网关时钟 = DWT@160 MHz(STM/1588 为 Phase-2)、周期任务在超环 `e2cf_core_poll`(无 STM 中断)、EMAC 与 CAN 同 NVIC prio 1、CAN0 eFIFO parked(走 MB 路径)、EQOS TX 环 64、one-shot = ENOTSUP、聚合 flush 仅靠"满"或 T_agg=50 µs 死线(已删 egress-idle)。

---

## 1. 芯片选型论证(结论:MCXE31B)

### 1.1 MCXE 系列横向对比(来源:MCXE31x 数据手册 Rev.2 2025-08 + SDK feature 头文件)

| 特性 | MCXE315/316 | MCXE317 | **MCXE31B** | MCXE24x |
|---|---|---|---|---|
| 内核/主频 | M7F/120 MHz | M7F/160 MHz | **M7F/160 MHz** | M4F/112 MHz |
| Flash / RAM | ≤1M / ≤128K | 2M / 192K | **4M / 512K(ECC,含 96K TCM)** | ≤2M / ≤256K |
| FlexCAN(全 FD) | 3 | 6 | **6** | 3 |
| Ethernet MAC | — | **—** | **1×EQOS 10/100 + TSN** | 1×ENET 10/100 |
| eDMA 通道 | 12 | 32 | **32** | 16 |

- **MCXE317 有 6 路 CAN 但没有以太网;MCXE24x 有以太网但只有 3 路 CAN。MCXE31B 是 MCXE 系列唯一同时满足两者的型号**(SDK `MCXE31B_features.h` 证实:`FSL_FEATURE_SOC_EMAC_COUNT=1`、6×FlexCAN 全部 `HAS_FLEXIBLE_DATA_RATE=1`、`FSL_FEATURE_FLEXCAN_MAX_CANFD_BITRATE=8000000`)。
- EMAC 实为 Synopsys DWC EQOS IP:2 DMA 通道/2 MTL 队列、描述符环、硬件 1588(4×PPS)、CBS/帧抢占(802.1Qbu)—— 10/100 Mbps,**无千兆**。带宽核算(协议 §1.3):6 路满载聚合后 ≤42 Mbps,100 Mbps 够用,但**聚合是强制的**。
- 评估板:**FRDM-MCXE31B**(LAN8741 PHY,RMII,板载仅 1 路 CAN PHY)。6 路网关需自制扩展板(§8)。

### 1.2 8 Mbps 数据相位的硬约束

1. **PE 时钟必须 80 MHz**:FlexCAN PE 时钟三选一(FIRC 48M / FXOSC / AIPS_PLAT_CLK 80M)。8 Mbps 用 80 MHz 得 10 tq/bit(CiA 601-3 推荐分辨率);48 MHz 仅 6 tq/bit 且 FIRC 是 RC 振荡器、频率容差不满足 CAN 要求。**上电默认是 48 MHz,固件必须显式 `CLOCK_AttachClk(kAIPS_PLAT_CLK_to_FLEXCAN012_PE / _FLEXCAN345_PE)`**(官方 `efifo_edma_transfer` 示例同此做法)。80 MHz 由 16 MHz 晶振经 PLL 生成,精度合规。
2. **收发器必须 CAN SIC 级**:8 Mbps 超出普通 HS 收发器(≤5 Mbps)位对称性指标,选 **NXP TJA1463**(CiA 601-4 SIC,官方标称 8 Mbit/s)×6。FRDM 板载那路 PHY 型号未公开,验证 8 Mbps 一律走扩展板。
3. **TDC**:8 Mbps 下必须开启发送器延迟补偿(EDCBT/TDC 寄存器),tdc_off 由协议 CFG 下发。

### 1.3 每实例资源差异(影响通道分配)—— 已确认的硬件不对称性

**关键事实(已确认):6 个 FlexCAN 实例全部带 eDMA 请求线(CAN0–3 在 DMAMUX0,CAN4/5 在 DMAMUX1),但 Enhanced RX FIFO 仅 FlexCAN_0 有。** 注意"有 DMA 请求线"≠"FD 模式下 DMA 可用":FlexCAN 的 DMA 只服务于 FIFO 输出口(无 TX DMA、无 MB DMA),因此各实例 RX 机制的真实可用性如下:

| 实例 | MB 数(8B 折算) | 64B-payload MB(≈) | Enhanced RX FIFO(支持 FD) | 传统 RX FIFO(**不支持 FD**) | DMA 请求线 | **FD 模式下 RX 实际可用机制** |
|---|---|---|---|---|---|---|
| CAN0 | 96 | ~21 | **✅ 20 帧深,128 过滤器** | ✅ | ✅ | **v1:MB + 中断**(eFIFO+eDMA 已 parked,Phase-2,见 §3.3) |
| CAN1/2 | 64 | ~14 | ❌ | ✅(FD 下不可用) | ✅(仅能接传统 FIFO→FD 下闲置) | **MB + 中断** |
| CAN3/4/5 | 32 | ~7 | ❌ | ✅(FD 下不可用) | ✅(同上) | **MB + 中断** |

由此导出三条设计规则:

1. **FD 模式下 CAN1–5 只能 Message Buffer + 中断接收**,DMA 通道虽存在但无处可接 —— §3.3 的 MB 中断方案不是偏好,是硬件唯一解;CPU 预算(§5)按此核算。
2. **通道分配规则**:负载最重/延迟最敏感的总线必须接 CAN0(eFIFO 20 帧深 ≈2 ms 缓冲余量 + eDMA 近零 per-frame CPU);其次按 MB 数排序 CAN1/2(可配更深 RX MB 组)优于 CAN3–5。E2CF 协议的 `chan` 编号是逻辑号,物理映射在部署指南中文档化。
3. **经典 CAN 通道的可选降级优化**:若部署确定某通道只跑 Classical CAN(8B),该通道可改用传统 RX FIFO(6 帧深)+ DMA —— 此时该实例的 DMA 请求线重新可用,缓冲深度 6 帧、IRQ 率摊薄。做成每通道编译/CFG 配置项,默认关闭(FD 优先)。

---

## 2. 软件架构决策:裸机数据面,不用 RTOS、不用 lwIP

### 2.1 CPU 预算(决策依据)

- 6 路 64B 满载:61 380 帧/s → 160 MHz ÷ 61.4k = **2 600 周期/帧**;双向同时满载事件率 ≈123 k/s → **1 300 周期/事件**。
- 8B 小帧风暴:145 900 帧/s 接收 → **1 100 周期/帧**。

| 方案 | 每帧成本 | 结论 |
|---|---|---|
| **裸机 ISR + raw L2 帧(本设计)** | CAN ISR 读 MB+编码 ~200–400 周期;EQOS 零拷贝 ring 收发 ~300–800 周期/以太帧(聚合后摊薄到每 CAN 帧 <100) | ✅ 满载预算内,余量 >50% |
| FreeRTOS 任务化数据面 | 每帧唤醒任务 = 61k 次/s 上下文切换 ≈10–20% CPU 纯开销,且引入调度抖动 | ❌ 数据面不可;管理面无必要(本设计管理面走 E2CF CFG,无独立 IP 服务) |
| lwIP UDP | 每包 2 000–5 000+ 周期,n=1 时需 120–300 M 周期/s | ❌ 超预算;且 +10–30 µs 级抖动 |

**决策:纯裸机(无 RTOS、无 lwIP)。** E2CF 是 raw L2 协议(EtherType 0x88B5),解析定长小端结构体即可;配置/诊断走协议内 CFG/EVT 通道,不需要任何 IP 栈。固件形态 = `main()` 超循环(后台杂务)+ 分级 NVIC 中断(全部数据面工作)。这是满足"latency 足够小 + performance 接近线速"的唯一稳健形态(对照:RTEMS 网关实测 15 µs、CAST 硬件网关 30 µs、Linux 用户态 3 ms —— 路径越短越确定)。

> 若产品线后续要求 OTA/Web 诊断,可加 FreeRTOS 仅托管管理面(低优先级任务 + EMAC ring1/MTL 队列 1 跑 lwIP),数据面仍在 ISR 直通 —— 架构已预留(§3.2 双队列),但 v1 不实现。

### 2.2 固件模块划分

```
┌────────────────────────── MCXE31B 固件(裸机)──────────────────────────┐
│  main 超循环:看门狗、HB/EVT 100ms 节拍(超环 poll)、统计、TIME 1Hz      │
│ ─────────────────────────── 中断域(数据面)────────────────────────────│
│  EMAC RX ISR(prio 1,与 CAN 同级):收 E2CF 帧→校验→demux                            │
│     DATA→写各通道 SW-TXFIFO→踢 CAN TX   CFG→执行+回 RSP   HB→喂链路监督  │
│  CAN0 Enhanced RX FIFO 已 parked(Phase-2);v1 走 MB 路径             │
│  CAN0..5 MB ISR ×6(prio 1):逐帧取 RX→聚合缓冲;TX-complete→TXC 记录+   │
│     从 SW-TXFIFO 装下一帧                                                │
│  SysTick(prio 3,1ms):推进 ms+DWT 回绕;周期 flush/超时在超环 poll      │
│  EMAC TX:写 ring0 描述符+tail doorbell;完成回收在下次发送前轮询          │
│ ───────────────────────────── 数据结构 ─────────────────────────────────│
│  agg_buf[2](乒乓,DTCM) │ sw_txfifo[6][16](DTCM) │ txc_buf │ stats     │
│  EQOS ring0 描述符+帧缓冲(SRAM,非 cache 或显式维护)                    │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 详细设计

### 3.1 时钟与初始化序列

1. FXOSC 16 MHz → PLL → CORE_CLK 160 MHz,AIPS_PLAT_CLK 80 MHz。
2. `CLOCK_AttachClk(kAIPS_PLAT_CLK_to_FLEXCAN012_PE)` + `..._FLEXCAN345_PE`(分频 ÷1)。
3. EMAC:RMII 50 MHz 参考时钟,PHY(LAN8741/自制板同级)自协商 100M 全双工。(EMAC 1588 时基为 Phase-2;v1 网关时钟用 Cortex-M7 DWT 周期计数器,见 §3.6。)
4. 网关时钟(v1):Cortex-M7 **DWT 周期计数器 @160 MHz**(6.25 ns/tick),经 1 ms SysTick 扩展为 64 位单调钟,作为 `ts_base` 与 T_agg/节拍定时源(`gw_time.c`)。STM@80 MHz 对齐 EMAC 1588 为 Phase-2 目标(同一 `gw_time` API 后端替换)。
5. 各 FlexCAN:`FLEXCAN_FDInit`(默认 1M/5M,等 Linux 侧 CFG 覆写);**冻结模式等待 START** —— 上电不自动上总线(协议 §4.8 安全语义)。
6. EMAC 描述符环:ring0 = 数据面(RX 16 描述符 ×1536B,TX 64 描述符 —— 8→64 治 TX ring wrap 异常);ring1 预留管理面,v1 关闭。MAC 过滤:单播本机 MAC + 组播 `01:E2:CF:00:00:01`(仅 HB);VLAN tag 不剥离(软件解析 PCP/VID)。

### 3.2 中断优先级表(NVIC,数值越小越高)

| prio | 中断 | 预算 | 说明 |
|---|---|---|---|
| 1 | CAN0 eFIFO eDMA、CAN1..5 MB(RX+TX 完成) | 单次 <2.5 µs | CAN 事件最高:RX 不取走会溢出。缓冲余量:CAN0 eFIFO 20 帧 ≈2 ms;CAN1/2 8×RX MB ≈782 µs(64B)/ 329 µs(8B);CAN3–5 4×RX MB ≈391 µs(64B)/ **164 µs(8B 风暴,最紧死线)** |
| 1 | EMAC RX ch0(**与 CAN 同级**) | 单帧 n=17 解析+分发 <30 µs | Linux→CAN 方向延迟主路径。A/B 实验:EMAC=2 反致 face starv +105、downlink seq_lost +332,故与 CAN 同级、无互抢 |
| 3 | SysTick(1 ms 节拍) | <1 µs | 推进 ms 计数器 + DWT 回绕跟踪;周期 flush/超时在超环 poll(非中断) |
| 4 | EMAC 其余(safety/common) | — | 异常处理 |

抢占与最坏排队分析:

- EMAC RX ISR 与 CAN ISR **同为 prio 1,互不抢占**(A/B 验证结论:EMAC 降到 prio 2 反而让 EMAC RX ISR 被 CAN 突发抢占、EQOS RX 环溢出,downlink seq_lost +332,且 face starv +105)。Linux→CAN 大聚合帧解析(最长 ~30 µs)与 CAN 接收按到达排队、不互相打断。
- **prio 1 组内 12 个中断源(11 CAN + 1 EMAC)同级互不抢占**,最坏排队延迟 = 其余 ISR WCET 之和(含 EMAC ~30 µs)≈55 µs ≪ 最紧死线 164 µs(CAN3–5,8B 风暴),仍有裕量。NVIC 子优先级在组内细分:RX MB > eFIFO DMA > TX-complete(RX 死线硬,TXC 仅影响流控释放时延)。
- 所有 ISR 内禁止任何忙等。

### 3.3 CAN→Eth 方向(RX 上报)

**CAN0(v1:MB + 中断;eFIFO + eDMA 为 Phase-2,已 parked):**
- **v1 现状**:CAN0 的 Enhanced RX FIFO 在本硅片上使能后 RX 中断从未触发(flag 疑似路由到未服务的 MB32–63 线),故 v1 将 eFIFO **parked**,CAN0 改用与 CAN1–5 相同的 **MB 组 + 中断** 路径(8 深 RX MB,`E2CF_RX_MB_CAN0=8`),由统一的 `chan_drain_mb_bank()` 处理(`can_hw.c`)。
- **Phase-2 目标**(待修复 eFIFO 中断线路由后启用):Enhanced RX FIFO 水位 = 4,eDMA 每 minor loop 搬 1 个 eFIFO 元素到 DTCM 环形缓冲,major loop 4 帧触发 ISR 批量编码,近零 per-frame CPU。
- 溢出保护:RX MB overrun → `rx_ovf_cnt++` → EVT err_flags[5]。

**CAN1–5(MB 中断 —— FD 模式下的硬件唯一解,见 §1.3):**
- **差异化 RX MB 分配**(按各实例 64B-MB 容量):CAN1/2 = 8×RX + 1×TX + 备用;CAN3–5 = 4×RX + 1×TX + 备用。RX MB 组掩码全通(精细过滤由 CFG SET_FILTER 写 MB 个体掩码)。
- **多 RX MB 的顺序与覆写问题(必须处理)**:同滤波的多个 RX MB,硬件按 MB 扫描序填充"第一个空闲匹配 MB",并不保证编号序 = 到达序。处理规则:
  - ISR 一次扫描全组 pending MB,**按 FlexCAN 16-bit MB 自由计数时戳排序后再编码上送**(组内最多 8 帧,插入排序即可,~50 周期;该 16-bit 时戳仅用于排序,不写入记录 tag,见 §3.6);
  - 读 MB 期间启用 **MB 锁定协议**(读 C/S 字锁定 → 读 ID/DATA/时戳 → 读 free-running timer 解锁),防止读取中途被新帧覆写造成撕裂;
  - 全组占满后再来帧 = 覆写丢帧,硬件置 overrun/CODE 异常 → `rx_ovf_cnt++` → EVT err_flags[5](死线裕量分析见 §3.2,正常负载不会发生)。
- RX ISR 成本:读 1 个 64B MB(18 字)+ 清 flag + 编码进 `agg_buf` + 时戳换算 ≈250–400 周期/帧;5 路合计满载 ≈51 k IRQ/s,占用 ~15–20% CPU(预算内);ISR 代码与缓冲全部放 TCM(§4)。
- 注:FlexCAN **无 TX DMA、无 MB DMA**,发送方向所有实例(含 CAN0)一律 CPU 写 MB,见 §3.4。

**聚合 flush(协议 §6.1,MCU→Linux T_agg=50 µs 强制):**
- `agg_buf` 乒乓双缓冲(各 1536B,DTCM)。追加记录时:**仅**在 ①将满(达 17 条 / 超 face)时立即 seal+发送;否则首条记录时刻 +50 µs 的 T_agg 死线由**超环 poll**(`e2cf_core_poll`)检查到期 flush。
  > 注:早期"②EQOS TX ring 空(egress 空闲)即发"规则已**删除** —— 100M 下 TX 环 ~7 µs 即排空,几乎每帧都"空闲",反而瓦解批处理、饿死 face 池;改回纯 T_agg 死线后聚合恢复(`e2cf_core.c` agg_commit 注释)。
- seal:填 Eth/VLAN/E2CF 头(seq++、`ts_base`=网关 DWT 钟低 32 位 ns)、缓冲翻面、写 TX 描述符 + tail pointer doorbell。EQOS TX 为零拷贝(描述符直接指向 agg_buf 翻面后的那一面;双缓冲保证 DMA 读取期间另一面可写)。
- TXC 与 EVT 共用同一 flush 节拍(独立小缓冲,可与 DATA 不同帧)。

### 3.4 Eth→CAN 方向(发送请求)

EMAC RX ISR(零拷贝:`ENET_QOS_GetRxFrame` 缓冲指针直接解析):

1. 校验:EtherType 0x88B5、版本、帧长一致性;SA 学习/校验 peer MAC。
2. DATA 记录逐条:`chan` 校验 → 压入 `sw_txfifo[chan]`(16 深,元素 = 记录头 8B+payload 副本 ≤72B;一次 memcpy)→ 若该通道当前无 MB 在发,立即装填 TX MB。
3. **TX 严格保序**:每通道**单活跃 TX MB**(FlexCAN 多 MB 发送按 ID 优先级仲裁会乱序,违反协议 §6.3)。TX-complete ISR 中:生成 TXC 记录(tag 回显、status、网关 DWT 时间戳)→ 从 sw_txfifo 取下一帧装 MB。单 MB 的帧间隙 = ISR 延迟 ~2 µs ≪ 帧时间 98 µs,吞吐损失 <2.5%,换取绝对保序 —— 8B 小帧时损失 ~5%,可接受(协议层窗口=16=sw_txfifo 深度,Linux 侧不会压更多)。
4. one-shot 模式(CFG mode_flags[4]):**v1 不支持** —— `can_hw_set_bitrate` 遇 `E2CF_MODE_ONESHOT` 直接回 CFG status=3(ENOTSUP);故 v1 不产生"仲裁丢失放弃 / TXC status=1",`arb_lost_cnt` 恒 0。(原 MB 配 RTREN 的方案为 Phase-2。)
5. CFG_REQ:在 ISR 内直接执行(全部操作 <10 µs:写 CBT/EDCBT 需冻结模式 —— STOP 状态才允许 SET_BITRATE,协议已保证)→ 回 CFG_RSP(独立小帧直发,不聚合)。token 去重表(每通道末次 token)保证幂等。

### 3.5 链路监督与安全态

- 超环 poll(`e2cf_core_poll`,SysTick 1ms 提供 ms 节拍)维护 HB 收发:100 ms 发 HB;500 ms 未收 Linux HB → **安全态**:6 通道全部停止装填新 TX MB(在发帧完成即止),RX 继续但丢弃(不上送),EVT 缓存。恢复后 Linux 重下发配置。
- 看门狗(SWT)由 main 循环喂,任何 ISR 死锁触发复位 → Linux 经 HB uptime 回绕检测到并重配。

### 3.6 时间戳方案

- **网关时钟(v1)= Cortex-M7 DWT 周期计数器 @160 MHz**(6.25 ns/tick),由 1 ms SysTick 跟踪回绕扩展为 64 位单调 ns 钟(`gw_time.c`)。TIME 消息 flags[0](1588 已同步)在 v1 **恒为 0**(`e2cf_core.c` "not 1588-synced in v1")。Phase-2:STM@80 MHz 与 EMAC 1588 上电对齐一次、周期校漂,后端替换同一 `gw_time` API。
- CAN RX:记录 `tag` 的 `ts_off` 取自**网关 DWT 钟在 RX ISR 交付时刻采样**,换算为相对 `ts_base` 的 2 µs 偏移。FlexCAN 的 16-bit 自由计数 MB 时戳**仅用于 bank 内到达排序**(§3.3),不写入 tag。(基于 HR timestamp 在 SOF/EOF 采样的方案为 Phase-2。)
- TXC:TX-complete ISR 取**网关 DWT 时刻**填 32-bit ns 偏移。

---

## 4. 内存布局(确定性优先)

| 区域 | 内容 | 理由 |
|---|---|---|
| ITCM 32 KB | 全部数据面 ISR + 编解码热函数 + 向量表 | 零等待、无 cache miss 抖动 |
| DTCM 64 KB | agg_buf×2、sw_txfifo[6][16](~7 KB)、txc/evt 缓冲、CAN0 eDMA 目标环、ISR 栈 | 同上;eDMA 可达 TCM 后门 |
| 系统 SRAM 320 KB | EQOS 描述符环+RX/TX 帧缓冲(**MPU 配为 non-cacheable** 或显式 clean/invalidate)、统计、main 栈 | EQOS DMA 一致性 |
| Flash 4 MB | 代码(热路径 `__attribute__((section(".itcm")))` 启动时搬运) | |

M7 cache 策略:开启 I/D cache;所有 DMA 共享区 non-cacheable MPU region(简单、确定),热路径数据反正都在 TCM,cache 维护开销为零。

---

## 5. CPU/总线负载汇总(满载最坏)

| 项 | 占用 |
|---|---|
| CAN1–5 RX ISR(51 k/s × ~350 周期) | ~11% |
| CAN0 RX ISR(v1 走 MB 路径,~10 k/s × ~350 周期;eFIFO+eDMA Phase-2 可降至 ~1%) | ~2% |
| CAN TX-complete ×6(61 k/s × ~250 周期) | ~10% |
| EMAC RX ISR(~9 k pkt/s × ~1200 周期,n=8) | ~7% |
| EMAC TX seal+doorbell(~9 k/s × ~400 周期) | ~2.5% |
| SysTick(1 kHz × ~40 周期) | <1% |
| **合计(双向 6 路 64B 满载)** | **~33%,余量 67%** |
| 8B 风暴单向接收(146 k/s × ~300 周期) | ~27%(仍可行) |

## 6. 延迟预算(MCU 内部贡献)

| 路径 | 典型 | 最坏 |
|---|---|---|
| EMAC RX 完成 → CAN MB 开始仲裁(Eth→CAN) | 4–8 µs | 15 µs(被 CAN ISR 抢占) |
| CAN RX EOF → E2CF 帧 doorbell(CAN→Eth) | 3–10 µs + T_agg 0–50 µs | 60 µs |

与协议 §8 端到端预算一致。

## 7. 工程结构与构建

```
e2cf_gw_mcxe31b/
├── board/(FRDM-MCXE31B 引脚/时钟,扩展板变体)
├── source/
│   ├── main.c            超循环 + SysTick 节拍
│   ├── e2cf_proto.h      协议结构体(与 Linux 侧字节级同一头文件,小端直映)
│   ├── e2cf_config.h     编译期配置(中断优先级、环深、face 数、autostart 等)
│   ├── e2cf_core.c/.h    协议引擎:聚合/窗口/CFG 幂等/HB/安全态/STATS/EVT/TXC
│   ├── can_hw.c/.h       FlexCAN 6 实例:MB 组 RX(CAN0 eFIFO parked)、保序单活跃 TX MB
│   ├── eth_raw.c/.h      EQOS ring0 零拷贝收发 / seal / demux
│   ├── gw_time.c/.h      网关时钟(DWT@160MHz 64 位单调钟;Phase-2:STM/1588)
│   ├── gw_prof.c/.h      性能计量(tx_gap 等;E2CF_PROFILE)
│   └── dbg_log.c/.h      延迟日志环(ISR 安全)
├── drivers/(SDK: fsl_flexcan, fsl_enet_qos —— 无 fsl_edma / fsl_stm)
├── Makefile             armclang(AC6)构建 → build/e2cf_mcxe31.{axf,bin,map}
└── e2cf_mcxe31.uvprojx  Keil MDK 工程(同 armclang;散列 MCXE31B/mdk/MCXE31B_flash.scf)
```

基线参考示例(SDK,frdmmcxe31b):`flexcan/efifo_edma_transfer`(80 MHz PE 切换 + eFIFO+eDMA)、`enet_qos/txrx_multiring_transfer`(裸帧双环)、`enet_qos/txrx_ptp1588_transfer`(1588)。

## 8. 硬件(扩展板)要求

- 6× TJA1463(SIC)+ 120Ω 端接选项;CAN0 走板上负载最重总线。
- RMII PHY 同 FRDM(LAN8741 级);测试点:每路 CANH/L、1×PPS 输出(延迟测量基准)、2× GPIO(ISR 时序示波)。
- 16 MHz 晶振(CAN 频率容差要求)。

## 9. 测试计划(与 Linux 侧 T1–T9 联调外的 MCU 单侧项)

| # | 测试 | 标准 |
|---|---|---|
| M1 | 6 路 8 Mbps 物理层 | TJA1463 + 1 m 线束,眼图/采样点扫描(CANoe/示波器),误码 0 |
| M2 | 保序(TX) | 单通道窗口 16 连发,总线监听帧序 = 注入序,10⁷ 帧无乱序 |
| M2b | 保序(RX,多 MB) | 对 CAN1–5 以最小帧间隔灌 8B 帧风暴,验证 HR 时戳排序后上送序 = 总线序、MB 锁定无撕裂(payload 含递增序号校验),10⁷ 帧无乱序/无错帧 |
| M3 | ISR 时序 | GPIO 翻转测各 ISR 进出,验证 §3.2 预算;最高优先级 ISR 抖动 <1 µs |
| M4 | 满载烤机 | 双向 6 路满载 24 h,无 eFIFO/MB 溢出、无看门狗复位 |
| M5 | 安全态 | 拔网线 500 ms 内停发;恢复后接受重配 |
| M6 | 时间戳精度 | PPS 对比,ts_off 误差 <±2 µs |

## 10. 风险与未决项

| 风险 | 缓解 |
|---|---|
| MCXE31B 为 2025 新品,数据手册仍标 Objective | 设计同时兼容 S32K344/358(IP 同源,FlexCAN/EQOS 驱动 API 一致),作为 fallback 型号 |
| ~~传统 RX FIFO 不支持 FD 为推断~~ **已确认(2026-06-11 设计评审)**:6 实例全部带 DMA 请求线,但 Enhanced RX FIFO 仅 CAN0;CAN1–5 的 DMA 仅能配合传统 RX FIFO,而传统 FIFO 不支持 FD → FD 模式下 CAN1–5 的 DMA 不可用 | 设计已按此落定(§1.3 规则表 + §3.3 MB 方案)。**v1 现状**:CAN0 的 eFIFO 在本硅片上中断未触发,已 parked,CAN0 同走 MB 路径;eFIFO+eDMA 留作 Phase-2。经典 CAN 专用通道保留 FIFO+DMA 降级优化选项(§1.3 规则 3) |
| CAN1–5 多 RX MB 到达序≠编号序、读取覆写撕裂 | §3.3 强制 HR 时戳组内排序 + MB 锁定协议;测试 M2 扩展覆盖多 MB 乱序场景 |
| 单 TX MB 保序方案在 8B 风暴下 ~5% 吞吐损失 | 可选优化:2 MB 乒乓 + 同 ID 优先级位强制(需 RM 复核仲裁细节),v1 不做 |
| FRDM 板载 CAN PHY 8 Mbps 能力未知 | 一律以扩展板 TJA1463 为准 |
| 100 Mbps 链路 + 极端聚合失效场景 | 协议已强制 MCU→Linux T_agg=50 µs;EVT/统计暴露聚合直方图监控 |
