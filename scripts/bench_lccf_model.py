#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run and classify the standalone LCCF Phase 0 paired cost model."""

from __future__ import annotations

import argparse
import csv
import ctypes
import hashlib
import json
import math
import os
import platform
import re
import shlex
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence

from process_utils import ProcessTimeoutError, run_capture
from safe_output import open_text_for_write, write_text_safely


RESULT_PREFIX = "LCCF_PAIR "
FUNCTIONAL_WORKLOADS = (
    "completion_io_pipeline",
    "completion_rpc_state",
    "completion_timer_cancel",
)
MIXED_WORKLOAD = "completion_mixed_fairness"
WORKLOADS = {*FUNCTIONAL_WORKLOADS, MIXED_WORKLOAD}
CANDIDATE_BASELINES = {
    "causal_cell_queue": "waker_queue",
    "fused_causal_cell": "waker_queue",
    "budgeted_fused_chain": "waker_queue",
    "remote_causal_cell": "remote_waker_queue",
}
FRAME_BYTES = (64, 128, 256)
CELL_BYTES = (64, 96, 128)
SITE_COUNTS = (1, 8)
FULL_SAMPLES = 9
FULL_MIN_MODE_NS = 250_000_000
QUICK_MIN_MODE_NS = 20_000_000
DECIMAL_RE = re.compile(r"[0-9]+")
RATIO_RE = re.compile(r"[0-9]+\.[0-9]{9,12}")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")
AFFINITY_RE = re.compile(r"[A-Za-z0-9_.:-]+")
INTEGER_FIELDS = {
    "instances",
    "frame_bytes",
    "cell_bytes",
    "sites",
    "budget",
    "chain",
    "producers",
    "min_mode_ns",
    "rounds_per_block",
    "ops_per_mode",
    "baseline_wall_ns",
    "candidate_wall_ns",
    "baseline_cpu_ns",
    "candidate_cpu_ns",
    "baseline_fair_p99_ns",
    "candidate_fair_p99_ns",
    "hot_allocations",
    "forced_escapes",
    "direct_calls",
}
RATIO_FIELDS = {"wall_speedup", "cpu_ratio"}
EXPECTED_FIELDS = {
    "version",
    "workload",
    "candidate",
    "baseline",
    *INTEGER_FIELDS,
    *RATIO_FIELDS,
    "checksum",
    "order",
    "affinity",
}


@dataclass(frozen=True)
class PairRow:
    workload: str
    candidate: str
    baseline: str
    instances: int
    frame_bytes: int
    cell_bytes: int
    sites: int
    budget: int
    chain: int
    producers: int
    min_mode_ns: int
    rounds_per_block: int
    ops_per_mode: int
    baseline_wall_ns: int
    candidate_wall_ns: int
    baseline_cpu_ns: int
    candidate_cpu_ns: int
    wall_speedup: float
    cpu_ratio: float
    baseline_fair_p99_ns: int
    candidate_fair_p99_ns: int
    checksum: str
    hot_allocations: int
    forced_escapes: int
    direct_calls: int
    order: str
    affinity: str


@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: PairRow


@dataclass(frozen=True)
class SummaryRow:
    workload: str
    candidate: str
    baseline: str
    frame_bytes: int
    cell_bytes: int
    sites: int
    budget: int
    chain: int
    producers: int
    sample_count: int
    wall_speedup: float
    cpu_ratio: float
    wall_ratio_spread: float
    cpu_ratio_spread: float
    baseline_wall_ns_per_op: float
    candidate_wall_ns_per_op: float
    baseline_cpu_ns_per_op: float
    candidate_cpu_ns_per_op: float
    fairness_p99_ratio: float
    checksum: str
    min_mode_ns: int = FULL_MIN_MODE_NS


@dataclass(frozen=True)
class MatrixCell:
    workload: str
    candidate: str
    baseline: str
    frame_bytes: int
    cell_bytes: int
    sites: int
    budget: int
    chain: int
    producers: int


class MatrixRunError(RuntimeError):
    def __init__(self, message: str, samples: list[SampleRow]) -> None:
        super().__init__(message)
        self.samples = samples


def _split_output(text: str) -> dict[str, str]:
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one LCCF_PAIR result row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty LCCF_PAIR result row")

    fields: dict[str, str] = {}
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed LCCF_PAIR field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(f"duplicate or empty LCCF_PAIR field: {key}")
        fields[key] = value
    if set(fields) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(fields))
        extra = sorted(set(fields) - EXPECTED_FIELDS)
        raise ValueError(
            f"LCCF_PAIR field mismatch: missing={missing} extra={extra}"
        )
    return fields


def _parse_integer(name: str, value: str) -> int:
    if DECIMAL_RE.fullmatch(value) is None:
        raise ValueError(f"invalid decimal integer field {name}")
    return int(value, 10)


def _parse_ratio(name: str, value: str) -> float:
    if RATIO_RE.fullmatch(value) is None:
        raise ValueError(f"invalid fixed-point ratio field {name}")
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0.0:
        raise ValueError(f"non-positive or non-finite ratio field {name}")
    return parsed


def _relative_ratio_matches(printed: float, recomputed: float) -> bool:
    if not math.isfinite(recomputed) or recomputed <= 0.0:
        return False
    return abs(printed - recomputed) / recomputed <= 1e-9


