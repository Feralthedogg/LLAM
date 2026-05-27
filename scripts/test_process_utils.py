#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for CI process timeout cleanup helpers."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from process_utils import ProcessTimeoutError, run_capture


def fail(message: str) -> None:
    raise SystemExit(f"[test_process_utils] {message}")


def process_alive(pid: int) -> bool:
    if os.name == "nt":
        result = subprocess.run(
            ["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        return f'"{pid}"' in result.stdout or f",{pid}," in result.stdout
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def kill_process(pid: int) -> None:
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def wait_for_exit(pid: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not process_alive(pid):
            return True
        time.sleep(0.05)
    return not process_alive(pid)


def test_path_command_capture() -> None:
    proc = run_capture(
        [Path(sys.executable), "-c", "print('process-utils-normal-ok')"],
        timeout=5.0,
        stderr_to_stdout=True,
    )
    if proc.returncode != 0:
        fail(f"normal command returned {proc.returncode}")
    if proc.stdout.strip() != "process-utils-normal-ok":
        fail(f"normal command output mismatch: {proc.stdout!r}")


def test_timeout_kills_descendant() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-process-utils-test-") as tmp:
        tmp_path = Path(tmp)
        pidfile = tmp_path / "child.pid"
        parent = tmp_path / "parent.py"
        parent.write_text(
            "\n".join(
                [
                    "import subprocess",
                    "import sys",
                    "import time",
                    "from pathlib import Path",
                    "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])",
                    "Path(sys.argv[1]).write_text(str(child.pid), encoding='utf-8')",
                    "print(f'child-started pid={child.pid}', flush=True)",
                    "time.sleep(30)",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        try:
            run_capture([Path(sys.executable), parent, pidfile], timeout=0.5, stderr_to_stdout=True)
        except ProcessTimeoutError as exc:
            if "child-started" not in exc.stdout:
                fail(f"timeout output did not include child startup: {exc.stdout!r}")
        else:
            fail("timeout command unexpectedly completed")

        if not pidfile.exists():
            fail("parent did not publish child pid before timeout")
        pid = int(pidfile.read_text(encoding="utf-8").strip())
        try:
            if not wait_for_exit(pid, 5.0):
                fail(f"descendant process survived timeout cleanup: pid={pid}")
        finally:
            if process_alive(pid):
                kill_process(pid)


def main() -> int:
    test_path_command_capture()
    test_timeout_kills_descendant()
    print("[test_process_utils] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
