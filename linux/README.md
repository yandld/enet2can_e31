# eth2can Linux Driver Installation and Test

[中文说明](README.zh-CN.md)

`eth2can.ko` exposes the six MCXE31B CAN FD channels as standard SocketCAN
devices, `eth2can0` through `eth2can5`. Normal users only need to install the
driver, confirm the Ethernet/E2CF heartbeat, run a basic CAN transmit check,
and run `canperf` when acceptance evidence is required.

Default CAN FD configuration:

```text
bitrate  1000000
dbitrate 5000000
fd       on
```

## Requirements

- A Linux distribution or BSP kernel with SocketCAN enabled:
  `CONFIG_CAN`, `CONFIG_CAN_RAW`, and `CONFIG_CAN_DEV`
- A kernel build tree matching the running `uname -r`, usually
  `/lib/modules/$(uname -r)/build`
- Common tools: `make`, `gcc`, `kmod`, `iproute2`, and `can-utils`

Raspberry Pi, Ubuntu/Debian, RK/i.MX vendor Linux, and similar systems use the
same installer. If the vendor BSP does not provide standard headers, pass the
matching kernel build directory through `KDIR=`.

## One-command installation

Run from the repository root:

```sh
sh linux/scripts/install_driver.sh
```

The script builds and loads `eth2can.ko`, waits for the MCXE31B heartbeat, and
configures `eth2can0..5` for CAN FD 1M/5M. It does not install DKMS, write
systemd units, or configure persistent module autoload.

Common options:

```sh
sh linux/scripts/install_driver.sh --ifname eth1
sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname end0
sh linux/scripts/install_driver.sh --dry-run
sh linux/scripts/install_driver.sh --no-load
sh linux/scripts/install_driver.sh --require-gateway
```

Explicit bitrate override:

```sh
sh linux/scripts/install_driver.sh --bitrate 1000000 --dbitrate 5000000
```

## Manual commands

When debugging installer failures, load and configure the driver manually:

```sh
sudo insmod linux/eth2can.ko ifname=eth0
sudo ip link set eth0 up
sudo ip link set eth2can0 type can bitrate 1000000 dbitrate 5000000 fd on
sudo ip link set eth2can0 up
```

Before basic traffic tests, check that each CAN FD bus has 120 ohm termination
at both ends, correct CANH/CANL polarity, powered transceivers, and common
ground.

Default harness:

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

Basic transmit check:

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

## Performance test entry point

```sh
cd linux/can_testcase
make
./canperf latency --count 10000
./canperf bandwidth
```

For acceptance, use the final `RESULT` line. `latency` reports end-to-end
p50, p99, and p99.9. `bandwidth` reports bidirectional MSR, the maximum
sustainable rate. `zero_loss=yes` means no application-level loss for the run.
`counters=clean` means readable driver and gateway counters did not increase
for loss, overflow, reject, or send-failure conditions. This repository does
not define fixed p99 or MSR limits.

## Troubleshooting

| Symptom | Checks |
|---|---|
| `/lib/modules/$(uname -r)/build` is missing | Install headers matching the running kernel, or pass `KDIR=` |
| `vermagic` mismatch | Rebuild `eth2can.ko` on the target kernel |
| No `eth2can0..5` devices | Check `dmesg` for insmod errors and verify SocketCAN kernel config |
| `gateway heartbeat was not observed` | Check cable, selected `--ifname`, MCXE31B firmware, switch path, and VLAN |
| `ip link set eth2canN ...` times out | Heartbeat is not established or the gateway did not answer CFG |
| CAN frame is not received | Check default harness, termination, transceiver power, common ground, CANH/CANL polarity, and bitrate |

LED hints:

| LED | State | Direction |
|---|---|---|
| SYS | About 1 Hz blinking | Firmware superloop is running |
| SYS | Not blinking | Check firmware image, power, and reset first |
| NET | Off | PHY link down; check cable, switch, and host interface state |
| NET | Blinking | PHY link up but E2CF peer heartbeat is not ready |
| NET | Solid on | Ethernet/E2CF link is ready |
| CAN | Blinking | CAN RX/TX traffic is active |
| CAN | Solid on | At least one active CAN channel is error-passive or bus-off |

Heartbeat capture:

```sh
sudo tcpdump -eni eth0 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'
```

Expected traffic is an EtherType `0x88B5` heartbeat every 100 ms. If tcpdump
sees traffic but the driver never reports `gateway alive`, save the packet
capture and `dmesg` for support.
