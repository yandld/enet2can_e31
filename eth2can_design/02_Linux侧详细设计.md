# Linux 侧详细设计:基于 ENETC4 fast path 的 6 路 SocketCAN 隧道驱动

**版本:** v1.0-draft1
**日期:** 2026-06-11
**目标内核:** Real-Time Edge Linux(本仓库 `real-time-edge-linux`,6.18.20-rt,PREEMPT_RT)
**目标硬件:** i.MX95,ENETC4 端口之一(Phase-2 独占;**Phase-1 不独占**,见下)
**配套文档:** 《01_E2CF协议规范》《03_MCXE侧详细设计》

---

> ## ⚠️ 阶段说明(2026-06-17 校准)
>
> **本文描述的是 Phase-2 目标设计**(ENETC4 fast-path `_k` 变体 + SCHED_FIFO 收发线程 + TX 聚合器 + skb 池),**尚未实现**。当前**已交付并入库的 Phase-1 驱动**(`linux/src/eth2can.c`)实现完全相同的**协议逻辑**(窗口/TXC/EVT/CFG/HB/安全态),但**传输层不同**。下面各节凡属 Phase-2 机制处标 “(Phase-2)”。设备名是 **`eth2can0..5`**(不是 `can0..5`)。
>
> | 维度 | **Phase-1(已交付,本仓库代码)** | Phase-2(本文设计) |
> |---|---|---|
> | RX 截获 | `dev_add_pack(0x88B5)`,**不独占网口**,其余流量照走协议栈 | `enetc4_ecat_fast_recv_k` 独占 ENETC4 口 |
> | TX | `dev_queue_xmit()` 逐帧直发 | TX 聚合器 + `enetc4_ecat_fast_xmit_k` |
> | 线程 | 无(RX 在 softirq;TX 在调用者上下文) | RX/TX 各一条 SCHED_FIFO kthread |
> | TX 槽位 | `echo_busy` 位图 + `find_first_zero_bit`,窗口满 `netif_stop_queue` 并回 `NETDEV_TX_BUSY` | `tx_head` 模计数 + `inflight` 计数 |
> | skb | 每帧 `alloc_can(fd)_skb`(失败计 `rx_dropped`) | 预分配 skb 池 + GFP_ATOMIC 兜底 |
> | netdev flag | **仅 `IFF_ECHO`**,走标准 qdisc(并处理 `NET_XMIT_CN`) | + `IFF_NO_QUEUE` noqueue 直通 |
> | Linux→MCU 聚合 | **逐帧直发,无 T_agg 定时器**(协议默认) | hrtimer `HARD` T_agg 聚合 |
> | 统计 | `ethtool -S`(`drv_/drvg_/gw_/gwg_`,46 项)+ `debugfs eth2can/{stats,clear_stats}` | 见 §6(部分为 Phase-2 计划) |

---

## 1. 需求与架构决策

### 1.1 需求映射

| 需求 | 设计响应 |
|---|---|
| 利用 `enetc4_ecat_fast_xmit/recv` 收发以太网帧 | 在 `enetc_ecat` 模块内新增内核缓冲区变体 `_k`(同一 ring/BD 代码路径,~50 行补丁,见 §3),保持原用户态 fast socket 能力不受影响 |
| 6 个 SocketCAN 节点(eth2can0..eth2can5),客户用标准工具链 | 新内核模块 `eth2can.ko`:6×`alloc_candev` + `register_candev`,支持 `ip link set eth2canX type can bitrate ... dbitrate ... fd on`、candump/cansend/busmaster 零修改可用 |
| 最低延迟 | 全内核态数据面(无用户态 daemon);noqueue 直通 TX;专职 SCHED_FIFO 收发线程;批量注入;skb 预分配池;CPU 隔离部署 |
| 接近线速 | 自适应聚合(协议 §6.1);每通道窗口流控防 MCU 溢出;预算见协议 §8 |

### 1.2 为什么必须是内核驱动而不是用户态 daemon

现有 fast path 是给用户态 EtherCAT master 的 syscall 旁路(`net/socket.c:2243/2312` 把 `sendto/recvfrom` 短路到 `ndo_fast_xmit/recv`)。理论上可以写一个用户态 daemon(fast socket + 6 个 vcan 回灌),但:

- CTU/EWC 2024 实测:同硬件满载下内核态 CAN 网关最大延迟 ≈1 ms,用户态(即使 SCHED_RR 80)≈3 ms —— 每帧多 2 次用户/内核切换 + 2 次 socket 队列;
- vcan 回灌无法实现 echo/TX 完成语义、bittiming 配置透传、bus-off 状态机;
- 6 路 61 380 fps 的 syscall 风暴本身就是不可接受的开销。

