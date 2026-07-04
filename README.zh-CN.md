# MCXE31B 以太网转六路 CAN FD 桥接器

[English](README.md)

本仓库交付一个基于 MCXE31B 的以太网转六路 CAN FD 桥接器。Linux 主机加载
`eth2can.ko` 后，会看到六个标准 SocketCAN 设备：`eth2can0` 到
`eth2can5`。应用侧可以继续使用 `ip link`、`candump`、`cansend` 和其他
SocketCAN 上层软件。

默认 CAN FD 配置为：

```text
仲裁相位速率: 1 Mbit/s
数据相位速率: 5 Mbit/s
CAN FD BRS:   使能
```

## 仓库内容

| 路径 | 用途 |
|---|---|
| `source/` | MCXE31B 桥接器固件源码 |
| `board/` | 板级时钟、pin mux 和底层板级支持 |
| `linux/` | Linux SocketCAN 驱动、安装脚本和驱动测试 |
| `linux/can_testcase/` | `canperf` 延迟和双向带宽测试 |
| `docs/*.md` | E2CF、Linux 驱动、固件和 EQOS TX 处理设计说明 |

## 系统概览

Linux 与 MCXE31B 网关之间使用 raw Layer-2 Ethernet。Linux 驱动对外暴露
六个 SocketCAN 接口，并与固件交换 E2CF 帧。固件将数据转发到 FlexCAN0
到 FlexCAN5。

关键默认值：

| 项目 | 值 |
|---|---|
| Ethernet 协议 | Raw Layer-2 E2CF，EtherType `0x88B5` |
| 部署 VLAN | VID `100`；bring-up 阶段允许 untagged 帧 |
| Linux 接口 | `eth2can0` 到 `eth2can5` |
| CAN 通道 | FlexCAN0 到 FlexCAN5 |
| TX 窗口 | 每通道 16 个 echo slot |
| MCU 到 Linux 聚合 | 按记录数或 50 us 定时器触发 |
| Heartbeat | 100 ms 周期，500 ms 超时 |

## 硬件连接

使用六路外部 CAN FD 收发器。默认测试线束为三条独立 CAN FD 总线：

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

每条 CAN FD 总线两端各需要 120 ohm 终端。请确认 CANH/CANL 极性、收发器
供电，以及主机测试环境与网关板之间共地。

状态灯：

| LED | GPIO | 状态 | 含义 |
|---|---|---|---|
| SYS | PTC16 | 约 1 Hz 闪烁 | 固件主循环正在运行 |
| SYS | PTC16 | 常亮 | 固件处于 fault 或 fatal path |
| NET | PTB22 | 熄灭 | PHY link down |
| NET | PTB22 | 闪烁 | PHY link up，但 E2CF heartbeat 尚未 ready |
| NET | PTB22 | 常亮 | Ethernet/E2CF 链路 ready |
| CAN | PTC14 | 熄灭 | 无 CAN 流量 |
| CAN | PTC14 | 闪烁 | 有 CAN RX/TX 流量 |
| CAN | PTC14 | 常亮 | 至少一路 active CAN 处于 error-passive 或 bus-off |

## 固件

如果交付板已经预烧录固件，直接上电后继续 Linux 驱动步骤。需要自行构建时，
使用 Keil MDK 打开 `e2cf_mcxe31.uvprojx` 并构建 MCXE31B target。烧录使用
现有 MCXE31B/SWD 流程。

## Linux 快速开始

在目标 Linux 主机的仓库根目录运行：

```sh
sh linux/scripts/install_driver.sh
```

脚本会针对当前内核构建 `eth2can.ko`，在当前启动周期加载模块，等待网关
heartbeat，然后将 `eth2can0..5` 配置为 CAN FD 1M/5M。脚本不会安装 DKMS、
systemd unit，也不会配置跨重启自动加载。

常用选项：

```sh
sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname end0
sh linux/scripts/install_driver.sh --dry-run
sh linux/scripts/install_driver.sh --no-load
sh linux/scripts/install_driver.sh --require-gateway
```

检查 CAN 设备：

```sh
ip -details link show type can
```

## 基础收发检查

默认线束连接后，从通道 0 向通道 4 发送一帧 CAN FD+BRS：

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

如果 `eth2can4` 可以收到该帧，说明 Linux 驱动、Ethernet 路径、MCXE31B
固件以及至少一条物理 CAN FD 总线可工作。

## 验收测试

构建客户测试工具：

```sh
cd linux/can_testcase
make
```

运行延迟测试：

```sh
./canperf latency --count 10000
```

运行双向最大可持续带宽测试：

```sh
./canperf bandwidth
```

验收时先看最终 `RESULT` 行。`PASS` 表示本次运行 zero-loss。延迟测试报告
p50、p99 和 p99.9；带宽测试报告 MSR，即 maximum sustainable rate。`EVIDENCE`
行用于保存可复验记录；`counters=clean` 表示确认轮期间驱动和网关的丢帧、
溢出、拒绝、发送失败等计数器没有增长。本仓库不定义固定 p99 或 MSR 阈值；
实际阈值应根据目标线束、主机和系统负载共同确认。

## 故障排查

| 现象 | 检查项 |
|---|---|
| 找不到 kernel build tree | 安装与 `uname -r` 匹配的 headers，或传入 `KDIR=/path/to/kernel/build` |
| `vermagic` 不匹配 | 在目标内核上重新构建 `eth2can.ko` |
| 没有 `eth2can0..5` 设备 | 检查 `dmesg`、SocketCAN 内核配置和模块加载错误 |
| 网关 heartbeat 缺失 | 检查网线、`--ifname`、VLAN 路径、MCXE31B 固件和 EtherType `0x88B5` 流量 |
| CAN 收不到帧 | 检查默认线束、终端、收发器供电、共地、极性和 CAN 速率 |
| `counters=dirty` | 查看 `seq_lost`、`rx_ovf`、`rej`、`starv`、`sfail`、`emac_rxdrop` 增量 |

抓取 heartbeat：

```sh
sudo tcpdump -eni eth0 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'
```

期望每 100 ms 看到一次 E2CF heartbeat。

## 文档入口

- Linux 驱动安装和故障排查：[`linux/README.md`](linux/README.md)
- `canperf` 构建和结果解读：[`linux/can_testcase/README.md`](linux/can_testcase/README.md)
- 协议和实现设计说明：[`docs/`](docs/)

## 授权和发布

本仓库当前不以开源许可证发布。复制、再分发、产品使用或超出授权项目范围的
公开发布，都需要获得仓库所有者的书面许可。部分源文件还带有各自的 SPDX
声明，这些声明必须保留。
