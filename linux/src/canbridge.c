/*
 * canbridge.c - userspace UDP <-> SocketCAN demux bridge for the MCXE31B
 *               "SCGW v3" Ethernet-to-6xCAN-FD gateway.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The board multiplexes all six CAN channels onto ONE UDP stream (port 50000),
 * tagging each frame with a per-frame channel byte. This daemon demuxes that one
 * stream into six SocketCAN interfaces (created as vcan, default vcan-gw0..vcan-gw5):
 *   - downlink UDP -> iface[channel]  (board RX frames appear on candump vcan-gw0..5)
 *   - uplink   can -> UDP            (cangen/app frames are forwarded to the board)
 *
 * Single epoll thread, libc only, no kernel module. Built on the target with
 * gcc + make. See README.md for the install / pressure-test workflow.
 *
 * Design notes:
 *  - recvmmsg drains the whole backlog per wakeup (no one-frame-per-select stall).
 *  - SO_RXQ_OVFL makes the daemon witness its OWN drops: vcan + CAN_RAW have no
 *    kernel drop counter, so without this a daemon drop would be silently blamed
 *    on the board during a pressure test.
 *  - startup HARD-FAILS if vcan/mtu-72/CAN_RAW_FD_FRAMES are not satisfied; it
 *    never silently downgrades CAN FD to classic.
 *  - the board unicasts RX frames to the LAST peer that sent a valid datagram, so
 *    only ONE bridge per board: we register on start and keepalive when idle.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sched.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <linux/can/error.h>

#include "can_gateway_wire.h"

/* SO_RXQ_OVFL lets the daemon witness its own kernel-queue drops. A few older
 * glibc <sys/socket.h> do not expose the constant even with _GNU_SOURCE. */
#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

#define MAX_CH CAN_GATEWAY_MAX_CHANNELS /* 6 */
#define RX_BATCH 64                     /* frames/datagrams drained per recvmmsg */
#define MAX_AGG (MAX_CH * RX_BATCH)     /* uplink frames buffered per wakeup */
#define CANFD_SZ ((int)sizeof(struct canfd_frame)) /* 72 */
#define CLASSIC_SZ ((int)sizeof(struct can_frame)) /* 16 */
#define TICK_MS 100                     /* base timer tick */

/* Wire-format guards: the daemon and firmware must agree byte-for-byte. */
_Static_assert(sizeof(can_gateway_packet_header_t) == 16, "v3 packet header must be 16 bytes");
_Static_assert(sizeof(can_gateway_frame_head_t) == 16, "v3 frame head must be 16 bytes");
_Static_assert(offsetof(can_gateway_frame_head_t, can_id) == 4, "frame head layout");
_Static_assert(offsetof(can_gateway_frame_head_t, timestamp) == 8, "frame head layout");
_Static_assert(offsetof(can_gateway_frame_head_t, status) == 12, "frame head layout");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "SCGW v3 wire format is little-endian; a big-endian host needs byte swaps"
#endif

/* ---- configuration ---- */
static struct {
    struct sockaddr_in board;        /* board IP + data port */
    uint16_t local_port;
    const char *ifaces[MAX_CH];
    int n;
    int keepalive_ms;
    int stats_ms;
    int rcvbuf;
} g_cfg = {
    .local_port = CAN_GATEWAY_UDP_DATA_PORT,
    .n = 0,
    .keepalive_ms = 200,
    .stats_ms = 1000,
    .rcvbuf = 4 * 1024 * 1024,
};

/* ---- counters ---- */
static struct {
    uint64_t udp_rx_pkts, udp_rx_frames, udp_tx_pkts, udp_tx_frames;
    uint64_t udp_seq_loss, udp_seq_reorder, udp_parse_err, udp_send_err;
    uint64_t can_rx[MAX_CH], can_tx[MAX_CH];
    uint64_t can_rx_err, can_tx_err, rxq_ovfl;
    int64_t last_seq;                /* -1 = unknown */
    uint32_t rxq_last[MAX_CH];       /* last SO_RXQ_OVFL counter per CAN socket */
    uint32_t udp_rxq_last;
} st = { .last_seq = -1 };

