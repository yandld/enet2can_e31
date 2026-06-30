# i.MX95 Internal Linux Tools

These scripts are for the NXP i.MX95 Real-Time Edge Linux bench flow. They are
not required for the customer-facing portable Linux driver install path.

Use `linux/scripts/install_driver.sh` for Raspberry Pi, RK, i.MX vendor Linux,
PC Ubuntu/Debian, and other targets that build the driver on the running
kernel.

## Scripts

| Script | Runs on | Purpose |
|---|---|---|
| `env.sh` | host | Shared RTE kernel, output, toolchain, and board deploy settings. |
| `build_driver.sh` | host | Cross-build `linux/eth2can.ko` against the RTE kernel tree. |
| `build_kernel.sh` | host | Prepare or build the i.MX95 RTE kernel Image, dtbs, and modules. |
| `deploy.sh` | host | Copy driver/kernel artifacts to an i.MX95 board over SSH/SCP. |
| `make_bundle.sh` | host | Build an offline i.MX95 board bundle with Image, module, and board scripts. |
| `install_kernel.sh` | board | Install bundled Image/dtbs/modules to the board boot partition. |
| `board_setup.sh` | board | Legacy i.MX95 bring-up entry: load `eth2can.ko` and bring up 6 channels. |

## Typical Internal Flow

```sh
cd tools/imx95
./build_kernel.sh prepare
./build_driver.sh
BOARD_IP=192.168.x.x ./deploy.sh drv eth1
```

For customer bring-up, do not use these scripts.