故采用:**`ndo_start_xmit`/`netif_receive_skb` 级内核数据面,参照 gs_usb(多通道复用)+ es58x(TX 聚合)的成熟模型**。

### 1.3 总体架构

```
   用户态: candump/cansend/客户应用(AF_CAN raw socket,每通道独立)
 ──────────────────────────────────────────────────────────────────────
   eth2can0…eth2can5  6 × candev(Phase-1:仅 IFF_ECHO;Phase-2 再加 IFF_NO_QUEUE;echo_skb_max=WIN=16)
     │ ndo_start_xmit(调用者进程上下文)            ▲ 批量 netif_receive_skb
     ▼                                             │(local_bh_disable 包裹)
 ┌──────────────────── eth2can.ko(隧道核心)─────────────────────────────┐
 │ TX 聚合器(agg_lock spinlock):                                          │
 │   当前以太帧缓冲 [Eth+VLAN hdr][E2CF hdr][N×DATA 记录]                  │
 │   flush:①满 ②egress 空闲立即发 ③hrtimer(HARD) T_agg 超时              │
 │ TX flush kthread(SCHED_FIFO 52)→ enetc4_ecat_fast_xmit_k()            │
 │ RX kthread(SCHED_FIFO 53)→ enetc4_ecat_fast_recv_k() 轮询/事件         │
 │   demux:DATA→skb 注入 │ TXC→echo 完成+wake_queue │ EVT→状态机          │
 │         CFG_RSP→completion │ TIME/HB→时钟回归/链路监督                  │
 │ 配置通道:do_set_bittiming/do_set_mode → CFG_REQ + wait_for_completion  │
 └───────┬──────────────────────────────────────────▲──────────────────────┘
         ▼                                          │
 ┌──────────── enetc_ecat(打补丁:+_k 变体,EtherType 参数化)──────────────┐
 │ tx_ring[0]/rx_ring[0] 直接 BD 操作,无中断纯轮询,预分配预映射缓冲       │
 └──────────────────────────────────────────────────────────────────────────┘
```

---

## 2. 现有 fast path 的事实约束(源码结论)

实现者必须知道的现状(均已在源码中核实,引用相对 `real-time-edge-linux/`):

1. **函数签名**(`drivers/net/ethernet/freescale/enetc_ecat/enetc_ecat.c:1596,1615`):
   `int enetc4_ecat_fast_xmit(struct net_device *ndev, void __user *buff, size_t len)` / `int enetc4_ecat_fast_recv(struct net_device *ndev, void __user *buff, size_t len, struct sockaddr __user *addr, int *addr_len)`,均 `EXPORT_SYMBOL_GPL`。**参数是用户态指针**,内部 `copy_from_user`(`:1535`)/`copy_to_user`(`:1664`)—— 内核线程拿内核缓冲调用在 arm64 上必然失败,**不能直接用,必须加 `_k` 变体**。
2. **锁与上下文**:两函数共用 `mutex_trylock(&priv->fast_ndev_lock)`(`:1601/:1626`),竞争立即返回 -EBUSY、不睡眠等锁;但 mutex 本身要求**进程上下文**。接口未 `ip link up` 时锁被驱动持有,fast path 恒 -EBUSY(`enetc4_ecat_pf.c:1375`,`enetc_ecat.c:2902/2986`)。
3. **TX**:固定 `tx_ring[0]`,2048 BD;每 BD 槽位预分配+预 DMA 映射 2048B 缓冲,发送 = memcpy + `dma_sync_single_for_device` + 写 TBPIR doorbell,**不等完成、无 ring 满检查、无 TX 完成回收**;单 BD 单帧,最大帧长受 `ENETC4_MAC_MAXFRM_SIZE=2000` 限制。⚠️ 原版 `len` 无上界校验(内核缓冲区溢出隐患),`_k` 变体必须加校验。
4. **RX**:纯轮询读 `rx_ring[0]` BD,无中断无 NAPI(`enetc_setup_irqs` 被注释,`:2874`);一次调用最多 1 帧;**硬编码只放行 EtherType 0x88A4**、其余帧丢弃(`:1662`);page 半页翻转复用 + 16 BD 批量回填。已知缺陷:无包路径上未初始化 `skb` 被 `dev_kfree_skb`(`:1633-1646,1679`,UB)—— `_k` 变体顺手修复。
5. **独占性**:该 PF 上 `ndo_start_xmit` 是空桩直接吞包(`:1504`),协议栈流量黑洞;驱动与标准 `fsl-enetc4` 用相同 PCI ID 表(`pci1131,e101`),通过 driver_override 选择绑定。**一个 ENETC 口整口献给 E2CF**(i.MX95 有 3 个 ENETC 口,其余口走标准驱动不受影响)。
6. **用户态 fast socket 框架**:全局单例(`net/socket.c:116` `fast_raw_socket_fd`),与本设计无冲突 —— 我们不经过 socket 层。

