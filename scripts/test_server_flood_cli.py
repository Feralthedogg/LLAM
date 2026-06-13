#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Focused CLI contract tests for server_flood."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path


def fail(message: str, proc: subprocess.CompletedProcess[str] | None = None) -> None:
    print(f"[test_server_flood_cli] {message}", file=sys.stderr)
    if proc is not None:
        print(f"[test_server_flood_cli] command: {' '.join(proc.args)}", file=sys.stderr)
        print(f"[test_server_flood_cli] exit: {proc.returncode}", file=sys.stderr)
        if proc.stdout:
            print(proc.stdout, end="", file=sys.stderr)
        if proc.stderr:
            print(proc.stderr, end="", file=sys.stderr)
    raise SystemExit(1)


def run(binary: Path, args: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    child_env = os.environ.copy()
    if env:
        child_env.update(env)
    return subprocess.run(
        [str(binary), *args],
        env=child_env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def expect_reject(
    binary: Path,
    name: str,
    args: list[str],
    env: dict[str, str] | None = None,
    output_contains: str | None = None,
) -> None:
    proc = run(binary, args, env)
    if proc.returncode == 0:
        fail(f"{name}: command unexpectedly succeeded", proc)
    output = proc.stdout + proc.stderr
    if output_contains is not None and output_contains not in output:
        fail(f"{name}: diagnostic did not contain {output_contains!r}", proc)


def expect_accept(
    binary: Path,
    name: str,
    args: list[str],
    env: dict[str, str] | None = None,
) -> None:
    proc = run(binary, args, env)
    if proc.returncode != 0:
        fail(f"{name}: command unexpectedly failed", proc)


def main(argv: list[str]) -> int:
    binary = Path(argv[1] if len(argv) > 1 else "./server_flood").resolve()

    reject_cases = (
        ("invalid --clients", ["--clients", "8x"], None),
        (
            "resource-exhausting --clients",
            ["--clients", "4294967295", "--duration", "0.001", "--message-bytes", "2", "--batch", "1", "--target-mps", "0"],
            None,
        ),
        (
            "resource-exhausting --batch",
            ["--clients", "2", "--duration", "0.001", "--message-bytes", "2", "--batch", "4294967295", "--target-mps", "0"],
            None,
        ),
        ("invalid --duration", ["--duration", "nan"], None),
        ("out-of-range --duration", ["--duration", "1e100"], None),
        ("resource-exhausting --duration", ["--duration", "1000000000"], None),
        ("sub-nanosecond --duration", ["--duration", "1e-100"], None),
        ("sub-millisecond --duration", ["--duration", "0.0001"], None),
        ("out-of-range --drain-sec", ["--drain-sec", "1e100"], None),
        ("resource-exhausting --drain-sec", ["--drain-sec", "1000000000"], None),
        ("out-of-range --shutdown-timeout", ["--shutdown-timeout", "1e100"], None),
        ("resource-exhausting --shutdown-timeout", ["--shutdown-timeout", "1000000000"], None),
        ("sub-nanosecond --shutdown-timeout", ["--shutdown-timeout", "1e-100"], None),
    )
    for name, args, output_contains in reject_cases:
        expect_reject(binary, name, args, output_contains=output_contains)

    expect_reject(
        binary,
        "invalid LLAM_SERVER_FLOOD_CLIENTS",
        ["--host", "not-an-ip"],
        {"LLAM_SERVER_FLOOD_CLIENTS": "-1"},
        "LLAM_SERVER_FLOOD_CLIENTS",
    )
    expect_reject(
        binary,
        "invalid LLAM_SERVER_FLOOD_DURATION",
        ["--host", "not-an-ip"],
        {"LLAM_SERVER_FLOOD_DURATION": "nan"},
        "LLAM_SERVER_FLOOD_DURATION",
    )
    expect_accept(
        binary,
        "false/no/off boolean environment values",
        ["--help"],
        {
            "LLAM_SERVER_FLOOD_ALLOW_FORCED_STOP": "false",
            "LLAM_SERVER_FLOOD_ALLOW_MISSING_STATS": "no",
            "LLAM_SERVER_FLOOD_SERVER_LOSSLESS": "off",
        },
    )
    expect_reject(
        binary,
        "invalid boolean environment value",
        ["--help"],
        {"LLAM_SERVER_FLOOD_ALLOW_FORCED_STOP": "maybe"},
        "LLAM_SERVER_FLOOD_ALLOW_FORCED_STOP must be a boolean token",
    )

    print("[test_server_flood_cli] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
