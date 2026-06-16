/*
 * canperf.c - E2CF CAN gateway segmented latency measurement tool
 *
 * Copyright 2026 NXP
 * Author: Ken Li <ken.li@nxp.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 *
 * =========================== Test topology ===============================
 *
 *  Linux app (TX)                                            Linux app (RX)
 *      | T0: send()                                       T4: recv() |
 *      v                                                              |
 *  eth2can0 (eth2can.ko) --- eth0 --- switch --- MCXE31B eth          |
 *      |                                               |              |
 *      |                              MCU downlink -> CAN0 controller |
 *      |                                               |              |
 *      |                          T1: frame TX complete on the bus    |
 *      |                                               |              |
 *      |                  physical CAN bus (120 ohm terminated)       |
 *      |                                               |              |
 *      |                       CAN4 controller, T2: MCU RX capture    |
 *      |                                               |              |
 *  eth2can4 <-- eth0 <-- switch <-- MCXE31B eth <-- uplink aggregation
 *      |                                              (T_agg <= 50us)
 *      +--> T3: frame enters the Linux network stack --> T4
 *
 *  Default external wiring, three independent CAN buses:
 *      can0 <-> can4,  can1 <-> can2,  can3 <-> can5
 *
 * ======================== Implementation notes ===========================
 *
 * Five timestamps per probe frame:
 *   T0  userspace clock right before send()        [exact, local clock]
 *   T1  CAN bus TX complete - gateway TXC record timestamp, delivered as
 *       a hardware timestamp on the local echo skb  [gateway clock, mapped]
 *   T2  remote MCU CAN RX capture - gateway DATA record timestamp,
 *       delivered as a hardware timestamp on the RX skb [gateway, mapped]
 *   T3  kernel software RX timestamp (frame entered the network stack)
 *                                                   [exact, kernel clock]
 *   T4  userspace clock right after recv()          [exact, local clock]
 *
 * Reported segments (histograms with min/p50/p99/p99.9/max/mean in us):
 *   total = T4 - T0   single local clock, no mapping error - hard number
 *   A = T1 - T0       Linux TX path + gateway downlink + CAN wire time
 *   B = T2 - T1       MCU capture delay; both ends on the SAME gateway
 *                     clock, so no mapping error; expected single-digit us
 *                     and therefore a sanity check of the clock mapping
 *   C = T3 - T2       gateway uplink (<= 50us aggregation + eth + NIC)
 *   D = T4 - T3       Linux RX wakeup
 *
 * Clock-mapping error model: T1/T2 originate from the gateway clock and
 * are mapped onto CLOCK_REALTIME by the driver, anchored by the 1 Hz TIME
 * message. The mapping bias roughly equals the one-way transit of the
 * TIME frame (tens of us): segment A reads high by the same amount that
 * segment C reads low, the total is unaffected. B, D and the total carry
 * no mapping error. Without driver/firmware timestamp support the tool
 * degrades gracefully to total + D only.
 *
 * Measurement mechanics:
 *   - one measurement thread per wired pair; pairs run CONCURRENTLY and
 *     are fully independent (own CAN ID 0x100 + tx_channel, own sockets,
 *     own histograms);
 *   - lockstep pacing: exactly one probe frame in flight per pair (send,
 *     wait for both the local echo and the remote copy, then pace) -
 *     latency probing, not a load generator;
 *   - probe payload carries a magic and a sequence number for matching;
 *   - per pair three sockets: a TX socket (filters disabled), an echo RX
 *     socket on the TX device (accepts only own frames, MSG_DONTROUTE
 *     set on loopback of locally sent frames) and a remote RX socket;
 *   - SO_TIMESTAMPING delivers both the software (T3) and the gateway
 *     hardware (T1/T2) timestamps via control messages;
 *   - the run length is either a frame count or a duration; progress is
 *     reported periodically; SIGINT stops gracefully and the statistics
 *     gathered so far are printed.
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/netlink.h>
#include <linux/if_link.h>
#include <linux/net_tstamp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#define MAX_PAIRS	6	/* 3 wired buses x 2 directions (--bidir) */
#define MAX_WINDOW	16	/* = driver TXC window depth (E2CF_WIN_DEPTH) */
#define PROBE_MAGIC	0xE2CF5EB5u
#define PROBE_ID_BASE	0x100u
#define WAIT_FRAME_MS	100	/* per-frame completion timeout */

static const int default_pairs[MAX_PAIRS][2] = { {0, 4}, {1, 2}, {3, 5} };

static volatile sig_atomic_t stop_flag;
static bool g_live;	/* stdout is a TTY: refresh the dashboard in place */

/* SIGINT/SIGTERM handler: request a graceful stop of all pair threads. */
static void on_signal(int sig)
{
	(void)sig;
	stop_flag = 1;
}

/* Parse a bitrate string with an optional k/M suffix ("1M", "500k",
 * "250000") into bits per second. Returns 0 on a malformed string (bad
 * suffix, trailing garbage, negative) so the caller's existing zero
 * check rejects it instead of silently configuring a bogus rate. */
static unsigned int parse_rate(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v <= 0)
		return 0;
	if (*end == 'M' || *end == 'm') {
		v *= 1e6;
		end++;
	} else if (*end == 'K' || *end == 'k') {
		v *= 1e3;
		end++;
	}
	if (*end != '\0')
		return 0;
	return (unsigned int)v;
}

/* Parse a duration with an optional unit suffix: bare number or Ns = seconds,
 * Nm = minutes, Nh = hours (e.g. "100s", "2m", "1h", "90"). Returns seconds,
 * or -1 on a malformed string. */
static long parse_duration(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	if (end == s || v < 0)
		return -1;
	switch (*end) {
	case 'h': case 'H': v *= 3600.0; end++; break;
	case 'm': case 'M': v *= 60.0;   end++; break;
	case 's': case 'S': end++;       break;
	case '\0': break;
	default: return -1;
	}
	if (*end != '\0')
		return -1;
	return (long)v;
}

/* Current CLOCK_REALTIME in nanoseconds (same axis as the kernel software
 * RX timestamps and the driver-mapped gateway hardware timestamps). Used
 * for the T0/T4 measurement points ONLY - never for timeouts. */
static uint64_t now_real_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Current CLOCK_MONOTONIC in nanoseconds. Used for every deadline and
 * duration computation so that an NTP/PTP step of CLOCK_REALTIME can
 * neither fake a frame loss nor stretch/shrink a --duration run. */
static uint64_t now_mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Worst-case wire time of one CAN / CAN FD frame (11-bit ID) in ns -
 * the theoretical bus-capacity yardstick printed in the banner and used
 * to scale the sweep. Classical: 47 framing bits + payload + worst-case
 * bit stuffing. FD with BRS: arbitration-phase bits at the nominal rate
 * plus data-phase bits (ESI+DLC+payload+ISO stuff count+CRC17/21+fixed
 * stuff+delimiter) at the data rate with worst-case dynamic stuffing.
 * Engineering approximation for capacity scaling, not a bit-exact
 * simulator. */
static uint64_t frame_wire_ns(int size, unsigned int bitrate,
			      unsigned int dbitrate)
{
	uint64_t ns;
	unsigned int bits;

	if (size <= 8 || !dbitrate) {
		bits = 47 + 8 * (unsigned int)size +
		       (34 + 8 * (unsigned int)size - 1) / 4;
		return (uint64_t)bits * 1000000000ull / bitrate;
	}
	/* SOF..BRS at nominal (with arbitration stuffing), ACK..IFS back
	 * at nominal: ~30 nominal bit times */
	ns = 30ull * 1000000000ull / bitrate;
	bits = 1 + 4 + 8 * (unsigned int)size + 4 +
	       ((size <= 16) ? 17 : 21) + 6;
	bits += (8 * (unsigned int)size) / 5;
	ns += (uint64_t)bits * 1000000000ull / dbitrate;
	return ns;
}

/* ========================================================================
 * Minimal rtnetlink channel configuration (replaces `ip link ... type can`)
 * ======================================================================== */

struct nl_req {
	struct nlmsghdr nh;
	struct ifinfomsg ifi;
	char attrs[1024];
};

/* Append one attribute to the request being built. */
static void nl_attr_put(struct nl_req *req, unsigned short type,
			const void *data, unsigned short len)
{
	struct rtattr *attr = (struct rtattr *)((char *)&req->nh +
						NLMSG_ALIGN(req->nh.nlmsg_len));

	attr->rta_type = type;
	attr->rta_len = RTA_LENGTH(len);
	if (len)
		memcpy(RTA_DATA(attr), data, len);
	req->nh.nlmsg_len = NLMSG_ALIGN(req->nh.nlmsg_len) +
			    RTA_ALIGN(RTA_LENGTH(len));
}

/* Open a nested attribute; close it with nl_nest_end(). */
static struct rtattr *nl_nest_begin(struct nl_req *req, unsigned short type)
{
	struct rtattr *nest = (struct rtattr *)((char *)&req->nh +
						NLMSG_ALIGN(req->nh.nlmsg_len));

	nl_attr_put(req, type, NULL, 0);
	return nest;
}

/* Patch the nest length after all children have been appended. */
static void nl_nest_end(struct nl_req *req, struct rtattr *nest)
{
	nest->rta_len = (unsigned short)((char *)&req->nh +
					 req->nh.nlmsg_len - (char *)nest);
}

/* Send the request and wait for the kernel ACK.
 * Returns 0 on ACK, a negative errno on NACK or socket error. */
static int nl_transact(int fd, struct nl_req *req)
{
	char buf[4096];
	ssize_t n;

	req->nh.nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
	if (send(fd, req, req->nh.nlmsg_len, 0) < 0)
		return -errno;
	n = recv(fd, buf, sizeof(buf), 0);
	if (n < 0)
		return -errno;
	for (struct nlmsghdr *h = (struct nlmsghdr *)buf;
	     NLMSG_OK(h, (size_t)n); h = NLMSG_NEXT(h, n))
		if (h->nlmsg_type == NLMSG_ERROR)
			return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
	return -EPROTO;
}

/* Administratively raise or lower one network interface. */
static int link_set_up(int fd, const char *ifname, bool up)
{
	struct nl_req req;
	unsigned int ifindex = if_nametoindex(ifname);

	if (!ifindex)
		return -ENODEV;
	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
	req.nh.nlmsg_type = RTM_NEWLINK;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index = (int)ifindex;
	req.ifi.ifi_change = IFF_UP;
	req.ifi.ifi_flags = up ? IFF_UP : 0;
	return nl_transact(fd, &req);
}

/* Configure one CAN channel: down -> set bitrate/dbitrate (the kernel
 * computes the bit timing from the driver's bittiming_const when only
 * the bitrate field is filled, same as iproute2) -> up.
 * dbitrate == 0 selects classical CAN (no FD). */
static int chan_setup(int fd, int ch, unsigned int bitrate,
		      unsigned int dbitrate, bool loopback)
{
	char dev[20];
	struct nl_req req;
	struct rtattr *linkinfo, *infodata;
	struct can_bittiming bt;
	struct can_ctrlmode cm;
	unsigned int ifindex;
	int ret;

	snprintf(dev, sizeof(dev), "eth2can%d", ch);
	ifindex = if_nametoindex(dev);
	if (!ifindex)
		return -ENODEV;
	(void)link_set_up(fd, dev, false);

	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifi));
	req.nh.nlmsg_type = RTM_NEWLINK;
	req.ifi.ifi_family = AF_UNSPEC;
	req.ifi.ifi_index = (int)ifindex;

	linkinfo = nl_nest_begin(&req, IFLA_LINKINFO);
	nl_attr_put(&req, IFLA_INFO_KIND, "can", 3);
	infodata = nl_nest_begin(&req, IFLA_INFO_DATA);

	memset(&cm, 0, sizeof(cm));
	cm.mask = CAN_CTRLMODE_FD | CAN_CTRLMODE_LOOPBACK;
	cm.flags = (dbitrate ? CAN_CTRLMODE_FD : 0) |
		   (loopback ? CAN_CTRLMODE_LOOPBACK : 0);
	nl_attr_put(&req, IFLA_CAN_CTRLMODE, &cm, sizeof(cm));

	memset(&bt, 0, sizeof(bt));
	bt.bitrate = bitrate;
	nl_attr_put(&req, IFLA_CAN_BITTIMING, &bt, sizeof(bt));
	if (dbitrate) {
		memset(&bt, 0, sizeof(bt));
		bt.bitrate = dbitrate;
		nl_attr_put(&req, IFLA_CAN_DATA_BITTIMING, &bt, sizeof(bt));
	}
	nl_nest_end(&req, infodata);
	nl_nest_end(&req, linkinfo);

	ret = nl_transact(fd, &req);
	if (ret)
		return ret;
	return link_set_up(fd, dev, true);
}

