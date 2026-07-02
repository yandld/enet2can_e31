# E2CF 协议规范

## 1. 基本约定

E2CF 是 MCXE31B 网关与 Linux `eth2can` 驱动之间的二层以太网协议。Linux 对外暴露 6 个 SocketCAN 设备：`eth2can0` 到 `eth2can5`。交付默认速率为 CAN FD `bitrate 1000000` / `dbitrate 5000000`，BRS 打开。

协议使用 EtherType `0x88B5`。部署默认使用 802.1Q VLAN，VID 为 `100`；bring-up 阶段允许不带 VLAN 的帧。DATA、TXC、EVT、TIME、HB 使用 PCP 6；CFG_REQ、CFG_RSP、STATS 使用 PCP 2。

所有多字节字段为 little-endian。公共头和记录结构按 4 字节对齐，DATA payload 补齐到 4 字节边界。通道号 `chan` 为 0..5，对应 Linux 设备 `eth2canN` 和 MCU FlexCAN 实例 N。

## 2. 以太帧格式

带 VLAN 的帧格式：

```text
DA(6) | SA(6) | TPID=0x8100(2) | TCI(2) | EtherType=0x88B5(2) | E2CF payload
```

公共头固定 8 字节：

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| 0 | `ver_type` | u8 | 高 4 bit 为版本，当前为 1；低 4 bit 为消息类型 |
| 1 | `count` | u8 | 本帧记录数，非记录类消息固定为 1 |
| 2 | `seq` | u16 | 每端发送方向独立递增，用于丢包/乱序统计 |
| 4 | `ts_base` | u32 | 发送端网关时钟低 32 位纳秒基准 |

消息类型：

| 值 | 类型 | 方向 | 用途 |
|---:|---|---|---|
| 0 | DATA | 双向 | CAN 帧数据或发送请求 |
| 1 | TXC | MCU -> Linux | CAN 发送完成确认 |
| 2 | EVT | MCU -> Linux | CAN 状态和错误事件 |
| 3 | CFG_REQ | Linux -> MCU | 配置请求 |
| 4 | CFG_RSP | MCU -> Linux | 配置响应 |
| 5 | TIME | MCU -> Linux | 网关完整时间 |
| 6 | HB | 双向 | 心跳和 peer 发现 |
| 7 | STATS | MCU -> Linux | 网关统计推送 |

## 3. DATA

DATA 记录头固定 8 字节，后接 `ceil4(len)` 字节 payload：

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| 0 | `can_id` | u32 | SocketCAN `can_id` 语义，含 EFF/RTR 标志 |
| 4 | `len` | u8 | 实际 payload 长度，0..64 |
| 5 | `flags` | u8 | bit0=BRS，bit1=ESI，bit2=FDF |
| 6 | `chan` | u8 | 通道号 0..5 |
| 7 | `tag` | u8 | Linux->MCU 为 echo slot；MCU->Linux 为 2 us 单位的 `ts_off` |

约束：

- FDF=0 时 `len <= 8` 且 BRS=0。
- FDF=1 时不允许 RTR。
- BRS=1 只允许用于数据相位速率高于仲裁相位速率的通道。
- Linux->MCU 的 ESI 必须为 0；MCU 接收时忽略该位。
- 非法记录由 MCU 拒绝，并返回 TXC `CTRL_ERROR`，不得静默截断或改写。

MCU->Linux 的 DATA 帧在最后一条记录后追加 4 字节 trailer：网关时钟低 32 位纳秒，表示该以太帧提交给 EQOS TX 的时间。该 trailer 不计入 `count`。

单个 DATA 以太帧最多包含 17 条 64 字节 CAN FD 记录。

## 4. TXC

TXC 是 Linux 侧发送窗口的唯一释放机制。MCU 只有在对应 CAN 帧真正完成发送后才返回 TXC。

TXC 记录 12 字节：

| 偏移 | 字段 | 类型 | 说明 |
|---:|---|---|---|
| 0 | `chan` | u8 | 通道号 |
| 1 | `tag` | u8 | 对应 DATA 请求的 echo slot |
| 2 | `status` | u8 | 发送结果 |
| 3 | `rsv` | u8 | 0 |
| 4 | `ts_off` | u32 | TX complete 相对本 TXC 帧 `ts_base` 的纳秒偏移 |
| 8 | `req_eth_rx_ns` | u32 | 请求帧到达 MCU EMAC 的网关时钟低 32 位纳秒 |