---

## 3. 补丁一:enetc_ecat 内核缓冲区变体(`_k`)

新增导出符号(放 `enetc_ecat.c`,复用既有静态函数,不动原路径):

```c
/* 内核缓冲发送:与 enetc4_ecat_fast_xmit 同路径,memcpy 替代 copy_from_user,
 * 并补上原版缺失的长度校验。仅进程上下文。 */
int enetc4_ecat_fast_xmit_k(struct net_device *ndev, const void *kbuf, size_t len)
{
    struct enetc_ndev_priv *priv = netdev_priv(ndev);
    struct enetc_bdr *tx_ring;
    int err;

    if (unlikely(len < ETH_ZLEN || len > ENETC4_MAC_MAXFRM_SIZE - ETH_FCS_LEN))
        return -EINVAL;
    if (!mutex_trylock(&priv->fast_ndev_lock))
        return -EBUSY;
    tx_ring = priv->tx_ring[0];
    err = enetc_ecat_map_tx_buffs_k(tx_ring, kbuf, len);  /* memcpy 版,内部含
                                                              enetc_bd_unused() 满检查 */
    mutex_unlock(&priv->fast_ndev_lock);
    return err;     /* 0 成功;-ENOSPC ring 满(调用方重试) */
}
EXPORT_SYMBOL_GPL(enetc4_ecat_fast_xmit_k);

/* 内核缓冲接收:EtherType 过滤参数化;返回实际帧长(含以太头)而非入参 len;
 * 一次取一帧,无帧返回 0。修复原版未初始化 skb 释放问题。 */
int enetc4_ecat_fast_recv_k(struct net_device *ndev, void *kbuf, size_t buflen,
                            __be16 ethertype);
EXPORT_SYMBOL_GPL(enetc4_ecat_fast_recv_k);
```

与原版的差异点(每条都是必须项):

| # | 原版行为 | `_k` 变体 |
|---|---|---|
| 1 | `copy_from_user/to_user` | `memcpy` |
| 2 | TX 无 ring 满检查 | `enetc_bd_unused() < 1` 返回 -ENOSPC(虽然 2048 深 ring 在本负载下几乎不可能满,但内核 API 不允许静默溢出) |
| 3 | TX `len` 无上界校验 | 校验 `[ETH_ZLEN, 1996]` |
| 4 | RX 硬编码 0x88A4 | 形参 `ethertype`(本驱动传 `htons(0x88B5)`);不匹配帧丢弃并计数(保持独占口语义) |
| 5 | RX 按入参 `len` 定长拷贝 | 按实际帧长拷贝(E2CF 聚合帧变长),返回帧长;`buflen` 不足返回 -EMSGSIZE 并丢弃计数 |
| 6 | 无包路径释放未初始化 skb | `skb = NULL` 初始化 + `if (skb)` 保护 |

不改变:ring 结构、buffer 预分配策略、doorbell、轮询模型、`fast_ndev_lock` 语义。原用户态 0x88A4 路径完全不受影响(两套入口共存,靠同一把锁互斥 —— 部署上一个口只用其一)。

可选增强(Phase 2,非首版必需):为 rx_ring[0] 启用 1 个 MSI-X 向量 + threaded IRQ(`ecat_enetc_alloc_msix` 框架已在,`enetc_ecat.c:3667`),RX 线程由纯轮询改为"中断唤醒 + 批量收割",空载 CPU 占用降为 0、唤醒延迟更确定。首版用自适应轮询即可达标。

---

## 4. eth2can.ko 模块设计

### 4.1 数据结构

