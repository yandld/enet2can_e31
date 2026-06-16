#!/bin/bash
# deploy.sh - push artifacts to the iMX95 board and (re)load the driver
#
# Usage:
#   ./deploy.sh drv  [eth-if] # scp eth2can.ko, rmmod+insmod, bring up 6 ch
#   ./deploy.sh kernel        # scp Image + imx95 evk dtbs to /run/media boot
#                             #   partition path on the board (adjust below)
#
#   BOARD_IP=192.168.x.x ./deploy.sh drv eth1
#
# SPDX-License-Identifier: GPL-2.0
set -e
source "$(dirname "$0")/env.sh"

# Password auth via sshpass when BOARD_PASS is set and no key works.
SSHOPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=5"
if [ -n "${BOARD_PASS}" ] && command -v sshpass >/dev/null; then
    SSH="sshpass -p ${BOARD_PASS} ssh ${SSHOPTS} ${BOARD_USER}@${BOARD_IP}"
    SCP="sshpass -p ${BOARD_PASS} scp ${SSHOPTS}"
else
    SSH="ssh ${SSHOPTS} ${BOARD_USER}@${BOARD_IP}"
    SCP="scp ${SSHOPTS}"
fi

case "${1:-drv}" in
    drv)
        ETHIF="${2:-eth0}"
        [ -f "${DRVDIR}/eth2can.ko" ] || { echo "build first: ./build_driver.sh" >&2; exit 1; }
        ${SSH} "mkdir -p ${BOARD_DIR}"
        ${SCP} "${DRVDIR}/eth2can.ko" "$(dirname "$0")/board_setup.sh" \
               "${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/"
        ${SSH} "sh ${BOARD_DIR}/board_setup.sh ${ETHIF}"
        echo ">>> driver deployed and loaded on ${BOARD_IP} (lower if: ${ETHIF})"
        ;;
    kernel)
        BOOTDIR="${BOOTDIR:-/run/media/boot-mmcblk0p1}"
        [ -f "${KOUT}/arch/arm64/boot/Image" ] || { echo "build first: ./build_kernel.sh" >&2; exit 1; }
        ${SCP} "${KOUT}/arch/arm64/boot/Image" "${BOARD_USER}@${BOARD_IP}:${BOOTDIR}/"
        ${SCP} "${KOUT}"/arch/arm64/boot/dts/freescale/imx95-19x19-evk*.dtb \
               "${BOARD_USER}@${BOARD_IP}:${BOOTDIR}/" 2>/dev/null || true
        ${SSH} "sync"
        echo ">>> kernel deployed to ${BOARD_IP}:${BOOTDIR} - reboot the board"
        ;;
    *)
        echo "usage: $0 [drv [eth-if]|kernel]" >&2
        exit 1
        ;;
esac
