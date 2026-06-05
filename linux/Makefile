# mcxe31b-canbridge - libc-only userspace UDP<->SocketCAN bridge.
# Build on the target (Raspberry Pi 4B / RK3588) with: make
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -Isrc   # _GNU_SOURCE is defined at the top of each .c (keeps standalone cc working)

PREFIX     ?= /usr
SBINDIR    ?= $(PREFIX)/sbin
LIBEXECDIR ?= $(PREFIX)/libexec/mcxe31b-canbridge
UNITDIR    ?= /lib/systemd/system
DEFAULTDIR ?= /etc/default
# Firmware wire header used by `check-proto` to catch protocol drift.
FW_HEADER  ?= ../../source/can_gateway_protocol.h

BIN = canbridge canbridge_ctl canmesh

all: $(BIN)

canbridge: src/canbridge.c src/can_gateway_wire.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/canbridge.c

canbridge_ctl: src/canbridge_ctl.c src/can_gateway_wire.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/canbridge_ctl.c

# Pressure test (C; epoll+recvmmsg+pthread so the tool is never the bottleneck).
canmesh: src/canmesh.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -pthread -o $@ src/canmesh.c

# Static drift guard: the bridge's wire header must agree with the firmware header.
check-proto:
	sh scripts/check-proto.sh src/can_gateway_wire.h $(FW_HEADER)

install: all
	install -d $(DESTDIR)$(SBINDIR) $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(UNITDIR) $(DESTDIR)$(DEFAULTDIR)
	install -m 0755 canbridge canbridge_ctl $(DESTDIR)$(SBINDIR)/
	install -m 0755 canmesh scripts/setup-vcan.sh scripts/stress.sh scripts/selftest.sh $(DESTDIR)$(LIBEXECDIR)/
	install -m 0644 systemd/mcxe31b-canbridge.service $(DESTDIR)$(UNITDIR)/
	[ -f $(DESTDIR)$(DEFAULTDIR)/mcxe31b-canbridge ] || \
		install -m 0644 systemd/mcxe31b-canbridge.default $(DESTDIR)$(DEFAULTDIR)/mcxe31b-canbridge

clean:
	rm -f $(BIN)

.PHONY: all check-proto install clean
