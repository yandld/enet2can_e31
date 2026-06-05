#!/bin/sh
# stress.sh - the pressure test for the MCXE31B canbridge. One command, one job.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Each of the 6 channels does 1 frame/ms (1000 fps) TX + 1000 fps RX, 64B FD.
# Wire each consecutive pair on its OWN terminated bus, so a channel's TX is its
# partner's RX (the bench stand-in for the customer's real CAN devices):
#   (can0,can1) (can2,can3) (can4,can5)     each 120R x2 = 60R
#
# The script sets the board bitrate (FD 1M/5M BRS), runs the load, then prints
# per-frame loss + the board's REAL internal latency (DWT, microseconds) + (on loss)
# the board counters that attribute it. The per-bus 'latency' line is the full
# Pi->board->Pi roundtrip (dominated by the host's non-realtime scheduling jitter);
# the board DWT figures are the product's own forwarding latency.
#
#   sudo ./stress.sh <board-ip>             # default: 1000 fps/ch, 64B, 10s
#   sudo ./stress.sh <board-ip> 2000        # 2nd arg = rate; raise it to find the ceiling
#
# Rate is a positional arg on purpose: 'RATE=2000 sudo ...' does NOT work because
# sudo drops the caller's env vars. To override other knobs, put them after sudo,
# e.g. 'sudo IFACES="can0 can1" ./stress.sh <ip>'.
#   env knobs: RATE LEN DURATION BITRATE DBITRATE IFACES
set -u

BOARD_IP="${1:-${BOARD_IP:-192.168.8.113}}"
RATE="${2:-${RATE:-1000}}"    # frames/sec per channel each way; 2nd arg overrides
IFACES="${IFACES:-can0 can1 can2 can3 can4 can5}"
LEN="${LEN:-64}"              # FD payload bytes
DURATION="${DURATION:-10}"
BITRATE="${BITRATE:-1000000}"
DBITRATE="${DBITRATE:-5000000}"

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CTL="${CTL:-$(command -v canbridge_ctl 2>/dev/null || echo "$HERE/../canbridge_ctl")}"
MESH="${MESH:-$HERE/../canmesh}"
[ -x "$MESH" ] || { echo "missing canmesh (run 'make')"; exit 1; }

echo "== pressure: 6ch x ${RATE} fps TX+RX, ${LEN}B FD, ${DURATION}s  (board $BOARD_IP) =="
echo "   wire pairs on independent buses: (can0,can1)(can2,can3)(can4,can5)"

# set FD bitrate on every channel, then zero the counters
i=0
for f in $IFACES; do
    "$CTL" --board "$BOARD_IP" set_can_config channel="$i" enabled=true fd=true \
        bitrate="$BITRATE" data_bitrate="$DBITRATE" brs=true >/dev/null 2>&1 \
        || echo "WARN: set_can_config ch$i failed (board $BOARD_IP reachable?)"
    i=$((i+1))
done
"$CTL" --board "$BOARD_IP" reset_stats >/dev/null 2>&1

# one canmesh per pair; --rate is aggregate over the 2 ifaces, so RATE*2 = RATE/ch each way
set -- $IFACES
pids=""
while [ $# -ge 2 ]; do
    a="$1"; b="$2"; shift 2
    "$MESH" "$a" "$b" --rate "$((RATE*2))" --duration "$DURATION" --len "$LEN" &
    pids="$pids $!"
done
[ $# -eq 1 ] && echo "note: odd iface '$1' unpaired (needs a partner on its bus)"
# each canmesh exits 0=PASS, 2=FAIL, 3=TOOL-LIMITED; aggregate the verdicts
fail=0
for pid in $pids; do
    wait "$pid" || fail=1
done

# The PRODUCT's real latency: the board's own DWT counters (microseconds), covering
# ONLY the on-board UDP<->CAN forwarding. The per-bus 'latency' above is the full
# Pi->board->Pi roundtrip and is dominated by the host's non-realtime scheduling, not
# the product. Counters were zeroed before the run (reset_stats above), so this snapshot
# is the latency over exactly this test window. Reused by the failure branch below.
status=$("$CTL" --board "$BOARD_IP" get_status 2>/dev/null)
lat=$(printf '%s' "$status" | awk '
function field(s, sect, key,   r) {
    if (match(s, "\"" sect "\":[{][^}]*[}]")) {
        r = substr(s, RSTART, RLENGTH)
        if (match(r, "\"" key "\":[0-9]+")) {
            r = substr(r, RSTART, RLENGTH); sub("\"" key "\":", "", r); return r
        }
    }
    return "?"
}
{
    printf "udp->can avg=%s max=%s  can->udp avg=%s max=%s  loop max=%s",
           field($0, "udp_to_can", "avg"), field($0, "udp_to_can", "max"),
           field($0, "can_to_udp", "avg"), field($0, "can_to_udp", "max"),
           field($0, "loop", "max")
}')
case "$lat" in
*max=[0-9]*)
    echo "product latency (board internal DWT, real microseconds): $lat"
    echo "  ^ this is the product. per-bus 'latency' above = Pi->board->Pi roundtrip incl. host jitter."
    ;;
esac

if [ "$fail" = 0 ]; then
    echo "ALL PASS - 6 channels lossless at ${RATE} fps/ch (raise rate via 2nd arg to find the ceiling)"
    exit 0
fi

# something lost frames: dump the board's own counters so the loss can be attributed
# ($status was fetched right after the run, above)
echo "-- board counters (why frames were lost) --"
printf '  totals:'
printf '%s' "$status" | grep -oE '"loss":[0-9]+|"queue_full":[0-9]+' | sed 's/"//g' | tr '\n' ' '; echo
printf '%s' "$status" | awk '
function v(r, k,  s) {
    if (match(r, "\"" k "\":\"?[^,\"}]*")) {
        s = substr(r, RSTART, RLENGTH); sub("\"" k "\":\"?", "", s); return s
    }
    return "?"
}
{
    cans = $0
    sub(/.*"can":\[/, "", cans)     # isolate the status array...
    sub(/\],"config".*/, "", cans)  # ...drop config[], which also starts each entry with {"ch":
    n = split(cans, c, /\{"ch":/)
    for (i = 2; i <= n; i++)
        printf "  ch%d: state=%-13s tx_drop=%s error=%s rx_fifo_overflow=%s\n",
               i - 2, v(c[i], "state"), v(c[i], "tx_drop"), v(c[i], "error"), v(c[i], "rx_fifo_overflow")
}'
printf '  bridge rxq_ovfl: '
journalctl -u mcxe31b-canbridge -n 2 --no-pager 2>/dev/null | grep -o 'rxq_ovfl=[0-9]*' | tail -1 || echo "n/a"
echo "  -> state=error-passive: bus wiring/termination;  rx_fifo_overflow: board RX cap;  queue_full/tx_drop: board TX cap"
exit 1
