/*
 * canmesh.c - shared-bus mesh pressure + latency/loss test (C, libc only).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * C + epoll + recvmmsg + a dedicated RX thread so the TEST TOOL is not the
 * bottleneck: it receives hundreds of thousands of frames/second, so any loss
 * it reports is the PRODUCT's (bridge/board), not the tool's. SO_RXQ_OVFL lets it
 * tell its own RX overflow (verdict TOOL-LIMITED) apart from real product loss.
 *
 * Prints ONE self-contained result line per run (so several pairs writing to the
 * same terminal stay readable) and exits 0=PASS, 2=FAIL, 3=TOOL-LIMITED.
 *
 * Topology: can0..canN all bridged to the board, board's N CAN channels on ONE bus.
 * A frame sent on can_i appears on every other can_j. Each socket is TX+RX with
 * CAN_RAW_RECV_OWN_MSGS=0, so it never sees its own sends (the local vcan echo) -
 * only the board's cross-bus copies. Each sent frame is expected exactly N-1 times.
 *
 *   canmesh --ifaces can0 can1 can2 can3 can4 can5 --rate 8000 --duration 10 --len 16
 *
 * SO_RXQ_OVFL still self-witnesses: tool_rx_overflow>0 means the tool dropped (lower
 * --rate); loss with tool_rx_overflow==0 is real product loss.
 *
 * --ping is a second, ping-style mode on a single pair: send ONE frame, wait for the
 * board to return it, print that round-trip, repeat after --interval (Ctrl-C to stop).
 * One frame in flight at a time, so the RTT it prints is real latency, never queueing.
 *   canmesh can0 can1 --ping --interval 1
 *
 * --loopback drops the bus wiring entirely: the board puts each FlexCAN in internal
 * loopback (self-reception), so a frame sent on can_i returns on can_i. Each iface then
 * self-loops and the round-trip is Pi->board->CAN-loopback->board->Pi. In mesh mode each
 * frame is expected exactly ONCE (on its own channel); in --ping mode every iface is
 * pinged at once (the 6-channels-simultaneously latency test). Enable board loopback via
 * canbridge_ctl set_can_config loopback=true first (the latency.sh/stress.sh scripts do).
 *   canmesh can0 can1 can2 can3 can4 can5 --ping --loopback
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <poll.h>
#include <sched.h>
#include <signal.h>

#include <net/if.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

#define MAX_CH 6
#define RX_BATCH 128
#define MAGIC 0x4D43u                 /* "MC" */
#define CANFD_SZ ((int)sizeof(struct canfd_frame)) /* 72 */
#define HDR_SZ 15

#pragma pack(push, 1)
struct mesh_hdr {
    uint16_t magic;
    uint32_t seq;
    uint8_t src;
    uint64_t send_ns;
};
#pragma pack(pop)

static int g_sock[MAX_CH];
static int g_n;
static volatile int g_stop;

/* receiver-owned stats (main reads after pthread_join provides the barrier) */
static uint64_t st_rx, st_foreign, st_tool_ovfl;
static uint64_t lat_count, lat_sum, lat_max;
static uint32_t rxq_last[MAX_CH];

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void die(const char *m)
{
    perror(m);
    exit(1);
}

/* Ctrl-C in ping mode: stop the loop and let it print the summary (like ping). */
static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Best-effort realtime so the TEST TOOL's own scheduling jitter does not inflate the
 * measured roundtrip: the RX thread stamps arrival time, so a late wakeup would be
 * misread as product latency. Non-fatal if the kernel denies it. Threads created after
 * this inherit the policy (PTHREAD_INHERIT_SCHED), so the RX thread is realtime too. */
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

static int open_can(const char *iface)
{
    int s = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (s < 0) die("socket(PF_CAN)");
    int on = 1, off = 0;
    if (setsockopt(s, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &on, sizeof(on)) < 0) {
        fprintf(stderr, "%s: CAN_RAW_FD_FRAMES failed (%s); is mtu 72 set?\n", iface, strerror(errno));
        exit(1);
    }
    setsockopt(s, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &off, sizeof(off));
    setsockopt(s, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
    int rbuf = 16 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rbuf, sizeof(rbuf));
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "%s: not found (%s)\n", iface, strerror(errno));
        exit(1);
    }
    struct sockaddr_can addr;
    memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    return s;
}

