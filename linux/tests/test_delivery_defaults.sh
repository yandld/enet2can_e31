#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

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
        grep -n -F -- "$pattern" "$file" >&2 || true
        fail "did not expect to find: $pattern"
    fi
}

assert_contains "$ROOT_DIR/source/e2cf_config.h" "#define E2CF_DEFAULT_DAT_BITRATE 5000000U"
assert_contains "$ROOT_DIR/linux/scripts/install_driver.sh" "DBITRATE=5000000"
assert_contains "$ROOT_DIR/linux/can_testcase/src/canperf.c" ".dbitrate = 5000000"
assert_contains "$ROOT_DIR/tools/imx95/board_setup.sh" "dbitrate 5000000"

for file in \
    "$ROOT_DIR/README.md" \
    "$ROOT_DIR/linux/README.md" \
    "$ROOT_DIR/linux/can_testcase/README.md" \
    "$ROOT_DIR/tools/imx95/README.md" \
    "$ROOT_DIR/CLAUDE.md"
do
    assert_not_contains "$file" "1M/8M"
    assert_not_contains "$file" "dbitrate 8000000"
done

assert_not_contains "$ROOT_DIR/linux/scripts/install_driver.sh" "Default: 8000000"
assert_not_contains "$ROOT_DIR/linux/can_testcase/src/canperf.c" "default 8M"
assert_not_contains "$ROOT_DIR/eth2can_design/02_Linux侧详细设计.md" "dbitrate 8000000"
assert_not_contains "$ROOT_DIR/eth2can_design/03_MCXE侧详细设计.md" "默认 1M/8M"

echo "PASS: delivery default checks"
