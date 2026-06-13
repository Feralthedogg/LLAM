#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Stats-file security regression tests for server_flood."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
from pathlib import Path


MALICIOUS_SERVER = r"""
#!/usr/bin/env python3
import os
import signal
import socket
import sys
import time

port = int(sys.argv[-1])
stats = os.environ.get("LLAM_CHAT_STATS_PATH")
attack = os.environ.get("LLAM_STATS_ATTACK")

if attack == "leaf_symlink":
    target = os.environ.get("LLAM_MALICIOUS_STATS_TARGET")
    if stats and target:
        try:
            os.unlink(stats)
        except FileNotFoundError:
            pass
        os.symlink(target, stats)
elif attack == "parent_symlink":
    target_dir = os.environ.get("LLAM_MALICIOUS_STATS_DIR")
    if stats and target_dir:
        parent = os.path.dirname(stats)
        try:
            os.rmdir(parent)
        except OSError:
            pass
        try:
            os.symlink(target_dir, parent)
        except FileExistsError:
            pass
elif attack == "malformed":
    if stats:
        with open(stats, "w", encoding="utf-8") as out:
            out.write(
                "server stopped; outbox_full_drops=-1 outbox_closed_drops=0 "
                "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
                "broadcast_deliveries_enqueued=1\n"
            )

stop = False


def handle(_signum, _frame):
    global stop
    stop = True


signal.signal(signal.SIGINT, handle)
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("127.0.0.1", port))
sock.listen(16)
sock.setblocking(False)
clients = []
deadline = time.time() + 5.0
while not stop and time.time() < deadline:
    try:
        client, _peer = sock.accept()
        client.setblocking(False)
        clients.append(client)
    except BlockingIOError:
        pass
    for client in list(clients):
        try:
            data = client.recv(4096)
            if data:
                client.sendall(b"x\n")
            elif data == b"":
                clients.remove(client)
                client.close()
        except BlockingIOError:
            pass
        except OSError:
            clients.remove(client)
            client.close()
    time.sleep(0.001)
for client in clients:
    try:
        client.close()
    except OSError:
        pass
sock.close()
"""


FLOOD_ARGS = [
    "--clients",
    "2",
    "--duration",
    "0.5",
    "--drain-sec",
    "0.5",
    "--message-bytes",
    "8",
    "--batch",
    "1",
    "--target-mps",
    "0.010",
    "--min-delivery-mps",
    "0",
    "--min-delivery-ratio",
    "0",
    "--allow-forced-stop",
    "--allow-missing-stats",
]


def fail(message: str, proc: subprocess.CompletedProcess[str] | None = None) -> None:
    print(f"[test_server_flood_stats_security] {message}", file=sys.stderr)
    if proc is not None:
        print(f"[test_server_flood_stats_security] command: {' '.join(proc.args)}", file=sys.stderr)
        print(f"[test_server_flood_stats_security] exit: {proc.returncode}", file=sys.stderr)
        if proc.stdout:
            print(proc.stdout, end="", file=sys.stderr)
        if proc.stderr:
            print(proc.stderr, end="", file=sys.stderr)
    raise SystemExit(1)


def write_malicious_server(tmp_path: Path) -> Path:
    server = tmp_path / "malicious_server.py"
    server.write_text(textwrap.dedent(MALICIOUS_SERVER).lstrip(), encoding="utf-8")
    server.chmod(0o755)
    return server


def run_flood(binary: Path, server: Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    child_env = os.environ.copy()
    child_env.update(env)
    return subprocess.run(
        [str(binary), "--server", str(server), *FLOOD_ARGS],
        env=child_env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
        check=False,
    )


def expect_stats_unavailable(
    binary: Path,
    name: str,
    env: dict[str, str],
    forbidden_output: str,
) -> None:
    with tempfile.TemporaryDirectory(prefix=f"llam-{name}-") as tmp:
        tmp_path = Path(tmp)
        server = write_malicious_server(tmp_path)
        try:
            proc = run_flood(binary, server, env)
        except subprocess.TimeoutExpired as exc:
            fail(f"{name}: server_flood timed out: {' '.join(exc.cmd)}")

    output = proc.stdout + proc.stderr
    if proc.returncode != 0:
        fail(f"{name}: server_flood failed unexpectedly", proc)
    if forbidden_output in output:
        fail(f"{name}: accepted attacker-controlled stats", proc)
    if "server flood stats: unavailable" not in output:
        fail(f"{name}: missing unavailable-stats diagnostic", proc)


def test_leaf_symlink(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="llam-server-flood-stats-symlink-") as tmp:
        tmp_path = Path(tmp)
        outside_stats = tmp_path / "outside-stats"
        outside_stats.write_text(
            "server stopped; outbox_full_drops=123456 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=1\n",
            encoding="utf-8",
        )
        expect_stats_unavailable(
            binary,
            "leaf symlink",
            {
                "LLAM_STATS_ATTACK": "leaf_symlink",
                "LLAM_MALICIOUS_STATS_TARGET": str(outside_stats),
            },
            "outbox_full_drops=123456",
        )


def test_parent_symlink(binary: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="llam-server-flood-stats-parent-symlink-") as tmp:
        tmp_path = Path(tmp)
        outside_dir = tmp_path / "outside"
        outside_dir.mkdir()
        (outside_dir / "stats.txt").write_text(
            "server stopped; outbox_full_drops=654321 outbox_closed_drops=0 "
            "broadcast_messages_created=1 broadcast_deliveries_attempted=1 "
            "broadcast_deliveries_enqueued=1\n",
            encoding="utf-8",
        )
        expect_stats_unavailable(
            binary,
            "parent symlink",
            {
                "LLAM_STATS_ATTACK": "parent_symlink",
                "LLAM_MALICIOUS_STATS_DIR": str(outside_dir),
            },
            "outbox_full_drops=654321",
        )


def test_malformed_stats(binary: Path) -> None:
    expect_stats_unavailable(
        binary,
        "malformed stats",
        {"LLAM_STATS_ATTACK": "malformed"},
        "outbox_full_drops=18446744073709551615",
    )


def main(argv: list[str]) -> int:
    binary = Path(argv[1] if len(argv) > 1 else "./server_flood").resolve()

    test_leaf_symlink(binary)
    test_parent_symlink(binary)
    test_malformed_stats(binary)
    print("[test_server_flood_stats_security] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
