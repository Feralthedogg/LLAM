#!/usr/bin/env python3
"""Run LLAM microbenchmarks and fail on catastrophic throughput regressions."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path


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

BENCH_RE = re.compile(r"^\[bench\]\s+name=(?P<name>\S+).*?\sops_per_sec=(?P<ops>[0-9.]+)")


def parse_min_ops(raw: str | None) -> dict[str, float]:
    values = dict(DEFAULT_MIN_OPS)

    if not raw:
        return values
    for entry in raw.split(","):
        if not entry:
            continue
        if "=" not in entry:
            raise SystemExit(f"invalid --min-ops entry {entry!r}; expected case=value")
        name, value = entry.split("=", 1)
        values[name.strip()] = float(value)
    return values


def parse_bench_output(text: str) -> dict[str, float]:
    results: dict[str, float] = {}

    for line in text.splitlines():
        match = BENCH_RE.search(line)
        if match:
            results[match.group("name")] = float(match.group("ops"))
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
    proc = subprocess.run(
        [str(bench)],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=args.timeout,
    )
    print(proc.stdout, end="")
    if proc.returncode != 0:
        raise RuntimeError(f"{case}: benchmark exited with rc={proc.returncode}")
    parsed = parse_bench_output(proc.stdout)
    if case not in parsed:
        raise RuntimeError(f"{case}: benchmark output did not contain ops_per_sec")
    return parsed[case], proc.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", default="./bench", type=Path)
    parser.add_argument("--cases", default=",".join(DEFAULT_CASES), help="comma-separated benchmark cases")
    parser.add_argument("--min-ops", default=os.getenv("LLAM_BENCH_GUARD_MIN_OPS"), help="case=value comma list")
    parser.add_argument("--rounds", type=int, default=int(os.getenv("LLAM_BENCH_GUARD_ROUNDS", "3")))
    parser.add_argument("--warmup", type=int, default=int(os.getenv("LLAM_BENCH_GUARD_WARMUP", "1")))
    parser.add_argument("--timeout", type=float, default=float(os.getenv("LLAM_BENCH_GUARD_TIMEOUT", "180")))
    parser.add_argument("--spawn-tasks", type=int, default=128)
    parser.add_argument("--channel-messages", type=int, default=1024)
    parser.add_argument("--select-ops", type=int, default=512)
    parser.add_argument("--io-messages", type=int, default=128)
    parser.add_argument("--poll-events", type=int, default=128)
    parser.add_argument("--sleep-tasks", type=int, default=256)
    parser.add_argument("--opaque-scopes", type=int, default=16)
    parser.add_argument("--out", type=Path, default=None, help="optional JSON result path")
    args = parser.parse_args()

    cases = [case.strip() for case in args.cases.split(",") if case.strip()]
    min_ops = parse_min_ops(args.min_ops)
    results = {}
    failures = []

    bench_path = args.bench if args.bench.is_absolute() else Path.cwd() / args.bench
    if not bench_path.exists():
        raise SystemExit(f"bench binary not found: {args.bench}")
    for case in cases:
        ops, _output = run_case(bench_path, case, args)
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
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if failures:
        for failure in failures:
            print(f"[bench-guard] fail: {failure}", file=sys.stderr)
        return 1
    print(f"[bench-guard] ok cases={len(cases)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
