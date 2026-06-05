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

#include <net/if.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
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

int main(int argc, char **argv)
{
    const char *ifaces[MAX_CH];
    int n = 0;
    double rate = 4000.0, duration = 10.0;
    int length = 16, brs = 1;
    unsigned base_id = 0x100, seed = 1;

    static const struct option o[] = {
        {"rate", required_argument, 0, 'r'}, {"duration", required_argument, 0, 'd'},
        {"len", required_argument, 0, 'l'}, {"base-id", required_argument, 0, 'b'},
        {"no-brs", no_argument, 0, 'B'}, {"seed", required_argument, 0, 's'},
        {"ifaces", no_argument, 0, 'i'}, {"help", no_argument, 0, 'h'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "r:d:l:b:Bs:ih", o, NULL)) != -1) {
        switch (c) {
        case 'r': rate = atof(optarg); break;
        case 'd': duration = atof(optarg); break;
        case 'l': length = atoi(optarg); break;
        case 'b': base_id = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'B': brs = 0; break;
        case 's': seed = (unsigned)atoi(optarg); break;
        case 'i': break; /* accept --ifaces, names follow as positional args */
        case 'h':
        default:
            fprintf(stderr, "usage: %s [--ifaces] can0 can1 ... [--rate fps] [--duration s]"
                            " [--len 15..64] [--base-id 0x100] [--no-brs] [--seed N]\n", argv[0]);
            return c == 'h' ? 0 : 1;
        }
    }
    for (int i = optind; i < argc && n < MAX_CH; i++) ifaces[n++] = argv[i];
    if (n == 0) {
        static const char *def[MAX_CH] = {"can0", "can1", "can2", "can3", "can4", "can5"};
        for (int i = 0; i < MAX_CH; i++) ifaces[i] = def[i];
        n = MAX_CH;
    }
    if (n < 2) { fprintf(stderr, "need >= 2 interfaces on the shared bus\n"); return 1; }
    if (length < HDR_SZ) length = HDR_SZ;
    if (length > 64) length = 64;
    g_n = n;
    for (int i = 0; i < n; i++) g_sock[i] = open_can(ifaces[i]);

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

    uint64_t expected = sent * (uint64_t)(n - 1);
    uint64_t rx = st_rx;
    long long loss = (long long)expected - (long long)rx;
    double loss_rate = expected ? (double)loss / (double)expected * 100.0 : 0.0;

    char label[64];
    if (n == 2)
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
                      rx == 0 ? "no frames returned - check bus wiring & 120R x2=60R termination"
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