def parse_output(text: str) -> PairRow:
    fields = _split_output(text)
    if fields["version"] != "1":
        raise ValueError("unsupported LCCF_PAIR version")
    integers = {
        name: _parse_integer(name, fields[name])
        for name in INTEGER_FIELDS
    }
    ratios = {
        name: _parse_ratio(name, fields[name])
        for name in RATIO_FIELDS
    }

    workload = fields["workload"]
    candidate = fields["candidate"]
    baseline = fields["baseline"]
    if workload not in WORKLOADS:
        raise ValueError("unknown workload")
    if candidate not in CANDIDATE_BASELINES:
        raise ValueError("unknown candidate")
    if baseline != CANDIDATE_BASELINES[candidate]:
        raise ValueError("candidate/baseline mismatch")
    if not (1 <= integers["instances"] <= 0xFFFF_FFFF):
        raise ValueError("instances out of range")
    if integers["frame_bytes"] not in FRAME_BYTES:
        raise ValueError("unsupported frame footprint")
    if integers["cell_bytes"] not in CELL_BYTES:
        raise ValueError("unsupported causal-cell footprint")
    if integers["sites"] not in SITE_COUNTS:
        raise ValueError("unsupported site distribution")
    if not (1 <= integers["budget"] <= 1024):
        raise ValueError("budget out of range")
    if not (1 <= integers["chain"] <= 1024):
        raise ValueError("chain out of range")
    if integers["producers"] != 2:
        raise ValueError("remote producer count must be two")
    if not (1_000_000 <= integers["min_mode_ns"] <= 60_000_000_000):
        raise ValueError("minimum mode duration out of range")
    if integers["rounds_per_block"] <= 0:
        raise ValueError("non-positive rounds per block")
    expected_ops = (
        integers["instances"]
        * integers["rounds_per_block"]
        * 2
    )
    if integers["ops_per_mode"] != expected_ops:
        raise ValueError("operation count mismatch")
    for name in (
        "baseline_wall_ns",
        "candidate_wall_ns",
        "baseline_cpu_ns",
        "candidate_cpu_ns",
    ):
        if integers[name] <= 0:
            raise ValueError(f"non-positive aggregate duration {name}")
    if (
        integers["baseline_wall_ns"] < integers["min_mode_ns"]
        or integers["candidate_wall_ns"] < integers["min_mode_ns"]
    ):
        raise ValueError("measured wall duration is below min_mode_ns")

    recomputed_wall = (
        integers["baseline_wall_ns"]
        / integers["candidate_wall_ns"]
    )
    recomputed_cpu = (
        integers["candidate_cpu_ns"]
        / integers["baseline_cpu_ns"]
    )
    if not _relative_ratio_matches(
        ratios["wall_speedup"], recomputed_wall
    ):
        raise ValueError("printed wall speedup disagrees with durations")
    if not _relative_ratio_matches(
        ratios["cpu_ratio"], recomputed_cpu
    ):
        raise ValueError("printed CPU ratio disagrees with durations")

    baseline_fair = integers["baseline_fair_p99_ns"]
    candidate_fair = integers["candidate_fair_p99_ns"]
    if workload == MIXED_WORKLOAD:
        if baseline_fair <= 0 or candidate_fair <= 0:
            raise ValueError("mixed fairness p99 values must be positive")
    elif baseline_fair != 0 or candidate_fair != 0:
        raise ValueError("non-fairness row has fairness p99 values")
    if CHECKSUM_RE.fullmatch(fields["checksum"]) is None:
        raise ValueError("invalid checksum")
    if integers["hot_allocations"] != 0:
        raise ValueError("measured hot allocation")
    if fields["order"] not in {"ABBA", "BAAB"}:
        raise ValueError("invalid pair order")
    if AFFINITY_RE.fullmatch(fields["affinity"]) is None:
        raise ValueError("invalid affinity description")

    callbacks = integers["ops_per_mode"] * integers["chain"]
    if candidate == "fused_causal_cell":
        if (
            integers["direct_calls"] != callbacks
            or integers["forced_escapes"] != 0
        ):
            raise ValueError("invalid unbounded fused accounting")
    elif candidate == "budgeted_fused_chain":
        if (
            integers["direct_calls"]
            + integers["forced_escapes"]
            != callbacks
        ):
            raise ValueError("invalid budgeted fused accounting")
    elif (
        integers["direct_calls"] != 0
        or integers["forced_escapes"] != 0
    ):
        raise ValueError("queued candidate reported direct execution")

    return PairRow(
        workload=workload,
        candidate=candidate,
        baseline=baseline,
        instances=integers["instances"],
        frame_bytes=integers["frame_bytes"],
        cell_bytes=integers["cell_bytes"],
        sites=integers["sites"],
        budget=integers["budget"],
        chain=integers["chain"],
        producers=integers["producers"],
        min_mode_ns=integers["min_mode_ns"],
        rounds_per_block=integers["rounds_per_block"],
        ops_per_mode=integers["ops_per_mode"],
        baseline_wall_ns=integers["baseline_wall_ns"],
        candidate_wall_ns=integers["candidate_wall_ns"],
        baseline_cpu_ns=integers["baseline_cpu_ns"],
        candidate_cpu_ns=integers["candidate_cpu_ns"],
        wall_speedup=ratios["wall_speedup"],
        cpu_ratio=ratios["cpu_ratio"],
        baseline_fair_p99_ns=baseline_fair,
        candidate_fair_p99_ns=candidate_fair,
        checksum=fields["checksum"],
        hot_allocations=integers["hot_allocations"],
        forced_escapes=integers["forced_escapes"],
        direct_calls=integers["direct_calls"],
        order=fields["order"],
        affinity=fields["affinity"],
    )


