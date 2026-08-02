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
# See LICENSES/OLD-LICENSE/Apache-2.0.txt.

"""Run and classify the standalone LCWE Phase 1 cost model."""

from __future__ import annotations

import argparse
import csv
import math
import os
import platform
import re
import shlex
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from process_utils import ProcessTimeoutError, run_capture
from safe_output import open_text_for_write


RESULT_PREFIX = "[lcwe-model] "
WORKLOADS = {
    "exec_io_pipeline",
    "exec_rpc_state",
    "exec_event_fanout",
}
MODES = {
    "scalar",
    "cohort",
    "wave_pointers",
    "wave_capsule",
    "wave_aosoa",
}
LANE_WIDTHS = {1, 2, 4, 8, 16, 32}
INTEGER_FIELDS = {
    "instances",
    "sites",
    "lanes",
    "rounds",
    "warmup",
    "ops",
    "wall_ns",
    "cpu_ns",
}
FLOAT_FIELDS = {
    "wall_ns_per_op",
    "cpu_ns_per_op",
    "p50_ns_per_op",
    "p99_ns_per_op",
}
EXPECTED_FIELDS = {
    "workload",
    "mode",
    *INTEGER_FIELDS,
    *FLOAT_FIELDS,
    "checksum",
}
DECIMAL_RE = re.compile(r"[0-9]+")
FIXED_FLOAT_RE = re.compile(r"[0-9]+\.[0-9]{2}")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")


@dataclass(frozen=True)
class ModelRow:
    workload: str
    mode: str
    instances: int
    sites: int
    lanes: int
    rounds: int
    warmup: int
    ops: int
    wall_ns: int
    cpu_ns: int
    wall_ns_per_op: float
    cpu_ns_per_op: float
    p50_ns_per_op: float
    p99_ns_per_op: float
    checksum: str


def _split_result(output: str) -> dict[str, str]:
    lines = output.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one LCWE model result row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty LCWE model result row")

    fields: dict[str, str] = {}
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed LCWE model field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(f"duplicate or empty LCWE model field: {key}")
        fields[key] = value
    if set(fields) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(fields))
        extra = sorted(set(fields) - EXPECTED_FIELDS)
        raise ValueError(f"LCWE model field mismatch: missing={missing} extra={extra}")
    return fields


def _parse_integer(name: str, value: str) -> int:
    if DECIMAL_RE.fullmatch(value) is None:
        raise ValueError(f"invalid integer field {name}")
    return int(value, 10)


def _parse_float(name: str, value: str) -> float:
    if FIXED_FLOAT_RE.fullmatch(value) is None:
        raise ValueError(f"invalid fixed-point field {name}")
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0.0:
        raise ValueError(f"non-positive or non-finite field {name}")
    return parsed