```c
#define E2CF_NCHAN   6
#define E2CF_WIN     16            /* 每通道窗口 = echo_skb_max,与 MCU GET_INFO 协商取 min */
#define E2CF_MTU_BUF 1536

struct e2cf_chan_priv {
    struct can_priv  can;          /* 必须是第一个成员 */
    struct e2cf_core *core;
    u8   ch;
    u8   tx_head;                  /* 下一个 echo_id(模 E2CF_WIN) */
    u8   inflight;                 /* 飞行中帧数,agg_lock 保护写,RX 线程读改 */
    bool remote_stopped;           /* bus-off / STOP 状态 */
};

struct e2cf_core {
    struct net_device   *canch[E2CF_NCHAN];
    struct net_device   *eth_ndev;        /* enetc_ecat 的 ndev,模块参数指定接口名 */

    /* TX 聚合器 */
    spinlock_t           agg_lock;        /* RT 上为 rtmutex,PI 友好 */
    u8                   agg_buf[E2CF_MTU_BUF];
    u16                  agg_len;         /* 当前已编码字节,0=空 */
    u8                   agg_cnt;         /* 记录数 */
    struct hrtimer       agg_timer;       /* HRTIMER_MODE_REL_HARD */
    ktime_t              agg_timeout;     /* 模块参数 t_agg_ns,默认 0=直发 */
    struct task_struct  *tx_thread;       /* SCHED_FIFO 52 */
    wait_queue_head_t    tx_wq;
    bool                 tx_kick;

    /* RX */
    struct task_struct  *rx_thread;       /* SCHED_FIFO 53 */
    u8                   rx_buf[E2CF_MTU_BUF];
    struct sk_buff_head  skb_pool;        /* 预分配 canfd skb 池,水位 256/512 */
    struct work_struct   pool_refill;

    /* 配置通道 */
    struct mutex         cfg_lock;        /* 串行化 netlink 配置 */
    struct completion    cfg_done;
    u8                   cfg_token;
    struct e2cf_cfg_rsp  cfg_rsp;

    /* 协议状态 */
    u16  tx_seq, rx_seq_expect;
    u8   peer_mac[ETH_ALEN];
    unsigned long        last_hb_jiffies;
    struct delayed_work  hb_work;         /* 100ms 发 HB + 检查超时 */

    struct e2cf_stats    stats;           /* seq_lost, agg histo, per-type counters */
};
```

> **(Phase-2 结构)** 上面是 Phase-2 的聚合器/线程模型。**Phase-1 实际结构**(`eth2can.c`):6 路共享一个 `struct e2cf_dev`(gs_usb 风格 mux),**无** TX 聚合器、kthread、skb 池;TX 槽位用每通道 `echo_busy` 位图(`find_first_zero_bit` 取槽,wire `tag`=槽号)而非 `tx_head/inflight`,并存 `echo_ts[]/echo_t2[]` 时戳。

### 4.2 初始化(probe 流程)

模块参数(**Phase-1 实际**,`eth2can.c`):`ifname`(下层网口,默认 **`eth0`**)、`vid`(默认 **`-1`** = 不打 VLAN tag 的 bring-up 默认;部署按协议置 `100`)、`peer`(默认空 = 从网关 HB 学习对端 MAC)。**无 `t_agg_ns`**(Phase-1 Linux→MCU 逐帧直发);Phase-2 才引入聚合定时参数。

1. `dev_get_by_name(ifname)`,校验其 `netdev_ops` 含 `ndo_fast_xmit`(确认是 ecat 驱动);`dev_open()` 确保 fast path 解锁。
2. 6 次 `alloc_candev(sizeof(struct e2cf_chan_priv), E2CF_WIN)`,填:

```c
priv->can.clock.freq              = 80 * 1000 * 1000;   /* FlexCAN PE 时钟,经 GET_INFO 校准 */
priv->can.bittiming_const         = &e2cf_nom_btc;      /* FlexCAN ENCBT 字段范围,见 §4.7 */
priv->can.fd.data_bittiming_const = &e2cf_dat_btc;
priv->can.do_set_bittiming        = e2cf_set_bittiming;
priv->can.fd.do_set_data_bittiming= e2cf_set_data_bittiming;
priv->can.do_set_mode             = e2cf_set_mode;       /* CAN_MODE_START: bus-off 恢复 */
priv->can.do_get_berr_counter     = e2cf_get_berr;       /* 回缓存的 EVT tec/rec */
priv->can.ctrlmode_supported      = CAN_CTRLMODE_FD | CAN_CTRLMODE_LISTENONLY |
                                    CAN_CTRLMODE_LOOPBACK;   /* v1:无 ONE_SHOT(固件 ENOTSUP)、无 BERR_REPORTING、无 FD_NON_ISO */
ndev->netdev_ops   = &e2cf_netdev_ops;
ndev->flags       |= IFF_ECHO;          /* 驱动负责回环,TX 完成语义正确(Phase-1 仅此一个 flag) */
/* (Phase-2) ndev->priv_flags |= IFF_NO_QUEUE; noqueue 直通。
   Phase-1 不设此 flag —— 走标准 qdisc,并在 xmit 中处理 NET_XMIT_CN 拥塞返回。 */
```

3. `register_candev()` ×6;启动 tx/rx kthread(`sched_set_fifo`);启动 hb_work;发 GET_INFO 协商 `WIN`/时钟;skb 池预填。

