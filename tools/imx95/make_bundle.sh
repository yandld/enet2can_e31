#!/bin/bash
# make_bundle.sh - pack what the board needs into one tarball.
#
#   ./make_bundle.sh        # minimal (default): Image + eth2can.ko + scripts
#                           #   -> e2cf_min_bundle.tgz  (~tens of MB)
#                           #   CAN/ENETC/PHY/MMC/ext4 are all built-in (=y),
#                           #   so neither modules nor dtbs are required when
#                           #   the board already runs a same-generation dtb.
#   ./make_bundle.sh full   # adds all imx95 dtbs + /lib/modules tgz
#                           #   -> e2cf_full_bundle.tgz (first flash / new board)
#
# SPDX-License-Identifier: GPL-2.0
set -e
source "$(dirname "$0")/env.sh"

MODE="${1:-min}"
case "$MODE" in min|full) ;; *) echo "usage: $0 [min|full]" >&2; exit 1;; esac

IMAGE="${KOUT}/arch/arm64/boot/Image"
DTBDIR="${KOUT}/arch/arm64/boot/dts/freescale"
[ -f "${IMAGE}" ] || { echo "build the kernel first: ./build_kernel.sh" >&2; exit 1; }
[ -f "${DRVDIR}/eth2can.ko" ] || { echo "build the driver first: ./build_driver.sh" >&2; exit 1; }

OUT="${DRVDIR}/e2cf_${MODE}_bundle.tgz"
STAGE=$(mktemp -d)
trap 'rm -rf "${STAGE}"' EXIT
mkdir -p "${STAGE}/bundle"

cp "${IMAGE}" "${STAGE}/bundle/"
cp "${DRVDIR}/eth2can.ko" "${STAGE}/bundle/"
cp "$(dirname "$0")/install_kernel.sh" "$(dirname "$0")/board_setup.sh" "${STAGE}/bundle/"
chmod +x "${STAGE}/bundle/"*.sh

if [ "$MODE" = "full" ]; then
    mkdir -p "${STAGE}/bundle/dtbs"
    cp "${DTBDIR}"/imx95-*.dtb "${STAGE}/bundle/dtbs/"
    echo ">>> staging kernel modules (modules_install)"
    make -s -C "${KSRC}" O="${KOUT}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" \
        INSTALL_MOD_PATH="${STAGE}/mods" modules_install
    rm -f "${STAGE}"/mods/lib/modules/*/build   # dangling host symlink
    tar czf "${STAGE}/bundle/modules.tgz" -C "${STAGE}/mods" lib
fi

cat > "${STAGE}/bundle/README.txt" <<EOF
E2CF board bundle (${MODE})  ($(date +%F))
kernel: $(make -s -C "${KSRC}" O="${KOUT}" ARCH="${ARCH}" kernelrelease 2>/dev/null)
CAN core/raw/bcm/gw/vcan/flexcan + ENETC4 + PHY + MMC are BUILT-IN (=y);
eth2can.ko is the only module to load.

On the board:
  1. ./install_kernel.sh        # installs Image (+dtb/modules if bundled), keeps .orig backups
  2. reboot
  3. ./board_setup.sh <eth-if>  # insmod eth2can, brings up 6 CAN channels
Rollback: cp <boot>/Image.orig <boot>/Image && sync && reboot
EOF

tar czf "${OUT}" -C "${STAGE}/bundle" .
echo ">>> bundle ready:"
ls -lh "${OUT}"
tar tzf "${OUT}"
