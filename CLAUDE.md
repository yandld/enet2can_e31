# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

E2CF gateway: an i.MX95 (Real-Time Edge Linux) talks to an NXP MCXE31B MCU over raw L2
Ethernet (EtherType 0x88B5, no IP/UDP), and the MCU bridges 6× CANFD channels (1M/8M BRS).
Three parts, all in this repo:

- **Root** — MCU firmware (bare-metal Cortex-M7 superloop, no RTOS/lwIP). Project logic in
  `source/`; `board/board.c` + `board/hardware_init.c` are board glue.
- **`linux/`** — customer-facing Linux kernel driver `eth2can.ko` (single file
  `linux/src/eth2can.c`) exposing the 6 channels as standard SocketCAN devices
  `eth2can0..5`, plus the portable installer `linux/scripts/install_driver.sh`.
- **`tools/imx95/`** — internal i.MX95/RTE kernel build, deploy, and bundle scripts.
- **`eth2can_design/`** — three Chinese design docs. `01_E2CF协议规范.md` is the **normative
  protocol spec**; its §9 conformance checklist is binding (deviating code is a spec violation,
  not an implementation choice). 02 = Linux side, 03 = MCXE side. The root `README.md` has a
  table of recorded v1-vs-design deviations — update it when closing one.

**Shared wire format**: `source/e2cf_proto.h` and `linux/src/e2cf_proto.h` are byte-identical
copies with **no sync automation** — any wire-format change must be applied to both files (and
both implementations). Keep the header free of SDK/kernel includes (only the `__KERNEL__` gate).
The protocol is deliberately little-endian packed structs overlaid on wire buffers — do not add
byte-order conversion. DATA record head is layout-congruent with `canfd_frame[0..7]`.

## Build commands

### MCU firmware (root)

```sh
make                # armclang; default AC6=/opt/ArmCompilerforEmbedded6.24
make clean          # outputs: build/e2cf_mcxe31.{axf,bin,map}
```

Requires Arm Compiler for Embedded 6 (armclang/armlink/fromelf). `armgcc/` is empty — there is
no GCC/CMake build. Alternative: Keil MDK project `e2cf_mcxe31.uvprojx` (same compiler/scatter).

Both builds force-include `source/mcux_config.h` + `source/mcuxsdk_version.h` (Keil via
MiscControls, Makefile via `-include` in CFLAGS) — this defines `FSL_ETH_ENABLE_CACHE_CONTROL`,
which compiles EMAC RX cache invalidation into `fsl_enet_qos.c`; do not drop it (D-cache is on
and RX buffers live in cacheable SRAM). Remaining Makefile/Keil divergence is non-functional:
Keil assembles `utilities/fsl_memcpy.S` (CLI uses libc memcpy) and uses a higher -O level.

No automated tests. Runtime verification = UART debug console @115200 (single keys: `0..5` log
level, `m` module mask, `s` dump stats, `?` help). The periodic UART stats line is **off by
default** (`E2CF_STATS_PERIOD_MS=0`; set e.g. 3000 for standalone bench bring-up); stats are
instead pushed in-band at 1 Hz via `E2CF_MSG_STATS`. Bring-up knobs in `source/e2cf_config.h`:
`E2CF_AUTOSTART_CHANNELS=1`
(default, auto-START all channels and disarm safe state until first HB),
`E2CF_AUTOSTART_LOOPBACK=1` (transceiver-less self-test; requires TDC off — already handled).

### Linux driver

Customer-facing install path:

```sh
sudo sh linux/scripts/install_driver.sh --ifname eth0
```

The installer builds `eth2can.ko` against the running kernel, loads it for the
current boot, and configures channels when the MCXE31B gateway heartbeat is
visible. It does not require a custom i.MX95 kernel flow.

i.MX95/RTE internal tools live in `tools/imx95/`. Those scripts source
`tools/imx95/env.sh`; every variable is environment-overridable (`KSRC` = RTE
6.18 kernel tree at `../../../real-time-edge-linux`, `KOUT`, `TOOLCHAIN_DIR` =
aarch64-none-linux-gnu 14.3, `BOARD_IP`/`BOARD_USER`/`BOARD_DIR`). Do **not**
source the Yocto SDK env script for kernel/module builds — its sysroot cflags
break kbuild.