static int g_udp = -1;
static int g_can[MAX_CH];
static uint32_t g_tx_seq = 0;
static bool g_uplink_this_tick = false;

/* Uplink aggregation: frames collected from the CAN side this wakeup, packed into
 * <=16-frame v3 datagrams at flush time (preserving the single-stream channel mux). */
static struct {
    int count;
    uint8_t reclen[MAX_AGG];
    uint8_t rec[MAX_AGG][CAN_GATEWAY_MAX_FRAME_RECORD];
} g_tx;

union ctlbuf {
    struct cmsghdr align;
    char buf[CMSG_SPACE(sizeof(uint32_t))];
};

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        die("fcntl(O_NONBLOCK): %s", strerror(errno));
}

/* Best-effort realtime: pin the daemon at SCHED_FIFO and lock its pages so a delayed
 * wakeup never lets the CAN->UDP backlog burst onto the board in one shot. That burst is
 * the dominant cause of the board's udp->can tail latency -- the board must serialize it
 * onto the 1M/5M CAN wire (~140 us/frame). Scheduled promptly, the bridge forwards
 * frame-by-frame and the board stays near its ~90 us average. Non-fatal: without
 * privilege the bridge still runs, just at normal scheduling. */
static void try_realtime(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
        fprintf(stderr, "note: SCHED_FIFO prio %d denied (%s); normal scheduling\n",
                prio, strerror(errno));
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0)
        fprintf(stderr, "note: mlockall denied (%s); pages not locked\n", strerror(errno));
}

/* ---- socket setup (with hard-fail self-checks) ---- */
static int open_can(const char *iface, int channel)
{
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0)
        die("%s: socket(PF_CAN): %s", iface, strerror(errno));

    int on = 1;
    if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0)
        die("%s: CAN_RAW_FD_FRAMES not supported (%s) - FD frames would be dropped", iface, strerror(errno));
    int got = 0;
    socklen_t gl = sizeof(got);
    if (getsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &got, &gl) < 0 || got != 1)
        die("%s: CAN_RAW_FD_FRAMES readback != 1 - refusing to silently drop FD frames", iface);

    int off = 0; /* default; set explicitly so the no-echo contract self-documents */
    setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &off, sizeof(off));
    int rxq = 1;
    if (setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL, &rxq, sizeof(rxq)) < 0)
        die("%s: SO_RXQ_OVFL: %s - cannot witness daemon drops", iface, strerror(errno));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &g_cfg.rcvbuf, sizeof(g_cfg.rcvbuf));

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0)
        die("%s: interface not found (%s) - run scripts/setup-vcan.sh first", iface, strerror(errno));
    int ifindex = ifr.ifr_ifindex;
    if (ioctl(s, SIOCGIFMTU, &ifr) < 0)
        die("%s: SIOCGIFMTU: %s", iface, strerror(errno));
    if (ifr.ifr_mtu != CANFD_MTU)
        die("%s: mtu=%d, expected %d - run: ip link set %s mtu %d (FD frames would be silently dropped)",
            iface, ifr.ifr_mtu, CANFD_MTU, iface, CANFD_MTU);

    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("%s: bind: %s", iface, strerror(errno));

    set_nonblock(s);
    (void)channel;
    return s;
}

static int open_udp(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        die("socket(AF_INET): %s", strerror(errno));
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &g_cfg.rcvbuf, sizeof(g_cfg.rcvbuf));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(g_cfg.local_port);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0)
        die("udp bind :%u: %s", g_cfg.local_port, strerror(errno));
    set_nonblock(s);
    return s;
}

/* ---- SO_RXQ_OVFL cmsg parse ---- */
static bool get_rxq_ovfl(struct msghdr *m, uint32_t *out)
{
    for (struct cmsghdr *c = CMSG_FIRSTHDR(m); c; c = CMSG_NXTHDR(m, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
            memcpy(out, CMSG_DATA(c), sizeof(uint32_t));
            return true;
        }
    }
    return false;
}

/* ---- frame conversion (v3 wire format; authoritative source: source/can_udp_gateway.c) ---- */

