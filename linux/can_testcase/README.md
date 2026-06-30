# canperf — E2CF 网关分段时延测量(单文件)

整个测试就**一个 C 文件**(`src/canperf.c`,自包含、零依赖),当前实现
一个能力:**应用层到应用层的端到端时延 + 三段分解**。后续 performance /
function 能力会继续加进这同一个文件。

## 测量模型(can0 → can4,默认外接 0-4 / 1-2 / 3-5)

一帧按时间从左到右,5 个全精确分段(无跨钟映射、无偏差):

```
 t1          t2            t3              t4           nrx        t5
 +━━━━━━━━━━━+┄┄┄┄┄┄┄┄┄┄┄┄+═══════════════+┄┄┄┄┄┄┄┄┄┄┄+━━━━━━━━━━+
 app         eth 出口      MCU eth 入口     MCU eth 出口  NIC 入栈   app
 (Linux 钟)              (网关钟)         (网关钟)      (Linux 钟)
 +━━+ Linux 钟段   +┄┄+ 以太线缆(派生)   +══+ 网关钟段
```

| 时刻 | 含义 | 时钟 |
|---|---|---|
| t1 | 应用 `send()` 前 | Linux REALTIME |
| t2 | eth2can 驱动把帧交给底层网卡(回显 skb 软件戳) | Linux REALTIME |
| t3 | 请求帧到达 MCU EMAC(经 TXC `req_eth_rx_ns`,回显 skb 硬件戳) | 网关钟(原始,不映射) |
| t4 | MCU 把上行帧交给 EQOS(经 DATA 帧尾 `tx_eth_ns`,远端 skb 硬件戳) | 网关钟(原始,不映射) |
| nrx | 上行帧进 Linux 协议栈(内核软件 RX 戳) | Linux REALTIME |
| t5 | 应用 `recv()` 返回 | Linux REALTIME |

五个分段(min/p50/p99/p99.9/max/平均,µs)**全部为同钟差值,精确无偏差**:

- **total = t5−t1** 端到端;
- **L1 = t2−t1** Linux 发送路径(app + CAN socket/qdisc + 驱动);
- **L2 = total−L1−L3−L4**(派生)上下行两段以太线缆 + 交换机 + NIC;
- **L3 = t4−t3** MCU 内部驻留,**含 CAN 总线往返**——t3/t4 同为网关钟,核心优化指标;
- **L4 = t5−nrx** Linux 接收路径(NIC 入栈 → 协议栈 → 唤醒)。

**关键**:t3/t4 的网关时刻**只参与互减(L3 = t4−t3),从不与 Linux 时钟比较**,
所以无需 1Hz TIME 锚定映射,L3 与其余段一样精确。L2 是端到端减掉三段已知量后的
余量,因 Linux/网关两钟微小频率差偶尔会有个位 µs 负值被钳零(表尾计数)。

## 实现原理(5 个时刻怎么拿到)

- **MCU 固件**:EMAC ISR 入口记请求帧到达时刻,穿过 sw_txfifo 在 TXC 里带回
  `req_eth_rx_ns`(t3);上行 DATA 帧发送瞬间在帧尾追加 4B `tx_eth_ns`(t4)。
- **驱动**:xmit 时记 t2 存入回显槽,TXC 回环时写到回显 skb 软件戳;TXC 的
  `req_eth_rx_ns` 原始值(不映射)写回显 skb 硬件戳;DATA 帧尾 `tx_eth_ns`
  原始值写远端 skb 硬件戳。应用层 `SO_TIMESTAMPING(RAW_HARDWARE|RX_SOFTWARE)`
  的 cmsg 一次拿到软/硬两个戳。

## 编译与使用

```sh
make            # 当前 Linux 平台本地编译(树莓派/Ubuntu/工控机等)
make cross      # 显式 aarch64 静态交叉编译; 可传 TOOLCHAIN_DIR=/opt/...
make host       # ASan 编译检查

# 板上(需 root, 驱动已加载):
./canperf                   # 自动配 1M/8M FD, 依次测 0->4, 1->2, 3->5
./canperf --pair 0:4        # 只测一对
./canperf --size 8          # 经典帧
./canperf --count 20000 --gap-us 500 --csv   # 长测+逐帧CSV
./canperf --window 16 --gap-us 125           # 吞吐/丢帧率压测(8000 fps 注入)
./canperf --no-setup        # 通道已手工配好时不动配置
```

两种引擎:

- **锁步(默认,`--window 1`)**:每对任意时刻只有 1 帧在途,纯时延探针,
  发送速率被往返时延钳制(~2800 fps/对),`--gap-us` 只是探测间隔下限;
- **窗口(`--window 2..16`)**:每对 N 帧在途(上限 16 = 驱动 TXC 窗口深度,
  天然反压),`--gap-us` 变成**注入速率**(1e6/gap fps)——这是吞吐量和
  丢帧率的压测模式,五段直方图照常输出,页脚多一行实际完成 fps。