static void *rx_thread(void *arg)
{
    (void)arg;
    int ep = epoll_create1(0);
    if (ep < 0) die("epoll_create1");
    struct epoll_event ev, evs[MAX_CH];
    for (int i = 0; i < g_n; i++) {
        ev.events = EPOLLIN;
        ev.data.u32 = (uint32_t)i;
        epoll_ctl(ep, EPOLL_CTL_ADD, g_sock[i], &ev);
    }
    static struct mmsghdr msgs[RX_BATCH];
    static struct iovec iov[RX_BATCH];
    static uint8_t buf[RX_BATCH][CANFD_SZ];
    static union { struct cmsghdr a; char b[CMSG_SPACE(sizeof(uint32_t))]; } ctl[RX_BATCH];

    while (!g_stop) {
        int ne = epoll_wait(ep, evs, g_n, 200);
        for (int e = 0; e < ne; e++) {
            int ch = (int)evs[e].data.u32;
            int s = g_sock[ch];
            for (;;) {
                for (int i = 0; i < RX_BATCH; i++) {
                    iov[i].iov_base = buf[i];
                    iov[i].iov_len = CANFD_SZ;
                    memset(&msgs[i].msg_hdr, 0, sizeof(msgs[i].msg_hdr));
                    msgs[i].msg_hdr.msg_iov = &iov[i];
                    msgs[i].msg_hdr.msg_iovlen = 1;
                    msgs[i].msg_hdr.msg_control = ctl[i].b;
                    msgs[i].msg_hdr.msg_controllen = sizeof(ctl[i].b);
                    msgs[i].msg_len = 0;
                }
                int r = recvmmsg(s, msgs, RX_BATCH, MSG_DONTWAIT, NULL);
                if (r <= 0) break;
                uint64_t t = now_ns();
                for (int i = 0; i < r; i++) {
                    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msgs[i].msg_hdr); c;
                         c = CMSG_NXTHDR(&msgs[i].msg_hdr, c)) {
                        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
                            uint32_t ov;
                            memcpy(&ov, CMSG_DATA(c), sizeof(ov));
                            uint32_t d = ov - rxq_last[ch];
                            if (d) st_tool_ovfl += d;
                            rxq_last[ch] = ov;
                        }
                    }
                    if (msgs[i].msg_len != (unsigned)CANFD_SZ) continue;
                    struct canfd_frame *f = (struct canfd_frame *)buf[i];
                    if (f->len < HDR_SZ) continue;
                    struct mesh_hdr h;
                    memcpy(&h, f->data, HDR_SZ);
                    if (h.magic != MAGIC) { st_foreign++; continue; }
                    st_rx++;
                    if (h.send_ns && t > h.send_ns) {
                        uint64_t lat = t - h.send_ns;
                        lat_count++;
                        lat_sum += lat;
                        if (lat > lat_max) lat_max = lat;
                    }
                }
                if (r < RX_BATCH) break;
            }
        }
    }
    close(ep);
    return NULL;
}

/* Block on socket s until a frame with our magic and the wanted seq arrives, or the
 * timeout expires. Returns the round-trip latency in ms (>=0), or -1 on timeout. The
 * send time travels inside the frame (mesh_hdr.send_ns), so RTT = arrival - send_ns. */
static double wait_echo(int s, uint32_t want_seq, double timeout_s)
{
    uint64_t deadline = now_ns() + (uint64_t)(timeout_s * 1e9);
    struct pollfd pfd = {.fd = s, .events = POLLIN};

    while (!g_stop) {
        uint64_t now = now_ns();
        if (now >= deadline) return -1.0;
        int ms = (int)((deadline - now) / 1000000ULL);
        int pr = poll(&pfd, 1, ms > 0 ? ms : 1);
        if (pr <= 0) {
            if (pr < 0 && errno == EINTR) continue; /* SIGINT -> g_stop checked by loop */
            return -1.0;
        }
        struct canfd_frame f;
        if (read(s, &f, CANFD_SZ) != CANFD_SZ || f.len < HDR_SZ) continue;
        struct mesh_hdr h;
        memcpy(&h, f.data, HDR_SZ);
        if (h.magic != MAGIC || h.seq != want_seq) continue; /* skip foreign/stale frames */
        uint64_t t = now_ns();
        return (h.send_ns && t > h.send_ns) ? (double)(t - h.send_ns) / 1e6 : 0.0;
    }
    return -1.0;
}