/* CAN frame bytes -> v3 wire record at `out`. Returns wire byte count, 0 to skip. */
static size_t can_to_wire(uint8_t channel, const uint8_t *raw, int rlen, uint8_t *out)
{
    can_gateway_frame_head_t h;
    memset(&h, 0, sizeof(h));
    h.channel = channel;

    uint32_t cid;
    uint8_t len;
    const uint8_t *data;
    uint8_t flags = 0;

    if (rlen == CANFD_SZ) {
        const struct canfd_frame *f = (const struct canfd_frame *)raw;
        cid = f->can_id;
        len = f->len;
        data = f->data;
        flags |= CAN_GATEWAY_FLAG_FD;
        if (f->flags & CANFD_BRS) flags |= CAN_GATEWAY_FLAG_BRS;
    } else if (rlen == CLASSIC_SZ) {
        const struct can_frame *f = (const struct can_frame *)raw;
        cid = f->can_id;
        len = f->can_dlc;
        data = f->data;
    } else {
        return 0;
    }

    /* Check the error flag FIRST: an error frame is 16 bytes but is not classic
     * data; its can_id holds error-class bits, not an SFF/EFF identifier. */
    if (cid & CAN_ERR_FLAG) {
        flags |= CAN_GATEWAY_FLAG_ERROR;
        h.can_id = cid & CAN_ERR_MASK;
    } else {
        if (cid & CAN_EFF_FLAG) {
            flags |= CAN_GATEWAY_FLAG_EXTENDED_ID;
            h.can_id = cid & CAN_EFF_MASK;
        } else {
            h.can_id = cid & CAN_SFF_MASK;
        }
        if (cid & CAN_RTR_FLAG) flags |= CAN_GATEWAY_FLAG_REMOTE;
    }
    h.flags = flags;

    if (flags & CAN_GATEWAY_FLAG_FD) {
        if (len > 64) len = 64;
        h.dlc = can_gateway_len_to_dlc(len);
    } else {
        if (len > 8) len = 8;
        h.dlc = len;
    }

    uint8_t wl = can_gateway_wire_data_len(h.flags, h.dlc);
    memcpy(out, &h, CAN_GATEWAY_FRAME_HEAD_SIZE);
    memcpy(out + CAN_GATEWAY_FRAME_HEAD_SIZE, data, wl);
    return (size_t)CAN_GATEWAY_FRAME_HEAD_SIZE + wl;
}

/* v3 wire record -> CAN frame, injected on `sock` (channel `ch`). */
static void wire_to_can(int sock, const can_gateway_frame_head_t *h,
                        const uint8_t *payload, uint8_t wl, int ch)
{
    uint32_t cid;
    if (h->flags & CAN_GATEWAY_FLAG_ERROR) {
        cid = (h->can_id & CAN_ERR_MASK) | CAN_ERR_FLAG;
    } else if (h->flags & CAN_GATEWAY_FLAG_EXTENDED_ID) {
        cid = (h->can_id & CAN_EFF_MASK) | CAN_EFF_FLAG;
        if (h->flags & CAN_GATEWAY_FLAG_REMOTE) cid |= CAN_RTR_FLAG;
    } else {
        cid = h->can_id & CAN_SFF_MASK;
        if (h->flags & CAN_GATEWAY_FLAG_REMOTE) cid |= CAN_RTR_FLAG;
    }

    if (h->flags & CAN_GATEWAY_FLAG_FD) {
        struct canfd_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id = cid;
        f.len = can_gateway_dlc_to_len(h->dlc);
        if (h->flags & CAN_GATEWAY_FLAG_BRS) f.flags |= CANFD_BRS;
        uint8_t n = f.len;
        if (n > wl) n = wl;
        memcpy(f.data, payload, n);
        if (write(sock, &f, CANFD_SZ) != CANFD_SZ) st.can_tx_err++;
        else st.can_tx[ch]++;
    } else {
        struct can_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id = cid;
        uint8_t n = h->dlc;
        if (n > 8) n = 8;
        f.can_dlc = n;
        if (n > wl) n = wl;
        memcpy(f.data, payload, n);
        if (write(sock, &f, CLASSIC_SZ) != CLASSIC_SZ) st.can_tx_err++;
        else st.can_tx[ch]++;
    }
}

