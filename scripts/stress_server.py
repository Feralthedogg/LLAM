#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Stress the LLAM chat server with real TCP clients."""

from __future__ import annotations

import argparse
import os
import random
import socket
import string
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from cli_numbers import (
    env_default,
    finite_nonnegative_float_at_most,
    finite_positive_float_at_most,
    integer,
    nonnegative_int_at_most,
    positive_int_at_most,
)
from process_utils import interrupt_process_tree, kill_process_tree


MAX_CLIENTS = 4096
MAX_MESSAGES = 1_000_000
MAX_PAYLOAD_BYTES = 1 << 20
MAX_STRESS_TIMEOUT_SEC = 3600.0
MAX_CONNECT_SPREAD_MS = 1000.0


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
    parser.add_argument("--clients",
                        type=positive_int_at_most(MAX_CLIENTS),
                        default=env_default(parser,
                                            "LLAM_SERVER_STRESS_CLIENTS",
                                            "16",
                                            positive_int_at_most(MAX_CLIENTS)))
    parser.add_argument("--messages",
                        type=positive_int_at_most(MAX_MESSAGES),
                        default=env_default(parser,
                                            "LLAM_SERVER_STRESS_MESSAGES",
                                            "8",
                                            positive_int_at_most(MAX_MESSAGES)))
    parser.add_argument(
        "--payload-bytes",
        type=nonnegative_int_at_most(MAX_PAYLOAD_BYTES),
        default=env_default(parser,
                            "LLAM_SERVER_STRESS_PAYLOAD_BYTES",
                            "48",
                            nonnegative_int_at_most(MAX_PAYLOAD_BYTES)),
        help="random bytes appended to each line payload",
    )
    parser.add_argument(
        "--timeout",
        type=finite_positive_float_at_most(MAX_STRESS_TIMEOUT_SEC),
        default=env_default(parser,
                            "LLAM_SERVER_STRESS_TIMEOUT",
                            "20",
                            finite_positive_float_at_most(MAX_STRESS_TIMEOUT_SEC)),
        help="seconds to wait for all expected broadcasts",
    )
    parser.add_argument(
        "--connect-spread-ms",
        type=finite_nonnegative_float_at_most(MAX_CONNECT_SPREAD_MS),
        default=env_default(parser,
                            "LLAM_SERVER_STRESS_CONNECT_SPREAD_MS",
                            "5",
                            finite_nonnegative_float_at_most(MAX_CONNECT_SPREAD_MS)),
        help="delay between client connects to avoid testing only listen backlog behavior",
    )
    parser.add_argument(
        "--seed",
        type=integer,
        default=None
        if not os.getenv("LLAM_SERVER_STRESS_SEED")
        else env_default(parser, "LLAM_SERVER_STRESS_SEED", "0", integer),
        help="random seed for deterministic payload generation",
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


def count_payloads_by_client(clients: list[ClientState], payloads: list[tuple[int, bytes]]) -> dict[tuple[int, bytes], dict[int, int]]:
    snapshots = {client.index: bytes(client.buffer) for client in clients}
    return {
        payload_record: {
            client.index: snapshots[client.index].count(payload_record[1])
            for client in clients
        }
        for payload_record in payloads
    }


def wait_for_broadcasts(clients: list[ClientState], payloads: list[tuple[int, bytes]], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    counts: dict[tuple[int, bytes], dict[int, int]] = {}

    while time.monotonic() < deadline:
        counts = count_payloads_by_client(clients, payloads)
        missing = [
            (sender_index, payload, client.index, client_counts.get(client.index, 0))
            for (sender_index, payload), client_counts in counts.items()
            for client in clients
            if client.index != sender_index and client_counts.get(client.index, 0) != 1
        ]
        self_echo = [
            (sender_index, payload, client_counts.get(sender_index, 0))
            for (sender_index, payload), client_counts in counts.items()
            if client_counts.get(sender_index, 0) != 0
        ]
        if not missing and not self_echo:
            return
        time.sleep(0.05)

    missing = [
        (sender_index, payload.decode("utf-8", "replace"), client.index, client_counts.get(client.index, 0))
        for (sender_index, payload), client_counts in counts.items()
        for client in clients
        if client.index != sender_index and client_counts.get(client.index, 0) != 1
    ]
    self_echo = [
        (sender_index, payload.decode("utf-8", "replace"), client_counts.get(sender_index, 0))
        for (sender_index, payload), client_counts in counts.items()
        if client_counts.get(sender_index, 0) != 0
    ]
    preview = ", ".join(
        f"{payload!r}:sender={sender_index}->client={client_index} count={count}/1"
        for sender_index, payload, client_index, count in missing[:8]
    )
    if self_echo:
        echo_preview = ", ".join(
            f"{payload!r}:sender={sender_index} self_count={count}"
            for sender_index, payload, count in self_echo[:8]
        )
        preview = f"{preview}; self_echo={echo_preview}" if preview else f"self_echo={echo_preview}"
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

    if args.seed is None:
        args.seed = random.SystemRandom().randrange(1, 2**63)
    random.seed(args.seed)

    if args.clients < 2:
        raise SystemExit("--clients must be >= 2")
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
        start_new_session=(os.name != "nt"),
    )
    stdout_thread = threading.Thread(target=drain_stream, args=(proc.stdout, server_stdout, stop_drains), daemon=True)
    stderr_thread = threading.Thread(target=drain_stream, args=(proc.stderr, server_stderr, stop_drains), daemon=True)
    stdout_thread.start()
    stderr_thread.start()

    started_at = time.monotonic()
    payloads: list[tuple[int, bytes]] = []
    try:
        connect_deadline = time.monotonic() + args.timeout
        for index in range(args.clients):
            sock = connect_client(args.host, port, connect_deadline)
            client = ClientState(index=index, sock=sock)
            clients.append(client)
            thread = threading.Thread(target=reader_loop, args=(client, stop_readers), daemon=True)
            thread.start()
            reader_threads.append(thread)
            # TCP connect can complete while the socket is still queued in the
            # kernel listen backlog.  The correctness phase wants registered
            # chat clients, not a backlog throughput race, so pace on the
            # server welcome for each new socket before opening more clients.
            wait_for_welcome([client], max(0.1, connect_deadline - time.monotonic()))
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
                payloads.append((client.index, payload.encode("utf-8")))

        wait_for_broadcasts(clients, payloads, timeout=args.timeout)
        elapsed = time.monotonic() - started_at
        total_messages = len(payloads)
        total_expected_deliveries = total_messages * (args.clients - 1)
        total_received_bytes = sum(len(client.buffer) for client in clients)
        print(
            "server stress ok: "
            f"clients={args.clients} messages={total_messages} "
            f"expected_deliveries={total_expected_deliveries} "
            f"received_bytes={total_received_bytes} elapsed={elapsed:.3f}s "
            f"seed={args.seed}"
        )
        return 0
    finally:
        close_clients(clients)
        stop_readers.set()
        for thread in reader_threads:
            thread.join(timeout=1.0)
        if proc.poll() is None:
            interrupt_process_tree(proc)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                kill_process_tree(proc)
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
