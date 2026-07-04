# canperf 客户测试工具

[English](README.md)

`canperf` 面向 MCXE31B 桥接器的两类客户验收问题：

- 延迟：`latency`
- 双向最大可持续带宽：`bandwidth`

默认 CAN FD 速率为 1M/5M，并使能 BRS。默认线束为 `0<->4`、`1<->2`、
`3<->5`。

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

每组是一条独立 CAN FD 总线。总线两端各使用 120 ohm 终端，CANH/CANL 极性
正确，收发器供电正常，并与网关共地。运行 `canperf` 前，请先使用
`linux/scripts/install_driver.sh` 加载驱动并配置 1M/5M。

## 测试模型

`canperf` 在 Linux SocketCAN 设备上发包和收包。测试路径包含 Linux
`eth2can` 驱动、Ethernet 链路、MCXE31B 网关和物理 CAN FD 总线。

`latency` 默认使用 `0->4`、`1->2`、`3->5` 三个方向。每个方向一个线程，
一次只保留一帧在途，用于测量端到端延迟。

`bandwidth` 将默认线束镜像成六个并发方向：

```text
0->4  4->0
1->2  2->1
3->5  5->3
```

每个方向使用独立线程、SocketCAN socket、CAN ID 和递增 sequence。带宽测试
是受控速率压力测试，不是随机流量。它按固定节奏发送 64 字节 CAN FD+BRS
帧，每个方向最多保持 16 帧在途，与驱动 TXC window 深度一致。

## 延迟测试

```sh
./canperf
./canperf latency
./canperf latency --count 10000
./canperf latency --pair 0:4
./canperf latency --duration 10m
```

输出解读：

- 最终 `RESULT latency` 行是客户结论；`PASS` 表示本次运行 zero-loss。
- `total` 的 p50、p99 和 p99.9 是主要端到端延迟指标。
- p99.9 也称为 p999。
- `L1/L2/L3/L4` 用于定位延迟来自 Linux TX、Ethernet、MCU/CAN 或 Linux RX。
- `EVIDENCE latency` 用于保存可复验记录。
- `counters=clean` 表示驱动和网关的丢帧、溢出、拒绝、失败等计数器未增长。

## 双向带宽测试

```sh
./canperf bandwidth
./canperf bandwidth --pair 0:4
./canperf bandwidth --count 30000
```

`bandwidth` 会先搜索候选速率，再执行一次 final confirmation。客户验收只使用
final confirmation 的 `RESULT bandwidth`、`MSR` 和 `EVIDENCE bandwidth`；
不要把 sweep 试探阶段当作验收结果。

结果解读：

- `RESULT bandwidth: PASS` 是首要条件。
- `MSR = ... fps/pair DELIVERED` 是每个方向的可持续帧率。
- `aggregate` 是所有并发方向的合计帧率。
- `per CAN bus` 表示物理 CAN 总线利用率。
- `ceiling` 提示当前限制更像 CAN 总线受限，还是网关/主机路径受限。
- `EVIDENCE bandwidth` 是可复制到报告里的简洁证据行。

## 常用参数

```sh
--pair A:B      指定一组线束；可重复或逗号分隔指定多组
--count N       latency 的帧数；bandwidth 的每步帧数
--duration T    latency 的运行时间；bandwidth 的每步运行时间
--bitrate R     仲裁相位速率，默认 1000000
--dbitrate R    CAN FD 数据相位速率，默认 5000000
--no-setup      不自动配置 eth2canN
--help          查看帮助
```

客户测试范围是 CAN FD+BRS。classic CAN、loopback、CSV 导出和手动
sweep/window 调参不作为默认验收接口。