/* ---- uplink: CAN -> board ---- */
static void flush_board(void)
{
    int i = 0;
    while (i < g_tx.count) {
        int n = g_tx.count - i;
        if (n > (int)CAN_GATEWAY_MAX_FRAMES_PER_PACKET) n = CAN_GATEWAY_MAX_FRAMES_PER_PACKET;

        uint8_t pkt[CAN_GATEWAY_MAX_PACKET_BYTES];
        can_gateway_packet_header_t hd;
        memset(&hd, 0, sizeof(hd));
        hd.magic = (uint32_t)CAN_GATEWAY_MAGIC;
        hd.version = CAN_GATEWAY_VERSION;
        hd.packet_type = CAN_GATEWAY_PACKET_TYPE_FRAMES;
        hd.frame_count = (uint16_t)n;
        hd.sequence = g_tx_seq++;
        memcpy(pkt, &hd, CAN_GATEWAY_PACKET_HEADER_SIZE);

        size_t off = CAN_GATEWAY_PACKET_HEADER_SIZE;
        for (int k = 0; k < n; k++) {
            memcpy(pkt + off, g_tx.rec[i + k], g_tx.reclen[i + k]);
            off += g_tx.reclen[i + k];
        }
        if (sendto(g_udp, pkt, off, 0, (struct sockaddr *)&g_cfg.board, sizeof(g_cfg.board)) < 0)
            st.udp_send_err++;
        else {
            st.udp_tx_pkts++;
            st.udp_tx_frames += (uint64_t)n;
            g_uplink_this_tick = true;
        }
        i += n;
    }
    g_tx.count = 0;
}

static void drain_can(int sock, uint8_t channel)
{
    static struct mmsghdr msgs[RX_BATCH];
    static struct iovec iovs[RX_BATCH];
    static uint8_t bufs[RX_BATCH][CANFD_SZ];
    static union ctlbuf ctl[RX_BATCH];

    for (;;) {
        for (int i = 0; i < RX_BATCH; i++) {
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len = CANFD_SZ;
            memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
            msgs[i].msg_hdr.msg_iov = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_control = ctl[i].buf;
            msgs[i].msg_hdr.msg_controllen = sizeof(ctl[i].buf);
            msgs[i].msg_len = 0;
        }
        int r = recvmmsg(sock, msgs, RX_BATCH, MSG_DONTWAIT, NULL);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            st.can_rx_err++;
            break;
        }
        if (r == 0) break;

        for (int i = 0; i < r; i++) {
            uint32_t ov;
            if (get_rxq_ovfl(&msgs[i].msg_hdr, &ov)) {
                uint32_t d = ov - st.rxq_last[channel];
                if (d) st.rxq_ovfl += d; /* unsigned sub is wrap-correct; only d==0 means no change */
                st.rxq_last[channel] = ov;
            }
            if (g_tx.count >= (int)MAX_AGG) flush_board();
            size_t wb = can_to_wire(channel, bufs[i], msgs[i].msg_len, g_tx.rec[g_tx.count]);
            if (wb) {
                g_tx.reclen[g_tx.count] = (uint8_t)wb;
                g_tx.count++;
                st.can_rx[channel]++;
            } else {
                st.can_rx_err++;
            }
        }
        if (r < RX_BATCH) break;
    }
}

