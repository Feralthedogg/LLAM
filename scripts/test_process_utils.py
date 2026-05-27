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


def test_run_with_timeout_kills_descendant() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-run-timeout-test-") as tmp:
        tmp_path = Path(tmp)
        pidfile = tmp_path / "child.pid"
        logfile = tmp_path / "run-with-timeout.log"
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

        wrapper = Path(__file__).resolve().with_name("run_with_timeout.py")
        proc = run_capture(
            [
                Path(sys.executable),
                wrapper,
                "--timeout",
                "0.5",
                "--kill-grace",
                "0.5",
                "--log",
                logfile,
                "--",
                Path(sys.executable),
                parent,
                pidfile,
            ],
            timeout=10.0,
            stderr_to_stdout=True,
        )
        if proc.returncode != 124:
            fail(f"run_with_timeout returned {proc.returncode}, expected 124")
        if "deadline_exceeded" not in proc.stdout:
            fail(f"run_with_timeout did not report timeout diagnosis: {proc.stdout!r}")
        if not pidfile.exists():
            fail("run_with_timeout child pidfile was not written before timeout")

        pid = int(pidfile.read_text(encoding="utf-8").strip())
        try:
            if not wait_for_exit(pid, 5.0):
                fail(f"run_with_timeout descendant survived timeout cleanup: pid={pid}")
        finally:
            if process_alive(pid):
                kill_process(pid)


