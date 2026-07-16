# canperf Customer Test Tool

[中文说明](README.zh-CN.md)

`canperf` answers two customer acceptance questions for the MCXE31B bridge:

- Latency: `latency`
- Bidirectional maximum sustainable bandwidth: `bandwidth`

The default CAN FD rate is 1M/5M with BRS enabled. The default harness is
`0<->4`, `1<->2`, and `3<->5`.

## Build

Native build on the target Linux host:

```sh
make
```

Use an explicit compiler:

```sh
make CC=gcc
make CC=clang
```

Explicit aarch64 cross build:

```sh
make cross
make cross TOOLCHAIN_DIR=/opt/arm-gnu-toolchain
```

## Wiring

```text
eth2can0 <-> eth2can4
eth2can1 <-> eth2can2
eth2can3 <-> eth2can5
```

Each pair is one independent CAN FD bus. Use 120 ohm termination at both ends,
keep CANH/CANL polarity correct, power the transceivers, and share ground with
the gateway. Before running `canperf`, load the driver and configure 1M/5M with
`linux/scripts/install_driver.sh`.

## Test model

`canperf` sends and receives on Linux SocketCAN devices. The tested path
includes the Linux `eth2can` driver, Ethernet link, MCXE31B gateway, and
physical CAN FD buses.

`latency` uses the default directions `0->4`, `1->2`, and `3->5`. Each
direction uses one thread and keeps only one CAN frame in flight to measure
end-to-end latency.

`bandwidth` mirrors the harness into six concurrent directions:

```text
0->4  4->0
1->2  2->1
3->5  5->3
```

Each direction uses an independent thread, SocketCAN socket, CAN ID, and
incrementing sequence. The bandwidth test is a controlled rate stress test,
not random traffic. It sends 64-byte CAN FD+BRS frames at a fixed cadence and
keeps at most 16 frames in flight per direction, matching the driver TXC
window depth.

## Latency

```sh
./canperf
./canperf latency
./canperf latency --count 10000
./canperf latency --pair 0:4
./canperf latency --duration 10m
```

Output interpretation:

- The final `RESULT latency` line is the customer conclusion; `PASS` means
  zero application loss with clean counters. A run whose counters cannot be
  read (or whose gateway STATS are stale) is reported `FAIL`, never a false
  pass.
- `total` p50, p99, and p99.9 are the main end-to-end latency numbers.
- p99.9 is also called p999.
- `L1/L2/L3/L4` help localize latency to Linux TX, Ethernet, MCU/CAN, or
  Linux RX.
- `EVIDENCE latency` is intended for reproducible records.
- `counters=clean` means driver and gateway loss, overflow, reject, and
  failure counters did not increase during the run.

## Bidirectional bandwidth

```sh
./canperf bandwidth
./canperf bandwidth --pair 0:4
./canperf bandwidth --count 30000
```

`bandwidth` first sweeps candidate rates, then runs one final confirmation.
For acceptance, only use `RESULT bandwidth`, `MSR`, and `EVIDENCE bandwidth`
from the final confirmation. Do not treat sweep probes as acceptance results.

Result interpretation:

- `RESULT bandwidth: PASS` is the required first condition.
- `MSR = ... fps/pair DELIVERED` is the sustainable frame rate per direction.
- `aggregate` is the sum of all concurrent directions.
- `per CAN bus` shows physical CAN bus utilization.
- `ceiling` indicates whether the observed limit looks CAN-bus-bound or
  gateway/host-path-bound.
- `EVIDENCE bandwidth` is the compact line to copy into reports.

## Common options

```sh
--pair A:B      Select one harness pair; repeat or comma-separate for more pairs
--count N       Frame count for latency; per-step frame count for bandwidth
--duration T    Runtime for latency; per-step runtime for bandwidth
--bitrate R     Nominal bitrate, default 1000000
--dbitrate R    CAN FD data bitrate, default 5000000
--no-setup      Do not configure eth2canN automatically
--help          Show help
```

The customer-facing test scope is CAN FD+BRS. Classic CAN, loopback, CSV
export, and manual sweep/window tuning are not the default acceptance
interfaces.