def parse_output(output: str) -> ModelRow:
    fields = _split_result(output)
    integers = {
        name: _parse_integer(name, fields[name]) for name in INTEGER_FIELDS
    }
    floats = {
        name: _parse_float(name, fields[name]) for name in FLOAT_FIELDS
    }

    if fields["workload"] not in WORKLOADS:
        raise ValueError("unknown workload")
    if fields["mode"] not in MODES:
        raise ValueError("unknown mode")
    if not (1 <= integers["instances"] <= 16_777_216):
        raise ValueError("instances out of range")
    if not (1 <= integers["sites"] <= 32):
        raise ValueError("sites out of range")
    if integers["lanes"] not in LANE_WIDTHS:
        raise ValueError("unsupported lane width")
    if not (1 <= integers["rounds"] <= 1_000_000):
        raise ValueError("rounds out of range")
    if not (0 <= integers["warmup"] <= 100_000):
        raise ValueError("warmup out of range")
    if integers["warmup"] >= integers["rounds"]:
        raise ValueError("warmup must be smaller than rounds")
    if fields["mode"] == "wave_aosoa" and integers["sites"] != 1:
        raise ValueError("wave_aosoa requires one site")

    measured_rounds = integers["rounds"] - integers["warmup"]
    expected_ops = integers["instances"] * measured_rounds
    if integers["ops"] != expected_ops:
        raise ValueError("operation count mismatch")
    if integers["wall_ns"] <= 0 or integers["cpu_ns"] <= 0:
        raise ValueError("non-positive aggregate time")
    if abs(
        floats["wall_ns_per_op"]
        - integers["wall_ns"] / integers["ops"]
    ) > 0.011:
        raise ValueError("wall ns/op mismatch")
    if abs(
        floats["cpu_ns_per_op"]
        - integers["cpu_ns"] / integers["ops"]
    ) > 0.011:
        raise ValueError("CPU ns/op mismatch")
    if floats["p99_ns_per_op"] < floats["p50_ns_per_op"]:
        raise ValueError("p99 smaller than p50")
    if CHECKSUM_RE.fullmatch(fields["checksum"]) is None:
        raise ValueError("invalid checksum")

    return ModelRow(
        workload=fields["workload"],
        mode=fields["mode"],
        instances=integers["instances"],
        sites=integers["sites"],
        lanes=integers["lanes"],
        rounds=integers["rounds"],
        warmup=integers["warmup"],
        ops=integers["ops"],
        wall_ns=integers["wall_ns"],
        cpu_ns=integers["cpu_ns"],
        wall_ns_per_op=floats["wall_ns_per_op"],
        cpu_ns_per_op=floats["cpu_ns_per_op"],
        p50_ns_per_op=floats["p50_ns_per_op"],
        p99_ns_per_op=floats["p99_ns_per_op"],
        checksum=fields["checksum"],
    )


@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: ModelRow


@dataclass(frozen=True)
class SummaryRow:
    workload: str
    mode: str
    sites: int
    lanes: int
    sample_count: int
    wall_ns_per_op: float
    cpu_ns_per_op: float
    p50_ns_per_op: float
    p99_ns_per_op: float
    spread: float
    checksum: str


