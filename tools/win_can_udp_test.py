#!/usr/bin/env python3
"""Windows UDP test tool for the MCXE31B CAN0 gateway path."""

from __future__ import annotations

import argparse
import dataclasses
import json
import socket
import struct
import time


MAGIC = 0x43474644
VERSION = 1
MAX_CHANNELS = 6
MAX_DATA_LEN = 64
DATA_PORT = 50000
STATUS_PORT = 50001
LISTEN_POLL_TIMEOUT = 0.2

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
STATUS_NO_PEER = 0x00000040
STATUS_PARSE_ERROR = 0x00000080

GATEWAY_FRAME = struct.Struct("<IBBBBIII64s")


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

        length = dlc_to_len(dlc) if flags & FLAG_FD else min(dlc, 8)
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


def parse_data(text: str) -> bytes:
    if not text:
        return b""

    normalized = text.replace(",", " ").replace(":", " ").replace("-", " ")
    values = []
    for token in normalized.split():
        value = int(token, 16)
        if not 0 <= value <= 0xFF:
            raise ValueError(f"byte out of range: {token}")
        values.append(value)
    return bytes(values)


def status_name(status: int) -> str:
    names = {
        STATUS_OK: "ok",
        STATUS_CAN_TX_BUSY: "can-tx-busy",
        STATUS_CAN_TX_ERROR: "can-tx-error",
        STATUS_CAN_RX_OVERFLOW: "can-rx-overflow",
        STATUS_INVALID_PACKET: "invalid-packet",
        STATUS_DISABLED_CHANNEL: "disabled-channel",
        STATUS_QUEUE_FULL: "queue-full",
        STATUS_NO_PEER: "no-peer",
        STATUS_PARSE_ERROR: "parse-error",
    }
    return names.get(status, f"0x{status:X}")


def parse_status_packet(packet: bytes) -> dict:
    text = packet.decode("utf-8").strip()
    return json.loads(text)


def format_frame(frame: GatewayFrame, source: tuple[str, int] | None = None) -> str:
    flags = []
    if frame.flags & FLAG_FD:
        flags.append("FD")
    if frame.flags & FLAG_BRS:
        flags.append("BRS")
    if frame.flags & FLAG_EXTENDED_ID:
        flags.append("EFF")
    if frame.flags & FLAG_REMOTE:
        flags.append("RTR")
    if frame.flags & FLAG_ERROR:
        flags.append("ERR")

    flag_text = "|".join(flags) if flags else "Classic"
    data_text = " ".join(f"{byte:02X}" for byte in frame.data)
    prefix = f"{source[0]}:{source[1]} " if source else ""
    return (
        f"{prefix}ch={frame.channel} id=0x{frame.can_id:X} dlc={frame.dlc} "
        f"flags={flag_text} status={status_name(frame.status)} data=[{data_text}]"
    )


def format_status(status: dict) -> str:
    can0 = next((item for item in status.get("can", []) if item.get("ch") == 0), {})
    return (
        f"link={status.get('link')} dhcp={status.get('dhcp')} ip={status.get('ip')} "
        f"active_mask=0x{int(status.get('active_mask', 0)):X} peer={status.get('peer')} "
        f"can0.rx={can0.get('rx')} can0.tx_start={can0.get('tx_start')} "
        f"can0.tx_queue={can0.get('tx_queue')} can0.rx_drop={can0.get('rx_drop')} "
        f"can0.tx_drop={can0.get('tx_drop')} can0.error={can0.get('error')}"
    )


def make_frame(args: argparse.Namespace) -> GatewayFrame:
    data = parse_data(args.data)
    flags = 0

    if args.fd:
        flags |= FLAG_FD
    if args.brs:
        flags |= FLAG_BRS
    if args.extended:
        flags |= FLAG_EXTENDED_ID
    if args.remote:
        flags |= FLAG_REMOTE

    if not args.fd and len(data) > 8:
        raise ValueError("classic CAN payload must be <= 8 bytes; add --fd for CAN FD")
    if args.remote and args.fd:
        raise ValueError("CAN FD remote frames are not valid")

    dlc = len_to_dlc(len(data)) if args.fd else len(data)
    can_id = int(args.send_id, 0)
    return GatewayFrame(channel=args.channel, flags=flags, dlc=dlc, can_id=can_id, data=data)