/* ping-style latency: send ONE frame on 'a', wait for the board to bring it back on 'b',
 * print that single round-trip, then repeat after 'interval'. One frame in flight at a
 * time, so the printed RTT is the real forwarding latency (Pi->board->bus->board->Pi),
 * never queueing. Prints a min/avg/max summary on exit (Ctrl-C or after 'count' pings). */
static int run_ping(const char *a, const char *b, int length, int brs, unsigned base_id,
                    long count, double interval)
{
    int sa = open_can(a);
    int sb = open_can(b);
    double timeout = interval > 1.0 ? interval : 1.0; /* wait at least 1s for a reply */

    printf("PING %s -> %s (board round-trip), %dB FD, interval %.2gs\n", a, b, length, interval);
    fflush(stdout);

    long tx = 0, rx = 0;
    double rmin = 0.0, rmax = 0.0, rsum = 0.0;
    for (long seq = 0; (count <= 0 || seq < count) && !g_stop; seq++) {
        struct timespec t_send;
        clock_gettime(CLOCK_MONOTONIC, &t_send);

        struct canfd_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id = base_id;
        f.len = (uint8_t)length;
        f.flags = brs ? CANFD_BRS : 0;
        struct mesh_hdr h = {MAGIC, (uint32_t)seq, 0, now_ns()};
        memcpy(f.data, &h, HDR_SZ);
        if (write(sa, &f, CANFD_SZ) != CANFD_SZ) {
            fprintf(stderr, "seq=%ld: write failed (%s)\n", seq, strerror(errno));
            break;
        }
        tx++;

        double rtt = wait_echo(sb, (uint32_t)seq, timeout);
        if (rtt >= 0.0) {
            rx++;
            if (rx == 1 || rtt < rmin) rmin = rtt;
            if (rtt > rmax) rmax = rtt;
            rsum += rtt;
            printf("seq=%-4ld rtt=%.3f ms\n", seq, rtt);
        } else {
            printf("seq=%-4ld timeout\n", seq);
        }
        fflush(stdout);

        if (interval > 0.0 && !g_stop && (count <= 0 || seq + 1 < count)) {
            uint64_t add = (uint64_t)(interval * 1e9);
            t_send.tv_nsec += (long)(add % 1000000000ULL);
            t_send.tv_sec += (long)(add / 1000000000ULL);
            while (t_send.tv_nsec >= 1000000000L) { t_send.tv_nsec -= 1000000000L; t_send.tv_sec++; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t_send, NULL); /* send-to-send cadence */
        }
    }

    double loss = tx ? 100.0 * (double)(tx - rx) / (double)tx : 0.0;
    printf("\n--- %s -> %s ping statistics ---\n", a, b);
    printf("%ld transmitted, %ld received, %.0f%% loss\n", tx, rx, loss);
    if (rx) printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n", rmin, rsum / (double)rx, rmax);
    fflush(stdout);

    close(sa);
    close(sb);
    return rx == tx ? 0 : 2;
}

/* loopback ping: each round, fire ONE frame on EVERY iface "simultaneously"; the board
 * loops each frame back on-chip (FlexCAN self-reception) and returns it on the SAME
 * channel, so the round-trip is Pi->board->CAN-loopback->board->Pi - no bus wiring. One
 * socket per iface with CAN_RAW_RECV_OWN_MSGS=0 means we never see our own send (the vcan
 * local echo), only the genuine board return. Prints one line per round (per-channel RTT
 * + the slowest of the round) and a per-channel min/avg/max summary on exit. */
