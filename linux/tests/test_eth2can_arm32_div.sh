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

if grep -nE 'ktime_get_ns[[:space:]]*\([^)]*\)[[:space:]]*/' "$SRC"; then
    fail "ktime_get_ns() must not be divided directly; use div_u64() on 32-bit ARM"
fi

echo "PASS: eth2can ARM32 division check"