/* ---- downlink: board -> CAN ---- */
static void parse_datagram(const uint8_t *p, int len)
{
    if (len < (int)CAN_GATEWAY_PACKET_HEADER_SIZE) { st.udp_parse_err++; return; }
    can_gateway_packet_header_t h;
    memcpy(&h, p, CAN_GATEWAY_PACKET_HEADER_SIZE);
    if (h.magic != (uint32_t)CAN_GATEWAY_MAGIC || h.version != CAN_GATEWAY_VERSION ||
        h.packet_type != CAN_GATEWAY_PACKET_TYPE_FRAMES ||
        h.frame_count > CAN_GATEWAY_MAX_FRAMES_PER_PACKET) {
        st.udp_parse_err++;
        return;
    }

    if (st.last_seq >= 0) {
        int32_t d = (int32_t)((uint32_t)h.sequence - (uint32_t)st.last_seq);
        if (d > 1) st.udp_seq_loss += (uint32_t)(d - 1);
        else if (d <= 0) st.udp_seq_reorder++;
    }
    st.last_seq = (int64_t)(uint32_t)h.sequence;
    st.udp_rx_pkts++;

    size_t off = CAN_GATEWAY_PACKET_HEADER_SIZE;
    for (int i = 0; i < h.frame_count; i++) {
        if (off + CAN_GATEWAY_FRAME_HEAD_SIZE > (size_t)len) { st.udp_parse_err++; return; }
        can_gateway_frame_head_t fh;
        memcpy(&fh, p + off, CAN_GATEWAY_FRAME_HEAD_SIZE);
        uint8_t wl = can_gateway_wire_data_len(fh.flags, fh.dlc);
        if (off + CAN_GATEWAY_FRAME_HEAD_SIZE + wl > (size_t)len) { st.udp_parse_err++; return; }
        st.udp_rx_frames++;
        if (fh.channel < g_cfg.n)
            wire_to_can(g_can[fh.channel], &fh, p + off + CAN_GATEWAY_FRAME_HEAD_SIZE, wl, fh.channel);
        off += (size_t)CAN_GATEWAY_FRAME_HEAD_SIZE + wl;
    }
    /* Trailing bytes after the last record => malformed (matches firmware/python). */
    if (off != (size_t)len) st.udp_parse_err++;
}

static void drain_udp(void)
{
    static struct mmsghdr msgs[RX_BATCH];
    static struct iovec iovs[RX_BATCH];
    static uint8_t bufs[RX_BATCH][CAN_GATEWAY_MAX_PACKET_BYTES];
    static struct sockaddr_in from[RX_BATCH];
    static union ctlbuf ctl[RX_BATCH];

    for (;;) {
        for (int i = 0; i < RX_BATCH; i++) {
            iovs[i].iov_base = bufs[i];
            iovs[i].iov_len = sizeof(bufs[i]);
            memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
            msgs[i].msg_hdr.msg_name = &from[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(from[i]);
            msgs[i].msg_hdr.msg_iov = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen = 1;
            msgs[i].msg_hdr.msg_control = ctl[i].buf;
            msgs[i].msg_hdr.msg_controllen = sizeof(ctl[i].buf);
            msgs[i].msg_len = 0;
        }
        int r = recvmmsg(g_udp, msgs, RX_BATCH, MSG_DONTWAIT, NULL);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;

        for (int i = 0; i < r; i++) {
            uint32_t ov;
            if (get_rxq_ovfl(&msgs[i].msg_hdr, &ov)) {
                uint32_t d = ov - st.udp_rxq_last;
                if (d) st.rxq_ovfl += d; /* unsigned sub is wrap-correct; only d==0 means no change */
                st.udp_rxq_last = ov;
            }
            /* single-board model: ignore datagrams from any other source */
            if (from[i].sin_addr.s_addr != g_cfg.board.sin_addr.s_addr) continue;
            parse_datagram(bufs[i], msgs[i].msg_len);
        }
        if (r < RX_BATCH) break;
    }
}

static void send_keepalive(void)
{
    can_gateway_packet_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic = (uint32_t)CAN_GATEWAY_MAGIC;
    h.version = CAN_GATEWAY_VERSION;
    h.packet_type = CAN_GATEWAY_PACKET_TYPE_FRAMES;
    h.frame_count = 0;
    h.sequence = g_tx_seq++;
    if (sendto(g_udp, &h, sizeof(h), 0, (struct sockaddr *)&g_cfg.board, sizeof(g_cfg.board)) < 0)
        st.udp_send_err++;
    else
        st.udp_tx_pkts++;
}

static void dump_stats(const char *tag)
{
    char rx[128], tx[128];
    int rp = 0, tp = 0;
    for (int i = 0; i < g_cfg.n; i++) {
        rp += snprintf(rx + rp, sizeof(rx) - rp, "%s%llu", i ? "," : "", (unsigned long long)st.can_rx[i]);
        tp += snprintf(tx + tp, sizeof(tx) - tp, "%s%llu", i ? "," : "", (unsigned long long)st.can_tx[i]);
    }
    fprintf(stderr,
            "%s udp_rx_pkts=%llu udp_rx_frames=%llu udp_tx_pkts=%llu udp_tx_frames=%llu "
            "seq_loss=%llu seq_reorder=%llu parse_err=%llu send_err=%llu "
            "can_rx_err=%llu can_tx_err=%llu rxq_ovfl=%llu can_rx=[%s] can_tx=[%s]\n",
            tag,
            (unsigned long long)st.udp_rx_pkts, (unsigned long long)st.udp_rx_frames,
            (unsigned long long)st.udp_tx_pkts, (unsigned long long)st.udp_tx_frames,
            (unsigned long long)st.udp_seq_loss, (unsigned long long)st.udp_seq_reorder,
            (unsigned long long)st.udp_parse_err, (unsigned long long)st.udp_send_err,
            (unsigned long long)st.can_rx_err, (unsigned long long)st.can_tx_err,
            (unsigned long long)st.rxq_ovfl, rx, tx);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --board <ipv4> [options] [vcan-gw0 vcan-gw1 ...]\n"
            "  --board <ipv4>        board IP (required)\n"
            "  --data-port <n>       board UDP data port (default %u)\n"
            "  --local-port <n>      local UDP bind port (default %u)\n"
            "  --keepalive-ms <n>    idle session keepalive (default 200)\n"
            "  --stats-ms <n>        journald stats interval, 0=off (default 1000)\n"
            "  --rcvbuf <bytes>      SO_RCVBUF (default 4194304)\n"
            "  interfaces default to vcan-gw0..vcan-gw5; each maps to channel 0..5\n",
            prog, CAN_GATEWAY_UDP_DATA_PORT, CAN_GATEWAY_UDP_DATA_PORT);
}

