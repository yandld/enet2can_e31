#!/bin/sh
# check-proto.sh - fail if the bridge wire header drifted from the firmware header.
# Compares the load-bearing #define constants (name + value), comments ignored.
# POSIX sh (no process substitution), so it runs under dash/busybox too.
set -eu

WIRE="${1:-}"
FW="${2:-}"
if [ ! -f "$WIRE" ] || [ ! -f "$FW" ]; then
    echo "usage: check-proto.sh <wire.h> <firmware.h>" >&2
    exit 2
fi

extract() {
    grep -E '^#define[[:space:]]+CAN_GATEWAY_(MAGIC|VERSION|MAX_CHANNELS|MAX_DATA_LEN|MAX_FRAMES_PER_PACKET|UDP_DATA_PORT|UDP_CONTROL_PORT|PACKET_TYPE_FRAMES|FLAG_[A-Z_]+|FRAME_HEAD_SIZE|PACKET_HEADER_SIZE)[[:space:]]' "$1" \
        | sed -E 's@/\*.*@@; s@[[:space:]]+@ @g; s@ $@@' \
        | awk '{print $2" "$3}' \
        | sort
}

TA=$(mktemp)
TB=$(mktemp)
trap 'rm -f "$TA" "$TB"' EXIT
extract "$WIRE" > "$TA"
extract "$FW" > "$TB"

if ! diff -u "$TB" "$TA" > /dev/null; then
    echo "PROTO DRIFT between $FW (expected) and $WIRE (actual):" >&2
    diff -u "$TB" "$TA" >&2 || true
    exit 1
fi
echo "PROTO OK: wire constants in $WIRE match $FW"

# Layout guard: static-assert the 16-byte head field offsets against the firmware
# struct, so a field reorder that leaves the #defines unchanged still fails the build.
CC_BIN=$(command -v gcc || command -v cc || true)
if [ -n "$CC_BIN" ]; then
    TC=$(mktemp)
    trap 'rm -f "$TA" "$TB" "$TC"' EXIT
    cat > "$TC" <<'EOF'
#include <stddef.h>
#include "can_gateway_protocol.h"
_Static_assert(sizeof(can_gateway_packet_header_t) == 16, "packet header layout");
_Static_assert(offsetof(can_gateway_frame_t, channel) == 0, "frame head layout");
_Static_assert(offsetof(can_gateway_frame_t, can_id) == 4, "frame head layout");
_Static_assert(offsetof(can_gateway_frame_t, timestamp) == 8, "frame head layout");
_Static_assert(offsetof(can_gateway_frame_t, status) == 12, "frame head layout");
EOF
    if "$CC_BIN" -std=c11 -x c -fsyntax-only -I"$(dirname "$FW")" "$TC" 2>/dev/null; then
        echo "PROTO OK: head field offsets match $FW"
    else
        echo "PROTO DRIFT: firmware head field offsets changed - reconcile $WIRE with $FW" >&2
        exit 1
    fi
else
    echo "NOTE: no C compiler; skipped struct-layout check (constants checked only)"
fi
