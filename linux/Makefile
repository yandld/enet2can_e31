# Out-of-tree build of the eth2can phase-1 driver.
#   make                         # against the running kernel
#   make KDIR=/path/to/rte-kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
#
# SPDX-License-Identifier: GPL-2.0

KDIR ?= /lib/modules/$(shell uname -r)/build

obj-m := eth2can.o
eth2can-objs := src/eth2can.o
ccflags-y := -I$(src)/src

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
