#!/usr/bin/env python3
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


def interrupt_process(proc: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        proc.terminate()
        return
    try:
        # POSIX children run in their own session, so an interrupt reaches any
        # helper processes the stress command spawned before the timeout.
        os.killpg(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        pass


def kill_process(proc: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        proc.kill()
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def request_runtime_dump(proc: subprocess.Popen[str], log, dump_path: Path | None, dump_grace: float) -> None:
    if dump_path is None or os.name == "nt" or not hasattr(signal, "SIGUSR2"):
        return
    if proc.poll() is not None:
        return
    message = f"[run_with_timeout] requesting runtime dump at {dump_path}\n"
    print(message, end="", file=sys.stderr)
    log.write(message)
    log.flush()
    proc.send_signal(signal.SIGUSR2)
    deadline = time.monotonic() + dump_grace
    while proc.poll() is None and time.monotonic() < deadline:
        time.sleep(0.05)


def main() -> int:
    args = parse_args()
    args.log.parent.mkdir(parents=True, exist_ok=True)

    with args.log.open("w", encoding="utf-8", errors="replace") as log:
        log.write(f"[run_with_timeout] command={' '.join(args.command)} timeout={args.timeout:.3f}s\n")
        log.flush()

        child_env = os.environ.copy()
        if args.dump_on_timeout is not None:
            args.dump_on_timeout.parent.mkdir(parents=True, exist_ok=True)
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
                interrupt_process(proc)
                try:
                    proc.wait(timeout=args.kill_grace)
                except subprocess.TimeoutExpired:
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

        log.write(f"[run_with_timeout] exit_code={rc} timed_out={int(timed_out)}\n")
        log.flush()
        if timed_out:
            return 124
        return rc


if __name__ == "__main__":
    raise SystemExit(main())
