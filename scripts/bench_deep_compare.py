#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0.
# See LICENSE.

"""Run an isolated, multi-axis LLAM/Go/Tokio benchmark investigation.

This complements bench_runtime_compare.py.  The normal comparison script is
the release-facing summary runner; this script is deliberately diagnostic and
adds workload scaling, competitor worker-width sweeps, timer-duration sweeps,
and LLAM scheduler fast-path ablations.  Every measurement is a fresh process
and the direct Go/Tokio binaries are built once so compiler startup is excluded.
"""

from __future__ import annotations

import argparse
import csv
import os
import pathlib
import platform
import re
import resource
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass, field


FIELD_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s]+)")
RUNTIME_PREFIX = {
    "LLAM": "[bench] ",
    "Goroutine": "[go-bench] ",
    "Tokio": "[tokio-bench] ",
}
SAFE_CASES = (
    "spawn_join",
    "channel_pingpong",
    "select_recv_ready",
    "select_park_wake",
    "io_echo",
    "poll_wake",
    "sleep_fanout",
    "opaque_block",
)


@dataclass(frozen=True)
class Scenario:
    name: str
    cases: tuple[str, ...]
    env: dict[str, str] = field(default_factory=dict)
    runtimes: tuple[str, ...] = ("LLAM", "Goroutine", "Tokio")
    note: str = ""


@dataclass
class Sample:
    scenario: str
    case: str
    runtime: str
    sample: int
    rounds: int
    ops_per_round: int
    ops_per_sec: float
    p50_us: float
    p99_us: float
    process_wall_sec: float
    process_cpu_sec: float
    cpu_ns_per_op: float
    note: str


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def build_scenarios(quick: bool) -> list[Scenario]:
    scenarios = [
        Scenario(
            "baseline",
            SAFE_CASES,
            note="default runtime widths and policies",
        ),
        Scenario(
            "select_timeout_semantics",
            ("select_timeout",),
            {"LLAM_BENCH_SELECT_OPS": "128"},
            note="semantic-mismatch diagnostic only; reduced work avoids timer-path domination",
        ),
    ]

    spawn_sizes = (128, 512) if quick else (32, 128, 512, 2048)
    for tasks in spawn_sizes:
        scenarios.append(
            Scenario(
                f"spawn_tasks_{tasks}",
                ("spawn_join",),
                {"LLAM_BENCH_SPAWN_TASKS": str(tasks)},
                note="spawn/yield/join scaling",
            )
        )

    sleep_durations = (1000, 30000) if quick else (100, 1000, 30000)
    for sleep_us in sleep_durations:
        scenarios.append(
            Scenario(
                f"sleep_{sleep_us}us",
                ("sleep_fanout",),
                {
                    "LLAM_BENCH_SLEEP_US": str(sleep_us),
                    "LLAM_BENCH_SLEEP_TASKS": "512",
                },
                note="512-task timer fanout; shorter durations expose runtime overhead",
            )
        )

    worker_widths = (1,) if quick else (1, 2, max(1, os.cpu_count() or 1))
    for workers in dict.fromkeys(worker_widths):
        scenarios.append(
            Scenario(
                f"competitor_workers_{workers}",
                ("spawn_join", "channel_pingpong", "select_park_wake", "io_echo"),
                {
                    "GO_BENCH_GOMAXPROCS": str(workers),
                    "TOKIO_BENCH_WORKER_THREADS": str(workers),
                },
                note="Go/Tokio worker width fixed; LLAM uses its normal dynamic policy",
            )
        )

    ablations = (
        ("llam_handoff_default", {}),
        ("llam_yield_handoff_off", {"LLAM_YIELD_DIRECT_HANDOFF": "0"}),
        ("llam_wake_handoff_off", {"LLAM_WAKE_HANDOFF": "0"}),
        (
            "llam_all_handoff_off",
            {"LLAM_YIELD_DIRECT_HANDOFF": "0", "LLAM_WAKE_HANDOFF": "0"},
        ),
    )
    if quick:
        ablations = (ablations[0], ablations[-1])
    for name, env in ablations:
        scenarios.append(
            Scenario(
                name,
                ("spawn_join", "channel_pingpong", "select_park_wake", "io_echo"),
                env,
                ("LLAM",),
                note="LLAM fast-path ablation",
            )
        )

    queue_ablations = (
        ("llam_scheduler_default", {}),
        ("llam_lockfree_normq_off", {"LLAM_EXPERIMENTAL_LOCKFREE_NORMQ": "0"}),
        ("llam_dynamic_workers_off", {"LLAM_EXPERIMENTAL_DYNAMIC_WORKERS": "0"}),
    )
    if quick:
        queue_ablations = queue_ablations[:1]
    for name, env in queue_ablations:
        scenarios.append(
            Scenario(
                name,
                ("spawn_join", "channel_pingpong", "select_park_wake"),
                env,
                ("LLAM",),
                note="LLAM scheduler-policy ablation",
            )
        )
    return scenarios


