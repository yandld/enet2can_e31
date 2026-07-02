# EQOS TX 环设计

## 1. 设计目标

EQOS TX 路径用于发送 MCU->Linux 的 E2CF DATA、TXC、EVT、TIME、HB 和 STATS 帧。设计目标是：

- TX descriptor 操作短且可在中断上下文调用。
- DMA 读取 payload 时 buffer 内容保持稳定。
- TX ring 不进入满环歧义状态。
- 所有发送失败都能通过计数器暴露。

## 2. 资源配置

当前配置：

| 资源 | 宏 | 数量 | 内存区域 |
|---|---|---:|---|
| TX BD | `E2CF_ETH_TXBD_NUM` | 64 | non-cacheable |
| RX BD | `E2CF_ETH_RXBD_NUM` | 16 | non-cacheable |
| TX faces | `E2CF_ETH_TXFACE_NUM` | 8 | non-cacheable frame staging |

每个 TX face 为一个完整以太帧 buffer。E2CF core 在 face 中完成封包，然后把该 face 交给 EQOS TX descriptor。

## 3. TX face 生命周期

TX face 状态：

```text
free -> open aggregation -> sealed/inflight -> reclaimed -> free
```

规则：

- 聚合中的 face 只由 E2CF core 写入。
- sealed face 交给 EQOS TX 后，直到 descriptor 完成前不得复用。
- `face_reclaim()` 根据 `eth_raw_tx_completed()` 回收 DMA 已完成的 face。
- DATA 和 TXC 使用独立聚合上下文，立即消息使用临时 face。

TX face 数量为 8，可覆盖两个聚合上下文、周期消息和短时 TX ring inflight。

## 4. Descriptor 提交规则

`eth_raw_send()` 在进入 ENET_QOS 发送 API 前短暂关中断，序列化 descriptor 和 tail pointer 访问。

提交前检查：

```c
if ((submitted - reclaimed) >= (TXBD_NUM - 1))
    return kStatus_ENET_QOS_TxFrameBusy;
```

该规则永久保留一个 descriptor 空槽，避免满环状态与空环状态混淆。发送成功后递增 `submitted`；TX complete 事件递增 `reclaimed`。

## 5. Tail pointer 约束

TX tail pointer 必须始终指向 ring 内地址。descriptor index wrap 时，tail pointer 回到 ring base，而不是指向 ring 末尾之后的地址。

固件侧配合规则：

- 不完全填满 TX ring。
- 每次提交只使用一个完整 frame buffer。
- descriptor OWN 位由 ENET_QOS 驱动设置。
- payload buffer 在 face 回收前保持不变。

## 6. 失败处理和统计

`eth_raw_send()` 返回 busy 时：

- `tx_busy` 计数增加。
- 当前 face 保持 sealed 状态，等待后续 reclaim 后重试或由上层统计发送失败。

E2CF core 暴露的关键统计：

| 计数 | 含义 |
|---|---|
| `frames_sent` | 成功提交给 EQOS TX 的 E2CF 帧 |
| `send_fail` | E2CF 发送失败 |
| `face_starved` | 无可用 TX face |
| `emac_tx_busy` | EQOS TX ring 暂时无可用 descriptor |
| `seq_lost` | 接收端确认的以太帧丢失 |

客户验收时，`canperf` final confirmation 的 `EVIDENCE ... counters=clean` 表示本次测试中相关丢包、溢出、拒绝和发送失败计数未增长。

## 7. 验收项

- `E2CF_ETH_TXBD_NUM` 为 64。
- `E2CF_ETH_TXFACE_NUM` 为 8。
- TX ring 提交逻辑保留至少一个空 descriptor。
- TX tail pointer wrap 后仍指向 ring 内。
- 长时间 `canperf bandwidth` final confirmation 不增加 `seq_lost`、`face_starved`、`send_fail`、`emac_tx_busy`。
