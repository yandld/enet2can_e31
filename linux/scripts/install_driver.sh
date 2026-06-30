#!/bin/sh
# install_driver.sh - build and start eth2can on the running Linux kernel.
#
# SPDX-License-Identifier: GPL-2.0
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LINUX_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
KO="$LINUX_DIR/eth2can.ko"

KREL=$(uname -r)
KDIR_EXPLICIT=0
if [ "${KDIR+x}" = x ]; then
    KDIR_EXPLICIT=1
fi
KDIR=${KDIR:-/lib/modules/$KREL/build}
NET_CLASS=${E2CF_NET_CLASS:-/sys/class/net}

IFNAME=
VID=-1
PEER=
BITRATE=1000000
DBITRATE=8000000
CHANNELS=0-5
NO_DEPS=0
NO_LOAD=0
NO_CONFIGURE=0
DRY_RUN=0
VERBOSE=0
HB_DMESG_BASE=0
REQUIRE_GATEWAY=0

usage()
{
    cat <<EOF
Usage:
  sh linux/scripts/install_driver.sh [options]

Build eth2can.ko for the running kernel, load it, and configure SocketCAN
interfaces for the current boot. This script does not install DKMS or systemd
units by default.

Options:
  --ifname IFACE       Ethernet interface used by eth2can. Auto-detected by default.
  --vid VID            VLAN id passed to the module. Default: -1.
  --peer MAC           Static peer MAC passed to the module. Default: learn from HB.
  --bitrate N          Nominal CAN bitrate. Default: 1000000.
  --dbitrate N         CAN FD data bitrate. Default: 8000000.
  --channels LIST      CAN channel list, for example 0-5 or 0,2-3. Default: 0-5.
  --no-deps            Do not try to install build/runtime packages.
  --no-load            Build only; do not modprobe/insmod eth2can.
  --no-configure       Do not configure eth2canN SocketCAN interfaces.
  --require-gateway    Fail if the MCXE31B heartbeat is not observed.
  --dry-run            Print actions without changing the system.
  --verbose            Print commands before executing them.
  -h, --help           Show this help.

Environment:
  KDIR=/path/to/kernel/build
      Kernel build directory. Default: /lib/modules/\$(uname -r)/build

Examples:
  sh linux/scripts/install_driver.sh
  sh linux/scripts/install_driver.sh --ifname eth1 --vid 100
  KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname end0
EOF
}

die()
{
    echo "ERROR: $*" >&2
    exit 1
}

warn()
{
    echo "WARNING: $*" >&2
}

info()
{
    echo ">>> $*"
}

have_cmd()
{
    command -v "$1" >/dev/null 2>&1
}

print_cmd()
{
    prefix=$1
    shift
    printf '%s' "$prefix"
    for arg in "$@"; do
        printf ' %s' "$arg"
    done
    printf '\n'
}

run()
{
    if [ "$DRY_RUN" -eq 1 ]; then
        print_cmd "DRY-RUN:" "$@"
        return 0
    fi
    if [ "$VERBOSE" -eq 1 ]; then
        print_cmd "RUN:" "$@"
    fi
    "$@"
}

run_root()
{
    if [ "$DRY_RUN" -eq 1 ]; then
        run "$@"
        return 0
    fi
    if [ "$(id -u)" = "0" ]; then
        run "$@"
        return $?
    fi
    if have_cmd sudo; then
        run sudo "$@"
        return $?
    fi
    die "root privileges are required; re-run as root or install sudo"
}

try_run_root()
{
    if [ "$DRY_RUN" -eq 1 ]; then
        run "$@"
        return 0
    fi
    run_root "$@" >/dev/null 2>&1 || true
}

try_pkg()
{
    if run_root "$@"; then
        return 0
    fi
    warn "package command failed: $*"
    warn "continuing with explicit local checks"
    return 1
}

need_arg()
{
    opt=$1
    val=${2:-}
    [ -n "$val" ] || die "$opt requires an argument"
}

