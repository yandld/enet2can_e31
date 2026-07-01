#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SCRIPT="$ROOT_DIR/linux/scripts/install_driver.sh"

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
        cat "$file" >&2 || true
        fail "did not expect to find: $pattern"
    fi
}

make_mock_bin()
{
    mock_bin=$1
    log_file=$2
    mkdir -p "$mock_bin"

    cat >"$mock_bin/uname" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "-r" ]; then
    echo "${E2CF_TEST_UNAME_R:-6.1.2-test}"
else
    echo Linux
fi
EOF
    chmod +x "$mock_bin/uname"

    cat >"$mock_bin/make" <<EOF
#!/bin/sh
echo "make \$*" >>"$log_file"
out_dir=
while [ \$# -gt 0 ]; do
    if [ "\$1" = "-C" ]; then
        shift
        out_dir=\${1:-}
    fi
    shift || break
done
if [ -n "\$out_dir" ] && [ ! -e "\$out_dir/eth2can.ko" ]; then
    : >"\$out_dir/eth2can.ko"
    : >"$tmp/mock-ko-created"
fi
exit 0
EOF
    chmod +x "$mock_bin/make"

    cat >"$mock_bin/modinfo" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "-F" ] && [ "${2:-}" = "vermagic" ]; then
    echo "${E2CF_TEST_UNAME_R:-6.1.2-test} SMP"
    exit 0
fi
echo "vermagic: ${E2CF_TEST_UNAME_R:-6.1.2-test} SMP"
EOF
    chmod +x "$mock_bin/modinfo"

    for cmd in apt-get dnf yum pacman zypper modprobe rmmod insmod ip dmesg sudo; do
        cat >"$mock_bin/$cmd" <<EOF
#!/bin/sh
echo "$cmd \$*" >>"$log_file"
exit 0
EOF
        chmod +x "$mock_bin/$cmd"
    done
}

