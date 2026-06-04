#!/usr/bin/env python3
"""Shared SocketCAN-first gateway protocol helpers.

Wire format v3: variable-length frame records.
  packet = header (16 bytes) + N frame records
  record = 16-byte fixed head <BBBBIII> {channel,flags,dlc,reserved,can_id,timestamp,status}
           followed by wire_data_len(flags, dlc) payload bytes (0..64).
"""

from __future__ import annotations

import dataclasses
import struct
from typing import Iterable


MAGIC = 0x53434757  # "SCGW"
VERSION = 3
MAX_CHANNELS = 6
MAX_DATA_LEN = 64
MAX_FRAMES_PER_PACKET = 16
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
# 16-byte fixed head of each frame record; payload follows, variable length.
FRAME_HEAD = struct.Struct("<BBBBIII")
FRAME_HEAD_SIZE = FRAME_HEAD.size  # 16
MAX_FRAME_RECORD = FRAME_HEAD_SIZE + MAX_DATA_LEN
MAX_PACKET_BYTES = PACKET_HEADER.size + MAX_FRAMES_PER_PACKET * MAX_FRAME_RECORD


def wire_data_len(flags: int, dlc: int) -> int:
    """Number of payload bytes carried on the wire for this frame.

    Mirrors the firmware: CAN FD uses the DLC->length table, Classic CAN is
    capped at 8 bytes. Both ends MUST agree so variable records stay aligned.
    """
    if flags & FLAG_FD:
        return dlc_to_len(dlc)
    return min(dlc, 8)


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
        length = wire_data_len(self.flags, self.dlc)
        payload = self.data[:length].ljust(length, b"\x00")
        head = FRAME_HEAD.pack(
            self.channel, self.flags, self.dlc, 0, self.can_id, self.timestamp, self.status
        )
        return head + payload

    @classmethod
    def unpack_from(cls, buffer: bytes, offset: int) -> tuple["GatewayFrame", int]:
        """Parse one variable-length record at offset; return (frame, next_offset)."""
        if offset + FRAME_HEAD_SIZE > len(buffer):
            raise ValueError("truncated frame head")
        channel, flags, dlc, _reserved, can_id, timestamp, status = FRAME_HEAD.unpack_from(buffer, offset)
        if channel >= MAX_CHANNELS:
            raise ValueError(f"invalid channel: {channel}")
        if dlc > 15:
            raise ValueError(f"invalid DLC: {dlc}")
        length = wire_data_len(flags, dlc)
        start = offset + FRAME_HEAD_SIZE
        end = start + length
        if end > len(buffer):
            raise ValueError("truncated frame payload")
        return (
            cls(channel=channel, flags=flags, dlc=dlc, can_id=can_id,
                timestamp=timestamp, status=status, data=buffer[start:end]),
            end,
        )

    @classmethod
    def unpack(cls, payload: bytes) -> "GatewayFrame":
        frame, consumed = cls.unpack_from(payload, 0)
        if consumed != len(payload):
            raise ValueError(f"trailing bytes in frame record: {len(payload) - consumed}")
        return frame


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
        frames = []
        offset = PACKET_HEADER.size
        for _ in range(frame_count):
            frame, offset = GatewayFrame.unpack_from(packet, offset)
            frames.append(frame)
        if offset != len(packet):
            raise ValueError(f"trailing bytes: {len(packet) - offset}")
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