### 4.3 TX 路径(用户 write → CAN 总线)

> **(本节为 Phase-2 设计:noqueue + 聚合器 + flush 线程。)** **Phase-1 实际**(`e2cf_ndo_start_xmit`):每帧独立成 skb,`echo_id = find_first_zero_bit(&chan->echo_busy, E2CF_WIN_DEPTH)` 取窗口槽位(满则 `netif_stop_queue` 并**返回 `NETDEV_TX_BUSY`**);经 `can_dropped_invalid_skb()` 过滤后 `e2cf_build_skb()` + `dev_queue_xmit()` **逐帧直发**(无聚合、无 hrtimer);`can_put_echo_skb(skb, ndev, echo_id, 0)`(frame_len 传 0,长度在 TXC echo-return 时由 `can_get_echo_skb` 取回)。下面的聚合器/flush-线程伪码是 Phase-2。

路径:`raw_sendmsg` → `can_send` → `dev_queue_xmit` → **noqueue 直通**(`net/core/dev.c:4746`,调用者上下文持 HARD_TX_LOCK 直接进 `ndo_start_xmit`,零 qdisc 排队;CAN 设备默认强制 pfifo_fast,`sch_generic.c:1176`,故必须 IFF_NO_QUEUE)。

```c
static netdev_tx_t e2cf_xmit(struct sk_buff *skb, struct net_device *ndev)
{
    struct e2cf_chan_priv *priv = netdev_priv(ndev);
    struct e2cf_core *core = priv->core;
    unsigned long flags;
    u8 idx;

    if (can_dev_dropped_skb(ndev, skb))           /* 非法帧/LISTENONLY 过滤 */
        return NETDEV_TX_OK;

    spin_lock_irqsave(&core->agg_lock, flags);
    if (e2cf_agg_room(core, skb) < 0)             /* 缓冲将超 MTU:先标记 flush */
        e2cf_kick_flush_locked(core);
    idx = priv->tx_head++ % E2CF_WIN;
    e2cf_encode_data_rec(core, priv->ch, idx, skb);   /* 编码进聚合缓冲(memcpy,
                                                          记录头与 canfd_frame 同构) */
    can_put_echo_skb(skb, ndev, idx, can_skb_get_frame_len(skb));
    if (++priv->inflight >= E2CF_WIN || priv->remote_stopped)
        netif_stop_queue(ndev);                    /* 窗口流控,永不依赖 TX_BUSY */

    if (core->agg_timeout == 0 || e2cf_agg_full(core)) {
        e2cf_kick_flush_locked(core);              /* 置 tx_kick + wake_up(tx_wq) */
    } else if (!hrtimer_active(&core->agg_timer)) {
        hrtimer_start(&core->agg_timer, core->agg_timeout, HRTIMER_MODE_REL_HARD);
    }
    spin_unlock_irqrestore(&core->agg_lock, flags);
    return NETDEV_TX_OK;
}
```

关键点:

- **为什么经 TX flush 线程而不在 xmit 上下文直接发**:`fast_xmit_k` 内是 mutex(可睡眠),而 `ndo_start_xmit` 在 noqueue 下运行于发送进程上下文且持 HARD_TX_LOCK(RT 上是可睡 spinlock,但在其临界区内再拿 mutex 会引入跨任务优先级耦合);更重要的是 6 个 netdev 并发 xmit 时,收敛到单一 flush 线程消除了对底层 ring 的争抢与乱序。flush 线程被 `wake_up` 后立即运行(同核 SCHED_FIFO,唤醒 ~2-3 µs),延迟代价可控。
- **hrtimer 用 `HRTIMER_MODE_REL_HARD`**:RT 上普通 hrtimer 回调进 ktimersoftd 有调度延迟;HARD 模式回调在硬中断,内部只做 `core->tx_kick = true; wake_up(&core->tx_wq);`(wake_up 在硬中断合法),不碰 agg_lock。
- **echo 语义**:`can_put_echo_skb` 暂存 skb 并打 TX 软时间戳;收到 TXC 后 `can_get_echo_skb(ndev, tag, &frame_len)` 回灌本机其它 socket 并计 `tx_packets/tx_bytes` —— 用户态看到的"发送完成"= 帧真正上了 CAN 总线,语义精确。
- TX flush 线程主循环:

```c
while (!kthread_should_stop()) {
    wait_event_interruptible(core->tx_wq, READ_ONCE(core->tx_kick) || kthread_should_stop());
    WRITE_ONCE(core->tx_kick, false);
    spin_lock_irqsave(&core->agg_lock, flags);
    len = e2cf_seal_frame_locked(core, frame_buf);   /* 填 Eth/VLAN/E2CF 头+seq,
                                                         拷出聚合缓冲并清空 */
    spin_unlock_irqrestore(&core->agg_lock, flags);
    if (len) {
        ret = enetc4_ecat_fast_xmit_k(core->eth_ndev, frame_buf, len);
        if (ret == -EBUSY) { usleep_range(2, 5); /* 与 RX 线程锁碰撞,重试 */ goto retry; }
    }
}
```

   (`fast_ndev_lock` 在 TX flush 线程与 RX 线程之间会碰撞 —— 两者都是短临界区,trylock 失败微退避重试即可;Phase 2 可在 `_k` 补丁中拆分 TX/RX 两把锁,见 §9 风险表。)

### 4.4 RX 路径(以太帧 → 用户 recv)

> **(本节 RX kthread / 轮询退避为 Phase-2。)** **Phase-1 实际**:RX 走 `dev_add_pack(htons(E2CF_ETHERTYPE))` 注册的 `packet_type` 回调,在 **softirq** 上下文逐帧处理(无 kthread、无轮询退避、无 skb 池);DATA 记录每条直接 `alloc_canfd_skb()/alloc_can_skb()` 分配(失败计 `rx_dropped`)后 `netif_receive_skb`。代码中的以太类型宏是 `E2CF_ETHERTYPE`(=0x88B5),**不存在** `ETH_P_E2CF` 符号。下方 demux 各分支(DATA/TXC/EVT/CFG_RSP/TIME/HB)的处理逻辑两阶段一致。

RX kthread 主循环(SCHED_FIFO 53,优先级高于 TX flush —— RX 还承担 TXC/流控释放职责):

```c
while (!kthread_should_stop()) {
    len = enetc4_ecat_fast_recv_k(eth, core->rx_buf, sizeof(core->rx_buf),
                                  htons(E2CF_ETHERTYPE));
    if (len <= 0) {
        if (++idle > 64) usleep_range(20, 50);   /* 自适应退避:忙时全速轮询,
                                                     空闲时 ~30µs 周期,单核占用 <5% */
        continue;
    }
    idle = 0;
    e2cf_handle_frame(core, core->rx_buf, len);   /* 还会连续把 ring 抽干再退避 */
}
```

`e2cf_handle_frame` demux:

- **DATA**:对帧内 N 条记录,从 `skb_pool` 取预分配 skb(池空则 `alloc_canfd_skb` GFP_ATOMIC 兜底+计数),记录头 8B + payload memcpy 进 `canfd_frame`(同构,无字段搬运),按 `tag`(ts_off)+ ts_base + TIME 回归还原 `skb->tstamp`;**整批一次** `local_bh_disable(); for_each netif_receive_skb(skb); local_bh_enable();` —— RT 上协议层(`can_rcv→raw_rcv→唤醒用户`)同步运行在 RX 线程自己的 RT 优先级里,无 softirq 漂移(这正是不用 can_rx_offload 的原因:单一有序隧道流不需要时间戳重排序,省一层队列+NAPI 调度)。
- **TXC**:逐条 `can_get_echo_skb(canch[chan], tag, &flen)`(status≠0 时 `can_free_echo_skb` + `tx_errors++`),`inflight--`,曾 stop 且 `inflight < WIN` 则 `netif_wake_queue`。echo 槽位的写者在 xmit(agg_lock 内)、清除者只有 RX 线程,单消费者无需额外锁(gs_usb 同语义)。
- **EVT**:`can_state_get_by_berr_counter` + `can_change_state`(状态变化时)+ `alloc_can_err_skb` 填 `cf->data[6]/[7]`=tec/rec 后 `netif_rx`;`state==BUS_OFF` 时 `can_bus_off(ndev)`(配 restart-ms 自动经 `do_set_mode(CAN_MODE_START)` → CFG START 恢复);rx_ovf/arb_lost 计入 netdev stats。
- **CFG_RSP**:token 匹配则拷贝 rsp + `complete(&core->cfg_done)`。
- **TIME**:更新 time_ref(线性回归 MCU 时钟→ktime,参照 peak_usb time_ref);**HB**:更新 `last_hb_jiffies`、首帧学习 peer_mac、检测 uptime 回绕(MCU 重启→重下发配置)。

### 4.5 配置通道(netlink 截获)

`ip link set eth2can2 type can bitrate 1000000 dbitrate 8000000 fd on` → `can_changelink`(RTNL 内,IFF_UP 时内核已拒绝)按 `bittiming_const` 算好 brp/tseg → 调驱动回调:

