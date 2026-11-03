#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/Apache-2.0.txt.

"""Compare LLAM CPU-spike latency with Go asynchronous preemption."""

from __future__ import annotations

import argparse
import csv
import math
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from cli_numbers import positive_int_at_most
from process_utils import ProcessTimeoutError, run_capture
from safe_output import open_text_for_write, prepare_output_dir, prepare_output_path


FIELD_TOKEN_RE = re.compile(r"([A-Za-z0-9_]+)=([^\s=]+)")
METRICS = ("timer", "io", "control")
EXPECTED_FIELDS = {
    "runtime",
    "metric",
    "workers",
    "rounds",
    "spike_ms",
    "settle_ms",
    "p50_us",
    "p99_us",
    "max_us",
}
MAX_TIMEOUT_SECONDS = 3600


@dataclass(frozen=True)
class SpikeRow:
    runtime: str
    metric: str
    workers: int
    rounds: int
    spike_ms: int
    settle_ms: int
    p50_us: float
    p99_us: float
    max_us: float


@dataclass(frozen=True)
class ComparisonRow:
    metric: str
    p50_ratio: float
    p99_ratio: float
    max_ratio: float


def _fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    prefix = "[cpu-spike] "

    if not line.startswith(prefix):
        raise ValueError(f"invalid CPU-spike row prefix: {line!r}")
    tokens = line[len(prefix) :].split()
    if not tokens:
        raise ValueError(f"empty CPU-spike row: {line!r}")
    for token in tokens:
        match = FIELD_TOKEN_RE.fullmatch(token)
        if match is None:
            raise ValueError(f"invalid CPU-spike field token {token!r}: {line!r}")
        key, value = match.groups()
        if key in fields:
            raise ValueError(f"duplicate field {key}: {line!r}")
        fields[key] = value
    if set(fields) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(fields))
        extra = sorted(set(fields) - EXPECTED_FIELDS)
        raise ValueError(f"invalid fields missing={missing} extra={extra}: {line!r}")
    return fields


def _positive_int(fields: dict[str, str], name: str, line: str) -> int:
    raw = fields[name]
    if not re.fullmatch(r"[0-9]+", raw):
        raise ValueError(f"invalid {name}: {line!r}")
    value = int(raw, 10)
    if value <= 0:
        raise ValueError(f"non-positive {name}: {line!r}")
    return value


def _latency(fields: dict[str, str], name: str, line: str) -> float:
    try:
        value = float(fields[name])
    except ValueError as exc:
        raise ValueError(f"invalid {name}: {line!r}") from exc
    if not math.isfinite(value) or value < 0.0:
        raise ValueError(f"invalid {name}: {line!r}")
    return value


def parse_output(runtime: str, output: str) -> list[SpikeRow]:
    rows: dict[str, SpikeRow] = {}
    encountered: list[str] = []

    for line in output.splitlines():
        if not line.startswith("[cpu-spike] "):
            continue
        fields = _fields(line)
        metric = fields["metric"]
        if fields["runtime"] != runtime:
            raise ValueError(f"expected runtime {runtime}, got {fields['runtime']}: {line!r}")
        if metric not in METRICS:
            raise ValueError(f"unknown metric {metric}: {line!r}")
        if metric in rows:
            raise ValueError(f"duplicate metric {metric}: {line!r}")
        row = SpikeRow(
            runtime=runtime,
            metric=metric,
            workers=_positive_int(fields, "workers", line),
            rounds=_positive_int(fields, "rounds", line),
            spike_ms=_positive_int(fields, "spike_ms", line),
            settle_ms=_positive_int(fields, "settle_ms", line),
            p50_us=_latency(fields, "p50_us", line),
            p99_us=_latency(fields, "p99_us", line),
            max_us=_latency(fields, "max_us", line),
        )
        if row.p50_us > row.p99_us or row.p99_us > row.max_us:
            raise ValueError(f"latencies are not ordered p50 <= p99 <= max: {line!r}")
        rows[metric] = row
        encountered.append(metric)

    if set(rows) != set(METRICS):
        raise ValueError(f"expected metrics {METRICS}, got {tuple(rows)}")
    if tuple(encountered) != METRICS:
        raise ValueError(f"expected metric row order {METRICS}, got {tuple(encountered)}")
    ordered = [rows[metric] for metric in METRICS]
    configs = {(row.workers, row.rounds, row.spike_ms, row.settle_ms) for row in ordered}
    if len(configs) != 1:
        raise ValueError("CPU-spike rows disagree on workers, rounds, spike_ms, or settle_ms")
    return ordered


def _ratio(numerator: float, denominator: float) -> float:
    if denominator == 0.0:
        return math.inf if numerator > 0.0 else 1.0
    return numerator / denominator


