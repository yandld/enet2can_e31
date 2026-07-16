#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
CANPERF_DIR="$ROOT_DIR/linux/can_testcase"
SRC="$CANPERF_DIR/src/canperf.c"
CC=${CC:-cc}
failures=0

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	failures=$((failures + 1))
}

assert_contains()
{
	file=$1
	pattern=$2
	if ! grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
		printf '%s\n' "----- $file -----" >&2
		cat "$file" >&2 || true
		fail "expected to find: $pattern"
	fi
}

assert_not_contains()
{
	file=$1
	pattern=$2
	if grep -F -- "$pattern" "$file" >/dev/null 2>&1; then
		printf '%s\n' "----- $file -----" >&2
		cat "$file" >&2 || true
		fail "did not expect to find: $pattern"
	fi
}

tmp=${TMPDIR:-/tmp}/e2cf-canperf-fail-closed-test.$$
trap 'rm -rf "$tmp"' EXIT INT HUP TERM
mkdir -p "$tmp"

cat >"$tmp/wrappers.c" <<'EOF'
#include <errno.h>
#include <sched.h>
#include <sys/types.h>

unsigned int __wrap_if_nametoindex(const char *ifname)
{
	(void)ifname;
	return 0;
}

int __wrap_sched_setscheduler(pid_t pid, int policy,
			      const struct sched_param *param)
{
	(void)pid;
	(void)policy;
	(void)param;
	errno = EPERM;
	return -1;
}
EOF

runtime_bin="$tmp/canperf-runtime"
"$CC" -O2 -g -Wall -Wextra -Wformat=2 -D_GNU_SOURCE -pthread \
	-Wl,--wrap=if_nametoindex -Wl,--wrap=sched_setscheduler \
	-o "$runtime_bin" "$SRC" "$tmp/wrappers.c"

set +e
"$runtime_bin" latency --no-setup --count 1 --pair 0:4 \
	>"$tmp/socket.out" 2>"$tmp/socket.err"
socket_rc=$?
set -e
if [ "$socket_rc" -eq 0 ]; then
	fail "socket startup failure returned success"
fi
assert_not_contains "$tmp/socket.out" "RESULT latency:"

set +e
"$runtime_bin" latency --count 1 --pair 0:4 \
	>"$tmp/setup.out" 2>"$tmp/setup.err"
setup_rc=$?
set -e
if [ "$setup_rc" -eq 0 ]; then
	fail "requested channel setup failure returned success"
fi
assert_contains "$tmp/setup.err" "warning: eth2can0 setup failed"
assert_contains "$tmp/setup.err" \
	"warning: eth2can0 setup failed (No such device)"
assert_not_contains "$tmp/setup.out" "canperf - app-to-app CAN latency"
assert_not_contains "$tmp/setup.out" "RESULT latency:"

cat >"$tmp/setup_contract.c" <<'EOF'
#define main canperf_cli_main
#include "canperf.c"
#undef main

static int send_calls;

unsigned int __wrap_if_nametoindex(const char *ifname)
{
	(void)ifname;
	return 1;
}

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags)
{
	(void)fd;
	(void)buf;
	(void)len;
	(void)flags;
	send_calls++;
	errno = EPERM;
	return -1;
}

int main(void)
{
	int ret = chan_setup(10, 0, 1000000, 5000000);

	if (ret != -EPERM) {
		fprintf(stderr, "FAIL: down failure returned %d, expected %d\n",
			ret, -EPERM);
		return 1;
	}
	if (send_calls != 1) {
		fprintf(stderr,
			"FAIL: setup continued after down failure (%d sends)\n",
			send_calls);
		return 1;
	}
	return 0;
}
EOF

setup_contract_bin="$tmp/canperf-setup-contract"
"$CC" -std=gnu11 -O2 -g -Wall -Wextra -Wformat=2 -Werror \
	-D_GNU_SOURCE -pthread -I"$CANPERF_DIR/src" \
	-Wl,--wrap=if_nametoindex -Wl,--wrap=send \
	-o "$setup_contract_bin" "$tmp/setup_contract.c"
