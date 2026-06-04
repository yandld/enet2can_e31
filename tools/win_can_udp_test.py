#!/usr/bin/env python3
"""Windows UDP config and pressure tool for the MCXE31B SocketCAN-first gateway."""

from __future__ import annotations

import argparse
import json
import socket
import sys
import time

from can_gateway_protocol import (
    CONTROL_PORT,
    DATA_PORT,
    FLAG_BRS,
    FLAG_ERROR,
    FLAG_EXTENDED_ID,
    FLAG_FD,
    FLAG_REMOTE,
    GatewayFrame,
    GatewayPacket,
    MAX_FRAMES_PER_PACKET,
    dlc_to_len,
    len_to_dlc,
    pack_frames,
    status_name,
)


LISTEN_POLL_TIMEOUT = 0.2
DEFAULT_CONTROL_TIMEOUT = 2.0


def progress(message: str) -> None:
    print(message, file=sys.stderr)


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


def parse_status_packet(packet: bytes) -> dict:
    return json.loads(packet.decode("utf-8").strip())


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
    tunnel = status.get("tunnel", {})
    return (
        f"link={status.get('link')} dhcp={status.get('dhcp')} ip={status.get('ip')} "
        f"active_mask=0x{int(status.get('active_mask', 0)):X} session={tunnel.get('session')} "
        f"seq_rx={tunnel.get('rx_seq')} seq_tx={tunnel.get('tx_seq')} loss={tunnel.get('loss')} "
        f"can0.rx={can0.get('rx')} can0.tx_start={can0.get('tx_start')} "
        f"can0.tx_queue={can0.get('tx_queue')} can0.rx_drop={can0.get('rx_drop')} "
        f"can0.tx_drop={can0.get('tx_drop')} can0.error={can0.get('error')}"
    )


def find_channel(items: list[dict] | None, channel: int) -> dict:
    return next((item for item in items or [] if item.get("ch") == channel), {})


def bool_text(value: object) -> str:
    return "yes" if value else "no"


def link_text(value: object) -> str:
    return "up" if value else "down"


def dhcp_text(value: object) -> str:
    return "bound" if value else "waiting"


def can_mode_text(config: dict, status: dict) -> str:
    return "CAN FD" if config.get("fd", status.get("fd")) else "Classic CAN"


def brs_text(config: dict) -> str:
    return "on" if config.get("brs") else "off"


def format_status_summary(status: dict) -> str:
    tunnel = status.get("tunnel", {})
    router = status.get("router", {})
    can_items = status.get("can") or []
    config_items = status.get("config") or []
    session = tunnel.get("session", "none")
    warnings = []

    lines = [
        (
            f"Ethernet: link={link_text(status.get('link'))} "
            f"dhcp={dhcp_text(status.get('dhcp'))} "
            f"ip={status.get('ip', '0.0.0.0')} "
            f"active_mask=0x{int(status.get('active_mask', 0) or 0):X}"
        ),
        (
            f"Tunnel: session={session} "
            f"packets rx={tunnel.get('rx_packets', 0)} tx={tunnel.get('tx_packets', 0)} "
            f"frames rx={tunnel.get('rx_frames', 0)} tx={tunnel.get('tx_frames', 0)} "
            f"drop={tunnel.get('drop', 0)} parse_error={tunnel.get('parse_error', 0)} "
            f"no_session={tunnel.get('no_session', 0)} loss={tunnel.get('loss', 0)}"
        ),
        (
            f"Router: rx={router.get('rx', 0)} tx={router.get('tx', 0)} "
            f"drop={router.get('drop', 0)} parse_error={router.get('parse_error', 0)} "
            f"disabled={router.get('disabled', 0)} queue_full={router.get('queue_full', 0)}"
        ),
    ]

    channels = sorted({
        int(item.get("ch"))
        for item in can_items + config_items
        if isinstance(item.get("ch"), int)
    })
    for channel in channels:
        can = find_channel(can_items, channel)
        config = find_channel(config_items, channel)
        rx_queue = int(can.get("rx_queue", 0) or 0)
        tx_drop = int(can.get("tx_drop", 0) or 0)
        rx_drop = int(can.get("rx_drop", 0) or 0)
        tx_err = int(can.get("tx_err_counter", 0) or 0)
        rx_err = int(can.get("rx_err_counter", 0) or 0)
        state = can.get("state", "unknown")

        lines.extend([
            (
                f"CAN{channel}: enabled={bool_text(config.get('enabled', can.get('enabled')))} "
                f"mode={can_mode_text(config, can)} "
                f"bitrate={config.get('bitrate', can.get('bitRate', 0))}/"
                f"{config.get('data_bitrate', can.get('bitRateFD', 0))} "
                f"brs={brs_text(config)} state={state}"
            ),
            (
                f"CAN{channel} counters: rx={can.get('rx', 0)} tx_start={can.get('tx_start', 0)} "
                f"tx_done={can.get('tx_done', 0)} rxq={rx_queue} txq={can.get('tx_queue', 0)} "
                f"rx_drop={rx_drop} tx_drop={tx_drop} "
                f"fifo_ovf={can.get('rx_fifo_overflow', 0)}"
            ),
            (
                f"CAN{channel} errors: total={can.get('error', 0)} "
                f"tx_err_counter={tx_err} rx_err_counter={rx_err} "
                f"last={can.get('last_error', 0)}"
            ),
        ])

        if state == "bus-off":
            warnings.append(f"WARNING: CAN{channel} is bus-off.")
        if (session == "none") and (rx_queue > 0):
            warnings.append(f"WARNING: CAN{channel} has {rx_queue} queued RX frame(s) but no UDP data session.")
        if (tx_err > 0) or (rx_err > 0):
            warnings.append(f"WARNING: CAN{channel} error counters tx={tx_err} rx={rx_err}.")
        if (tx_drop > 0) or (rx_drop > 0):
            warnings.append(f"WARNING: CAN{channel} drops tx={tx_drop} rx={rx_drop}.")

    return "\n".join(lines + warnings)


