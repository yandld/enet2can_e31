# i.MX95 Maintainer Bench Notes

[中文说明](README.zh-CN.md)

This directory contains maintainer-only scripts for an internal i.MX95/RTE
bench. They are used to build a vendor kernel, deploy the driver, copy test
tools, and maintain the boot partition during integration work.

These scripts are not the customer bring-up entry point. For normal Linux
hosts, use:

```sh
sh linux/scripts/install_driver.sh
cd linux/can_testcase && make && ./canperf latency
```

## Default configuration

The bench scripts keep the same customer-facing CAN FD defaults:

```text
bitrate  1000000
dbitrate 5000000
```

`board_setup.sh` loads `eth2can.ko` on the target board, waits for the MCXE31B
heartbeat, and configures `eth2can0..5`.

## When to use

- i.MX95/RTE bench automation
- Vendor kernel and driver integration
- Board preparation before repeated `canperf latency` or `canperf bandwidth`
  runs

Do not use this directory as a generic installation path for Raspberry Pi,
Ubuntu/Debian, RK/i.MX vendor Linux, or other customer systems. Use
`linux/scripts/install_driver.sh` instead.
