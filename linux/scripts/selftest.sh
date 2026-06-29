#!/bin/sh
# selftest.sh - one-shot real-hardware acceptance for mcxe31b-canbridge.
# Builds, brings up vcan, starts the bridge, exercises host->CAN. Pure C + can-utils,
# no python. Emits ONE pasteable evidence block.
#
#   sudo ./scripts/selftest.sh <board-ip>
#
# Run as root (the bridge binds AF_CAN raw and setup-vcan touches ip link).
set -u

BOARD="${1:-192.168.8.113}"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$HERE"

[ "$(id -u)" = 0 ] || { echo "run as root: sudo $0 $BOARD" >&2; exit 1; }
sep() { echo "----- $* -----"; }
echo "===== mcxe31b-canbridge selftest  board=$BOARD ====="

sep "1. BUILD (the real compile test)"
if command -v gcc >/dev/null 2>&1; then CC=gcc; else CC=cc; fi
make CC="$CC" all
rc=$?
echo "make exit=$rc"
if [ ! -x ./canbridge ] || [ "$rc" != 0 ]; then
    echo ">>> BUILD FAILED -- paste everything above this line."
    exit 1
fi
make check-proto 2>&1 || echo "(check-proto needs the firmware header at ../../source; skip if absent)"

sep "2. KERNEL CAPABILITIES"
uname -a
zcat /proc/config.gz 2>/dev/null | grep -E 'CONFIG_CAN(_VCAN|_RAW|_DEV)?=' \
    || echo "(no /proc/config.gz; checking modules) $(ls /lib/modules/$(uname -r)/kernel/drivers/net/can/vcan.ko* 2>/dev/null || echo 'vcan.ko not found')"
command -v candump >/dev/null 2>&1 || echo "WARNING: can-utils not installed (apt-get install can-utils)"

sep "3. BRING UP vcan vcan-gw0..vcan-gw5 @ mtu 72"
./scripts/setup-vcan.sh
ip -d link show vcan-gw0 | sed -n '1,3p'

sep "4. BOARD REACHABLE?"
if ping -c2 -W1 "$BOARD" >/dev/null 2>&1; then echo "ping $BOARD OK"; else echo "ping $BOARD FAIL (check cable/IP)"; fi
echo "get_status (first 500 bytes):"
./canbridge_ctl --board "$BOARD" --timeout-ms 1500 get_status | head -c 500; echo

sep "5. START BRIDGE (background)"
./canbridge --board "$BOARD" --stats-ms 1000 vcan-gw0 vcan-gw1 vcan-gw2 vcan-gw3 vcan-gw4 vcan-gw5 >/tmp/canbridge.log 2>&1 &
CB=$!
sleep 1
if ! kill -0 "$CB" 2>/dev/null; then
    echo ">>> BRIDGE EXITED at startup (self-check hard-fail). Reason:"
    cat /tmp/canbridge.log
    exit 1
fi
echo "bridge pid=$CB up; startup self-checks passed (vcan/mtu72/FD_FRAMES OK)"

sep "6. HOST->CAN round trip (cangen vcan-gw0 -> board)"
./canbridge_ctl --board "$BOARD" reset_stats >/dev/null 2>&1
if command -v cangen >/dev/null 2>&1; then
    cangen vcan-gw0 -g 5 -n 200 -L 8 2>/dev/null
    sleep 1
    echo "board counters after 200x cangen vcan-gw0 (expect tunnel.rx_frames ~200):"
    ./canbridge_ctl --board "$BOARD" get_status | tr ',{' '\n\n' \
        | grep -E '"(rx_frames|tx_frames|loss|parse_error|queue_full|drop|rx|tx_done|rx_fifo_overflow)"' | head -24
else
    echo "SKIP: cangen missing"
fi
echo "bridge stats line:"; tail -n 2 /tmp/canbridge.log

sep "7. PRESSURE / LATENCY TEST -- run separately (on-chip CAN loopback, no wiring)"
echo "6ch x 1kHz TX+RX, 64B FD, via on-chip CAN loopback (no bus cabling/termination needed):"
echo "  sudo ./scripts/stress.sh  $BOARD 2000      # throughput/loss; raise rate to find the ceiling"
echo "  sudo ./scripts/latency.sh $BOARD           # per-channel round-trip latency (ping-style)"

kill "$CB" 2>/dev/null
sep "DONE -- paste this whole block back"
