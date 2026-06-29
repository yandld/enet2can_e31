#!/bin/sh
# install.sh - build + install mcxe31b-canbridge as an "install once, it just works"
# product. Universal POSIX sh (works without a package manager). Needs root.
#
#   sudo ./install.sh [BOARD_IP] [--prefix NAME]
#
# Interfaces default to vcan-gw0..vcan-gw5 (virtual CAN) so they never collide with
# a customer's existing physical can0/can1. --prefix NAME overrides the names to
# NAME0..NAME5 (e.g. if even vcan-gw* is already taken).
set -eu

# BOARD_IP is positional (back-compat); --prefix is optional.
BOARD_IP=""
IFACE_PREFIX=""
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) shift; IFACE_PREFIX="${1:-}"
                  [ -n "$IFACE_PREFIX" ] || { echo "--prefix needs a value" >&2; exit 1; } ;;
        -h|--help) echo "usage: sudo ./install.sh [BOARD_IP] [--prefix NAME]"; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 1 ;;
        *) BOARD_IP="$1" ;;
    esac
    shift
done

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

# Interface names are install-driven (no backward-compat with a stale config): ALWAYS
# (re)assert them, so re-installing a host that had the old can0..can5 just replaces
# them with the safe default. --prefix NAME overrides (-> NAME0..NAME5).
IFACE_PREFIX="${IFACE_PREFIX:-vcan-gw}"
printf '%s' "$IFACE_PREFIX" | grep -qE '^[a-zA-Z0-9_-]+$' \
    || { echo "ERROR: prefix must be alphanumeric / - / _ (got '$IFACE_PREFIX')" >&2; exit 1; }
[ "${#IFACE_PREFIX}" -le 14 ] \
    || { echo "ERROR: prefix too long ('$IFACE_PREFIX'); max 14 chars (IFNAMSIZ)" >&2; exit 1; }

# names the previous install used, so we can tear down the vcan we no longer need
. /etc/default/mcxe31b-canbridge
OLD_IFACES="${IFACES:-}"

IFACES=""
i=0
while [ "$i" -le 5 ]; do
    IF="${IFACE_PREFIX}${i}"
    # abort only if the name is a NON-vcan device (a real port we'd wrongly bind onto);
    # our own leftover vcan is fine -- setup-vcan reuses it.
    if ip link show "$IF" >/dev/null 2>&1 && ! ip -d link show "$IF" 2>/dev/null | grep -qw vcan; then
        echo "ERROR: $IF already exists and is not a vcan; choose a different --prefix" >&2; exit 1
    fi
    IFACES="${IFACES}${IFACES:+ }$IF"
    i=$((i + 1))
done
# '|' delimiter so the prefix can't break the sed expression.
sed -i "s|^IFACES=.*|IFACES=\"$IFACES\"|" /etc/default/mcxe31b-canbridge
echo "set IFACES=$IFACES"

# stop the running service before retiring its old interfaces, so it doesn't flap on
# Restart=always while we delete the vcan it is bound to (no-op on a first install).
command -v systemctl >/dev/null 2>&1 && systemctl stop mcxe31b-canbridge.service 2>/dev/null || true

# drop old vcan interfaces no longer in IFACES (vcan-only check, so a real CAN port is
# never touched); they are virtual and would otherwise linger until reboot.
for old in $OLD_IFACES; do
    case " $IFACES " in *" $old "*) continue ;; esac
    if ip -d link show "$old" 2>/dev/null | grep -qw vcan; then
        ip link delete "$old" 2>/dev/null && echo "removed stale vcan $old" || true
    fi
done

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
    echo "service (re)started. Check:  systemctl status mcxe31b-canbridge ;  candump ${IFACES%% *}"
else
    . /etc/default/mcxe31b-canbridge
    echo "no systemd detected - start manually:"
    # shellcheck disable=SC2086
    echo "  /usr/libexec/mcxe31b-canbridge/setup-vcan.sh $IFACES"
    echo "  /usr/sbin/canbridge --board $BOARD_IP $IFACES"
fi

cat <<'EOF'

NOTE: the bridged CAN interfaces are VIRTUAL (vcan). `ip link set <iface> type can bitrate ...`
does NOT work on them. Set the real CAN bitrate on the BOARD, e.g.:
  canbridge_ctl --board <board-ip> set_can_config channel=0 fd=true bitrate=1000000 data_bitrate=5000000 brs=true
EOF
