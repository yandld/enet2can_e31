#!/bin/sh
# install_kernel.sh - run ON the iMX95 board, from the extracted bundle dir.
# Installs the E2CF kernel Image (+ dtbs/modules when bundled).
#
#   ./install_kernel.sh [boot-mount-or-partition]   # auto-detects when omitted
#
# U-Boot on i.MX loads Image from the FAT partition 1 of the disk it booted
# from - NOT from /boot inside the rootfs. Auto-detection therefore derives
# the boot disk from root= in /proc/cmdline and uses <disk>p1, mounting it
# if nothing has mounted it yet. It never silently falls back to /boot.
#
# Safety: the first run keeps Image.orig / <dtb>.orig backups on the boot
# partition. Rollback = copy the .orig files back and reboot.
#
# SPDX-License-Identifier: GPL-2.0
set -e
cd "$(dirname "$0")"

[ -f Image ] || { echo "ERROR: Image not found - run from the extracted bundle"; exit 1; }

# --- locate the partition U-Boot really loads the kernel from -----------
BOOT="$1"
MNT_TMP=""
if [ -n "$BOOT" ] && [ -b "$BOOT" ]; then
    # a block device was passed - mount it ourselves
    MNT_TMP=/tmp/e2cf_boot
    mkdir -p "$MNT_TMP"
    mountpoint -q "$MNT_TMP" 2>/dev/null || mount "$BOOT" "$MNT_TMP"
    BOOT="$MNT_TMP"
fi
if [ -z "$BOOT" ]; then
    root_part=$(sed -n 's/.*root=\([^ ]*\).*/\1/p' /proc/cmdline)
    case "$root_part" in
        PARTUUID=*|UUID=*|LABEL=*) root_dev=$(blkid -t "$root_part" -o device 2>/dev/null || true) ;;
        *) root_dev="$root_part" ;;
    esac
    disk=""
    case "$root_dev" in
        /dev/mmcblk*p*) disk="${root_dev%p*}p1" ;;
        /dev/sd*)       disk="$(echo "$root_dev" | sed 's/[0-9]*$//')1" ;;
    esac
    if [ -n "$disk" ] && [ -b "$disk" ]; then
        echo ">>> boot disk derived from root=$root_part -> kernel partition $disk"
        BOOT=$(awk -v d="$disk" '$1==d{print $2; exit}' /proc/mounts)
        if [ -z "$BOOT" ]; then
            MNT_TMP=/tmp/e2cf_boot
            mkdir -p "$MNT_TMP"
            mount "$disk" "$MNT_TMP"
            BOOT="$MNT_TMP"
            echo ">>> $disk was not mounted - mounted at $MNT_TMP"
        fi
    fi
fi
if [ -z "$BOOT" ]; then
    # last resort: an already-mounted media partition that carries an Image.
    # (Deliberately NOT /boot: on i.MX the rootfs /boot is ignored by U-Boot.)
    for d in /run/media/*; do
        [ -f "$d/Image" ] && BOOT="$d" && break
    done
fi
[ -n "$BOOT" ] && [ -f "$BOOT/Image" ] || {
    echo "ERROR: kernel boot partition not found or it has no Image."
    echo "       Find it manually (cat /proc/cmdline; lsblk) and pass it:"
    echo "         ./install_kernel.sh /dev/mmcblkXp1     (partition), or"
    echo "         ./install_kernel.sh /run/media/<dir>   (mount point)"
    exit 1
}
echo ">>> boot partition: $BOOT"
echo ">>> on it now: $(cd "$BOOT" && ls Image* *.scr extlinux 2>/dev/null | tr '\n' ' ')"

# --- kernel image ------------------------------------------------------
[ -f "$BOOT/Image.orig" ] || cp "$BOOT/Image" "$BOOT/Image.orig"
cp Image "$BOOT/Image"
sync
echo ">>> Image installed (backup: Image.orig)"
echo ">>> verify: bundle    $(md5sum Image | cut -d' ' -f1)"
echo ">>>         installed $(md5sum "$BOOT/Image" | cut -d' ' -f1)"

# --- dtbs (optional - min bundle ships none, the on-board dtb is kept) --
if [ -d dtbs ]; then
    N=0
    for f in dtbs/*.dtb; do
        name=$(basename "$f")
        if [ -f "$BOOT/$name" ]; then
            [ -f "$BOOT/$name.orig" ] || cp "$BOOT/$name" "$BOOT/$name.orig"
            cp "$f" "$BOOT/$name"
            N=$((N + 1))
        fi
    done
    echo ">>> $N dtb(s) replaced (only names already present were touched)"
    [ "$N" -gt 0 ] || echo "WARNING: no dtb matched - check 'printenv fdtfile' in U-Boot"
else
    echo ">>> no dtbs in bundle - keeping the on-board dtb"
fi

# --- kernel modules (optional - everything E2CF needs is built-in) -------
if [ -f modules.tgz ]; then
    NEWVER=$(tar tzf modules.tgz | head -1 | sed 's#lib/modules/\([^/]*\).*#\1#')
    tar xzf modules.tgz -C /
    echo ">>> modules installed: /lib/modules/$NEWVER"
else
    echo ">>> no modules.tgz in bundle - skipped (CAN/ENETC/MMC are built-in;"
    echo ">>>   modprobe of unrelated =m drivers will fail on this kernel)"
fi

sync
[ -n "$MNT_TMP" ] && umount "$MNT_TMP" 2>/dev/null || true
echo ""
echo ">>> DONE. Now:   reboot"
echo ">>> after boot:  zcat /proc/config.gz | grep '^CONFIG_CAN_RAW'   -> must say =y"
echo ">>>              ./board_setup.sh <eth-if>"
echo ">>> rollback:    cp <boot>/Image.orig <boot>/Image && sync && reboot"