def open_udp_socket(local_port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", local_port))
    return sock


def open_udp_send_socket() -> socket.socket:
    return socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def send_learn_packet(sock: socket.socket, remote: tuple[str, int]) -> None:
    sock.sendto(b"\x00", remote)


def query_control(board: str, status_port: int, payload: bytes, timeout: float | None) -> dict:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(payload, (board, status_port))
        packet, _ = sock.recvfrom(4096)
        return parse_status_packet(packet)
    finally:
        sock.close()


def query_status(board: str, status_port: int, timeout: float | None) -> dict:
    return query_control(board, status_port, b"get_status\n", timeout)


def query_config(board: str, status_port: int, timeout: float | None) -> dict:
    return query_control(board, status_port, b"get_config\n", timeout)


def build_config_command(args: argparse.Namespace) -> bytes:
    fields = ["set_can0_config"]

    if args.can_enabled is not None:
        fields.append(f"enabled={int(args.can_enabled)}")
    if args.can_fd is not None:
        fields.append(f"fd={int(args.can_fd)}")
    if args.bitrate is not None:
        fields.append(f"bitrate={args.bitrate}")
    if args.data_bitrate is not None:
        fields.append(f"data_bitrate={args.data_bitrate}")
    if args.config_brs is not None:
        fields.append(f"brs={int(args.config_brs)}")
    if args.filter_mode is not None:
        fields.append(f"filter={args.filter_mode}")
    if args.filter_id is not None:
        fields.append(f"filter_id={int(args.filter_id, 0)}")
    if args.filter_mask is not None:
        fields.append(f"filter_mask={int(args.filter_mask, 0)}")
    if args.tx_drop_policy is not None:
        fields.append(f"tx_drop_policy={args.tx_drop_policy}")

    return (" ".join(fields) + "\n").encode("ascii")


def configure_can0(args: argparse.Namespace) -> dict:
    return query_control(args.board, args.status_port, build_config_command(args), args.timeout)


def raise_control_timeout(args: argparse.Namespace, exc: TimeoutError) -> None:
    raise SystemExit(
        f"error: timeout waiting for MCU status/control response on "
        f"{args.board}:{args.status_port}; check the board is running firmware with "
        f"UDP status/control port {args.status_port}"
    ) from exc


def watch_status(args: argparse.Namespace) -> None:
    while True:
        status = query_status(args.board, args.status_port, args.timeout)
        if args.json:
            print(json.dumps(status, separators=(",", ":")))
        else:
            print(format_status(status))
        time.sleep(args.interval_ms / 1000.0)


def listen(sock: socket.socket, timeout: float | None) -> None:
    sock.settimeout(timeout if timeout is not None else LISTEN_POLL_TIMEOUT)
    while True:
        try:
            packet, source = sock.recvfrom(2048)
        except socket.timeout:
            if timeout is None:
                continue
            return
        try:
            frame = GatewayFrame.unpack(packet)
        except ValueError as exc:
            print(f"drop {source[0]}:{source[1]} {exc}")
            continue
        print(format_frame(frame, source))


def run(args: argparse.Namespace) -> None:
    remote = (args.board, args.remote_port)
    if args.listen:
        sock = open_udp_socket(args.local_port)
    elif args.send_id is not None:
        sock = open_udp_send_socket()
    else:
        sock = None

    if sock is not None and args.listen and args.learn:
        send_learn_packet(sock, remote)
        print(f"learn sent to {remote[0]}:{remote[1]}")

    if sock is not None and args.send_id is not None:
        frame = make_frame(args)
        for index in range(args.count):
            sock.sendto(frame.pack(), remote)
            print(f"tx {index + 1}/{args.count} {format_frame(frame)}")
            if args.interval_ms and index + 1 < args.count:
                time.sleep(args.interval_ms / 1000.0)

    try:
        if args.status:
            status = query_status(args.board, args.status_port, args.timeout)
            if args.json:
                print(json.dumps(status, separators=(",", ":")))
            else:
                print(format_status(status))

        if args.get_config:
            config = query_config(args.board, args.status_port, args.timeout)
            print(json.dumps(config, separators=(",", ":")))

        if args.set_can0_config:
            config = configure_can0(args)
            print(json.dumps(config, separators=(",", ":")))

        if args.watch_status:
            watch_status(args)
    except TimeoutError as exc:
        raise_control_timeout(args, exc)

    if sock is not None and args.listen:
        listen(sock, args.timeout)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", required=True, help="FRDM-MCXE31B IPv4 address from DHCP log")
    parser.add_argument("--remote-port", type=int, default=DATA_PORT, help="MCU UDP data port")
    parser.add_argument("--status-port", type=int, default=STATUS_PORT, help="MCU UDP status port")
    parser.add_argument("--local-port", type=int, default=DATA_PORT, help="local UDP port for replies")
    parser.add_argument("--channel", type=int, default=0, help="gateway channel, CAN0 is 0")
    parser.add_argument("--send-id", help="CAN ID to send, for example 0x123")
    parser.add_argument("--data", default="", help='hex bytes, for example "11 22 33 44"')
    parser.add_argument("--fd", action="store_true", help="send CAN FD frame")
    parser.add_argument("--brs", action="store_true", help="set CAN FD BRS flag")
    parser.add_argument("--extended", action="store_true", help="send extended ID frame")
    parser.add_argument("--remote", action="store_true", help="send remote frame")
    parser.add_argument("--count", type=int, default=1, help="number of frames to send")
    parser.add_argument("--interval-ms", type=int, default=100, help="delay between repeated sends")
    parser.add_argument("--listen", action="store_true", help="print UDP frames received from the MCU")
    parser.add_argument("--status", action="store_true", help="query one JSON status snapshot from the MCU")
    parser.add_argument("--watch-status", action="store_true", help="query status repeatedly")
    parser.add_argument("--get-config", action="store_true", help="query CAN runtime configuration")
    parser.add_argument("--set-can0-config", action="store_true", help="apply CAN0 runtime configuration")
    parser.add_argument("--can-enabled", action=argparse.BooleanOptionalAction, default=None,
                        help="enable or disable CAN0 for --set-can0-config")
    parser.add_argument("--can-fd", action=argparse.BooleanOptionalAction, default=None,
                        help="select CAN FD or Classic CAN for --set-can0-config")
    parser.add_argument("--bitrate", type=int, help="CAN nominal bitrate for --set-can0-config")
    parser.add_argument("--data-bitrate", type=int, help="CAN FD data bitrate for --set-can0-config")
    parser.add_argument("--config-brs", action=argparse.BooleanOptionalAction, default=None,
                        help="enable or disable CAN FD BRS for --set-can0-config")
    parser.add_argument("--filter-mode", choices=("accept_all", "id_mask"),
                        help="software RX filter mode for --set-can0-config")
    parser.add_argument("--filter-id", help="software RX filter ID for --set-can0-config")
    parser.add_argument("--filter-mask", help="software RX filter mask for --set-can0-config")
    parser.add_argument("--tx-drop-policy", choices=("drop_newest", "drop_oldest"),
                        help="TX queue overflow policy for --set-can0-config")
    parser.add_argument("--json", action="store_true", help="print raw compact JSON for status output")
    parser.add_argument("--learn", action=argparse.BooleanOptionalAction, default=True,
                        help="send a one-byte learn packet before listening")
    parser.add_argument("--timeout", type=float, default=None, help="listen timeout in seconds")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    try:
        run(args)
    except KeyboardInterrupt as exc:
        raise SystemExit(0) from exc


if __name__ == "__main__":
    main()
