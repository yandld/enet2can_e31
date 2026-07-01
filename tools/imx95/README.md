# i.MX95 内部 Bench 工具

本目录脚本用于内部 i.MX95/RTE bench：构建 vendor kernel、部署驱动、拷贝测试工具和维护启动分区。客户交付入口不是本目录，而是：

```sh
sh linux/scripts/install_driver.sh
cd linux/can_testcase && make && ./canperf latency
```

## 默认配置

内部脚本与客户交付保持一致：CAN FD `bitrate 1000000` / `dbitrate 5000000`。

`board_setup.sh` 会在板端加载 `eth2can.ko`，等待 MCXE31B heartbeat，然后配置 `eth2can0..5`。

## 何时使用

- NXP 内部 i.MX95 bench 自动部署
- vendor kernel/driver 联调
- 回归 canperf latency/bandwidth 前的板端准备

普通客户安装、Raspberry Pi、Ubuntu/Debian、RK/i.MX vendor Linux 等平台，请优先使用 `linux/scripts/install_driver.sh`。
