#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
LINUX_DESIGN="$ROOT_DIR/docs/linux-driver-design.md"
MCX_DESIGN="$ROOT_DIR/docs/mcxe31b-firmware-design.md"
PROTO_SPEC="$ROOT_DIR/docs/e2cf-protocol-spec.md"
EQOS_DESIGN="$ROOT_DIR/docs/eqos-tx-ring-design.md"

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

for file in "$LINUX_DESIGN" "$MCX_DESIGN" "$PROTO_SPEC" "$EQOS_DESIGN"; do
    [ -f "$file" ] || fail "expected design document: $file"
done

for file in "$ROOT_DIR/docs"/*.md; do
    base=$(basename -- "$file")
    case "$base" in
        *[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-]*)
            fail "docs markdown filenames must use English safe characters: $base"
            ;;
    esac
done

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
assert_not_contains "$LINUX_DESIGN" "dbitrate 8000000"
assert_not_contains "$MCX_DESIGN" "默认 1M/8M"

OLD_DESIGN_DIR=eth2can"_design"
for file in "$ROOT_DIR/docs"/*.md; do
    assert_not_contains "$file" "$OLD_DESIGN_DIR"
    assert_not_contains "$file" "Phase"
    assert_not_contains "$file" "A/B"
    assert_not_contains "$file" "早期"
    assert_not_contains "$file" "历史"
    assert_not_contains "$file" "背景"
    assert_not_contains "$file" "尚未"
    assert_not_contains "$file" "未实现"
done

assert_contains "$ROOT_DIR/README.md" "e2cf_mcxe31.uvprojx"
assert_contains "$ROOT_DIR/README.md" "FLEXCAN_0_TX"
assert_contains "$ROOT_DIR/README.md" "FLEXCAN_5_RX"
assert_contains "$ROOT_DIR/README.md" "EMAC_MII_RMII_MDIO"
assert_contains "$ROOT_DIR/README.md" "EMAC_MII_RMII_TX_CLK"
assert_contains "$ROOT_DIR/README.md" "LPUART_5_TX"
assert_contains "$ROOT_DIR/README.md" "120"
assert_contains "$ROOT_DIR/README.md" "RESULT"
assert_contains "$ROOT_DIR/README.md" "EVIDENCE"
assert_contains "$ROOT_DIR/linux/README.md" "maximum sustainable rate"
assert_contains "$ROOT_DIR/linux/README.md" "p99.9"
assert_contains "$ROOT_DIR/linux/can_testcase/README.md" "final confirmation"
assert_contains "$ROOT_DIR/linux/can_testcase/README.md" "EVIDENCE bandwidth"
assert_contains "$ROOT_DIR/linux/can_testcase/README.md" "120"
assert_contains "$PROTO_SPEC" "E2CF 协议规范"
assert_contains "$LINUX_DESIGN" "Linux 驱动设计"
assert_contains "$MCX_DESIGN" "MCXE31B 固件设计"
assert_contains "$EQOS_DESIGN" "EQOS TX 环设计"

echo "PASS: delivery default checks"
