# Makefile - canperf (single-file segmented latency measurement tool)
#
# Copyright 2026 NXP
# Author: Ken Li <ken.li@nxp.com>
#
# Default build is native on the current Linux platform. Use `make cross`
# explicitly when producing an aarch64 static binary.
#
# SPDX-License-Identifier: GPL-2.0-only

CROSS_COMPILE ?= aarch64-none-linux-gnu-
ifneq ($(strip $(TOOLCHAIN_DIR)),)
CROSS_CC := $(TOOLCHAIN_DIR)/bin/$(CROSS_COMPILE)gcc
else
CROSS_CC := $(CROSS_COMPILE)gcc
endif

CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -Wformat=2 -D_GNU_SOURCE -pthread
LDFLAGS ?=
CROSS_LDFLAGS ?= -static

all: canperf

canperf: src/canperf.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

cross: src/canperf.c
	$(CROSS_CC) $(CFLAGS) $(CROSS_LDFLAGS) -o canperf $<

host: src/canperf.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o canperf.host $<
	./canperf.host --help > /dev/null

clean:
	rm -f canperf canperf.host

.PHONY: all cross host clean