/* ========================================================================
 * Timestamping sockets
 * ======================================================================== */

struct rx_stamps {
	uint64_t sw_ns;		/* kernel software RX timestamp (T3) */
	uint64_t hw_ns;		/* gateway hardware timestamp (T1/T2), 0 = none */
	bool own;		/* MSG_DONTROUTE: locally generated echo */
};

/* Open a CAN_RAW socket on @dev with software + raw-hardware RX
 * timestamping enabled. @filter_id selects a single SFF CAN identifier
 * (UINT32_MAX = receive everything). Returns the fd or -1. */
static int open_can_socket(const char *dev, bool fd_frames,
			   uint32_t filter_id, bool want_ts)
{
	struct sockaddr_can addr = { .can_family = AF_CAN };
	int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
	int one = 1;
	int rcvbuf = 4 * 1024 * 1024;
	int ts_flags = SOF_TIMESTAMPING_RX_SOFTWARE |
		       SOF_TIMESTAMPING_SOFTWARE |
		       SOF_TIMESTAMPING_RX_HARDWARE |
		       SOF_TIMESTAMPING_RAW_HARDWARE;

	if (fd < 0) {
		perror("socket(PF_CAN)");
		return -1;
	}
	if (fd_frames)
		setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
			   &one, sizeof(one));
	if (filter_id != UINT32_MAX) {
		struct can_filter flt = {
			.can_id = filter_id,
			.can_mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG,
		};

		setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof(flt));
	}
	if (want_ts)
		setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &ts_flags,
			   sizeof(ts_flags));
	if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf,
		       sizeof(rcvbuf)) < 0)
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

	addr.can_ifindex = (int)if_nametoindex(dev);
	if (!addr.can_ifindex) {
		fprintf(stderr, "error: no such interface: %s (driver loaded?)\n",
			dev);
		close(fd);
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("bind");
		close(fd);
		return -1;
	}
	return fd;
}

/* Non-blocking receive of one CAN(-FD) frame plus its timestamps.
 * Returns 1 when a frame was read, 0 when the queue is empty, -1 on a
 * fatal socket error. */
static int recv_frame(int fd, struct canfd_frame *cf, struct rx_stamps *st)
{
	char cbuf[256];
	struct iovec iov = { .iov_base = cf, .iov_len = sizeof(*cf) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = cbuf, .msg_controllen = sizeof(cbuf),
	};
	ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);

	memset(st, 0, sizeof(*st));
	if (n < 0)
		return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
	if (n != CAN_MTU && n != CANFD_MTU)
		return 0;
	st->own = !!(msg.msg_flags & MSG_DONTROUTE);

	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c;
	     c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level == SOL_SOCKET &&
		    c->cmsg_type == SO_TIMESTAMPING) {
			struct timespec tv[3];

			/* [0] = software stamp, [2] = raw hardware stamp */
			memcpy(tv, CMSG_DATA(c), sizeof(tv));
			st->sw_ns = (uint64_t)tv[0].tv_sec * 1000000000ull +
				    tv[0].tv_nsec;
			st->hw_ns = (uint64_t)tv[2].tv_sec * 1000000000ull +
				    tv[2].tv_nsec;
		}
	}
	return 1;
}

/* ========================================================================
 * Fixed-resolution latency histogram
 * ======================================================================== */

#define HIST_FINE	2048	/* 1 us buckets covering 0..2047 us */
#define HIST_LOG2	64	/* log2(us) buckets above that */

struct hist {
	uint64_t fine[HIST_FINE];
	uint64_t coarse[HIST_LOG2];
	uint64_t count, max_ns;
	uint64_t min_ns;	/* valid only when count > 0 */
	uint64_t clamped;	/* negative cross-clock samples clamped to 0 */
	double sum_ns;
};

/* Insert one latency sample. Negative cross-clock samples (clock-mapping
 * bias on the A/C segments) are clamped to zero and counted separately so
 * both the sample count and the minimum stay meaningful. */
static void hist_add(struct hist *h, int64_t ns)
{
	uint64_t us;

	if (ns < 0) {
		ns = 0;
		h->clamped++;
	}
	us = (uint64_t)ns / 1000;
	if (us < HIST_FINE) {
		h->fine[us]++;
	} else {
		int idx = 63 - __builtin_clzll(us);

		h->coarse[idx >= HIST_LOG2 ? HIST_LOG2 - 1 : idx]++;
	}
	if (!h->count || (uint64_t)ns < h->min_ns)
		h->min_ns = (uint64_t)ns;
	h->count++;
	h->sum_ns += (double)ns;
	if ((uint64_t)ns > h->max_ns)
		h->max_ns = (uint64_t)ns;
}

/* Interpolated percentile in microseconds (p in 0..100). */
static double hist_pct(const struct hist *h, double p)
{
	uint64_t target, cum = 0;

	if (!h->count)
		return 0;
	target = (uint64_t)(p / 100.0 * (double)h->count);
	if (target >= h->count)
		target = h->count - 1;
	for (int i = 0; i < HIST_FINE; i++) {
		cum += h->fine[i];
		if (cum > target)
			return i + 0.5;
	}
	for (int i = 0; i < HIST_LOG2; i++) {
		cum += h->coarse[i];
		if (cum > target)
			return 1.5 * (double)(1ull << i);
	}
	return (double)h->max_ns / 1000.0;
}

/* Print one row of the per-pair result table (all values in us). */
static void hist_row(const char *seg, const char *span, const char *acc,
		     const struct hist *h)
{
	if (!h->count) {
		printf("| %-9s| %-7s| %-7s| %51s |\n",
		       seg, span, acc, "no samples");
		return;
	}
	printf("| %-9s| %-7s| %-7s| %6.0f | %6.0f | %6.0f | %6.0f | %6.0f | %6.0f |\n",
	       seg, span, acc,
	       (double)h->min_ns / 1000.0, hist_pct(h, 50), hist_pct(h, 99),
	       hist_pct(h, 99.9), (double)h->max_ns / 1000.0,
	       h->sum_ns / (double)h->count / 1000.0);
}

/* ========================================================================
 * Per-pair measurement thread
 * ======================================================================== */

struct opts {
	long count;		/* frames per pair (0 = unbounded) */
	long duration_s;	/* run length in seconds (0 = count-bound) */
	int gap_us;		/* inter-frame pacing */
	int size;		/* 8 = classical, 64 = CAN FD with BRS */
	int report_s;		/* progress report period */
	unsigned int window;	/* probes in flight per pair (1 = lockstep) */
	bool setup;		/* configure channels at startup */
	bool csv;		/* dump per-frame timestamps */
	bool bidir;		/* mirror every pair (A:B adds B:A) */
	bool sweep;		/* max-sustainable-rate search */
	long sweep_frames;	/* probes per pair per sweep step */
	int p99_limit_us;	/* sweep pass criterion on total p99 */
	bool performance;	/* decoupled one-way throughput + loss test */
	bool loopback;		/* controller loopback (LPB): same-channel x:x pairs */
	unsigned int bitrate, dbitrate;
};

struct pair_ctx {
	int tx_ch, rx_ch;
	const struct opts *opts;
	pthread_t thread;

	/* results - written by the pair thread; the progress reporter
	 * reads them racily (display only), the final report reads them
	 * after join */
	struct hist h_total, h_l1, h_l2, h_l3, h_l4;
	uint64_t sent, lost, echo_miss, no_hw_stamp;
	/* frames that DID arrive but only after their probe's deadline had
	 * expired (counted lost/echo-miss at the time): proof the path is
	 * slow, not lossy, for that probe */
	uint64_t late_rx, late_echo;
	uint64_t elapsed_ns;	/* measurement loop wall time */
	volatile int done;
};

/* The seven per-probe instants. t1/t2/nicrx/t5 are CLOCK_REALTIME (Linux);
 * X/Y are RAW gateway-clock low32 ns (used only as the difference Y-X, so
 * never compared against the Linux clock). 0 = missing. */
enum { TS_T1, TS_T2, TS_X, TS_Y, TS_NICRX, TS_T5, TS_N };

/* Send one probe and wait (up to WAIT_FRAME_MS) for the local echo (carrying
 * t2 + X) and the remote copy (carrying Y + nicrx). Returns 0 when the remote
 * copy arrived, -1 on loss. */
static int probe_once(struct pair_ctx *pc, int s_tx, int s_echo, int s_rx,
		      uint32_t can_id, uint32_t seq, size_t mtu,
		      uint64_t ts[TS_N])
{
	const struct opts *o = pc->opts;
	struct canfd_frame cf;
	struct pollfd pf[2] = {
		{ .fd = s_echo, .events = POLLIN },
		{ .fd = s_rx, .events = POLLIN },
	};
	bool got_echo = false, got_rx = false;
	uint64_t deadline;

	memset(ts, 0, TS_N * sizeof(ts[0]));
	memset(&cf, 0, sizeof(cf));
	cf.can_id = can_id;
	cf.len = (uint8_t)o->size;
	if (o->size > 8)
		cf.flags = CANFD_BRS;
	memcpy(cf.data, &(uint32_t){PROBE_MAGIC}, 4);
	memcpy(cf.data + 4, &seq, 4);

	ts[TS_T1] = now_real_ns();
	if (write(s_tx, &cf, mtu) != (ssize_t)mtu)
		return -1;

	deadline = now_mono_ns() + (uint64_t)WAIT_FRAME_MS * 1000000ull;
	while ((!got_echo || !got_rx) && now_mono_ns() < deadline &&
	       !stop_flag) {
		struct canfd_frame rf;
		struct rx_stamps st;

		if (poll(pf, 2, 10) <= 0)
			continue;

		while (recv_frame(s_echo, &rf, &st) > 0) {
			uint32_t magic, rseq;

			memcpy(&magic, rf.data, 4);
			memcpy(&rseq, rf.data + 4, 4);
			if (st.own && magic == PROBE_MAGIC && rseq == seq) {
				ts[TS_T2] = st.sw_ns; /* eth-egress from Linux */
				ts[TS_X] = st.hw_ns;  /* req eth-rx at MCU (raw gw) */
				got_echo = true;
			} else if (st.own && magic == PROBE_MAGIC &&
				   (int32_t)(seq - rseq) > 0) {
				/* echo of an earlier probe arriving after its
				 * deadline - it was charged as echo-miss */
				pc->late_echo++;
			}
		}
		while (recv_frame(s_rx, &rf, &st) > 0) {
			uint32_t magic, rseq;

			memcpy(&magic, rf.data, 4);
			memcpy(&rseq, rf.data + 4, 4);
			if (!st.own && magic == PROBE_MAGIC && rseq == seq) {
				ts[TS_T5] = now_real_ns();
				ts[TS_Y] = st.hw_ns;     /* eth-egress at MCU (raw gw) */
				ts[TS_NICRX] = st.sw_ns; /* Linux NIC ingress */
				got_rx = true;
			} else if (!st.own && magic == PROBE_MAGIC &&
				   (int32_t)(seq - rseq) > 0) {
				/* remote copy of an earlier probe arriving
				 * after its deadline - it was charged as lost,
				 * but the path was slow, not lossy */
				pc->late_rx++;
			}
		}
	}
	if (!got_rx)
		return -1;
	if (!got_echo)
		pc->echo_miss++;
	return 0;
}