def build_binaries(root: pathlib.Path, out_dir: pathlib.Path) -> dict[str, list[str]]:
    bin_dir = out_dir / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    go_binary = bin_dir / "go_bench"

    subprocess.run(["make", "-j4", "bench"], cwd=root, check=True)
    subprocess.run(
        ["go", "build", "-o", str(go_binary), "scripts/bench_go_compare.go"],
        cwd=root,
        check=True,
    )
    subprocess.run(
        [
            "cargo",
            "build",
            "--release",
            "--manifest-path",
            "scripts/bench_tokio_compare/Cargo.toml",
        ],
        cwd=root,
        check=True,
    )
    tokio_binary = root / "scripts" / "bench_tokio_compare" / "target" / "release" / "bench_tokio_compare"
    return {
        "LLAM": [str(root / "bench")],
        "Goroutine": [str(go_binary)],
        "Tokio": [str(tokio_binary)],
    }


def existing_binaries(root: pathlib.Path, out_dir: pathlib.Path) -> dict[str, list[str]]:
    commands = {
        "LLAM": [str(root / "bench")],
        "Goroutine": [str(out_dir / "bin" / "go_bench")],
        "Tokio": [
            str(root / "scripts" / "bench_tokio_compare" / "target" / "release" / "bench_tokio_compare")
        ],
    }
    missing = [command[0] for command in commands.values() if not pathlib.Path(command[0]).is_file()]
    if missing:
        raise SystemExit(f"--no-build requested but binaries are missing: {', '.join(missing)}")
    return commands


def parse_report(runtime: str, case: str, output: str) -> tuple[int, int, float, float, float]:
    prefix = RUNTIME_PREFIX[runtime]
    matches: list[dict[str, str]] = []
    for line in output.splitlines():
        if not line.startswith(prefix):
            continue
        fields = dict(FIELD_RE.findall(line))
        name = fields.get("name", "")
        if name in {case, "poll_wake_approx", "opaque_syscall_sleep_approx", "opaque_block_in_place_approx"}:
            matches.append(fields)
    if len(matches) != 1:
        raise RuntimeError(f"expected one {runtime}/{case} result, got {len(matches)}\n{output}")
    fields = matches[0]
    return (
        int(fields["rounds"]),
        int(fields["ops"]),
        float(fields["ops_per_sec"]),
        float(fields["p50_us"]),
        float(fields["p99_us"]),
    )


