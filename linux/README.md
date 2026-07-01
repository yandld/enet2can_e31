# eth2can Linux 驱动安装与测试

`eth2can.ko` 将 MCXE31B 网关的 6 路 CAN FD 暴露为标准 SocketCAN 设备 `eth2can0..5`。客户默认只需要安装驱动、确认链路、运行基础收发和 canperf 测试。

默认 CAN FD 配置：`bitrate 1000000` / `dbitrate 5000000`。

## 环境要求

- Linux 发行版或 BSP 内核支持 SocketCAN：`CONFIG_CAN`、`CONFIG_CAN_RAW`、`CONFIG_CAN_DEV`
- 有匹配当前 `uname -r` 的 kernel build tree，通常是 `/lib/modules/$(uname -r)/build`
- 常用工具：`make`、`gcc`、`kmod`、`iproute2`、`can-utils`

Raspberry Pi、Ubuntu/Debian、RK/i.MX vendor Linux 都走同一个安装脚本。若 vendor BSP 没有标准 headers，请用 `KDIR=/path/to/kernel/build` 指向匹配的内核构建目录。

## 一键安装

从仓库根目录运行：

```sh
sh linux/scripts/install_driver.sh
```

脚本会构建并加载 `eth2can.ko`，等待网关 heartbeat，然后把 `eth2can0..5` 配置为 1M/5M CAN FD。它不会安装 DKMS，不会写 systemd，也不会让模块跨重启自动加载。

常用参数：

```sh
sh linux/scripts/install_driver.sh --ifname eth1
sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname eth0
sh linux/scripts/install_driver.sh --dry-run
sh linux/scripts/install_driver.sh --no-load
sh linux/scripts/install_driver.sh --require-gateway
```

显式覆盖速率：

```sh
sh linux/scripts/install_driver.sh --bitrate 1000000 --dbitrate 5000000
```

## 手动命令

安装脚本失败排查时，可手动加载和配置：

```sh
sudo insmod linux/eth2can.ko ifname=eth0
sudo ip link set eth0 up
sudo ip link set eth2can0 type can bitrate 1000000 dbitrate 5000000 fd on
sudo ip link set eth2can0 up
```

基础收发：

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

默认线束：

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

## 性能测试入口

```sh
cd linux/can_testcase
make
./canperf latency
./canperf bandwidth
```

`latency` 输出端到端延迟和分段 p50/p99；`bandwidth` 输出双向最大可持续速率 MSR。客户验收建议保存完整终端输出，重点看 zero-loss、p50/p99、MSR 和 counters delta。

## 故障排查

| 现象 | 检查项 |
|---|---|
| 找不到 `/lib/modules/$(uname -r)/build` | 安装匹配当前内核的 headers，或传入 `KDIR=` |
| `vermagic` mismatch | 在目标板当前内核上重编 `eth2can.ko` |
| 没有 `eth2can0..5` | 查 `dmesg` 中 insmod 错误，确认 SocketCAN 内核配置 |
| `gateway heartbeat was not observed` | 检查网线、选中的 `--ifname`、MCXE31B 固件和交换机/VLAN |
| `ip link set eth2canN ...` 超时 | heartbeat 未建立或网关未响应 CFG |
| CAN 收不到帧 | 检查默认线束、终端电阻、收发器供电、CANH/CANL 极性 |

heartbeat 抓包：

```sh
sudo tcpdump -eni eth0 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'
```

期望看到 EtherType `0x88b5` heartbeat 周期帧。若 tcpdump 能看到但驱动无 `gateway alive`，请保存抓包和 `dmesg` 给支持团队。
