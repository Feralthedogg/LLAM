#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Small process helpers for CI scripts that must clean up timeout children."""

from __future__ import annotations

import math
import os
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Mapping, Sequence


@dataclass(frozen=True)
class CapturedProcess:
    args: Sequence[str]
    returncode: int
    stdout: str
    stderr: str
    stdout_truncated: bool = False
    stderr_truncated: bool = False


class ProcessTimeoutError(RuntimeError):
    def __init__(
        self,
        args: Sequence[str],
        timeout: float,
        stdout: str,
        stderr: str,
        *,
        stdout_truncated: bool = False,
        stderr_truncated: bool = False,
    ) -> None:
        super().__init__(f"command timed out after {timeout:.3f}s: {' '.join(str(arg) for arg in args)}")
        self.args_list = args
        self.timeout = timeout
        self.stdout = stdout
        self.stderr = stderr
        self.stdout_truncated = stdout_truncated
        self.stderr_truncated = stderr_truncated


def interrupt_process_tree(proc: subprocess.Popen[str]) -> None:
    """Request cooperative termination for a process tree when supported."""

    if os.name == "nt":
        proc.terminate()
        return
    try:
        os.killpg(proc.pid, signal.SIGINT)
    except ProcessLookupError:
        pass


def kill_process_tree(proc: subprocess.Popen[str]) -> None:
    """Forcefully terminate a process and any descendants we can address.

    POSIX callers are expected to create the child in a new session so the
    process group is isolated from the invoking shell. Windows has no portable
    Python process-group equivalent, so use taskkill's tree mode.
    """

    if os.name == "nt":
        try:
            subprocess.run(
                ["taskkill", "/PID", str(proc.pid), "/T", "/F"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=10.0,
            )
        except (OSError, subprocess.TimeoutExpired):
            proc.kill()
        return
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def _validate_timeout(timeout: float | None) -> None:
    if timeout is None:
        return
    timeout_value = float(timeout)
    if not math.isfinite(timeout_value) or timeout_value < 0.0:
        raise ValueError("timeout must be a finite non-negative value")


def _validate_output_limit(max_output_bytes: int | None) -> None:
    if max_output_bytes is None:
        return
    if (
        isinstance(max_output_bytes, bool)
        or not isinstance(max_output_bytes, int)
        or max_output_bytes < 0
    ):
        raise ValueError(
            "max_output_bytes must be a non-negative integer"
        )


class _PipeCapture:
    def __init__(self, limit: int | None) -> None:
        self.limit = limit
        self.chunks: list[bytes] = []
        self.size = 0
        self.truncated = False

    def drain(self, stream: BinaryIO) -> None:
        try:
            while True:
                chunk = stream.read(64 * 1024)
                if not chunk:
                    break
                if self.limit is None:
                    self.chunks.append(chunk)
                    self.size += len(chunk)
                    continue
                remaining = max(0, self.limit - self.size)
                if remaining:
                    retained = chunk[:remaining]
                    self.chunks.append(retained)
                    self.size += len(retained)
                if len(chunk) > remaining:
                    self.truncated = True
        except OSError:
            # A killed process tree may tear down a pipe while a reader is
            # draining its final chunk. The retained prefix is still useful.
            pass
        finally:
            stream.close()

    def text(self) -> str:
        return b"".join(self.chunks).decode(errors="replace")


def run_capture(
    command: Sequence[str],
    *,
    cwd: str | Path | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
    stderr_to_stdout: bool = False,
    max_output_bytes: int | None = None,
) -> CapturedProcess:
    """Run a command and kill its process tree/group on timeout.

    ``subprocess.run(..., timeout=...)`` kills only the direct child. Several LLAM
    CI helpers launch wrappers such as ``cargo run`` or shell scripts that spawn
    the actual workload, so timeout cleanup must cover descendants too. POSIX
    callers get a new session/process group; Windows callers use ``taskkill /T``.
    """

    _validate_timeout(timeout)
    _validate_output_limit(max_output_bytes)
    command_args = [str(arg) for arg in command]
    stderr = subprocess.STDOUT if stderr_to_stdout else subprocess.PIPE
    proc = subprocess.Popen(
        command_args,
        cwd=cwd,
        env=None if env is None else dict(env),
        stdout=subprocess.PIPE,
        stderr=stderr,
        text=False,
        start_new_session=(os.name != "nt"),
    )
    assert proc.stdout is not None
    stdout_capture = _PipeCapture(max_output_bytes)
    stdout_reader = threading.Thread(
        target=stdout_capture.drain,
        args=(proc.stdout,),
        daemon=True,
    )
    stderr_capture = (
        None
        if stderr_to_stdout
        else _PipeCapture(max_output_bytes)
    )
    stderr_reader = None
    if stderr_capture is not None:
        assert proc.stderr is not None
        stderr_reader = threading.Thread(
            target=stderr_capture.drain,
            args=(proc.stderr,),
            daemon=True,
        )
    stdout_reader.start()
    if stderr_reader is not None:
        stderr_reader.start()

    timed_out = False
    deadline = (
        None
        if timeout is None
        else time.monotonic() + float(timeout)
    )
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        kill_process_tree(proc)
        proc.wait()

    readers = [stdout_reader]
    if stderr_reader is not None:
        readers.append(stderr_reader)
    if not timed_out and deadline is not None:
        for reader in readers:
            reader.join(
                max(0.0, deadline - time.monotonic())
            )
        if any(reader.is_alive() for reader in readers):
            timed_out = True
            kill_process_tree(proc)
    for reader in readers:
        reader.join()
    stdout = stdout_capture.text()
    captured_stderr = (
        ""
        if stderr_capture is None
        else stderr_capture.text()
    )

    if timed_out:
        raise ProcessTimeoutError(
            command_args,
            0.0 if timeout is None else float(timeout),
            stdout,
            captured_stderr,
            stdout_truncated=stdout_capture.truncated,
            stderr_truncated=(
                False
                if stderr_capture is None
                else stderr_capture.truncated
            ),
        ) from None

    return CapturedProcess(
        args=command_args,
        returncode=proc.returncode,
        stdout=stdout,
        stderr=captured_stderr,
        stdout_truncated=stdout_capture.truncated,
        stderr_truncated=(
            False
            if stderr_capture is None
            else stderr_capture.truncated
        ),
    )


def print_captured_output(stdout: str, stderr: str = "") -> None:
    if stdout:
        print(stdout, end="", flush=True)
    if stderr:
        print(stderr, end="", flush=True)
