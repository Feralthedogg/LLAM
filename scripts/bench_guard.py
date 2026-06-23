#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run LLAM microbenchmarks and fail on catastrophic throughput regressions."""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from pathlib import Path

from cli_numbers import (
    env_default,
    finite_nonnegative_float,
    finite_positive_float_at_most,
    nonnegative_int_at_most,
    positive_int_at_most,
)
from process_utils import ProcessTimeoutError, print_captured_output, run_capture
from safe_output import write_text_safely


DEFAULT_CASES = [
    "spawn_join",
    "channel_pingpong",
    "select_recv_ready",
    "select_park_wake",
    "select_timeout",
    "io_echo",
    "poll_wake",
]

DEFAULT_MIN_OPS = {
    "spawn_join": 500_000.0,
    "channel_pingpong": 1_500_000.0,
    "select_recv_ready": 8_000_000.0,
    "select_park_wake": 800_000.0,
    "select_timeout": 8_000_000.0,
    "io_echo": 150_000.0,
    "poll_wake": 200_000.0,
}

FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
MAX_BENCH_GUARD_TIMEOUT_SEC = 3600.0
MAX_BENCH_GUARD_ROUNDS = 10_000
MAX_BENCH_GUARD_WARMUP = 1_000
MAX_BENCH_GUARD_WORK = 1_000_000


def parse_min_ops(raw: str | None) -> dict[str, float]:
    values = dict(DEFAULT_MIN_OPS)
    seen: set[str] = set()

    if not raw:
        return values
    for entry in raw.split(","):
        if not entry:
            continue
        if "=" not in entry:
            raise SystemExit(f"invalid --min-ops entry {entry!r}; expected case=value")
        name, value = entry.split("=", 1)
        name = name.strip()
        if name not in DEFAULT_MIN_OPS:
            raise SystemExit(f"invalid --min-ops entry {entry!r}: unknown benchmark case {name!r}")
        if name in seen:
            raise SystemExit(f"invalid --min-ops entry {entry!r}: duplicate benchmark case {name!r}")
        seen.add(name)
        try:
            values[name] = finite_nonnegative_float(value)
        except argparse.ArgumentTypeError as exc:
            raise SystemExit(f"invalid --min-ops entry {entry!r}: {exc}") from exc
    return values


def parse_cases(raw: str) -> list[str]:
    cases = [case.strip() for case in raw.split(",") if case.strip()]
    unknown = [case for case in cases if case not in DEFAULT_CASES]
    seen: set[str] = set()
    duplicates: list[str] = []

    for case in cases:
        if case in seen and case not in duplicates:
            duplicates.append(case)
        seen.add(case)

    if not cases:
        raise argparse.ArgumentTypeError("at least one benchmark case is required")
    if unknown:
        raise argparse.ArgumentTypeError(f"unknown benchmark case(s): {', '.join(unknown)}")
    if duplicates:
        raise argparse.ArgumentTypeError(f"duplicate benchmark case(s): {', '.join(duplicates)}")
    return cases


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}

    for key, value in FIELD_RE.findall(line):
        if key in fields:
            raise ValueError(f"duplicate benchmark field {key}: {line!r}")
        fields[key] = value
    return fields


def parse_bench_output(text: str) -> dict[str, float]:
    results: dict[str, float] = {}

    for line in text.splitlines():
        if not line.startswith("[bench] "):
            continue
        fields = parse_fields(line)
        if "name" not in fields:
            continue
        name = fields["name"]
        if "ops_per_sec" not in fields:
            raise ValueError(f"benchmark row missing ops_per_sec: {line!r}")
        try:
            ops = float(fields["ops_per_sec"])
        except ValueError as exc:
            raise ValueError(f"invalid ops_per_sec in benchmark row: {line!r}") from exc
        if not math.isfinite(ops) or ops <= 0.0:
            raise ValueError(f"non-positive or non-finite ops_per_sec in benchmark row: {line!r}")
        if name in results:
            raise ValueError(f"duplicate benchmark row for {name}: {line!r}")
        results[name] = ops
    return results