```sh
cd tools/imx95
./build_kernel.sh prepare          # first time: defconfig + CAN built-in (=y) + modules_prepare
./build_driver.sh                  # cross-compile eth2can.ko (auto-prepares if needed)
./build_driver.sh host             # build against host kernel — quick API/syntax check
./build_kernel.sh                  # full kernel: Image + dtbs + modules
BOARD_IP=x.x.x.x ./deploy.sh drv eth1   # scp + insmod + bring up 6 channels 1M/8M
BOARD_IP=x.x.x.x ./deploy.sh kernel     # scp Image + imx95 dtb to board boot partition
./make_bundle.sh                   # minimal tgz: Image+ko+scripts (CAN/ENETC/MMC are =y;
                                   #   `full` arg adds dtbs+modules for first flash)
```

After only `prepare` there is no `Module.symvers`; `build_driver.sh` auto-passes
`KBUILD_MODPOST_WARN=1` (the .ko still insmods). `board_setup.sh` refuses insmod on vermagic
mismatch. Driver must keep building on both 5.15 (host check) and 6.9+/6.18 (target) — preserve
the `KERNEL_VERSION(6,9,0)` ifdefs around `can.fd.data_bittiming` vs `can.data_bittiming`.

On-board usage: `insmod eth2can.ko ifname=eth0 [vid=100] [peer=...]`, then standard iproute2
(`ip link set eth2can0 type can bitrate 1000000 dbitrate 8000000 fd on`). `vid=-1` (untagged) is
the bring-up default; the spec requires 802.1Q (VID 100, PCP 6 data / PCP 2 CFG) in deployment.

## Architecture

### Protocol essentials (spec doc 01)

Message types: DATA (≤17 records/frame), TXC, EVT, CFG_REQ/CFG_RSP, TIME, HB. Flow control is
**TXC only**: per-channel sliding window of 16 (`E2CF_WIN_DEPTH` ≡ MCU `E2CF_SW_TXFIFO_DEPTH` ≡
Linux `echo_skb_max` — these must stay equal); Linux stops the queue when full, a TXC releases
the slot. CFG is token-matched request/response (10 ms timeout ×3 retries, MCU idempotent via
token cache). HB every 100 ms both ways; 500 ms silence → Linux carriers off all 6 devs, MCU
enters safe state (stops all CAN TX). Data plane never retransmits or reorders — only counts
seq gaps. Aggregation: MCU→Linux T_agg = 50 µs is mandatory; a partial frame is sealed only by
the T_agg deadline, the 17-record cap, or face overflow (the old "egress-idle sends immediately"
rule was removed — it defeated batching).

### MCU firmware (`source/`)

`e2cf_core.c` is the only glue module. `can_hw.c` and `eth_raw.c` never include `e2cf_core.h` or
each other — each declares the upcalls it invokes in its *own* header (`e2cf_core_can_rx/...`,
`e2cf_core_eth_frame`) and `e2cf_core.c` implements them. `gw_time` (DWT@160 MHz 64-bit
timebase) and `dbg_log` are leaf utilities.

The data plane runs entirely in ISRs; the superloop only handles deadlines and slow paths:

- **CAN→ETH (CAN ISR, NVIC prio 1)**: all six instances read their RX MB banks and insertion-sort
  by 16-bit timestamp (with IRMQ, MB index order ≠ arrival order). CAN0's Enhanced RX FIFO is
  parked: enabled, its RX interrupt never fired on this silicon (flags appear to route to the
  unserved MB32-63 line) — Phase 2 must fix the line routing before reviving eFIFO+eDMA. Records
  append into DTCM aggregation "faces"; flush on 17 records / face full / 50 µs T_agg deadline.
- **ETH→CAN (EMAC ISR, prio 1 — same level as CAN, no mutual preemption)**: zero-copy EQOS RX → demux → DATA records into per-channel
  16-deep `sw_txfifo` feeding a **single active TX MB** (strict per-channel ordering — multiple
  TX MBs would arbitrate by CAN ID and reorder; spec §6.3). CFG ops (including full
  `FLEXCAN_Init` / freeze-mode work) execute synchronously inside this ISR.
- **Superloop (`main.c`)**: `e2cf_core_poll()` every iteration (T_agg deadline, HB/EVT/TIME
  periodics, safe-state arming, face reclaim); `can_hw_poll_errors()` every 10 ms (error state
  machine + 100 ms stuck-TX watchdog); link poll 100 ms; `dbg_log_drain()` + UART console.

