# eth2can Linux 驱动安装与测试

[English](README.md)

`eth2can.ko` 将 MCXE31B 的六路 CAN FD 暴露为标准 SocketCAN 设备：
`eth2can0` 到 `eth2can5`。普通用户默认只需要安装驱动、确认
Ethernet/E2CF heartbeat、运行基础 CAN 收发检查，并在需要验收证据时运行
`canperf`。

默认 CAN FD 配置：

```text
bitrate  1000000
dbitrate 5000000
fd       on
```

## 环境要求

- Linux 发行版或 BSP 内核启用 SocketCAN：
  `CONFIG_CAN`、`CONFIG_CAN_RAW`、`CONFIG_CAN_DEV`
- 与当前 `uname -r` 匹配的 kernel build tree，通常为
  `/lib/modules/$(uname -r)/build`
- 常用工具：`make`、`gcc`、`kmod`、`iproute2`、`can-utils`

Raspberry Pi、Ubuntu/Debian、RK/i.MX vendor Linux 等系统都使用同一个安装
脚本。如果 vendor BSP 没有标准 headers，请通过 `KDIR=` 指向匹配的内核构建
目录。

## 一键安装

在仓库根目录运行：

```sh
sh linux/scripts/install_driver.sh
```

脚本会构建并加载 `eth2can.ko`，等待 MCXE31B heartbeat，然后将
`eth2can0..5` 配置为 CAN FD 1M/5M。脚本不会安装 DKMS，不会写 systemd
unit，也不会配置跨重启自动加载。

常用选项：

```sh
sh linux/scripts/install_driver.sh --ifname eth1
sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname end0
sh linux/scripts/install_driver.sh --dry-run
sh linux/scripts/install_driver.sh --no-load
sh linux/scripts/install_driver.sh --require-gateway
```

显式覆盖速率：

```sh
sh linux/scripts/install_driver.sh --bitrate 1000000 --dbitrate 5000000
```

## 手动命令

安装脚本失败排查时，可手动加载和配置驱动：

```sh
sudo insmod linux/eth2can.ko ifname=eth0
sudo ip link set eth0 up
sudo ip link set eth2can0 type can bitrate 1000000 dbitrate 5000000 fd on
sudo ip link set eth2can0 up
```

基础流量测试前，确认每条 CAN FD 总线两端各有 120 ohm 终端，CANH/CANL
极性正确，收发器供电正常并共地。

默认线束：

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

基础收发检查：

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

## 性能测试入口

```sh
cd linux/can_testcase
make
./canperf latency --count 10000
./canperf bandwidth
```

验收时先看最终 `RESULT` 行。`latency` 报告端到端 p50、p99 和 p99.9；
`bandwidth` 报告双向 MSR，即 maximum sustainable rate。`zero_loss=yes`
表示本次运行应用层无丢帧。`counters=clean` 表示可读的驱动和网关计数器
没有出现丢帧、溢出、拒绝或发送失败增量。本仓库不定义固定 p99 或 MSR 阈值。

## 故障排查

| 现象 | 检查项 |
|---|---|
| 找不到 `/lib/modules/$(uname -r)/build` | 安装与当前内核匹配的 headers，或传入 `KDIR=` |
| `vermagic` 不匹配 | 在目标内核上重新构建 `eth2can.ko` |
| 没有 `eth2can0..5` 设备 | 查看 `dmesg` 中的 insmod 错误，并确认 SocketCAN 内核配置 |
| `gateway heartbeat was not observed` | 检查网线、`--ifname`、MCXE31B 固件、交换路径和 VLAN |
| `ip link set eth2canN ...` 超时 | heartbeat 未建立，或网关没有响应 CFG |
| CAN 收不到帧 | 检查默认线束、终端、收发器供电、共地、CANH/CANL 极性和速率 |

LED 辅助判断：

| LED | 状态 | 检查方向 |
|---|---|---|
| SYS | 约 1 Hz 闪烁 | 固件主循环正常运行 |
| SYS | 不闪烁 | 先检查固件镜像、电源和复位 |
| NET | 熄灭 | PHY link down；检查网线、交换机和主机网口状态 |
| NET | 闪烁 | PHY link up，但 E2CF peer heartbeat 尚未 ready |
| NET | 常亮 | Ethernet/E2CF 链路 ready |
| CAN | 闪烁 | CAN RX/TX 流量正在通过 |
| CAN | 常亮 | 至少一个 active CAN 通道 error-passive 或 bus-off |

抓取 heartbeat：

```sh
sudo tcpdump -eni eth0 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'
```

期望每 100 ms 看到一次 EtherType `0x88B5` heartbeat。如果 tcpdump 能看到流量，
但驱动始终没有 `gateway alive`，请保存抓包和 `dmesg` 给支持团队。
