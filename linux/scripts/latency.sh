#!/bin/sh
# latency.sh - ping-style round-trip latency of the MCXE31B canbridge. NOT a throughput test.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Like `ping`: send ONE CAN-FD frame through the board, wait for it to come back on the
# partner channel, print that single round-trip, then repeat once per interval (Ctrl-C to
# stop). One frame in flight at a time, so each printed RTT is the REAL forwarding latency
# (Pi -> board -> CAN bus -> board -> Pi), never queueing. For loss/throughput at full
# load use stress.sh instead (which, under a flood, does NOT report latency on purpose).
#
#   sudo ./latency.sh <board-ip>            # ping can0<->can1, 1/s, until Ctrl-C
#   sudo ./latency.sh <board-ip> 0.2        # 2nd arg = interval seconds (like ping -i)
#   sudo ./latency.sh <board-ip> 1 10       # 3rd arg = count (0/empty = infinite)
#
# Interval and count are positional on purpose: 'INTERVAL=.. sudo ...' does NOT work
# because sudo drops the caller's env. Override other knobs after sudo, e.g.
# 'sudo PAIR="can2 can3" ./latency.sh <ip>'.
#   env knobs: PAIR LEN BITRATE DBITRATE
set -u

BOARD_IP="${1:-${BOARD_IP:-192.168.8.113}}"
INTERVAL="${2:-${INTERVAL:-1}}"   # seconds between pings (2nd arg); lower = faster ping
COUNT="${3:-${COUNT:-0}}"         # number of pings, 0 = until Ctrl-C (3rd arg)
PAIR="${PAIR:-can0 can1}"         # the one wired bus pair to ping (TX iface, RX iface)
LEN="${LEN:-64}"                  # FD payload bytes
BITRATE="${BITRATE:-1000000}"
DBITRATE="${DBITRATE:-5000000}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CTL="${CTL:-$(command -v canbridge_ctl 2>/dev/null || echo "$HERE/../canbridge_ctl")}"
MESH="${MESH:-$HERE/../canmesh}"
[ -x "$MESH" ] || { echo "missing canmesh (run 'make')"; exit 1; }

set -- $PAIR
A="${1:-can0}"; B="${2:-can1}"

echo "== latency: ping-style, $A<->$B, ${LEN}B FD, every ${INTERVAL}s  (board $BOARD_IP) =="
echo "   one frame in flight: each line is REAL forwarding latency, not queueing (Ctrl-C to stop)."

# set FD bitrate on every channel, then zero the counters so the footer is this run only
for ch in 0 1 2 3 4 5; do
    "$CTL" --board "$BOARD_IP" set_can_config channel="$ch" enabled=true fd=true \
        bitrate="$BITRATE" data_bitrate="$DBITRATE" brs=true >/dev/null 2>&1 \
        || echo "WARN: set_can_config ch$ch failed (board $BOARD_IP reachable?)"
done
"$CTL" --board "$BOARD_IP" reset_stats >/dev/null 2>&1

# Run the ping in the foreground so its per-packet lines stream live. Ctrl-C reaches
# canmesh (which prints its own ping summary) AND this script; trap it as a no-op so the
# script survives to print the board-internal footer below.
trap 'true' INT
"$MESH" "$A" "$B" --ping --interval "$INTERVAL" --count "$COUNT" --len "$LEN"
trap - INT

# Footer: the board's own on-chip forwarding latency (DWT, host-clock free), printed once.
# This is the slice of each ping spent INSIDE the board; the rest is Ethernet + the Pi.
status=$("$CTL" --board "$BOARD_IP" get_status 2>/dev/null)
printf '%s' "$status" | awk '
function field(s, sect, key,   r) {
    if (match(s, "\"" sect "\":[{][^}]*[}]")) {
        r = substr(s, RSTART, RLENGTH)
        if (match(r, "\"" key "\":[0-9]+")) {
            r = substr(r, RSTART, RLENGTH); sub("\"" key "\":", "", r); return r + 0
        }
    }
    return -1
}
{
    u2ca = field($0, "udp_to_can", "avg"); u2cm = field($0, "udp_to_can", "max")
    c2ua = field($0, "can_to_udp", "avg"); c2um = field($0, "can_to_udp", "max")
    if (u2cm < 0) exit   # firmware without the latency block
    printf "board on-chip (DWT, real us):  udp->can avg=%dus max=%dus   can->udp avg=%dus max=%dus\n",
           u2ca, u2cm, c2ua, c2um
    printf "  => board forwarding ~%dus (<< 1 ms); the rest of each ping is Ethernet + the Pi.\n", u2ca + c2ua
}'