`status` 定义：

| 值 | 名称 | 说明 |
|---:|---|---|
| 0 | OK | 已完成 CAN 发送 |
| 1 | ARB_LOST | one-shot 仲裁失败放弃；当前固件不启用 one-shot |
| 2 | CTRL_ERROR | 控制器错误或非法帧 |
| 3 | CHAN_STOPPED | 通道停止或安全态禁止发送 |
| 4 | QUEUE_OVF | MCU 通道软件队列溢出 |

单个 TXC 以太帧最多包含 32 条记录。

## 5. CFG

CFG_REQ 和 CFG_RSP body 固定 24 字节。`token` 由请求方生成，响应必须回显。MCU 对同一通道的重复 token 保持幂等，重复请求返回缓存响应。

操作：

| op | 名称 | 说明 |
|---:|---|---|
| 1 | SET_BITRATE | 设置仲裁/数据相位 bit timing 和 mode flags |
| 2 | SET_MODE | 仅更新 mode flags |
| 3 | SET_FILTER | 设置过滤器；当前驱动默认接收全部 ID |
| 4 | START | 启动通道 |
| 5 | STOP | 停止通道 |
| 6 | RESET | 复位通道或全局复位 |
| 7 | GET_STATUS | 读取通道状态 |
| 8 | GET_INFO | 读取固件/协议/窗口/时钟能力 |
| 9 | CLEAR_STATS | 清零统计计数器，不改变通道状态 |

`mode_flags`：

| bit | 名称 | 当前行为 |
|---:|---|---|
| 0 | FD | CAN FD 使能 |
| 1 | NONISO | 不支持，返回 ENOTSUP |
| 2 | LISTENONLY | 支持 |
| 3 | LOOPBACK | 支持，主要用于内部 bring-up |
| 4 | ONESHOT | 不支持，返回 ENOTSUP |

CFG 请求 10 ms 未收到匹配响应时重试，最多 3 次。Linux 驱动必须保证 STOP 后再修改 bit timing。

## 6. 心跳、安全态和时间

HB 周期为 100 ms。MCU 在收到 Linux HB 后锁定 peer MAC；500 ms 未收到 peer HB 时进入安全态。安全态下 MCU 停止装填新的 CAN TX MB，CAN RX 不再上送 DATA，等待 Linux 恢复配置。

TIME 消息 1 Hz 发送，body 16 字节：

```text
full_time_ns:u64 | flags:u32 | rsv:u32
```

`flags bit0` 表示网关时间是否与 1588 同步。当前固件使用 DWT 扩展出的 64 位单调时钟，默认不声明 1588 同步。

## 7. STATS 和丢包判据

STATS 由 MCU 1 Hz 推送，包含全局计数和 6 个通道计数。接收端必须按 wire 中的 block length 解析，新增字段只追加在 block 尾部。

关键判据：

- `seq_lost` 为确认丢失帧数。
- `seq_gaps` 只是非连续序号事件数，乱序也会增加，不能单独作为丢包结论。
- `rx_ovf` 表示 CAN RX MB 溢出。
- `data_rx_rejects` 和 TXC 非 OK 状态用于定位下行发送失败原因。

客户验收以 `canperf` 最终 `RESULT` / `EVIDENCE` 行为准：`zero_loss=yes` 且 `counters=clean` 表示本次测试未发现应用层丢帧或驱动/网关关键计数器增长。

## 8. 聚合、排序和流控

MCU->Linux DATA/TXC 聚合使用 50 us `T_agg`。当记录数达到上限或聚合窗口到期时发送。Linux->MCU 当前驱动逐帧提交 DATA，请求仍遵守相同 wire 格式。

每通道 TX 窗口深度为 16。Linux 达到窗口上限时停止该通道队列；收到 TXC 后释放 echo slot 并唤醒队列。MCU 软件 TX FIFO 深度同为 16。

同一通道内 MCU 必须按 DATA 到达顺序发送 CAN 帧，不按 CAN ID 重新排序。跨通道不定义顺序关系。