/* Fold one completed probe's timestamps into the pair histograms and the
 * optional per-frame CSV (shared by the lockstep and windowed engines). */
static void record_sample(struct pair_ctx *pc, FILE *csv, uint32_t seq,
			  const uint64_t ts[TS_N])
{
	int64_t total = (int64_t)(ts[TS_T5] - ts[TS_T1]);

	hist_add(&pc->h_total, total);
	hist_add(&pc->h_l1, (int64_t)(ts[TS_T2] - ts[TS_T1]));
	hist_add(&pc->h_l4, (int64_t)(ts[TS_T5] - ts[TS_NICRX]));
	if (ts[TS_X] && ts[TS_Y]) {
		/* X/Y are raw gw low32; the duration is the 32-bit difference
		 * (residency << 4.29 s, so the wrap-safe subtraction is exact) */
		int64_t l3 = (int64_t)(uint32_t)((uint32_t)ts[TS_Y] -
						 (uint32_t)ts[TS_X]);

		hist_add(&pc->h_l3, l3);
		hist_add(&pc->h_l2, total -
			 (int64_t)(ts[TS_T2] - ts[TS_T1]) - l3 -
			 (int64_t)(ts[TS_T5] - ts[TS_NICRX]));
	} else {
		pc->no_hw_stamp++;
	}
	if (csv)
		fprintf(csv, "%u,%llu,%llu,%llu,%llu,%llu,%llu\n", seq,
			(unsigned long long)ts[TS_T1], (unsigned long long)ts[TS_T2],
			(unsigned long long)ts[TS_X], (unsigned long long)ts[TS_Y],
			(unsigned long long)ts[TS_NICRX],
			(unsigned long long)ts[TS_T5]);
}

/* One in-flight probe of the windowed engine. */
struct probe_slot {
	bool used;
	bool got_echo, got_rx;
	uint32_t seq;
	uint64_t deadline;	/* CLOCK_MONOTONIC ns */
	uint64_t ts[TS_N];	/* t1,t2,X,Y,nicrx,t5; 0 = missing */
};

/* Windowed measurement loop: up to o->window probes in flight per pair
 * (slots backed by the driver's 16-deep TXC window, which provides the
 * backpressure), sends paced on an absolute grid - so --gap-us sets the
 * OFFERED load (1e6/gap fps) instead of being RTT-bound like lockstep.
 * This is the throughput / loss-rate engine; segment histograms are
 * filled exactly like in lockstep. */
static void pair_run_window(struct pair_ctx *pc, int s_tx, int s_echo,
			    int s_rx, uint32_t can_id, size_t mtu, FILE *csv)
{
	const struct opts *o = pc->opts;
	struct probe_slot slots[MAX_WINDOW];
	struct canfd_frame cf;
	struct pollfd pf[2] = {
		{ .fd = s_echo, .events = POLLIN },
		{ .fd = s_rx, .events = POLLIN },
	};
	uint64_t end_ns = o->duration_s ?
		now_mono_ns() + (uint64_t)o->duration_s * 1000000000ull : 0;
	uint64_t grid = now_mono_ns();
	uint32_t seq_next = 0;
	int outstanding = 0;

	memset(slots, 0, sizeof(slots));
	memset(&cf, 0, sizeof(cf));
	cf.can_id = can_id;
	cf.len = (uint8_t)o->size;
	if (o->size > 8)
		cf.flags = CANFD_BRS;
	memcpy(cf.data, &(uint32_t){PROBE_MAGIC}, 4);

	for (;;) {
		uint64_t now = now_mono_ns();
		uint64_t wake;
		bool sending_done = stop_flag ||
			(o->count && seq_next >= (uint32_t)o->count) ||
			(end_ns && now >= end_ns);
		int i, tmo_ms;

		if (sending_done && !outstanding)
			break;

		/* launch probes while the grid time has come and the window
		 * has room (grid clamps to now when behind: rate is capped,
		 * stalls are not compensated by bursts) */
		while (!sending_done && outstanding < (int)o->window &&
		       now >= grid) {
			struct probe_slot *sl = NULL;

			for (i = 0; i < (int)o->window; i++)
				if (!slots[i].used) {
					sl = &slots[i];
					break;
				}
			if (!sl)
				break;	/* cannot happen: outstanding < window */

			memcpy(cf.data + 4, &seq_next, 4);
			memset(sl, 0, sizeof(*sl));
			sl->seq = seq_next;
			sl->ts[TS_T1] = now_real_ns();
			if (write(s_tx, &cf, mtu) != (ssize_t)mtu) {
				if (errno == ENOBUFS || errno == EAGAIN) {
					/* transient qdisc backpressure: back
					 * off briefly, do not consume the seq */
					grid = now + 50000;
					break;
				}
				pc->sent++;
				pc->lost++;
				seq_next++;
				continue;
			}
			sl->used = true;
			sl->deadline = now + (uint64_t)WAIT_FRAME_MS * 1000000ull;
			pc->sent++;
			seq_next++;
			outstanding++;
			grid += (uint64_t)o->gap_us * 1000;
			if (grid < now)
				grid = now;
			now = now_mono_ns();
			sending_done = stop_flag ||
				(o->count && seq_next >= (uint32_t)o->count) ||
				(end_ns && now >= end_ns);
		}

		/* sleep until the next send slot or the nearest deadline */
		wake = (uint64_t)-1;
		if (!sending_done && outstanding < (int)o->window)
			wake = grid;
		for (i = 0; i < (int)o->window; i++)
			if (slots[i].used && slots[i].deadline < wake)
				wake = slots[i].deadline;
		now = now_mono_ns();
		tmo_ms = (wake == (uint64_t)-1) ? 10 :
			 (wake <= now) ? 0 : (int)((wake - now) / 1000000ull + 1);
		if (tmo_ms > 10)
			tmo_ms = 10;
		poll(pf, 2, tmo_ms);

		/* drain the echo socket: T1 onto the matching slot */
		{
			struct canfd_frame rf;
			struct rx_stamps st;

			while (recv_frame(s_echo, &rf, &st) > 0) {
				uint32_t magic, rseq;
				struct probe_slot *sl = NULL;

				memcpy(&magic, rf.data, 4);
				memcpy(&rseq, rf.data + 4, 4);
				if (!st.own || magic != PROBE_MAGIC)
					continue;
				for (i = 0; i < (int)o->window; i++)
					if (slots[i].used &&
					    slots[i].seq == rseq) {
						sl = &slots[i];
						break;
					}
				if (sl) {
					sl->ts[TS_T2] = st.sw_ns;
					sl->ts[TS_X] = st.hw_ns;
					sl->got_echo = true;
				} else if ((int32_t)(seq_next - rseq) > 0) {
					pc->late_echo++;
				}
			}
			/* drain the remote socket: T2/T3/T4 */
			while (recv_frame(s_rx, &rf, &st) > 0) {
				uint32_t magic, rseq;
				struct probe_slot *sl = NULL;

				memcpy(&magic, rf.data, 4);
				memcpy(&rseq, rf.data + 4, 4);
				if (st.own || magic != PROBE_MAGIC)
					continue;
				for (i = 0; i < (int)o->window; i++)
					if (slots[i].used &&
					    slots[i].seq == rseq) {
						sl = &slots[i];
						break;
					}
				if (sl) {
					sl->ts[TS_T5] = now_real_ns();
					sl->ts[TS_Y] = st.hw_ns;
					sl->ts[TS_NICRX] = st.sw_ns;
					sl->got_rx = true;
				} else if ((int32_t)(seq_next - rseq) > 0) {
					pc->late_rx++;
				}
			}
		}

		/* finalize completed probes; expire the ones past deadline */
		now = now_mono_ns();
		for (i = 0; i < (int)o->window; i++) {
			struct probe_slot *sl = &slots[i];

			if (!sl->used)
				continue;
			if (sl->got_rx && sl->got_echo) {
				record_sample(pc, csv, sl->seq, sl->ts);
				sl->used = false;
				outstanding--;
			} else if (now >= sl->deadline || stop_flag) {
				if (sl->got_rx) {
					pc->echo_miss++;
					record_sample(pc, csv, sl->seq, sl->ts);
				} else {
					pc->lost++;
				}
				sl->used = false;
				outstanding--;
			}
		}
	}
}

/* Measurement loop for one wired pair. Runs lockstep (one frame in
 * flight), paces on an absolute clock grid, accumulates the segment
 * histograms and optionally dumps per-frame timestamps to CSV. */
static void *pair_thread(void *arg)
{
	struct pair_ctx *pc = arg;
	const struct opts *o = pc->opts;
	char tx_dev[20], rx_dev[20], csv_path[40];
	uint32_t can_id = PROBE_ID_BASE + (uint32_t)pc->tx_ch;
	size_t mtu = (o->size > 8) ? CANFD_MTU : CAN_MTU;
	int s_tx, s_echo, s_rx;
	FILE *csv = NULL;
	struct timespec next;
	uint64_t end_ns = o->duration_s ?
		now_mono_ns() + (uint64_t)o->duration_s * 1000000000ull : 0;

	snprintf(tx_dev, sizeof(tx_dev), "eth2can%d", pc->tx_ch);
	snprintf(rx_dev, sizeof(rx_dev), "eth2can%d", pc->rx_ch);

	s_tx = open_can_socket(tx_dev, o->size > 8, UINT32_MAX, true);
	s_echo = open_can_socket(tx_dev, o->size > 8, can_id, true);
	s_rx = open_can_socket(rx_dev, o->size > 8, can_id, true);
	if (s_tx < 0 || s_echo < 0 || s_rx < 0)
		goto out;
	/* the TX socket never reads anything back */
	setsockopt(s_tx, SOL_CAN_RAW, CAN_RAW_FILTER, NULL, 0);

	if (o->csv) {
		snprintf(csv_path, sizeof(csv_path), "pair-%d-%d.csv",
			 pc->tx_ch, pc->rx_ch);
		csv = fopen(csv_path, "w");
		if (csv)
			fprintf(csv, "seq,t1_ns,t2_ns,X_gw,Y_gw,nicrx_ns,t5_ns\n");
	}

	pc->elapsed_ns = now_mono_ns();

	if (o->window > 1) {
		pair_run_window(pc, s_tx, s_echo, s_rx, can_id, mtu, csv);
		goto done;
	}

	clock_gettime(CLOCK_MONOTONIC, &next);

	for (uint32_t seq = 0; !stop_flag; seq++) {
		uint64_t ts[TS_N];

		if (o->count && seq >= (uint32_t)o->count)
			break;
		if (end_ns && now_mono_ns() >= end_ns)
			break;

		pc->sent++;
		if (probe_once(pc, s_tx, s_echo, s_rx, can_id, seq, mtu,
			       ts)) {
			pc->lost++;
			goto pace;
		}

		record_sample(pc, csv, seq, ts);
pace:
		next.tv_nsec += (long)o->gap_us * 1000;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
				       &next, NULL) == EINTR && !stop_flag)
			;
	}

done:
	pc->elapsed_ns = now_mono_ns() - pc->elapsed_ns;
	if (csv)
		fclose(csv);
out:
	if (s_tx >= 0)
		close(s_tx);
	if (s_echo >= 0)
		close(s_echo);
	if (s_rx >= 0)
		close(s_rx);
	pc->done = 1;
	return NULL;
}

/* Print the measurement-path diagram and the timestamp/segment legend.
 * Shown once at startup so every run is self-describing. */
