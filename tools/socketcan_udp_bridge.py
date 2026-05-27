#!/usr/bin/env python3
"""Userspace SocketCAN to UDP bridge for the MCXE31B gateway protocol."""

from __future__ import annotations

import argparse
import dataclasses
import selectors
import socket
import struct
from typing import Sequence


MAGIC = 0x43474644
VERSION = 1
MAX_CHANNELS = 6
MAX_DATA_LEN = 64
DATA_PORT = 50000
STATUS_PORT = 50001

FLAG_FD = 0x01
FLAG_BRS = 0x02
FLAG_EXTENDED_ID = 0x04
FLAG_REMOTE = 0x08
FLAG_ERROR = 0x10

CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
CAN_SFF_MASK = 0x000007FF
CAN_EFF_MASK = 0x1FFFFFFF

CANFD_BRS = 0x01

GATEWAY_FRAME = struct.Struct("<IBBBBIII64s")
CAN_CLASSIC_FRAME = struct.Struct("<IB3x8s")
CANFD_FRAME = struct.Struct("<IBB2x64s")


@dataclasses.dataclass(frozen=True)
class GatewayFrame:
    channel: int
    flags: int
    dlc: int
    can_id: int
    timestamp: int = 0
    status: int = 0
    data: bytes = b""

    def pack(self) -> bytes:
        payload = self.data[:MAX_DATA_LEN].ljust(MAX_DATA_LEN, b"\x00")
        return GATEWAY_FRAME.pack(
            MAGIC,
            VERSION,
            self.channel,
            self.flags,
            self.dlc,
            self.can_id,
            self.timestamp,
            self.status,
            payload,
        )

    @classmethod
    def unpack(cls, packet: bytes) -> "GatewayFrame":
        if len(packet) != GATEWAY_FRAME.size:
            raise ValueError(f"invalid packet size: {len(packet)}")

        magic, version, channel, flags, dlc, can_id, timestamp, status, data = GATEWAY_FRAME.unpack(packet)
        if magic != MAGIC:
            raise ValueError(f"invalid magic: 0x{magic:08x}")
        if version != VERSION:
            raise ValueError(f"unsupported version: {version}")
        if channel >= MAX_CHANNELS:
            raise ValueError(f"invalid channel: {channel}")

        length = dlc_to_len(dlc) if (flags & FLAG_FD) else min(dlc, 8)
        return cls(channel, flags, dlc, can_id, timestamp, status, data[:length])


def len_to_dlc(length: int) -> int:
    if length <= 8:
        return length
    for dlc, size in ((9, 12), (10, 16), (11, 20), (12, 24), (13, 32), (14, 48), (15, 64)):
        if length <= size:
            return dlc
    raise ValueError(f"CAN FD payload too large: {length}")


def dlc_to_len(dlc: int) -> int:
    table = (0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64)
    if dlc >= len(table):
        raise ValueError(f"invalid DLC: {dlc}")
    return table[dlc]


def open_can_socket(interface: str) -> socket.socket:
    can_sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    if hasattr(socket, "CAN_RAW_FD_FRAMES"):
        can_sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FD_FRAMES, 1)
    can_sock.bind((interface,))
    can_sock.setblocking(False)
    return can_sock


def gateway_from_can(channel: int, packet: bytes) -> GatewayFrame:
    if len(packet) == CANFD_FRAME.size:
        can_id, length, can_flags, data = CANFD_FRAME.unpack(packet)
        flags = FLAG_FD
        if can_flags & CANFD_BRS:
            flags |= FLAG_BRS
        dlc = len_to_dlc(length)
    elif len(packet) == CAN_CLASSIC_FRAME.size:
        can_id, length, data = CAN_CLASSIC_FRAME.unpack(packet)
        flags = 0
        dlc = min(length, 8)
    else:
        raise ValueError(f"unsupported SocketCAN frame size: {len(packet)}")

    if can_id & CAN_EFF_FLAG:
        flags |= FLAG_EXTENDED_ID
        wire_can_id = can_id & CAN_EFF_MASK
    else:
        wire_can_id = can_id & CAN_SFF_MASK
    if can_id & CAN_RTR_FLAG:
        flags |= FLAG_REMOTE
    if can_id & CAN_ERR_FLAG:
        flags |= FLAG_ERROR

    return GatewayFrame(channel=channel, flags=flags, dlc=dlc, can_id=wire_can_id, data=data[:length])


def can_from_gateway(frame: GatewayFrame) -> bytes:
    can_id = frame.can_id & (CAN_EFF_MASK if frame.flags & FLAG_EXTENDED_ID else CAN_SFF_MASK)
    if frame.flags & FLAG_EXTENDED_ID:
        can_id |= CAN_EFF_FLAG
    if frame.flags & FLAG_REMOTE:
        can_id |= CAN_RTR_FLAG
    if frame.flags & FLAG_ERROR:
        can_id |= CAN_ERR_FLAG

    if frame.flags & FLAG_FD:
        length = dlc_to_len(frame.dlc)
        can_flags = CANFD_BRS if frame.flags & FLAG_BRS else 0
        return CANFD_FRAME.pack(can_id, length, can_flags, frame.data[:length].ljust(MAX_DATA_LEN, b"\x00"))

    length = min(frame.dlc, 8)
    return CAN_CLASSIC_FRAME.pack(can_id, length, frame.data[:length].ljust(8, b"\x00"))


def run_bridge(can_interfaces: Sequence[str], remote_host: str, remote_port: int, local_port: int) -> None:
    selector = selectors.DefaultSelector()
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.bind(("0.0.0.0", local_port))
    udp_sock.setblocking(False)
    selector.register(udp_sock, selectors.EVENT_READ, ("udp", None))

    can_socks = []
    for channel, interface in enumerate(can_interfaces):
        can_sock = open_can_socket(interface)
        can_socks.append(can_sock)
        selector.register(can_sock, selectors.EVENT_READ, ("can", channel))

    remote = (remote_host, remote_port)
    while True:
        for key, _ in selector.select():
            kind, channel = key.data
            if kind == "can":
                packet = key.fileobj.recv(CANFD_FRAME.size)
                udp_sock.sendto(gateway_from_can(channel, packet).pack(), remote)
            else:
                packet, _ = udp_sock.recvfrom(GATEWAY_FRAME.size)
                frame = GatewayFrame.unpack(packet)
                if frame.channel >= len(can_socks):
                    continue
                can_socks[frame.channel].send(can_from_gateway(frame))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--remote-host", required=True, help="MCU IPv4 address")
    parser.add_argument("--remote-port", type=int, default=DATA_PORT)
    parser.add_argument("--local-port", type=int, default=DATA_PORT)
    parser.add_argument("--can", nargs="+", default=[f"can{i}" for i in range(MAX_CHANNELS)])
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    run_bridge(args.can, args.remote_host, args.remote_port, args.local_port)


if __name__ == "__main__":
    main()
