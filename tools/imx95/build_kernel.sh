#!/bin/bash
# build_kernel.sh - configure / build the real-time-edge kernel for iMX95
#
# Usage:
#   ./build_kernel.sh prepare     # defconfig + modules_prepare only (fast,
#                                 #   enough to compile out-of-tree modules)
#   ./build_kernel.sh             # full build: Image + dtbs + modules
#   ./build_kernel.sh menuconfig  # interactive config on top of current
#   ./build_kernel.sh clean       # wipe the output dir
#
# Output goes to $KOUT (separate O= dir, source tree stays pristine).
# Artifacts after a full build:
#   $KOUT/arch/arm64/boot/Image
#   $KOUT/arch/arm64/boot/dts/freescale/imx95-*-evk*.dtb
#   modules under $KOUT (deploy with deploy.sh or modules_install)
#
# SPDX-License-Identifier: GPL-2.0
set -e
source "$(dirname "$0")/env.sh"

MAKE="make -C ${KSRC} O=${KOUT} ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} -j${JOBS}"

configure()
{
    if [ ! -f "${KOUT}/.config" ]; then
        echo ">>> first-time config: imx_v8_defconfig"
        ${MAKE} imx_v8_defconfig
    fi
    # E2CF policy: every CAN dependency is BUILT-IN (=y) - only the
    # out-of-tree eth2can.ko itself stays a module. CAN_DEV provides the
    # candev core symbols; RAW/VCAN/GW are for board-side testing.
    "${KSRC}/scripts/config" --file "${KOUT}/.config" \
        -e CAN -e CAN_DEV -e CAN_RAW -e CAN_VCAN -e CAN_GW \
        -e CAN_BCM -e CAN_FLEXCAN
    ${MAKE} olddefconfig
}

case "${1:-all}" in
    prepare)
        configure
        echo ">>> modules_prepare (out-of-tree module build support)"
        ${MAKE} modules_prepare
        echo ">>> done. Build the driver with ./build_driver.sh"
        ;;
    menuconfig)
        configure
        ${MAKE} menuconfig
        ;;
    clean)
        rm -rf "${KOUT}"
        echo ">>> ${KOUT} removed"
        ;;
    all)
        configure
        echo ">>> full build: Image dtbs modules  (-j${JOBS})"
        time ${MAKE} Image dtbs modules
        echo ">>> artifacts:"
        ls -lh "${KOUT}/arch/arm64/boot/Image"
        ls "${KOUT}/arch/arm64/boot/dts/freescale/" | grep -E "imx95.*evk.*\.dtb$" | head -5
        ;;
    *)
        echo "usage: $0 [prepare|menuconfig|clean|all]" >&2
        exit 1
        ;;
esac