is_uint()
{
    case ${1:-} in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

validate_bitrate()
{
    is_uint "$2" || die "$1 must be a positive integer"
    [ "$2" -gt 0 ] || die "$1 must be greater than zero"
}

validate_channel()
{
    is_uint "$1" || die "invalid channel '$1'"
    [ "$1" -ge 0 ] && [ "$1" -le 5 ] || die "channel '$1' is outside supported range 0-5"
}

expand_channels()
{
    list=$1
    old_ifs=$IFS
    IFS=,
    # shellcheck disable=SC2086
    set -- $list
    IFS=$old_ifs

    for item in "$@"; do
        case $item in
            *-*)
                start=${item%-*}
                end=${item#*-}
                validate_channel "$start"
                validate_channel "$end"
                [ "$start" -le "$end" ] || die "invalid channel range '$item'"
                n=$start
                while [ "$n" -le "$end" ]; do
                    echo "$n"
                    n=$((n + 1))
                done
                ;;
            *)
                validate_channel "$item"
                echo "$item"
                ;;
        esac
    done
}

parse_args()
{
    while [ "$#" -gt 0 ]; do
        case $1 in
            --ifname)
                need_arg "$1" "${2:-}"
                IFNAME=$2
                shift 2
                ;;
            --ifname=*)
                IFNAME=${1#*=}
                shift
                ;;
            --vid)
                need_arg "$1" "${2:-}"
                VID=$2
                shift 2
                ;;
            --vid=*)
                VID=${1#*=}
                shift
                ;;
            --peer)
                need_arg "$1" "${2:-}"
                PEER=$2
                shift 2
                ;;
            --peer=*)
                PEER=${1#*=}
                shift
                ;;
            --bitrate)
                need_arg "$1" "${2:-}"
                BITRATE=$2
                shift 2
                ;;
            --bitrate=*)
                BITRATE=${1#*=}
                shift
                ;;
            --dbitrate)
                need_arg "$1" "${2:-}"
                DBITRATE=$2
                shift 2
                ;;
            --dbitrate=*)
                DBITRATE=${1#*=}
                shift
                ;;
            --channels)
                need_arg "$1" "${2:-}"
                CHANNELS=$2
                shift 2
                ;;
            --channels=*)
                CHANNELS=${1#*=}
                shift
                ;;
            --no-deps)
                NO_DEPS=1
                shift
                ;;
            --no-load)
                NO_LOAD=1
                shift
                ;;
            --no-configure)
                NO_CONFIGURE=1
                shift
                ;;
            --require-gateway)
                REQUIRE_GATEWAY=1
                shift
                ;;
            --dry-run)
                DRY_RUN=1
                shift
                ;;
            --verbose)
                VERBOSE=1
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                die "unknown option: $1"
                ;;
        esac
    done
}

detect_iface()
{
    if [ -n "$IFNAME" ]; then
        echo "$IFNAME"
        return
    fi

    for want_carrier in 1 0; do
        for path in "$NET_CLASS"/*; do
            [ -e "$path" ] || continue
            name=${path##*/}
            [ "$name" = "lo" ] && continue
            [ -d "$path/wireless" ] && continue
            if [ -r "$path/type" ] && [ "$(cat "$path/type" 2>/dev/null || echo 0)" != "1" ]; then
                continue
            fi
            link=$(readlink "$path" 2>/dev/null || echo "")
            case $link in
                *virtual*) continue ;;
            esac
            if [ "$want_carrier" -eq 1 ]; then
                [ -r "$path/carrier" ] || continue
                [ "$(cat "$path/carrier" 2>/dev/null || echo 0)" = "1" ] || continue
            fi
            echo "$name"
            return
        done
    done

    echo eth0
}

is_raspberry_pi()
{
    for file in /proc/device-tree/model /sys/firmware/devicetree/base/model /proc/cpuinfo; do
        [ -r "$file" ] || continue
        if grep -qi "raspberry" "$file" 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

kdir_exists()
{
    [ -d "$KDIR" ] && [ -f "$KDIR/Makefile" ]
}

kdir_candidate_ok()
{
    [ -d "$1" ] && [ -f "$1/Makefile" ]
}

find_kernel_build_dir()
{
    [ "$KDIR_EXPLICIT" -eq 0 ] || return 1

    roots=${E2CF_HEADER_SEARCH_ROOTS:-/usr/src}
    old_ifs=$IFS
    IFS=' '
    # shellcheck disable=SC2086
    set -- $roots
    IFS=$old_ifs

    for root in "$@"; do
        [ -n "$root" ] || continue
        for cand in \
            "$root/linux-headers-$KREL" \
            "$root/linux-headers-$KREL/build"
        do
            if kdir_candidate_ok "$cand"; then
                echo "$cand"
                return 0
            fi
        done

        for cand in "$root"/linux-headers-"$KREL"* "$root"/linux-headers-*"$KREL"*; do
            [ -e "$cand" ] || continue
            if kdir_candidate_ok "$cand"; then
                echo "$cand"
                return 0
            fi
            if kdir_candidate_ok "$cand/build"; then
                echo "$cand/build"
                return 0
            fi
        done
    done

    return 1
}

resolve_kdir()
{
    kdir_exists && return 0

    found=$(find_kernel_build_dir 2>/dev/null || true)
    [ -n "$found" ] || return 1

    KDIR=$found
    info "using kernel build directory: $KDIR"
    return 0
}

kernel_family()
{
    family=${KREL%%-*}
    family=${family%+}
    echo "$family"
}

nearby_header_dirs()
{
    family=$(kernel_family)
    roots=${E2CF_HEADER_SEARCH_ROOTS:-/usr/src}
    old_ifs=$IFS
    IFS=' '
    # shellcheck disable=SC2086
    set -- $roots
    IFS=$old_ifs

    for root in "$@"; do
        [ -n "$root" ] || continue
        for cand in "$root"/linux-headers-"$family"*; do
            [ -e "$cand" ] || continue
            case $cand in
                *"linux-headers-$KREL"|*"linux-headers-$KREL"/build) continue ;;
            esac
            [ -d "$cand" ] && echo "$cand"
        done
    done | sort -u
}

