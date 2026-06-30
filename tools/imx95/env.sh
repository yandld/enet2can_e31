#!/bin/bash
# env.sh - shared settings for the E2CF Linux build scripts (source me)
#
# All paths/tools resolved here so an SDK or kernel upgrade is a one-file
# change. Override any variable from the environment, e.g.:
#   KOUT=/tmp/kbuild ./build_kernel.sh
#
# SPDX-License-Identifier: GPL-2.0

# --- target kernel source tree (real-time-edge 6.18) ---------------------
export KSRC="${KSRC:-$(realpath "$(dirname "${BASH_SOURCE[0]}")/../../../real-time-edge-linux")}"

# --- separate build output dir (keeps the source tree clean) -------------
export KOUT="${KOUT:-${KSRC}/build_imx95}"

# --- cross toolchain ------------------------------------------------------
# Bare-metal-rootfs-independent GCC; the kernel does not need the Yocto
# sysroot. (The Yocto SDK env script would inject sysroot cflags that break
# kbuild - do not source it for kernel/module builds.)
TOOLCHAIN_DIR="${TOOLCHAIN_DIR:-/opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-linux-gnu}"
export PATH="${TOOLCHAIN_DIR}/bin:${PATH}"
export ARCH=arm64
export CROSS_COMPILE="${CROSS_COMPILE:-aarch64-none-linux-gnu-}"

export JOBS="${JOBS:-$(nproc)}"

# --- repository / driver location ----------------------------------------
export REPO_ROOT="$(realpath "$(dirname "${BASH_SOURCE[0]}")/../..")"
export DRVDIR="${REPO_ROOT}/linux"

# --- board deploy target (adjust to your bench) ---------------------------
export BOARD_IP="${BOARD_IP:-10.17.18.9}"
export BOARD_USER="${BOARD_USER:-root}"
export BOARD_PASS="${BOARD_PASS:-root}"   # used via sshpass when ssh keys absent
export BOARD_DIR="${BOARD_DIR:-/home/root/e2cf}"

# sanity
[ -f "${KSRC}/Makefile" ] || { echo "ERROR: kernel source not found at KSRC=${KSRC}" >&2; return 1 2>/dev/null || exit 1; }
command -v "${CROSS_COMPILE}gcc" >/dev/null || { echo "ERROR: ${CROSS_COMPILE}gcc not in PATH (TOOLCHAIN_DIR=${TOOLCHAIN_DIR})" >&2; return 1 2>/dev/null || exit 1; }
