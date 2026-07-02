# MCXE31B 固件设计

## 1. 固件结构

固件为裸机结构，不使用 RTOS 和 lwIP。数据面由 CAN 和 EMAC 中断处理；主循环只执行非硬实时维护：

- E2CF 聚合超时 flush。
- HB、EVT、TIME、STATS 周期发送。
- CAN error polling 和 stuck-TX watchdog。
- PHY link polling。
- 状态 LED 刷新。
- 延迟日志 UART drain 和调试命令。

启动顺序：

```text
BOARD_InitHardware
status_led_init
gw_time_init
dbg_log_init
SysTick_Config
eth_raw_init
can_hw_init
e2cf_core_init
main superloop
```

## 2. 默认配置

交付默认配置：

| 项 | 值 |
|---|---|
| CAN nominal bitrate | 1 Mbit/s |
| CAN FD data bitrate | 5 Mbit/s |
| CAN FD | enabled |
| BRS | enabled |
| active channels | 0..5 |
| TX window / sw FIFO | 16 per channel |
| MCU->Linux aggregation | 50 us |

FlexCAN PE clock 固定使用 AIPS_PLAT_CLK 80 MHz。8 Mbit/s data phase 是设备能力上限，可作为显式 override；交付默认不使用 8 Mbit/s。

## 3. 中断优先级

| 中断 | NVIC priority | 说明 |
|---|---:|---|
| FlexCAN0..5 MB IRQ | 1 | CAN RX 和 TX complete |
| EMAC RX/TX IRQ | 1 | E2CF raw L2 收发 |
| SysTick | 3 | 1 ms tick 和 DWT wrap 跟踪 |

CAN 和 EMAC 同优先级，互不抢占。该设计让 CAN RX、TXC 流控释放和 EMAC RX 在同一数据面优先级内排队，避免单侧长期压制另一侧。

## 4. 状态 LED

三颗状态灯为 active-low GPIO，由 `status_led_poll()` 在主循环中刷新，内部 20 ms 节流，不进入数据面中断路径。

| LED | GPIO | 状态语义 |
|---|---|---|
| SYS | PTC16 | 约 1 Hz 闪烁表示主循环运行；fault/fatal path 常亮 |
| NET | PTB22 | 熄灭=PHY down；闪烁=PHY up 但 E2CF peer 未 ready；常亮=E2CF ready |
| CAN | PTC14 | 熄灭=无 CAN 流量；闪烁=CAN RX/TX 活动；常亮=任一 active/running CAN error-passive 或 bus-off |

## 5. FlexCAN 资源分配

所有 6 个 FlexCAN 实例使用统一的 MB RX bank + 单 active TX MB 结构。

| 通道 | RX MB 数 | TX MB | 说明 |
|---:|---:|---:|---|
| CAN0 | 8 | 1 | 与 CAN1/2 使用同等 RX 深度 |
| CAN1 | 8 | 1 | FD MB 接收 |
| CAN2 | 8 | 1 | FD MB 接收 |
| CAN3 | 4 | 1 | FD MB 接收 |
| CAN4 | 4 | 1 | FD MB 接收 |
| CAN5 | 4 | 1 | FD MB 接收 |

CAN0 当前同样使用 MB RX bank，不启用 eFIFO/eDMA。CAN1..5 在 CAN FD 模式下没有可用于 FD RX 的 FIFO DMA 路径。统一 MB 路径使六路行为一致，便于时序和丢帧判据验证。

## 6. CAN RX 路径

FlexCAN MB IRQ 进入统一 `can_irq()`：

1. 读取 pending MB flags。
2. RX 优先，扫描整个 RX MB bank。
3. 对每个 pending MB 先 W1C 清 flag，再读 MB。
4. 使用 FlexCAN 16-bit timestamp 对同一 bank 内帧排序。
5. 转换成 E2CF DATA 记录，交给 `e2cf_core_can_rx()` 聚合。
6. 再处理 TX complete。