def test_run_with_timeout_dump_signal_reaches_descendant() -> None:
    if os.name == "nt" or not hasattr(signal, "SIGUSR2"):
        return

    with tempfile.TemporaryDirectory(prefix="llam-run-timeout-dump-test-") as tmp:
        tmp_path = Path(tmp)
        dumpfile = tmp_path / "runtime.dump"
        logfile = tmp_path / "run-with-timeout.log"
        child = tmp_path / "child.py"
        parent = tmp_path / "parent.py"
        child.write_text(
            "\n".join(
                [
                    "import signal",
                    "import sys",
                    "import time",
                    "from pathlib import Path",
                    "dump = Path(sys.argv[1])",
                    "def handle(signum, frame):",
                    "    dump.write_text('descendant-dump-ok', encoding='utf-8')",
                    "signal.signal(signal.SIGUSR2, handle)",
                    "print('child-ready', flush=True)",
                    "time.sleep(30)",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        parent.write_text(
            "\n".join(
                [
                    "import signal",
                    "import subprocess",
                    "import sys",
                    "import time",
                    "signal.signal(signal.SIGUSR2, signal.SIG_IGN)",
                    "child = subprocess.Popen([sys.executable, sys.argv[1], sys.argv[2]])",
                    "print(f'parent-started child={child.pid}', flush=True)",
                    "time.sleep(30)",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        wrapper = Path(__file__).resolve().with_name("run_with_timeout.py")
        proc = run_capture(
            [
                Path(sys.executable),
                wrapper,
                "--timeout",
                "0.5",
                "--kill-grace",
                "0.5",
                "--dump-grace",
                "1.0",
                "--dump-on-timeout",
                dumpfile,
                "--log",
                logfile,
                "--",
                Path(sys.executable),
                parent,
                child,
                dumpfile,
            ],
            timeout=10.0,
            stderr_to_stdout=True,
        )
        if proc.returncode != 124:
            fail(f"dump run_with_timeout returned {proc.returncode}, expected 124")
        if not dumpfile.exists():
            fail(f"dump signal did not reach descendant: {proc.stdout!r}")
        if dumpfile.read_text(encoding="utf-8") != "descendant-dump-ok":
            fail("dump signal wrote unexpected descendant dump content")


def test_run_with_timeout_cleans_after_dump_kills_wrapper() -> None:
    if os.name == "nt" or not hasattr(signal, "SIGUSR2"):
        return

    with tempfile.TemporaryDirectory(prefix="llam-run-timeout-dump-cleanup-test-") as tmp:
        tmp_path = Path(tmp)
        dumpfile = tmp_path / "runtime.dump"
        logfile = tmp_path / "run-with-timeout.log"
        pidfile = tmp_path / "child.pid"
        child = tmp_path / "child.py"
        parent = tmp_path / "parent.py"
        child.write_text(
            "\n".join(
                [
                    "import signal",
                    "import sys",
                    "import time",
                    "from pathlib import Path",
                    "dump = Path(sys.argv[1])",
                    "pidfile = Path(sys.argv[2])",
                    "pidfile.write_text(str(__import__('os').getpid()), encoding='utf-8')",
                    "def handle(signum, frame):",
                    "    dump.write_text('descendant-dump-before-cleanup', encoding='utf-8')",
                    "signal.signal(signal.SIGUSR2, handle)",
                    "print('child-ready', flush=True)",
                    "time.sleep(30)",
                    "",
                ]
            ),
            encoding="utf-8",
        )
        parent.write_text(
            "\n".join(
                [
                    "import subprocess",
                    "import sys",
                    "import time",
                    "subprocess.Popen([sys.executable, sys.argv[1], sys.argv[2], sys.argv[3]])",
                    "print('parent-started', flush=True)",
                    "time.sleep(30)",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        wrapper = Path(__file__).resolve().with_name("run_with_timeout.py")
        proc = run_capture(
            [
                Path(sys.executable),
                wrapper,
                "--timeout",
                "0.5",
                "--kill-grace",
                "0.5",
                "--dump-grace",
                "1.0",
                "--dump-on-timeout",
                dumpfile,
                "--log",
                logfile,
                "--",
                Path(sys.executable),
                parent,
                child,
                dumpfile,
                pidfile,
            ],
            timeout=10.0,
            stderr_to_stdout=True,
        )
        if proc.returncode != 124:
            fail(f"dump cleanup run_with_timeout returned {proc.returncode}, expected 124")
        if not dumpfile.exists():
            fail(f"dump signal did not reach descendant before cleanup: {proc.stdout!r}")
        if not pidfile.exists():
            fail("dump cleanup child pidfile was not written")

        pid = int(pidfile.read_text(encoding="utf-8").strip())
        try:
            if not wait_for_exit(pid, 5.0):
                fail(f"dump cleanup descendant survived wrapper exit: pid={pid}")
        finally:
            if process_alive(pid):
                kill_process(pid)


def write_fake_server_wrapper(path: Path) -> None:
    path.write_text(
        "\n".join(
            [
                "#!/usr/bin/env python3",
                "import os",
                "import subprocess",
                "import sys",
                "import time",
                "from pathlib import Path",
                "pidfile = Path(os.environ['LLAM_FAKE_SERVER_CHILD_PID'])",
                "child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])",
                "pidfile.write_text(str(child.pid), encoding='utf-8')",
                "print(f'fake-server-child pid={child.pid}', flush=True)",
                "time.sleep(30)",
                "",
            ]
        ),
        encoding="utf-8",
    )
    path.chmod(0o755)


def test_stress_server_cleans_wrapper_descendant() -> None:
    if os.name == "nt":
        return

    with tempfile.TemporaryDirectory(prefix="llam-stress-server-cleanup-test-") as tmp:
        tmp_path = Path(tmp)
        fake_server = tmp_path / "fake_server.py"
        pidfile = tmp_path / "child.pid"
        write_fake_server_wrapper(fake_server)

        env = os.environ.copy()
        env["LLAM_FAKE_SERVER_CHILD_PID"] = str(pidfile)
        script = Path(__file__).resolve().with_name("stress_server.py")
        proc = run_capture(
            [
                Path(sys.executable),
                script,
                "--server",
                fake_server,
                "--host",
                "127.0.0.1",
                "--clients",
                "2",
                "--messages",
                "1",
                "--timeout",
                "0.5",
            ],
            env=env,
            timeout=10.0,
            stderr_to_stdout=True,
        )
        if proc.returncode == 0:
            fail("stress_server unexpectedly succeeded with fake non-listening server")
        if not pidfile.exists():
            fail(f"fake stress_server child pidfile was not written: {proc.stdout!r}")

        pid = int(pidfile.read_text(encoding="utf-8").strip())
        try:
            if not wait_for_exit(pid, 5.0):
                fail(f"stress_server fake server descendant survived cleanup: pid={pid}")
        finally:
            if process_alive(pid):
                kill_process(pid)


def test_stress_server_composite_cleans_wrapper_descendant() -> None:
    if os.name == "nt":
        return

    with tempfile.TemporaryDirectory(prefix="llam-stress-composite-cleanup-test-") as tmp:
        tmp_path = Path(tmp)
        fake_server = tmp_path / "fake_server.py"
        pidfile = tmp_path / "child.pid"
        write_fake_server_wrapper(fake_server)

        env = os.environ.copy()
        env["LLAM_FAKE_SERVER_CHILD_PID"] = str(pidfile)
        script = Path(__file__).resolve().with_name("stress_server_composite.py")
        proc = run_capture(
            [
                Path(sys.executable),
                script,
                "--server",
                fake_server,
                "--server-flood",
                fake_server,
                "--host",
                "127.0.0.1",
                "--correctness-timeout",
                "0.5",
                "--correctness-matrix",
                "2:1:1",
                "--skip-flood",
                "--skip-edge",
            ],
            env=env,
            timeout=10.0,
            stderr_to_stdout=True,
        )
        if proc.returncode == 0:
            fail("stress_server_composite unexpectedly succeeded with fake non-listening server")
        if not pidfile.exists():
            fail(f"fake composite child pidfile was not written: {proc.stdout!r}")

        pid = int(pidfile.read_text(encoding="utf-8").strip())
        try:
            if not wait_for_exit(pid, 5.0):
                fail(f"stress_server_composite fake server descendant survived cleanup: pid={pid}")
        finally:
            if process_alive(pid):
                kill_process(pid)


def main() -> int:
    test_path_command_capture()
    test_timeout_kills_descendant()
    test_run_with_timeout_kills_descendant()
    test_run_with_timeout_dump_signal_reaches_descendant()
    test_run_with_timeout_cleans_after_dump_kills_wrapper()
    test_stress_server_cleans_wrapper_descendant()
    test_stress_server_composite_cleans_wrapper_descendant()
    print("[test_process_utils] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