if ! "$setup_contract_bin"; then
	fail "channel setup did not stop at the down failure"
fi

cat >"$tmp/acceptance.c" <<'EOF'
#define main canperf_cli_main
#include "canperf.c"
#undef main

static int test_failures;

static void expect_result(bool actual, bool expected, const char *name)
{
	if (actual != expected) {
		fprintf(stderr, "FAIL: %s (got %d, expected %d)\n",
			name, actual, expected);
		test_failures++;
	}
}

int main(void)
{
	struct run_summary summary = {
		.sent = 1,
		.samples = 1,
	};

	expect_result(run_zero_loss(&summary, COUNTERS_CLEAN), true,
		      "traffic plus clean counters passes");
	summary.sent = 0;
	expect_result(run_zero_loss(&summary, COUNTERS_CLEAN), false,
		      "zero sent frames fail");
	summary.sent = 1;
	summary.samples = 0;
	expect_result(run_zero_loss(&summary, COUNTERS_CLEAN), false,
		      "zero samples fail");
	summary.samples = 1;
	expect_result(run_zero_loss(&summary, COUNTERS_UNAVAILABLE), false,
		      "unavailable counters fail");
	summary.lost = 1;
	expect_result(run_zero_loss(&summary, COUNTERS_CLEAN), false,
		      "reported loss fails");

	return test_failures ? 1 : 0;
}
EOF

acceptance_bin="$tmp/canperf-acceptance"
"$CC" -O2 -g -Wall -Wextra -Wformat=2 -D_GNU_SOURCE -pthread \
	-I"$CANPERF_DIR/src" -o "$acceptance_bin" "$tmp/acceptance.c"
if ! "$acceptance_bin"; then
	fail "acceptance predicate contract failed"
fi

cat >"$tmp/counters.c" <<'EOF'
#define main canperf_cli_main
#include "canperf.c"
#undef main

static const char snapshot_1000[] =
	"gateway: alive peer=02:00:00:00:00:01 hb_age_ms=5 uptime_s=10\n"
	"time: gw=1000 ns flags=0x0 local-gw=5 ns\n"
	"stats: age_ms=5 layout_rev=1 gw_time_ns=1000\n"
	"gw: rx=1 bad=0 untag=0 gaps=1 lost=1 reorder=0 | dn=1 rej=1 | up=1 txc=1\n"
	"gw: sent=1 sfail=1 starv=1 gated=0 cfg=1 hbrx=1 hbtx=1 logdrop=0\n"
	"gw: emac rx=1 rxerr=1 rxdrop=1 tx=1 txbusy=1 loop=100 | peer=1 safe=0 link=1\n"
	"gw CAN0: rx=1 ovf=1 tx=1 rej=0 tmo=0 hwm=1 irq=1 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN1: rx=1 ovf=2 tx=1 rej=0 tmo=0 hwm=1 irq=1 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN2: rx=1 ovf=3 tx=1 rej=0 tmo=0 hwm=1 irq=1 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN3: rx=1 ovf=4 tx=1 rej=0 tmo=0 hwm=1 irq=1 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN4: rx=1 ovf=5 tx=1 rej=0 tmo=0 hwm=1 irq=1 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN5: rx=1 ovf=6 tx=1 rej=0 tmo=0 hwm=1 irq=1 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"drv: rx=1 bad=0 gaps=1 lost=1 reorder=0 nomem=1 trunc=1 tx_cn=1 cfg_retries=0 cfg_timeouts=0\n"
	"drv CAN0: txc_tmo=0 txc_err=0 state=ERROR-ACTIVE tec=0 rec=0 arb_lost=0 rx_ovf=0\n"
	"drv: txc_rej ctrl_error=1 chan_stopped=1 queue_ovf=1 arb_lost=0\n";