install_deps()
{
    [ "$NO_DEPS" -eq 0 ] || return 0

    need_install=0
    for cmd in make gcc modinfo modprobe insmod rmmod ip; do
        have_cmd "$cmd" || need_install=1
    done
    kdir_exists || need_install=1
    [ "$need_install" -eq 1 ] || return 0

    if have_cmd apt-get; then
        pkgs="make gcc kmod iproute2 can-utils"
        if ! kdir_exists; then
            if is_raspberry_pi; then
                pkgs="$pkgs raspberrypi-kernel-headers"
            else
                pkgs="$pkgs linux-headers-$KREL"
            fi
        fi
        info "installing packages with apt-get"
        try_pkg apt-get update || true
        # shellcheck disable=SC2086
        try_pkg apt-get install -y $pkgs || true
        return 0
    fi

    if have_cmd dnf; then
        pkgs="make gcc kmod iproute iproute-tc can-utils"
        [ -n "$pkgs" ] || true
        if ! kdir_exists; then
            pkgs="$pkgs kernel-devel-$KREL"
        fi
        info "installing packages with dnf"
        # shellcheck disable=SC2086
        if ! try_pkg dnf install -y $pkgs; then
            try_pkg dnf install -y make gcc kmod iproute can-utils kernel-devel || true
        fi
        return 0
    fi

    if have_cmd yum; then
        pkgs="make gcc kmod iproute can-utils"
        if ! kdir_exists; then
            pkgs="$pkgs kernel-devel-$KREL"
        fi
        info "installing packages with yum"
        # shellcheck disable=SC2086
        if ! try_pkg yum install -y $pkgs; then
            try_pkg yum install -y make gcc kmod iproute can-utils kernel-devel || true
        fi
        return 0
    fi

    if have_cmd pacman; then
        pkgs="base-devel kmod iproute2 can-utils"
        if ! kdir_exists; then
            pkgs="$pkgs linux-headers"
        fi
        info "installing packages with pacman"
        # shellcheck disable=SC2086
        try_pkg pacman -S --needed --noconfirm $pkgs || true
        return 0
    fi

    if have_cmd zypper; then
        pkgs="make gcc kmod iproute2 can-utils"
        if ! kdir_exists; then
            pkgs="$pkgs kernel-default-devel"
        fi
        info "installing packages with zypper"
        # shellcheck disable=SC2086
        try_pkg zypper --non-interactive install $pkgs || true
        return 0
    fi

    warn "no supported package manager found; continuing with current system"
}

