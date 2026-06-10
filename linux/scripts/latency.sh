#!/bin/sh
# latency.sh - ping-style round-trip latency of the MCXE31B canbridge, via on-chip CAN
# loopback (NO bus wiring). NOT a throughput test.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Each round sends ONE CAN-FD frame on every channel at once; the board loops each frame
# back on-chip (FlexCAN self-reception) and returns it on the SAME channel. So each line is
# the REAL round-trip Pi -> board -> CAN-loopback -> board -> Pi, for all channels at once
# (like `ping`: one frame in flight per channel, never queueing). No CAN cabling or 120R
# termination needed - the board self-loops. For loss/throughput use stress.sh instead.
#
#   sudo ./latency.sh <board-ip>            # ping all 6 channels, 1/s, until Ctrl-C
#   sudo ./latency.sh <board-ip> 0.2        # 2nd arg = interval seconds (like ping -i)
#   sudo ./latency.sh <board-ip> 1 10       # 3rd arg = count (0/empty = infinite)
#
# Interval and count are positional on purpose: 'INTERVAL=.. sudo ...' does NOT work
# because sudo drops the caller's env. Override other knobs after sudo, e.g.
# 'sudo IFACES="can0 can1" ./latency.sh <ip>'.
#   env knobs: IFACES LEN BITRATE DBITRATE DEBUG
set -u

BOARD_IP="${1:-${BOARD_IP:-192.168.8.113}}"
INTERVAL="${2:-${INTERVAL:-1}}"   # seconds between pings (2nd arg); lower = faster ping
COUNT="${3:-${COUNT:-0}}"         # number of pings, 0 = until Ctrl-C (3rd arg)
IFACES="${IFACES:-can0 can1 can2 can3 can4 can5}" # channels to loopback-ping (canX = channel X)
LEN="${LEN:-64}"                  # FD payload bytes
BITRATE="${BITRATE:-1000000}"
DBITRATE="${DBITRATE:-5000000}"
DEBUG="${DEBUG:-0}"               # DEBUG=1 also prints the per-leg breakdown (engineers only)

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CTL="${CTL:-$(command -v canbridge_ctl 2>/dev/null || echo "$HERE/../canbridge_ctl")}"
MESH="${MESH:-$HERE/../canmesh}"
[ -x "$MESH" ] || { echo "missing canmesh (run 'make')"; exit 1; }

echo "== latency: loopback ping, [$IFACES], ${LEN}B FD, every ${INTERVAL}s  (board $BOARD_IP) =="
echo "   on-chip CAN loopback, no wiring: each line is the REAL per-channel round-trip (Ctrl-C to stop)."

# enable FD + on-chip loopback on every channel, then zero the counters for this run
for f in $IFACES; do
    ch=${f#can}
    "$CTL" --board "$BOARD_IP" set_can_config channel="$ch" enabled=true fd=true \
        bitrate="$BITRATE" data_bitrate="$DBITRATE" brs=true loopback=true >/dev/null 2>&1 \
        || echo "WARN: set_can_config ch$ch failed (board $BOARD_IP reachable?)"
done
"$CTL" --board "$BOARD_IP" reset_stats >/dev/null 2>&1

# Put the board back to normal (non-loopback) bus forwarding when we are done - loopback
# is a test mode; leaving it on would make the gateway swallow frames instead of putting
# them on the real bus. Runs on any exit (including Ctrl-C).
restore_loopback() {
    for f in $IFACES; do
        "$CTL" --board "$BOARD_IP" set_can_config channel="${f#can}" loopback=false >/dev/null 2>&1
    done
    echo "loopback disabled - board back to normal bus forwarding."
}
# restore_loopback runs on ANY exit. INT/TERM exit cleanly (instead of dying on the signal)
# so the EXIT trap is guaranteed to fire and turn loopback off, even mid-footer.
trap restore_loopback EXIT
trap 'exit 130' INT TERM

# Run the ping in the foreground so its per-round lines stream live. Ctrl-C reaches canmesh
# (which prints its own ping summary) AND this script; absorb it here so the script survives
# to print the board-internal footer below, then arm a clean-exit INT for the footer window.
# BRS is ON: the board disables TDC on loopback channels (the RM rule the SDK skips), so a
# self-transmitted frame really switches to the 5M data phase and still self-receives. The
# RTT thus includes the true fast-data-phase airtime; only the transceiver/cable/termination
# are bypassed (validate real-bus signal quality with a real node).
trap 'true' INT
"$MESH" $IFACES --ping --loopback --interval "$INTERVAL" --count "$COUNT" --len "$LEN"
trap 'exit 130' INT

# Footer: the ONE number the customer cares about - the board's longest internal path,
# eth-in -> CAN self-loop -> eth-out, measured per frame by the board itself (DWT, host-clock
# free). MAX is the worst case. The query uses the control plane (canbridge_ctl --board
# $BOARD_IP); a wrong/unreachable IP makes it fail, so warn loudly instead of dropping it.
# DEBUG=1 also prints the per-leg breakdown (udp->can / can->udp), for engineers locating
# where the time goes - meaningless to the customer, so hidden by default.
status=$("$CTL" --board "$BOARD_IP" get_status 2>/dev/null)
if [ -z "$status" ]; then
    echo "board eth-to-eth latency: board $BOARD_IP unreachable on control plane - pass the real board IP"
else
    printf '%s' "$status" | awk -v dbg="$DEBUG" '
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
    e2ec = field($0, "eth_to_eth", "count"); e2ea = field($0, "eth_to_eth", "avg"); e2em = field($0, "eth_to_eth", "max")
    if (e2ec > 0) {
        printf "board eth-to-eth latency (eth-in -> CAN loopback -> eth-out, real us):  MAX=%dus  avg=%dus\n", e2em, e2ea
    } else {
        print "board eth-to-eth latency: firmware does not report it yet (update board firmware)"
    }
    if (dbg == "1") {
        u2ca = field($0, "udp_to_can", "avg"); u2cm = field($0, "udp_to_can", "max")
        c2ua = field($0, "can_to_udp", "avg"); c2um = field($0, "can_to_udp", "max")
        printf "  [debug] legs: udp->can avg=%dus max=%dus   can->udp avg=%dus max=%dus  (breakdown only; eth-to-eth above is the number)\n", u2ca, u2cm, c2ua, c2um
    }
}'
fi