static const char snapshot_2000[] =
	"gateway: alive peer=02:00:00:00:00:01 hb_age_ms=5 uptime_s=11\n"
	"time: gw=2000 ns flags=0x0 local-gw=5 ns\n"
	"stats: age_ms=5 layout_rev=1 gw_time_ns=2000\n"
	"gw: rx=2 bad=0 untag=0 gaps=1 lost=2 reorder=0 | dn=2 rej=1 | up=2 txc=2\n"
	"gw: sent=2 sfail=1 starv=1 gated=0 cfg=1 hbrx=1 hbtx=1 logdrop=0\n"
	"gw: emac rx=2 rxerr=1 rxdrop=1 tx=2 txbusy=1 loop=100 | peer=1 safe=0 link=1\n"
	"gw CAN0: rx=2 ovf=1 tx=2 rej=0 tmo=0 hwm=1 irq=2 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN1: rx=2 ovf=2 tx=2 rej=0 tmo=0 hwm=1 irq=2 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN2: rx=2 ovf=3 tx=2 rej=0 tmo=0 hwm=1 irq=2 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN3: rx=2 ovf=4 tx=2 rej=0 tmo=0 hwm=1 irq=2 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN4: rx=2 ovf=5 tx=2 rej=0 tmo=0 hwm=1 irq=2 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"gw CAN5: rx=2 ovf=6 tx=2 rej=0 tmo=0 hwm=1 irq=2 busoff=0 ERROR-ACTIVE running tec=0 rec=0 rxovf=0\n"
	"drv: rx=2 bad=0 gaps=1 lost=1 reorder=0 nomem=1 trunc=1 tx_cn=1 cfg_retries=0 cfg_timeouts=0\n"
	"drv CAN0: txc_tmo=0 txc_err=0 state=ERROR-ACTIVE tec=0 rec=0 arb_lost=0 rx_ovf=0\n"
	"drv: txc_rej ctrl_error=1 chan_stopped=1 queue_ovf=1 arb_lost=0\n";

struct sequence_reader {
	const struct cnt_snap *snapshots;
	size_t count;
	size_t next;
	size_t calls;
};

static void read_sequence(struct cnt_snap *snapshot, void *opaque)
{
	struct sequence_reader *reader = opaque;
	size_t index = reader->next;

	if (index >= reader->count)
		index = reader->count - 1;
	*snapshot = reader->snapshots[index];
	if (reader->next < reader->count)
		reader->next++;
	reader->calls++;
}

static int test_failures;

static void expect_true(bool value, const char *name)
{
	if (!value) {
		fprintf(stderr, "FAIL: %s\n", name);
		test_failures++;
	}
}

