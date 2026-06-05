#!/usr/bin/env python3
"""Windows UDP test tool for the MCXE31B SocketCAN-first gateway.

Pick exactly one action:
  --status                 query and print board status (add --json for raw)
  --config --channel N     set a channel's CAN config (--fd/--bitrate/--brs/--filter ...)
  --send   --channel N     send frame(s) to a channel (--id, --fd, --brs, --data, --count)
  --listen                 print frames the board forwards from CAN (Ctrl-C to stop)
  --pressure --channels .. sustained load test (+ --rx-watch N to check real CAN delivery)
  --reset-stats            clear all board counters
"""

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
    len_to_dlc,
    pack_frames,
    status_name,
)

CONTROL_TIMEOUT = 2.0


# --- UDP control plane -----------------------------------------------------
def control(board: str, port: int, command: dict, timeout: float = CONTROL_TIMEOUT) -> dict:
    """Send one JSON command to the control port and return the parsed reply."""
    payload = (json.dumps(command, separators=(",", ":")) + "\n").encode("ascii")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(payload, (board, port))
        data, _ = sock.recvfrom(4096)
        return json.loads(data.decode("utf-8").strip())
    except (TimeoutError, ConnectionResetError) as exc:
        raise SystemExit(
            f"error: no control reply from {board}:{port}. "
            f"Check board power, Ethernet link, IP, firewall, and UDP control port."
        ) from exc
    finally:
        sock.close()


def get_status(board: str, port: int) -> dict:
    return control(board, port, {"cmd": "get_status"})


# --- frames ----------------------------------------------------------------
def parse_data(text: str) -> bytes:
    if not text:
        return b""
    tokens = text.replace(",", " ").replace(":", " ").replace("-", " ").split()
    return bytes(int(token, 16) for token in tokens)


def make_frame(channel: int, can_id: int, fd: bool, brs: bool, data: bytes, extended: bool = False) -> GatewayFrame:
    if not fd and len(data) > 8:
        raise SystemExit("error: classic CAN payload must be <= 8 bytes; add --fd for CAN FD")
    flags = 0
    if fd:
        flags |= FLAG_FD
    if brs:
        flags |= FLAG_BRS
    if extended:
        flags |= FLAG_EXTENDED_ID
    dlc = len_to_dlc(len(data)) if fd else len(data)
    return GatewayFrame(channel=channel, flags=flags, dlc=dlc, can_id=can_id, data=data)


def describe_frame(frame: GatewayFrame, source: tuple[str, int] | None = None) -> str:
    names = [name for bit, name in (
        (FLAG_FD, "FD"), (FLAG_BRS, "BRS"), (FLAG_EXTENDED_ID, "EFF"),
        (FLAG_REMOTE, "RTR"), (FLAG_ERROR, "ERR"),
    ) if frame.flags & bit]
    data = " ".join(f"{byte:02X}" for byte in frame.data)
    prefix = f"{source[0]}:{source[1]} " if source else ""
    return (
        f"{prefix}ch={frame.channel} id=0x{frame.can_id:X} dlc={frame.dlc} "
        f"flags={'|'.join(names) or 'Classic'} status={status_name(frame.status)} data=[{data}]"
    )