static void print_banner(const struct opts *o, int npairs)
{
	printf(
"+----------------------------------------------------------------------------------+\n"
"| canperf - app-to-app CAN latency: 5 exact segments (us), no clock mapping        |\n"
"+----------------------------------------------------------------------------------+\n"
"|                                                                                  |\n"
"|   i.MX95 / Linux            MCXE31B gateway              i.MX95 / Linux          |\n"
"|   app - eth2can drv -eth->  MCU == 6x CANFD == MCU  -eth-> drv - app             |\n"
"|                                                                                  |\n"
"|   one frame, left to right in time:                                              |\n"
"|                                                                                  |\n"
"|   t1         t2          t3              t4          nrx        t5               |\n"
"|   +----------+...........+===============+...........+----------+                |\n"
"|   app        eth out     MCU in          MCU out     NIC in     app              |\n"
"|   send       (Linux)     (gw clk)        (gw clk)    (Linux)    recv             |\n"
"|                                                                                  |\n"
"|   + point    - Linux-clock seg    . eth wire (derived)    = gateway-clock        |\n"
"|                                                                                  |\n"
"+----------+-----------+-----------------------------------------------------------+\n"
"| segment  | span      | what it measures                                          |\n"
"| total    | t5 - t1   | end to end, app to app (Linux clock)                      |\n"
"| L1 lnxTX | t2 - t1   | app write() + CAN socket/qdisc + eth2can driver           |\n"
"| L2 wire  | derived   | both eth directions: downlink + uplink + switch + NIC     |\n"
"| L3 mcu   | t4 - t3   | MCU eth-in to eth-out, incl. the CAN bus round trip       |\n"
"| L4 lnxRX | t5 - nrx  | NIC ingress -> kernel stack -> app recv() wakeup          |\n"
"+----------+-----------+-----------------------------------------------------------+\n"
"| Every segment is an exact same-clock difference: no cross-clock mapping, no      |\n"
"| bias. L3 is on the gateway clock (t3,t4 both there); the rest on the Linux       |\n"
"| clock. L2 = total - L1 - L3 - L4, i.e. the two eth-wire crossings.               |\n"
"+----------------------------------------------------------------------------------+\n");
	char run[40];

	if (o->duration_s)
		snprintf(run, sizeof(run), "duration=%lds", o->duration_s);
	else if (o->count)
		snprintf(run, sizeof(run), "frames/pair=%ld", o->count);
	else
		snprintf(run, sizeof(run), "frames/pair=unbounded");
	printf("  pairs=%d%s  %s  gap=%dus  win=%u  size=%dB %s  rate=%u/%u\n",
	       npairs, o->bidir ? " (bidir)" : "", run, o->gap_us, o->window,
	       o->size, o->size > 8 ? "FD+BRS" : "classical",
	       o->bitrate, o->dbitrate);
	{
		uint64_t wns = frame_wire_ns(o->size, o->bitrate, o->dbitrate);

		printf("  bus theory: %.1fus/frame -> max %.0f fps per bus (worst-case stuffing)\n\n",
		       (double)wns / 1e3, 1e9 / (double)wns);
	}
}

/* Print the full five-segment result table for one pair (84 columns). */
static void pair_report(const struct pair_ctx *pc)
{
	printf("\n"
"+- eth2can%d -> eth2can%d -- latency (us) -------------------------------------------+\n",
	       pc->tx_ch, pc->rx_ch);
	printf(
"| segment  | span   | trust  |    min |    p50 |    p99 |  p99.9 |    max |   mean |\n"
"+----------+--------+--------+--------+--------+--------+--------+--------+--------+\n");
	hist_row("total",    "t5-t1", "exact", &pc->h_total);
	hist_row("L1 lnxTX", "t2-t1", "exact", &pc->h_l1);
	hist_row("L2 wire",  "deriv", "exact", &pc->h_l2);
	hist_row("L3 mcu",   "Y-X",   "exact", &pc->h_l3);
	hist_row("L4 lnxRX", "t5-nrx","exact", &pc->h_l4);
	printf(
"+----------+--------+--------+--------+--------+--------+--------+--------+--------+\n");
	printf("| sent %-9llu lost %-7llu echo-miss %-7llu no-gw-stamp %-7llu%16s|\n",
	       (unsigned long long)pc->sent, (unsigned long long)pc->lost,
	       (unsigned long long)pc->echo_miss,
	       (unsigned long long)pc->no_hw_stamp, "");
	{
		char note[96];
		double fps = pc->elapsed_ns ? (double)pc->h_total.count * 1e9 /
					      (double)pc->elapsed_ns : 0.0;

		/* late = arrived after the deadline (charged lost/echo-miss
		 * at the time): never-arrived = lost - late rx */
		snprintf(note, sizeof(note),
			 "late(>%dms): rx %llu echo %llu | completed %.0f fps",
			 WAIT_FRAME_MS,
			 (unsigned long long)pc->late_rx,
			 (unsigned long long)pc->late_echo, fps);
		printf("| %-80s |\n", note);
	}
	if (pc->h_l2.clamped) {
		char note[112];

		snprintf(note, sizeof(note),
			 "note: %llu L2 samples clamped at 0 (clock-rate skew on the derived segment)",
			 (unsigned long long)pc->h_l2.clamped);
		printf("| %-80s |\n", note);
	}
	if (pc->no_hw_stamp && pc->no_hw_stamp >= pc->h_total.count)
		printf("| %-80s |\n",
		       "WARNING: no gateway timestamps (driver/firmware mismatch) - only total/L1/L4 valid");
	if (pc->opts->csv) {
		char note[64];

		snprintf(note, sizeof(note), "per-frame data: pair-%d-%d.csv",
			 pc->tx_ch, pc->rx_ch);
		printf("| %-80s |\n", note);
	}
	printf(
"+----------------------------------------------------------------------------------+\n");
}

/* Print the cross-pair summary table (one line per pair, p99 focus). */
static void summary_table(struct pair_ctx *pairs, int npairs)
{
	/* 84 columns to match the per-pair latency table; header and rows share
	 * the same %-field widths so labels sit over their data. */
	printf("\n"
"+- summary (us) -------------------------------------------------------------------+\n"
"| %-13s | %7s | %4s | %17s | %6s %6s %6s %6s |\n"
"+---------------+---------+------+-------------------+-----------------------------+\n",
	       "pair", "frames", "lost", "p50 / p99 / max",
	       "L1p99", "L2p99", "L3p99", "L4p99");
	for (int i = 0; i < npairs; i++) {
		struct pair_ctx *pc = &pairs[i];
		char pr[16], ppm[24];

		snprintf(pr, sizeof(pr), "eth2can%d > %d", pc->tx_ch, pc->rx_ch);
		snprintf(ppm, sizeof(ppm), "%.0f / %.0f / %.0f",
			 hist_pct(&pc->h_total, 50), hist_pct(&pc->h_total, 99),
			 (double)pc->h_total.max_ns / 1000.0);
		printf("| %-13s | %7llu | %4llu | %17s | %6.0f %6.0f %6.0f %6.0f |\n",
		       pr, (unsigned long long)pc->sent,
		       (unsigned long long)pc->lost, ppm,
		       hist_pct(&pc->h_l1, 99), hist_pct(&pc->h_l2, 99),
		       hist_pct(&pc->h_l3, 99), hist_pct(&pc->h_l4, 99));
	}
	printf(
"+---------------+---------+------+-------------------+-----------------------------+\n");
}

/* ======================================================================== */

/* ========================================================================
 * Gateway/driver counter bracketing
 *
 * Snapshot the loss-relevant counters from the eth2can debugfs before
 * and after the run and print the delta: every report then carries its
 * own evidence of where (or that nowhere) frames were lost.
 * ======================================================================== */

struct cnt_snap {
	bool ok;
	uint64_t drv_lost, drv_gaps, drv_nomem, drv_trunc, drv_tx_cn;
	uint64_t gw_lost, gw_rej, gw_starv, gw_sfail, gw_txbusy, gw_ovf;
	uint64_t gw_up, gw_sent, gw_loop;
	uint64_t gw_emac_drop, gw_emac_err;	/* MCU EMAC RX-ring drops/errors */
	uint64_t rej_ctrl, rej_stopped, rej_ovf; /* non-OK TXC by reason */
};

