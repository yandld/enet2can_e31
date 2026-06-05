/*
 * canbridge_ctl.c - minimal UDP/JSON control client for the MCXE31B gateway.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Sends one JSON control request to the board's control port (50001) and prints
 * the JSON reply to stdout. libc only, so the pressure rig (scripts/stress.sh)
 * can read board counters and set the real CAN bitrate without a Python runtime
 * on the target. Speaks the JSON control protocol defined in source/can_udp_gateway.c.
 *
 *   canbridge_ctl --board <ip> [--port 50001] [--timeout-ms 2000] <cmd> [k=v ...]
 *
 * Examples:
 *   canbridge_ctl --board 192.168.1.50 get_status
 *   canbridge_ctl --board 192.168.1.50 reset_stats
 *   canbridge_ctl --board 192.168.1.50 set_can_config channel=0 fd=true \
 *                 bitrate=1000000 data_bitrate=5000000 brs=true
 *
 * k=v values are emitted as JSON: true/false -> bool, numeric -> number, else a
 * quoted string. Exit status: 0 on reply, 1 on timeout/error.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "can_gateway_wire.h"

static bool is_number(const char *s)
{
    if (!*s) return false;
    if (*s == '-' || *s == '+') s++;
    bool digit = false, dot = false;
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) digit = true;
        else if (*s == '.' && !dot) dot = true;
        else return false;
    }
    return digit;
}

/* Append a JSON value for the raw token `v` into buf. */
static void append_value(char *buf, size_t cap, const char *v)
{
    size_t len = strlen(buf);
    if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0 || is_number(v))
        snprintf(buf + len, cap - len, "%s", v);
    else
        snprintf(buf + len, cap - len, "\"%s\"", v);
}

int main(int argc, char **argv)
{
    const char *board = NULL;
    uint16_t port = CAN_GATEWAY_UDP_CONTROL_PORT;
    int timeout_ms = 2000;

    static const struct option opts[] = {
        {"board", required_argument, 0, 'b'},
        {"port", required_argument, 0, 'p'},
        {"timeout-ms", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "b:p:t:h", opts, NULL)) != -1) {
        switch (c) {
        case 'b': board = optarg; break;
        case 'p': port = (uint16_t)atoi(optarg); break;
        case 't': timeout_ms = atoi(optarg); break;
        case 'h':
        default:
            fprintf(stderr, "usage: %s --board <ip> [--port n] [--timeout-ms n] <cmd> [k=v ...]\n", argv[0]);
            return (c == 'h') ? 0 : 1;
        }
    }
    if (!board || optind >= argc) {
        fprintf(stderr, "usage: %s --board <ip> [--port n] [--timeout-ms n] <cmd> [k=v ...]\n", argv[0]);
        return 1;
    }

    char req[2048];
    snprintf(req, sizeof(req), "{\"cmd\":\"%s\"", argv[optind]);
    for (int i = optind + 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "ignoring malformed arg '%s' (want k=v)\n", argv[i]); continue; }
        *eq = '\0';
        size_t len = strlen(req);
        snprintf(req + len, sizeof(req) - len, ",\"%s\":", argv[i]);
        append_value(req, sizeof(req), eq + 1);
    }
    size_t len = strlen(req);
    snprintf(req + len, sizeof(req) - len, "}\n");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { perror("socket"); return 1; }
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, board, &dst.sin_addr) != 1) {
        fprintf(stderr, "invalid board IPv4: %s\n", board);
        return 1;
    }

    if (sendto(s, req, strlen(req), 0, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("sendto");
        return 1;
    }

    char reply[8192];
    ssize_t n = recvfrom(s, reply, sizeof(reply) - 1, 0, NULL, NULL);
    if (n < 0) {
        fprintf(stderr, "no reply from %s:%u within %dms\n", board, port, timeout_ms);
        return 1;
    }
    reply[n] = '\0';
    fputs(reply, stdout);
    if (n == 0 || reply[n - 1] != '\n') fputc('\n', stdout);
    return 0;
}