# --- status ----------------------------------------------------------------
def print_status(status: dict) -> None:
    tunnel = status.get("tunnel", {})
    router = status.get("router", {})
    config = {item.get("ch"): item for item in status.get("config", [])}

    print(
        f"link={'up' if status.get('link') else 'down'} "
        f"dhcp={'bound' if status.get('dhcp') else 'wait'} "
        f"ip={status.get('ip', '0.0.0.0')} active_mask=0x{int(status.get('active_mask', 0) or 0):X} "
        f"session={tunnel.get('session', 'none')}"
    )
    print(
        f"tunnel: rx_frames={tunnel.get('rx_frames', 0)} tx_frames={tunnel.get('tx_frames', 0)} "
        f"drop={tunnel.get('drop', 0)} loss={tunnel.get('loss', 0)} parse_err={tunnel.get('parse_error', 0)}"
    )
    print(
        f"router: rx={router.get('rx', 0)} tx={router.get('tx', 0)} "
        f"drop={router.get('drop', 0)} queue_full={router.get('queue_full', 0)}"
    )
    lat = status.get("latency_us")
    if lat:
        u2c = lat.get("udp_to_can", {})
        c2u = lat.get("can_to_udp", {})
        loop = lat.get("loop", {})
        print(
            f"latency_us (board, DWT): udp->can avg={u2c.get('avg', 0)}/max={u2c.get('max', 0)} "
            f"can->udp avg={c2u.get('avg', 0)}/max={c2u.get('max', 0)} "
            f"loop avg={loop.get('avg', 0)}/max={loop.get('max', 0)} "
            f"(n={u2c.get('count', 0)}/{c2u.get('count', 0)})"
        )
    for can in status.get("can", []):
        ch = can.get("ch")
        cfg = config.get(ch, {})
        print(
            f"CAN{ch}: {'FD' if cfg.get('fd', can.get('fd')) else 'CLASSIC'} "
            f"{cfg.get('bitrate', 0)}/{cfg.get('data_bitrate', 0)} brs={'on' if cfg.get('brs') else 'off'} "
            f"state={can.get('state', '?')} rx={can.get('rx', 0)} tx_done={can.get('tx_done', 0)} "
            f"rxq={can.get('rx_queue', 0)}/{can.get('rx_capacity', 0)} "
            f"rx_drop={can.get('rx_drop', 0)} tx_drop={can.get('tx_drop', 0)} "
            f"ovf={can.get('rx_fifo_overflow', 0)} err={can.get('error', 0)}"
        )


# --- actions ---------------------------------------------------------------
def do_status(args: argparse.Namespace) -> None:
    status = get_status(args.board, args.control_port)
    if args.json:
        print(json.dumps(status, indent=2, sort_keys=True))
    else:
        print_status(status)


def do_config(args: argparse.Namespace) -> None:
    command: dict[str, object] = {"cmd": "set_can_config", "channel": args.channel}
    if args.enabled is not None:
        command["enabled"] = args.enabled
    if args.fd is not None:
        command["fd"] = args.fd
    if args.bitrate is not None:
        command["bitrate"] = args.bitrate
    if args.data_bitrate is not None:
        command["data_bitrate"] = args.data_bitrate
    if args.brs is not None:
        command["brs"] = args.brs
    if args.filter is not None:
        command["filter"] = args.filter
    if args.filter_id is not None:
        command["filter_id"] = int(args.filter_id, 0)
    if args.filter_mask is not None:
        command["filter_mask"] = int(args.filter_mask, 0)
    print(json.dumps(control(args.board, args.control_port, command), indent=2, sort_keys=True))


def do_reset(args: argparse.Namespace) -> None:
    control(args.board, args.control_port, {"cmd": "reset_stats"})
    print("stats reset.")


def do_send(args: argparse.Namespace) -> None:
    if args.id is None:
        raise SystemExit("error: --send requires --id (e.g. --id 0x123)")
    frame = make_frame(args.channel, int(args.id, 0), bool(args.fd), bool(args.brs),
                       parse_data(args.data), args.extended)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        for index in range(args.count):
            sock.sendto(pack_frames(index, [frame]), (args.board, args.data_port))
            print(f"tx {index + 1}/{args.count} {describe_frame(frame)}")
            if args.interval_ms and index + 1 < args.count:
                time.sleep(args.interval_ms / 1000.0)
    finally:
        sock.close()


