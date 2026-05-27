#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Small process helpers for CI scripts that must clean up timeout children."""

from __future__ import annotations

import os
import signal
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, Sequence


@dataclass(frozen=True)
class CapturedProcess:
    args: Sequence[str]
    returncode: int
    stdout: str
    stderr: str


class ProcessTimeoutError(RuntimeError):
    def __init__(self, args: Sequence[str], timeout: float, stdout: str, stderr: str) -> None:
        super().__init__(f"command timed out after {timeout:.3f}s: {' '.join(args)}")
        self.args_list = args
        self.timeout = timeout
        self.stdout = stdout
        self.stderr = stderr


def _kill_process_group(proc: subprocess.Popen[str]) -> None:
    if os.name == "nt":
        proc.kill()
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _decode_output(output: str | bytes | None) -> str:
    if output is None:
        return ""
    if isinstance(output, bytes):
        return output.decode(errors="replace")
    return output


def run_capture(
    command: Sequence[str],
    *,
    cwd: str | Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    stderr_to_stdout: bool = False,
) -> CapturedProcess:
    """Run a command and kill its POSIX process group on timeout.

    ``subprocess.run(..., timeout=...)`` kills only the direct child. Several LLAM
    CI helpers launch wrappers such as ``cargo run`` or shell scripts that spawn
    the actual workload, so timeout cleanup must cover descendants too.
    """

    stderr = subprocess.STDOUT if stderr_to_stdout else subprocess.PIPE
    proc = subprocess.Popen(
        list(command),
        cwd=cwd,
        env=None if env is None else dict(env),
        stdout=subprocess.PIPE,
        stderr=stderr,
        text=True,
        start_new_session=(os.name != "nt"),
    )
    try:
        stdout, captured_stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        _kill_process_group(proc)
        stdout, captured_stderr = proc.communicate()
        raise ProcessTimeoutError(
            list(command),
            0.0 if timeout is None else float(timeout),
            _decode_output(stdout or exc.stdout),
            "" if stderr_to_stdout else _decode_output(captured_stderr or exc.stderr),
        ) from None

    return CapturedProcess(
        args=list(command),
        returncode=proc.returncode,
        stdout=_decode_output(stdout),
        stderr="" if stderr_to_stdout else _decode_output(captured_stderr),
    )


def print_captured_output(stdout: str, stderr: str = "") -> None:
    if stdout:
        print(stdout, end="", flush=True)
    if stderr:
        print(stderr, end="", flush=True)