int main(int argc, char **argv)
{
    const char *board_ip = NULL;
    uint16_t data_port = CAN_GATEWAY_UDP_DATA_PORT;

    static const struct option opts[] = {
        {"board", required_argument, 0, 'b'},
        {"data-port", required_argument, 0, 'd'},
        {"local-port", required_argument, 0, 'l'},
        {"keepalive-ms", required_argument, 0, 'k'},
        {"stats-ms", required_argument, 0, 's'},
        {"rcvbuf", required_argument, 0, 'r'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "b:d:l:k:s:r:h", opts, NULL)) != -1) {
        switch (c) {
        case 'b': board_ip = optarg; break;
        case 'd': data_port = (uint16_t)atoi(optarg); break;
        case 'l': g_cfg.local_port = (uint16_t)atoi(optarg); break;
        case 'k': g_cfg.keepalive_ms = atoi(optarg); break;
        case 's': g_cfg.stats_ms = atoi(optarg); break;
        case 'r': g_cfg.rcvbuf = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }
    if (!board_ip) { usage(argv[0]); die("--board is required"); }

    memset(&g_cfg.board, 0, sizeof(g_cfg.board));
    g_cfg.board.sin_family = AF_INET;
    g_cfg.board.sin_port = htons(data_port);
    if (inet_pton(AF_INET, board_ip, &g_cfg.board.sin_addr) != 1)
        die("invalid --board IPv4 address: %s", board_ip);

    for (int i = optind; i < argc && g_cfg.n < (int)MAX_CH; i++)
        g_cfg.ifaces[g_cfg.n++] = argv[i];
    if (g_cfg.n == 0) {
        static const char *def[MAX_CH] = {"vcan-gw0", "vcan-gw1", "vcan-gw2", "vcan-gw3", "vcan-gw4", "vcan-gw5"};
        for (int i = 0; i < (int)MAX_CH; i++) g_cfg.ifaces[i] = def[i];
        g_cfg.n = MAX_CH;
    }

    g_udp = open_udp();
    for (int i = 0; i < g_cfg.n; i++)
        g_can[i] = open_can(g_cfg.ifaces[i], i);

    /* Raise scheduling before the event loop so the very first burst is handled RT. */
    try_realtime(50);

    /* signals via signalfd, folded into epoll */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
        die("sigprocmask: %s", strerror(errno));
    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) die("signalfd: %s", strerror(errno));

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) die("timerfd: %s", strerror(errno));
    struct itimerspec its = {
        .it_interval = {.tv_sec = 0, .tv_nsec = TICK_MS * 1000000L},
        .it_value = {.tv_sec = 0, .tv_nsec = TICK_MS * 1000000L},
    };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) die("timerfd_settime: %s", strerror(errno));

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) die("epoll_create1: %s", strerror(errno));
    struct epoll_event ev;