def select_median(rows: list[ModelRow]) -> SummaryRow:
    if not rows:
        raise ValueError("cannot summarize an empty sample set")
    identity = (
        rows[0].workload,
        rows[0].mode,
        rows[0].instances,
        rows[0].sites,
        rows[0].lanes,
        rows[0].rounds,
        rows[0].warmup,
        rows[0].ops,
    )
    for row in rows:
        if (
            row.workload,
            row.mode,
            row.instances,
            row.sites,
            row.lanes,
            row.rounds,
            row.warmup,
            row.ops,
        ) != identity:
            raise ValueError("sample set mixes benchmark cells")
        if row.wall_ns_per_op <= 0.0 or not math.isfinite(row.wall_ns_per_op):
            raise ValueError("sample set contains invalid wall time")

    ordered = sorted(rows, key=lambda row: row.wall_ns_per_op)
    selected = ordered[len(ordered) // 2]
    checksums = {row.checksum for row in rows}
    return SummaryRow(
        workload=selected.workload,
        mode=selected.mode,
        sites=selected.sites,
        lanes=selected.lanes,
        sample_count=len(rows),
        wall_ns_per_op=selected.wall_ns_per_op,
        cpu_ns_per_op=selected.cpu_ns_per_op,
        p50_ns_per_op=selected.p50_ns_per_op,
        p99_ns_per_op=selected.p99_ns_per_op,
        spread=ordered[-1].wall_ns_per_op / ordered[0].wall_ns_per_op,
        checksum=selected.checksum if len(checksums) == 1 else "MISMATCH",
    )


def _cell_key(row: SummaryRow) -> tuple[str, str, int, int]:
    return (row.workload, row.mode, row.sites, row.lanes)


def _passes_workload_gate(
    scalar: SummaryRow,
    candidate: SummaryRow,
) -> bool:
    throughput_speedup = scalar.wall_ns_per_op / candidate.wall_ns_per_op
    cpu_ratio = candidate.cpu_ns_per_op / scalar.cpu_ns_per_op
    return throughput_speedup >= 1.50 and cpu_ratio <= 0.70


def _integrity_reasons(summary: list[SummaryRow]) -> list[str]:
    reasons: list[str] = []
    seen: set[tuple[str, str, int, int]] = set()

    if not summary:
        return ["no summary rows were produced"]
    for row in summary:
        key = _cell_key(row)
        if key in seen:
            reasons.append(f"duplicate summary cell: {key}")
        seen.add(key)
        if (
            row.sample_count <= 0
            or not math.isfinite(row.spread)
            or row.spread > 1.15
        ):
            reasons.append(
                f"unstable process spread for {key}: {row.spread:.4f}x"
            )
        if row.checksum == "MISMATCH":
            reasons.append(f"process checksum mismatch for {key}")

    main_rows = [row for row in summary if row.sites == 1]
    for workload in sorted(WORKLOADS):
        workload_rows = [
            row for row in main_rows if row.workload == workload
        ]
        for mode in (
            "scalar",
            "cohort",
            "wave_pointers",
            "wave_capsule",
            "wave_aosoa",
        ):
            if not any(row.mode == mode for row in workload_rows):
                reasons.append(f"missing {workload}/{mode} main row")
        checksums = {row.checksum for row in workload_rows}
        if len(checksums) > 1:
            reasons.append(f"cross-mode checksum mismatch for {workload}")
    return reasons


def classify(summary: list[SummaryRow]) -> tuple[str, list[str]]:
    integrity = _integrity_reasons(summary)
    if integrity:
        return "INCONCLUSIVE", integrity

    main_rows = [row for row in summary if row.sites == 1]
    realistic_passes: list[str] = []
    upper_bound_passes: list[str] = []
    cohort_speedups: list[tuple[str, float]] = []
    reasons: list[str] = []

    for workload in sorted(WORKLOADS):
        workload_rows = [
            row for row in main_rows if row.workload == workload
        ]
        scalar = next(row for row in workload_rows if row.mode == "scalar")
        cohort = min(
            (row for row in workload_rows if row.mode == "cohort"),
            key=lambda row: row.wall_ns_per_op,
        )
        cohort_speedups.append(
            (workload, scalar.wall_ns_per_op / cohort.wall_ns_per_op)
        )

        realistic = [
            row
            for row in workload_rows
            if row.mode in {"wave_pointers", "wave_capsule"}
        ]
        upper_bound = [
            row for row in workload_rows if row.mode == "wave_aosoa"
        ]
        passing_realistic = [
            row
            for row in realistic
            if _passes_workload_gate(scalar, row)
        ]
        passing_upper = [
            row
            for row in upper_bound
            if _passes_workload_gate(scalar, row)
        ]
        if passing_realistic:
            best = max(
                passing_realistic,
                key=lambda row: scalar.wall_ns_per_op
                / row.wall_ns_per_op,
            )
            realistic_passes.append(workload)
            reasons.append(
                f"{workload}: realistic {best.mode}/{best.lanes} passes "
                f"at {scalar.wall_ns_per_op / best.wall_ns_per_op:.2f}x "
                f"wall and {best.cpu_ns_per_op / scalar.cpu_ns_per_op:.2f}x CPU"
            )
        else:
            reasons.append(f"{workload}: no realistic layout passes both gates")
        if passing_upper:
            upper_bound_passes.append(workload)

    cohort_workload, best_cohort_speedup = max(
        cohort_speedups,
        key=lambda item: item[1],
    )
    cohort_passes = best_cohort_speedup >= 1.05
    if cohort_passes:
        reasons.append(
            f"cohort gate passes on {cohort_workload} at "
            f"{best_cohort_speedup:.2f}x"
        )
    else:
        reasons.append(
            f"cohort gate fails: best speedup is {best_cohort_speedup:.2f}x"
        )

    if len(realistic_passes) >= 2 and cohort_passes:
        return "PROMISING", reasons
    if len(realistic_passes) < 2 and len(upper_bound_passes) >= 2:
        reasons.append(
            "at least two workloads pass only in the site-resident "
            "AoSoA upper bound"
        )
        return "LAYOUT_BLOCKED", reasons
    return "REJECT", reasons


def write_samples_csv(path: Path, rows: list[SampleRow]) -> None:
    fieldnames = [
        "process_sample",
        "workload",
        "mode",
        "instances",
        "sites",
        "lanes",
        "rounds",
        "warmup",
        "ops",
        "wall_ns",
        "cpu_ns",
        "wall_ns_per_op",
        "cpu_ns_per_op",
        "p50_ns_per_op",
        "p99_ns_per_op",
        "checksum",
    ]
    with open_text_for_write(path, newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for sample in rows:
            row = sample.row
            writer.writerow(
                {
                    "process_sample": sample.process_sample,
                    "workload": row.workload,
                    "mode": row.mode,
                    "instances": row.instances,
                    "sites": row.sites,
                    "lanes": row.lanes,
                    "rounds": row.rounds,
                    "warmup": row.warmup,
                    "ops": row.ops,
                    "wall_ns": row.wall_ns,
                    "cpu_ns": row.cpu_ns,
                    "wall_ns_per_op": f"{row.wall_ns_per_op:.2f}",
                    "cpu_ns_per_op": f"{row.cpu_ns_per_op:.2f}",
                    "p50_ns_per_op": f"{row.p50_ns_per_op:.2f}",
                    "p99_ns_per_op": f"{row.p99_ns_per_op:.2f}",
                    "checksum": row.checksum,
                }
            )


def write_summary_csv(path: Path, rows: list[SummaryRow]) -> None:
    fieldnames = [
        "workload",
        "mode",
        "sites",
        "lanes",
        "sample_count",
        "wall_ns_per_op",
        "cpu_ns_per_op",
        "p50_ns_per_op",
        "p99_ns_per_op",
        "spread",
        "checksum",
    ]
    with open_text_for_write(path, newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "workload": row.workload,
                    "mode": row.mode,
                    "sites": row.sites,
                    "lanes": row.lanes,
                    "sample_count": row.sample_count,
                    "wall_ns_per_op": f"{row.wall_ns_per_op:.2f}",
                    "cpu_ns_per_op": f"{row.cpu_ns_per_op:.2f}",
                    "p50_ns_per_op": f"{row.p50_ns_per_op:.2f}",
                    "p99_ns_per_op": f"{row.p99_ns_per_op:.2f}",
                    "spread": f"{row.spread:.6f}",
                    "checksum": row.checksum,
                }
            )


def _layout_class(mode: str) -> str:
    if mode in {"wave_pointers", "wave_capsule"}:
        return "realistic"
    if mode == "wave_aosoa":
        return "upper-bound only"
    return "baseline"


def _performance_table(rows: list[SummaryRow]) -> list[str]:
    if not rows:
        return ["_No rows._", ""]
    scalar = next((row for row in rows if row.mode == "scalar"), None)
    lines = [
        "| Mode | Lanes | Layout | Wall ns/op | CPU ns/op | "
        "Wall speedup | CPU reduction | p99 ns/op | Spread |",
        "|---|---:|---|---:|---:|---:|---:|---:|---:|",
    ]
    mode_order = {
        "scalar": 0,
        "cohort": 1,
        "wave_pointers": 2,
        "wave_capsule": 3,
        "wave_aosoa": 4,
    }
    for row in sorted(rows, key=lambda item: (mode_order[item.mode], item.lanes)):
        if scalar is None:
            speedup = "n/a"
            cpu_reduction = "n/a"
        else:
            speedup = f"{scalar.wall_ns_per_op / row.wall_ns_per_op:.2f}x"
            cpu_reduction = (
                f"{(1.0 - row.cpu_ns_per_op / scalar.cpu_ns_per_op) * 100.0:.1f}%"
            )
        lines.append(
            f"| {row.mode} | {row.lanes} | {_layout_class(row.mode)} | "
            f"{row.wall_ns_per_op:.2f} | {row.cpu_ns_per_op:.2f} | "
            f"{speedup} | {cpu_reduction} | {row.p99_ns_per_op:.2f} | "
            f"{row.spread:.3f}x |"
        )
    lines.append("")
    return lines


def write_markdown(
    path: Path,
    rows: list[SummaryRow],
    verdict: str,
    reasons: list[str],
    metadata: dict[str, str],
) -> None:
    lines = [
        "# LCWE Phase 1 Cost Model",
        "",
        "## Verdict",
        "",
        f"`{verdict}`",
        "",
    ]
    lines.extend(f"- {reason}" for reason in reasons)
    lines.extend(
        [
            "",
            "Phase 1 is not production validation. It is a standalone "
            "cost-model decision only.",
            "",
            "## Reproduction",
            "",
        ]
    )
    for key in sorted(metadata):
        if key == "command":
            continue
        lines.append(f"- {key}: `{metadata[key]}`")
    lines.extend(
        [
            "",
            "```sh",
            metadata.get("command", "unavailable"),
            "```",
            "",
            "## Homogeneous One-Site Matrix",
            "",
            "Pointer and capsule rows are **realistic** layouts. AoSoA is "
            "**upper-bound only** and cannot produce `PROMISING`.",
            "",
        ]
    )
    for workload in sorted(WORKLOADS):
        lines.extend(
            [
                f"### {workload}",
                "",
                *_performance_table(
                    [
                        row
                        for row in rows
                        if row.sites == 1 and row.workload == workload
                    ]
                ),
            ]
        )
    lines.extend(["## Mixed-Site Diagnostic", ""])
    mixed = [row for row in rows if row.sites != 1]
    if mixed:
        lines.extend(
            [
                "| Workload | Sites | Mode | Lanes | Layout | "
                "Wall ns/op | CPU ns/op | p99 ns/op | Spread |",
                "|---|---:|---|---:|---|---:|---:|---:|---:|",
            ]
        )
        for row in sorted(mixed, key=lambda item: _cell_key(item)):
            lines.append(
                f"| {row.workload} | {row.sites} | {row.mode} | "
                f"{row.lanes} | {_layout_class(row.mode)} | "
                f"{row.wall_ns_per_op:.2f} | {row.cpu_ns_per_op:.2f} | "
                f"{row.p99_ns_per_op:.2f} | {row.spread:.3f}x |"
            )
        lines.append("")
    else:
        lines.extend(["_No mixed-site rows._", ""])
    lines.extend(
        [
            "## Remaining Production Gates",
            "",
            "- real completion-driven I/O and timer wakeups;",
            "- balanced-mode p99 latency degradation at or below 10%;",
            "- low-load and mixed-site throughput regression at or below 5%;",
            "- Linux, Darwin/BSD, and Windows correctness;",
            "- existing stackful benchmark geometric-mean regression at or "
            "below 3%.",
            "",
        ]
    )
    with open_text_for_write(path) as handle:
        handle.write("\n".join(lines))


def _bounded_int(name: str, minimum: int, maximum: int):
    def parse(value: str) -> int:
        if DECIMAL_RE.fullmatch(value) is None:
            raise argparse.ArgumentTypeError(f"{name} must be a decimal integer")
        parsed = int(value, 10)
        if not minimum <= parsed <= maximum:
            raise argparse.ArgumentTypeError(
                f"{name} must be in {minimum}..{maximum}"
            )
        return parsed

    return parse


def _parse_widths(value: str) -> list[int]:
    if not value:
        raise argparse.ArgumentTypeError("width list must not be empty")
    widths: list[int] = []
    for token in value.split(","):
        if DECIMAL_RE.fullmatch(token) is None:
            raise argparse.ArgumentTypeError("widths must be comma-separated integers")
        width = int(token, 10)
        if width not in LANE_WIDTHS:
            raise argparse.ArgumentTypeError(f"unsupported lane width: {width}")
        if width in widths:
            raise argparse.ArgumentTypeError(f"duplicate lane width: {width}")
        widths.append(width)
    return widths


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the deterministic LCWE Phase 1 cost-model matrix."
    )
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument(
        "--samples",
        type=_bounded_int("samples", 1, 31),
        default=7,
    )
    parser.add_argument(
        "--instances",
        type=_bounded_int("instances", 1, 16_777_216),
        default=65_536,
    )
    parser.add_argument(
        "--rounds",
        type=_bounded_int("rounds", 1, 1_000_000),
        default=31,
    )
    parser.add_argument(
        "--warmup",
        type=_bounded_int("warmup", 0, 100_000),
        default=5,
    )
    parser.add_argument(
        "--seed",
        type=_bounded_int("seed", 0, 18_446_744_073_709_551_615),
        default=7_810_762_890_074_515_045,
    )
    parser.add_argument(
        "--widths",
        type=_parse_widths,
        default=[1, 2, 4, 8, 16, 32],
    )
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("object/lcwe-phase1"),
    )
    parser.add_argument("--quick", action="store_true")
    return parser