static int run_ping_loopback(const char **ifaces, int n, int length, int brs,
                             unsigned base_id, long count, double interval)
{
    int s[MAX_CH];
    for (int i = 0; i < n; i++) s[i] = open_can(ifaces[i]);
    double timeout = interval > 1.0 ? interval : 1.0; /* wait at least 1s for replies */

    printf("PING loopback x%d (Pi->board->CAN-loopback->board->Pi), %dB FD, interval %.2gs\n",
           n, length, interval);
    fflush(stdout);

    long tx = 0, rx = 0;
    long ch_rx[MAX_CH] = {0};
    double ch_min[MAX_CH] = {0}, ch_max[MAX_CH] = {0}, ch_sum[MAX_CH] = {0};

    for (long seq = 0; (count <= 0 || seq < count) && !g_stop; seq++) {
        struct timespec t_send;
        clock_gettime(CLOCK_MONOTONIC, &t_send);

        /* fire one frame on every channel, stamping each just before its own write */
        for (int i = 0; i < n; i++) {
            struct canfd_frame f;
            memset(&f, 0, sizeof(f));
            f.can_id = base_id + (unsigned)i;
            f.len = (uint8_t)length;
            f.flags = brs ? CANFD_BRS : 0;
            struct mesh_hdr h = {MAGIC, (uint32_t)seq, (uint8_t)i, now_ns()};
            memcpy(f.data, &h, HDR_SZ);
            if (write(s[i], &f, CANFD_SZ) == CANFD_SZ) tx++;
        }

        /* collect all N returns (each on its own socket) until done or timeout */
        double rtt[MAX_CH];
        int got[MAX_CH];
        for (int i = 0; i < n; i++) { rtt[i] = -1.0; got[i] = 0; }
        int remaining = n;
        uint64_t deadline = now_ns() + (uint64_t)(timeout * 1e9);
        while (remaining > 0 && !g_stop) {
            uint64_t now = now_ns();
            if (now >= deadline) break;
            int ms = (int)((deadline - now) / 1000000ULL);
            struct pollfd pfd[MAX_CH];
            for (int i = 0; i < n; i++) { pfd[i].fd = s[i]; pfd[i].events = POLLIN; pfd[i].revents = 0; }
            int pr = poll(pfd, (nfds_t)n, ms > 0 ? ms : 1);
            if (pr < 0) { if (errno == EINTR) continue; break; }
            if (pr == 0) continue; /* deadline re-checked at loop top */
            for (int i = 0; i < n; i++) {
                if (got[i] || !(pfd[i].revents & POLLIN)) continue;
                struct canfd_frame f;
                if (read(s[i], &f, CANFD_SZ) != CANFD_SZ || f.len < HDR_SZ) continue;
                struct mesh_hdr h;
                memcpy(&h, f.data, HDR_SZ);
                if (h.magic != MAGIC || h.seq != (uint32_t)seq) continue; /* stale/foreign */
                uint64_t t = now_ns();
                rtt[i] = (h.send_ns && t > h.send_ns) ? (double)(t - h.send_ns) / 1e6 : 0.0;
                got[i] = 1;
                remaining--;
            }
        }

        /* one line per round: per-channel RTT + the slowest channel of the round */
        char line[256];
        int p = 0;
        double rmax = -1.0;
        p += snprintf(line + p, sizeof(line) - (size_t)p, "seq=%-4ld", seq);
        for (int i = 0; i < n; i++) {
            if (got[i]) {
                p += snprintf(line + p, sizeof(line) - (size_t)p, " c%d=%.3f", i, rtt[i]);
                rx++;
                ch_rx[i]++;
                if (ch_rx[i] == 1 || rtt[i] < ch_min[i]) ch_min[i] = rtt[i];
                if (rtt[i] > ch_max[i]) ch_max[i] = rtt[i];
                ch_sum[i] += rtt[i];
                if (rtt[i] > rmax) rmax = rtt[i];
            } else {
                p += snprintf(line + p, sizeof(line) - (size_t)p, " c%d=--", i);
            }
        }
        if (rmax >= 0.0) snprintf(line + p, sizeof(line) - (size_t)p, "  max=%.3f ms", rmax);
        else snprintf(line + p, sizeof(line) - (size_t)p, "  (all timeout)");
        printf("%s\n", line);
        fflush(stdout);

        if (interval > 0.0 && !g_stop && (count <= 0 || seq + 1 < count)) {
            uint64_t add = (uint64_t)(interval * 1e9);
            t_send.tv_nsec += (long)(add % 1000000000ULL);
            t_send.tv_sec += (long)(add / 1000000000ULL);
            while (t_send.tv_nsec >= 1000000000L) { t_send.tv_nsec -= 1000000000L; t_send.tv_sec++; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t_send, NULL); /* send-to-send cadence */
        }
    }

    double loss = tx ? 100.0 * (double)(tx - rx) / (double)tx : 0.0;
    printf("\n--- loopback ping statistics (%d channels) ---\n", n);
    printf("%ld transmitted, %ld received, %.0f%% loss\n", tx, rx, loss);
    for (int i = 0; i < n; i++) {
        if (ch_rx[i])
            printf("  c%d rtt min/avg/max = %.3f/%.3f/%.3f ms\n",
                   i, ch_min[i], ch_sum[i] / (double)ch_rx[i], ch_max[i]);
        else
            printf("  c%d no replies - is board loopback enabled on this channel?\n", i);
    }
    fflush(stdout);

    for (int i = 0; i < n; i++) close(s[i]);
    return rx == tx ? 0 : 2;
}