def format_status_output(status: dict, json_mode: bool, compact_json: bool) -> str:
    if compact_json:
        return json.dumps(status, separators=(",", ":"))
    if json_mode:
        return json.dumps(status, indent=2, sort_keys=True)
    return format_status_summary(status)


def format_json_output(payload: dict, compact_json: bool) -> str:
    if compact_json:
        return json.dumps(payload, separators=(",", ":"))
    return json.dumps(payload, indent=2, sort_keys=True)


def counter_delta(before: dict, after: dict, section: str, key: str) -> int:
    return int(after.get(section, {}).get(key, 0) or 0) - int(before.get(section, {}).get(key, 0) or 0)


def channel_status(status: dict, channel: int) -> dict:
    return find_channel(status.get("can") or [], channel)


def channel_delta(before: dict, after: dict, channel: int, key: str) -> int:
    return int(channel_status(after, channel).get(key, 0) or 0) - int(channel_status(before, channel).get(key, 0) or 0)


def evaluate_pressure_acceptance(before: dict, after: dict, report: dict) -> dict:
    frames_sent = int(report.get("frames_sent", 0) or 0)
    channels = [int(channel) for channel in report.get("channels", [report.get("channel", 0)])]
    frames_per_channel = report.get("frames_per_channel", {})
    failures: list[str] = []
    can_counters: dict[str, dict[str, object]] = {}
    counters = {
        "tunnel_rx_frames_delta": counter_delta(before, after, "tunnel", "rx_frames"),
        "tunnel_drop_delta": counter_delta(before, after, "tunnel", "drop"),
        "tunnel_parse_error_delta": counter_delta(before, after, "tunnel", "parse_error"),
        "tunnel_loss_delta": counter_delta(before, after, "tunnel", "loss"),
        "router_rx_delta": counter_delta(before, after, "router", "rx"),
        "router_drop_delta": counter_delta(before, after, "router", "drop"),
        "router_parse_error_delta": counter_delta(before, after, "router", "parse_error"),
        "router_disabled_delta": counter_delta(before, after, "router", "disabled"),
        "router_queue_full_delta": counter_delta(before, after, "router", "queue_full"),
        "can": can_counters,
    }

    if counters["tunnel_rx_frames_delta"] < frames_sent:
        failures.append(f"tunnel.rx_frames delta {counters['tunnel_rx_frames_delta']} < frames_sent {frames_sent}")
    if counters["router_rx_delta"] < frames_sent:
        failures.append(f"router.rx delta {counters['router_rx_delta']} < frames_sent {frames_sent}")

    for name, label in (
        ("tunnel_drop_delta", "tunnel.drop"),
        ("tunnel_parse_error_delta", "tunnel.parse_error"),
        ("tunnel_loss_delta", "tunnel.loss"),
        ("router_drop_delta", "router.drop"),
        ("router_parse_error_delta", "router.parse_error"),
        ("router_disabled_delta", "router.disabled"),
        ("router_queue_full_delta", "router.queue_full"),
    ):
        value = int(counters[name])
        if value != 0:
            failures.append(f"{label} delta {value}")

    for channel in channels:
        expected = int(frames_per_channel.get(str(channel), frames_per_channel.get(channel, 0)) or 0)
        post = channel_status(after, channel)
        item = {
            "tx_start_delta": channel_delta(before, after, channel, "tx_start"),
            "tx_done_delta": channel_delta(before, after, channel, "tx_done"),
            "tx_drop_delta": channel_delta(before, after, channel, "tx_drop"),
            "rx_drop_delta": channel_delta(before, after, channel, "rx_drop"),
            "error_delta": channel_delta(before, after, channel, "error"),
            "state": post.get("state", "unknown"),
            "tx_err_counter": int(post.get("tx_err_counter", 0) or 0),
            "rx_err_counter": int(post.get("rx_err_counter", 0) or 0),
        }
        can_counters[str(channel)] = item

        if item["tx_start_delta"] < expected:
            failures.append(f"CAN{channel} tx_start delta {item['tx_start_delta']} < expected {expected}")
        if item["tx_done_delta"] < expected:
            failures.append(f"CAN{channel} tx_done delta {item['tx_done_delta']} < expected {expected}")
        for key, label in (("tx_drop_delta", "tx_drop"), ("rx_drop_delta", "rx_drop"), ("error_delta", "error")):
            if int(item[key]) != 0:
                failures.append(f"CAN{channel} {label} delta {item[key]}")
        if item["state"] == "bus-off":
            failures.append(f"CAN{channel} state bus-off")
        if item["tx_err_counter"] != 0 or item["rx_err_counter"] != 0:
            failures.append(f"CAN{channel} error counters tx={item['tx_err_counter']} rx={item['rx_err_counter']}")

    return {
        "passed": len(failures) == 0,
        "failures": failures,
        "counters": counters,
    }


