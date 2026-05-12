#!/usr/bin/env python3
"""Run a composite LLAM chat-server stress suite."""

from __future__ import annotations

import argparse
import os
import random
import shutil
import signal
import socket
import string
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path


TAIL_LIMIT = 200


@dataclass
class ResourceSample:
    elapsed_sec: float
    rss_kb: int | None
    fd_count: int | None


@dataclass
class ResourceSummary:
    samples: int
    first_rss_kb: int | None
    last_rss_kb: int | None
    min_rss_kb: int | None
    max_rss_kb: int | None
    first_fds: int | None
    last_fds: int | None
    min_fds: int | None
    max_fds: int | None


@dataclass
class RunningServer:
    proc: subprocess.Popen[str]
    port: int
    stdout_tail: list[str] = field(default_factory=list)
    stderr_tail: list[str] = field(default_factory=list)
    stop_drains: threading.Event = field(default_factory=threading.Event)
    drain_threads: list[threading.Thread] = field(default_factory=list)


@dataclass
class EdgeStats:
    sent_lines: int = 0
    recv_lines: int = 0
    recv_bytes: int = 0
    churn_connects: int = 0
    half_closes: int = 0
    resets: int = 0
    slow_clients: int = 0
    client_errors: int = 0


class ResourceSampler:
    def __init__(self, pid: int, interval_sec: float) -> None:
        self.pid = pid
        self.interval_sec = interval_sec
        self.started = time.monotonic()
        self.stop_event = threading.Event()
        self.samples: list[ResourceSample] = []
        self.thread = threading.Thread(target=self._run, name="resource-sampler", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> ResourceSummary:
        self.stop_event.set()
        self.thread.join(timeout=2.0)
        return self.summary()

    def summary(self) -> ResourceSummary:
        rss_values = [sample.rss_kb for sample in self.samples if sample.rss_kb is not None]
        fd_values = [sample.fd_count for sample in self.samples if sample.fd_count is not None]
        first_rss = next((sample.rss_kb for sample in self.samples if sample.rss_kb is not None), None)
        last_rss = next((sample.rss_kb for sample in reversed(self.samples) if sample.rss_kb is not None), None)
        first_fds = next((sample.fd_count for sample in self.samples if sample.fd_count is not None), None)
        last_fds = next((sample.fd_count for sample in reversed(self.samples) if sample.fd_count is not None), None)

        return ResourceSummary(
            samples=len(self.samples),
            first_rss_kb=first_rss,
            last_rss_kb=last_rss,
            min_rss_kb=min(rss_values) if rss_values else None,
            max_rss_kb=max(rss_values) if rss_values else None,
            first_fds=first_fds,
            last_fds=last_fds,
            min_fds=min(fd_values) if fd_values else None,
            max_fds=max(fd_values) if fd_values else None,
        )

    def _run(self) -> None:
        while not self.stop_event.is_set():
            self.samples.append(
                ResourceSample(
                    elapsed_sec=time.monotonic() - self.started,
                    rss_kb=read_rss_kb(self.pid),
                    fd_count=count_fds(self.pid),
                )
            )
            self.stop_event.wait(self.interval_sec)


def append_tail(lines: list[str], line: str) -> None:
    lines.append(line.rstrip("\n"))
    if len(lines) > TAIL_LIMIT:
        del lines[: len(lines) - TAIL_LIMIT]


def drain_stream(stream, lines: list[str], stop: threading.Event) -> None:
    try:
        while not stop.is_set():
            line = stream.readline()
            if line == "":
                return
            append_tail(lines, line)
    except Exception as exc:  # pragma: no cover - best-effort diagnostics
        append_tail(lines, f"<drain error: {exc}>")


def find_free_port(host: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def connect_client(host: str, port: int, deadline_sec: float, timeout_sec: float = 0.5) -> socket.socket:
    last_error: OSError | None = None
    while time.monotonic() < deadline_sec:
        try:
            sock = socket.create_connection((host, port), timeout=timeout_sec)
            sock.settimeout(0.1)
            return sock
        except OSError as exc:
            last_error = exc
            time.sleep(0.02)
    raise TimeoutError(f"could not connect to {host}:{port}: {last_error}")


def start_server(server_path: Path, host: str, timeout_sec: float) -> RunningServer:
    port = find_free_port(host)
    env = os.environ.copy()

    env.setdefault("LLAM_CHAT_QUIET", "1")
    proc = subprocess.Popen(
        [str(server_path.resolve()), str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
        env=env,
    )
    server = RunningServer(proc=proc, port=port)
    if proc.stdout is not None:
        thread = threading.Thread(target=drain_stream, args=(proc.stdout, server.stdout_tail, server.stop_drains), daemon=True)
        thread.start()
        server.drain_threads.append(thread)
    if proc.stderr is not None:
        thread = threading.Thread(target=drain_stream, args=(proc.stderr, server.stderr_tail, server.stop_drains), daemon=True)
        thread.start()
        server.drain_threads.append(thread)

    deadline = time.monotonic() + timeout_sec
    probe: socket.socket | None = None
    try:
        probe = connect_client(host, port, deadline)
    except Exception:
        stop_server(server)
        raise
    finally:
        if probe is not None:
            probe.close()
    return server


def stop_server(server: RunningServer, timeout_sec: float = 30.0) -> bool:
    killed = False

    if server.proc.poll() is None:
        server.proc.send_signal(signal.SIGINT)
        try:
            server.proc.wait(timeout=timeout_sec)
        except subprocess.TimeoutExpired:
            server.proc.kill()
            server.proc.wait(timeout=5.0)
            killed = True
    server.stop_drains.set()
    for thread in server.drain_threads:
        thread.join(timeout=1.0)
    return killed


def ensure_server_alive(server: RunningServer) -> None:
    rc = server.proc.poll()
    if rc is None:
        return
    details = [f"server exited early with rc={rc}"]
    if server.stdout_tail:
        details.append("stdout tail:\n" + "\n".join(server.stdout_tail[-20:]))
    if server.stderr_tail:
        details.append("stderr tail:\n" + "\n".join(server.stderr_tail[-20:]))
    raise RuntimeError("\n".join(details))


def server_tail_details(server: RunningServer) -> str:
    details: list[str] = []

    if server.stdout_tail:
        details.append("stdout tail:\n" + "\n".join(server.stdout_tail[-20:]))
    if server.stderr_tail:
        details.append("stderr tail:\n" + "\n".join(server.stderr_tail[-20:]))
    return "\n".join(details)


def read_rss_kb(pid: int) -> int | None:
    try:
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=1.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    text = result.stdout.strip()
    if result.returncode != 0 or not text:
        return None
    try:
        return int(text.splitlines()[-1].strip())
    except ValueError:
        return None


def count_fds(pid: int) -> int | None:
    proc_fd = Path(f"/proc/{pid}/fd")
    if proc_fd.exists():
        try:
            return len(list(proc_fd.iterdir()))
        except OSError:
            return None
    if shutil.which("lsof") is None:
        return None
    try:
        result = subprocess.run(
            ["lsof", "-n", "-p", str(pid)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2.0,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if result.returncode != 0:
        return None
    lines = [line for line in result.stdout.splitlines() if line]
    return max(0, len(lines) - 1)


def run_checked(cmd: list[str], label: str) -> None:
    print(f"[{label}] {' '.join(cmd)}", flush=True)
    started = time.monotonic()
    proc = subprocess.run(cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    elapsed = time.monotonic() - started
    if proc.stdout:
        print(proc.stdout.rstrip())
    if proc.stderr:
        print(proc.stderr.rstrip(), file=sys.stderr)
    if proc.returncode != 0:
        raise RuntimeError(f"{label} failed with rc={proc.returncode}")
    print(f"[{label}] ok elapsed={elapsed:.3f}s", flush=True)


def parse_correctness_matrix(value: str) -> list[tuple[int, int, int]]:
    cases: list[tuple[int, int, int]] = []
    for raw_case in value.split(","):
        raw_case = raw_case.strip()
        if not raw_case:
            continue
        parts = raw_case.split(":")
        if len(parts) != 3:
            raise ValueError(f"bad correctness case {raw_case!r}; expected clients:messages:payload_bytes")
        clients, messages, payload_bytes = (int(part) for part in parts)
        cases.append((clients, messages, payload_bytes))
    if not cases:
        raise ValueError("correctness matrix is empty")
    return cases


def phase_correctness(args: argparse.Namespace, script_path: Path) -> None:
    for clients, messages, payload_bytes in parse_correctness_matrix(args.correctness_matrix):
        run_checked(
            [
                sys.executable,
                str(script_path),
                "--server",
                str(args.server),
                "--host",
                args.host,
                "--clients",
                str(clients),
                "--messages",
                str(messages),
                "--payload-bytes",
                str(payload_bytes),
                "--timeout",
                str(args.correctness_timeout),
            ],
            f"correctness clients={clients} payload={payload_bytes}",
        )


def phase_flood(args: argparse.Namespace) -> None:
    lossless_duration = 2.0 if args.quick else 5.0
    if args.quick:
        # Hosted CI runners vary heavily; quick mode is a smoke/stability
        # profile, not a throughput benchmark. The standard/soak profiles keep
        # the high-rate best-effort flood.
        cases = [
            ("lossless-8b", 8, lossless_duration, 8, 32, 0.02, 0.05, 0.999),
            ("throughput-8b", 8, args.flood_duration, 8, 32, 0.08, 0.10, 0.0),
            ("throughput-64b", 8, args.payload_flood_duration, 64, 32, 0.05, 0.08, 0.0),
            ("lossless-1kb", 8, args.payload_flood_duration, 1024, 16, 0.02, 0.05, 0.999),
        ]
    else:
        cases = [
            ("lossless-8b", 8, lossless_duration, 8, 32, 0.02, 0.05, 0.999),
            ("throughput-8b", 16, args.flood_duration, 8, 64, 0.30, 1.3, 0.0),
            ("throughput-64b", 16, args.payload_flood_duration, 64, 64, 0.15, 1.0, 0.0),
            ("lossless-1kb", 8, args.payload_flood_duration, 1024, 16, 0.02, 0.05, 0.999),
        ]
    for label, clients, duration, payload_bytes, batch, target_mps, min_delivery_mps, case_min_ratio in cases:
        min_delivery_ratio = args.flood_min_delivery_ratio if args.flood_min_delivery_ratio > 0.0 else case_min_ratio
        cmd = [
            str(args.server_flood),
            "--server",
            str(args.server),
            "--host",
            args.host,
            "--clients",
            str(clients),
            "--duration",
            str(duration),
            "--message-bytes",
            str(payload_bytes),
            "--batch",
            str(batch),
            "--target-mps",
            f"{target_mps:.3f}",
            "--min-delivery-mps",
            f"{min_delivery_mps:.3f}",
            "--min-delivery-ratio",
            f"{min_delivery_ratio:.9f}",
            "--shutdown-timeout",
            f"{args.shutdown_timeout:.3f}",
        ]
        if args.allow_flood_forced_stop:
            cmd.append("--allow-forced-stop")
        else:
            cmd.append("--fail-on-forced-stop")
        run_checked(cmd, f"flood {label}")


def random_payload(prefix: str, payload_bytes: int) -> bytes:
    alphabet = string.ascii_letters + string.digits
    suffix_len = max(1, payload_bytes)
    suffix = "".join(random.choice(alphabet) for _ in range(suffix_len))
    return f"{prefix}:{suffix}\n".encode("utf-8")


def send_fragmented(sock: socket.socket, payload: bytes) -> None:
    cursor = 0
    while cursor < len(payload):
        step = random.randint(1, min(7, len(payload) - cursor))
        sock.sendall(payload[cursor : cursor + step])
        cursor += step
        if random.random() < 0.25:
            time.sleep(0.0005)


def reset_socket(sock: socket.socket) -> None:
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    except OSError:
        pass
    sock.close()


def reader_loop(sock: socket.socket, stop_event: threading.Event, stats: EdgeStats, lock: threading.Lock, slow: bool) -> None:
    local_lines = 0
    local_bytes = 0
    while not stop_event.is_set():
        try:
            chunk = sock.recv(4096 if not slow else 128)
        except socket.timeout:
            continue
        except OSError:
            break
        if not chunk:
            break
        local_bytes += len(chunk)
        local_lines += chunk.count(b"\n")
        if slow:
            time.sleep(0.02)
    with lock:
        stats.recv_lines += local_lines
        stats.recv_bytes += local_bytes


def stable_sender_loop(
    index: int,
    sock: socket.socket,
    end_time: float,
    stop_event: threading.Event,
    stats: EdgeStats,
    lock: threading.Lock,
) -> None:
    sent = 0
    payload_sizes = [8, 64, 1024]
    while time.monotonic() < end_time and not stop_event.is_set():
        payload = random_payload(f"stable:{index}:{sent}", random.choice(payload_sizes))
        try:
            if sent % 7 == 0:
                send_fragmented(sock, payload)
            else:
                sock.sendall(payload)
            sent += 1
        except OSError:
            with lock:
                stats.client_errors += 1
            return
        if sent % 16 == 0:
            time.sleep(0.001)
    with lock:
        stats.sent_lines += sent


def churn_loop(
    host: str,
    port: int,
    end_time: float,
    stop_event: threading.Event,
    stats: EdgeStats,
    lock: threading.Lock,
) -> None:
    sequence = 0
    while time.monotonic() < end_time and not stop_event.is_set():
        sock: socket.socket | None = None
        try:
            sock = connect_client(host, port, time.monotonic() + 2.0, timeout_sec=0.25)
            payload = random_payload(f"churn:{sequence}", random.choice([8, 64, 1024]))
            if sequence % 3 == 0:
                send_fragmented(sock, payload)
            else:
                sock.sendall(payload)
            with lock:
                stats.churn_connects += 1
                stats.sent_lines += 1
            if sequence % 5 == 0:
                try:
                    sock.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                with lock:
                    stats.half_closes += 1
                time.sleep(0.005)
                sock.close()
            elif sequence % 7 == 0:
                reset_socket(sock)
                with lock:
                    stats.resets += 1
            else:
                sock.close()
        except OSError:
            with lock:
                stats.client_errors += 1
            if sock is not None:
                sock.close()
        sequence += 1
        time.sleep(0.001)


def slow_client_loop(
    host: str,
    port: int,
    end_time: float,
    stop_event: threading.Event,
    stats: EdgeStats,
    lock: threading.Lock,
) -> None:
    sequence = 0
    while time.monotonic() < end_time and not stop_event.is_set():
        sock: socket.socket | None = None
        try:
            sock = connect_client(host, port, time.monotonic() + 2.0, timeout_sec=0.25)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.sendall(random_payload(f"slow:{sequence}", 64))
            with lock:
                stats.slow_clients += 1
                stats.sent_lines += 1
            time.sleep(0.25)
            if sequence % 4 == 0:
                reset_socket(sock)
                with lock:
                    stats.resets += 1
            else:
                sock.close()
        except OSError:
            with lock:
                stats.client_errors += 1
            if sock is not None:
                sock.close()
        sequence += 1


def close_sockets(sockets: list[socket.socket]) -> None:
    for sock in sockets:
        try:
            sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        sock.close()


def phase_edge(args: argparse.Namespace) -> None:
    print(f"[edge] start duration={args.edge_duration}s", flush=True)
    server = start_server(Path(args.server), args.host, args.correctness_timeout)
    sampler = ResourceSampler(server.proc.pid, args.resource_interval)
    stop_event = threading.Event()
    stats = EdgeStats()
    stats_lock = threading.Lock()
    sockets: list[socket.socket] = []
    threads: list[threading.Thread] = []
    end_time = time.monotonic() + args.edge_duration

    try:
        sampler.start()
        connect_deadline = time.monotonic() + args.correctness_timeout
        for index in range(args.edge_clients):
            sock = connect_client(args.host, server.port, connect_deadline)
            sockets.append(sock)
            reader = threading.Thread(
                target=reader_loop,
                args=(sock, stop_event, stats, stats_lock, index % 5 == 0),
                daemon=True,
            )
            sender = threading.Thread(
                target=stable_sender_loop,
                args=(index, sock, end_time, stop_event, stats, stats_lock),
                daemon=True,
            )
            reader.start()
            sender.start()
            threads.extend([reader, sender])

        for _ in range(args.churn_threads):
            thread = threading.Thread(
                target=churn_loop,
                args=(args.host, server.port, end_time, stop_event, stats, stats_lock),
                daemon=True,
            )
            thread.start()
            threads.append(thread)

        for _ in range(args.slow_threads):
            thread = threading.Thread(
                target=slow_client_loop,
                args=(args.host, server.port, end_time, stop_event, stats, stats_lock),
                daemon=True,
            )
            thread.start()
            threads.append(thread)

        while time.monotonic() < end_time:
            ensure_server_alive(server)
            time.sleep(0.25)
    finally:
        stop_event.set()
        close_sockets(sockets)
        for thread in threads:
            thread.join(timeout=2.0)
        summary = sampler.stop()
        killed = stop_server(server, args.shutdown_timeout)

    if killed:
        details = server_tail_details(server)
        suffix = f"\n{details}" if details else ""
        raise RuntimeError(f"server did not stop within {args.shutdown_timeout:.1f}s after SIGINT{suffix}")
    if server.proc.returncode not in (0, None):
        raise RuntimeError(f"server exited with rc={server.proc.returncode}")
    if summary.max_rss_kb is not None and summary.max_rss_kb > args.max_rss_mb * 1024:
        raise RuntimeError(f"server RSS exceeded limit: {summary.max_rss_kb} KiB > {args.max_rss_mb} MiB")
    if summary.max_fds is not None and summary.max_fds > args.max_fds:
        raise RuntimeError(f"server fd count exceeded limit: {summary.max_fds} > {args.max_fds}")

    print(
        "server edge stress ok: "
        f"duration={args.edge_duration:.1f}s sent_lines={stats.sent_lines} "
        f"recv_lines={stats.recv_lines} recv_bytes={stats.recv_bytes} "
        f"churn_connects={stats.churn_connects} half_closes={stats.half_closes} "
        f"resets={stats.resets} slow_clients={stats.slow_clients} "
        f"client_errors={stats.client_errors} "
        f"rss_kb={summary.first_rss_kb}->{summary.last_rss_kb} "
        f"rss_minmax={summary.min_rss_kb}->{summary.max_rss_kb} "
        f"fds={summary.first_fds}->{summary.last_fds} "
        f"fds_minmax={summary.min_fds}->{summary.max_fds} samples={summary.samples}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="./server", help="path to the server binary")
    parser.add_argument("--server-flood", default="./server_flood", help="path to the native flood binary")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--correctness-matrix",
        default=os.getenv("LLAM_SERVER_COMPOSITE_CORRECTNESS", "8:8:16,16:4:64,32:2:1024"),
        help="comma-separated clients:messages:payload_bytes cases",
    )
    parser.add_argument("--correctness-timeout", type=float, default=float(os.getenv("LLAM_SERVER_COMPOSITE_TIMEOUT", "30")))
    parser.add_argument("--flood-duration", type=float, default=float(os.getenv("LLAM_SERVER_COMPOSITE_FLOOD_DURATION", "60")))
    parser.add_argument(
        "--flood-min-delivery-ratio",
        type=float,
        default=float(os.getenv("LLAM_SERVER_COMPOSITE_FLOOD_MIN_DELIVERY_RATIO", "0.0")),
        help="optional minimum observed/expected broadcast ratio for native flood phases",
    )
    parser.add_argument(
        "--allow-flood-forced-stop",
        action="store_true",
        default=os.getenv("LLAM_SERVER_COMPOSITE_ALLOW_FLOOD_FORCED_STOP", "0") != "0",
        help="allow native flood cleanup to SIGKILL the server without failing the phase",
    )
    parser.add_argument(
        "--payload-flood-duration",
        type=float,
        default=float(os.getenv("LLAM_SERVER_COMPOSITE_PAYLOAD_FLOOD_DURATION", "10")),
    )
    parser.add_argument("--edge-duration", type=float, default=float(os.getenv("LLAM_SERVER_COMPOSITE_EDGE_DURATION", "60")))
    parser.add_argument("--edge-clients", type=int, default=int(os.getenv("LLAM_SERVER_COMPOSITE_EDGE_CLIENTS", "24")))
    parser.add_argument("--churn-threads", type=int, default=int(os.getenv("LLAM_SERVER_COMPOSITE_CHURN_THREADS", "4")))
    parser.add_argument("--slow-threads", type=int, default=int(os.getenv("LLAM_SERVER_COMPOSITE_SLOW_THREADS", "4")))
    parser.add_argument("--resource-interval", type=float, default=1.0)
    parser.add_argument("--shutdown-timeout", type=float, default=float(os.getenv("LLAM_SERVER_COMPOSITE_SHUTDOWN_TIMEOUT", "30")))
    parser.add_argument("--max-rss-mb", type=int, default=int(os.getenv("LLAM_SERVER_COMPOSITE_MAX_RSS_MB", "2048")))
    parser.add_argument("--max-fds", type=int, default=int(os.getenv("LLAM_SERVER_COMPOSITE_MAX_FDS", "4096")))
    parser.add_argument("--skip-correctness", action="store_true")
    parser.add_argument("--skip-flood", action="store_true")
    parser.add_argument("--skip-edge", action="store_true")
    parser.add_argument("--quick", action="store_true", help="run a short smoke version of the composite suite")
    parser.add_argument(
        "--soak-hour",
        action="store_true",
        help="run the long one-hour composite profile: 30m main flood, 10m payload floods, 20m edge stress",
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    if args.quick and args.soak_hour:
        raise SystemExit("--quick and --soak-hour are mutually exclusive")
    if args.quick:
        args.correctness_matrix = "4:2:16,8:2:64"
        args.flood_duration = 5.0
        args.payload_flood_duration = 2.0
        args.edge_duration = 5.0
        args.edge_clients = min(args.edge_clients, 8)
        args.churn_threads = min(args.churn_threads, 2)
        args.slow_threads = min(args.slow_threads, 2)
    if args.soak_hour:
        args.flood_duration = 1800.0
        args.payload_flood_duration = 300.0
        args.edge_duration = 1200.0
    if not Path(args.server).exists():
        raise SystemExit(f"server binary not found: {args.server}")
    if not args.skip_flood and not Path(args.server_flood).exists():
        raise SystemExit(f"server_flood binary not found: {args.server_flood}")
    if args.edge_clients < 2:
        raise SystemExit("--edge-clients must be >= 2")
    if args.churn_threads < 0 or args.slow_threads < 0:
        raise SystemExit("--churn-threads and --slow-threads must be >= 0")
    if args.shutdown_timeout <= 0.0:
        raise SystemExit("--shutdown-timeout must be > 0")
    if args.flood_min_delivery_ratio < 0.0 or args.flood_min_delivery_ratio > 1.0:
        raise SystemExit("--flood-min-delivery-ratio must be in [0.0, 1.0]")
    parse_correctness_matrix(args.correctness_matrix)


def main() -> int:
    args = parse_args()
    validate_args(args)
    script_path = Path(__file__).with_name("stress_server.py")
    started = time.monotonic()

    if not args.skip_correctness:
        phase_correctness(args, script_path)
    if not args.skip_flood:
        phase_flood(args)
    if not args.skip_edge:
        phase_edge(args)

    print(f"server composite stress ok: elapsed={time.monotonic() - started:.3f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