def _cell_key_from_pair(row: PairRow) -> tuple[object, ...]:
    return (
        row.workload,
        row.candidate,
        row.baseline,
        row.frame_bytes,
        row.cell_bytes,
        row.sites,
        row.budget,
        row.chain,
        row.producers,
    )


def _cell_key_from_summary(row: SummaryRow) -> tuple[object, ...]:
    return (
        row.workload,
        row.candidate,
        row.baseline,
        row.frame_bytes,
        row.cell_bytes,
        row.sites,
        row.budget,
        row.chain,
        row.producers,
    )


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (
        cell.workload,
        cell.candidate,
        cell.baseline,
        cell.frame_bytes,
        cell.cell_bytes,
        cell.sites,
        cell.budget,
        cell.chain,
        cell.producers,
    )


def _summarize_group(samples: list[SampleRow]) -> SummaryRow:
    if not samples:
        raise ValueError("cannot summarize an empty sample group")
    identity = _cell_key_from_pair(samples[0].row)
    instances = samples[0].row.instances
    min_mode_ns = samples[0].row.min_mode_ns
    seen_processes: set[int] = set()
    checksums_by_rounds: dict[int, set[str]] = {}

    for sample in samples:
        row = sample.row
        if _cell_key_from_pair(row) != identity:
            raise ValueError("sample group mixes matrix cells")
        if row.instances != instances or row.min_mode_ns != min_mode_ns:
            raise ValueError("sample group mixes run controls")
        if sample.process_sample <= 0 or sample.process_sample in seen_processes:
            raise ValueError("duplicate or invalid process sample index")
        seen_processes.add(sample.process_sample)
        if not _relative_ratio_matches(
            row.wall_speedup,
            row.baseline_wall_ns / row.candidate_wall_ns,
        ) or not _relative_ratio_matches(
            row.cpu_ratio,
            row.candidate_cpu_ns / row.baseline_cpu_ns,
        ):
            raise ValueError("sample ratio disagrees with raw durations")
        checksums_by_rounds.setdefault(
            row.rounds_per_block, set()
        ).add(row.checksum)

    ordered = sorted(samples, key=lambda item: item.row.wall_speedup)
    selected = ordered[len(ordered) // 2].row
    wall_ratios = [sample.row.wall_speedup for sample in samples]
    cpu_ratios = [sample.row.cpu_ratio for sample in samples]
    checksum = (
        "MISMATCH"
        if any(len(values) != 1 for values in checksums_by_rounds.values())
        else selected.checksum
    )
    fairness_ratio = (
        selected.candidate_fair_p99_ns
        / selected.baseline_fair_p99_ns
        if selected.workload == MIXED_WORKLOAD
        else 1.0
    )
    return SummaryRow(
        workload=selected.workload,
        candidate=selected.candidate,
        baseline=selected.baseline,
        frame_bytes=selected.frame_bytes,
        cell_bytes=selected.cell_bytes,
        sites=selected.sites,
        budget=selected.budget,
        chain=selected.chain,
        producers=selected.producers,
        sample_count=len(samples),
        wall_speedup=selected.wall_speedup,
        cpu_ratio=selected.cpu_ratio,
        wall_ratio_spread=max(wall_ratios) / min(wall_ratios),
        cpu_ratio_spread=max(cpu_ratios) / min(cpu_ratios),
        baseline_wall_ns_per_op=(
            selected.baseline_wall_ns / selected.ops_per_mode
        ),
        candidate_wall_ns_per_op=(
            selected.candidate_wall_ns / selected.ops_per_mode
        ),
        baseline_cpu_ns_per_op=(
            selected.baseline_cpu_ns / selected.ops_per_mode
        ),
        candidate_cpu_ns_per_op=(
            selected.candidate_cpu_ns / selected.ops_per_mode
        ),
        fairness_p99_ratio=fairness_ratio,
        checksum=checksum,
        min_mode_ns=min_mode_ns,
    )


def summarize(rows: list[SampleRow]) -> list[SummaryRow]:
    grouped: dict[tuple[object, ...], list[SampleRow]] = {}
    for sample in rows:
        grouped.setdefault(
            _cell_key_from_pair(sample.row), []
        ).append(sample)
    return [
        _summarize_group(grouped[key])
        for key in sorted(grouped, key=lambda item: tuple(str(x) for x in item))
    ]


def _make_cell(
    workload: str,
    candidate: str,
    frame_bytes: int,
    cell_bytes: int,
    sites: int,
    *,
    chain: int,
) -> MatrixCell:
    return MatrixCell(
        workload=workload,
        candidate=candidate,
        baseline=CANDIDATE_BASELINES[candidate],
        frame_bytes=frame_bytes,
        cell_bytes=cell_bytes,
        sites=sites,
        budget=8,
        chain=chain,
        producers=2,
    )


def full_matrix() -> list[MatrixCell]:
    cells: list[MatrixCell] = []
    for workload in FUNCTIONAL_WORKLOADS:
        for candidate in (
            "causal_cell_queue",
            "fused_causal_cell",
            "budgeted_fused_chain",
        ):
            chain = 18 if candidate == "budgeted_fused_chain" else 1
            for frame_bytes in FRAME_BYTES:
                for cell_bytes in CELL_BYTES:
                    for sites in SITE_COUNTS:
                        cells.append(
                            _make_cell(
                                workload,
                                candidate,
                                frame_bytes,
                                cell_bytes,
                                sites,
                                chain=chain,
                            )
                        )
        for cell_bytes in CELL_BYTES:
            cells.append(
                _make_cell(
                    workload,
                    "remote_causal_cell",
                    128,
                    cell_bytes,
                    8,
                    chain=1,
                )
            )
    for cell_bytes in CELL_BYTES:
        cells.append(
            _make_cell(
                MIXED_WORKLOAD,
                "budgeted_fused_chain",
                128,
                cell_bytes,
                8,
                chain=18,
            )
        )
    return cells


def quick_matrix() -> list[MatrixCell]:
    cells = [
        _make_cell(
            "completion_io_pipeline",
            candidate,
            128,
            96,
            8,
            chain=18 if candidate == "budgeted_fused_chain" else 1,
        )
        for candidate in CANDIDATE_BASELINES
    ]
    cells.append(
        _make_cell(
            MIXED_WORKLOAD,
            "budgeted_fused_chain",
            128,
            96,
            8,
            chain=18,
        )
    )
    return cells


def _integrity_reasons(
    rows: list[SummaryRow],
    expected_samples: int,
) -> tuple[list[str], bool]:
    quick = expected_samples == 1
    expected_cells = quick_matrix() if quick else full_matrix()
    expected_keys = {_cell_key(cell) for cell in expected_cells}
    actual: dict[tuple[object, ...], SummaryRow] = {}
    reasons: list[str] = []
    required_min_ns = QUICK_MIN_MODE_NS if quick else FULL_MIN_MODE_NS

    if not quick and expected_samples != FULL_SAMPLES:
        reasons.append(
            f"full research verdict requires exactly {FULL_SAMPLES} "
            f"samples, requested {expected_samples}"
        )
    for row in rows:
        key = _cell_key_from_summary(row)
        if key in actual:
            reasons.append(f"duplicate summary cell: {key}")
        actual[key] = row
        if row.sample_count != expected_samples:
            reasons.append(
                f"sample count mismatch for {key}: "
                f"{row.sample_count} != {expected_samples}"
            )
        if row.min_mode_ns < required_min_ns:
            reasons.append(
                f"measured duration control below {required_min_ns}ns "
                f"for {key}: {row.min_mode_ns}ns"
            )
        numeric = (
            row.wall_speedup,
            row.cpu_ratio,
            row.wall_ratio_spread,
            row.cpu_ratio_spread,
            row.baseline_wall_ns_per_op,
            row.candidate_wall_ns_per_op,
            row.baseline_cpu_ns_per_op,
            row.candidate_cpu_ns_per_op,
            row.fairness_p99_ratio,
        )
        if any(not math.isfinite(value) or value <= 0.0 for value in numeric):
            reasons.append(f"non-positive or non-finite summary value for {key}")
        if not (1.0 <= row.wall_ratio_spread <= 1.10):
            reasons.append(
                f"wall paired-ratio spread exceeds 1.10x for {key}: "
                f"{row.wall_ratio_spread:.6f}x"
            )
        if not (1.0 <= row.cpu_ratio_spread <= 1.10):
            reasons.append(
                f"CPU paired-ratio spread exceeds 1.10x for {key}: "
                f"{row.cpu_ratio_spread:.6f}x"
            )
        if row.checksum == "MISMATCH" or (
            CHECKSUM_RE.fullmatch(row.checksum) is None
        ):
            reasons.append(f"checksum mismatch for {key}")
        if row.baseline != CANDIDATE_BASELINES.get(row.candidate):
            reasons.append(f"candidate/baseline mismatch for {key}")
        if row.workload == MIXED_WORKLOAD:
            if row.fairness_p99_ratio <= 0.0:
                reasons.append(f"invalid fairness ratio for {key}")
        elif abs(row.fairness_p99_ratio - 1.0) > 1e-12:
            reasons.append(f"non-fair row carries fairness ratio for {key}")

    missing = expected_keys - set(actual)
    extra = set(actual) - expected_keys
    if missing:
        reasons.append(f"missing {len(missing)} required matrix cells")
    if extra:
        reasons.append(f"found {len(extra)} unexpected matrix cells")
    return reasons, quick


def _layout_gate(
    rows: list[SummaryRow],
    cell_bytes: int,
) -> tuple[list[str], bool, int, int]:
    layout = [row for row in rows if row.cell_bytes == cell_bytes]
    queue_rows = [
        row
        for row in layout
        if row.candidate == "causal_cell_queue"
        and row.workload in FUNCTIONAL_WORKLOADS
    ]
    remote_rows = [
        row
        for row in layout
        if row.candidate == "remote_causal_cell"
        and row.workload in FUNCTIONAL_WORKLOADS
    ]
    fairness_rows = [
        row
        for row in layout
        if row.candidate == "budgeted_fused_chain"
        and row.workload == MIXED_WORKLOAD
    ]
    reasons: list[str] = []
    controls_pass = (
        bool(queue_rows)
        and min(row.wall_speedup for row in queue_rows) >= 0.95
        and bool(remote_rows)
        and min(row.wall_speedup for row in remote_rows) >= 0.95
        and len(fairness_rows) == 1
        and fairness_rows[0].fairness_p99_ratio <= 1.10
    )
    reasons.append(
        f"cell={cell_bytes}: queue floor "
        f"{min((row.wall_speedup for row in queue_rows), default=0.0):.3f}x, "
        f"remote floor "
        f"{min((row.wall_speedup for row in remote_rows), default=0.0):.3f}x, "
        f"fairness "
        f"{max((row.fairness_p99_ratio for row in fairness_rows), default=math.inf):.3f}x"
    )

    passing_workloads: list[str] = []
    high_speedup_count = 0
    for workload in FUNCTIONAL_WORKLOADS:
        fused = [
            row
            for row in layout
            if row.candidate == "fused_causal_cell"
            and row.workload == workload
        ]
        worst_wall = min(
            (row.wall_speedup for row in fused), default=0.0
        )
        worst_cpu = max(
            (row.cpu_ratio for row in fused), default=math.inf
        )
        if len(fused) == len(FRAME_BYTES) * len(SITE_COUNTS):
            if worst_wall >= 1.25 and worst_cpu <= 0.80:
                passing_workloads.append(workload)
                if worst_wall >= 1.50:
                    high_speedup_count += 1
        reasons.append(
            f"cell={cell_bytes} {workload}: fused worst wall "
            f"{worst_wall:.3f}x, worst CPU {worst_cpu:.3f}x"
        )
    return (
        reasons,
        controls_pass,
        len(passing_workloads),
        high_speedup_count,
    )


def classify(
    rows: list[SummaryRow],
    expected_samples: int,
) -> tuple[str, list[str], int | None]:
    integrity, quick = _integrity_reasons(rows, expected_samples)
    if integrity:
        return "INCONCLUSIVE", integrity, None
    if quick:
        return (
            "SMOKE_ONLY",
            [
                "quick mode validates parsing, paired execution, checksums, "
                "fairness, and zero hot allocations only"
            ],
            96,
        )

    promising: list[tuple[int, list[str]]] = []
    narrow: list[tuple[int, list[str]]] = []
    all_reasons: list[str] = []
    for cell_bytes in CELL_BYTES:
        (
            layout_reasons,
            controls_pass,
            passed_workloads,
            high_speedup_count,
        ) = _layout_gate(rows, cell_bytes)
        all_reasons.extend(layout_reasons)
        if (
            controls_pass
            and passed_workloads == len(FUNCTIONAL_WORKLOADS)
            and high_speedup_count >= 2
        ):
            promising.append((cell_bytes, layout_reasons))
        elif controls_pass and passed_workloads >= 1:
            narrow.append((cell_bytes, layout_reasons))

    if promising:
        selected, reasons = min(promising, key=lambda item: item[0])
        return (
            "PROMISING",
            [
                f"smallest fully passing causal-cell footprint is "
                f"{selected} bytes",
                *reasons,
            ],
            selected,
        )
    if narrow:
        selected, reasons = min(narrow, key=lambda item: item[0])
        passing_names = []
        for workload in FUNCTIONAL_WORKLOADS:
            fused = [
                row
                for row in rows
                if row.cell_bytes == selected
                and row.candidate == "fused_causal_cell"
                and row.workload == workload
            ]
            if (
                fused
                and min(row.wall_speedup for row in fused) >= 1.25
                and max(row.cpu_ratio for row in fused) <= 0.80
            ):
                passing_names.append(workload)
        return (
            "NARROW",
            [
                f"opt-in benefit is limited to: {', '.join(passing_names)}",
                *reasons,
            ],
            selected,
        )
    return (
        "REJECT",
        [
            "no causal-cell footprint passes even one complete functional "
            "workload together with queue, remote, and fairness controls",
            *all_reasons,
        ],
        None,
    )


def benchmark_command(
    binary: Path,
    cell: MatrixCell,
    *,
    instances: int,
    min_mode_ms: int,
    seed: int,
    order: str,
    owner_cpu: str,
) -> list[str]:
    return [
        str(binary),
        "--workload",
        cell.workload,
        "--candidate",
        cell.candidate,
        "--instances",
        str(instances),
        "--frame-bytes",
        str(cell.frame_bytes),
        "--cell-bytes",
        str(cell.cell_bytes),
        "--sites",
        str(cell.sites),
        "--budget",
        str(cell.budget),
        "--chain",
        str(cell.chain),
        "--producers",
        str(cell.producers),
        "--min-mode-ms",
        str(min_mode_ms),
        "--seed",
        str(seed),
        "--order",
        order.lower(),
        "--owner-cpu",
        owner_cpu,
    ]


def _validate_echo(
    row: PairRow,
    cell: MatrixCell,
    *,
    instances: int,
    min_mode_ms: int,
    order: str,
) -> None:
    expected = (
        cell.workload,
        cell.candidate,
        cell.baseline,
        instances,
        cell.frame_bytes,
        cell.cell_bytes,
        cell.sites,
        cell.budget,
        cell.chain,
        cell.producers,
        min_mode_ms * 1_000_000,
        order,
    )
    actual = (
        row.workload,
        row.candidate,
        row.baseline,
        row.instances,
        row.frame_bytes,
        row.cell_bytes,
        row.sites,
        row.budget,
        row.chain,
        row.producers,
        row.min_mode_ns,
        row.order,
    )
    if actual != expected:
        raise ValueError(
            f"benchmark echoed the wrong cell: expected={expected} "
            f"actual={actual}"
        )


def _run_matrix(
    binary: Path,
    cells: list[MatrixCell],
    *,
    samples: int,
    instances: int,
    min_mode_ms: int,
    seed: int,
    owner_cpu: str,
) -> list[SampleRow]:
    collected: list[SampleRow] = []
    total = len(cells) * samples
    completed = 0
    timeout = max(120.0, min_mode_ms / 1000.0 * 20.0)

    for sample_index in range(1, samples + 1):
        offset = (sample_index - 1) % len(cells)
        ordered = cells[offset:] + cells[:offset]
        if sample_index % 2 == 0:
            ordered = list(reversed(ordered))
        order = "ABBA" if sample_index % 2 == 1 else "BAAB"
        for cell in ordered:
            command = benchmark_command(
                binary,
                cell,
                instances=instances,
                min_mode_ms=min_mode_ms,
                seed=seed,
                order=order,
                owner_cpu=owner_cpu,
            )
            try:
                result = run_capture(command, timeout=timeout)
            except (OSError, ProcessTimeoutError) as exc:
                raise MatrixRunError(
                    f"benchmark could not complete: {shlex.join(command)}: "
                    f"{exc}",
                    collected.copy(),
                ) from None
            if result.returncode != 0:
                raise MatrixRunError(
                    f"benchmark failed ({result.returncode}): "
                    f"{shlex.join(command)}\n"
                    f"{result.stdout}{result.stderr}",
                    collected.copy(),
                )
            try:
                row = parse_output(result.stdout)
                _validate_echo(
                    row,
                    cell,
                    instances=instances,
                    min_mode_ms=min_mode_ms,
                    order=order,
                )
            except ValueError as exc:
                raise MatrixRunError(
                    f"benchmark integrity failure: {exc}",
                    collected.copy(),
                ) from None
            collected.append(SampleRow(sample_index, row))
            completed += 1
            print(
                f"[lccf-phase0] sample {completed}/{total}: "
                f"{cell.workload} {cell.candidate} "
                f"frame={cell.frame_bytes} cell={cell.cell_bytes} "
                f"sites={cell.sites}",
                file=sys.stderr,
            )
    return collected


def _write_samples_csv(path: Path, rows: list[SampleRow]) -> None:
    pair_fields = list(PairRow.__dataclass_fields__)
    fieldnames = ["process_sample", *pair_fields]
    with open_text_for_write(path, newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for sample in rows:
            values = asdict(sample.row)
            values["process_sample"] = sample.process_sample
            writer.writerow(values)


def _write_summary_csv(path: Path, rows: list[SummaryRow]) -> None:
    fieldnames = list(SummaryRow.__dataclass_fields__)
    with open_text_for_write(path, newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def _gate_table(rows: list[SummaryRow]) -> list[str]:
    lines = [
        "| Cell bytes | Workload | Fused worst wall | "
        "Fused worst CPU | Queue floor | Remote floor | Fairness p99 |",
        "|---:|---|---:|---:|---:|---:|---:|",
    ]
    for cell_bytes in CELL_BYTES:
        layout = [row for row in rows if row.cell_bytes == cell_bytes]
        queue_floor = min(
            (
                row.wall_speedup
                for row in layout
                if row.candidate == "causal_cell_queue"
            ),
            default=0.0,
        )
        remote_floor = min(
            (
                row.wall_speedup
                for row in layout
                if row.candidate == "remote_causal_cell"
            ),
            default=0.0,
        )
        fairness = max(
            (
                row.fairness_p99_ratio
                for row in layout
                if row.workload == MIXED_WORKLOAD
            ),
            default=0.0,
        )
        for workload in FUNCTIONAL_WORKLOADS:
            fused = [
                row
                for row in layout
                if row.candidate == "fused_causal_cell"
                and row.workload == workload
            ]
            lines.append(
                f"| {cell_bytes} | {workload} | "
                f"{min((row.wall_speedup for row in fused), default=0.0):.3f}x | "
                f"{max((row.cpu_ratio for row in fused), default=0.0):.3f}x | "
                f"{queue_floor:.3f}x | {remote_floor:.3f}x | "
                f"{fairness:.3f}x |"
            )
    lines.append("")
    return lines


def _render_report(
    out_dir: Path,
    summary: list[SummaryRow],
    verdict: str,
    reasons: list[str],
    selected_cell_bytes: int | None,
    metadata: dict[str, object],
) -> str:
    lines = [
        "# LCCF Phase 0 Paired Cost Model",
        "",
        "## Verdict",
        "",
        f"`{verdict}`",
        "",
        f"Selected causal-cell footprint: "
        f"`{selected_cell_bytes if selected_cell_bytes is not None else 'none'}`",
        "",
    ]
    lines.extend(f"- {reason}" for reason in reasons)
    lines.extend(
        [
            "",
            "This is a standalone synthetic cost model, not production "
            "validation. It does not validate real io_uring, kqueue, IOCP, "
            "compiler lowering, or foreign-language ABI behavior.",
            "",
            "## Raw evidence",
            "",
            f"- Samples: `{out_dir / 'lccf_phase0_samples.csv'}`",
            f"- Summary: `{out_dir / 'lccf_phase0_summary.csv'}`",
            f"- Metadata: `{out_dir / 'lccf_phase0_metadata.json'}`",
            "",
            "## Gate overview",
            "",
            *_gate_table(summary),
            "## Complete paired summary",
            "",
            "| Workload | Candidate | Frame | Cell | Sites | Chain | "
            "Wall | CPU | Wall spread | CPU spread | Fairness p99 | Samples |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in sorted(
        summary,
        key=lambda item: tuple(str(value) for value in _cell_key_from_summary(item)),
    ):
        lines.append(
            f"| {row.workload} | {row.candidate} | {row.frame_bytes} | "
            f"{row.cell_bytes} | {row.sites} | {row.chain} | "
            f"{row.wall_speedup:.3f}x | {row.cpu_ratio:.3f}x | "
            f"{row.wall_ratio_spread:.3f}x | "
            f"{row.cpu_ratio_spread:.3f}x | "
            f"{row.fairness_p99_ratio:.3f}x | {row.sample_count} |"
        )
    lines.extend(["", "## Reproduction and host", ""])
    for key in sorted(metadata):
        if key == "command":
            continue
        value = json.dumps(metadata[key], ensure_ascii=False, sort_keys=True)
        lines.append(f"- {key}: `{value}`")
    lines.extend(
        [
            "",
            "```sh",
            str(metadata.get("command", "unavailable")),
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def write_evidence(
    out_dir: Path,
    tracked_report: Path | None,
    samples: list[SampleRow],
    summary: list[SummaryRow],
    verdict: str,
    reasons: list[str],
    selected_cell_bytes: int | None,
    metadata: dict[str, object],
) -> None:
    sample_path = out_dir / "lccf_phase0_samples.csv"
    summary_path = out_dir / "lccf_phase0_summary.csv"
    metadata_path = out_dir / "lccf_phase0_metadata.json"
    report_path = out_dir / "lccf_phase0_report.md"
    enriched = dict(metadata)
    enriched.update(
        {
            "verdict": verdict,
            "selected_cell_bytes": selected_cell_bytes,
            "reasons": reasons,
            "sample_rows": len(samples),
            "summary_rows": len(summary),
        }
    )
    report = _render_report(
        out_dir,
        summary,
        verdict,
        reasons,
        selected_cell_bytes,
        enriched,
    )
    _write_samples_csv(sample_path, samples)
    _write_summary_csv(summary_path, summary)
    write_text_safely(
        metadata_path,
        json.dumps(
            enriched,
            indent=2,
            ensure_ascii=False,
            sort_keys=True,
        )
        + "\n",
    )
    write_text_safely(report_path, report)
    if tracked_report is not None:
        write_text_safely(tracked_report, report)


def _bounded_int(name: str, minimum: int, maximum: int):
    def parse(value: str) -> int:
        if DECIMAL_RE.fullmatch(value) is None:
            raise argparse.ArgumentTypeError(
                f"{name} must be a decimal integer"
            )
        parsed = int(value, 10)
        if not minimum <= parsed <= maximum:
            raise argparse.ArgumentTypeError(
                f"{name} must be in {minimum}..{maximum}"
            )
        return parsed

    return parse


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the paired LCCF Phase 0 cost-model matrix."
    )
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=Path("object/lccf-phase0"),
    )
    parser.add_argument("--tracked-report", type=Path)
    parser.add_argument(
        "--samples",
        type=_bounded_int("samples", 1, 31),
        default=FULL_SAMPLES,
    )
    parser.add_argument(
        "--instances",
        type=_bounded_int("instances", 1, 0xFFFF_FFFF),
        default=65_536,
    )
    parser.add_argument(
        "--min-mode-ms",
        type=_bounded_int("min-mode-ms", 1, 60_000),
        default=250,
    )
    parser.add_argument(
        "--budget",
        type=_bounded_int("budget", 1, 1024),
        choices=(8,),
        default=8,
        help="protocol-locked direct-dispatch budget",
    )
    parser.add_argument(
        "--chain",
        type=_bounded_int("chain", 1, 1024),
        choices=(18,),
        default=18,
        help="protocol-locked budgeted-chain length",
    )
    parser.add_argument(
        "--producers",
        type=_bounded_int("producers", 1, 2),
        choices=(2,),
        default=2,
        help="protocol-locked remote producer count",
    )
    parser.add_argument(
        "--seed",
        type=_bounded_int("seed", 0, 0xFFFF_FFFF_FFFF_FFFF),
        default=7_810_762_890_074_515_045,
    )
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument(
        "--owner-cpu",
        default="auto",
        help="auto, none, or a decimal CPU index",
    )
    parser.add_argument("--quick", action="store_true")
    return parser


def _owner_cpu_argument(value: str) -> tuple[str, dict[str, object]]:
    if value == "none":
        return "none", {"owner_cpu_policy": "none"}
    if value != "auto":
        if DECIMAL_RE.fullmatch(value) is None:
            raise ValueError("--owner-cpu must be auto, none, or decimal")
        return value, {
            "owner_cpu_policy": "explicit",
            "owner_cpu_requested": int(value, 10),
        }
    if hasattr(os, "sched_getaffinity"):
        available = sorted(os.sched_getaffinity(0))
        if available:
            return str(available[0]), {
                "owner_cpu_policy": "lowest available",
                "available_cpus": available,
                "owner_cpu_requested": available[0],
            }
    return "none", {
        "owner_cpu_policy": "auto unavailable",
        "available_cpus": list(range(os.cpu_count() or 0)),
    }


def _tool_identity(command: list[str]) -> str:
    result = run_capture(command, timeout=15.0)
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
        raise RuntimeError(
            f"identity command produced no output: {command}"
        )
    return lines[0]


def _darwin_qos_metadata() -> dict[str, object]:
    if sys.platform != "darwin":
        return {}
    metadata: dict[str, object] = {}
    try:
        libc = ctypes.CDLL(None)
        pthread_self = libc.pthread_self
        pthread_self.restype = ctypes.c_void_p
        get_qos = libc.pthread_get_qos_class_np
        get_qos.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_int),
        ]
        qos_class = ctypes.c_uint()
        relative = ctypes.c_int()
        rc = get_qos(
            pthread_self(),
            ctypes.byref(qos_class),
            ctypes.byref(relative),
        )
        metadata["darwin_qos_result"] = rc
        metadata["darwin_qos_class"] = qos_class.value
        metadata["darwin_qos_relative_priority"] = relative.value
    except (AttributeError, OSError) as exc:
        metadata["darwin_qos_error"] = str(exc)
    try:
        metadata["darwin_activecpu"] = _tool_identity(
            ["sysctl", "-n", "hw.activecpu"]
        )
    except (OSError, RuntimeError) as exc:
        metadata["darwin_activecpu_error"] = str(exc)
    return metadata


def _metadata(
    *,
    binary: Path,
    cc: str,
    command: list[str],
    quick: bool,
    samples: int,
    instances: int,
    min_mode_ms: int,
    budget: int,
    chain: int,
    producers: int,
    seed: int,
    owner_metadata: dict[str, object],
) -> dict[str, object]:
    try:
        compiler = _tool_identity([cc, "--version"])
    except (OSError, RuntimeError) as exc:
        compiler = f"unavailable: {exc}"
    try:
        commit = _tool_identity(["git", "rev-parse", "HEAD"])
    except (OSError, RuntimeError) as exc:
        commit = f"unavailable: {exc}"
    try:
        digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    except OSError as exc:
        digest = f"unavailable: {exc}"
    metadata: dict[str, object] = {
        "binary": str(binary),
        "binary_sha256": digest,
        "command": shlex.join(command),
        "commit": commit,
        "compiler": compiler,
        "host": platform.platform(),
        "machine": platform.machine(),
        "python": platform.python_version(),
        "quick": quick,
        "process_samples": samples,
        "instances": instances,
        "min_mode_ms": min_mode_ms,
        "budget": budget,
        "chain": chain,
        "producers": producers,
        "seed": seed,
        "active_processor_count": os.cpu_count(),
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
    }
    metadata.update(owner_metadata)
    metadata.update(_darwin_qos_metadata())
    return metadata


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if args.quick:
        args.samples = 1
        args.instances = 4096
        args.min_mode_ms = 20
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"--binary is not a regular file: {binary}")
    try:
        owner_cpu, owner_metadata = _owner_cpu_argument(
            args.owner_cpu
        )
    except ValueError as exc:
        parser.error(str(exc))

    cells = quick_matrix() if args.quick else full_matrix()
    command_args = (
        [sys.executable, *sys.argv]
        if argv is None
        else [sys.executable, str(Path(__file__)), *argv]
    )
    metadata = _metadata(
        binary=binary,
        cc=args.cc,
        command=command_args,
        quick=args.quick,
        samples=args.samples,
        instances=args.instances,
        min_mode_ms=args.min_mode_ms,
        budget=args.budget,
        chain=args.chain,
        producers=args.producers,
        seed=args.seed,
        owner_metadata=owner_metadata,
    )
    samples: list[SampleRow] = []
    summary: list[SummaryRow] = []
    verdict = "INCONCLUSIVE"
    reasons: list[str] = []
    selected: int | None = None
    integrity_failure = False

    try:
        samples = _run_matrix(
            binary,
            cells,
            samples=args.samples,
            instances=args.instances,
            min_mode_ms=args.min_mode_ms,
            seed=args.seed,
            owner_cpu=owner_cpu,
        )
        summary = summarize(samples)
        verdict, reasons, selected = classify(
            summary, args.samples
        )
        integrity_failure = verdict == "INCONCLUSIVE"
    except MatrixRunError as exc:
        samples = exc.samples
        try:
            summary = summarize(samples)
        except ValueError:
            summary = []
        reasons = [str(exc)]
        integrity_failure = True
    except (OSError, RuntimeError, ValueError) as exc:
        reasons = [f"runner integrity failure: {exc}"]
        integrity_failure = True

    try:
        write_evidence(
            args.out_dir,
            args.tracked_report,
            samples,
            summary,
            verdict,
            reasons,
            selected,
            metadata,
        )
    except (OSError, RuntimeError) as exc:
        print(f"[lccf-phase0] evidence error: {exc}", file=sys.stderr)
        return 1

    print(
        f"[lccf-phase0] verdict={verdict} "
        f"selected_cell_bytes={selected} "
        f"samples={len(samples)} summary={len(summary)}",
        file=sys.stderr,
    )
    return 1 if integrity_failure else 0


if __name__ == "__main__":
    raise SystemExit(main())
