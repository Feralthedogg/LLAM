#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Diagnostic-output path security smoke tests for example binaries."""

from __future__ import annotations

import os
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def fail(message: str, output: str = "") -> None:
    print(f"[test_example_diagnostic_security] {message}", file=sys.stderr)
    if output:
        print(output, end="" if output.endswith("\n") else "\n", file=sys.stderr)
    raise SystemExit(1)


def reserve_port() -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])
    finally:
        sock.close()


def wait_for_server(port: int, proc: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            output = proc.communicate(timeout=1)[0]
            fail("server exited before it accepted connections", output)
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    proc.terminate()
    output = proc.communicate(timeout=2)[0]
    fail("server did not start listening in time", output)


def run_server_once(server: Path, env: dict[str, str]) -> str:
    port = reserve_port()
    child_env = os.environ.copy()
    child_env.update(env)
    child_env["LLAM_CHAT_QUIET"] = "1"
    proc = subprocess.Popen(
        [str(server), str(port)],
        env=child_env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    wait_for_server(port, proc)
    try:
        os.kill(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        pass
    try:
        output = proc.communicate(timeout=10)[0]
    except subprocess.TimeoutExpired:
        proc.kill()
        output = proc.communicate(timeout=2)[0]
        fail("server did not stop after SIGINT", output)
    if proc.returncode != 0:
        fail(f"server exited with {proc.returncode}", output)
    return output


def test_server_rejects_symlink_leaf(server: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="llam-server-diagnostics-symlink-") as tmp:
        tmp_path = Path(tmp)
        outside_stats = tmp_path / "outside-stats"
        outside_dump = tmp_path / "outside-dump"
        stats_link = tmp_path / "stats-link"
        dump_link = tmp_path / "dump-link"
        outside_stats.touch()
        outside_dump.touch()
        stats_link.symlink_to(outside_stats)
        dump_link.symlink_to(outside_dump)

        output = run_server_once(
            server,
            {
                "LLAM_CHAT_STATS_PATH": str(stats_link),
                "LLAM_CHAT_DUMP_ON_STOP": str(dump_link),
            },
        )
        if outside_stats.stat().st_size != 0 or outside_dump.stat().st_size != 0:
            fail("server followed a diagnostic symlink path", output)


def test_server_rejects_hardlink_leaf(server: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="llam-server-diagnostics-hardlink-") as tmp:
        tmp_path = Path(tmp)
        outside_stats = tmp_path / "outside-stats"
        outside_dump = tmp_path / "outside-dump"
        stats_hardlink = tmp_path / "stats-hardlink"
        dump_hardlink = tmp_path / "dump-hardlink"
        outside_stats.write_text("outside-stats\n", encoding="utf-8")
        outside_dump.write_text("outside-dump\n", encoding="utf-8")
        os.link(outside_stats, stats_hardlink)
        os.link(outside_dump, dump_hardlink)

        output = run_server_once(
            server,
            {
                "LLAM_CHAT_STATS_PATH": str(stats_hardlink),
                "LLAM_CHAT_DUMP_ON_STOP": str(dump_hardlink),
            },
        )
        if outside_stats.read_text(encoding="utf-8") != "outside-stats\n":
            fail("server modified a hard-linked stats path", output)
        if outside_dump.read_text(encoding="utf-8") != "outside-dump\n":
            fail("server modified a hard-linked dump path", output)


def test_server_rejects_parent_symlink(server: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="llam-server-diagnostics-parent-symlink-") as tmp:
        tmp_path = Path(tmp)
        outside = tmp_path / "outside"
        diag_link = tmp_path / "diag-link"
        outside.mkdir()
        diag_link.symlink_to(outside, target_is_directory=True)

        output = run_server_once(
            server,
            {
                "LLAM_CHAT_STATS_PATH": str(diag_link / "stats.txt"),
                "LLAM_CHAT_DUMP_ON_STOP": str(diag_link / "dump.txt"),
            },
        )
        if (outside / "stats.txt").exists() or (outside / "dump.txt").exists():
            fail("server followed a diagnostic parent symlink path", output)


def read_until_stress_ready(proc: subprocess.Popen[str]) -> tuple[bool, str]:
    assert proc.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(proc.stdout, selectors.EVENT_READ)
    output_parts: list[str] = []
    deadline = time.monotonic() + 10.0
    ready = False
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            remainder = proc.stdout.read()
            if remainder:
                output_parts.append(remainder)
            break
        for key, _events in selector.select(timeout=0.1):
            line = key.fileobj.readline()
            if line:
                output_parts.append(line)
                if "signal dump path" in line:
                    ready = True
                    return ready, "".join(output_parts)
    return ready, "".join(output_parts)


def test_stress_rejects_parent_symlink(stress: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="llam-stress-dump-parent-symlink-") as tmp:
        tmp_path = Path(tmp)
        outside = tmp_path / "outside"
        diag_link = tmp_path / "diag-link"
        outside.mkdir()
        diag_link.symlink_to(outside, target_is_directory=True)

        child_env = os.environ.copy()
        child_env.update(
            {
                "LLAM_STRESS_ROUNDS": "1",
                "LLAM_STRESS_DYNAMIC_ROUNDS": "1",
                "LLAM_RUNTIME_DUMP_ON_SIGNAL": str(diag_link / "dump.txt"),
            }
        )
        proc = subprocess.Popen(
            [str(stress)],
            env=child_env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        ready, output = read_until_stress_ready(proc)
        if not ready:
            if proc.poll() is None:
                proc.terminate()
                output += proc.communicate(timeout=2)[0]
            fail("stress signal dump test did not reach signal setup", output)

        sent_signal = False
        for _ in range(20):
            if proc.poll() is not None:
                break
            try:
                os.kill(proc.pid, signal.SIGUSR2)
                sent_signal = True
            except ProcessLookupError:
                break
            time.sleep(0.05)

        try:
            output += proc.communicate(timeout=20)[0]
        except subprocess.TimeoutExpired:
            proc.kill()
            output += proc.communicate(timeout=2)[0]
            fail("stress did not exit after signal dump test", output)
        if proc.returncode != 0:
            fail(f"stress exited with {proc.returncode}", output)
        if not sent_signal:
            fail("stress exited before SIGUSR2 could be sent", output)
        if (outside / "dump.txt").exists():
            fail("stress followed a diagnostic parent symlink path", output)


def main(argv: list[str]) -> int:
    server = Path(argv[1] if len(argv) > 1 else "./server").resolve()
    stress = Path(argv[2] if len(argv) > 2 else "./stress").resolve()

    test_server_rejects_symlink_leaf(server)
    test_server_rejects_hardlink_leaf(server)
    test_server_rejects_parent_symlink(server)
    test_stress_rejects_parent_symlink(stress)
    print("[test_example_diagnostic_security] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