#define ADD(fd, dat)                                  \
    do {                                              \
        ev.events = EPOLLIN;                          \
        ev.data.u32 = (dat);                          \
        if (epoll_ctl(ep, EPOLL_CTL_ADD, (fd), &ev) < 0) die("epoll_ctl: %s", strerror(errno)); \
    } while (0)
    ADD(g_udp, 1000);
    ADD(sig_fd, 1001);
    ADD(tfd, 1002);
    for (int i = 0; i < g_cfg.n; i++) ADD(g_can[i], (uint32_t)i);
#undef ADD

    send_keepalive(); /* register the session with the board */
    fprintf(stderr, "mcxe31b-canbridge: board=%s:%u, %d ifaces ready (channels 0..%d)\n",
            board_ip, data_port, g_cfg.n, g_cfg.n - 1);

    int ka_acc = 0, st_acc = 0, start = 0;
    bool running = true;
    struct epoll_event events[MAX_CH + 3];

    while (running) {
        int ne = epoll_wait(ep, events, MAX_CH + 3, -1);
        if (ne < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait: %s", strerror(errno));
        }
        bool can_ready[MAX_CH] = {false};
        bool udp_ready = false, timer_ready = false, sig_ready = false;
        for (int i = 0; i < ne; i++) {
            uint32_t d = events[i].data.u32;
            if (d < MAX_CH) can_ready[d] = true;
            else if (d == 1000) udp_ready = true;
            else if (d == 1001) sig_ready = true;
            else if (d == 1002) timer_ready = true;
        }

        /* uplink first, rotating the channel start index for fairness */
        for (int j = 0; j < g_cfg.n; j++) {
            int ch = (start + j) % g_cfg.n;
            if (can_ready[ch]) drain_can(g_can[ch], (uint8_t)ch);
        }
        start = (start + 1) % g_cfg.n;
        if (g_tx.count) flush_board();

        if (udp_ready) drain_udp();

        if (timer_ready) {
            uint64_t exp, ticks = 0;
            /* read() returns the number of expirations coalesced since last read; a busy
             * wakeup can skip several ticks, so advance the clocks by all of them. */
            while (read(tfd, &exp, sizeof(exp)) == (ssize_t)sizeof(exp)) ticks += exp;
            ka_acc += (int)(ticks * TICK_MS);
            st_acc += (int)(ticks * TICK_MS);
            if (ka_acc >= g_cfg.keepalive_ms) {
                if (!g_uplink_this_tick) send_keepalive();
                g_uplink_this_tick = false;
                ka_acc = 0;
            }
            if (g_cfg.stats_ms > 0 && st_acc >= g_cfg.stats_ms) {
                dump_stats("stats");
                st_acc = 0;
            }
        }

        if (sig_ready) {
            struct signalfd_siginfo si;
            while (read(sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                if (si.ssi_signo == SIGUSR1) {
                    dump_stats("dump");
                } else if (si.ssi_signo == SIGHUP) {
                    /* Preserve the SO_RXQ_OVFL baselines: they track the kernel's
                     * absolute (monotonic) per-socket drop counter, which we do NOT
                     * reset. Zeroing them would make the next cmsg add the whole
                     * lifetime drop count as a bogus rxq_ovfl spike. */
                    uint32_t saved_rxq[MAX_CH];
                    uint32_t saved_udp_rxq = st.udp_rxq_last;
                    memcpy(saved_rxq, st.rxq_last, sizeof(saved_rxq));
                    memset(&st, 0, sizeof(st));
                    memcpy(st.rxq_last, saved_rxq, sizeof(st.rxq_last));
                    st.udp_rxq_last = saved_udp_rxq;
                    st.last_seq = -1;
                    send_keepalive();
                    fprintf(stderr, "counters reset, session re-registered\n");
                } else {
                    running = false;
                }
            }
        }
    }

    dump_stats("final");
    for (int i = 0; i < g_cfg.n; i++) close(g_can[i]);
    close(g_udp);
    return 0;
}