def compare_rows(llam_rows: list[SpikeRow], go_rows: list[SpikeRow]) -> list[ComparisonRow]:
    llam_by_metric = {row.metric: row for row in llam_rows}
    go_by_metric = {row.metric: row for row in go_rows}
    if set(llam_by_metric) != set(METRICS) or set(go_by_metric) != set(METRICS):
        raise ValueError("comparison requires timer, io, and control rows from both runtimes")
    if llam_rows[0].workers != go_rows[0].workers:
        raise ValueError("LLAM and Go worker counts differ")
    if (
        llam_rows[0].rounds != go_rows[0].rounds
        or llam_rows[0].spike_ms != go_rows[0].spike_ms
        or llam_rows[0].settle_ms != go_rows[0].settle_ms
    ):
        raise ValueError("LLAM and Go round/spike/settle configurations differ")

    return [
        ComparisonRow(
            metric=metric,
            p50_ratio=_ratio(llam_by_metric[metric].p50_us, go_by_metric[metric].p50_us),
            p99_ratio=_ratio(llam_by_metric[metric].p99_us, go_by_metric[metric].p99_us),
            max_ratio=_ratio(llam_by_metric[metric].max_us, go_by_metric[metric].max_us),
        )
        for metric in METRICS
    ]


def _run_program(root: Path, command: list[str], env: dict[str, str], timeout: float) -> str:
    try:
        result = run_capture(command, cwd=root, env=env, timeout=timeout)
    except ProcessTimeoutError as exc:
        raise RuntimeError(f"CPU-spike command timed out: {' '.join(command)}\n{exc.stdout}{exc.stderr}") from exc
    except OSError as exc:
        raise RuntimeError(f"CPU-spike command could not start: {' '.join(command)}: {exc}") from exc
    if result.returncode != 0:
        raise RuntimeError(
            f"CPU-spike command failed with exit {result.returncode}: {' '.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )
    if result.stderr:
        sys.stderr.write(result.stderr)
    return result.stdout


def force_go_async_preemption(godebug: str) -> str:
    settings = [item for item in godebug.split(",") if item and not item.startswith("asyncpreemptoff=")]
    settings.append("asyncpreemptoff=0")
    return ",".join(settings)


def run_pair(
    root: Path,
    llam_command: list[str],
    go_command: list[str],
    env_overrides: dict[str, str],
    *,
    timeout: float,
) -> tuple[list[SpikeRow], list[SpikeRow]]:
    base_env = os.environ.copy()
    base_env.update(env_overrides)
    llam_rows = parse_output("LLAM", _run_program(root, llam_command, base_env, timeout))
    _validate_requested_config(llam_rows, env_overrides)

    go_env = base_env.copy()
    go_env["LLAM_CPU_SPIKE_WORKERS"] = str(llam_rows[0].workers)
    go_rows = parse_output("Go", _run_program(root, go_command, go_env, timeout))
    _validate_requested_config(go_rows, go_env)
    return llam_rows, go_rows


def _validate_requested_config(rows: list[SpikeRow], env: dict[str, str]) -> None:
    first = rows[0]
    expected = {
        "LLAM_CPU_SPIKE_WORKERS": first.workers,
        "LLAM_CPU_SPIKE_ROUNDS": first.rounds,
        "LLAM_CPU_SPIKE_MS": first.spike_ms,
        "LLAM_CPU_SPIKE_SETTLE_MS": first.settle_ms,
    }
    for name, actual in expected.items():
        raw = env.get(name)
        if raw is None:
            continue
        try:
            requested = int(raw, 10)
        except ValueError as exc:
            raise ValueError(f"invalid requested {name}={raw!r}") from exc
        if requested != actual:
            raise ValueError(f"{rows[0].runtime} reported {name}={actual}, requested {requested}")


def format_summary(
    llam_rows: list[SpikeRow],
    go_rows: list[SpikeRow],
    comparisons: list[ComparisonRow],
) -> str:
    llam_by_metric = {row.metric: row for row in llam_rows}
    go_by_metric = {row.metric: row for row in go_rows}
    compare_by_metric = {row.metric: row for row in comparisons}
    lines = [
        "metric   LLAM p50 us   Go p50 us   LLAM/Go p50   "
        "LLAM p99 us   Go p99 us   LLAM/Go p99   LLAM max us   Go max us   LLAM/Go max"
    ]
    for metric in METRICS:
        llam_row = llam_by_metric[metric]
        go_row = go_by_metric[metric]
        comparison = compare_by_metric[metric]
        lines.append(
            f"{metric:<9}  {llam_row.p50_us:>9.2f}  {go_row.p50_us:>11.2f}  {comparison.p50_ratio:>13.2f}x  "
            f"{llam_row.p99_us:>11.2f}  {go_row.p99_us:>9.2f}  {comparison.p99_ratio:>13.2f}x  "
            f"{llam_row.max_us:>11.2f}  {go_row.max_us:>9.2f}  {comparison.max_ratio:>13.2f}x"
        )
    return "\n".join(lines)


def write_csv(
    path: Path,
    llam_rows: list[SpikeRow],
    go_rows: list[SpikeRow],
    comparisons: list[ComparisonRow],
) -> None:
    llam_by_metric = {row.metric: row for row in llam_rows}
    go_by_metric = {row.metric: row for row in go_rows}
    compare_by_metric = {row.metric: row for row in comparisons}

    with open_text_for_write(path, newline="", encoding="utf-8") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "metric",
                "workers",
                "rounds",
                "spike_ms",
                "settle_ms",
                "llam_p50_us",
                "llam_p99_us",
                "llam_max_us",
                "go_p50_us",
                "go_p99_us",
                "go_max_us",
                "llam_over_go_p50",
                "llam_over_go_p99",
                "llam_over_go_max",
            ]
        )
        for metric in METRICS:
            llam_row = llam_by_metric[metric]
            go_row = go_by_metric[metric]
            comparison = compare_by_metric[metric]
            writer.writerow(
                [
                    metric,
                    llam_row.workers,
                    llam_row.rounds,
                    llam_row.spike_ms,
                    llam_row.settle_ms,
                    f"{llam_row.p50_us:.2f}",
                    f"{llam_row.p99_us:.2f}",
                    f"{llam_row.max_us:.2f}",
                    f"{go_row.p50_us:.2f}",
                    f"{go_row.p99_us:.2f}",
                    f"{go_row.max_us:.2f}",
                    f"{comparison.p50_ratio:.2f}",
                    f"{comparison.p99_ratio:.2f}",
                    f"{comparison.max_ratio:.2f}",
                ]
            )