def make_frame(args: argparse.Namespace, sequence: int = 0, channel: int | None = None) -> GatewayFrame:
    data = parse_data(args.data)
    if args.pressure and not data:
        data = bytes((index & 0xFF) for index in range(args.dlc))

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
    if args.send_id is not None:
        can_id = int(args.send_id, 0)
    elif args.extended:
        can_id = (args.base_id + sequence) & 0x1FFFFFFF
    else:
        can_id = (args.base_id + sequence) & 0x7FF
    return GatewayFrame(channel=args.channel if channel is None else channel, flags=flags, dlc=dlc, can_id=can_id, data=data)


def open_udp_socket(local_port: int) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.bind(("0.0.0.0", local_port))
    except OSError as exc:
        sock.close()
        raise SystemExit(
            f"error: cannot bind local UDP port {local_port}; close another listener "
            f"or choose a different --local-port. Details: {exc}"
        ) from exc
    return sock


def local_socket_port(sock: socket.socket, configured_port: int) -> int:
    try:
        return int(sock.getsockname()[1])
    except (AttributeError, OSError, IndexError):
        return configured_port


def open_udp_send_socket() -> socket.socket:
    return socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


def query_control(board: str, control_port: int, payload: bytes, timeout: float | None, verbose: bool = True) -> dict:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        if verbose:
            progress(f"Checking {board}:{control_port}...")
        sock.sendto(payload, (board, control_port))
        try:
            packet, _ = sock.recvfrom(4096)
        except (TimeoutError, ConnectionResetError) as exc:
            raise TimeoutError() from exc
        return parse_status_packet(packet)
    finally:
        sock.close()


def build_json_command(command: dict) -> bytes:
    return (json.dumps(command, separators=(",", ":")) + "\n").encode("ascii")


def query_status(board: str, control_port: int, timeout: float | None, verbose: bool = True) -> dict:
    return query_control(board, control_port, build_json_command({"cmd": "get_status"}), timeout, verbose)


def query_config(board: str, control_port: int, timeout: float | None) -> dict:
    return query_control(board, control_port, build_json_command({"cmd": "get_config"}), timeout)