def run_one(
    root: pathlib.Path,
    command: list[str],
    runtime: str,
    scenario: Scenario,
    case: str,
    sample_index: int,
    base_env: dict[str, str],
    timeout: int,
) -> Sample:
    env = base_env.copy()
    env.update(scenario.env)
    env["LLAM_BENCH_ONLY"] = case
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    wall_start = time.monotonic()
    proc = subprocess.run(
        command,
        cwd=root,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )
    wall_sec = time.monotonic() - wall_start
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu_sec = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    if proc.returncode != 0:
        raise RuntimeError(
            f"{runtime}/{scenario.name}/{case} failed with {proc.returncode}\n{proc.stdout}\n{proc.stderr}"
        )
    rounds, ops, ops_per_sec, p50_us, p99_us = parse_report(runtime, case, proc.stdout)
    total_ops = rounds * ops
    cpu_ns_per_op = cpu_sec * 1_000_000_000.0 / total_ops if total_ops else 0.0
    return Sample(
        scenario=scenario.name,
        case=case,
        runtime=runtime,
        sample=sample_index,
        rounds=rounds,
        ops_per_round=ops,
        ops_per_sec=ops_per_sec,
        p50_us=p50_us,
        p99_us=p99_us,
        process_wall_sec=wall_sec,
        process_cpu_sec=cpu_sec,
        cpu_ns_per_op=cpu_ns_per_op,
        note=scenario.note,
    )


def write_raw(path: pathlib.Path, samples: list[Sample]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "scenario",
                "case",
                "runtime",
                "sample",
                "rounds",
                "ops_per_round",
                "ops_per_sec",
                "p50_us",
                "p99_us",
                "process_wall_sec",
                "process_cpu_sec",
                "cpu_ns_per_op",
                "note",
            ]
        )
        for row in samples:
            writer.writerow(
                [
                    row.scenario,
                    row.case,
                    row.runtime,
                    row.sample,
                    row.rounds,
                    row.ops_per_round,
                    f"{row.ops_per_sec:.2f}",
                    f"{row.p50_us:.2f}",
                    f"{row.p99_us:.2f}",
                    f"{row.process_wall_sec:.6f}",
                    f"{row.process_cpu_sec:.6f}",
                    f"{row.cpu_ns_per_op:.2f}",
                    row.note,
                ]
            )


def summarize(samples: list[Sample]) -> list[dict[str, object]]:
    grouped: dict[tuple[str, str, str], list[Sample]] = {}
    for sample in samples:
        grouped.setdefault((sample.scenario, sample.case, sample.runtime), []).append(sample)

    rows: list[dict[str, object]] = []
    for (scenario, case, runtime), values in sorted(grouped.items()):
        throughputs = [value.ops_per_sec for value in values]
        rows.append(
            {
                "scenario": scenario,
                "case": case,
                "runtime": runtime,
                "samples": len(values),
                "ops_per_sec": statistics.median(throughputs),
                "p50_us": statistics.median(value.p50_us for value in values),
                "p99_us": statistics.median(value.p99_us for value in values),
                "cpu_ns_per_op": statistics.median(value.cpu_ns_per_op for value in values),
                "spread": max(throughputs) / min(throughputs),
                "note": values[0].note,
            }
        )
    return rows