def _matrix(widths: list[int]) -> list[tuple[str, str, int, int]]:
    cells: list[tuple[str, str, int, int]] = []
    for workload in sorted(WORKLOADS):
        cells.append((workload, "scalar", 1, 1))
        cells.append((workload, "cohort", 1, 1))
        for mode in ("wave_pointers", "wave_capsule", "wave_aosoa"):
            for width in widths:
                cells.append((workload, mode, 1, width))

        cells.append((workload, "scalar", 8, 1))
        cells.append((workload, "cohort", 8, 1))
        for mode in ("wave_pointers", "wave_capsule"):
            for width in (1, 8, 32):
                cells.append((workload, mode, 8, width))
    return cells


def _benchmark_command(
    binary: Path,
    cell: tuple[str, str, int, int],
    instances: int,
    rounds: int,
    warmup: int,
    seed: int,
) -> list[str]:
    workload, mode, sites, lanes = cell
    return [
        str(binary),
        "--workload",
        workload,
        "--mode",
        mode,
        "--instances",
        str(instances),
        "--sites",
        str(sites),
        "--lanes",
        str(lanes),
        "--rounds",
        str(rounds),
        "--warmup",
        str(warmup),
        "--seed",
        str(seed),
    ]


def _run_matrix(
    binary: Path,
    cells: list[tuple[str, str, int, int]],
    *,
    samples: int,
    instances: int,
    rounds: int,
    warmup: int,
    seed: int,
) -> list[SampleRow]:
    collected: list[SampleRow] = []
    total = len(cells) * samples
    completed = 0

    for sample_index in range(1, samples + 1):
        offset = (sample_index - 1) % len(cells)
        ordered = cells[offset:] + cells[:offset]
        if sample_index % 2 == 0:
            ordered = list(reversed(ordered))
        for cell in ordered:
            command = _benchmark_command(
                binary,
                cell,
                instances,
                rounds,
                warmup,
                seed,
            )
            try:
                result = run_capture(command, timeout=120.0)
            except ProcessTimeoutError as exc:
                raise RuntimeError(
                    f"benchmark timed out: {shlex.join(command)}\n"
                    f"{exc.stdout}{exc.stderr}"
                ) from None
            if result.returncode != 0:
                raise RuntimeError(
                    f"benchmark failed ({result.returncode}): "
                    f"{shlex.join(command)}\n{result.stdout}{result.stderr}"
                )
            row = parse_output(result.stdout)
            expected = (cell[0], cell[1], instances, cell[2], cell[3], rounds, warmup)
            actual = (
                row.workload,
                row.mode,
                row.instances,
                row.sites,
                row.lanes,
                row.rounds,
                row.warmup,
            )
            if actual != expected:
                raise RuntimeError(
                    f"benchmark echoed the wrong cell: expected={expected} actual={actual}"
                )
            collected.append(SampleRow(sample_index, row))
            completed += 1
            print(
                f"[lcwe-phase1] sample {completed}/{total}: "
                f"{cell[0]} {cell[1]} sites={cell[2]} lanes={cell[3]}",
                file=sys.stderr,
            )
    return collected