def run_case(bench: Path, case: str, args: argparse.Namespace) -> tuple[float, str]:
    env = os.environ.copy()

    env.update(
        {
            "LLAM_BENCH_ONLY": case,
            "LLAM_BENCH_ROUNDS": str(args.rounds),
            "LLAM_BENCH_WARMUP_ROUNDS": str(args.warmup),
            "LLAM_BENCH_SPAWN_TASKS": str(args.spawn_tasks),
            "LLAM_BENCH_CHANNEL_MESSAGES": str(args.channel_messages),
            "LLAM_BENCH_SELECT_OPS": str(args.select_ops),
            "LLAM_BENCH_IO_MESSAGES": str(args.io_messages),
            "LLAM_BENCH_POLL_EVENTS": str(args.poll_events),
            "LLAM_BENCH_SLEEP_TASKS": str(args.sleep_tasks),
            "LLAM_BENCH_OPAQUE_SCOPES": str(args.opaque_scopes),
        }
    )
    try:
        proc = run_capture(
            [str(bench)],
            env=env,
            timeout=args.timeout,
            stderr_to_stdout=True,
        )
    except ProcessTimeoutError as exc:
        print_captured_output(exc.stdout, exc.stderr)
        raise RuntimeError(f"{case}: benchmark timed out after {args.timeout:.3f}s") from None
    print_captured_output(proc.stdout, proc.stderr)
    if proc.returncode != 0:
        raise RuntimeError(f"{case}: benchmark exited with rc={proc.returncode}")
    try:
        parsed = parse_bench_output(proc.stdout)
    except ValueError as exc:
        raise RuntimeError(f"{case}: failed to parse benchmark output: {exc}") from None
    if case not in parsed:
        raise RuntimeError(f"{case}: benchmark output did not contain ops_per_sec")
    return parsed[case], proc.stdout

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", default="./bench", type=Path)
    parser.add_argument("--cases", default=",".join(DEFAULT_CASES), help="comma-separated benchmark cases")
    parser.add_argument("--min-ops", default=os.getenv("LLAM_BENCH_GUARD_MIN_OPS"), help="case=value comma list")
    parser.add_argument("--rounds",
                        type=positive_int_at_most(MAX_BENCH_GUARD_ROUNDS),
                        default=env_default(parser,
                                            "LLAM_BENCH_GUARD_ROUNDS",
                                            "3",
                                            positive_int_at_most(MAX_BENCH_GUARD_ROUNDS)))
    parser.add_argument("--warmup",
                        type=nonnegative_int_at_most(MAX_BENCH_GUARD_WARMUP),
                        default=env_default(parser,
                                            "LLAM_BENCH_GUARD_WARMUP",
                                            "1",
                                            nonnegative_int_at_most(MAX_BENCH_GUARD_WARMUP)))
    parser.add_argument("--timeout",
                        type=finite_positive_float_at_most(MAX_BENCH_GUARD_TIMEOUT_SEC),
                        default=env_default(parser,
                                            "LLAM_BENCH_GUARD_TIMEOUT",
                                            "180",
                                            finite_positive_float_at_most(MAX_BENCH_GUARD_TIMEOUT_SEC)))
    parser.add_argument("--spawn-tasks", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=128)
    parser.add_argument("--channel-messages", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=1024)
    parser.add_argument("--select-ops", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=512)
    parser.add_argument("--io-messages", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=128)
    parser.add_argument("--poll-events", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=128)
    parser.add_argument("--sleep-tasks", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=256)
    parser.add_argument("--opaque-scopes", type=positive_int_at_most(MAX_BENCH_GUARD_WORK), default=16)
    parser.add_argument("--out", type=Path, default=None, help="optional JSON result path")
    args = parser.parse_args()

    try:
        cases = parse_cases(args.cases)
    except argparse.ArgumentTypeError as exc:
        raise SystemExit(f"invalid --cases: {exc}") from exc
    min_ops = parse_min_ops(args.min_ops)
    results = {}
    failures = []

    bench_path = args.bench if args.bench.is_absolute() else Path.cwd() / args.bench
    if not bench_path.exists():
        raise SystemExit(f"bench binary not found: {args.bench}")
    for case in cases:
        try:
            ops, _output = run_case(bench_path, case, args)
        except RuntimeError as exc:
            print(f"[bench-guard] fail: {exc}", file=sys.stderr)
            return 1
        minimum = min_ops.get(case, 0.0)
        ok = ops >= minimum
        results[case] = {
            "ops_per_sec": ops,
            "min_ops_per_sec": minimum,
            "ok": ok,
        }
        if not ok:
            failures.append(f"{case}: {ops:.2f} < {minimum:.2f}")

    if args.out is not None:
        write_text_safely(args.out, json.dumps(results, indent=2, sort_keys=True) + "\n")

    if failures:
        for failure in failures:
            print(f"[bench-guard] fail: {failure}", file=sys.stderr)
        return 1
    print(f"[bench-guard] ok cases={len(cases)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