/* Return the unsigned number right after `key` in `s`, 0 when absent. */
static uint64_t find_u64(const char *s, const char *key)
{
	const char *p = strstr(s, key);

	return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

/* Read /sys/kernel/debug/eth2can/stats (local file, zero tooling).
 * Unreadable (no driver / not root) => snap->ok stays false and the
 * delta block is skipped. */
static void counters_snapshot(struct cnt_snap *s)
{
	static char buf[8192];
	const char *gw, *drv, *p;
	size_t n;
	FILE *f = fopen("/sys/kernel/debug/eth2can/stats", "r");

	memset(s, 0, sizeof(*s));
	if (!f)
		return;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';

	gw = strstr(buf, "\ngw: ");
	drv = strstr(buf, "\ndrv: ");
	if (!gw || !drv)
		return;
	s->gw_lost = find_u64(gw, "lost=");
	s->gw_rej = find_u64(gw, "rej=");
	s->gw_up = find_u64(gw, "up=");
	s->gw_starv = find_u64(gw, "starv=");
	s->gw_sfail = find_u64(gw, "sfail=");
	s->gw_sent = find_u64(gw, "sent=");
	s->gw_txbusy = find_u64(gw, "txbusy=");
	s->gw_loop = find_u64(gw, "loop=");
	s->gw_emac_drop = find_u64(gw, "rxdrop=");
	s->gw_emac_err = find_u64(gw, "rxerr=");
	for (p = strstr(buf, "gw CAN"); p; p = strstr(p + 1, "gw CAN"))
		s->gw_ovf += find_u64(p, " ovf=");
	p = strstr(buf, "txc_rej");
	if (p) {
		s->rej_ctrl = find_u64(p, "ctrl_error=");
		s->rej_stopped = find_u64(p, "chan_stopped=");
		s->rej_ovf = find_u64(p, "queue_ovf=");
	}
	s->drv_lost = find_u64(drv, "lost=");
	s->drv_gaps = find_u64(drv, "gaps=");
	s->drv_nomem = find_u64(drv, "nomem=");
	s->drv_trunc = find_u64(drv, "trunc=");
	s->drv_tx_cn = find_u64(drv, "tx_cn=");
	s->ok = true;
}

/* Print the counter movement across the run. */
static void counters_delta(const struct cnt_snap *a, const struct cnt_snap *b)
{
	if (!a->ok || !b->ok) {
		printf("\n(counter delta unavailable: /sys/kernel/debug/eth2can/stats not readable)\n");
		return;
	}
#define DELTA(f) ((unsigned long long)(b->f - a->f))
	char c1[24], c2[24], c3[24];

	printf("\n+------------------ counters delta (this run) ------------------+\n");
	snprintf(c1, sizeof(c1), "seq_lost +%llu", DELTA(drv_lost));
	snprintf(c2, sizeof(c2), "gap-evt +%llu", DELTA(drv_gaps));
	snprintf(c3, sizeof(c3), "nomem +%llu", DELTA(drv_nomem));
	printf("| %-4s %-18s %-18s %-18s |\n", "drv", c1, c2, c3);
	snprintf(c1, sizeof(c1), "trunc +%llu", DELTA(drv_trunc));
	snprintf(c2, sizeof(c2), "tx_cn +%llu", DELTA(drv_tx_cn));
	printf("| %-4s %-18s %-18s %-18s |\n", "", c1, c2, "");
	snprintf(c1, sizeof(c1), "seq_lost +%llu", DELTA(gw_lost));
	snprintf(c2, sizeof(c2), "rej +%llu", DELTA(gw_rej));
	snprintf(c3, sizeof(c3), "starv +%llu", DELTA(gw_starv));
	printf("| %-4s %-18s %-18s %-18s |\n", "gw", c1, c2, c3);
	snprintf(c1, sizeof(c1), "sfail +%llu", DELTA(gw_sfail));
	snprintf(c2, sizeof(c2), "txbusy +%llu", DELTA(gw_txbusy));
	snprintf(c3, sizeof(c3), "rx_ovf +%llu", DELTA(gw_ovf));
	printf("| %-4s %-18s %-18s %-18s |\n", "", c1, c2, c3);
	printf("+---------------------------------------------------------------+\n");
	printf("  uplink +%llu recs in +%llu eth frames  |  loop/s %llu -> %llu\n",
	       DELTA(gw_up), DELTA(gw_sent),
	       (unsigned long long)a->gw_loop, (unsigned long long)b->gw_loop);
	if (DELTA(drv_lost))
		printf("  ! driver seq_lost  = UPLINK LOSS\n");
	if (DELTA(gw_lost))
		/* locate the downlink drop: EMAC rxdrop>0 => MCU EQOS RX ring
		 * overflow; both 0 => lost upstream on the NIC/switch/wire */
		printf("  ! downlink seq_lost +%llu : emac_rxdrop +%llu emac_rxerr +%llu"
		       " (>0 = MCU RX ring; 0 = NIC/switch)\n",
		       DELTA(gw_lost), DELTA(gw_emac_drop), DELTA(gw_emac_err));
	if (DELTA(gw_rej))
		/* locate the submit reject by reason */
		printf("  ! rej +%llu : chan_stopped +%llu queue_ovf +%llu ctrl_error +%llu\n",
		       DELTA(gw_rej), DELTA(rej_stopped), DELTA(rej_ovf),
		       DELTA(rej_ctrl));
#undef DELTA
}

/* ========================================================================
 * Run orchestration (shared by the normal run and the sweep)
 * ======================================================================== */

/* Clear one pair's accumulated results (between sweep steps). */
static void pair_reset(struct pair_ctx *pc)
{
	memset(&pc->h_total, 0, sizeof(pc->h_total));
	memset(&pc->h_l1, 0, sizeof(pc->h_l1));
	memset(&pc->h_l2, 0, sizeof(pc->h_l2));
	memset(&pc->h_l3, 0, sizeof(pc->h_l3));
	memset(&pc->h_l4, 0, sizeof(pc->h_l4));
	pc->sent = pc->lost = pc->echo_miss = pc->no_hw_stamp = 0;
	pc->late_rx = pc->late_echo = 0;
	pc->elapsed_ns = 0;
}

/* --- live TTY dashboard ----------------------------------------------------
 * When stdout is a terminal, the long-running modes refresh a compact table in
 * place (cursor moved up over the previous draw) instead of scrolling. Cadence
 * is data-driven: 1 s while the shown values move, 5 s once they hold steady.
 * Rendering runs in the main thread (idle, just waiting on the workers) and
 * reads the stats unlocked - display-only, so a slightly stale value is fine;
 * at >=1 s cadence the cost is far below the measurement noise floor. When
 * stdout is redirected the dashboard is off and only the final tables print. */
#define LIVE_EOL "\033[K\n" /* clear to end-of-line, then newline */

static void live_cursor_up(int n)
{
	if (n > 0)
		printf("\033[%dA", n);
}

/* Round-trip live view: one line per pair (sent / lost / total p50-p99).
 * Returns the line count; *hash changes only when the shown values change. */
static int live_rt_draw(struct pair_ctx *pairs, int npairs, double el,
			unsigned int *hash)
{
	unsigned int h = 0;
	int lines = 3, i;

	printf("  latency (live)  elapsed %6.1fs  refresh 1-5s  Ctrl-C to stop" LIVE_EOL,
	       el);
	printf("  pair        sent     lost    total p50 / p99 (us)" LIVE_EOL);
	printf("  ----------------------------------------------------" LIVE_EOL);
	for (i = 0; i < npairs; i++) {
		struct pair_ctx *pc = &pairs[i];
		double p50 = hist_pct(&pc->h_total, 50);
		double p99 = hist_pct(&pc->h_total, 99);

		printf("  %d->%-4d %9llu %8llu    %8.0f / %-8.0f" LIVE_EOL,
		       pc->tx_ch, pc->rx_ch, (unsigned long long)pc->sent,
		       (unsigned long long)pc->lost, p50, p99);
		lines++;
		h = h * 131u + (unsigned int)p50 + (unsigned int)p99 * 7u +
		    (unsigned int)pc->lost * 13u;
	}
	fflush(stdout);
	*hash = h;
	return lines;
}

/* Launch one measurement thread per pair, refresh the live dashboard (TTY) or
 * silently wait (redirected) until all finish, join. `quiet` (sweep) forces the
 * silent path. Returns 1 on thread-creation failure. */
static int run_pairs(const struct opts *o, struct pair_ctx *pairs,
		     int npairs, bool quiet)
{
	int i;

	for (i = 0; i < npairs; i++) {
		pairs[i].opts = o;
		pairs[i].done = 0;
		if (pthread_create(&pairs[i].thread, NULL, pair_thread,
				   &pairs[i])) {
			perror("pthread_create");
			/* stop and reap the threads already running so they
			 * are not killed mid-write by exit() */
			stop_flag = 1;
			while (--i >= 0)
				pthread_join(pairs[i].thread, NULL);
			return 1;
		}
	}

	{
		bool live = g_live && !quiet;
		uint64_t start = now_mono_ns(), last_render = 0;
		double interval = 1.0;
		unsigned int last_hash = 0;
		int prev_lines = 0, first = 1;

		for (;;) {
			bool all_done = true;
			uint64_t now = now_mono_ns();

			for (i = 0; i < npairs; i++)
				if (!pairs[i].done)
					all_done = false;
			if (live && (first ||
			    (now - last_render) >= (uint64_t)(interval * 1e9))) {
				unsigned int h;

				live_cursor_up(prev_lines);
				prev_lines = live_rt_draw(pairs, npairs,
					(double)(now - start) / 1e9, &h);
				interval = (!first && h == last_hash) ? 5.0 : 1.0;
				last_hash = h;
				last_render = now;
				first = 0;
			}
			if (all_done)
				break;
			usleep(live ? 200000 : 100000);
		}
		if (live)
			printf("\n"); /* leave the dashboard; final tables follow */
	}

	for (i = 0; i < npairs; i++)
		pthread_join(pairs[i].thread, NULL);
	return 0;
}

/* ========================================================================
 * --sweep: max-sustainable-rate search
 *
 * Windowed engine at a controlled offered rate per step. Ladder up from
 * theory/8 doubling the rate until a step fails (any loss, or the worst
 * pair's total p99 above --p99-limit), then binary-search the pass/fail
 * gap down to ~3%. All pairs run concurrently at the same offered rate;
 * the verdict takes the worst pair.
 * ======================================================================== */
static int sweep_run(struct opts *o, struct pair_ctx *pairs, int npairs)
{
	uint64_t wns = frame_wire_ns(o->size, o->bitrate, o->dbitrate);
	double theory = 1e9 / (double)wns;
	double lo = 0, hi = 0, rate;
	double best_deliv = 0, best_off = 0; /* best DELIVERED rate + its offer */
	bool saturated = false; /* offered rose but delivered plateaued */
	int step = 0;

	o->count = o->sweep_frames;
	o->duration_s = 0;

	printf("sweep: %d pair(s), window=%u, %ld frames/step, criterion: lost=0 & p99<=%dus\n",
	       npairs, o->window, o->sweep_frames, o->p99_limit_us);
	printf("+------+------------+-----------+----------+----------+---------+\n"
	       "| step | offered    | delivered |   lost   |  p99(us) | verdict |\n"
	       "+------+------------+-----------+----------+----------+---------+\n");

	rate = theory / 8;
	if (rate < 200)
		rate = 200;
	while (!stop_flag) {
		uint64_t lost = 0, completed = 0;
		double p99 = 0, ach = 0;
		bool pass;
		int i;

		o->gap_us = (int)(1e6 / rate);
		if (o->gap_us < 1)
			o->gap_us = 1;
		for (i = 0; i < npairs; i++)
			pair_reset(&pairs[i]);
		if (run_pairs(o, pairs, npairs, true))
			return 1;
		for (i = 0; i < npairs; i++) {
			double p = hist_pct(&pairs[i].h_total, 99);

			lost += pairs[i].lost;
			completed += pairs[i].h_total.count;
			if (p > p99)
				p99 = p;
			if (pairs[i].elapsed_ns)
				ach += (double)pairs[i].h_total.count * 1e9 /
				       (double)pairs[i].elapsed_ns;
		}
		ach /= npairs;
		/* a step with (almost) no completed probes is broken setup,
		 * never a pass - e.g. sockets failed to open */
		pass = !lost && p99 <= (double)o->p99_limit_us && !stop_flag &&
		       completed >= (uint64_t)o->sweep_frames *
				    (uint64_t)npairs * 9U / 10U;
		printf("| %4d | %10.0f | %9.0f | %8llu | %8.0f | %-7s |\n",
		       ++step, rate, ach, (unsigned long long)lost, p99,
		       stop_flag ? "ABORT" : pass ? "PASS" : "FAIL");
		if (stop_flag)
			break;
		if (pass) {
			/* Saturation knee: a passing step whose DELIVERED rate
			 * did not climb >3% over the best so far means the
			 * window/pipeline is delivering its max - offering more
			 * only fills the in-flight window, not the wire. That
			 * delivered rate IS the throughput ceiling; stop here
			 * rather than chasing the offered rate to its cap. */
			if (best_deliv > 0 && ach <= best_deliv * 1.03) {
				saturated = true;
				break;
			}
			if (ach > best_deliv) {
				best_deliv = ach;
				best_off = rate;
			}
			lo = rate;
			if (hi) {
				if (hi - lo <= lo * 0.03)
					break;
				rate = (lo + hi) / 2;
			} else if (rate >= theory * 1.02) {
				break;	/* offered-rate cap reached */
			} else {
				rate *= 2;
				if (rate > theory * 1.02)
					rate = theory * 1.02;
			}
		} else {
			hi = rate;
			if (!lo) {
				rate /= 2;
				if (rate < 100)
					break;
			} else {
				if (hi - lo <= lo * 0.03)
					break;
				rate = (lo + hi) / 2;
			}
		}
	}
	printf("+------+------------+-----------+----------+----------+---------+\n");
	if (best_deliv > 0) {
		/* Physical CAN buses carrying the load: --bidir mirrors every
		 * wire so two opposing pairs share one half-duplex bus; without
		 * it each pair owns its bus. Per-bus utilization (not per-pair vs
		 * full bus) is the honest saturation metric. */
		int buses = o->bidir ? npairs / 2 : npairs;
		double per_bus = (buses > 0) ? best_deliv * npairs / buses : best_deliv;
		double bus_util = 100.0 * per_bus / theory;

		/* MSR is the DELIVERED rate (what the system actually carried
		 * with zero loss and bounded p99), not the offered rate. */
		printf("MSR = %.0f fps/pair DELIVERED (%d concurrent, %.0f fps aggregate)\n",
		       best_deliv, npairs, best_deliv * npairs);
		if (o->bidir)
			printf("  per CAN bus: %.0f fps across 2 directions = %.0f%% of bus theory (%.0f fps/bus)\n",
			       per_bus, bus_util, theory);
		else
			printf("  per CAN bus: %.0f fps = %.0f%% of bus theory (%.0f fps/bus)\n",
			       per_bus, bus_util, theory);

		/* Evidence-based ceiling classification: a bus sitting near its
		 * physical ceiling is bus-bound (the wire is the limit); a plateau
		 * well below it is pipeline-bound (window depth / face pool / ISR
		 * upstream of the wire), which is the only case worth optimizing. */
		if (bus_util >= 65.0) {
			printf("  ceiling: CAN-bus-bound - each bus is near its physical limit%s\n",
			       o->bidir ?
			       "; bidir shares one half-duplex wire between two directions,\n"
			       "    so per-pair is ~half the bus and CAN arbitration caps it below unidirectional"
			       : "");
		} else if (saturated || best_off > best_deliv * 1.05) {
			printf("  ceiling: pipeline-bound - delivered plateaued at ~%.0f fps/pair (offered up to %.0f)\n"
			       "    while the bus sat at only %.0f%%, so the limit is UPSTREAM of the wire\n"
			       "    (window depth / face pool / ISR), not the CAN bus - this is the case to optimize\n",
			       best_deliv, best_off, bus_util);
		}
	} else {
		printf("no passing rate found (link broken, or lower the start rate / raise --p99-limit)\n");
	}
	return (best_deliv > 0) ? 0 : 1;
}

/* ========================================================================
 * --performance: decoupled one-way throughput + loss (no latency)
 *
 * One wired pair, two independent threads: a TX thread floods `count` frames
 * on eth2can<tx> paced by --gap-us; an RX thread counts the frames that arrive
 * on eth2can<rx> (the partner that physically receives them over the CAN bus).
 * No probe ever waits for a return, so unlike the round-trip engines this
 * measures the raw forward path (Linux TX + CAN bus delivery). A per-seq bitmap
 * gives the exact unique-receive count, hence exact loss. Reports offered FPS,
 * delivered FPS and loss rate only - latency is out of scope here.
 * ======================================================================== */
#define PERF_BATCH 64	/* frames per recvmmsg/sendmmsg syscall */

/* ========================================================================
 * --performance: decoupled one-way throughput + loss (no latency)
 *
 * One TX thread and one RX thread drive ALL channels, so the thread count is
 * fixed at 2 regardless of how many pairs run (no per-pair threads, no core
 * oversubscription). The TX thread round-robins non-blocking sendmmsg across
 * every channel and, when all windows are full, usleep()s to YIELD - CAN raw
 * sockets do NOT signal TX-window backpressure via EPOLLOUT (poll-out tracks
 * the socket buffer, not the gateway window), so epoll on the TX side would
 * busy-spin at RT priority and starve the softirq that frees the window. The
 * RX side CAN use epoll (EPOLLIN is valid for CAN): the RX thread drains every
 * readable channel (recvmmsg), skipping the local software echo so
 * controller-loopback (x:x) is not double-counted. A per-seq bitmap gives exact
 * loss for count-bound runs; duration runs count (loss = sent - recv).
 * ======================================================================== */
struct perf_ctx {
	int tx_ch, rx_ch;
	const struct opts *o;
	long count;			/* 0 = duration-bound */
	/* TX state (owned by the single TX thread) */
	int tx_fd;
	uint32_t tx_seq;
	uint64_t sent;
	uint64_t grid;			/* next send time, paced mode */
	uint64_t tx_start_ns, tx_end_ns;
	int tx_pair_done;
	int tx_err;
	/* RX state (owned by the single RX thread) */
	int rx_fd;
	uint64_t recv, rx_dup, rx_alien;
	uint64_t rx_first_ns, rx_last_ns;
	uint8_t *seen;			/* per-seq bitmap (count mode) */
};

struct perf_set {
	struct perf_ctx *ctx;
	int npairs;
	const struct opts *o;
	volatile int tx_all_done;
	volatile int rx_done;
};

/* Pin the calling thread to one CPU (no-op when cpu < 0). */
static void perf_pin(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return;
	CPU_ZERO(&set);
	CPU_SET((size_t)cpu, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

/* Single TX thread for all channels. CAN raw sockets do NOT report TX-window
 * backpressure via EPOLLOUT (poll-out is always ready on the socket buffer),
 * so epoll is useless here and a non-blocking busy-retry would spin at RT
 * priority and starve the softirq that processes the uplink TXCs (which free
 * the window) - deadlocking after ~one window. Instead: non-blocking sendmmsg
 * round-robin, and when every channel's window is full, usleep() to YIELD so
 * the softirq can free a slot. --gap-us > 1 paces each channel on its grid. */
static void *perf_tx_thread(void *arg)
{
	struct perf_set *S = arg;
	const struct opts *o = S->o;
	size_t mtu = (o->size > 8) ? CANFD_MTU : CAN_MTU;
	bool paced = (o->gap_us > 1);
	struct perf_tx_batch {
		struct mmsghdr m[PERF_BATCH];
		struct iovec iov[PERF_BATCH];
		struct canfd_frame cf[PERF_BATCH];
	} *B;
	uint64_t start, end_ns;
	int i, j, remaining = S->npairs;

	perf_pin(0);
	B = calloc((size_t)S->npairs, sizeof(*B));
	if (!B) {
		for (i = 0; i < S->npairs; i++) {
			S->ctx[i].tx_err = 1;
			S->ctx[i].tx_pair_done = 1;
		}
		S->tx_all_done = 1;
		return NULL;
	}
	start = now_mono_ns();
	end_ns = o->duration_s ? start + (uint64_t)o->duration_s * 1000000000ull : 0;
	for (i = 0; i < S->npairs; i++) {
		struct perf_ctx *p = &S->ctx[i];
		char dev[20];
		int fl;

		snprintf(dev, sizeof(dev), "eth2can%d", p->tx_ch);
		p->tx_fd = open_can_socket(dev, o->size > 8,
					   PROBE_ID_BASE + (uint32_t)p->tx_ch, false);
		if (p->tx_fd < 0) {
			p->tx_err = 1;
			p->tx_pair_done = 1;
			remaining--;
			continue;
		}
		fl = fcntl(p->tx_fd, F_GETFL, 0);
		fcntl(p->tx_fd, F_SETFL, fl | O_NONBLOCK);
		p->tx_start_ns = start;
		p->grid = start;
		for (j = 0; j < PERF_BATCH; j++) {
			memset(&B[i].cf[j], 0, sizeof(B[i].cf[j]));
			B[i].cf[j].can_id = PROBE_ID_BASE + (uint32_t)p->tx_ch;
			B[i].cf[j].len = (uint8_t)o->size;
			if (o->size > 8)
				B[i].cf[j].flags = CANFD_BRS;
			memcpy(B[i].cf[j].data, &(uint32_t){ PROBE_MAGIC }, 4);
			B[i].iov[j].iov_base = &B[i].cf[j];
			B[i].iov[j].iov_len = mtu;
			memset(&B[i].m[j].msg_hdr, 0, sizeof(B[i].m[j].msg_hdr));
			B[i].m[j].msg_hdr.msg_iov = &B[i].iov[j];
			B[i].m[j].msg_hdr.msg_iovlen = 1;
		}
	}

	while (remaining > 0 && !stop_flag) {
		uint64_t now = now_mono_ns();
		uint64_t min_grid = (uint64_t)-1;
		int progressed = 0;

		if (end_ns && now >= end_ns)
			break;
		for (i = 0; i < S->npairs; i++) {
			struct perf_ctx *p = &S->ctx[i];
			int batch, m;

			if (p->tx_pair_done)
				continue;
			if (paced && p->grid > now) {
				if (p->grid < min_grid)
					min_grid = p->grid;
				continue;
			}
			if (end_ns) {
				batch = PERF_BATCH;
			} else {
				batch = (int)((uint32_t)p->count - p->tx_seq);
				if (batch > PERF_BATCH)
					batch = PERF_BATCH;
				if (batch <= 0) {
					p->tx_pair_done = 1;
					p->tx_end_ns = now;
					remaining--;
					continue;
				}
			}
			for (j = 0; j < batch; j++)
				memcpy(B[i].cf[j].data + 4,
				       &(uint32_t){ p->tx_seq + (uint32_t)j }, 4);
			m = sendmmsg(p->tx_fd, B[i].m, (unsigned int)batch, MSG_DONTWAIT);
			if (m > 0) {
				p->sent += (uint64_t)m;
				p->tx_seq += (uint32_t)m;
				progressed = 1;
				if (paced)
					p->grid += (uint64_t)m *
						   (uint64_t)o->gap_us * 1000ull;
				if (!end_ns && p->tx_seq >= (uint32_t)p->count) {
					p->tx_pair_done = 1;
					p->tx_end_ns = now_mono_ns();
					remaining--;
				}
			} else if (errno == EAGAIN || errno == ENOBUFS ||
				   errno == EINTR) {
				/* this channel's window/qdisc is full */
			} else {
				p->tx_err = 1;
				p->tx_pair_done = 1;
				p->tx_end_ns = now;
				remaining--;
			}
		}
		if (remaining <= 0)
			break;
		if (!progressed) {
			/* nothing went out: yield so the softirq can free a window
			 * slot (busy-retry would starve it). paced: sleep to grid. */
			if (paced && min_grid != (uint64_t)-1) {
				uint64_t w = (end_ns && end_ns < min_grid) ?
					     end_ns : min_grid;
				struct timespec ts = {
					.tv_sec = (time_t)(w / 1000000000ull),
					.tv_nsec = (long)(w % 1000000000ull),
				};

				clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
						&ts, NULL);
			} else {
				usleep(100);
			}
		}
	}
	{
		uint64_t now = now_mono_ns();

		for (i = 0; i < S->npairs; i++) {
			if (S->ctx[i].tx_start_ns && !S->ctx[i].tx_end_ns)
				S->ctx[i].tx_end_ns = now;
			if (S->ctx[i].tx_fd >= 0)
				close(S->ctx[i].tx_fd);
		}
	}
	free(B);
	S->tx_all_done = 1;
	return NULL;
}

/* Single RX thread: epoll over every channel's non-blocking RX socket, draining
 * each readable one with recvmmsg. Skips the local software echo (own TX) so
 * controller loopback is counted once; counts unique in-range frames (bitmap)
 * or all valid frames (duration). Stops once the TX thread is done and the link
 * has been quiet for a drain grace. */
static void *perf_rx_thread(void *arg)
{
	struct perf_set *S = arg;
	const struct opts *o = S->o;
	struct mmsghdr m[PERF_BATCH];
	struct iovec iov[PERF_BATCH];
	struct canfd_frame fr[PERF_BATCH];
	uint64_t quiet_deadline = 0;
	int ep, i;

	perf_pin(1);
	ep = epoll_create1(0);
	if (ep < 0) {
		S->rx_done = 1;
		return NULL;
	}
	for (i = 0; i < S->npairs; i++) {
		struct perf_ctx *p = &S->ctx[i];
		struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)i };
		char dev[20];
		int fl;

		snprintf(dev, sizeof(dev), "eth2can%d", p->rx_ch);
		p->rx_fd = open_can_socket(dev, o->size > 8,
					   PROBE_ID_BASE + (uint32_t)p->tx_ch, false);
		if (p->rx_fd < 0)
			continue;
		fl = fcntl(p->rx_fd, F_GETFL, 0);
		fcntl(p->rx_fd, F_SETFL, fl | O_NONBLOCK);
		epoll_ctl(ep, EPOLL_CTL_ADD, p->rx_fd, &ev);
	}
	for (i = 0; i < PERF_BATCH; i++) {
		iov[i].iov_base = &fr[i];
		iov[i].iov_len = sizeof(fr[i]);
		memset(&m[i].msg_hdr, 0, sizeof(m[i].msg_hdr));
		m[i].msg_hdr.msg_iov = &iov[i];
		m[i].msg_hdr.msg_iovlen = 1;
	}

	while (!stop_flag) {
		struct epoll_event evs[MAX_PAIRS];
		int n = epoll_wait(ep, evs, S->npairs, 50);
		uint64_t now = now_mono_ns();
		int got_any = 0, e;

		for (e = 0; e < n; e++) {
			struct perf_ctx *p = &S->ctx[evs[e].data.u32];
			int got;

			while ((got = recvmmsg(p->rx_fd, m, PERF_BATCH,
					       MSG_DONTWAIT, NULL)) > 0) {
				for (i = 0; i < got; i++) {
					uint32_t magic, rseq;

					if (m[i].msg_hdr.msg_flags & MSG_DONTROUTE)
						continue;
					if (m[i].msg_len < 8)
						continue;
					memcpy(&magic, fr[i].data, 4);
					memcpy(&rseq, fr[i].data + 4, 4);
					if (magic != PROBE_MAGIC) {
						p->rx_alien++;
						continue;
					}
					if (p->seen) {
						if (rseq >= (uint32_t)p->count) {
							p->rx_alien++;
							continue;
						}
						if (p->seen[rseq >> 3] &
						    (uint8_t)(1u << (rseq & 7u))) {
							p->rx_dup++;
							continue;
						}
						p->seen[rseq >> 3] |=
							(uint8_t)(1u << (rseq & 7u));
					}
					if (p->recv == 0)
						p->rx_first_ns = now;
					p->recv++;
				}
				p->rx_last_ns = now;
				got_any = 1;
				if (got < PERF_BATCH)
					break;
			}
		}
		if (S->tx_all_done) {
			if (got_any || quiet_deadline == 0)
				quiet_deadline = now_mono_ns() + 300000000ull;
			if (now_mono_ns() >= quiet_deadline)
				break;
		}
	}
	for (i = 0; i < S->npairs; i++)
		if (S->ctx[i].rx_fd >= 0)
			close(S->ctx[i].rx_fd);
	close(ep);
	S->rx_done = 1;
	return NULL;
}