def query_capabilities(board: str, control_port: int, timeout: float | None) -> dict:
    return query_control(board, control_port, build_json_command({"cmd": "get_capabilities"}), timeout)


def reset_stats(board: str, control_port: int, timeout: float | None) -> dict:
    return query_control(board, control_port, build_json_command({"cmd": "reset_stats"}), timeout)


def build_config_command(args: argparse.Namespace) -> bytes:
    command: dict[str, object] = {"cmd": "set_can_config", "channel": args.channel}

    if args.can_enabled is not None:
        command["enabled"] = args.can_enabled
    if args.can_fd is not None:
        command["fd"] = args.can_fd
    if args.bitrate is not None:
        command["bitrate"] = args.bitrate
    if args.data_bitrate is not None:
        command["data_bitrate"] = args.data_bitrate
    if args.config_brs is not None:
        command["brs"] = args.config_brs
    if args.filter_mode is not None:
        command["filter"] = args.filter_mode
    if args.filter_id is not None:
        command["filter_id"] = int(args.filter_id, 0)
    if args.filter_mask is not None:
        command["filter_mask"] = int(args.filter_mask, 0)
    if args.tx_drop_policy is not None:
        command["tx_drop_policy"] = args.tx_drop_policy

    return build_json_command(command)


def configure_can(args: argparse.Namespace) -> dict:
    return query_control(args.board, args.control_port, build_config_command(args), control_timeout(args))


def raise_control_timeout(args: argparse.Namespace, exc: TimeoutError) -> None:
    raise SystemExit(
        f"error: timeout waiting for MCU control response on "
        f"{args.board}:{args.control_port}. "
        f"Check board power, Ethernet link, board IP, firewall, and firmware UDP control port."
    ) from exc


def watch_status(args: argparse.Namespace) -> None:
    while True:
        status = query_status(args.board, args.control_port, control_timeout(args), verbose=False)
        print(format_status_output(status, args.json, args.compact_json))
        time.sleep(args.interval_ms / 1000.0)


def listen(sock: socket.socket, timeout: float | None) -> int:
    deadline = (time.monotonic() + timeout) if timeout is not None else None
    received = 0

    sock.settimeout(LISTEN_POLL_TIMEOUT)
    while True:
        try:
            packet, source = sock.recvfrom(2048)
        except (socket.timeout, ConnectionResetError):
            if (deadline is not None) and (time.monotonic() >= deadline):
                return received
            continue
        try:
            gateway_packet = GatewayPacket.unpack(packet)
        except ValueError as exc:
            print(f"drop {source[0]}:{source[1]} {exc}")
            continue
        for frame in gateway_packet.frames:
            print(format_frame(frame, source))
            received += 1
    return received


def establish_data_session(sock: socket.socket, remote: tuple[str, int]) -> None:
    progress(f"Sending UDP data session probe to {remote[0]}:{remote[1]}...")
    sock.sendto(pack_frames(0, []), remote)


def session_control_timeout(args: argparse.Namespace) -> float | None:
    return control_timeout(args)


def control_timeout(args: argparse.Namespace) -> float:
    return DEFAULT_CONTROL_TIMEOUT if args.timeout is None else args.timeout


def verify_data_session(args: argparse.Namespace) -> dict | None:
    status = query_status(args.board, args.control_port, session_control_timeout(args))
    session = status.get("tunnel", {}).get("session", "none")
    if session == "none":
        progress("warning: UDP data session is still none; check data port 50000 and firewall.")
    else:
        progress(f"UDP data session: {session}")
    return status


def send_one_shot(args: argparse.Namespace, sock: socket.socket, remote: tuple[str, int]) -> None:
    frame_count = args.count if args.count is not None else 1
    for index in range(frame_count):
        frame = make_frame(args, index)
        sock.sendto(pack_frames(index, [frame]), remote)
        print(f"tx {index + 1}/{frame_count} {format_frame(frame)}")
        if args.interval_ms and index + 1 < frame_count:
            time.sleep(args.interval_ms / 1000.0)


