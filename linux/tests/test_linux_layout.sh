#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

set -- "$ROOT_DIR"/linux/scripts/*.sh
[ "$#" -eq 1 ] || fail "linux/scripts must contain only install_driver.sh"
[ "$(basename "$1")" = "install_driver.sh" ] || fail "linux/scripts must contain only install_driver.sh"

for script in \
    board_setup.sh \
    build_driver.sh \
    build_kernel.sh \
    deploy.sh \
    env.sh \
    install_kernel.sh \
    make_bundle.sh
do
    [ -f "$ROOT_DIR/tools/imx95/$script" ] || fail "missing tools/imx95/$script"
done

echo "PASS: linux layout checks"
