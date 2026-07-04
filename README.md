# MCXE31B Ethernet-to-Six-CAN FD Bridge

[中文说明](README.zh-CN.md)

This repository delivers an MCXE31B-based Ethernet-to-six-CAN FD bridge.
The Linux host loads `eth2can.ko` and sees six standard SocketCAN devices,
`eth2can0` through `eth2can5`. Applications can keep using normal CAN tools
such as `ip link`, `candump`, `cansend`, and higher-level SocketCAN software.

The default CAN FD configuration is:

```text
nominal bitrate: 1 Mbit/s
data bitrate:    5 Mbit/s
CAN FD BRS:      enabled
```

## What is included

| Path | Purpose |
|---|---|
| `source/` | MCXE31B bridge firmware source |
| `board/` | Board clock, pin mux, and low-level board support |
| `linux/` | Linux SocketCAN driver, installer, and driver tests |
| `linux/can_testcase/` | `canperf` latency and bidirectional bandwidth tests |
| `docs/*.md` | Design notes for E2CF, Linux driver, firmware, and EQOS TX handling |

## System overview

The bridge uses raw Layer-2 Ethernet between Linux and the MCXE31B gateway.
The Linux driver exposes six SocketCAN interfaces and exchanges E2CF frames
with the firmware. The firmware forwards traffic to FlexCAN0 through FlexCAN5.

Key defaults:

| Item | Value |
|---|---|
| Ethernet protocol | Raw Layer-2 E2CF, EtherType `0x88B5` |
| Deployment VLAN | VID `100`; untagged frames are allowed during bring-up |
| Linux interfaces | `eth2can0` through `eth2can5` |
| CAN channels | FlexCAN0 through FlexCAN5 |
| TX window | 16 echo slots per channel |
| MCU-to-Linux aggregation | Count based or 50 us timer |
| Heartbeat | 100 ms period, 500 ms timeout |

## Hardware setup

Use six external CAN FD transceivers. For the default test harness, connect
three independent CAN FD buses:

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

Each CAN FD bus needs 120 ohm termination at both ends. Verify CANH/CANL
polarity, transceiver power, and a common ground between the host setup and
the gateway board.

Status LEDs:

| LED | GPIO | State | Meaning |
|---|---|---|---|
| SYS | PTC16 | About 1 Hz blinking | Firmware superloop is running |
| SYS | PTC16 | Solid on | Firmware is in a fault or fatal path |
| NET | PTB22 | Off | PHY link is down |
| NET | PTB22 | Blinking | PHY link is up, E2CF heartbeat is not ready yet |
| NET | PTB22 | Solid on | Ethernet/E2CF link is ready |
| CAN | PTC14 | Off | No CAN traffic |
| CAN | PTC14 | Blinking | CAN RX/TX traffic is active |
| CAN | PTC14 | Solid on | At least one active CAN bus is error-passive or bus-off |

## Firmware

If the board is already programmed, power it up and continue with the Linux
driver setup. To build the firmware yourself, open `e2cf_mcxe31.uvprojx` in
Keil MDK and build the MCXE31B target. Program the board with your normal
MCXE31B/SWD flow.

## Linux quick start

Run the installer from the repository root on the target Linux host:

```sh
sh linux/scripts/install_driver.sh
```

The script builds `eth2can.ko` for the running kernel, loads it for the
current boot, waits for the gateway heartbeat, and configures `eth2can0..5`
for CAN FD 1M/5M. It does not install DKMS, systemd units, or persistent
autoload configuration.

Common options:

```sh
sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname end0
sh linux/scripts/install_driver.sh --dry-run
sh linux/scripts/install_driver.sh --no-load
sh linux/scripts/install_driver.sh --require-gateway
```

Check the created CAN devices:

```sh
ip -details link show type can
```

## Basic traffic check

With the default harness connected, send one CAN FD+BRS frame from channel 0
to channel 4:

```sh
candump eth2can4 &
cansend eth2can0 123##3001122334455667788
```

If `eth2can4` receives the frame, the Linux driver, Ethernet path, MCXE31B
firmware, and at least one physical CAN FD bus are working.

## Acceptance tests

Build the customer test tool:

```sh
cd linux/can_testcase
make
```

Run latency:

```sh
./canperf latency --count 10000
```

Run bidirectional maximum sustainable bandwidth:

```sh
./canperf bandwidth
```

For acceptance, use the final `RESULT` line first. `PASS` means zero-loss for
that run. Latency reports p50, p99, and p99.9. Bandwidth reports MSR, the
maximum sustainable rate. The `EVIDENCE` line is intended for reproducible
test records; `counters=clean` means the driver and gateway loss, overflow,
reject, and send-failure counters did not increase during the confirmation
round. This repository does not define fixed p99 or MSR limits; thresholds
must be agreed for the target harness, host, and system load.

## Troubleshooting

| Symptom | Checks |
|---|---|
| Kernel build tree is missing | Install headers matching `uname -r`, or pass `KDIR=/path/to/kernel/build` |
| `vermagic` mismatch | Rebuild `eth2can.ko` on the target kernel |
| No `eth2can0..5` devices | Check `dmesg`, SocketCAN kernel config, and module load errors |
| Gateway heartbeat is missing | Check cable, selected `--ifname`, VLAN path, MCXE31B firmware, and EtherType `0x88B5` traffic |
| CAN frame is not received | Check default harness wiring, termination, transceiver power, common ground, polarity, and CAN rate |
| `counters=dirty` | Inspect `seq_lost`, `rx_ovf`, `rej`, `starv`, `sfail`, and `emac_rxdrop` deltas |

Heartbeat capture:

```sh
sudo tcpdump -eni eth0 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'
```

Expected traffic is an E2CF heartbeat every 100 ms.

## Documentation map

- Linux driver installation and troubleshooting: [`linux/README.md`](linux/README.md)
- `canperf` build and result interpretation: [`linux/can_testcase/README.md`](linux/can_testcase/README.md)
- Protocol and implementation design notes: [`docs/`](docs/)

## License and distribution

This repository is not published under an open-source license in its current
form. Copying, redistribution, product use, or publication outside the
authorized project scope requires written permission from the repository owner.
Some source files also carry their own SPDX notices; those notices must be
preserved.