```c
static int e2cf_set_bittiming(struct net_device *ndev)
{
    struct e2cf_chan_priv *priv = netdev_priv(ndev);
    struct can_bittiming *bt = &priv->can.bittiming;
    struct e2cf_cfg_bitrate c = {
        .nom_brp = bt->brp, .nom_tseg1 = bt->prop_seg + bt->phase_seg1,
        .nom_tseg2 = bt->phase_seg2, .nom_sjw = bt->sjw,
        /* data 段参数在 do_set_data_bittiming 中合并填充;两回调都只缓存,
           真正下发集中在 ndo_open(e2cf_open)里一次 SET_BITRATE+START */
    };
    return e2cf_cfg_xfer(priv->core, E2CF_OP_SET_BITRATE, priv->ch, &c);
}

/* e2cf_cfg_xfer: cfg_lock 串行 → 编码 CFG_REQ(token++) → 经 TX 线程直发(绕过聚合,
   PCP=2)→ wait_for_completion_timeout(10ms) → 不匹配/超时重发,共 3 次 → -ETIMEDOUT */
```

`ndo_open`(`e2cf_open`):`open_candev()` 校验 → 下发 SET_BITRATE(含 ctrlmode 映射:FD/listen-only/**loopback**;one-shot 固件 ENOTSUP,驱动不广告)→ START → `netif_start_queue`。`ndo_stop`:STOP → `close_candev()`。

### 4.6 锁与并发汇总

> **(Phase-2 模型)** Phase-1 并发更简单:RX 在 softirq、TX 在调用者上下文;echo 槽位由每通道 `echo_busy` 位图管理(写在 xmit、清在 TXC softirq),CFG 由 `cfg_lock` 串行化,无 `agg_lock`/flush 线程/`fast_ndev_lock`。

| 锁/机制 | 类型 | 保护对象 | 持有者 |
|---|---|---|---|
| `agg_lock` | spinlock_t(RT=rtmutex,PI) | 聚合缓冲、tx_head/inflight、seq | 6×xmit、TX flush 线程(取帧时) |
| `fast_ndev_lock` | 既有 mutex(enetc_ecat 内,trylock) | 底层 ring | 仅 TX flush 线程 / RX 线程 |
| echo_skb[] | 写在 agg_lock 内、清除仅 RX 线程 | 槽位 | 单消费者隐式安全 |
| `cfg_lock` + completion | mutex | 配置请求-响应配对 | netlink 调用进程(RTNL 下) |
| RTNL | 内核持有 | bittiming/mode | netlink 回调天然串行 |

### 4.7 bittiming_const(已按 `eth2can.c` 实测常量校准 —— FlexCAN ENCBT/EDCBT 字段宽度)

```c
/* 实际值见 eth2can.c:e2cf_nom_bittiming_const / e2cf_dat_bittiming_const */
static const struct can_bittiming_const e2cf_nom_btc = {
    .name = "e2cf", .tseg1_min = 2, .tseg1_max = 96, .tseg2_min = 2, .tseg2_max = 32,
    .sjw_max = 32, .brp_min = 1, .brp_max = 1024, .brp_inc = 1,   /* CiA 区间;EPRS brp 10bit */
};
static const struct can_bittiming_const e2cf_dat_btc = {
    .name = "e2cf", .tseg1_min = 2, .tseg1_max = 32, .tseg2_min = 2, .tseg2_max = 16,
    .sjw_max = 16, .brp_min = 1, .brp_max = 255, .brp_inc = 1,    /* EDCBT 5/4bit;线上 dat_brp 为 u8 */
};
```

80 MHz PE 时钟下 8 Mbps 数据段 = 10 tq/bit(brp=1, tseg1=7, tseg2=2, sjw=2, TDC on),1 Mbps 仲裁段 = 80 tq/bit(brp=1, tseg1=63, tseg2=16)。

---

## 5. PREEMPT_RT 部署调优(交付物含部署脚本)

| 项 | 配置 | 理由 |
|---|---|---|
| CPU 隔离 | `isolcpus=nohz,domain,managed_irq,5 rcu_nocbs=5`(i.MX95 共 6×A55,留 core5) | RX/TX 线程 + 用户实时线程同核,避免跨核唤醒 IPI |
| 线程优先级 | RX kthread FIFO 53 > TX flush FIFO 52 ≥ 用户实时线程 ≤51 | RX 承担流控释放,优先 |
| 线程绑核 | `kthread_bind(core5)` | |
| skb 池 | 预填 512 个 canfd skb,水位 <128 时低优先级 work 补充 | 规避 GFP_ATOMIC slab 慢路径尾延迟 |
| backlog | 不用 netif_rx,无需 `thread_backlog_napi` | 批量 `netif_receive_skb` 已在 RX 线程内同步完成 |
| busy poll | 不适用:`SO_BUSY_POLL` 对 AF_CAN 是 no-op(`net/can/` 无 napi_id 标记,已查证) | 用户侧用阻塞 recv + RT 优先级即可 |
| 用户态建议 | `CAN_RAW_LOOPBACK` 按需关闭;接收线程 SCHED_FIFO;`SO_RXQ_OVFL` 监控丢包 | |

---

## 6. 统计与可观测性

- 标准 netdev stats:rx/tx_packets/bytes/errors/dropped(语义见协议 §7)。
- `ethtool -S eth2canN`(**Phase-1 实际**字符串集,共 46 项):前缀 `drv_`/`drvg_` = 驱动通道/全局计数(如 `drvg_seq_lost`、`drvg_seq_gaps`、`drvg_cfg_retries`),前缀 `gw_`/`gwg_` = 网关 1 Hz STATS 推送(协议 §4.9)解出的通道/全局计数。
- **debugfs**(Phase-1 已实现):`/sys/kernel/debug/eth2can/stats`(人类可读全量)、`/sys/kernel/debug/eth2can/clear_stats`(写 `all` 或 `0..5` → CFG op=9 CLEAR_STATS)。
- `ip -d -s link show eth2canX`:state/berr-counter/restart-ms 原生可见。
- tracepoints(`e2cf_tx_flush`/`e2cf_rx_frame`):**Phase-2 计划项,Phase-1 未实现**。

## 7. 测试计划(验收标准)

| # | 测试 | 方法 | 通过标准 |
|---|---|---|---|
| T1 | 功能:6 节点配置 | `ip link set eth2canX type can bitrate 1000000 dbitrate 8000000 fd on up` ×6 | 全部 UP,GET_INFO 协商成功 |
| T2 | 回环延迟 | eth2canX 发 → MCU 侧 CAN 线对接 eth2canY → 收;用 `skb->tstamp`+硬件 GPIO 比对 | p99 单向附加延迟达协议 §8 预算 |
| T3 | 满载吞吐 | 6 路双向 64B 满载(cangen 改造版,61 380 fps/向)持续 1 h | 零丢帧(seq_lost=0,rx_ovf=0),CPU(core5)<70% |
| T4 | 小帧风暴 | 6 路 8B 145 900 fps 持续 10 min | 零丢帧,聚合直方图显示 n≥4 |
| T5 | 流控 | 单通道灌 >10 230 fps | netif_stop/wake 正常,无 MCU 溢出 TXC status=4 |
| T6 | 错误注入 | 拔 CAN 线/短接制造 bus-off | EVT→error frame→candump 可见;restart-ms 自动恢复 |
| T7 | 链路监督 | 拔以太网线/复位 MCU | 500 ms 内 carrier off;恢复后配置自动重下发 |
| T8 | 长稳 | 72 h 混合负载 + 周期配置变更 | 无内存泄漏(kmemleak)、无 ring 卡死 |
| T9 | RT 干扰 | T3 同时跑 `stress-ng` + cyclictest | 延迟预算仍满足(隔离核不受扰) |

## 8. 交付物清单

1. `0001-enetc_ecat-add-kernel-buffer-fast-path-variants.patch`(§3,~120 行含修复)
2. `drivers/net/can/e2cf/eth2can.c|.h` + Kconfig/Makefile(~2500 行)
3. 部署脚本:driver_override 绑定、cmdline、线程绑核/优先级、switch PCP 配置说明
4. `e2cf.lua` Wireshark dissector
5. 测试工具:`e2cf_bench`(cangen/canecho 改造,带时戳统计)

## 9. 风险与未决项

| 风险 | 影响 | 缓解 |
|---|---|---|
| `fast_ndev_lock` 单锁导致 TX/RX 线程互斥碰撞 | 微秒级重试延迟 | 首版微退避;Phase 2 补丁拆 TX/RX 两把锁(ring 本就独立) |
| RX 纯轮询空载 CPU 占用(~5%@30µs 退避) | 功耗/占核 | Phase 2 启用 rx_ring MSI-X threaded IRQ |
| MCXE31x FlexCAN ENCBT 字段范围未经 RM 复核 | bittiming_const 偏差 | 集成时对照 MCXE31XRM 修正(占位值取保守超集,内核会再夹紧) |
| 1 s TXC 超时兜底误判(极端拥塞) | 误报 tx_errors | 阈值可调;依赖 HB 先行判链路断 |
| 原 0x88A4 用户态 fast socket 与本驱动同口并用 | 互斥锁竞争 | 部署规则:一个口只跑一种业务(文档化) |
