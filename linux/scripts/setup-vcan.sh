#!/bin/sh
# setup-vcan.sh - create + bring up vcan interfaces (default vcan-gw0..5) at CAN-FD MTU.
# Idempotent. Needs root. CANFD_MTU is 72; without it FD frames are silently dropped.
set -eu

MTU=72
IFACES="${*:-vcan-gw0 vcan-gw1 vcan-gw2 vcan-gw3 vcan-gw4 vcan-gw5}"

if ! ip link show type vcan >/dev/null 2>&1 && ! lsmod 2>/dev/null | grep -q '^vcan'; then
    if ! modprobe vcan 2>/dev/null; then
        echo "ERROR: cannot load 'vcan' (CONFIG_CAN_VCAN)." >&2
        echo "  check: zcat /proc/config.gz | grep CONFIG_CAN_VCAN" >&2
        echo "         ls /lib/modules/\$(uname -r)/kernel/drivers/net/can/vcan.ko*" >&2
        exit 1
    fi
fi

for i in $IFACES; do
    if ! ip link show "$i" >/dev/null 2>&1; then
        ip link add dev "$i" type vcan
    fi
    ip link set "$i" mtu "$MTU"
    ip link set "$i" up
done

echo "vcan ready: $IFACES (mtu $MTU)"
