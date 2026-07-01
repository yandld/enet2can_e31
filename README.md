# E2CF MCXE31B CAN FD 网关交付说明

本仓库交付 MCXE31B 以太网到 6 路 CAN FD 网关方案。Linux 侧加载 `eth2can.ko` 后，会看到标准 SocketCAN 设备 `eth2can0..5`，客户可以继续使用 `ip link`、`candump`、`cansend` 和上层 CAN 工具。

默认交付配置为 CAN FD `bitrate 1000000` / `dbitrate 5000000`，即 1M/5M BRS。

## 交付内容

| 路径 | 用途 |
|---|---|
| `source/` | MCXE31B 网关固件源码 |
| `linux/` | Linux SocketCAN 驱动、安装脚本和测试工具 |
| `linux/can_testcase/` | `canperf` 延迟与双向带宽测试 |
| `eth2can_design/` | 协议、Linux 驱动和 MCU 侧设计说明 |
| `tools/imx95/` | 内部 i.MX95 bench 构建/部署脚本，不作为客户入口 |

## 最短 Bring-up

1. 烧录 MCXE31B 固件并上电。
2. Linux 主机连接到网关以太网口。
3. 从仓库根目录安装驱动：

```sh
sh linux/scripts/install_driver.sh
```

4. 确认 6 个 SocketCAN 设备存在并已配置为 1M/5M：

```sh
ip -details link show type can
```

5. 按默认线束连接 3 组 CAN 总线：

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

每组总线需要正确终端电阻和 CAN FD 收发器连接。

## 基础自检

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

如果 `eth2can4` 能收到帧，说明驱动、网关、物理 CAN 线束至少一组链路可用。

## 交付测试

构建客户测试工具：

```sh
cd linux/can_testcase
make
```

延迟测试：

```sh
./canperf latency
```

双向最大可持续带宽测试：

```sh
./canperf bandwidth
```

验收时关注输出中的 `lost=0`、p50/p99 延迟、MSR 和 counters delta。当前文档不固定 p99 或带宽硬阈值，实际阈值应由客户线束、主机性能和系统负载共同确认。

## 常见入口

- Linux 驱动安装和故障排查：`linux/README.md`
- canperf 编译和结果解读：`linux/can_testcase/README.md`
- i.MX95 内部 bench 工具：`tools/imx95/README.md`