int main(void)
{
	struct cnt_snap before, newer, rebooted, after;
	struct cnt_snap sequence[3];
	struct sequence_reader reader;
	char malformed[sizeof(snapshot_1000)];
	char *field;
	bool fresh;

	counters_parse_text(&before, snapshot_1000);
	counters_parse_text(&newer, snapshot_2000);
	expect_true(before.ok && before.gw_time_ns == 1000,
		    "parser retains first gateway generation");
	expect_true(newer.ok && newer.gw_time_ns == 2000,
		    "parser retains next gateway generation");
	expect_true(before.gw_ovf == 21,
		    "parser requires and sums all six CAN overflow counters");
	expect_true(counters_state(&before, &before) == COUNTERS_UNAVAILABLE,
		    "unchanged generation is unavailable, not clean");
	expect_true(counters_state(&before, &newer) == COUNTERS_DIRTY,
		    "new generation exposes its counter delta");

	memcpy(malformed, snapshot_1000, sizeof(malformed));
	field = strstr(malformed, " rej=1 | up=");
	expect_true(field != NULL, "cross-line fixture marker exists");
	if (field) {
		field[3] = 'x';
		counters_parse_text(&after, malformed);
		expect_true(!after.ok,
			    "a field missing from its real line cannot be borrowed later");
	}

	memcpy(malformed, snapshot_1000, sizeof(malformed));
	field = strstr(malformed, "lost=1");
	expect_true(field != NULL, "non-numeric fixture marker exists");
	if (field) {
		field[5] = 'x';
		counters_parse_text(&after, malformed);
		expect_true(!after.ok, "non-numeric counters are unavailable");
	}

	memcpy(malformed, snapshot_1000, sizeof(malformed));
	field = strstr(malformed, "gw CAN5:");
	expect_true(field != NULL, "CAN5 fixture marker exists");
	if (field) {
		field[6] = 'X';
		counters_parse_text(&after, malformed);
		expect_true(!after.ok, "all six CAN overflow rows are required");
	}

	memcpy(malformed, snapshot_1000, sizeof(malformed));
	field = strstr(malformed, "tx_cn=1");
	expect_true(field != NULL, "truncation fixture marker exists");
	if (field) {
		field[6] = '\0';
		counters_parse_text(&after, malformed);
		expect_true(!after.ok, "a truncated counter value is unavailable");
	}

	sequence[0] = before;
	sequence[1] = before;
	sequence[2] = newer;
	reader = (struct sequence_reader) {
		.snapshots = sequence,
		.count = 3,
	};
	fresh = counters_wait_new_generation(&before, &after, read_sequence,
					     &reader, 3, 0);
	expect_true(fresh, "1000 -> 1000 -> 2000 finds a fresh snapshot");
	expect_true(reader.calls == 3,
		    "stale middle generation is skipped");
	expect_true(after.ok && after.gw_time_ns == 2000,
		    "fresh snapshot is returned");

	memset(&rebooted, 0, sizeof(rebooted));
	rebooted.ok = true;
	rebooted.gw_time_ns = 500;
	reader = (struct sequence_reader) {
		.snapshots = &rebooted,
		.count = 1,
	};
	fresh = counters_wait_new_generation(&before, &after, read_sequence,
					     &reader, 1, 0);
	expect_true(fresh && after.gw_time_ns == 500,
		    "lower post-reboot generation is fresh");
	expect_true(counters_state(&before, &after) == COUNTERS_UNAVAILABLE,
		    "1000 -> 500 with reset counters is unavailable, never clean");

	rebooted = newer;
	rebooted.gw_time_ns = 3000;
	rebooted.drv_lost = before.drv_lost - 1;
	expect_true(counters_state(&before, &rebooted) == COUNTERS_UNAVAILABLE,
		    "any cumulative counter rollback is unavailable");

	reader = (struct sequence_reader) {
		.snapshots = sequence,
		.count = 1,
	};
	fresh = counters_wait_new_generation(&before, &after, read_sequence,
					     &reader, 3, 0);
	expect_true(!fresh && !after.ok,
		    "permanently unchanged generation becomes stale");
	expect_true(reader.calls == 3,
		    "stale polling is bounded by max reads");

	memset(&before, 0, sizeof(before));
	reader = (struct sequence_reader) {
		.snapshots = sequence,
		.count = 1,
	};
	fresh = counters_wait_new_generation(&before, &after, read_sequence,
					     &reader, 3, 0);
	expect_true(!fresh && !after.ok,
		    "unavailable pre-snapshot stays unavailable");
	expect_true(reader.calls == 0,
		    "unavailable pre-snapshot does not wait");

	return test_failures ? 1 : 0;
}
EOF

counter_bin="$tmp/canperf-counters"
if "$CC" -O2 -g -Wall -Wextra -Wformat=2 -D_GNU_SOURCE -pthread \
	-I"$CANPERF_DIR/src" -o "$counter_bin" "$tmp/counters.c" \
	>"$tmp/counters-build.out" 2>"$tmp/counters-build.err"; then
	if ! "$counter_bin"; then
		fail "counter generation contract failed"
	fi
else
	cat "$tmp/counters-build.err" >&2
	fail "counter generation test did not build"
fi

cat >"$tmp/cancel.c" <<'EOF'
#define main canperf_cli_main
#include "canperf.c"
#undef main

