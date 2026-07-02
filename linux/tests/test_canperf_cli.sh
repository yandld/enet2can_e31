#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CANPERF_DIR="$ROOT_DIR/linux/can_testcase"
BIN="$CANPERF_DIR/canperf.host"

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

assert_contains()
{
    file=$1
    pattern=$2
    if ! grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
        echo "----- $file -----" >&2
        cat "$file" >&2 || true
        fail "expected to find: $pattern"
    fi
}

assert_not_contains()
{
    file=$1
    pattern=$2
    if grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
        echo "----- $file -----" >&2
        cat "$file" >&2 || true
        fail "did not expect to find: $pattern"
    fi
}

expect_rejected()
{
    out=$1
    err=$2
    shift 2

    if "$BIN" "$@" >"$out" 2>"$err"; then
        fail "expected command to reject: $*"
    fi
    assert_contains "$err" "unrecognized option"
}

tmp=${TMPDIR:-/tmp}/e2cf-canperf-cli-test.$$
trap 'rm -rf "$tmp"; make -C "$CANPERF_DIR" clean >/dev/null 2>&1 || true' EXIT INT HUP TERM
mkdir -p "$tmp"

make -C "$CANPERF_DIR" clean host >/dev/null

help=$tmp/help.out
"$BIN" --help >"$help"
assert_contains "$help" "canperf latency"
assert_contains "$help" "canperf bandwidth"
assert_contains "$help" "default CAN FD rate: 1M/5M"
assert_contains "$help" "p99.9 (p999)"
assert_contains "$help" "final confirmation"
assert_contains "$help" "zero-loss evidence"
assert_contains "$help" "RESULT line for customers"
assert_contains "$help" "--pair A:B"
assert_contains "$help" "--count N"
assert_contains "$help" "--duration T"
assert_contains "$help" "--bitrate R"
assert_contains "$help" "--dbitrate R"
assert_contains "$help" "--no-setup"
assert_not_contains "$help" "--performance"
assert_not_contains "$help" "--csv"
assert_not_contains "$help" "--loopback"
assert_not_contains "$help" "--size"
assert_not_contains "$help" "--window"
assert_not_contains "$help" "--sweep"
assert_not_contains "$help" "--sweep-frames"
assert_not_contains "$help" "--p99-limit"
assert_not_contains "$help" "--gap-us"
assert_not_contains "$help" "--bidir"
assert_not_contains "$help" "--report-s"
assert_not_contains "$help" "default 8M"

"$BIN" latency --help >"$tmp/latency-help.out"
assert_contains "$tmp/latency-help.out" "canperf latency"

"$BIN" bandwidth --help >"$tmp/bandwidth-help.out"
assert_contains "$tmp/bandwidth-help.out" "canperf bandwidth"

SRC="$CANPERF_DIR/src/canperf.c"
assert_not_contains "$SRC" "criterion: lost=0 & p99<="
assert_contains "$SRC" "RESULT latency"
assert_contains "$SRC" "RESULT bandwidth"
assert_contains "$SRC" "EVIDENCE latency"
assert_contains "$SRC" "EVIDENCE bandwidth"
assert_contains "$SRC" "final confirmation"
assert_contains "$SRC" "p99.9"

expect_rejected "$tmp/perf.out" "$tmp/perf.err" --performance --no-setup --count 1
expect_rejected "$tmp/csv.out" "$tmp/csv.err" --csv --no-setup --count 1
expect_rejected "$tmp/loopback.out" "$tmp/loopback.err" --loopback --no-setup --count 1
expect_rejected "$tmp/size.out" "$tmp/size.err" --size 8 --no-setup --count 1
expect_rejected "$tmp/window.out" "$tmp/window.err" --window 16 --no-setup --count 1
expect_rejected "$tmp/sweep.out" "$tmp/sweep.err" --sweep --no-setup --count 1
expect_rejected "$tmp/sweep-frames.out" "$tmp/sweep-frames.err" --sweep-frames 1000 --no-setup --count 1
expect_rejected "$tmp/p99.out" "$tmp/p99.err" --p99-limit 1000 --no-setup --count 1
expect_rejected "$tmp/gap.out" "$tmp/gap.err" --gap-us 100 --no-setup --count 1
expect_rejected "$tmp/bidir.out" "$tmp/bidir.err" --bidir --no-setup --count 1
expect_rejected "$tmp/report.out" "$tmp/report.err" --report-s 10 --no-setup --count 1

echo "PASS: canperf CLI checks"
