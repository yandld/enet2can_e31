# Linux 驱动设计

## 1. 模块和接口

Linux 驱动模块名为 `eth2can`，下层绑定一个以太网接口，默认 `ifname=eth0`。驱动创建 6 个 SocketCAN 设备：`eth2can0` 到 `eth2can5`。每个设备对应 E2CF 通道 0..5。

模块参数：

| 参数 | 默认值 | 说明 |
|---|---|---|
| `ifname` | `eth0` | 承载 E2CF raw L2 帧的以太网接口 |
| `vid` | `-1` | `-1` 表示 bring-up 使用 untagged；部署可设为 `100` |
| `peer` | 空 | 为空时从 MCU HB 自动学习 peer MAC |

客户交付默认 CAN FD 配置为 `bitrate 1000000 dbitrate 5000000 fd on`。

## 2. 数据结构

驱动使用一个全局 `struct e2cf_dev` 管理底层以太网、peer MAC、HB/TIME/STATS 状态和全局序号。每个 CAN 通道使用一个 `struct e2cf_chan`，保存 candev、echo slot、CFG 状态、TXC 统计和 MCU 通道状态缓存。

每通道 echo 窗口深度为 16，与协议 `E2CF_WIN_DEPTH` 和 MCU `sw_txfifo` 深度一致。窗口 slot 号写入 DATA 记录 `tag`，MCU 在 TXC 中原样回显。

## 3. TX 路径：Linux -> MCU -> CAN

`ndo_start_xmit` 对 SocketCAN skb 做合法性检查后，分配一个空闲 echo slot：

1. `can_dropped_invalid_skb()` 过滤非法 CAN skb。
2. `find_first_zero_bit(echo_busy)` 获取 echo slot。
3. 窗口满时 `netif_stop_queue()` 并返回 `NETDEV_TX_BUSY`。
4. 构造 E2CF DATA 帧，写入 `chan`、`tag`、`can_id`、`len`、`flags` 和 payload。
5. 调用 `dev_queue_xmit()` 发送到底层以太网接口。
6. 调用 `can_put_echo_skb()` 保存 echo skb，等待 TXC 完成语义。

当前驱动每个 CAN skb 生成一个 E2CF DATA 以太帧。MCU 侧按每通道 FIFO 顺序装填单 active TX MB，CAN 发送完成后返回 TXC。

## 4. RX 路径：MCU -> Linux

驱动通过 `dev_add_pack()` 注册 EtherType `0x88B5` 的 `packet_type` 回调，在 softirq 上下文解析 E2CF 帧。

处理规则：

- DATA：逐条构造 `can_skb` 或 `canfd_skb`，注入对应 `eth2canN`。
- TXC：按 `chan/tag` 查找 echo slot，成功时 `can_get_echo_skb()`，错误时释放 echo skb 并统计 TX error。
- EVT：更新 CAN error state，并在需要时注入 CAN error frame。
- CFG_RSP：唤醒等待中的配置事务。
- TIME：更新网关时间映射，用于硬件时间戳还原。
- HB：刷新 peer 存活状态，检测 MCU 重启并触发恢复。
- STATS：缓存 MCU 统计，供 `ethtool -S` 和 debugfs 查询。

## 5. 配置和恢复

`ndo_open` 流程：

1. `open_candev()`。
2. 发送 SET_BITRATE，包含 Linux CAN bit timing 和 mode flags。
3. 发送 START。
4. `netif_start_queue()`。

`ndo_stop` 流程：

1. 发送 STOP。
2. 清理 echo slot。
3. `close_candev()`。

当 HB 超时或 MCU uptime 回退时，驱动进入恢复流程：停止队列，清理 stale echo slot，重新执行打开通道时的 CFG 序列，然后恢复队列。TXC 超时兜底为 1 s，用于释放因以太帧丢失或异常路径遗留的 echo slot。

## 6. 统计接口

每个 `eth2canN` 支持 `ethtool -S`。统计名前缀：

- `drv_`：驱动本通道计数。
- `drvg_`：驱动全局计数。
- `gw_`：MCU 本通道 STATS/EVT 缓存。
- `gwg_`：MCU 全局 STATS 缓存。

debugfs 路径：

```text
/sys/kernel/debug/eth2can/stats
/sys/kernel/debug/eth2can/clear_stats
```

`clear_stats` 可写 `all` 或 `0..5`。该操作只清计数器，不改变通道配置或运行状态。

## 7. 测试和客户输出

客户测试入口为：

```sh
cd linux/can_testcase
make
./canperf latency --count 10000
./canperf bandwidth
```

`latency` 输出端到端 p50/p99/p99.9 和分段延迟。`bandwidth` 先搜索候选速率，再执行 final confirmation。客户只看最终 `RESULT` 和 `EVIDENCE` 行：

```text
RESULT latency: PASS, zero_loss=yes, ...
RESULT bandwidth: PASS, zero_loss=yes, MSR=... fps/pair, ...
EVIDENCE ... counters=clean
```

`counters=clean` 表示驱动和网关关键丢包、溢出、拒绝计数器在本次确认轮未增长。

## 8. 验收要点

- 6 个 `eth2canN` 设备可创建并可配置为 CAN FD 1M/5M。
- 默认线束 `0<->4`、`1<->2`、`3<->5` 下，`canperf latency` 输出 `RESULT latency: PASS`。
- `canperf bandwidth` 输出 `RESULT bandwidth: PASS` 和 `MSR = ... fps/pair DELIVERED`。
- debugfs 可读时，最终 `EVIDENCE` 行应为 `counters=clean`。
- 出现 `counters=dirty` 时，以 `seq_lost`、`rx_ovf`、`rej`、`starv`、`sfail`、`emac_rxdrop` 的增量定位问题。