def do_listen(args: argparse.Namespace) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", args.local_port))
    sock.sendto(pack_frames(0, []), (args.board, args.data_port))  # register the data session
    print(f"listening on UDP {sock.getsockname()[1]} (Ctrl-C to stop)...", file=sys.stderr)

    deadline = (time.monotonic() + args.timeout) if args.timeout is not None else None
    sock.settimeout(0.5)
    received = 0
    try:
        while (deadline is None) or (time.monotonic() < deadline):
            try:
                packet, source = sock.recvfrom(2048)
            except (socket.timeout, ConnectionResetError):
                continue
            try:
                for frame in GatewayPacket.unpack(packet).frames:
                    print(describe_frame(frame, source))
                    received += 1
            except ValueError as exc:
                print(f"drop {source[0]}:{source[1]} {exc}")
    finally:
        sock.close()
    if received == 0 and args.timeout is not None:
        print(f"no frames in {args.timeout:g}s.")


def do_pressure(args: argparse.Namespace) -> None:
    channels = args.channels or [args.channel]
    total = max(1, int(args.duration * args.rate))
    payload = bytes(index & 0xFF for index in range(args.dlc if args.fd else min(args.dlc, 8)))
    id_mask = 0x1FFFFFFF if args.extended else 0x7FF  # keep generated IDs in range; the board rejects out-of-range IDs

    # Reset first so every counter -- including the board DWT latency high-water max
    # (which only ever increases until a reset) -- reflects THIS run, not whatever
    # accumulated since the last reset. Best-effort: a transient control loss still
    # lets the run proceed (counts come from the before/after delta either way).
    try:
        control(args.board, args.control_port, {"cmd": "reset_stats"})
    except SystemExit:
        print("warn: reset_stats failed; latency max may be a stale high-water from earlier runs")

    before = get_status(args.board, args.control_port)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sent = 0
    start = time.monotonic()
    try:
        index = 0
        while index < total:
            # Pace by an accumulated deadline and send whole packets, instead of
            # sleeping once per frame (Windows' ~1ms timer would cap the rate).
            batch = []
            while (len(batch) < MAX_FRAMES_PER_PACKET) and (index < total):
                channel = channels[index % len(channels)]
                batch.append(make_frame(channel, (args.base_id + index) & id_mask, bool(args.fd), bool(args.brs),
                                        payload, args.extended))
                index += 1
            sock.sendto(pack_frames(sent, batch), (args.board, args.data_port))
            sent += 1
            if (args.rate > 0) and (index < total):
                slack = (start + index / args.rate) - time.monotonic()
                if slack > 0:
                    time.sleep(slack)
        elapsed = max(time.monotonic() - start, 1e-6)
        time.sleep(0.5)  # let the board's CAN TX queues drain before reading counters
        after = get_status(args.board, args.control_port)
    finally:
        sock.close()

    def delta(section: str, key: str) -> int:
        return int(after.get(section, {}).get(key, 0) or 0) - int(before.get(section, {}).get(key, 0) or 0)

    before_can = {item.get("ch"): item for item in before.get("can", [])}
    after_can = {item.get("ch"): item for item in after.get("can", [])}

    def cdelta(ch: int, key: str) -> int:
        return int(after_can.get(ch, {}).get(key, 0) or 0) - int(before_can.get(ch, {}).get(key, 0) or 0)

    rx_frames = delta("tunnel", "rx_frames")
    loss = delta("tunnel", "loss")
    parse_error = delta("tunnel", "parse_error")
    queue_full = delta("router", "queue_full")
    router_reject = delta("router", "drop") - queue_full  # router rejects that are NOT tx-queue saturation

    # Frames actually transmitted AND ACKed on the wire by the sender(s). The rest
    # were dropped at the TX queue because we sent faster than the bus could carry
    # (saturation / source over-rate) -- that is NOT delivery loss.
    tx_on_bus = sum(cdelta(ch, "tx_done") for ch in channels)
    over_rate = max(0, total - tx_on_bus)
    saturated = (queue_full > 0) or (over_rate > 0)

    print(
        f"sent={total} on_bus={tx_on_bus} ({total / elapsed:.0f} fps over {elapsed:.1f}s)"
        f"{' SATURATED' if saturated else ''} -> rx_frames+={rx_frames} loss+={loss} "
        f"parse_err+={parse_error} router.drop+={delta('router', 'drop')} queue_full+={queue_full}"
    )
    if saturated:
        print(f"  note: {over_rate} frames over bus capacity (source rate too high) -- not delivery loss")

    # Per-channel delta breakdown for every channel the board reports.
    send_set = set(channels)
    watch_set = set(args.rx_watch or [])
    issues = []
    for ch in sorted(after_can.keys()):
        state = after_can[ch].get("state", "?")
        d_err = cdelta(ch, "error")
        d_rx = cdelta(ch, "rx")
        # A non-sender channel that received SOME frames but fewer than reached the
        # bus is a receiver that lost frames (e.g. its cable was pulled mid-test).
        # rx==0 channels are senders / disabled / not on this bus, so are not flagged.
        rx_short = (tx_on_bus - d_rx) if ((ch not in send_set) and (0 < d_rx < tx_on_bus)) else 0
        note = f"  <- RX LOST {rx_short}/{tx_on_bus}" if rx_short else ""
        print(
            f"  CAN{ch}: tx_done+={cdelta(ch, 'tx_done')} rx+={d_rx} "
            f"rx_drop+={cdelta(ch, 'rx_drop')} tx_drop+={cdelta(ch, 'tx_drop')} "
            f"ovf+={cdelta(ch, 'rx_fifo_overflow')} err+={d_err} state={state}{note}"
        )
        if d_err or state == "bus-off":
            issues.append(f"CAN{ch} fault(err+={d_err} state={state})")
        if rx_short and (ch not in watch_set):  # watch channels are checked in the rx-watch loop
            issues.append(f"CAN{ch} rx lost {rx_short}/{tx_on_bus}")

    # Real CAN delivery: each watched receiver must get every frame that actually
    # reached the wire (rx == on_bus). Compared to on_bus, NOT sent, so saturation
    # is not mislabeled as loss. ovf only flags an overrun (lower bound) -- trust
    # the rx shortfall, which catches even silent 5-deep RX bank overruns.
    can_loss = False
    for ch in (args.rx_watch or []):
        info = after_can.get(ch, {})
        if not info.get("enabled", True):
            print(f"  rx-watch CAN{ch}: DISABLED -- enable it or drop it from --rx-watch")
            continue
        got, d_ovf, d_rx_drop = cdelta(ch, "rx"), cdelta(ch, "rx_fifo_overflow"), cdelta(ch, "rx_drop")
        lost = tx_on_bus - got
        print(f"  rx-watch CAN{ch}: rx+={got}/{tx_on_bus} on-bus ovf+={d_ovf} ({'ok' if lost <= 0 else f'LOST {lost}'})")
        if (lost > 0) or d_ovf or d_rx_drop or info.get("state") == "bus-off":
            can_loss = True

    # Board-internal latency (DWT ground truth, host-clock free) for THIS run, since
    # the run reset stats at the start. udp->can and can->udp are the forwarding legs;
    # loop is the super-loop period that bounds the wait before a frame is serviced.
    board_lat = after.get("latency_us")
    if board_lat:
        u2c = board_lat.get("udp_to_can", {})
        c2u = board_lat.get("can_to_udp", {})
        loop = board_lat.get("loop", {})
        print(
            f"  board DWT us (this run): udp->can avg={u2c.get('avg', 0)}/max={u2c.get('max', 0)} "
            f"can->udp avg={c2u.get('avg', 0)}/max={c2u.get('max', 0)} "
            f"loop avg={loop.get('avg', 0)}/max={loop.get('max', 0)}"
        )

    # Saturation (TX queue_full, or frames that never reached the bus) means the board
    # could not sustain this rate losslessly -- the acceptance criteria require
    # queue_full == 0, so a SATURATED run FAILS (lower --rate to find the limit).
    passed = ((rx_frames >= total) and (loss == 0) and (parse_error == 0)
              and (router_reject == 0) and not saturated and not issues and not can_loss)
    if issues:
        print("channel issues: " + "; ".join(issues))
    print("PASS" if passed else "FAIL")
    if args.json:
        print(json.dumps(after, indent=2, sort_keys=True))


