#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SRC="$ROOT_DIR/linux/src/eth2can.c"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

awk '
    /static int e2cf_ndo_open\(struct net_device \*ndev\)/ { in_open = 1 }
    in_open && /e2cf_chan_configure\(chan\)/ {
        if (!seen_guard) {
            exit 2
        }
    }
    in_open && /gw_alive/ { seen_guard = 1 }
    in_open && /return -ENETDOWN/ { seen_enetdown = 1 }
    in_open && /^}/ {
        if (!seen_guard || !seen_enetdown) {
            exit 3
        }
        exit 0
    }
' "$SRC" || fail "e2cf_ndo_open must return -ENETDOWN before CFG when gateway is not alive"

if grep -nE '^[[:space:]]*dev_mc_add\(edev->lower, e2cf_hb_mcast\);' "$SRC"; then
    fail "dev_mc_add return value must be checked"
fi

echo "PASS: eth2can gateway guard checks"
