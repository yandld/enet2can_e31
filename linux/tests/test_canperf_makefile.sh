#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CANPERF_DIR="$ROOT_DIR/linux/can_testcase"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

assert_contains()
{
    text=$1
    pattern=$2
    case "$text" in
        *"$pattern"*) ;;
        *)
            printf '%s\n' "$text" >&2
            fail "expected to find: $pattern"
            ;;
    esac
}

assert_not_contains()
{
    text=$1
    pattern=$2
    case "$text" in
        *"$pattern"*)
            printf '%s\n' "$text" >&2
            fail "did not expect to find: $pattern"
            ;;
        *) ;;
    esac
}

default_build=$(make -C "$CANPERF_DIR" -n -B all 2>&1) ||
    fail "default dry-run build failed"
assert_not_contains "$default_build" "/opt/arm-gnu-toolchain"
assert_not_contains "$default_build" "aarch64-none-linux-gnu-gcc"
assert_contains "$default_build" "-o canperf src/canperf.c"

native_override=$(make -C "$CANPERF_DIR" -n -B all CC=gcc 2>&1) ||
    fail "native override dry-run build failed"
assert_contains "$native_override" "gcc "
assert_contains "$native_override" "-o canperf src/canperf.c"

cross_build=$(make -C "$CANPERF_DIR" -n -B cross 2>&1) ||
    fail "cross dry-run build failed"
assert_contains "$cross_build" "aarch64-none-linux-gnu-gcc"
assert_contains "$cross_build" "-static"
assert_contains "$cross_build" "-o canperf src/canperf.c"

cross_toolchain_dir=$(make -C "$CANPERF_DIR" -n -B cross TOOLCHAIN_DIR=/opt/tc 2>&1) ||
    fail "cross TOOLCHAIN_DIR dry-run build failed"
assert_contains "$cross_toolchain_dir" "/opt/tc/bin/aarch64-none-linux-gnu-gcc"
assert_contains "$cross_toolchain_dir" "-o canperf src/canperf.c"

host_build=$(make -C "$CANPERF_DIR" -n -B host 2>&1) ||
    fail "host dry-run build failed"
assert_contains "$host_build" "-fsanitize=address,undefined"
assert_contains "$host_build" "-o canperf.host src/canperf.c"

grep -qx 'canperf' "$CANPERF_DIR/.gitignore" ||
    fail "generated canperf binary must be ignored"

echo "PASS: canperf Makefile checks"