ACTIONS = (
    ("status", do_status),
    ("config", do_config),
    ("send", do_send),
    ("listen", do_listen),
    ("pressure", do_pressure),
    ("reset_stats", do_reset),
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--board", required=True, help="board IPv4 address (from the serial DHCP log)")
    parser.add_argument("--data-port", type=int, default=DATA_PORT, help="board UDP data port")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT, help="board UDP control port")
    parser.add_argument("--local-port", type=int, default=0, help="local UDP port for --listen (0 = auto)")

    # actions (choose exactly one)
    parser.add_argument("--status", action="store_true", help="query and print board status")
    parser.add_argument("--config", action="store_true", help="set a channel's CAN config")
    parser.add_argument("--send", action="store_true", help="send frame(s) to --channel")
    parser.add_argument("--listen", action="store_true", help="print frames forwarded from CAN")
    parser.add_argument("--pressure", action="store_true", help="sustained load test")
    parser.add_argument("--reset-stats", action="store_true", help="clear all board counters")

    # shared
    parser.add_argument("--channel", type=int, default=0, help="CAN channel 0..5")
    parser.add_argument("--json", action="store_true", help="print raw JSON (--status / --pressure)")
    parser.add_argument("--timeout", type=float, default=None, help="--listen timeout seconds (default: forever)")

    # frame / config options
    parser.add_argument("--id", help="CAN ID for --send, e.g. 0x123")
    parser.add_argument("--data", default="", help='hex payload, e.g. "11 22 33 44"')
    parser.add_argument("--fd", action=argparse.BooleanOptionalAction, default=None,
                        help="CAN FD frame (--send/--pressure) or set channel FD (--config)")
    parser.add_argument("--brs", action=argparse.BooleanOptionalAction, default=None,
                        help="BRS bit (--send/--pressure) or set channel BRS (--config)")
    parser.add_argument("--extended", action="store_true", help="extended (29-bit) CAN ID")
    parser.add_argument("--count", type=int, default=1, help="--send frame count")
    parser.add_argument("--interval-ms", type=int, default=100, help="--send gap between frames")
    parser.add_argument("--enabled", action=argparse.BooleanOptionalAction, default=None,
                        help="--config: enable/disable the channel")
    parser.add_argument("--bitrate", type=int, default=None, help="--config nominal bitrate")
    parser.add_argument("--data-bitrate", type=int, default=None, help="--config CAN FD data bitrate")
    parser.add_argument("--filter", choices=("accept_all", "id_mask"), help="--config software RX filter mode")
    parser.add_argument("--filter-id", help="--config filter ID")
    parser.add_argument("--filter-mask", help="--config filter mask")

    # pressure
    parser.add_argument("--channels", nargs="+", type=int, help="--pressure channels, round-robin, e.g. 0 1 2")
    parser.add_argument("--duration", type=float, default=10.0, help="--pressure seconds")
    parser.add_argument("--rate", type=float, default=1000.0,
                        help="--pressure AGGREGATE frames/second across all --channels (round-robin); "
                             "per-channel rate = rate / channel-count, so N channels at R fps each need --rate N*R")
    parser.add_argument("--dlc", type=int, default=64, help="--pressure payload length")
    parser.add_argument("--base-id", type=lambda value: int(value, 0), default=0x100, help="--pressure base CAN ID")
    parser.add_argument("--rx-watch", nargs="+", type=int, default=None,
                        help="--pressure: channels expected to RECEIVE the frames over a shared CAN bus; "
                             "fails if rx < sent (real CAN loss) or the RX bank overran. Omit to report only.")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    chosen = [handler for name, handler in ACTIONS if getattr(args, name)]
    if len(chosen) != 1:
        raise SystemExit("error: choose exactly one of --status --config --send --listen --pressure --reset-stats")
    try:
        chosen[0](args)
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