tmp=${TMPDIR:-/tmp}/e2cf-install-test.$$
cleanup()
{
    if [ -f "$tmp/mock-ko-created" ]; then
        rm -f "$ROOT_DIR/linux/eth2can.ko"
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT INT HUP TERM
mkdir -p "$tmp"

test_help_lists_portable_options()
{
    out=$tmp/help.out
    sh "$SCRIPT" --help >"$out"
    assert_contains "$out" "Usage:"
    assert_contains "$out" "install_driver.sh"
    assert_contains "$out" "--ifname IFACE"
    assert_contains "$out" "--dry-run"
    assert_contains "$out" "--require-gateway"
    assert_contains "$out" "--dbitrate N         CAN FD data bitrate. Default: 5000000."
    assert_contains "$out" "KDIR=/path/to/kernel/build"
}

test_auto_detect_skips_wireless_and_prefers_carrier_up_wired()
{
    mock_bin=$tmp/iface-bin
    log_file=$tmp/iface.log
    out=$tmp/iface.out
    config=$tmp/iface.config
    kdir=$tmp/iface-kdir
    net_class=$tmp/net-class

    : >"$log_file"
    mkdir -p "$kdir" "$net_class/wlan0/wireless" "$net_class/end0" "$net_class/eth9"
    : >"$kdir/Makefile"
    echo 1 >"$net_class/wlan0/type"
    echo 1 >"$net_class/wlan0/carrier"
    echo 1 >"$net_class/end0/type"
    echo 0 >"$net_class/end0/carrier"
    echo 1 >"$net_class/eth9/type"
    echo 1 >"$net_class/eth9/carrier"
    cat >"$config" <<'EOF'
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_DEV=y
EOF
    make_mock_bin "$mock_bin" "$log_file"

    PATH="$mock_bin:$PATH" \
    KDIR="$kdir" \
    E2CF_NET_CLASS="$net_class" \
    E2CF_KERNEL_CONFIG="$config" \
    sh "$SCRIPT" --dry-run --no-deps --no-load >"$out"

    assert_contains "$out" "interface: eth9"
}

test_dry_run_uses_overrides_without_side_effect_commands()
{
    mock_bin=$tmp/mock-bin
    log_file=$tmp/mock.log
    out=$tmp/dry-run.out
    config=$tmp/config
    kdir=$tmp/kdir

    : >"$log_file"
    mkdir -p "$kdir"
    : >"$kdir/Makefile"
    cat >"$config" <<'EOF'
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_DEV=y
EOF
    make_mock_bin "$mock_bin" "$log_file"

    PATH="$mock_bin:$PATH" \
    KDIR="$kdir" \
    E2CF_KERNEL_CONFIG="$config" \
    sh "$SCRIPT" --dry-run --no-deps --ifname eth9 --vid 100 \
        --peer 02:00:00:00:00:01 --channels 0,2-3 >"$out"

    assert_contains "$out" "kernel: 6.1.2-test"
    assert_contains "$out" "interface: eth9"
    assert_contains "$out" "DRY-RUN: make -C"
    assert_contains "$out" "ifname=eth9"
    assert_contains "$out" "vid=100"
    assert_contains "$out" "peer=02:00:00:00:00:01"
    assert_contains "$out" "eth2can0"
    assert_contains "$out" "eth2can2"
    assert_contains "$out" "eth2can3"
    assert_not_contains "$out" "eth2can1"

    if [ -s "$log_file" ]; then
        cat "$log_file" >&2
        fail "dry-run executed side-effect commands"
    fi
}

test_missing_kdir_explains_vendor_bsp_remedy()
{
    mock_bin=$tmp/missing-kdir-bin
    log_file=$tmp/missing-kdir.log
    out=$tmp/missing-kdir.out
    err=$tmp/missing-kdir.err

    : >"$log_file"
    make_mock_bin "$mock_bin" "$log_file"

    if PATH="$mock_bin:$PATH" \
       E2CF_TEST_UNAME_R=6.7.8-vendor \
       KDIR="$tmp/does-not-exist" \
       sh "$SCRIPT" --dry-run --no-deps --no-load >"$out" 2>"$err"; then
        fail "missing KDIR should fail"
    fi

    assert_contains "$err" "kernel build directory not found"
    assert_contains "$err" "KDIR=/path/to/kernel/build"
    assert_contains "$err" "linux-headers-6.7.8-vendor"
}

test_dry_run_warns_but_continues_when_socketcan_is_missing()
{
    mock_bin=$tmp/missing-can-bin
    log_file=$tmp/missing-can.log
    out=$tmp/missing-can.out
    config=$tmp/missing-can.config
    kdir=$tmp/missing-can-kdir

    : >"$log_file"
    mkdir -p "$kdir"
    : >"$kdir/Makefile"
    cat >"$config" <<'EOF'
# CONFIG_CAN is not set
# CONFIG_CAN_RAW is not set
# CONFIG_CAN_DEV is not set
EOF
    make_mock_bin "$mock_bin" "$log_file"

    PATH="$mock_bin:$PATH" \
    KDIR="$kdir" \
    E2CF_KERNEL_CONFIG="$config" \
    sh "$SCRIPT" --dry-run --no-deps --ifname eth0 --channels 0 >"$out" 2>&1

    assert_contains "$out" "WARNING: CONFIG_CAN is not enabled"
    assert_contains "$out" "DRY-RUN: insmod"
    assert_contains "$out" "eth2can0"
}

test_package_install_failure_still_reports_kdir_remedy()
{
    mock_bin=$tmp/pkg-fail-bin
    log_file=$tmp/pkg-fail.log
    out=$tmp/pkg-fail.out
    err=$tmp/pkg-fail.err

    : >"$log_file"
    make_mock_bin "$mock_bin" "$log_file"
    cat >"$mock_bin/id" <<'EOF'
#!/bin/sh
if [ "${1:-}" = "-u" ]; then
    echo 0
    exit 0
fi
exit 1
EOF
    chmod +x "$mock_bin/id"
    cat >"$mock_bin/apt-get" <<EOF
#!/bin/sh
echo "apt-get \$*" >>"$log_file"
exit 1
EOF
    chmod +x "$mock_bin/apt-get"

    if PATH="$mock_bin:$PATH" \
       E2CF_TEST_UNAME_R=6.8.9-vendor \
       KDIR="$tmp/no-vendor-headers" \
       sh "$SCRIPT" --ifname eth0 --no-load >"$out" 2>"$err"; then
        fail "missing KDIR should fail after package install fallback"
    fi

    assert_contains "$err" "kernel build directory not found"
    assert_contains "$err" "KDIR=/path/to/kernel/build"
    assert_contains "$err" "linux-headers-6.8.9-vendor"
}

test_raspberry_pi_headers_without_modules_build_are_used()
{
    mock_bin=$tmp/rpi-headers-bin
    log_file=$tmp/rpi-headers.log
    out=$tmp/rpi-headers.out
    config=$tmp/rpi-headers.config
    header_root=$tmp/usr-src
    header_dir=$header_root/linux-headers-6.1.21-v8+

    : >"$log_file"
    mkdir -p "$header_dir"
    : >"$header_dir/Makefile"
    cat >"$config" <<'EOF'
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_DEV=y
EOF
    make_mock_bin "$mock_bin" "$log_file"

    PATH="$mock_bin:$PATH" \
    E2CF_TEST_UNAME_R=6.1.21-v8+ \
    E2CF_HEADER_SEARCH_ROOTS="$header_root" \
    E2CF_KERNEL_CONFIG="$config" \
    sh "$SCRIPT" --dry-run --no-deps --ifname eth0 --no-load >"$out"

    assert_contains "$out" "using kernel build directory: $header_dir"
    assert_contains "$out" "KDIR: $header_dir"
    assert_contains "$out" "DRY-RUN: make -C"
    assert_contains "$out" "KDIR=$header_dir"
}

test_raspberry_pi_nearby_headers_are_reported_but_not_used()
{
    mock_bin=$tmp/rpi-nearby-bin
    log_file=$tmp/rpi-nearby.log
    out=$tmp/rpi-nearby.out
    err=$tmp/rpi-nearby.err
    header_root=$tmp/rpi-nearby-usr-src

    : >"$log_file"
    mkdir -p \
        "$header_root/linux-headers-6.1.21+" \
        "$header_root/linux-headers-6.1.21-v7+" \
        "$header_root/linux-headers-6.1.21-v7l+"
    : >"$header_root/linux-headers-6.1.21+/Makefile"
    : >"$header_root/linux-headers-6.1.21-v7+/Makefile"
    : >"$header_root/linux-headers-6.1.21-v7l+/Makefile"
    make_mock_bin "$mock_bin" "$log_file"

    if PATH="$mock_bin:$PATH" \
       E2CF_TEST_UNAME_R=6.1.21-v8+ \
       E2CF_HEADER_SEARCH_ROOTS="$header_root" \
       sh "$SCRIPT" --dry-run --no-deps --ifname eth0 --no-load >"$out" 2>"$err"; then
        fail "nearby headers must not be used for a mismatched running kernel"
    fi

    assert_contains "$err" "running kernel: 6.1.21-v8+"
    assert_contains "$err" "nearby kernel header directories were found"
    assert_contains "$err" "linux-headers-6.1.21-v7l+"
    assert_contains "$err" "do not build eth2can.ko against these mismatched headers"
}

test_gateway_carrier_allows_channel_configuration_without_dmesg_access()
{
    mock_bin=$tmp/carrier-hb-bin
    log_file=$tmp/carrier-hb.log
    out=$tmp/carrier-hb.out
    err=$tmp/carrier-hb.err
    config=$tmp/carrier-hb.config
    kdir=$tmp/carrier-hb-kdir
    net_class=$tmp/carrier-hb-net

    : >"$log_file"
    mkdir -p "$kdir" "$net_class/eth2can0"
    : >"$kdir/Makefile"
    echo 1 >"$net_class/eth2can0/carrier"
    cat >"$config" <<'EOF'
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_DEV=y
EOF
    make_mock_bin "$mock_bin" "$log_file"
    cat >"$mock_bin/dmesg" <<EOF
#!/bin/sh
echo "dmesg \$*" >>"$log_file"
exit 1
EOF
    chmod +x "$mock_bin/dmesg"

    PATH="$mock_bin:$PATH" \
    KDIR="$kdir" \
    E2CF_NET_CLASS="$net_class" \
    E2CF_KERNEL_CONFIG="$config" \
    E2CF_HB_WAIT_LOOPS=1 \
    sh "$SCRIPT" --no-deps --ifname eth0 --channels 0 >"$out" 2>"$err"

    assert_not_contains "$err" "gateway heartbeat was not observed"
    assert_contains "$log_file" "ip link set eth2can0 type can bitrate 1000000 dbitrate 5000000 fd on"
}

test_no_gateway_heartbeat_skips_channel_configuration()
{
    mock_bin=$tmp/no-hb-bin
    log_file=$tmp/no-hb.log
    out=$tmp/no-hb.out
    err=$tmp/no-hb.err
    config=$tmp/no-hb.config
    kdir=$tmp/no-hb-kdir

    : >"$log_file"
    mkdir -p "$kdir"
    : >"$kdir/Makefile"
    cat >"$config" <<'EOF'
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_DEV=y
EOF
    make_mock_bin "$mock_bin" "$log_file"

    PATH="$mock_bin:$PATH" \
    KDIR="$kdir" \
    E2CF_KERNEL_CONFIG="$config" \
    E2CF_HB_WAIT_LOOPS=1 \
    sh "$SCRIPT" --no-deps --ifname eth0 --channels 0 >"$out" 2>"$err"

    assert_contains "$err" "gateway heartbeat was not observed"
    assert_contains "$err" "tcpdump -eni eth0"
    assert_contains "$err" "vlan and ether proto 0x88b5"
    assert_contains "$err" "SocketCAN channel configuration skipped"
    assert_contains "$out" "SocketCAN configuration skipped"
    assert_not_contains "$log_file" "ip link set eth2can0 type can"
}

test_require_gateway_fails_without_heartbeat()
{
    mock_bin=$tmp/require-hb-bin
    log_file=$tmp/require-hb.log
    out=$tmp/require-hb.out
    err=$tmp/require-hb.err
    config=$tmp/require-hb.config
    kdir=$tmp/require-hb-kdir

    : >"$log_file"
    mkdir -p "$kdir"
    : >"$kdir/Makefile"
    cat >"$config" <<'EOF'
CONFIG_CAN=y
CONFIG_CAN_RAW=y
CONFIG_CAN_DEV=y
EOF
    make_mock_bin "$mock_bin" "$log_file"

    if PATH="$mock_bin:$PATH" \
       KDIR="$kdir" \
       E2CF_KERNEL_CONFIG="$config" \
       E2CF_HB_WAIT_LOOPS=1 \
       sh "$SCRIPT" --no-deps --require-gateway --ifname eth0 --channels 0 >"$out" 2>"$err"; then
        fail "--require-gateway should fail without heartbeat"
    fi

    assert_contains "$err" "ERROR: gateway heartbeat was not observed"
    assert_not_contains "$log_file" "ip link set eth2can0 type can"
}

test_help_lists_portable_options
test_auto_detect_skips_wireless_and_prefers_carrier_up_wired
test_dry_run_uses_overrides_without_side_effect_commands
test_missing_kdir_explains_vendor_bsp_remedy
test_dry_run_warns_but_continues_when_socketcan_is_missing
test_package_install_failure_still_reports_kdir_remedy
test_raspberry_pi_headers_without_modules_build_are_used
test_raspberry_pi_nearby_headers_are_reported_but_not_used
test_gateway_carrier_allows_channel_configuration_without_dmesg_access
test_no_gateway_heartbeat_skips_channel_configuration
test_require_gateway_fails_without_heartbeat

echo "PASS: install_driver tests"
