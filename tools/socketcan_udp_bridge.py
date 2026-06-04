#!/usr/bin/env python3
"""Bridge Linux SocketCAN interfaces to the MCXE31B UDP tunnel."""

from __future__ import annotations

import argparse
import dataclasses
import selectors
import socket
import struct
import subprocess
import sys
import time
from typing import Iterable, Sequence

from can_gateway_protocol import (
    DATA_PORT,
    FLAG_BRS,
    FLAG_ERROR,
    FLAG_EXTENDED_ID,
    FLAG_FD,
    FLAG_REMOTE,
    FRAME_RECORD,
    GatewayFrame,
    GatewayPacket,
    MAX_FRAMES_PER_PACKET,
    pack_frames,
    dlc_to_len,
    len_to_dlc,
)


MAX_CHANNELS = 6
MAX_DATA_LEN = 64

CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
CAN_SFF_MASK = 0x000007FF
CAN_EFF_MASK = 0x1FFFFFFF

CANFD_BRS = 0x01

CAN_CLASSIC_FRAME = struct.Struct("<IB3x8s")
CANFD_FRAME = struct.Struct("<IBB2x64s")


@dataclasses.dataclass
class BridgeStats:
    channel_count: int = MAX_CHANNELS
    start_time: float = dataclasses.field(default_factory=time.monotonic)
    udp_rx_packets: int = 0
    udp_rx_frames: int = 0
    udp_tx_packets: int = 0
    udp_tx_frames: int = 0
    udp_sequence_loss: int = 0
    udp_sequence_reorder: int = 0
    udp_parse_errors: int = 0
    udp_send_errors: int = 0
    can_recv_errors: int = 0
    can_send_errors: int = 0
    last_udp_sequence: int | None = None
    last_report_time: float = 0.0

    def __post_init__(self) -> None:
        self.can_rx_frames = [0] * self.channel_count
        self.can_tx_frames = [0] * self.channel_count
        self.last_report_time = self.start_time

    def record_udp_rx(self, packet: GatewayPacket) -> None:
        sequence = int(packet.sequence)
        if self.last_udp_sequence is not None:
            expected = self.last_udp_sequence + 1
            if sequence > expected:
                self.udp_sequence_loss += sequence - expected
            elif sequence < expected:
                self.udp_sequence_reorder += 1
        self.last_udp_sequence = sequence
        self.udp_rx_packets += 1
        self.udp_rx_frames += len(packet.frames)

    def record_udp_tx(self, packet_count: int, frame_count: int) -> None:
        self.udp_tx_packets += packet_count
        self.udp_tx_frames += frame_count

    def record_udp_parse_error(self) -> None:
        self.udp_parse_errors += 1

    def record_udp_send_error(self) -> None:
        self.udp_send_errors += 1

    def record_can_rx(self, channel: int) -> None:
        if 0 <= channel < self.channel_count:
            self.can_rx_frames[channel] += 1

    def record_can_tx(self, channel: int) -> None:
        if 0 <= channel < self.channel_count:
            self.can_tx_frames[channel] += 1

    def record_can_recv_error(self) -> None:
        self.can_recv_errors += 1

    def record_can_send_error(self) -> None:
        self.can_send_errors += 1

    def should_report(self, now: float, interval: float) -> bool:
        if interval <= 0:
            return False
        if (now - self.last_report_time) < interval:
            return False
        self.last_report_time = now
        return True

    def format_report(self, now: float | None = None) -> str:
        if now is None:
            now = time.monotonic()
        elapsed = max(now - self.start_time, 0.000001)
        can_rx = ",".join(str(value) for value in self.can_rx_frames)
        can_tx = ",".join(str(value) for value in self.can_tx_frames)
        return (
            f"stats elapsed={elapsed:.1f}s "
            f"udp_rx_packets={self.udp_rx_packets} udp_rx_frames={self.udp_rx_frames} "
            f"udp_tx_packets={self.udp_tx_packets} udp_tx_frames={self.udp_tx_frames} "
            f"seq_loss={self.udp_sequence_loss} seq_reorder={self.udp_sequence_reorder} "
            f"udp_parse_errors={self.udp_parse_errors} udp_send_errors={self.udp_send_errors} "
            f"can_recv_errors={self.can_recv_errors} can_send_errors={self.can_send_errors} "
            f"udp_rx_fps={self.udp_rx_frames / elapsed:.1f} udp_tx_fps={self.udp_tx_frames / elapsed:.1f} "
            f"can_rx=[{can_rx}] can_tx=[{can_tx}]"
        )


def socketcan_workflow_text() -> str:
    return (
        "SocketCAN workflow:\n"
        "  sudo modprobe vcan\n"
        "  sudo ip link add dev vcan0 type vcan\n"
        "  sudo ip link set up vcan0\n"
        "  candump vcan0\n"
        "  cangen vcan0 -g 1 -L 64 -f\n"
        "  canplayer vcan0=can0 -I trace.log\n"
    )