int main(int argc, char **argv)
{
    const char *ifaces[MAX_CH];
    int n = 0;
    double rate = 4000.0, duration = 10.0;
    int length = 16, brs = 1;
    unsigned base_id = 0x100, seed = 1;
    int ping = 0;             /* ping mode: one round-trip at a time, ping-style output */
    int loopback = 0;         /* board-loopback mode: each channel self-loops, no wiring */
    long ping_count = 0;      /* 0 = until Ctrl-C */
    double ping_interval = 1.0;

    static const struct option o[] = {
        {"rate", required_argument, 0, 'r'}, {"duration", required_argument, 0, 'd'},
        {"len", required_argument, 0, 'l'}, {"base-id", required_argument, 0, 'b'},
        {"no-brs", no_argument, 0, 'B'}, {"seed", required_argument, 0, 's'},
        {"ifaces", no_argument, 0, 'i'}, {"ping", no_argument, 0, 'p'},
        {"loopback", no_argument, 0, 'L'},
        {"count", required_argument, 0, 'c'}, {"interval", required_argument, 0, 'I'},
        {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "r:d:l:b:Bs:ipLc:I:h", o, NULL)) != -1) {
        switch (c) {
        case 'r': rate = atof(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'l': length = atoi(optarg); break;
        case 'b': base_id = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'B': brs = 0; break;
        case 's': seed = (unsigned)atoi(optarg); break;
        case 'i': break; /* accept --ifaces, names follow as positional args */
        case 'p': ping = 1; break;
        case 'L': loopback = 1; break;
        case 'c': ping_count = atol(optarg); break;
        case 'I': ping_interval = atof(optarg); break;
        case 'h':
        default:
            fprintf(stderr,
                    "usage: %s [--ifaces] can0 can1 ... [--rate fps] [--duration s]\n"
                    "       [--len 15..64] [--base-id 0x100] [--no-brs] [--seed N] [--loopback]\n"
                    "  ping (wired pair): %s can0 can1 --ping [--interval s] [--count N]\n"
                    "  ping (loopback)  : %s can0..can5 --ping --loopback   (each channel self-loops)\n",
                    argv[0], argv[0], argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }
    for (int i = optind; i < argc && n < MAX_CH; i++) ifaces[n++] = argv[i];
    if (n == 0) {
        static const char *def[MAX_CH] = {"can0", "can1", "can2", "can3", "can4", "can5"};
        for (int i = 0; i < MAX_CH; i++) ifaces[i] = def[i];
        n = MAX_CH;
    }
    if (!loopback && n < 2) { fprintf(stderr, "need >= 2 interfaces on the shared bus (or use --loopback)\n"); return 1; }
    if (length < HDR_SZ) length = HDR_SZ;
    if (length > 64) length = 64;

    /* ping mode: synchronous send/wait, one frame in flight (per channel). */
    if (ping) {
        try_realtime(50);
        signal(SIGINT, on_sigint);
        if (loopback)
            return run_ping_loopback(ifaces, n, length, brs, base_id, ping_count, ping_interval);
        return run_ping(ifaces[0], ifaces[1], length, brs, base_id, ping_count, ping_interval);
    }

    g_n = n;
    for (int i = 0; i < n; i++) g_sock[i] = open_can(ifaces[i]);

    /* Realtime BEFORE creating the RX thread so it inherits the policy. */
    try_realtime(50);

    pthread_t rxt;
    if (pthread_create(&rxt, NULL, rx_thread, NULL) != 0) die("pthread_create");

    uint64_t total = (uint64_t)(rate * duration);
    uint64_t period_ns = rate > 0 ? (uint64_t)(1e9 / rate) : 0;
    uint64_t sent_ch[MAX_CH] = {0};
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    for (uint64_t i = 0; i < total && !g_stop; i++) {
        int ch = (int)(rand_r(&seed) % (unsigned)n);
        struct canfd_frame f;
        memset(&f, 0, sizeof(f));
        f.can_id = base_id + (unsigned)ch;
        f.len = (uint8_t)length;
        f.flags = brs ? CANFD_BRS : 0;
        struct mesh_hdr h = {MAGIC, (uint32_t)i, (uint8_t)ch, now_ns()};
        memcpy(f.data, &h, HDR_SZ);
        if (write(g_sock[ch], &f, CANFD_SZ) == CANFD_SZ) sent_ch[ch]++;
        if (period_ns) {
            next.tv_nsec += (long)period_ns;
            while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        }
    }

    uint64_t sent = 0;
    for (int i = 0; i < n; i++) sent += sent_ch[i];
    usleep(500000); /* let in-flight frames arrive */
    g_stop = 1;
    pthread_join(rxt, NULL);

    /* loopback: each frame returns once on its OWN channel (self-loop); shared-bus mesh:
     * each frame appears once on every other channel, so sent*(n-1). */
    uint64_t expected = loopback ? sent : sent * (uint64_t)(n - 1);
    uint64_t rx = st_rx;
    long long loss = (long long)expected - (long long)rx;
    double loss_rate = expected ? (double)loss / (double)expected * 100.0 : 0.0;

    char label[64];
    if (loopback)
        snprintf(label, sizeof(label), "%s..%s(%dch loopback)", ifaces[0], ifaces[n - 1], n);
    else if (n == 2)
        snprintf(label, sizeof(label), "%s<->%s", ifaces[0], ifaces[1]);
    else
        snprintf(label, sizeof(label), "%s..%s(%dch mesh)", ifaces[0], ifaces[n - 1], n);

    /* One self-contained line per run. Several canmesh write to the same terminal
     * at once (one per bus pair), so compose the whole result into one buffer and
     * emit it with a single sub-PIPE_BUF write to keep each pair's output atomic. */
    char out[512];
    int p = 0;
    p += snprintf(out + p, sizeof(out) - (size_t)p, "%s: %llu rx, %lld lost (%.2f%%), ",
                  label, (unsigned long long)rx, loss, loss_rate);
    if (lat_count)
        p += snprintf(out + p, sizeof(out) - (size_t)p, "latency avg=%.2fms max=%.2fms",
                      (double)lat_sum / (double)lat_count / 1e6, (double)lat_max / 1e6);
    else
        p += snprintf(out + p, sizeof(out) - (size_t)p, "latency n/a");

    int rc;
    if (loss <= 0) {
        p += snprintf(out + p, sizeof(out) - (size_t)p, "  -> PASS\n");
        rc = 0;
    } else if (st_tool_ovfl >= (uint64_t)loss) {
        p += snprintf(out + p, sizeof(out) - (size_t)p,
                      "  -> TOOL-LIMITED (Pi/tool overflow=%llu, lower RATE)\n",
                      (unsigned long long)st_tool_ovfl);
        rc = 3;
    } else {
        p += snprintf(out + p, sizeof(out) - (size_t)p, "  -> FAIL\n");
        p += snprintf(out + p, sizeof(out) - (size_t)p, "    %s\n",
                      rx == 0 ? (loopback ? "no frames returned - is board loopback enabled on these channels?"
                                          : "no frames returned - check bus wiring & 120R x2=60R termination")
                              : "real product loss - see board counters below");
        rc = 2;
    }
    if (st_foreign)
        p += snprintf(out + p, sizeof(out) - (size_t)p,
                      "    note: foreign=%llu (unexpected frames on the bus)\n",
                      (unsigned long long)st_foreign);
    fwrite(out, 1, (size_t)p, stdout);
    fflush(stdout);

    for (int i = 0; i < n; i++) close(g_sock[i]);
    return rc;
}
