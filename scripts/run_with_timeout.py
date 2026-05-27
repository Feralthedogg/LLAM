#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run a command with streamed logging and a diagnostic timeout.

This wrapper is intentionally small: GitHub Actions should keep the partial
runtime log even when a stress process hangs and has to be interrupted.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

from process_utils import kill_process_tree
from safe_output import open_text_for_write, prepare_output_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run a command with a hard timeout and live log capture.")
    parser.add_argument("--timeout", type=float, required=True, help="timeout in seconds")
    parser.add_argument("--kill-grace", type=float, default=10.0, help="seconds to wait after graceful interrupt")
    parser.add_argument("--dump-grace", type=float, default=2.0, help="seconds to wait after requesting a dump")
    parser.add_argument(
        "--dump-on-timeout",
        type=Path,
        default=None,
        help="request a target runtime dump at this path before interrupting on POSIX",
    )
    parser.add_argument("--log", type=Path, required=True, help="combined stdout/stderr log path")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command to run after --")
    args = parser.parse_args()
    if args.command and args.command[0] == "--":
        args.command = args.command[1:]
    if not args.command:
        parser.error("missing command")
    if args.timeout <= 0.0:
        parser.error("--timeout must be positive")
    if args.kill_grace < 0.0:
        parser.error("--kill-grace must be non-negative")
    if args.dump_grace < 0.0:
        parser.error("--dump-grace must be non-negative")
    return args


def interrupt_process(proc: subprocess.Popen[str]) -> str:
    if os.name == "nt":
        # TerminateProcess only kills the direct child. The stress wrappers this
        # script supervises may launch the real workload below that child, so on
        # Windows use taskkill tree mode immediately when the deadline expires.
        kill_process_tree(proc)
        return "kill_tree"
    try:
        # POSIX children run in their own session, so an interrupt reaches any
        # helper processes the stress command spawned before the timeout.
        os.killpg(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        pass
    return "interrupt"


def kill_process(proc: subprocess.Popen[str]) -> None:
    kill_process_tree(proc)


def request_runtime_dump(proc: subprocess.Popen[str], log, dump_path: Path | None, dump_grace: float) -> None:
    if dump_path is None or os.name == "nt" or not hasattr(signal, "SIGUSR2"):
        return
    if proc.poll() is not None:
        return
    message = f"[run_with_timeout] requesting runtime dump at {dump_path}\n"
    print(message, end="", file=sys.stderr)
    log.write(message)
    log.flush()
    try:
        # Children run in their own session. Signal the whole group so a
        # wrapper process does not hide the actual LLAM runtime that owns the
        # signal-driven dump handler.
        os.killpg(proc.pid, signal.SIGUSR2)
    except ProcessLookupError:
        return
    deadline = time.monotonic() + dump_grace
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)


def signal_name(signum: int) -> str:
    try:
        return signal.Signals(signum).name
    except (ValueError, AttributeError):
        return f"SIG{signum}"


def classify_exit(rc: int, timed_out: bool) -> tuple[str, str]:
    if timed_out:
        return "timeout", "deadline_exceeded"
    if rc == 0:
        return "ok", "completed"
    if rc < 0:
        signum = -rc
        return "signal", f"terminated_by_{signal_name(signum)}"
    return "nonzero_exit", f"exit_code_{rc}"


def dump_state(dump_path: Path | None) -> str:
    if dump_path is None:
        return "none"
    return "exists" if dump_path.exists() else "missing"


def main() -> int:
    args = parse_args()
    with open_text_for_write(args.log, encoding="utf-8", errors="replace") as log:
        log.write(f"[run_with_timeout] command={' '.join(args.command)} timeout={args.timeout:.3f}s\n")
        log.flush()

        child_env = os.environ.copy()
        if args.dump_on_timeout is not None:
            prepare_output_path(args.dump_on_timeout)
            child_env.setdefault("LLAM_RUNTIME_DUMP_ON_SIGNAL", str(args.dump_on_timeout))

        proc = subprocess.Popen(
            args.command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=child_env,
            # Make timeout cleanup cover descendants without touching the
            # GitHub Actions shell process that launched this wrapper.
            start_new_session=(os.name != "nt"),
        )

        def drain_stdout() -> None:
            assert proc.stdout is not None
            for line in proc.stdout:
                print(line, end="")
                log.write(line)
                log.flush()

        reader = threading.Thread(target=drain_stdout, daemon=True)
        reader.start()

        deadline = time.monotonic() + args.timeout
        timed_out = False
        timeout_action = "none"
        while proc.poll() is None:
            if time.monotonic() >= deadline:
                timed_out = True
                message = f"[run_with_timeout] timeout after {args.timeout:.3f}s; interrupting process\n"
                print(message, end="", file=sys.stderr)
                log.write(message)
                log.flush()
                request_runtime_dump(proc, log, args.dump_on_timeout, args.dump_grace)
                if proc.poll() is not None:
                    break
                timeout_action = interrupt_process(proc)
                try:
                    proc.wait(timeout=args.kill_grace)
                except subprocess.TimeoutExpired:
                    timeout_action = "kill"
                    message = f"[run_with_timeout] process did not exit after {args.kill_grace:.3f}s; killing\n"
                    print(message, end="", file=sys.stderr)
                    log.write(message)
                    log.flush()
                    kill_process(proc)
                    proc.wait()
                break
            time.sleep(0.1)

        reader.join(timeout=5.0)
        rc = proc.returncode
        if rc is None:
            rc = 124

        class_name, reason = classify_exit(rc, timed_out)
        wrapper_rc = 124 if timed_out else rc
        diagnostic = (
            f"[run_with_timeout] diagnosis class={class_name} reason={reason} "
            f"child_exit_code={rc} wrapper_exit_code={wrapper_rc} "
            f"timed_out={int(timed_out)} timeout_action={timeout_action} "
            f"log={args.log} dump={dump_state(args.dump_on_timeout)}\n"
        )
        log.write(f"[run_with_timeout] exit_code={rc} timed_out={int(timed_out)}\n")
        log.write(diagnostic)
        log.flush()
        print(diagnostic, end="", file=sys.stderr if rc != 0 or timed_out else sys.stdout)
        if timed_out:
            return 124
        return rc


if __name__ == "__main__":
    raise SystemExit(main())