def setup_vcan(interfaces: Iterable[str]) -> None:
    subprocess.run(["sudo", "modprobe", "vcan"], check=True)
    for interface in interfaces:
        subprocess.run(["sudo", "ip", "link", "add", "dev", interface, "type", "vcan"], check=False)
        subprocess.run(["sudo", "ip", "link", "set", "up", interface], check=True)


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


def make_gateway_packet(sequence: int, frames: Sequence[GatewayFrame]) -> bytes:
    return pack_frames(sequence, frames)


def send_gateway_batches(sock: socket.socket,
                         remote: tuple[str, int],
                         sequence: int,
                         frames: Sequence[GatewayFrame],
                         stats: BridgeStats | None = None) -> int:
    for offset in range(0, len(frames), MAX_FRAMES_PER_PACKET):
        batch = frames[offset:offset + MAX_FRAMES_PER_PACKET]
        try:
            sock.sendto(make_gateway_packet(sequence, batch), remote)
        except OSError:
            if stats is not None:
                stats.record_udp_send_error()
            continue
        if stats is not None:
            stats.record_udp_tx(packet_count=1, frame_count=len(batch))
        sequence += 1
    return sequence


def run_bridge(can_interfaces: Sequence[str],
               remote_host: str,
               remote_port: int,
               local_port: int,
               stats_interval: float = 0.0) -> None:
    selector = selectors.DefaultSelector()
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    can_socks = []
    stats = BridgeStats(channel_count=len(can_interfaces))
    try:
        udp_sock.bind(("0.0.0.0", local_port))
        udp_sock.setblocking(False)
        selector.register(udp_sock, selectors.EVENT_READ, ("udp", None))

        for channel, interface in enumerate(can_interfaces):
            can_sock = open_can_socket(interface)
            can_socks.append(can_sock)
            selector.register(can_sock, selectors.EVENT_READ, ("can", channel))

        remote = (remote_host, remote_port)
        tx_sequence = 0
        udp_sock.sendto(make_gateway_packet(tx_sequence, []), remote)
        stats.record_udp_tx(packet_count=1, frame_count=0)
        tx_sequence += 1

        while True:
            tx_frames = []
            for key, _ in selector.select():
                kind, channel = key.data
                if kind == "can":
                    try:
                        packet = key.fileobj.recv(CANFD_FRAME.size)
                        tx_frames.append(gateway_from_can(channel, packet))
                        stats.record_can_rx(channel)
                    except (OSError, ValueError) as exc:
                        stats.record_can_recv_error()
                        print(f"drop can{channel}: {exc}", file=sys.stderr)
                else:
                    packet, _ = udp_sock.recvfrom(FRAME_RECORD.size * 8 + 64)
                    try:
                        gateway_packet = GatewayPacket.unpack(packet)
                    except ValueError as exc:
                        stats.record_udp_parse_error()
                        print(f"drop udp: {exc}", file=sys.stderr)
                        continue
                    stats.record_udp_rx(gateway_packet)
                    for frame in gateway_packet.frames:
                        if frame.channel < len(can_socks):
                            try:
                                can_socks[frame.channel].send(can_from_gateway(frame))
                                stats.record_can_tx(frame.channel)
                            except OSError as exc:
                                stats.record_can_send_error()
                                print(f"drop can{frame.channel}: {exc}", file=sys.stderr)
            if tx_frames:
                tx_sequence = send_gateway_batches(udp_sock, remote, tx_sequence, tx_frames, stats)
            now = time.monotonic()
            if stats.should_report(now, stats_interval):
                print(stats.format_report(now), file=sys.stderr)
    finally:
        selector.close()
        udp_sock.close()
        for can_sock in can_socks:
            can_sock.close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__,
        epilog=socketcan_workflow_text(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--remote-host", required=True, help="MCU IPv4 address")
    parser.add_argument("--remote-port", type=int, default=DATA_PORT)
    parser.add_argument("--local-port", type=int, default=DATA_PORT)
    parser.add_argument("--can", nargs="+", default=["vcan0"], help="SocketCAN/vcan interfaces mapped to channels")
    parser.add_argument("--setup-vcan", action="store_true", help="create and bring up the requested vcan interfaces")
    parser.add_argument("--stats-interval", type=float, default=0.0,
                        help="print bridge throughput/error stats to stderr every N seconds; 0 disables reports")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    try:
        if args.setup_vcan:
            setup_vcan(args.can)
        run_bridge(args.can, args.remote_host, args.remote_port, args.local_port, getattr(args, "stats_interval", 0.0))
    except KeyboardInterrupt as exc:
        raise SystemExit(0) from exc


if __name__ == "__main__":
    main()
