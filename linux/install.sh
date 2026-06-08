#!/bin/sh
# install.sh - build + install mcxe31b-canbridge as an "install once, it just works"
# product. Universal POSIX sh (works without a package manager). Needs root.
#
#   sudo ./install.sh [BOARD_IP]
#
set -eu

BOARD_IP="${1:-}"

[ "$(id -u)" = 0 ] || { echo "run as root: sudo ./install.sh <board-ip>" >&2; exit 1; }

DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$DIR"

# --- compiler ---
if command -v gcc >/dev/null 2>&1; then CC=gcc
elif command -v cc >/dev/null 2>&1; then CC=cc
else echo "ERROR: no C compiler. Install build-essential (apt-get install build-essential)." >&2; exit 1
fi

echo "== build =="
make CC="$CC" all

echo "== capability checks =="
if ! modprobe vcan 2>/dev/null; then
    echo "WARNING: 'vcan' module not loadable. Kernel needs CONFIG_CAN_VCAN=m." >&2
    echo "         check: zcat /proc/config.gz | grep CONFIG_CAN_VCAN" >&2
fi
command -v candump >/dev/null 2>&1 || \
    echo "NOTE: can-utils not installed; pressure tests need it (apt-get install can-utils)."

echo "== sysctl (SO_RCVBUF headroom) =="
cat > /etc/sysctl.d/90-mcxe31b-canbridge.conf <<'EOF'
# Let the bridge's 4 MB SO_RCVBUF request through (avoid silent clamp under burst).
net.core.rmem_max = 8388608
EOF
sysctl -p /etc/sysctl.d/90-mcxe31b-canbridge.conf >/dev/null 2>&1 || true

echo "== install =="
make CC="$CC" install

if [ -n "$BOARD_IP" ]; then
    sed -i "s/^BOARD_IP=.*/BOARD_IP=$BOARD_IP/" /etc/default/mcxe31b-canbridge
    echo "set BOARD_IP=$BOARD_IP"
fi

if command -v systemctl >/dev/null 2>&1; then
    . /etc/default/mcxe31b-canbridge      # use the operator's IFACES, not the default
    # shellcheck disable=SC2086
    /usr/libexec/mcxe31b-canbridge/setup-vcan.sh $IFACES
    systemctl daemon-reload
    # enable + restart (not "enable --now"): on a re-install the service is already
    # running, and "--now" would NOT pick up the rebuilt binary or the changed unit
    # file (e.g. the realtime scheduling). restart always redeploys both.
    systemctl enable mcxe31b-canbridge.service
    systemctl restart mcxe31b-canbridge.service
    echo "service (re)started. Check:  systemctl status mcxe31b-canbridge ;  candump can0"
else
    echo "no systemd detected - start manually after editing /etc/default/mcxe31b-canbridge:"
    echo "  /usr/libexec/mcxe31b-canbridge/setup-vcan.sh"
    echo "  /usr/sbin/canbridge --board <board-ip> can0 can1 can2 can3 can4 can5"
fi

cat <<'EOF'

NOTE: can0..can5 are VIRTUAL (vcan) interfaces. `ip link set canX type can bitrate ...`
does NOT work on them. Set the real CAN bitrate on the BOARD, e.g.:
  canbridge_ctl --board <board-ip> set_can_config channel=0 fd=true bitrate=1000000 data_bitrate=5000000 brs=true
EOF
