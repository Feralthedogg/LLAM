#!/usr/bin/env python3
"""Stress the LLAM chat server with real TCP clients."""

from __future__ import annotations

import argparse
import os
import random
import signal
import socket
import string
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class ClientState:
    index: int
    sock: socket.socket
    buffer: bytearray = field(default_factory=bytearray)
    error: BaseException | None = None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="./server", help="path to the server binary")
    parser.add_argument("--host", default="127.0.0.1", help="server bind/connect host")
    parser.add_argument("--clients", type=int, default=int(os.getenv("LLAM_SERVER_STRESS_CLIENTS", "16")))
    parser.add_argument("--messages", type=int, default=int(os.getenv("LLAM_SERVER_STRESS_MESSAGES", "8")))
    parser.add_argument(
        "--payload-bytes",
        type=int,
        default=int(os.getenv("LLAM_SERVER_STRESS_PAYLOAD_BYTES", "48")),
        help="random bytes appended to each line payload",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=float(os.getenv("LLAM_SERVER_STRESS_TIMEOUT", "20")),
        help="seconds to wait for all expected broadcasts",
    )
    parser.add_argument(
        "--connect-spread-ms",
        type=float,
        default=float(os.getenv("LLAM_SERVER_STRESS_CONNECT_SPREAD_MS", "5")),
        help="delay between client connects to avoid testing only listen backlog behavior",
    )
    return parser.parse_args()


def find_free_port(host: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def drain_stream(stream, lines: list[str], stop: threading.Event) -> None:
    try:
        while not stop.is_set():
            line = stream.readline()
            if line == "":
                return
            lines.append(line.rstrip("\n"))
    except Exception as exc:  # pragma: no cover - best-effort diagnostic drain
        lines.append(f"<drain error: {exc}>")


def connect_client(host: str, port: int, deadline: float) -> socket.socket:
    last_error: OSError | None = None
    while time.monotonic() < deadline:
        try:
            sock = socket.create_connection((host, port), timeout=0.5)
            sock.settimeout(0.1)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.025)
    raise TimeoutError(f"could not connect to {host}:{port}: {last_error}")


def reader_loop(client: ClientState, stop: threading.Event) -> None:
    while not stop.is_set():
        try:
            chunk = client.sock.recv(4096)
        except socket.timeout:
            continue
        except OSError as exc:
            client.error = exc
            return
        if not chunk:
            return
        client.buffer.extend(chunk)


def random_token(length: int) -> str:
    alphabet = string.ascii_letters + string.digits
    return "".join(random.choice(alphabet) for _ in range(length))


def count_payloads(clients: list[ClientState], payloads: list[bytes]) -> dict[bytes, int]:
    snapshots = [bytes(client.buffer) for client in clients]
    return {payload: sum(snapshot.count(payload) for snapshot in snapshots) for payload in payloads}


def wait_for_broadcasts(clients: list[ClientState], payloads: list[bytes], expected_each: int, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    counts: dict[bytes, int] = {}

    while time.monotonic() < deadline:
        counts = count_payloads(clients, payloads)
        missing = [payload for payload, count in counts.items() if count < expected_each]
        if not missing:
            return
        time.sleep(0.05)

    missing = [(payload.decode("utf-8", "replace"), counts.get(payload, 0)) for payload in payloads if counts.get(payload, 0) < expected_each]
    preview = ", ".join(f"{payload!r}:{count}/{expected_each}" for payload, count in missing[:8])
    raise AssertionError(f"missing broadcasts: {preview}")


def wait_for_welcome(clients: list[ClientState], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    marker = b"Welcome to LLAM chat. Type and press enter."
    missing: list[int] = []

    while time.monotonic() < deadline:
        missing = [client.index for client in clients if marker not in bytes(client.buffer)]
        if not missing:
            return
        time.sleep(0.01)

    raise AssertionError(f"clients did not receive welcome before payload phase: {missing[:8]}")


def close_clients(clients: list[ClientState]) -> None:
    for client in clients:
        try:
            client.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        client.sock.close()


def main() -> int:
    args = parse_args()
    server_path = Path(args.server)

    if args.clients < 2:
        raise SystemExit("--clients must be >= 2")
    if args.messages < 1:
        raise SystemExit("--messages must be >= 1")
    if not server_path.exists():
        raise SystemExit(f"server binary not found: {server_path}")

    port = find_free_port(args.host)
    stop_readers = threading.Event()
    stop_drains = threading.Event()
    server_stdout: list[str] = []
    server_stderr: list[str] = []
    clients: list[ClientState] = []
    reader_threads: list[threading.Thread] = []

    proc = subprocess.Popen(
        [str(server_path.resolve()), str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    stdout_thread = threading.Thread(target=drain_stream, args=(proc.stdout, server_stdout, stop_drains), daemon=True)
    stderr_thread = threading.Thread(target=drain_stream, args=(proc.stderr, server_stderr, stop_drains), daemon=True)
    stdout_thread.start()
    stderr_thread.start()

    started_at = time.monotonic()
    payloads: list[bytes] = []
    try:
        connect_deadline = time.monotonic() + args.timeout
        for index in range(args.clients):
            sock = connect_client(args.host, port, connect_deadline)
            client = ClientState(index=index, sock=sock)
            clients.append(client)
            thread = threading.Thread(target=reader_loop, args=(client, stop_readers), daemon=True)
            thread.start()
            reader_threads.append(thread)
            if args.connect_spread_ms > 0:
                time.sleep(args.connect_spread_ms / 1000.0)

        # Wait until every connection has passed the server accept/registration
        # path.  Otherwise a heavily loaded CI runner can send payloads before
        # the last sockets are visible to broadcast fanout, creating false
        # correctness failures.
        wait_for_welcome(clients, args.timeout)

        for round_index in range(args.messages):
            for client in clients:
                token = random_token(args.payload_bytes)
                payload = f"stress:{client.index}:{round_index}:{token}"
                line = (payload + "\n").encode("utf-8")
                client.sock.sendall(line)
                payloads.append(payload.encode("utf-8"))

        wait_for_broadcasts(clients, payloads, expected_each=args.clients - 1, timeout=args.timeout)
        elapsed = time.monotonic() - started_at
        total_messages = len(payloads)
        total_expected_deliveries = total_messages * (args.clients - 1)
        total_received_bytes = sum(len(client.buffer) for client in clients)
        print(
            "server stress ok: "
            f"clients={args.clients} messages={total_messages} "
            f"expected_deliveries={total_expected_deliveries} "
            f"received_bytes={total_received_bytes} elapsed={elapsed:.3f}s"
        )
        return 0
    finally:
        close_clients(clients)
        stop_readers.set()
        for thread in reader_threads:
            thread.join(timeout=1.0)
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        stop_drains.set()
        stdout_thread.join(timeout=1.0)
        stderr_thread.join(timeout=1.0)
        if proc.returncode not in (0, None):
            print(f"server exited with rc={proc.returncode}", file=sys.stderr)
            if server_stdout:
                print("server stdout tail:", file=sys.stderr)
                print("\n".join(server_stdout[-12:]), file=sys.stderr)
            if server_stderr:
                print("server stderr tail:", file=sys.stderr)
                print("\n".join(server_stderr[-12:]), file=sys.stderr)
            raise SystemExit(1)


if __name__ == "__main__":
    raise SystemExit(main())