RX MB bank 使用 accept-all 配置。精细过滤由 Linux/SocketCAN 层承担；固件只负责无损搬运和状态上报。

## 7. CAN TX 路径

Linux->MCU DATA 在 EMAC IRQ 中解析后进入对应通道软件 FIFO。每个通道只允许一个 active TX MB：

1. `can_hw_submit_tx()` 校验通道、FD/BRS/RTR、长度和运行状态。
2. 请求进入 16 深软件 FIFO。
3. 通道空闲时 `chan_tx_kick()` 装填 TX MB。
4. TX complete IRQ 生成 TXC，释放当前帧。
5. 继续装填 FIFO 中下一帧。

单 active TX MB 保证同一通道严格按 Linux DATA 到达顺序上总线。多 MB 并行发送可能被 CAN ID 仲裁顺序影响，不用于当前设计。

## 8. EMAC raw L2 路径

固件直接使用 ENET_QOS raw L2，不运行 IP 栈。RX descriptor 和 TX descriptor 由 SDK ENET_QOS 驱动管理，E2CF frame 在中断上下文直接解析或提交。

当前环深：

| 资源 | 数量 | 说明 |
|---|---:|---|
| EQOS RX BD | 16 | 吸收下行 burst |
| EQOS RX buffers | 32 | 每个 buffer 可容纳完整以太帧 |
| EQOS TX BD | 64 | 降低 wrap 边界压力 |
| TX faces | 8 | E2CF sealed frame staging |

TX descriptor 环保留一个空槽，不允许完全填满。TX tail pointer 始终保持在 ring 内地址。

## 9. E2CF core

`e2cf_core` 负责：

- DATA/TXC 聚合和发送。
- CFG_REQ 执行和幂等响应。
- HB peer lock、安全态和恢复。
- TIME/STATS/EVT 发送。
- 以太帧 `seq_lost` / `seq_reorder` 统计。

MCU->Linux DATA/TXC 聚合只由两个条件触发：记录达到上限，或 50 us `T_agg` 到期。聚合不会因为 EQOS TX ring 暂时空闲而提前 flush。

## 10. 时间戳

网关时间为 DWT cycle counter 扩展出的 64 位单调纳秒时钟。SysTick 每 1 ms 跟踪 DWT wrap。

时间戳用途：

- CAN RX：进入 RX ISR 并读取 MB 后采样网关时间。
- TXC：TX complete IRQ 中采样网关时间。
- DATA trailer：E2CF DATA 以太帧提交给 EQOS TX 时采样。
- TIME：1 Hz 发送完整网关时间给 Linux 做时间映射。

FlexCAN 16-bit MB timestamp 只用于同一 RX bank 内排序，不作为对外协议时间戳。

## 11. 安全态和错误处理

MCU 500 ms 未收到 Linux HB 后进入安全态：

- 停止装填新的 CAN TX MB。
- CAN RX 继续被硬件接收，但不上送 DATA。
- HB 状态变为 FAULT。
- 收到 Linux HB 后清除安全态，等待 Linux 重新配置通道。

CAN bus-off、RX overrun、TX queue overflow、stuck TX watchdog 等通过 EVT、TXC status 和 STATS 上报。Linux 驱动按 SocketCAN 语义恢复通道。

## 12. 验收项

- 上电后 6 路 FlexCAN PE clock 为 80 MHz。
- 默认 autostart 配置为 1M/5M CAN FD BRS。
- `eth2can0..5` 能通过 CFG STOP/SET_BITRATE/START 配置。
- 默认三组线束 `0<->4`、`1<->2`、`3<->5` 能完成 `canperf latency --count 10000`。
- `canperf bandwidth` final confirmation 输出 `RESULT bandwidth: PASS`。
- debugfs 可读时，最终 `EVIDENCE` 为 `counters=clean`。
