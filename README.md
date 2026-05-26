# FRDM-MCXE31B 6x CAN FD Bring-up

This project is the current bring-up baseline for a customer-facing 6CANFD to Ethernet reference design on NXP MCXE31B.

Current firmware scope:

| Area | Status |
|---|---|
| FlexCAN 0 / 1 / 2 / 3 / 4 / 5 | Enabled as six independent channels |
| Default CAN mode | CAN FD with 1 Mbps arbitration and 5 Mbps data phase |
| Classic CAN compatibility | Compile-time fallback through `CAN_USE_CANFD = 0` |
| Debug console | LPUART5 only |
| Ethernet | Pins and clocks only; Ethernet bridge is not complete |
| Linux SocketCAN | Userspace UDP bridge starter under `tools/` |

The first Ethernet transport target is a UDP raw frame gateway. TCP, Modbus TCP, CANopen over Ethernet, PTP/1588, DHCP, and remote update are later evaluation items, not part of the current firmware baseline.

---

## Hardware Platform

| Item | Value |
|---|---|
| Board | FRDM-MCXE31B |
| MCU | NXP MCXE31B, ARM Cortex-M7, 160 MHz |
| SDK base | MCUXpresso SDK 2025.12.00 style project |
| Toolchain | Keil MDK / ARM Compiler 6 |

---

## CAN Defaults

| Parameter | Default |
|---|---|
| Channels | 6 |
| Arbitration bitrate | 1 Mbps |
| Data phase bitrate | 5 Mbps |
| CAN FD | Enabled |
| BRS | Enabled |
| FD payload | 64 bytes, DLC 15 |
| Classic payload fallback | 8 bytes |
| TX period | 1000 ms |
| TX timeout | 200 ms |
| Self reception | Disabled |

Transmit IDs:

| Channel | TX ID |
|---|---|
| CAN0 | `0x100` |
| CAN1 | `0x101` |
| CAN2 | `0x102` |
| CAN3 | `0x103` |
| CAN4 | `0x104` |
| CAN5 | `0x105` |

---

## Pin Assignment

CAN pins follow `MCXE31B_IOMUX.xlsx` and `board/pin_mux.c`. The compiled project uses `board/pin_mux.c`; `.mex` was not regenerated for the latest staged CAN expansion.

| Channel | Peripheral | TX pin | TX package pin | RX pin | RX package pin |
|---|---|---:|---:|---:|---:|
| CAN0 | FLEXCAN_0 | PTA7 | 100 | PTA6 | 102 |
| CAN1 | FLEXCAN_1 | PTA11 | 160 | PTA12 | 159 |
| CAN2 | FLEXCAN_2 | PTE24 | 157 | PTE25 | 158 |
| CAN3 | FLEXCAN_3 | PTC28 | 96 | PTC29 | 99 |
| CAN4 | FLEXCAN_4 | PTC30 | 101 | PTC31 | 103 |
| CAN5 | FLEXCAN_5 | PTC27 | 93 | PTC26 | 91 |

### Debug Console

| Function | TX | RX | Baudrate |
|---|---:|---:|---:|
| Debug UART, LPUART5 | PTE14 | PTE3 | 115200 |

---

## Configuration

Main CAN parameters are in `source/main.c`:

```c
#define CAN_CHANNEL_COUNT       6U
#define CAN_BITRATE             1000000U
#define CAN_USE_CANFD           1
#define CAN_FD_BITRATE          5000000U
#define CAN_CLASSIC_DLC         8U
#define CAN_FD_DLC              15U
```

The UDP gateway packet contract is defined in `source/can_gateway_protocol.h`:

```c
#define CAN_GATEWAY_UDP_DATA_PORT 50000U
#define CAN_GATEWAY_UDP_STATUS_PORT 50001U
```

---

## Expected Debug Output

Startup starts with a channel summary:

```text
========================================
  6x CAN FD Bring-up  -  MCXE31B
========================================
  CAN0: 1000kbps (arb) / 5000kbps (data)  CAN FD  TX=0x100  period=1000ms
  ...
  RX: all standard frames
  UDP gateway protocol: magic=0x43474644 data_port=50000 status_port=50001
========================================
```

Runtime logs include TX starts, RX frames, timeouts, and low-rate status counters:

```text
[CAN3] TX start  id=0x103  data=0x5
[CAN4] RX  id=0x123  len=64  data: ... ts=1234
[CAN3] TX timeout, abort pending frame
[CAN3] status rx=0 tx_start=5 tx_done=0 tx_err=5 tx_timeout=5 rx_err=0 last=0x0
```

TX start only means the non-blocking transfer was queued. It does not prove bus ACK.

---

## Linux Bridge Starter

`tools/socketcan_udp_bridge.py` is the first userspace bridge target. It maps `can0..can5` to the UDP raw frame protocol so the Linux side can keep using SocketCAN tools such as `candump`, `cansend`, and `cangen`.

Example:

```bash
python3 tools/socketcan_udp_bridge.py --remote-host 192.168.0.10 --can can0 can1 can2 can3 can4 can5
```

This script is a protocol/host-side starting point. It requires Linux SocketCAN interfaces and does not make the MCU Ethernet firmware complete by itself.

---

## Build

Keil target:

```text
enet2can_e31 debug
```

Command-line rebuild:

```powershell
& 'C:\Keil_v5\UV4\UV4.exe' -b 'enet2can_e31.uvprojx' -t 'enet2can_e31 debug' -o 'debug\codex_build.log'
```

---

## Known Limits

- Ethernet bridge is not complete. The repository currently has EMAC pin/clock setup and protocol boundaries, but no ENET/lwIP driver integration.
- DHCP, TCP server/client, multicast, Modbus TCP, CANopen over Ethernet, PTP/1588, and remote update are not implemented.
- The current MCU code still logs through `PRINTF`, so it is not the final high-load 80% CANFD stress path.
- Hardware CAN termination policy remains a customer board decision; the requirement asks for per-channel controllable termination, but this firmware cannot add hardware switching if the board does not expose it.
