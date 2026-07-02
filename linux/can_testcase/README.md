# canperf 客户测试工具

`canperf` 只面向两类客户验收问题：

- 延迟性能：`latency`
- 双向最大可持续带宽：`bandwidth`

默认 CAN FD 速率为 1M/5M，默认线束为 `0->4,1->2,3->5`。

## 编译

在目标 Linux 主机上原生编译：

```sh
make
```

指定编译器：

```sh
make CC=gcc
make CC=clang
```

显式 aarch64 交叉编译：

```sh
make cross
make cross TOOLCHAIN_DIR=/opt/arm-gnu-toolchain
```

## 线束

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

每组是独立 CAN FD 总线。总线两端各 120Ω 终端，CANH/CANL 不能接反，收发器需要正确供电并与网关共地。测试前先确认 `linux/scripts/install_driver.sh` 已经加载驱动并配置 1M/5M。

## 延迟测试

```sh
./canperf
./canperf latency
./canperf latency --count 10000
./canperf latency --pair 0:4
./canperf latency --duration 10m
```

输出重点：

- 最后的 `RESULT latency` 行是客户结论，`PASS` 表示 zero-loss
- `total` 的 p50/p99/p99.9 是客户最直观的端到端延迟；p99.9 即 p999
- `L1/L2/L3/L4` 用于定位延迟来自 Linux TX、以太网、MCU/CAN 或 Linux RX
- `EVIDENCE latency` 行用于保存复验证据；`counters=clean` 表示驱动/网关丢帧、溢出、拒绝等计数器未增长

## 双向带宽测试

```sh
./canperf bandwidth
./canperf bandwidth --pair 0:4
./canperf bandwidth --count 30000
```

`bandwidth` 会先搜索候选速率，再自动执行一次 final confirmation。客户验收只看 final confirmation 后的 `RESULT bandwidth`、`MSR` 和 `EVIDENCE bandwidth`，不要把 sweep 试探阶段当作验收结论。zero-loss 表示本次确认轮无应用层丢帧且可读取 counters 为 clean；MSR 是 maximum sustainable rate，即最终带宽结论。

结果解读：

- `RESULT bandwidth: PASS` 是最重要前提
- `MSR = ... fps/pair DELIVERED` 是每个方向的可持续帧率
- `aggregate` 是所有并发方向合计帧率
- `per CAN bus` 说明每条物理 CAN 总线的利用率
- `ceiling` 会提示当前更像 CAN 总线受限还是网关/主机数据路径受限
- `EVIDENCE bandwidth` 是可复制到报告里的简洁证据行

## 常用参数

```sh
--pair A:B      指定一组线束，可重复或逗号分隔
--count N       latency 的帧数；bandwidth 的每步搜索帧数
--duration T    latency 的运行时间；bandwidth 的每步搜索时间
--bitrate R     nominal bitrate，默认 1M
--dbitrate R    CAN FD data bitrate，默认 5M
--no-setup      不自动配置 eth2canN
--help          查看帮助
```

交付默认只支持 CAN FD BRS 测试；classic CAN、loopback、CSV、手动 sweep/window 不作为客户接口。
