#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Repeat direct LLAM runtime tests for time-bounded core soak coverage."""

from __future__ import annotations

import argparse
import os
import shlex
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from process_utils import ProcessTimeoutError, print_captured_output, run_capture


UINT64_MASK = (1 << 64) - 1


@dataclass(frozen=True)
class SoakCommand:
    name: str
    argv: list[str]
    env: dict[str, str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run repeated direct runtime soak tests.")
    parser.add_argument("--duration", type=float, default=300.0, help="target runtime in seconds")
    parser.add_argument("--timeout", type=float, default=180.0, help="per-command timeout in seconds")
    parser.add_argument("--seed", type=lambda value: int(value, 0), default=0x4C4C414D534F414B)
    parser.add_argument("--fuzz-scenarios", type=int, default=128)
    parser.add_argument("--multi-fuzz-scenarios", type=int, default=64)
    parser.add_argument("--runtime-fuzz", default="./test_runtime_fuzz")
    parser.add_argument("--multi-runtime-core", default="./test_multi_runtime_core")
    parser.add_argument("--runtime-stress", default="./test_runtime_stress")
    parser.add_argument("--runtime-shutdown", default="./test_runtime_shutdown_internal")
    parser.add_argument("--io-buffers", default="./test_io_buffers")
    args = parser.parse_args()
    if args.duration <= 0.0:
        parser.error("--duration must be positive")
    if args.timeout <= 0.0:
        parser.error("--timeout must be positive")
    if args.fuzz_scenarios <= 0:
        parser.error("--fuzz-scenarios must be positive")
    if args.multi_fuzz_scenarios <= 0:
        parser.error("--multi-fuzz-scenarios must be positive")
    return args


def check_executable(path: str) -> None:
    candidate = Path(path)
    if not candidate.exists():
        raise FileNotFoundError(f"missing test binary: {path}")
    if not candidate.is_file():
        raise IsADirectoryError(f"test binary path is not a file: {path}")
    if not os.access(candidate, os.X_OK):
        raise PermissionError(f"test binary is not executable: {path}")


def run_command(command: SoakCommand, timeout: float) -> int:
    env = os.environ.copy()
    env.update(command.env)
    print(f"[runtime_soak] begin {command.name}: {shlex.join(command.argv)}", flush=True)
    try:
        proc = run_capture(command.argv, env=env, timeout=timeout, stderr_to_stdout=True)
    except ProcessTimeoutError as exc:
        print_captured_output(exc.stdout, exc.stderr)
        print(f"[runtime_soak] timeout {command.name} after {timeout:.3f}s", file=sys.stderr)
        return 124
    print_captured_output(proc.stdout, proc.stderr)
    rc = proc.returncode
    print(f"[runtime_soak] end {command.name}: rc={rc}", flush=True)
    return rc


def cycle_commands(args: argparse.Namespace, cycle: int) -> list[SoakCommand]:
    seed = (args.seed + cycle * 0x9E3779B97F4A7C15) & UINT64_MASK
    multi_seed = (seed ^ 0x6D756C7469727466) & UINT64_MASK
    return [
        SoakCommand(
            "runtime_fuzz",
            [args.runtime_fuzz],
            {
                "LLAM_RUNTIME_FUZZ_SEED": str(seed),
                "LLAM_MULTI_RUNTIME_FUZZ_SEED": str(multi_seed),
                "LLAM_RUNTIME_FUZZ_SCENARIOS": str(args.fuzz_scenarios),
                "LLAM_MULTI_RUNTIME_FUZZ_SCENARIOS": str(args.multi_fuzz_scenarios),
            },
        ),
        SoakCommand("multi_runtime_core", [args.multi_runtime_core], {}),
        SoakCommand("runtime_stress", [args.runtime_stress], {}),
        SoakCommand("runtime_shutdown", [args.runtime_shutdown], {}),
        SoakCommand("io_buffers", [args.io_buffers], {}),
    ]


def main() -> int:
    args = parse_args()
    for binary in (
        args.runtime_fuzz,
        args.multi_runtime_core,
        args.runtime_stress,
        args.runtime_shutdown,
        args.io_buffers,
    ):
        check_executable(binary)

    started = time.monotonic()
    deadline = started + args.duration
    cycle = 0
    command_count = 0
    print(
        "[runtime_soak] start "
        f"duration={args.duration:.3f}s timeout={args.timeout:.3f}s seed={args.seed} "
        f"fuzz_scenarios={args.fuzz_scenarios} multi_fuzz_scenarios={args.multi_fuzz_scenarios}",
        flush=True,
    )
    while cycle == 0 or time.monotonic() < deadline:
        print(f"[runtime_soak] cycle {cycle} start", flush=True)
        for command in cycle_commands(args, cycle):
            rc = run_command(command, args.timeout)
            command_count += 1
            if rc != 0:
                elapsed = time.monotonic() - started
                print(
                    f"[runtime_soak] failed cycle={cycle} command={command.name} "
                    f"elapsed={elapsed:.3f}s rc={rc}",
                    file=sys.stderr,
                )
                return rc
        cycle += 1
    elapsed = time.monotonic() - started
    print(f"[runtime_soak] ok cycles={cycle} commands={command_count} elapsed={elapsed:.3f}s", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