/* Performance live view: one line per pair plus an aggregate. Returns the line
 * count; *hash changes only when the shown rates change. lost/loss% are '-'
 * live (recv lags sent by frames still in flight; valid only after the drain). */
static int live_perf_draw(struct perf_ctx *ctx, int npairs, double el,
			  unsigned int *hash)
{
	uint64_t a_sent = 0, a_recv = 0;
	double a_tx = 0, a_rx = 0;
	unsigned int h = 0;
	int lines = 3, i;

	printf("  performance (live)  elapsed %6.1fs  refresh 1-5s  Ctrl-C to stop" LIVE_EOL,
	       el);
	printf("  pair        sent      recv    lost    loss%%     TX fps    RX fps" LIVE_EOL);
	printf("  ------------------------------------------------------------------" LIVE_EOL);
	for (i = 0; i < npairs; i++) {
		struct perf_ctx *p = &ctx[i];
		uint64_t now = now_mono_ns();
		double txd = p->tx_start_ns ?
			(double)(now - p->tx_start_ns) / 1e9 : 0.0;
		double rxd = (p->rx_last_ns > p->rx_first_ns) ?
			(double)(p->rx_last_ns - p->rx_first_ns) / 1e9 : 0.0;
		double txf = txd > 0 ? (double)p->sent / txd : 0.0;
		double rxf = rxd > 0 ? (double)p->recv / rxd : 0.0;

		printf("  %d->%-4d %9llu %9llu %7s  %7s  %9.0f %9.0f" LIVE_EOL,
		       p->tx_ch, p->rx_ch, (unsigned long long)p->sent,
		       (unsigned long long)p->recv, "-", "-", txf, rxf);
		lines++;
		a_sent += p->sent;
		a_recv += p->recv;
		a_tx += txf;
		a_rx += rxf;
		h = h * 131u + (unsigned int)(txf / 100) +
		    (unsigned int)(rxf / 100) * 7u;
	}
	if (npairs > 1) {
		printf("  ------------------------------------------------------------------" LIVE_EOL);
		printf("  agg     %9llu %9llu %7s  %7s  %9.0f %9.0f" LIVE_EOL,
		       (unsigned long long)a_sent, (unsigned long long)a_recv,
		       "-", "-", a_tx, a_rx);
		lines += 2;
	}
	fflush(stdout);
	*hash = h;
	return lines;
}