def run_pressure(args: argparse.Namespace, sock: socket.socket, remote: tuple[str, int]) -> dict:
    channels = args.channels if args.channels is not None else [args.channel]
    frame_count = args.count if args.count is not None else max(1, int(args.duration * args.rate))
    interval = 1.0 / args.rate if args.rate > 0 else 0
    batch_size = max(1, min(args.batch_size, MAX_FRAMES_PER_PACKET))
    start = time.monotonic()
    payload_bytes = args.dlc if args.fd else min(args.dlc, 8)
    args.data = " ".join(f"{index & 0xFF:02X}" for index in range(payload_bytes))
    frames_per_channel = {str(channel): 0 for channel in channels}
    batch = []
    packets_sent = 0

    for sequence in range(frame_count):
        channel = channels[sequence % len(channels)]
        frame = make_frame(args, sequence, channel)
        frames_per_channel[str(channel)] += 1
        batch.append(frame)
        if len(batch) >= batch_size:
            sock.sendto(pack_frames(packets_sent, batch), remote)
            packets_sent += 1
            batch = []
        if interval and sequence + 1 < frame_count:
            time.sleep(interval)

    if batch:
        sock.sendto(pack_frames(packets_sent, batch), remote)
        packets_sent += 1

    elapsed = max(time.monotonic() - start, 0.000001)
    return {
        "mode": "pressure",
        "channel": args.channel,
        "channels": channels,
        "fd": args.fd,
        "brs": args.brs,
        "frames_sent": frame_count,
        "frames_per_channel": frames_per_channel,
        "packets_sent": packets_sent,
        "batch_size": batch_size,
        "payload_bytes": payload_bytes,
        "duration_s": elapsed,
        "target_rate_fps": args.rate,
        "actual_rate_fps": frame_count / elapsed,
    }


def run_smoke(args: argparse.Namespace, sock: socket.socket, remote: tuple[str, int]) -> None:
    timeout = control_timeout(args)
    status = query_status(args.board, args.control_port, timeout)
    print(format_status_output(status, args.json, args.compact_json))
    establish_data_session(sock, remote)
    status = query_status(args.board, args.control_port, timeout)
    print(format_status_output(status, args.json, args.compact_json))
    print("Check complete.")