**真丢 vs 迟到**:超过 100ms 才到达的帧在超时当下被记为 lost/echo-miss,
事后到达时单独计入 `late(>100ms)` 行——真正物理丢失 = lost − late rx。
配合驱动侧 `ethtool -S` 的 `drvg_seq_lost`(64 帧滑动窗口确认的净丢帧数,
区别于会被乱序触发的 `drvg_seq_gaps`)可把每一帧丢失定位到具体环节。

典型输出(运行开始先打印测量路径示意图 + 分段图例横幅,结束后每对一张
分解表、最后一张跨链路汇总表,全部制表符对齐、单位统一 µs):

```
+- eth2can0 -> eth2can4 -- latency (us) --------------------------------------------+
| segment  | span   | trust  |    min |    p50 |    p99 |  p99.9 |    max |   mean |
+----------+--------+--------+--------+--------+--------+--------+--------+--------+
| total    | t5-t1  | exact  |    142 |    171 |    219 |    240 |    263 |    175 |
| L1 lnxTX | t2-t1  | exact  |      2 |      4 |      7 |      9 |     12 |      4 |
| L2 wire  | deriv  | exact  |     14 |     18 |     26 |     31 |     38 |     19 |
| L3 mcu   | Y-X    | exact  |    110 |    135 |    170 |    188 |    205 |    138 |
| L4 lnxRX | t5-nrx | exact  |      3 |      6 |     11 |     14 |     19 |      7 |
+----------+--------+--------+--------+--------+--------+--------+--------+--------+
| sent 5000      lost 0       echo-miss 0       no-gw-stamp 0                       |
+----------------------------------------------------------------------------------+
```

trust 列:**五段全部 `exact`** —— 每段都是同钟差值,无跨钟映射、无偏差
(L3 = 网关钟原始 Y−X,从不与 Linux 钟比较)。L2 为派生段(total 减
L1/L3/L4);Linux/网关两钟微小频率差偶尔致 L2 个位 µs 负值被钳零,在表尾
note 行计数。

`--csv` 生成 `pair-A-B.csv`(seq + **六时刻**原始 ns:
`seq,t1_ns,t2_ns,X_gw,Y_gw,nicrx_ns,t5_ns`),可直接 Excel/脚本分析。

## 性能测试规程(标准矩阵)

每份报告自带证据:工具在运行前后自动快照 debugfs 计数器并打印**增量**
(seq_lost/rej/starv/sfail/rx_ovf 等,丢帧自动标注方向),横幅给出当前
帧长/速率下的**总线理论容量**(最坏填充位),sweep 结果直接给出达成率。

| 步骤 | 命令 | 看什么 |
|---|---|---|
| 1. 时延基线 | `./canperf --count 100000` | 五段直方图;total≈L1+L2+L3+L4 闭合;counters delta 全 0 |
| 2. 单向极限 | `./canperf --sweep` | MSR 与理论达成率(64B FD 理论 ≈ 8989 fps/总线) |
| 3. 双向极限 | `./canperf --sweep --bidir` | 6 流并发;MCU 负载看 delta 行的 loop_per_s 跌幅 |
| 4. 经典帧极限 | `./canperf --sweep --size 8 --dbitrate 0` | 8B 经典帧理论 ≈ 1M/(47+64+~28)bit ≈ 7.2k fps |
| 5. 帧长扫描 | `--sweep --size 12/16/24/32/48` 逐档 | MSR-帧长曲线(吞吐字节率 vs 帧率的折衷) |
| 6. 负载下时延 | `./canperf --window 16 --gap-us <1.25/MSR>` 即 80% MSR | 与步骤 1 对比 p99/p99.9 退化 |
| 7. 长稳 | `./canperf --duration 3600 --report-s 60` | 1 小时零丢帧;期间后台跑你的 stress-ng/cyclictest |

sweep 判据可调:`--p99-limit N`(默认 1000µs)、`--sweep-frames N`(默认
2 万/档);Ctrl-C 随时中止并输出已完成的台阶表。`loop_per_s` 是网关超环
每秒迭代数(未标定 CPU 余量计——空载基线对比用,负载越高跌得越多)。

## 判读建议

- 五段全部同钟精确,无"映射健康"之说;若某时刻缺失,样本不计入对应段
  并反映在 `no-gw-stamp` 计数。
- 总时延闭合:total ≈ L1+L2+L3+L4(L2 即按此派生);明显不闭合说明某段
  时刻缺失(看 `no-gw-stamp` 计数)。
- **L3(MCU)** 天然含 0~50µs 聚合等待(协议 T_agg)与 CAN 总线往返,其
  p50 比 L1/L4 高是正常的 —— 这是核心优化指标。
- 测试期间的背景负载(stress-ng/cyclictest)自行后台运行,本工具不感知。
