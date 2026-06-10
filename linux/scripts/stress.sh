#!/bin/sh
# stress.sh - the pressure test for the MCXE31B canbridge, via on-chip CAN loopback.
#
# SPDX-License-Identifier: BSD-3-Clause
#
# Each of the 6 channels does RATE frame/s TX + RATE frame/s RX, 64B FD. Instead of wiring
# channel pairs on real terminated buses, the board puts each FlexCAN in internal loopback
# (self-reception), so every frame a channel sends returns on that SAME channel. NO CAN
# cabling or 120R termination needed - the board self-loops.
#
# The script sets the board bitrate (FD 1M/5M BRS) + loopback, runs the load, then prints
# per-frame loss + the board's REAL internal latency (DWT, microseconds) + (on loss) the
# board counters that attribute it. The 'latency' line is the full Pi->board->loopback->Pi
# roundtrip (dominated by the host's non-realtime scheduling jitter); the board DWT figures
# are the product's own forwarding latency.
#
#   sudo ./stress.sh <board-ip>             # default: 1000 fps/ch, 64B, 10s
#   sudo ./stress.sh <board-ip> 2000        # 2nd arg = rate/ch; raise it to find the ceiling
#   sudo ./stress.sh <board-ip> 2000 30     # 3rd arg = duration in seconds
#
# Rate and duration are positional on purpose: 'RATE=2000 sudo ...' does NOT work because
# sudo drops the caller's env vars. To override other knobs, put them after sudo, e.g.
# 'sudo IFACES="can0 can1" ./stress.sh <ip>'.
# This is the LOSS/THROUGHPUT test. It does NOT report a latency max on purpose: under a
# 6xRATE flood that "max" is queueing, not forwarding latency. For the real latency, run
# latency.sh (ping-style). DEBUG=1 re-enables the detailed board-internal latency breakdown
# for tuning (hidden from customers).
#   env knobs: RATE LEN DURATION BITRATE DBITRATE IFACES DEBUG
set -u

BOARD_IP="${1:-${BOARD_IP:-192.168.8.113}}"
RATE="${2:-${RATE:-1000}}"    # frames/sec per channel each way; 2nd arg overrides
IFACES="${IFACES:-can0 can1 can2 can3 can4 can5}" # channels to loopback (canX = channel X)
LEN="${LEN:-64}"              # FD payload bytes
DURATION="${3:-${DURATION:-10}}"  # seconds; 3rd arg overrides
BITRATE="${BITRATE:-1000000}"
DBITRATE="${DBITRATE:-5000000}"
DEBUG="${DEBUG:-0}"          # 1 = show detailed board-internal latency debug (hidden from customers)

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CTL="${CTL:-$(command -v canbridge_ctl 2>/dev/null || echo "$HERE/../canbridge_ctl")}"
MESH="${MESH:-$HERE/../canmesh}"
[ -x "$MESH" ] || { echo "missing canmesh (run 'make')"; exit 1; }