/* Orchestrate a performance run: one TX + one RX epoll thread over all pairs. */
static int perf_run(const struct opts *o, struct pair_ctx *pairs, int npairs)
{
	static struct perf_ctx ctx[MAX_PAIRS];
	struct perf_set S = { .ctx = ctx, .npairs = npairs, .o = o };
	long count = (o->count > 0) ? o->count : 10000;
	uint64_t agg_sent = 0, agg_recv = 0, agg_lost = 0;
	double agg_tx_fps = 0, agg_rx_fps = 0;
	pthread_t tx_t, rx_t;
	int i, rc = 0;

	for (i = 0; i < npairs; i++) {
		memset(&ctx[i], 0, sizeof(ctx[i]));
		ctx[i].tx_ch = pairs[i].tx_ch;
		ctx[i].rx_ch = pairs[i].rx_ch;
		ctx[i].o = o;
		ctx[i].count = count;
		ctx[i].tx_fd = -1;
		ctx[i].rx_fd = -1;
		/* exact-loss bitmap for a count-bound run; a duration run has
		 * unbounded seq, so it counts (loss = sent - recv) */
		if (!o->duration_s) {
			ctx[i].seen = calloc((size_t)((count + 7) / 8), 1);
			if (!ctx[i].seen) {
				fprintf(stderr,
					"error: out of memory for %ld-frame bitmap\n",
					count);
				while (--i >= 0)
					free(ctx[i].seen);
				return 1;
			}
		}
	}

	if (o->duration_s)
		printf("performance: %d pair(s)  gap=%dus  size=%dB %s  duration=%lds%s\n",
		       npairs, o->gap_us, o->size,
		       o->size > 8 ? "FD+BRS" : "classic", o->duration_s,
		       (o->gap_us <= 1) ? "  [blast: sendmmsg/recvmmsg x64]" : "");
	else
		printf("performance: %d pair(s)  gap=%dus  size=%dB %s  count=%ld/pair%s\n",
		       npairs, o->gap_us, o->size,
		       o->size > 8 ? "FD+BRS" : "classic", count,
		       (o->gap_us <= 1) ? "  [blast: sendmmsg/recvmmsg x64]" : "");
	printf("  one TX + one RX epoll thread over all channels (no per-pair threads)\n\n");

	if (pthread_create(&rx_t, NULL, perf_rx_thread, &S) != 0) {
		perror("pthread_create rx");
		rc = 1;
		goto cleanup;
	}
	usleep(50000); /* let the RX sockets bind before the flood */
	if (pthread_create(&tx_t, NULL, perf_tx_thread, &S) != 0) {
		perror("pthread_create tx");
		stop_flag = 1;
		pthread_join(rx_t, NULL);
		rc = 1;
		goto cleanup;
	}

	if (g_live) {
		uint64_t start = now_mono_ns(), last_render = 0;
		double interval = 1.0;
		unsigned int last_hash = 0;
		int prev_lines = 0, first = 1;

		for (;;) {
			uint64_t now = now_mono_ns();
			int done = (S.tx_all_done && S.rx_done);

			if (first || (now - last_render) >=
			    (uint64_t)(interval * 1e9)) {
				unsigned int h;

				live_cursor_up(prev_lines);
				prev_lines = live_perf_draw(ctx, npairs,
					(double)(now - start) / 1e9, &h);
				interval = (!first && h == last_hash) ? 5.0 : 1.0;
				last_hash = h;
				last_render = now;
				first = 0;
			}
			if (done)
				break;
			usleep(200000);
		}
		printf("\n"); /* leave the dashboard; final table follows */
	}

	pthread_join(tx_t, NULL);
	pthread_join(rx_t, NULL);

	printf("  pair        sent      recv    lost    loss%%     TX fps    RX fps\n");
	printf("  ------------------------------------------------------------------\n");
	for (i = 0; i < npairs; i++) {
		double tx_dur = (ctx[i].tx_end_ns > ctx[i].tx_start_ns) ?
			(double)(ctx[i].tx_end_ns - ctx[i].tx_start_ns) / 1e9 : 0.0;
		double rx_dur = (ctx[i].rx_last_ns > ctx[i].rx_first_ns) ?
			(double)(ctx[i].rx_last_ns - ctx[i].rx_first_ns) / 1e9 : 0.0;
		uint64_t lost = (ctx[i].sent > ctx[i].recv) ?
			ctx[i].sent - ctx[i].recv : 0;
		double txf = tx_dur > 0 ? (double)ctx[i].sent / tx_dur : 0.0;
		double rxf = rx_dur > 0 ? (double)ctx[i].recv / rx_dur : 0.0;

		printf("  %d->%-4d %9llu %9llu %7llu  %6.3f%%  %9.0f %9.0f%s\n",
		       ctx[i].tx_ch, ctx[i].rx_ch,
		       (unsigned long long)ctx[i].sent, (unsigned long long)ctx[i].recv,
		       (unsigned long long)lost,
		       ctx[i].sent ? 100.0 * (double)lost / (double)ctx[i].sent : 0.0,
		       txf, rxf, ctx[i].tx_err ? "  TX-ERR" : "");
		agg_sent += ctx[i].sent;
		agg_recv += ctx[i].recv;
		agg_lost += lost;
		agg_tx_fps += txf;
		agg_rx_fps += rxf;
	}
	if (npairs > 1) {
		printf("  ------------------------------------------------------------------\n");
		printf("  agg     %9llu %9llu %7llu  %6.3f%%  %9.0f %9.0f\n",
		       (unsigned long long)agg_sent, (unsigned long long)agg_recv,
		       (unsigned long long)agg_lost,
		       agg_sent ? 100.0 * (double)agg_lost / (double)agg_sent : 0.0,
		       agg_tx_fps, agg_rx_fps);
	}
cleanup:
	for (i = 0; i < npairs; i++)
		free(ctx[i].seen);
	return rc;
}

static void usage(void)
{
	printf(
"canperf - E2CF gateway segmented latency measurement\n"
"            (app-to-app across Linux -> eth -> MCXE31B -> CAN bus -> back)\n"
"\n"
"  ./canperf                 configure channels (1M/8M FD) and measure the\n"
"                              default pairs 0->4, 1->2, 3->5 CONCURRENTLY\n"
"  ./canperf --pair 0:4      measure selected pair(s); repeatable or\n"
"                              comma-separated: --pair 0:4,1:2,3:5\n"
"    --count N      frames per pair (default 5000; 0 = unbounded)\n"
"    --duration T   run for a time instead of a frame count: bare seconds or\n"
"                     a suffix - 100s, 2m, 1h (perf: keeps sending the whole time)\n"
"    --gap-us N     inter-frame pacing (default 1000 us)\n"
"    --size 64|8    64 = CAN FD + BRS (default), 8 = classical\n"
"    --bitrate R    arbitration bitrate, k/M suffix ok (default 1M)\n"
"    --dbitrate R   data bitrate; 0 = classical CAN (default 8M)\n"
"    --report-s N   progress report period (default 10 s)\n"
"    --window N     probes in flight per pair (default 1 = lockstep\n"
"                     latency probing; 2..16 = throughput/loss testing,\n"
"                     --gap-us then sets the OFFERED rate of 1e6/gap fps)\n"
"    --bidir        mirror every pair: A->B adds B->A (full-duplex load)\n"
"    --sweep        max-sustainable-rate search (windowed engine): ladder\n"
"                     then binary-search the highest rate with zero loss\n"
"                     and p99 <= --p99-limit; prints MSR vs bus theory\n"
"    --sweep-frames N  probes per pair per sweep step (default 20000)\n"
"    --p99-limit N  sweep pass criterion in us (default 1000)\n"
"    --performance  one-way throughput+loss test: two threads (TX flood on\n"
"                     --pair x, RX count on y), no round trip, no latency;\n"
"                     reports offered/delivered FPS and loss (default 10000)\n"
"    --loopback     controller loopback (FlexCAN LPB, no bus/transceiver):\n"
"                     use same-channel pairs --pair 0:0,1:1,2:2 (default off)\n"
"    --no-setup     do not touch channel configuration\n"
"    --csv          dump per-frame timestamps to pair-A-B.csv\n"
"\n"
"examples:\n"
"  ./canperf --count 10000             10000 frames per pair, all pairs\n"
"  ./canperf --gap-us 500              one probe every 500 us\n"
"  ./canperf --pair 0:4 --count 20000 --gap-us 200 --csv\n"
"  ./canperf --window 16 --gap-us 125 --count 100000   8000 fps load test\n"
"  ./canperf --performance --pair 0:4 --gap-us 1   one-way TX flood, FPS+loss\n"
"  ./canperf --performance --pair 0:4,1:2,3:5      3 pairs concurrently\n"
"  ./canperf --loopback --pair 0:0,1:1,2:2         controller loopback (no bus)\n"
"  ./canperf --sweep                   find the max sustainable rate\n"
"  ./canperf --sweep --bidir --size 8  classical frames, full duplex\n"
"  ./canperf --bitrate 500k --dbitrate 2M\n"
"  ./canperf --duration 3600 --report-s 60     one-hour soak\n"
"\n"
"timestamps: T0 app send / T1 bus TX complete / T2 remote MCU capture /\n"
"            T3 Linux net stack / T4 app recv\n"
"segments:   A=T1-T0 tx path  B=T2-T1 mcu capture  C=T3-T2 uplink\n"
"            D=T4-T3 rx wakeup; total and B/D carry no clock-mapping error\n"
"            (the full path diagram is printed at the start of every run)\n");
}