def _resolve_from_root(root: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else root / path


def _build_binaries(root: Path, go_binary: Path) -> None:
    subprocess.run(["make", "-j4", "cpu_spike"], cwd=root, check=True)
    prepare_output_path(go_binary)
    subprocess.run(
        ["go", "build", "-o", str(go_binary), "scripts/cpu_spike_go.go"],
        cwd=root,
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Compare LLAM CPU-spike latency with Go asynchronous preemption."
    )
    parser.add_argument("--rounds", type=positive_int_at_most(100), default=7)
    parser.add_argument("--spike-ms", type=positive_int_at_most(10000), default=200)
    parser.add_argument("--settle-ms", type=positive_int_at_most(5000), default=100)
    parser.add_argument("--timeout", type=positive_int_at_most(MAX_TIMEOUT_SECONDS), default=60)
    parser.add_argument("--out-dir", default="object/cpu_spike")
    parser.add_argument("--llam-bin", default="cpu_spike")
    parser.add_argument("--go-bin", default=None)
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()

    if os.name == "nt":
        parser.error("the CPU-spike comparison currently requires POSIX socketpair support")

    try:
        root = Path(__file__).resolve().parent.parent
        out_dir = prepare_output_dir(_resolve_from_root(root, args.out_dir))
        llam_binary = _resolve_from_root(root, args.llam_bin)
        go_binary = (
            _resolve_from_root(root, args.go_bin)
            if args.go_bin is not None
            else out_dir / "go_cpu_spike"
        )
        if not args.no_build:
            _build_binaries(root, go_binary)
        for label, binary in (("LLAM", llam_binary), ("Go", go_binary)):
            if not binary.is_file():
                raise RuntimeError(f"{label} CPU-spike binary not found: {binary}")
            if os.name != "nt" and not os.access(binary, os.X_OK):
                raise RuntimeError(f"{label} CPU-spike binary is not executable: {binary}")

        env = {
            "LLAM_CPU_SPIKE_ROUNDS": str(args.rounds),
            "LLAM_CPU_SPIKE_MS": str(args.spike_ms),
            "LLAM_CPU_SPIKE_SETTLE_MS": str(args.settle_ms),
            "LLAM_PREEMPT_MODE": "auto",
            "LLAM_PREEMPT_QUANTUM_NS": "1000000",
            "LLAM_RUNTIME_PROFILE": "release-fast",
            "GODEBUG": force_go_async_preemption(os.environ.get("GODEBUG", "")),
        }
        llam_rows, go_rows = run_pair(
            root,
            [str(llam_binary)],
            [str(go_binary)],
            env,
            timeout=float(args.timeout),
        )
        comparisons = compare_rows(llam_rows, go_rows)
        csv_path = out_dir / "cpu_spike_compare.csv"
        write_csv(csv_path, llam_rows, go_rows, comparisons)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as exc:
        print(f"[cpu-spike-compare] {exc}", file=sys.stderr)
        return 1

    first = llam_rows[0]
    print(
        f"CPU spike: workers={first.workers} rounds={first.rounds} "
        f"spike_ms={first.spike_ms} settle_ms={first.settle_ms} (Go async preemption forced on)"
    )
    print(format_summary(llam_rows, go_rows, comparisons))
    print(f"\nCSV: {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