explain_missing_kdir()
{
    nearby=$(nearby_header_dirs)

    cat >&2 <<EOF
ERROR: kernel build directory not found: $KDIR
running kernel: $KREL
EOF

    if [ -n "$nearby" ]; then
        cat >&2 <<EOF

nearby kernel header directories were found, but none matches the running
kernel exactly; do not build eth2can.ko against these mismatched headers:
EOF
        echo "$nearby" | sed 's/^/  /' >&2
        cat >&2 <<EOF

On Raspberry Pi OS this commonly means the board booted a -v8+ 64-bit kernel
while the installed headers only cover 32-bit kernel variants. Use one of:
  1. Boot a kernel variant that has matching headers, then reboot and confirm
     uname -r matches one of /usr/src/linux-headers-*.
  2. Install or switch to a Raspberry Pi OS/kernel package that provides
     /usr/src/linux-headers-$KREL.
  3. Build/prepare the exact Raspberry Pi kernel tree and rerun with KDIR=...
EOF
    fi

    cat >&2 <<EOF

Install kernel headers/build files that exactly match the running kernel:
  Debian/Ubuntu:      sudo apt-get install linux-headers-$KREL
  Raspberry Pi OS:    sudo apt-get install raspberrypi-kernel-headers
  Fedora/RHEL/Rocky:  sudo dnf install kernel-devel-$KREL
  Arch/Manjaro:       sudo pacman -S linux-headers
  openSUSE:           sudo zypper install kernel-default-devel

For RK/i.MX/vendor BSP kernels, install the vendor kernel headers or point KDIR
to the prepared kernel build tree:
  KDIR=/path/to/kernel/build sh linux/scripts/install_driver.sh --ifname eth0
EOF
    exit 1
}

require_cmd()
{
    have_cmd "$1" || die "required command not found: $1"
}

require_commands()
{
    [ "$DRY_RUN" -eq 0 ] || return 0
    require_cmd make
    require_cmd gcc
    require_cmd modinfo
    if [ "$NO_LOAD" -eq 0 ]; then
        require_cmd modprobe
        require_cmd insmod
        require_cmd rmmod
    fi
    if [ "$NO_CONFIGURE" -eq 0 ]; then
        require_cmd ip
    fi
}

kernel_config_path()
{
    if [ -n "${E2CF_KERNEL_CONFIG:-}" ]; then
        [ -r "$E2CF_KERNEL_CONFIG" ] && echo "$E2CF_KERNEL_CONFIG"
        return 0
    fi
    if [ -r /proc/config.gz ]; then
        echo /proc/config.gz
        return 0
    fi
    if [ -r "/boot/config-$KREL" ]; then
        echo "/boot/config-$KREL"
        return 0
    fi
    return 0
}

read_kernel_config()
{
    cfg=$1
    case $cfg in
        *.gz|/proc/config.gz)
            gzip -dc "$cfg" 2>/dev/null || zcat "$cfg"
            ;;
        *)
            cat "$cfg"
            ;;
    esac
}

config_value()
{
    cfg=$1
    sym=$2
    read_kernel_config "$cfg" | sed -n "s/^$sym=//p" | sed -n '1p'
}

check_can_config()
{
    cfg=$(kernel_config_path)
    if [ -z "$cfg" ]; then
        warn "kernel config not found; trying SocketCAN modules at load time"
        return 0
    fi

    for sym in CONFIG_CAN CONFIG_CAN_RAW CONFIG_CAN_DEV; do
        val=$(config_value "$cfg" "$sym")
        case $val in
            y|m) ;;
            *)
                if [ "$DRY_RUN" -eq 1 ]; then
                    warn "$sym is not enabled in $cfg; real install would require SocketCAN support"
                else
                    die "$sym is not enabled in $cfg; enable SocketCAN in the kernel or boot a kernel with CAN support"
                fi
                ;;
        esac
    done
}

build_module()
{
    info "building eth2can.ko"
    run make -C "$LINUX_DIR" KDIR="$KDIR" all
    if [ "$DRY_RUN" -eq 0 ] && [ ! -f "$KO" ]; then
        die "build finished but $KO was not created"
    fi
}

check_vermagic()
{
    [ "$DRY_RUN" -eq 0 ] || return 0
    [ -f "$KO" ] || die "module not found: $KO"

    modver=$(modinfo -F vermagic "$KO" 2>/dev/null | sed 's/ .*//')
    [ -n "$modver" ] || die "could not read module vermagic from $KO"
    if [ "$modver" != "$KREL" ]; then
        die "module built for '$modver' but target kernel is '$KREL'; rebuild on this target"
    fi
}

load_module()
{
    [ "$NO_LOAD" -eq 0 ] || {
        info "module load skipped"
        return 0
    }

    info "loading eth2can.ko"
    check_can_config
    try_run_root modprobe can_dev
    try_run_root modprobe can
    try_run_root modprobe can_raw
    try_run_root rmmod eth2can

    set -- "$KO" "ifname=$IFNAME" "vid=$VID"
    if [ -n "$PEER" ]; then
        set -- "$@" "peer=$PEER"
    fi
    run_root insmod "$@"
    try_run_root ip link set "$IFNAME" up
}