int main(int argc, char **argv)
{
	static const struct option long_opts[] = {
		{ "pair",     1, 0, 'p' }, { "count",    1, 0, 'c' },
		{ "duration", 1, 0, 'D' }, { "gap-us",   1, 0, 'g' },
		{ "size",     1, 0, 's' }, { "report-s", 1, 0, 'r' },
		{ "bitrate",  1, 0, 'b' }, { "dbitrate", 1, 0, 'B' },
		{ "window",   1, 0, 'w' }, { "bidir",    0, 0, 'i' },
		{ "sweep",    0, 0, 'S' }, { "sweep-frames", 1, 0, 'F' },
		{ "p99-limit", 1, 0, 'L' }, { "performance", 0, 0, 'P' },
		{ "loopback", 0, 0, 'l' },
		{ "no-setup", 0, 0, 'n' }, { "csv",      0, 0, 'v' },
		{ "help",     0, 0, 'h' }, { 0, 0, 0, 0 },
	};
	struct opts opts = {
		.count = 5000, .gap_us = 1000, .size = 64, .report_s = 10,
		.window = 1, .sweep_frames = 20000, .p99_limit_us = 1000,
		.setup = true, .bitrate = 1000000, .dbitrate = 8000000,
	};
	struct cnt_snap snap0, snap1;
	static struct pair_ctx pairs[MAX_PAIRS];
	int npairs = 0;
	int c, ret = 0;
	bool count_set = false;

	while ((c = getopt_long(argc, argv, "h", long_opts, NULL)) != -1) {
		switch (c) {
		case 'p': {
			/* one or more comma-separated pairs: "x:y" or "x:y,a:b,..."
			 * (repeating --pair also appends). */
			char buf[128];
			char *save = NULL, *tok;

			strncpy(buf, optarg, sizeof(buf) - 1);
			buf[sizeof(buf) - 1] = '\0';
			for (tok = strtok_r(buf, ",", &save); tok;
			     tok = strtok_r(NULL, ",", &save)) {
				int a, b;

				if (sscanf(tok, "%d:%d", &a, &b) != 2 ||
				    a < 0 || a > 5 || b < 0 || b > 5) {
					usage();
					return 1;
				}
				if (npairs < MAX_PAIRS) {
					pairs[npairs].tx_ch = a;
					pairs[npairs].rx_ch = b;
					npairs++;
				}
			}
			break;
		}
		case 'c': opts.count = atol(optarg); count_set = true; break;
		case 'D':
			opts.duration_s = parse_duration(optarg);
			if (opts.duration_s < 0) {
				fprintf(stderr,
					"error: bad --duration '%s' (use e.g. 100s, 2m, 1h)\n",
					optarg);
				return 1;
			}
			break;
		case 'g': opts.gap_us = atoi(optarg); break;
		case 's': opts.size = atoi(optarg); break;
		case 'r': opts.report_s = atoi(optarg) > 0 ? atoi(optarg) : 10; break;
		case 'b': opts.bitrate = parse_rate(optarg); break;
		case 'B': opts.dbitrate = parse_rate(optarg); break;
		case 'w': opts.window = (unsigned int)atoi(optarg); break;
		case 'i': opts.bidir = true; break;
		case 'S': opts.sweep = true; break;
		case 'F': opts.sweep_frames = atol(optarg); break;
		case 'L': opts.p99_limit_us = atoi(optarg); break;
		case 'P': opts.performance = true; break;
		case 'l': opts.loopback = true; break;
		case 'n': opts.setup = false; break;
		case 'v': opts.csv = true; break;
		case 'h': usage(); return 0;
		default: usage(); return 1;
		}
	}
	if (opts.size == 64 && !opts.dbitrate) {
		fprintf(stderr,
			"error: --dbitrate 0 (classical CAN) requires --size 8\n");
		return 1;
	}
	if (!opts.window || opts.window > MAX_WINDOW) {
		fprintf(stderr,
			"error: --window must be 1..%d (the driver's per-channel\n"
			"  TXC window is %d slots deep - more cannot be in flight)\n",
			MAX_WINDOW, MAX_WINDOW);
		return 1;
	}
	if (!opts.bitrate || opts.bitrate > 1000000 ||
	    (opts.dbitrate && opts.dbitrate > 8000000)) {
		fprintf(stderr,
"error: bitrate out of range (got %u/%u)\n"
"  gateway device limit: the CAN controllers sit on the MCXE31B, whose\n"
"  NXP device data caps CAN FD at 8 Mbit/s data phase\n"
"  (MCXE31B_features.h FSL_FEATURE_FLEXCAN_MAX_CANFD_BITRATE, enforced\n"
"  by the FLEXCAN_FDInit assert; FlexCAN PE clock is 80 MHz);\n"
"  the arbitration phase is capped at 1 Mbit/s by CAN bit-wise\n"
"  arbitration physics (applies to every CAN/CAN FD node).\n",
			opts.bitrate, opts.dbitrate);
		return 1;
	}
	{
		/* probe payload (magic+seq) needs >= 8; above 8 only the CAN
		 * FD DLC-encodable lengths exist on the wire */
		static const int fd_sizes[] = { 8, 12, 16, 20, 24, 32, 48, 64 };
		bool size_ok = false;

		for (size_t i = 0; i < sizeof(fd_sizes) / sizeof(fd_sizes[0]); i++)
			if (opts.size == fd_sizes[i])
				size_ok = true;
		if (!size_ok) {
			fprintf(stderr,
				"error: --size must be one of 8,12,16,20,24,32,48,64\n"
				"  (CAN FD DLC lengths; probe payload needs >= 8 bytes)\n");
			return 1;
		}
	}
	if (opts.sweep) {
		if (opts.window == 1)
			opts.window = MAX_WINDOW; /* sweep loads the window engine */
		if (opts.sweep_frames < 1000)
			opts.sweep_frames = 1000;
	}
	if (opts.duration_s)
		opts.count = 0; /* duration-bound run */
	if (opts.performance) {
		/* one pair, two threads; default 0:4, default 10000 frames */
		if (!count_set && !opts.duration_s)
			opts.count = 10000;
		if (!npairs) {
			pairs[0].tx_ch = 0;
			pairs[0].rx_ch = 4;
			npairs = 1;
		}
	}
	if (!npairs) {
		/* the three wired buses; --bidir mirrors them below */
		for (npairs = 0; npairs < MAX_PAIRS / 2; npairs++) {
			pairs[npairs].tx_ch = default_pairs[npairs][0];
			pairs[npairs].rx_ch = default_pairs[npairs][1];
		}
	}
	if (opts.bidir) {
		int n = npairs;

		for (int i = 0; i < n && npairs < MAX_PAIRS; i++) {
			pairs[npairs].tx_ch = pairs[i].rx_ch;
			pairs[npairs].rx_ch = pairs[i].tx_ch;
			npairs++;
		}
	}
	/* distinct TX channels keep the per-pair CAN IDs (0x100+tx) and the
	 * echo matching collision-free */
	for (int i = 0; i < npairs; i++)
		for (int k = i + 1; k < npairs; k++)
			if (pairs[i].tx_ch == pairs[k].tx_ch) {
				fprintf(stderr,
					"error: two pairs share TX channel %d\n",
					pairs[i].tx_ch);
				return 1;
			}

	/* same-channel pairs (x:x) only work in controller loopback; a wired
	 * x:y pair is broken by loopback (each channel loops to itself) */
	for (int i = 0; i < npairs; i++) {
		if (pairs[i].tx_ch == pairs[i].rx_ch && !opts.loopback) {
			fprintf(stderr,
				"error: pair %d:%d is same-channel - add --loopback "
				"(controller LPB self-test)\n",
				pairs[i].tx_ch, pairs[i].rx_ch);
			return 1;
		}
		if (pairs[i].tx_ch != pairs[i].rx_ch && opts.loopback)
			fprintf(stderr,
				"warning: --loopback with wired pair %d:%d - each "
				"channel loops to itself, they will not reach each other\n",
				pairs[i].tx_ch, pairs[i].rx_ch);
	}

	/* live in-place dashboard only when stdout is a real terminal; a pipe or
	 * file gets plain scrolling output (no ANSI escapes) */
	g_live = isatty(STDOUT_FILENO) ? true : false;

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* minimize the measurement process's own scheduling noise. perf uses a
	 * fixed 2 threads (one TX + one epoll RX over all channels), so it never
	 * oversubscribes; round-trip uses one thread per pair. Warn only if the
	 * thread count exceeds the cores (round-trip with many pairs). */
	mlockall(MCL_CURRENT | MCL_FUTURE);
	{
		long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
		int threads = opts.performance ? 2 : npairs;
		struct sched_param sp = { .sched_priority = 50 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			fprintf(stderr,
				"note: SCHED_FIFO unavailable, measuring at normal priority\n");
		if (ncpu > 0 && threads > (int)ncpu)
			fprintf(stderr,
				"warning: %d threads > %ld cores - measurement may be CPU-starved\n",
				threads, ncpu);
	}

	if (opts.setup) {
		int nlfd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
				  NETLINK_ROUTE);

		if (nlfd < 0) {
			perror("netlink");
			return 1;
		}
		printf("configuring channels (%u/%u%s%s) ...\n", opts.bitrate,
		       opts.dbitrate, opts.dbitrate ? " FD" : " classical",
		       opts.loopback ? " loopback" : "");
		for (int i = 0; i < npairs; i++)
			for (int k = 0; k < 2; k++) {
				int ch = k ? pairs[i].rx_ch : pairs[i].tx_ch;

				if (chan_setup(nlfd, ch, opts.bitrate,
					       opts.dbitrate, opts.loopback))
					fprintf(stderr,
						"warning: eth2can%d setup failed (gateway alive?)\n",
						ch);
			}
		close(nlfd);
		/* wait for the gateway heartbeat and the first TIME anchor
		 * (the hardware timestamps depend on it) */
		printf("waiting for the gateway time anchor (TIME, 1 Hz) ...\n");
		sleep(2);
	}

	if (opts.performance) {
		counters_snapshot(&snap0);
		ret = perf_run(&opts, pairs, npairs);
		counters_snapshot(&snap1);
		counters_delta(&snap0, &snap1);
		return stop_flag ? 130 : ret;
	}

	print_banner(&opts, npairs);
	counters_snapshot(&snap0);

	if (opts.sweep) {
		ret = sweep_run(&opts, pairs, npairs);
		counters_snapshot(&snap1);
		counters_delta(&snap0, &snap1);
		return stop_flag ? 130 : ret;
	}

	if (run_pairs(&opts, pairs, npairs, false))
		return 1;

	for (int i = 0; i < npairs; i++) {
		pair_report(&pairs[i]);
		if (pairs[i].lost)
			ret = 1;
	}
	summary_table(pairs, npairs);
	counters_snapshot(&snap1);
	counters_delta(&snap0, &snap1);

	return stop_flag ? 130 : ret;
}