Concurrency primitive is exclusively short global PRIMASK critical sections (state is shared
across CAN ISR / EMAC ISR / main loop). **TXC contract**: every accepted DATA record gets exactly
one TXC (bus completion, submit-time reject, STOP flush, or watchdog CTRL_ERROR) — Linux window
slots leak otherwise. v1 intentionally returns ENOTSUP for hardware filters, one-shot mode, and
non-ISO FD rather than silently accepting — keep that. Frame-type validation is normative (spec
§4.3): FDF records on a classical-only channel and classical records with len>8 or BRS are
rejected with TXC CTRL_ERROR, never truncated. ISR code must log via `LOG_*` (deferred ring
buffer) only; `PRINTF` is main-loop/fault-handler only.

### Linux driver (`linux/src/eth2can.c`)

gs_usb-style multi-channel mux: 6× `alloc_candev` sharing one `struct e2cf_dev`. RX via
`dev_add_pack(0x88B5)` — only E2CF frames are intercepted, all other traffic flows normally
through the unmodified NIC driver. TX via `dev_queue_xmit`. `ndo_start_xmit` claims a slot in a
16-bit `echo_busy` bitmap (slot index = wire `tag`); TXC matches it back via
`can_get_echo_skb`. `ndo_open` = STOP→SET_BITRATE→START CFG transaction (sleepable, serialized
by `cfg_lock`); RX handlers run in softirq context — keep GFP_ATOMIC/spinlock discipline.
Carrier state is driven solely by gateway HBs. Phase 2 (per design doc 02) replaces only the
transport with `enetc4_ecat_fast_*_k` variants — protocol logic stays.

### Memory map & board (firmware)

- Scatter file `MCXE31B/mdk/MCXE31B_flash.scf`. Boot header IVT at flash 0x0040_0000
  (`boot_header/boot_header.c`, `BOOT_HEADER_ENABLE=1`) is mandatory — the SBAF boot ROM needs it
  to find the vector table at 0x0040_1000; removing it = non-booting image.
- First 16 KB of DTCM = non-cacheable region holding EQOS descriptor rings + the 8×1536 B
  zero-copy TX faces (`E2CF_ETH_TXFACE_NUM`, raised 6→8). MPU region sizing in `board.c` is computed from the
  `RW_m_ncache`/`RW_m_ncache_unused` linker symbols — keep those region names. EMAC RX buffers
  live in cacheable SRAM and rely on driver invalidation (`FSL_ETH_ENABLE_CACHE_CONTROL`).
- Stack is deliberately 8 KB (SDK default 0x400 overflowed in the donor project) — don't shrink.
- **FlexCAN PE clock is 80 MHz at runtime** (attached to AIPS_PLAT_CLK in
  `board/hardware_init.c`), even though the generated YAML comments in `board/clock_config.c`
  still say 48 MHz FIRC. Don't "fix" bit timing from the YAML; `can_hw.c` queries
  `CLOCK_GetFreq()` so it tracks the real attachment. 80 MHz is required for 8 Mbps FD.
- `board/pin_mux.c` carries a "generated, edits will be overwritten" banner but
  `BOARD_InitFlexCANPins` was hand-edited — re-running MCUXpresso Config Tools will clobber it.

### Vendor-code boundary

`drivers/`, `component/`, `utilities/`, `device/`, `CMSIS/`, `startup/`, `boot_header/` are NXP
MCUXpresso SDK code — configure via `source/mcux_config.h` / `source/e2cf_config.h` rather than
editing. ONE recorded exception: the `E2CF WORKAROUND` block in `drivers/fsl_enet_qos.c`
`ENET_QOS_SendFrame` (TX ring-wrap tail pointer; wire-proven frame-skip/retransmit anomaly —
see `eth2can_design/eqos_tx_ring_wrap_report.md` and the README deviation table). Do not add
further SDK edits; revert this one when the SDK ships an official fix. Project-owned code is `source/`, `board/board.c`, `board/hardware_init.c`, the Makefile,
and everything under `linux/` (sources: `src/`, `Makefile`, `scripts/` — the `.ko`/`.o`/`.tgz`
files sitting in `linux/` are build artifacts).