capture_gateway_baseline()
{
    [ "$DRY_RUN" -eq 0 ] || return 0
    have_cmd dmesg || return 0

    HB_DMESG_BASE=$(dmesg 2>/dev/null | grep -c 'eth2can: gateway alive' || true)
}

gateway_carrier_up()
{
    carrier=$NET_CLASS/eth2can0/carrier
    [ -r "$carrier" ] || return 1
    [ "$(cat "$carrier" 2>/dev/null || echo 0)" = "1" ]
}

wait_gateway()
{
    [ "$NO_LOAD" -eq 0 ] || return 0
    [ "$DRY_RUN" -eq 0 ] || {
        echo "DRY-RUN: wait for eth2can gateway alive"
        return 0
    }

    n=0
    limit=${E2CF_HB_WAIT_LOOPS:-20}
    while [ "$n" -lt "$limit" ]; do
        if gateway_carrier_up; then
            return 0
        fi
        if have_cmd dmesg; then
            alive=$(dmesg 2>/dev/null | grep -c 'eth2can: gateway alive' || true)
            [ "$alive" -gt "$HB_DMESG_BASE" ] && return 0
        fi
        sleep 0.1 2>/dev/null || sleep 1
        n=$((n + 1))
    done
    return 1
}

explain_no_gateway()
{
    level=$1
    cat >&2 <<EOF
$level: gateway heartbeat was not observed on $IFNAME.

The kernel module loaded, but no E2CF heartbeat frame arrived from the MCXE31B
gateway, so channel configuration would only time out. Check the link before
bringing up eth2can channels:
  ip -br link show $IFNAME
  sudo dmesg | tail -80
  sudo tcpdump -eni $IFNAME 'ether proto 0x88b5 or (vlan and ether proto 0x88b5)'

Expected traffic: EtherType 0x88b5 heartbeat frames every 100 ms. If tcpdump
sees nothing, check MCU power, cable, selected interface, switch/VLAN path, and
that the raw-Ethernet MCXE31B firmware is running.
EOF
}

configure_channels()
{
    [ "$NO_CONFIGURE" -eq 0 ] || {
        info "SocketCAN configuration skipped"
        return 0
    }

    info "configuring SocketCAN channels"
    ok=0
    total=0
    for ch in $(expand_channels "$CHANNELS"); do
        total=$((total + 1))
        dev=eth2can$ch
        if run_root ip link set "$dev" type can bitrate "$BITRATE" dbitrate "$DBITRATE" fd on &&
           run_root ip link set "$dev" up; then
            ok=$((ok + 1))
        else
            warn "failed to configure $dev"
        fi
    done

    info "$ok/$total channels configured"
    if [ "$DRY_RUN" -eq 0 ] && [ "$total" -gt 0 ] && [ "$ok" -eq 0 ]; then
        die "no eth2can channels were configured"
    fi
}

show_plan()
{
    info "plan"
    echo "kernel: $KREL"
    echo "KDIR: $KDIR"
    echo "module: $KO"
    echo "interface: $IFNAME"
    echo "vid: $VID"
    [ -z "$PEER" ] || echo "peer: $PEER"
    echo "channels: $CHANNELS"
}

show_status()
{
    info "status"
    [ "$DRY_RUN" -eq 0 ] || return 0
    if have_cmd ip; then
        ip -br link show type can 2>/dev/null || true
    fi
    if have_cmd dmesg; then
        dmesg 2>/dev/null | grep -i 'eth2can\|e2cf' | tail -8 || true
    fi
}

main()
{
    parse_args "$@"
    validate_bitrate --bitrate "$BITRATE"
    validate_bitrate --dbitrate "$DBITRATE"
    expand_channels "$CHANNELS" >/dev/null

    [ "$(uname)" = "Linux" ] || die "this installer must run on Linux"
    IFNAME=$(detect_iface)

    resolve_kdir || true
    install_deps
    resolve_kdir || true
    show_plan
    kdir_exists || explain_missing_kdir
    require_commands
    build_module
    check_vermagic
    capture_gateway_baseline
    load_module
    if ! wait_gateway; then
        if [ "$REQUIRE_GATEWAY" -eq 1 ]; then
            explain_no_gateway ERROR
            exit 1
        fi
        if [ "$NO_CONFIGURE" -eq 0 ]; then
            explain_no_gateway WARNING
            warn "driver build/load finished; SocketCAN channel configuration skipped"
            NO_CONFIGURE=1
        else
            warn "gateway heartbeat was not observed"
        fi
    fi
    configure_channels
    show_status
}

main "$@"
