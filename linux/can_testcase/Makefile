# Makefile - canperf (single-file segmented latency measurement tool)
#
# Copyright 2026 NXP
# Author: Ken Li <ken.li@nxp.com>
#
# Environment auto-detection: x86 host = aarch64 cross + static link;
# aarch64 board = native gcc. `make host` = x86 ASan compile check.
#
# SPDX-License-Identifier: GPL-2.0-only

ARCH_NOW := $(shell uname -m)
NATIVE   ?= $(if $(filter aarch64,$(ARCH_NOW)),1,0)

TOOLCHAIN_DIR ?= /opt/arm-gnu-toolchain-14.3.rel1-x86_64-aarch64-none-linux-gnu
CROSS_COMPILE ?= aarch64-none-linux-gnu-

ifeq ($(NATIVE),1)
  CC      := gcc
  LDFLAGS ?=
else
  CC      := $(TOOLCHAIN_DIR)/bin/$(CROSS_COMPILE)gcc
  LDFLAGS ?= -static
endif

CFLAGS ?= -O2 -g -Wall -Wextra -Wformat=2 -D_GNU_SOURCE -pthread

all: canperf

canperf: src/canperf.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

host: src/canperf.c
	gcc $(CFLAGS) -fsanitize=address,undefined -o canperf.host $<
	./canperf.host --help > /dev/null

clean:
	rm -f canperf canperf.host

.PHONY: all host clean
