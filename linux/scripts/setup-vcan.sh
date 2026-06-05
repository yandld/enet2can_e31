#!/bin/sh
# setup-vcan.sh - create + bring up vcan interfaces named can0..canN at CAN-FD MTU.
# Idempotent. Needs root. CANFD_MTU is 72; without it FD frames are silently dropped.
set -eu

MTU=72
IFACES="${*:-can0 can1 can2 can3 can4 can5}"

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