NCH=$(set -- $IFACES; echo $#)   # channel count, for the aggregate send rate below

echo "== pressure: ${NCH}ch x ${RATE} fps TX+RX (loopback), ${LEN}B FD, ${DURATION}s  (board $BOARD_IP) =="
echo "   on-chip CAN loopback, no wiring: each channel sends and self-receives its own frames."

# enable FD + on-chip loopback on every channel, then zero the counters
for f in $IFACES; do
    ch=${f#can}
    "$CTL" --board "$BOARD_IP" set_can_config channel="$ch" enabled=true fd=true \
        bitrate="$BITRATE" data_bitrate="$DBITRATE" brs=true loopback=true >/dev/null 2>&1 \
        || echo "WARN: set_can_config ch$ch failed (board $BOARD_IP reachable?)"
done
"$CTL" --board "$BOARD_IP" reset_stats >/dev/null 2>&1

# Restore normal (non-loopback) bus forwarding when we are done; loopback is a test mode.
restore_loopback() {
    for f in $IFACES; do
        "$CTL" --board "$BOARD_IP" set_can_config channel="${f#can}" loopback=false >/dev/null 2>&1
    done
}
# Arm restore right after loopback was enabled, so ANY exit turns it back off. INT/TERM
# (Ctrl-C, kill) exit cleanly so the EXIT trap is guaranteed to run instead of the signal
# killing the script with loopback still on.
trap restore_loopback EXIT
trap 'exit 130' INT TERM

# One canmesh over all channels in loopback. --rate is the AGGREGATE send rate across all
# ifaces, so RATE*NCH gives RATE fps/channel; each frame self-loops and returns on its own
# channel (RATE fps RX/channel too). BRS is ON: the board disables TDC on loopback channels
# (the RM rule the SDK skips), so the test frames really switch to the 5M data phase and still
# self-receive - the load is full FD+BRS. Capture to a tmp file to replay + parse the latency.
TMPD=$(mktemp -d)
trap 'rm -rf "$TMPD"; restore_loopback' EXIT
"$MESH" $IFACES --rate "$((RATE*NCH))" --duration "$DURATION" --len "$LEN" --loopback >"$TMPD/run.out" 2>&1
rc=$?

# Per-bus line: in a normal (customer) run, hide the latency figures -- under this flood
# that "max" is queueing, not latency (use latency.sh for the real number). DEBUG=1 keeps
# them plus the full board-internal breakdown below.
if [ "$DEBUG" = 1 ]; then
    cat "$TMPD/run.out" 2>/dev/null
else
    sed 's/, latency avg=[^ ]* max=[^ ]*//' "$TMPD/run.out" 2>/dev/null
fi

# Board counters snapshot (zeroed before the run). Reused by the failure branch below.
status=$("$CTL" --board "$BOARD_IP" get_status 2>/dev/null)

if [ "$DEBUG" = 1 ]; then
    # Detailed debug (hidden from customers): board DWT latency, the board-vs-network split,
    # and per-leg poll timings that locate a board-internal loop spike. Under this flood the
    # "latency" maxes are queueing, not forwarding latency -- which is exactly why they are
    # gated behind DEBUG. For the real latency run latency.sh.
    rt_max=$(grep -ho 'max=[0-9.]*ms' "$TMPD/run.out" 2>/dev/null | sed 's/max=//;s/ms//' | sort -gr | head -1)
    rt_avg=$(grep -ho 'avg=[0-9.]*ms' "$TMPD/run.out" 2>/dev/null | sed 's/avg=//;s/ms//' | sort -gr | head -1)
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
        lpa  = field($0, "loop", "avg");       lpm  = field($0, "loop", "max")
        ethm = field($0, "eth_poll", "max")
        canm = field($0, "can_poll", "max")
        gwm  = field($0, "gw_poll", "max")
        if (u2cm < 0) exit

        printf "product latency (board internal DWT, real microseconds): udp->can avg=%d max=%d  can->udp avg=%d max=%d  loop max=%d\n",
               u2ca, u2cm, c2ua, c2um, lpm
        bmax = u2cm + c2um; bavg = u2ca + c2ua
        print "=== latency breakdown: board vs network+host (DEBUG; under load this is QUEUEING, not latency) ==="
        if (rtmax + 0 > 0) {
            tmax = rtmax * 1000.0; omax = tmax - bmax; if (omax < 0) omax = 0
            printf "  MAX   board %.2f ms  |  total %.2f ms  |  network+host %.2f ms  (%.0f%% of total)\n",
                   bmax / 1000.0, tmax / 1000.0, omax / 1000.0, 100.0 * omax / tmax
        }
        if (rtavg + 0 > 0) {
            tavg = rtavg * 1000.0; oavg = tavg - bavg; if (oavg < 0) oavg = 0
            printf "  AVG   board %.2f ms  |  total %.2f ms  |  network+host %.2f ms  (%.0f%% of total)\n",
                   bavg / 1000.0, tavg / 1000.0, oavg / 1000.0, 100.0 * oavg / tavg
        }
        printf "  [board internals] loop avg=%d max=%d us  |  poll-leg max: eth=%d can=%d gw=%d us\n",
               lpa, lpm, ethm, canm, gwm
    }'
else
    # Customer view: throughput + board queue health, NO latency (latency belongs in
    # latency.sh, measured unloaded). 0 drops + queue headroom = lossless at this rate.
    printf '%s' "$status" | awk '
    function num(s, k,   r) { if (match(s, "\"" k "\":[0-9]+")) { r = substr(s, RSTART, RLENGTH); sub("\"" k "\":", "", r); return r + 0 } return 0 }
    function wm(s, k,   w)  { if (match(s, "\"watermark\":[{][^}]*[}]")) { w = substr(s, RSTART, RLENGTH); return num(w, k) } return 0 }
    {
        qfull = 0
        if (match($0, "\"router\":[{][^}]*[}]")) { rr = substr($0, RSTART, RLENGTH); qfull = num(rr, "queue_full") }
        cans = $0; sub(/.*"can":\[/, "", cans); sub(/\],"config".*/, "", cans)
        ns = split(cans, c, /\{"ch":/)
        wtx = 0; wrx = 0; dtx = 0; drx = 0; ovf = 0
        for (i = 2; i <= ns; i++) {
            dtx += num(c[i], "tx_drop"); drx += num(c[i], "rx_drop"); ovf += num(c[i], "rx_fifo_overflow")
            a = wm(c[i], "tx"); if (a > wtx) wtx = a
            b = wm(c[i], "rx"); if (b > wrx) wrx = b
        }
        printf "board: drops tx=%d rx=%d overflow=%d queue_full=%d  |  peak queue tx=%d/64 rx=%d/64\n",
               dtx, drx, ovf, qfull, wtx, wrx
    }'
fi

if [ "$rc" = 0 ]; then
    echo "ALL PASS - $NCH channels lossless at ${RATE} fps/ch (raise rate via 2nd arg to find the ceiling)"
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
echo "  -> rx_fifo_overflow: board RX cap;  queue_full/tx_drop: board TX cap;  state!=error-active is unexpected in loopback (no bus)."
exit 1
