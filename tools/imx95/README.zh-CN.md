# i.MX95 维护者 Bench 说明

[English](README.md)

本目录包含维护者专用的内部 i.MX95/RTE bench 脚本，用于构建 vendor kernel、
部署驱动、拷贝测试工具，以及在集成工作中维护启动分区。

这些脚本不是客户 bring-up 入口。普通 Linux 主机请使用：

```sh
sh linux/scripts/install_driver.sh
cd linux/can_testcase && make && ./canperf latency
```

## 默认配置

bench 脚本保持与客户侧一致的 CAN FD 默认值：

```text
bitrate  1000000
dbitrate 5000000
```

`board_setup.sh` 会在目标板上加载 `eth2can.ko`，等待 MCXE31B heartbeat，
然后配置 `eth2can0..5`。

## 何时使用

- i.MX95/RTE bench 自动化
- vendor kernel 和驱动集成
- 重复运行 `canperf latency` 或 `canperf bandwidth` 前的板端准备

不要把本目录作为 Raspberry Pi、Ubuntu/Debian、RK/i.MX vendor Linux 或其他
客户系统的通用安装路径。请使用 `linux/scripts/install_driver.sh`。