int main(int argc, char **argv)
{
	struct opts opts = {
		.count = 0,
		.duration_s = 30,
		.gap_us = 1000,
		.size = 8,
		.report_s = 1,
		.window = 1,
	};
	struct pair_ctx pairs[2] = {
		{ .tx_ch = 1, .rx_ch = 2 },
		{ .tx_ch = 0, .rx_ch = 4 },
	};
	int ret;
	char *end;

	if (argc != 2)
		return 2;
	opts.window = (unsigned int)strtoul(argv[1], &end, 10);
	if (*end || !opts.window || opts.window > MAX_WINDOW)
		return 2;

	stop_flag = 0;
	g_live = false;
	ret = run_pairs(&opts, pairs, 2, true);
	if (!ret) {
		fprintf(stderr, "FAIL: startup failure was not propagated\n");
		return 1;
	}
	if (!pairs[0].sent) {
		fprintf(stderr, "FAIL: peer pair never entered its wait path\n");
		return 1;
	}
	if (!atomic_load_explicit(&pairs[0].done, memory_order_acquire) ||
	    !atomic_load_explicit(&pairs[1].done, memory_order_acquire)) {
		fprintf(stderr, "FAIL: cancelled pair did not finish\n");
		return 1;
	}
	return 0;
}
EOF

cat >"$tmp/cancel_wrappers.c" <<'EOF'
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static atomic_bool good_pair_entered_wait;
static atomic_int next_fd = 100;

unsigned int __wrap_if_nametoindex(const char *ifname)
{
	if (!strcmp(ifname, "eth2can0") || !strcmp(ifname, "eth2can4")) {
		for (int i = 0; i < 1000 &&
		     !atomic_load_explicit(&good_pair_entered_wait,
					   memory_order_acquire); i++)
			usleep(1000);
		return 0;
	}
	return 1;
}

int __wrap_socket(int domain, int type, int protocol)
{
	(void)domain;
	(void)type;
	(void)protocol;
	return atomic_fetch_add_explicit(&next_fd, 1, memory_order_relaxed);
}

int __wrap_setsockopt(int fd, int level, int name,
		      const void *value, socklen_t len)
{
	(void)fd;
	(void)level;
	(void)name;
	(void)value;
	(void)len;
	return 0;
}

int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)fd;
	(void)addr;
	(void)len;
	return 0;
}

ssize_t __wrap_write(int fd, const void *buf, size_t len)
{
	(void)fd;
	(void)buf;
	atomic_store_explicit(&good_pair_entered_wait, true,
			      memory_order_release);
	return (ssize_t)len;
}

ssize_t __wrap_recvmsg(int fd, struct msghdr *msg, int flags)
{
	(void)fd;
	(void)msg;
	(void)flags;
	errno = EAGAIN;
	return -1;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
	(void)fds;
	(void)count;
	(void)timeout;
	usleep(1000);
	return 0;
}

int __wrap_close(int fd)
{
	(void)fd;
	return 0;
}
EOF

cancel_bin="$tmp/canperf-cancel"
"$CC" -std=gnu11 -O2 -g -Wall -Wextra -Wformat=2 -Werror \
	-D_GNU_SOURCE -pthread -I"$CANPERF_DIR/src" \
	-Wl,--wrap=if_nametoindex -Wl,--wrap=socket \
	-Wl,--wrap=setsockopt -Wl,--wrap=bind -Wl,--wrap=write \
	-Wl,--wrap=recvmsg -Wl,--wrap=poll -Wl,--wrap=close \
	-o "$cancel_bin" "$tmp/cancel.c" "$tmp/cancel_wrappers.c"
for cancel_window in 1 4; do
	set +e
	timeout 2 "$cancel_bin" "$cancel_window" \
		>"$tmp/cancel-$cancel_window.out" \
		2>"$tmp/cancel-$cancel_window.err"
	cancel_rc=$?
	set -e
	if [ "$cancel_rc" -eq 124 ]; then
		fail "startup failure did not cancel window=$cancel_window peer"
	elif [ "$cancel_rc" -ne 0 ]; then
		cat "$tmp/cancel-$cancel_window.err" >&2 || true
		fail "window=$cancel_window cancellation contract failed"
	fi
done

if [ "$failures" -ne 0 ]; then
	exit 1
fi

echo "PASS: canperf fail-closed checks"
