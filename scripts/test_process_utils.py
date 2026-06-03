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
from safe_output import prepare_output_dir, write_text_safely


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


def try_symlink(target: Path, link: Path) -> bool:
    """Create a symlink when the host permits it; Windows may deny this."""

    try:
        link.symlink_to(target, target_is_directory=target.is_dir())
        return True
    except (OSError, NotImplementedError):
        return False


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


def test_run_capture_rejects_nonfinite_timeout_before_spawn() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-process-utils-invalid-timeout-test-") as tmp:
        tmp_path = Path(tmp)
        pidfile = tmp_path / "spawned.pid"
        child = tmp_path / "child.py"
        child.write_text(
            "\n".join(
                [
                    "import os",
                    "import sys",
                    "import time",
                    "from pathlib import Path",
                    "Path(sys.argv[1]).write_text(str(os.getpid()), encoding='utf-8')",
                    "time.sleep(30)",
                    "",
                ]
            ),
            encoding="utf-8",
        )

        for timeout in (float("nan"), float("inf"), -1.0):
            try:
                run_capture([Path(sys.executable), child, pidfile], timeout=timeout, stderr_to_stdout=True)
            except ValueError as exc:
                if "finite non-negative" not in str(exc):
                    fail(f"run_capture rejected invalid timeout with unexpected error: {exc}")
            else:
                fail(f"run_capture accepted invalid timeout {timeout!r}")

            time.sleep(0.2)
            if pidfile.exists():
                pid = int(pidfile.read_text(encoding="utf-8").strip())
                try:
                    fail(f"run_capture spawned child before rejecting invalid timeout {timeout!r}: pid={pid}")
                finally:
                    if process_alive(pid):
                        kill_process(pid)


def test_run_with_timeout_rejects_nonfinite_timeouts() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-run-timeout-invalid-test-") as tmp:
        tmp_path = Path(tmp)
        wrapper = Path(__file__).resolve().with_name("run_with_timeout.py")
        cases = [
            ["--timeout", "nan"],
            ["--timeout", "inf"],
            ["--timeout", "-1"],
            ["--timeout", "1000000000"],
            ["--timeout", "1", "--kill-grace", "nan"],
            ["--timeout", "1", "--kill-grace", "1000000000"],
            ["--timeout", "1", "--dump-grace", "inf"],
            ["--timeout", "1", "--dump-grace", "-1"],
            ["--timeout", "1", "--dump-grace", "1000000000"],
        ]

        for index, option_args in enumerate(cases):
            logfile = tmp_path / f"invalid-{index}.log"
            proc = run_capture(
                [
                    Path(sys.executable),
                    wrapper,
                    *option_args,
                    "--log",
                    logfile,
                    "--",
                    Path(sys.executable),
                    "-c",
                    "pass",
                ],
                timeout=5.0,
                stderr_to_stdout=True,
            )
            if proc.returncode == 0:
                fail(f"run_with_timeout accepted invalid timing args {option_args!r}")
            if "finite" not in proc.stdout:
                fail(f"run_with_timeout invalid timing diagnostic was unclear: {proc.stdout!r}")