def _summarize(samples: list[SampleRow]) -> list[SummaryRow]:
    grouped: dict[tuple[str, str, int, int], list[ModelRow]] = {}
    for sample in samples:
        key = (
            sample.row.workload,
            sample.row.mode,
            sample.row.sites,
            sample.row.lanes,
        )
        grouped.setdefault(key, []).append(sample.row)
    return [
        select_median(grouped[key])
        for key in sorted(grouped, key=lambda item: (item[2], item[0], item[1], item[3]))
    ]


def _tool_identity(command: list[str]) -> str:
    result = run_capture(command, timeout=10.0)
    if result.returncode != 0:
        raise RuntimeError(
            f"identity command failed: {shlex.join(command)}\n"
            f"{result.stdout}{result.stderr}"
        )
    lines = [
        line.strip()
        for line in (result.stdout + result.stderr).splitlines()
        if line.strip()
    ]
    if not lines:
        raise RuntimeError(f"identity command produced no output: {command}")
    return lines[0]


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.quick:
        args.samples = 3
        args.instances = 8_192
        args.rounds = 9
        args.warmup = 2
    if args.warmup >= args.rounds:
        parser.error("--warmup must be smaller than --rounds")

    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"--binary is not a regular file: {binary}")
    cells = _matrix(args.widths)
    try:
        samples = _run_matrix(
            binary,
            cells,
            samples=args.samples,
            instances=args.instances,
            rounds=args.rounds,
            warmup=args.warmup,
            seed=args.seed,
        )
        summary = _summarize(samples)
        verdict, reasons = classify(summary)
        compiler = _tool_identity([args.cc, "--version"])
        try:
            commit = _tool_identity(["git", "rev-parse", "HEAD"])
        except (OSError, RuntimeError):
            commit = "unavailable"
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"[lcwe-phase1] error: {exc}", file=sys.stderr)
        return 1

    output_dir: Path = args.out_dir
    sample_path = output_dir / "lcwe_phase1_samples.csv"
    summary_path = output_dir / "lcwe_phase1_summary.csv"
    report_path = output_dir / "lcwe_phase1_report.md"
    command_args = (
        [sys.executable, *sys.argv]
        if argv is None
        else [sys.executable, str(Path(__file__)), *argv]
    )
    metadata = {
        "binary": str(binary),
        "command": shlex.join(command_args),
        "commit": commit,
        "compiler": compiler,
        "host": platform.platform(),
        "instances": str(args.instances),
        "machine": platform.machine(),
        "process samples": str(args.samples),
        "Python": platform.python_version(),
        "rounds": str(args.rounds),
        "seed": str(args.seed),
        "warmup": str(args.warmup),
    }
    try:
        write_samples_csv(sample_path, samples)
        write_summary_csv(summary_path, summary)
        write_markdown(report_path, summary, verdict, reasons, metadata)
    except (OSError, RuntimeError) as exc:
        print(f"[lcwe-phase1] output error: {exc}", file=sys.stderr)
        return 1
    print(f"[lcwe-phase1] verdict={verdict} report={report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
