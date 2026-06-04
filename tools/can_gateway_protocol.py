#!/usr/bin/env python3
"""Shared SocketCAN-first gateway protocol helpers."""

from __future__ import annotations

import dataclasses
import struct
from typing import Iterable


MAGIC = 0x53434757  # "SCGW"
VERSION = 2
MAX_CHANNELS = 6
MAX_DATA_LEN = 64
MAX_FRAMES_PER_PACKET = 8
DATA_PORT = 50000
CONTROL_PORT = 50001

PACKET_TYPE_FRAMES = 1

FLAG_FD = 0x01
FLAG_BRS = 0x02
FLAG_EXTENDED_ID = 0x04
FLAG_REMOTE = 0x08
FLAG_ERROR = 0x10

STATUS_OK = 0x00000000
STATUS_CAN_TX_BUSY = 0x00000001
STATUS_CAN_TX_ERROR = 0x00000002
STATUS_CAN_RX_OVERFLOW = 0x00000004
STATUS_INVALID_PACKET = 0x00000008
STATUS_DISABLED_CHANNEL = 0x00000010
STATUS_QUEUE_FULL = 0x00000020
STATUS_NO_SESSION = 0x00000040
STATUS_PARSE_ERROR = 0x00000080

PACKET_HEADER = struct.Struct("<IBBHII")
FRAME_RECORD = struct.Struct("<BBBBIII64s")


@dataclasses.dataclass(frozen=True)
class GatewayFrame:
    channel: int
    flags: int
    dlc: int
    can_id: int
    timestamp: int = 0
    status: int = STATUS_OK
    data: bytes = b""

    def pack(self) -> bytes:
        if not 0 <= self.channel < MAX_CHANNELS:
            raise ValueError(f"invalid channel: {self.channel}")
        if not 0 <= self.dlc <= 15:
            raise ValueError(f"invalid DLC: {self.dlc}")
        payload = self.data[:MAX_DATA_LEN].ljust(MAX_DATA_LEN, b"\x00")
        return FRAME_RECORD.pack(
            self.channel,
            self.flags,
            self.dlc,
            0,
            self.can_id,
            self.timestamp,
            self.status,
            payload,
        )

    @classmethod
    def unpack(cls, payload: bytes) -> "GatewayFrame":
        if len(payload) != FRAME_RECORD.size:
            raise ValueError(f"invalid frame size: {len(payload)}")
        channel, flags, dlc, _reserved, can_id, timestamp, status, data = FRAME_RECORD.unpack(payload)
        if channel >= MAX_CHANNELS:
            raise ValueError(f"invalid channel: {channel}")
        if dlc > 15:
            raise ValueError(f"invalid DLC: {dlc}")
        length = dlc_to_len(dlc) if (flags & FLAG_FD) else min(dlc, 8)
        return cls(channel=channel, flags=flags, dlc=dlc, can_id=can_id,
                   timestamp=timestamp, status=status, data=data[:length])


@dataclasses.dataclass(frozen=True)
class GatewayPacket:
    sequence: int
    frames: tuple[GatewayFrame, ...] | list[GatewayFrame]
    status: int = STATUS_OK
    packet_type: int = PACKET_TYPE_FRAMES

    def pack(self) -> bytes:
        frames = tuple(self.frames)
        if len(frames) > MAX_FRAMES_PER_PACKET:
            raise ValueError(f"too many frames: {len(frames)}")
        header = PACKET_HEADER.pack(MAGIC, VERSION, self.packet_type, len(frames), self.sequence, self.status)
        return header + b"".join(frame.pack() for frame in frames)

    @classmethod
    def unpack(cls, packet: bytes) -> "GatewayPacket":
        if len(packet) < PACKET_HEADER.size:
            raise ValueError(f"packet too short: {len(packet)}")
        magic, version, packet_type, frame_count, sequence, status = PACKET_HEADER.unpack(
            packet[:PACKET_HEADER.size]
        )
        if magic != MAGIC:
            raise ValueError(f"invalid magic: 0x{magic:08x}")
        if version != VERSION:
            raise ValueError(f"unsupported version: {version}")
        if packet_type != PACKET_TYPE_FRAMES:
            raise ValueError(f"unsupported packet type: {packet_type}")
        if frame_count > MAX_FRAMES_PER_PACKET:
            raise ValueError(f"too many frames: {frame_count}")
        expected_len = PACKET_HEADER.size + frame_count * FRAME_RECORD.size
        if len(packet) != expected_len:
            raise ValueError(f"invalid packet size: {len(packet)} expected {expected_len}")
        frames = []
        offset = PACKET_HEADER.size
        for _ in range(frame_count):
            frames.append(GatewayFrame.unpack(packet[offset:offset + FRAME_RECORD.size]))
            offset += FRAME_RECORD.size
        return cls(sequence=sequence, frames=frames, status=status, packet_type=packet_type)


def pack_frames(sequence: int, frames: Iterable[GatewayFrame], status: int = STATUS_OK) -> bytes:
    return GatewayPacket(sequence=sequence, frames=tuple(frames), status=status).pack()


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


def status_name(status: int) -> str:
    names = {
        STATUS_OK: "ok",
        STATUS_CAN_TX_BUSY: "can-tx-busy",
        STATUS_CAN_TX_ERROR: "can-tx-error",
        STATUS_CAN_RX_OVERFLOW: "can-rx-overflow",
        STATUS_INVALID_PACKET: "invalid-packet",
        STATUS_DISABLED_CHANNEL: "disabled-channel",
        STATUS_QUEUE_FULL: "queue-full",
        STATUS_NO_SESSION: "no-session",
        STATUS_PARSE_ERROR: "parse-error",
    }
    return names.get(status, f"0x{status:X}")
