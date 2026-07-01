#!/bin/sh
# board_setup.sh - run ON the iMX95 board: load eth2can and bring up 6 ch.
# Works standalone (U-disk / manual scp case); deploy.sh drv does the same.
#
#   ./board_setup.sh [eth-if]     # default eth0
#
# SPDX-License-Identifier: GPL-2.0
set -e
ETHIF="${1:-eth0}"
cd "$(dirname "$0")"

echo ">>> kernel: $(uname -r)"
# .ko lookup, relative to this script: same dir for bundle/manual copy, or
# ../../linux/eth2can.ko for the repository layout.
KO=./eth2can.ko
[ -f "$KO" ] || KO=../../linux/eth2can.ko
[ -f "$KO" ] || { echo "ERROR: eth2can.ko not found in $(pwd) or ../../linux"; exit 1; }
echo ">>> module: $KO"

# Old and new kernels share the same release string (same source tree), so
# vermagic alone cannot tell them apart - check the live config instead.
if [ -r /proc/config.gz ]; then
    if ! zcat /proc/config.gz | grep -q '^CONFIG_CAN_RAW=y'; then
        echo "WARNING: this kernel does NOT have CAN built-in (=y) - the E2CF"
        echo "         bundle Image is not what booted. install_kernel.sh likely"
        echo "         wrote to the wrong partition; re-run it and check that it"
        echo "         picked <bootdisk>p1, then reboot. Continuing with =m..."
    fi
fi

# vermagic guard: insmod of a mismatched .ko fails cryptically - say it clearly
KVER=$(uname -r)
MODVER=$(modinfo "$KO" 2>/dev/null | sed -n 's/^vermagic: *\([^ ]*\).*/\1/p')
if [ -n "$MODVER" ] && [ "$MODVER" != "$KVER" ]; then
    echo "ERROR: module built for '$MODVER' but board runs '$KVER'"
    echo "       rebuild with build_driver.sh against the matching tree, or"
    echo "       deploy the matching kernel first (deploy.sh kernel)."
    exit 1
fi

# CAN core deps are BUILT-IN (=y) in the bundled kernel; these modprobes are
# no-ops there and only matter as a fallback on an older =m kernel.
modprobe can_dev 2>/dev/null || true
modprobe can 2>/dev/null || true
modprobe can_raw 2>/dev/null || true

rmmod eth2can 2>/dev/null || true
ALIVE0=$(dmesg | grep -c 'eth2can: gateway alive' || true)
insmod "$KO" ifname="$ETHIF"

ip link set "$ETHIF" up 2>/dev/null || true

# Wait for the first gateway HB (peer lock) so the CFG transactions below go
# unicast - pre-lock multicast CFG may not traverse every switch. Compare
# against the pre-insmod count so an "alive" line from a previous load
# doesn't satisfy the wait. HB period 100 ms; allow 2 s (PHY autoneg).
n=0
while [ $n -lt 20 ]; do
    [ "$(dmesg | grep -c 'eth2can: gateway alive' || true)" -gt "$ALIVE0" ] && break
    sleep 0.1 2>/dev/null || sleep 1
    n=$((n + 1))
done

ok=0
for i in 0 1 2 3 4 5; do
    if ip link set "eth2can$i" type can bitrate 1000000 dbitrate 5000000 fd on 2>/dev/null &&
       ip link set "eth2can$i" up 2>/dev/null; then
        ok=$((ok + 1))
    fi
done

echo ">>> $ok/6 channels up (UP needs the MCXE gateway answering CFG):"
ip -br link show type can 2>/dev/null || ip link show | grep eth2can
echo ">>> driver log:"
dmesg | grep -i e2cf | tail -8 || true
echo ">>> quick test:  candump eth2can0 &   cansend eth2can0 123##300DEADBEEF"