def test_stress_and_bench_helpers_reject_invalid_numbers() -> None:
    script_dir = Path(__file__).resolve().parent
    cases = [
        [
            script_dir / "runtime_soak.py",
            "--duration",
            "nan",
            "--runtime-fuzz",
            "./does-not-exist",
            "--multi-runtime-core",
            "./does-not-exist",
            "--runtime-stress",
            "./does-not-exist",
            "--runtime-shutdown",
            "./does-not-exist",
            "--io-buffers",
            "./does-not-exist",
        ],
        [script_dir / "runtime_soak.py", "--timeout", "inf"],
        [script_dir / "runtime_soak.py", "--duration", "1000000000"],
        [script_dir / "runtime_soak.py", "--timeout", "1000000000"],
        [script_dir / "runtime_soak.py", "--fuzz-scenarios", "1000000000"],
        [script_dir / "runtime_soak.py", "--multi-fuzz-scenarios", "1000000000"],
        [script_dir / "runtime_soak.py", "--seed", "18446744073709551616"],
        [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge", "--edge-duration", "nan"],
        [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge", "--flood-duration", "inf"],
        [
            script_dir / "stress_server_composite.py",
            "--skip-correctness",
            "--skip-flood",
            "--skip-edge",
            "--edge-duration",
            "1000000000",
        ],
        [
            script_dir / "stress_server_composite.py",
            "--skip-correctness",
            "--skip-flood",
            "--skip-edge",
            "--edge-clients",
            "1000000000",
        ],
        [
            script_dir / "stress_server_composite.py",
            "--skip-correctness",
            "--skip-flood",
            "--skip-edge",
            "--churn-threads",
            "1000000000",
        ],
        [
            script_dir / "stress_server_composite.py",
            "--skip-correctness",
            "--skip-flood",
            "--skip-edge",
            "--slow-threads",
            "1000000000",
        ],
        [
            script_dir / "stress_server_composite.py",
            "--skip-correctness",
            "--skip-flood",
            "--skip-edge",
            "--command-timeout-padding",
            "1000000000",
        ],
        [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2", "--timeout", "nan"],
        [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2", "--timeout", "1000000000"],
        [
            script_dir / "stress_server.py",
            "--server",
            "./does-not-exist",
            "--clients",
            "2",
            "--connect-spread-ms",
            "1000000000",
        ],
        [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "1000000000"],
        [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2", "--payload-bytes", "1000000000000"],
        [script_dir / "bench_guard.py", "--timeout", "nan"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--rounds", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--timeout", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--spawn-tasks", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--channel-messages", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--select-ops", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--io-messages", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--poll-events", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--sleep-tasks", "1000000000"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--opaque-scopes", "1000000000"],
        [script_dir / "bench_matrix.py", "--timeout", "-1", "--no-build"],
        [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam", "--timeout", "-1"],
        [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam", "--spread-warning-ratio", "nan"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--min-ops", "spawn_join=-1"],
        [script_dir / "bench_guard.py", "--bench", "./does-not-exist", "--min-ops", "spawn_join=nan"],
        [script_dir / "bench_matrix.py", "--rounds", "1000000000", "--no-build"],
        [script_dir / "bench_matrix.py", "--timeout", "1000000000", "--no-build"],
        [script_dir / "bench_matrix.py", "--spin-ns", "10000000000", "--no-build"],
        [script_dir / "bench_matrix.py", "--spin-iters", "1000000000", "--no-build"],
        [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam", "--rounds", "1000000000"],
        [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam", "--timeout", "1000000000"],
        [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam", "--samples", "1000000000"],
        [
            script_dir / "bench_runtime_compare.py",
            "--no-build",
            "--runtime",
            "llam",
            "--spread-warning-ratio",
            "1000000000",
        ],
    ]

    for command in cases:
        proc = run_capture(
            [Path(sys.executable), *command],
            timeout=5.0,
            stderr_to_stdout=True,
        )
        if proc.returncode == 0:
            fail(f"helper accepted invalid numeric argument: {command!r}")
        if "finite" not in proc.stdout and "positive" not in proc.stdout and "integer" not in proc.stdout:
            fail(f"helper invalid numeric diagnostic was unclear for {command!r}: {proc.stdout!r}")

    env_cases = [
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_EDGE_DURATION": "nan"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_EDGE_DURATION": "1000000000"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_EDGE_CLIENTS": "1000000000"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_CHURN_THREADS": "1000000000"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_SLOW_THREADS": "1000000000"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_COMMAND_TIMEOUT_PADDING": "1000000000"},
        ),
        (
            [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2"],
            {"LLAM_SERVER_STRESS_TIMEOUT": "nan"},
        ),
        (
            [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2"],
            {"LLAM_SERVER_STRESS_TIMEOUT": "1000000000"},
        ),
        (
            [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2"],
            {"LLAM_SERVER_STRESS_CONNECT_SPREAD_MS": "1000000000"},
        ),
        (
            [script_dir / "stress_server.py", "--server", "./does-not-exist"],
            {"LLAM_SERVER_STRESS_CLIENTS": "1000000000"},
        ),
        (
            [script_dir / "stress_server.py", "--server", "./does-not-exist"],
            {"LLAM_SERVER_STRESS_PAYLOAD_BYTES": "1000000000000"},
        ),
        (
            [script_dir / "stress_server.py", "--server", "./does-not-exist", "--clients", "2"],
            {"LLAM_SERVER_STRESS_SEED": "nan"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_MAX_UNEXPECTED_CLIENT_ERRORS": "nan"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--skip-correctness", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_ALLOW_FLOOD_FORCED_STOP": "maybe"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--server", "./does-not-exist", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_CORRECTNESS": "bad"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--server", "./does-not-exist", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_CORRECTNESS": "1:1:-1"},
        ),
        (
            [script_dir / "stress_server_composite.py", "--server", "./does-not-exist", "--skip-flood", "--skip-edge"],
            {"LLAM_SERVER_COMPOSITE_CORRECTNESS": "0:1:1"},
        ),
        (
            [script_dir / "bench_guard.py", "--bench", "./does-not-exist"],
            {"LLAM_BENCH_GUARD_TIMEOUT": "nan"},
        ),
        (
            [script_dir / "bench_guard.py", "--bench", "./does-not-exist"],
            {"LLAM_BENCH_GUARD_TIMEOUT": "1000000000"},
        ),
        (
            [script_dir / "bench_guard.py", "--bench", "./does-not-exist"],
            {"LLAM_BENCH_GUARD_ROUNDS": "1000000000"},
        ),
        (
            [script_dir / "bench_guard.py", "--bench", "./does-not-exist"],
            {"LLAM_BENCH_GUARD_WARMUP": "1000000000"},
        ),
        (
            [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam"],
            {"LLAM_BENCH_COMPARE_SAMPLES": "nan"},
        ),
        (
            [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam"],
            {"LLAM_BENCH_COMPARE_SAMPLES": "1000000000"},
        ),
        (
            [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam"],
            {"LLAM_BENCH_COMPARE_SPREAD_WARNING_RATIO": "1000000000"},
        ),
        (
            [script_dir / "bench_runtime_compare.py", "--no-build", "--runtime", "llam"],
            {"LLAM_BENCH_COMPARE_SAMPLE_POLICY": "garbage"},
        ),
    ]

    for command, extra_env in env_cases:
        env = os.environ.copy()
        env.update(extra_env)
        proc = run_capture(
            [Path(sys.executable), *command],
            env=env,
            timeout=5.0,
            stderr_to_stdout=True,
        )
        if proc.returncode == 0:
            fail(f"helper accepted invalid numeric environment: {command!r} env={extra_env!r}")
        if "Traceback" in proc.stdout:
            fail(f"helper produced traceback for invalid environment {extra_env!r}: {proc.stdout!r}")
        if (
            "finite" not in proc.stdout
            and "positive" not in proc.stdout
            and "integer" not in proc.stdout
            and "correctness case" not in proc.stdout
            and "must be one of" not in proc.stdout
        ):
            fail(f"helper invalid environment diagnostic was unclear for {extra_env!r}: {proc.stdout!r}")

    env = os.environ.copy()
    env["LLAM_SERVER_COMPOSITE_ALLOW_FLOOD_FORCED_STOP"] = "false"
    proc = run_capture(
        [
            Path(sys.executable),
            script_dir / "stress_server_composite.py",
            "--skip-correctness",
            "--skip-flood",
            "--skip-edge",
        ],
        env=env,
        timeout=5.0,
        stderr_to_stdout=True,
    )
    if proc.returncode != 0:
        fail(f"stress composite rejected false boolean environment: {proc.stdout!r}")


def test_safe_output_rejects_symlink_leaf() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-safe-output-leaf-test-") as tmp:
        tmp_path = Path(tmp)
        outside = tmp_path / "outside.txt"
        link = tmp_path / "result.txt"
        outside.write_text("outside-before", encoding="utf-8")
        if not try_symlink(outside, link):
            return

        try:
            write_text_safely(link, "unsafe")
        except RuntimeError as exc:
            if "refusing symlink output path" not in str(exc):
                fail(f"safe_output reported unexpected symlink leaf error: {exc}")
        else:
            fail("safe_output followed a symlink output leaf")
        if outside.read_text(encoding="utf-8") != "outside-before":
            fail("safe_output modified a symlink target")


def test_safe_output_rejects_hardlinked_leaf() -> None:
    if not hasattr(os, "link"):
        return

    with tempfile.TemporaryDirectory(prefix="llam-safe-output-hardlink-test-") as tmp:
        tmp_path = Path(tmp)
        outside = tmp_path / "outside.txt"
        hardlink = tmp_path / "result.txt"
        outside.write_text("outside-before", encoding="utf-8")
        try:
            os.link(outside, hardlink)
        except OSError:
            return

        try:
            write_text_safely(hardlink, "unsafe")
        except RuntimeError as exc:
            if "refusing hard-linked output path" not in str(exc):
                fail(f"safe_output reported unexpected hardlink error: {exc}")
        else:
            fail("safe_output overwrote a hard-linked output leaf")
        if outside.read_text(encoding="utf-8") != "outside-before":
            fail("safe_output modified a hard-linked target")


def test_safe_output_rejects_symlink_parent_prepare_dir() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-safe-output-parent-test-") as tmp:
        tmp_path = Path(tmp)
        outside = tmp_path / "outside"
        link = tmp_path / "link-dir"
        outside.mkdir()
        if not try_symlink(outside, link):
            return

        try:
            prepare_output_dir(link / "artifacts")
        except RuntimeError as exc:
            if "refusing symlink output directory" not in str(exc):
                fail(f"safe_output reported unexpected symlink parent error: {exc}")
        else:
            fail("safe_output followed a symlink output parent")
        if (outside / "artifacts").exists():
            fail("safe_output created a directory through a symlink parent")


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


def test_stress_server_composite_times_out_wrapper_flood_descendant() -> None:
    if os.name == "nt":
        return

    with tempfile.TemporaryDirectory(prefix="llam-stress-composite-flood-timeout-test-") as tmp:
        tmp_path = Path(tmp)
        fake_flood = tmp_path / "fake_flood.py"
        pidfile = tmp_path / "child.pid"
        write_fake_server_wrapper(fake_flood)

        env = os.environ.copy()
        env["LLAM_FAKE_SERVER_CHILD_PID"] = str(pidfile)
        script = Path(__file__).resolve().with_name("stress_server_composite.py")
        proc = run_capture(
            [
                Path(sys.executable),
                script,
                "--server",
                fake_flood,
                "--server-flood",
                fake_flood,
                "--host",
                "127.0.0.1",
                "--skip-correctness",
                "--skip-edge",
                "--flood-duration",
                "0.1",
                "--payload-flood-duration",
                "0.1",
                "--shutdown-timeout",
                "0.5",
                "--command-timeout-padding",
                "0.5",
            ],
            env=env,
            timeout=10.0,
            stderr_to_stdout=True,
        )
        if proc.returncode == 0:
            fail("stress_server_composite unexpectedly succeeded with hanging fake flood")
        if "class=command_timeout" not in proc.stdout:
            fail(f"fake flood timeout did not produce command timeout diagnostic: {proc.stdout!r}")
        if not pidfile.exists():
            fail(f"fake flood child pidfile was not written: {proc.stdout!r}")

        pid = int(pidfile.read_text(encoding="utf-8").strip())
        try:
            if not wait_for_exit(pid, 5.0):
                fail(f"stress_server_composite fake flood descendant survived timeout cleanup: pid={pid}")
        finally:
            if process_alive(pid):
                kill_process(pid)


def main() -> int:
    test_path_command_capture()
    test_run_capture_rejects_nonfinite_timeout_before_spawn()
    test_run_with_timeout_rejects_nonfinite_timeouts()
    test_stress_and_bench_helpers_reject_invalid_numbers()
    test_safe_output_rejects_symlink_leaf()
    test_safe_output_rejects_hardlinked_leaf()
    test_safe_output_rejects_symlink_parent_prepare_dir()
    test_timeout_kills_descendant()
    test_run_with_timeout_kills_descendant()
    test_run_with_timeout_dump_signal_reaches_descendant()
    test_run_with_timeout_cleans_after_dump_kills_wrapper()
    test_stress_server_cleans_wrapper_descendant()
    test_stress_server_composite_cleans_wrapper_descendant()
    test_stress_server_composite_times_out_wrapper_flood_descendant()
    print("[test_process_utils] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