def write_summary(path: pathlib.Path, rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
        writer.writeheader()
        for row in rows:
            formatted = row.copy()
            for key in ("ops_per_sec", "p50_us", "p99_us", "cpu_ns_per_op", "spread"):
                formatted[key] = f"{float(row[key]):.2f}"
            writer.writerow(formatted)


def write_markdown(path: pathlib.Path, rows: list[dict[str, object]], samples: int, rounds: int, warmup: int) -> None:
    by_key = {(str(row["scenario"]), str(row["case"]), str(row["runtime"])): row for row in rows}
    lines = [
        "# LLAM Deep Benchmark Report",
        "",
        f"- Host: `{platform.platform()}`",
        f"- CPU count: `{os.cpu_count()}`",
        f"- Process samples: `{samples}`; measured rounds: `{rounds}`; warmup rounds: `{warmup}`",
        "- Selection: median process-level throughput; every sample ran in a fresh process.",
        "- CPU ns/op is whole-process child CPU time divided by reported operations; use it as a diagnostic, not a hardware counter.",
        "",
        "## Baseline",
        "",
        "| Case | LLAM ops/s | Go ops/s | Tokio ops/s | LLAM/Go | LLAM/Tokio |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for case in SAFE_CASES + ("select_timeout",):
        scenario = "select_timeout_semantics" if case == "select_timeout" else "baseline"
        values = [by_key.get((scenario, case, runtime)) for runtime in ("LLAM", "Goroutine", "Tokio")]
        if any(value is None for value in values):
            continue
        llam, go, tokio = values
        assert llam is not None and go is not None and tokio is not None
        llam_ops = float(llam["ops_per_sec"])
        go_ops = float(go["ops_per_sec"])
        tokio_ops = float(tokio["ops_per_sec"])
        lines.append(
            f"| {case} | {llam_ops:,.0f} | {go_ops:,.0f} | {tokio_ops:,.0f} | {llam_ops/go_ops:.2f}x | {llam_ops/tokio_ops:.2f}x |"
        )
    lines.extend(
        [
            "",
            "> `select_timeout` combines different API semantics and is excluded from general-performance conclusions.",
            "",
            "## All Scenario Medians",
            "",
            "| Scenario | Case | Runtime | ops/s | p50 us | p99 us | CPU ns/op | spread |",
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for row in rows:
        lines.append(
            f"| {row['scenario']} | {row['case']} | {row['runtime']} | "
            f"{float(row['ops_per_sec']):,.0f} | {float(row['p50_us']):.2f} | "
            f"{float(row['p99_us']):.2f} | {float(row['cpu_ns_per_op']):.1f} | {float(row['spread']):.2f}x |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=positive_int, default=5)
    parser.add_argument("--rounds", type=positive_int, default=11)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--timeout", type=positive_int, default=180)
    parser.add_argument("--out-dir", default="object/bench_deep")
    parser.add_argument("--quick", action="store_true", help="run a reduced matrix for harness verification")
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    if args.warmup < 0:
        parser.error("--warmup must be non-negative")

    root = pathlib.Path(__file__).resolve().parents[1]
    out_dir = root / args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    commands = existing_binaries(root, out_dir) if args.no_build else build_binaries(root, out_dir)
    scenarios = build_scenarios(args.quick)
    base_env = os.environ.copy()
    base_env.update(
        {
            "LLAM_BENCH_ROUNDS": str(args.rounds),
            "LLAM_BENCH_WARMUP_ROUNDS": str(args.warmup),
            "LLAM_BENCH_SPAWN_TASKS": "512",
            "LLAM_BENCH_CHANNEL_MESSAGES": "4096",
            "LLAM_BENCH_SELECT_OPS": "4096",
            "LLAM_BENCH_IO_MESSAGES": "512",
            "LLAM_BENCH_POLL_EVENTS": "512",
            "LLAM_BENCH_SLEEP_TASKS": "1024",
            "LLAM_BENCH_OPAQUE_SCOPES": "64",
        }
    )

    all_samples: list[Sample] = []
    total_runs = sum(len(scenario.cases) * len(scenario.runtimes) * args.samples for scenario in scenarios)
    completed = 0
    for scenario in scenarios:
        for case in scenario.cases:
            for sample_index in range(1, args.samples + 1):
                runtimes = list(scenario.runtimes)
                rotation = (sample_index - 1) % len(runtimes)
                runtimes = runtimes[rotation:] + runtimes[:rotation]
                for runtime in runtimes:
                    completed += 1
                    print(
                        f"[bench-deep] {completed}/{total_runs} {scenario.name} {case} {runtime} sample={sample_index}",
                        file=sys.stderr,
                        flush=True,
                    )
                    all_samples.append(
                        run_one(
                            root,
                            commands[runtime],
                            runtime,
                            scenario,
                            case,
                            sample_index,
                            base_env,
                            args.timeout,
                        )
                    )

    raw_path = out_dir / "deep_samples.csv"
    summary_path = out_dir / "deep_summary.csv"
    report_path = out_dir / "deep_report.md"
    write_raw(raw_path, all_samples)
    summary_rows = summarize(all_samples)
    write_summary(summary_path, summary_rows)
    write_markdown(report_path, summary_rows, args.samples, args.rounds, args.warmup)
    print(f"Raw samples: {raw_path}")
    print(f"Summary: {summary_path}")
    print(f"Report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
