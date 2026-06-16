#!/bin/bash
# build_driver.sh - build eth2can.ko
#
# Usage:
#   ./build_driver.sh         # cross-compile against the RTE 6.18 tree
#                             #   (runs build_kernel.sh prepare if needed)
#   ./build_driver.sh host    # compile against the running host kernel
#                             #   (quick API/syntax check, x86)
#   ./build_driver.sh clean
#
# SPDX-License-Identifier: GPL-2.0
set -e
source "$(dirname "$0")/env.sh"

case "${1:-cross}" in
    host)
        # env.sh exports ARCH/CROSS_COMPILE for the cross build; they must be
        # UNSET (not empty strings) so kbuild autodetects the host arch.
        env -u ARCH -u CROSS_COMPILE \
            make -C "${DRVDIR}" KDIR="/lib/modules/$(uname -r)/build" clean all
        ;;
    clean)
        make -C "${DRVDIR}" KDIR="${KOUT}" clean || true
        ;;
    cross)
        if [ ! -f "${KOUT}/include/generated/autoconf.h" ]; then
            echo ">>> kernel not prepared yet - running build_kernel.sh prepare"
            "$(dirname "$0")/build_kernel.sh" prepare
        fi
        # Module.symvers only exists after a FULL kernel build. With just
        # modules_prepare, downgrade modpost symbol errors to warnings -
        # the .ko still resolves everything at insmod time.
        MODPOST_ARG=""
        if [ ! -s "${KOUT}/Module.symvers" ]; then
            echo ">>> note: no Module.symvers (prepare-only kernel) - modpost warn mode"
            MODPOST_ARG="KBUILD_MODPOST_WARN=1"
        fi
        make -C "${KSRC}" O="${KOUT}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
            M="${DRVDIR}" -j"${JOBS}" ${MODPOST_ARG} modules
        echo ">>> built:"
        ls -l "${DRVDIR}/eth2can.ko"
        file "${DRVDIR}/eth2can.ko" | sed 's/, BuildID.*//'
        "${CROSS_COMPILE}"strip --strip-debug -o "${DRVDIR}/eth2can.stripped.ko" "${DRVDIR}/eth2can.ko" 2>/dev/null || true
        ;;
    *)
        echo "usage: $0 [cross|host|clean]" >&2
        exit 1
        ;;
esac