def run(args: argparse.Namespace) -> None:
    remote = (args.board, args.remote_port)
    sock = open_udp_socket(args.local_port) if (args.listen or args.smoke) else open_udp_send_socket()

    try:
        if args.smoke:
            run_smoke(args, sock, remote)
            return
        if args.pressure:
            pressure_status_before = None
            if args.verify_pressure_status:
                pressure_status_before = query_status(args.board, args.control_port, control_timeout(args), verbose=False)
            report = run_pressure(args, sock, remote)
            if args.verify_pressure_status:
                if args.verify_settle_ms > 0:
                    time.sleep(args.verify_settle_ms / 1000.0)
                pressure_status_after = query_status(args.board, args.control_port, control_timeout(args), verbose=False)
                report["acceptance"] = evaluate_pressure_acceptance(pressure_status_before, pressure_status_after, report)
            print(json.dumps(report, separators=(",", ":")) if args.json_report else report)
        elif args.send_id is not None:
            send_one_shot(args, sock, remote)

        if args.status:
            status = query_status(args.board, args.control_port, control_timeout(args))
            print(format_status_output(status, args.json, args.compact_json))
        if args.get_config:
            print(format_json_output(query_config(args.board, args.control_port, control_timeout(args)), args.compact_json))
        if args.get_capabilities:
            print(format_json_output(query_capabilities(args.board, args.control_port, control_timeout(args)), args.compact_json))
        if args.set_can_config:
            print(format_json_output(configure_can(args), args.compact_json))
        if args.reset_stats:
            print(format_json_output(reset_stats(args.board, args.control_port, control_timeout(args)), args.compact_json))
        if args.watch_status:
            watch_status(args)
        if args.listen:
            establish_data_session(sock, remote)
            progress(f"Listening on UDP {local_socket_port(sock, args.local_port)}...")
            received = listen(sock, args.timeout)
            if (received == 0) and (args.timeout is not None):
                print(f"No frames received in {args.timeout:g}s.")
    except TimeoutError as exc:
        raise_control_timeout(args, exc)
    finally:
        close = getattr(sock, "close", None)
        if close is not None:
            close()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", required=True, help="FRDM-MCXE31B IPv4 address from DHCP log")
    parser.add_argument("--remote-port", type=int, default=DATA_PORT, help="MCU UDP data port")
    parser.add_argument("--control-port", "--status-port", type=int, default=CONTROL_PORT, help="MCU UDP control port")
    parser.add_argument("--local-port", type=int, default=0, help="local UDP port for replies; 0 lets Windows choose")
    parser.add_argument("--channel", type=int, default=0, help="gateway channel, CAN0 is 0")
    parser.add_argument("--channels", nargs="+", type=int,
                        help="gateway channels for pressure mode, round-robin order such as 0 1 2 3 4 5")
    parser.add_argument("--send-id", help="CAN ID to send, for example 0x123")
    parser.add_argument("--base-id", type=lambda value: int(value, 0), default=0x100, help="pressure CAN ID base")
    parser.add_argument("--data", default="", help='hex bytes, for example "11 22 33 44"')
    parser.add_argument("--fd", action="store_true", help="send CAN FD frame")
    parser.add_argument("--brs", action="store_true", help="set CAN FD BRS flag")
    parser.add_argument("--extended", action="store_true", help="send extended ID frame")
    parser.add_argument("--remote", action="store_true", help="send remote frame")
    parser.add_argument("--count", type=int, default=None, help="number of frames to send")
    parser.add_argument("--interval-ms", type=int, default=100, help="delay between repeated sends")
    parser.add_argument("--listen", action="store_true", help="print UDP frames received from the MCU")
    parser.add_argument("--check", dest="smoke", action="store_true",
                        help="check control path and establish UDP data session")
    parser.add_argument("--smoke", dest="smoke", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--status", action="store_true", help="query one JSON status snapshot from the MCU")
    parser.add_argument("--watch-status", action="store_true", help="query status repeatedly")
    parser.add_argument("--get-config", action="store_true", help="query CAN runtime configuration")
    parser.add_argument("--get-capabilities", action="store_true", help="query gateway capabilities")
    parser.add_argument("--set-can-config", action="store_true", help="apply CAN runtime configuration")
    parser.add_argument("--reset-stats", action="store_true", help="clear gateway counters")
    parser.add_argument("--can-enabled", action=argparse.BooleanOptionalAction, default=None,
                        help="enable or disable CAN channel for --set-can-config")
    parser.add_argument("--can-fd", action=argparse.BooleanOptionalAction, default=None,
                        help="select CAN FD or Classic CAN for --set-can-config")
    parser.add_argument("--bitrate", type=int, help="CAN nominal bitrate for --set-can-config")
    parser.add_argument("--data-bitrate", type=int, help="CAN FD data bitrate for --set-can-config")
    parser.add_argument("--config-brs", action=argparse.BooleanOptionalAction, default=None,
                        help="enable or disable CAN FD BRS for --set-can-config")
    parser.add_argument("--filter-mode", choices=("accept_all", "id_mask"), help="software RX filter mode")
    parser.add_argument("--filter-id", help="software RX filter ID")
    parser.add_argument("--filter-mask", help="software RX filter mask")
    parser.add_argument("--tx-drop-policy", choices=("drop_newest", "drop_oldest"), help="TX overflow policy")
    parser.add_argument("--pressure", action="store_true", help="run a summary-only UDP pressure test")
    parser.add_argument("--verify-pressure-status", action="store_true",
                        help="query status before/after pressure and add pass/fail acceptance counters to the report")
    parser.add_argument("--verify-settle-ms", type=int, default=500,
                        help="delay before post-pressure status verification, allowing CAN TX queues to drain")
    parser.add_argument("--duration", type=float, default=10.0, help="pressure duration in seconds")
    parser.add_argument("--rate", type=float, default=100.0, help="pressure target frame rate")
    parser.add_argument("--dlc", type=int, default=64, help="pressure payload length or FD DLC length")
    parser.add_argument("--batch-size", type=int, default=MAX_FRAMES_PER_PACKET,
                        help="frames per UDP packet during pressure mode")
    parser.add_argument("--summary-only", action="store_true", default=True, help="suppress per-frame TX logs")
    parser.add_argument("--json", action="store_true", help="print pretty JSON for status output")
    parser.add_argument("--compact-json", action="store_true", help="print compact JSON for scripts")
    parser.add_argument("--json-report", action="store_true", help="print pressure report as compact JSON")
    parser.add_argument("--timeout", type=float, default=None,
                        help="listen timeout in seconds; listen defaults to forever, control commands default to 2s")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    try:
        run(args)
    except KeyboardInterrupt as exc:
        raise SystemExit(0) from exc


if __name__ == "__main__":
    main()
