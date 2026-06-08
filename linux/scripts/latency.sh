#!/bin/sh
# latency.sh - ping-style latency of the MCXE31B canbridge. NOT a throughput test.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Latency and throughput are different questions and want different tests (like ping vs
# iperf). This one is the "ping": it runs at a LOW frame rate so there is ~1 frame in
# flight at a time and the board's CAN bus never queues. The numbers it prints are the
# REAL forwarding latency, not queueing under saturation. For loss/throughput at full
# load, use stress.sh (which, on purpose, does NOT report a latency max -- under a flood
# that "max" is queueing, not latency).
#
#   sudo ./latency.sh <board-ip>            # default: 100 fps/ch, 64B FD, 10s
#   sudo ./latency.sh <board-ip> 50         # 2nd arg = rate/ch (keep it low)
#
# Rate is a positional arg on purpose: 'RATE=50 sudo ...' does NOT work because sudo
# drops the caller's env. Override other knobs after sudo, e.g. 'sudo IFACES="can0 can1"'.
#   env knobs: RATE LEN DURATION BITRATE DBITRATE IFACES
set -u

BOARD_IP="${1:-${BOARD_IP:-192.168.8.113}}"
RATE="${2:-${RATE:-100}}"     # frames/sec per channel each way - LOW on purpose (ping-style)
IFACES="${IFACES:-can0 can1 can2 can3 can4 can5}"
LEN="${LEN:-64}"              # FD payload bytes
DURATION="${DURATION:-10}"
BITRATE="${BITRATE:-1000000}"
DBITRATE="${DBITRATE:-5000000}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CTL="${CTL:-$(command -v canbridge_ctl 2>/dev/null || echo "$HERE/../canbridge_ctl")}"
MESH="${MESH:-$HERE/../canmesh}"
[ -x "$MESH" ] || { echo "missing canmesh (run 'make')"; exit 1; }

echo "== latency: ping-style, ${RATE} fps/ch, ${LEN}B FD, ${DURATION}s  (board $BOARD_IP) =="
echo "   low rate on purpose (~1 frame in flight): this is forwarding latency, not queueing."

# set FD bitrate on every channel, then zero the counters so the snapshot is this run only
i=0
for f in $IFACES; do
    "$CTL" --board "$BOARD_IP" set_can_config channel="$i" enabled=true fd=true \
        bitrate="$BITRATE" data_bitrate="$DBITRATE" brs=true >/dev/null 2>&1 \
        || echo "WARN: set_can_config ch$i failed (board $BOARD_IP reachable?)"
    i=$((i+1))
done
"$CTL" --board "$BOARD_IP" reset_stats >/dev/null 2>&1

TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"' EXIT
set -- $IFACES
pids=""
p=0
while [ $# -ge 2 ]; do
    a="$1"; b="$2"; shift 2
    "$MESH" "$a" "$b" --rate "$((RATE*2))" --duration "$DURATION" --len "$LEN" >"$TMPD/p$p.out" 2>&1 &
    pids="$pids $!"
    p=$((p+1))
done
fail=0
for pid in $pids; do
    wait "$pid" || fail=1
done
cat "$TMPD"/p*.out 2>/dev/null   # per-bus roundtrip lines (latency here IS meaningful)

status=$("$CTL" --board "$BOARD_IP" get_status 2>/dev/null)
rt_max=$(grep -ho 'max=[0-9.]*ms' "$TMPD"/p*.out 2>/dev/null | sed 's/max=//;s/ms//' | sort -gr | head -1)
rt_avg=$(grep -ho 'avg=[0-9.]*ms' "$TMPD"/p*.out 2>/dev/null | sed 's/avg=//;s/ms//' | sort -gr | head -1)

printf '%s' "$status" | awk -v rtmax="${rt_max:-0}" -v rtavg="${rt_avg:-0}" '
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

    print  "=== product latency (lightly loaded -- the REAL forwarding latency) ==="
    printf "  board (on-chip UDP<->CAN, DWT):    udp->can avg=%dus max=%dus   can->udp avg=%dus max=%dus\n",
           u2ca, u2cm, c2ua, c2um
    bmax = u2cm + c2um; bavg = u2ca + c2ua
    if (rtmax + 0 > 0)
        printf "  roundtrip (Pi<->board<->Pi):      avg=%.2f ms  max=%.2f ms\n", rtavg, rtmax
    if (rtmax + 0 > 0) {
        om = rtmax * 1000.0 - bmax; if (om < 0) om = 0
        oa = rtavg * 1000.0 - bavg; if (oa < 0) oa = 0
        printf "  network+host (roundtrip - board): avg=%.2f ms  max=%.2f ms\n", oa / 1000.0, om / 1000.0
    }
    printf "  => board forwarding ~%d us (<< 1 ms); the rest is Ethernet + the Pi.\n", bavg
}'

[ "$fail" = 0 ] || echo "note: a pair did not PASS. At this low rate that means wiring/termination/board, NOT load."
