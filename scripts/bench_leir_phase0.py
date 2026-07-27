#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run and classify the native LEIR Phase 0A paired benchmark."""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
import platform
import re
import shlex
import statistics
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Sequence

try:
    from process_utils import (
        CapturedProcess,
        ProcessTimeoutError,
        run_capture,
    )
    from safe_output import write_text_safely
except ModuleNotFoundError:
    from scripts.process_utils import (
        CapturedProcess,
        ProcessTimeoutError,
        run_capture,
    )
    from scripts.safe_output import write_text_safely


RESULT_PREFIX = "LEIR_PAIR "
CORE_WORKLOADS = ("socket_relay", "framed_rpc")
CONTROL_WORKLOADS = ("graph_break",)
WORKLOADS = {*CORE_WORKLOADS, *CONTROL_WORKLOADS}
NODES = (1, 2, 4, 8)
CONCURRENCY = (1, 64, 512)
PAYLOADS = (64, 1024, 16384)
INLINE_BUDGETS = (1, 8, 32)
GATE_NODES = (1, 4, 8)
GATE_CONCURRENCY = (64, 512)
GATE_PAYLOADS = (64, 1024)
GATE_INLINE_BUDGET = 8
SCREEN_SAMPLES = 5
GATE_SAMPLES = 9
SCREEN_MIN_MODE_NS = 100_000_000
GATE_MIN_MODE_NS = 250_000_000
BLOCK_COUNT = 16

DECIMAL_RE = re.compile(r"[0-9]+")
RATIO_RE = re.compile(r"[0-9]+\.[0-9]{9}")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")
TOKEN_RE = re.compile(r"[A-Za-z0-9_.:-]+")

FIELD_ORDER = (
    "version",
    "workload",
    "nodes",
    "concurrency",
    "payload",
    "inline_budget",
    "activations",
    "min_mode_ns",
    "blocks_per_mode",
    "baseline_wall_ns",
    "candidate_wall_ns",
    "baseline_cpu_ns",
    "candidate_cpu_ns",
    "wall_speedup",
    "cpu_ratio",
    "baseline_ctx_switches",
    "candidate_ctx_switches",
    "baseline_task_io_submits",
    "candidate_task_io_submits",
    "baseline_task_io_completions",
    "candidate_task_io_completions",
    "candidate_backend_submits",
    "effect_completions",
    "direct_completions",
    "terminal_publications",
    "resumes_avoided",
    "fairness_resubmits",
    "heap_requests",
    "hot_allocations",
    "baseline_checksum",
    "candidate_checksum",
    "pending_path_valid",
    "baseline_service_gap_p99_ns",
    "candidate_service_gap_p99_ns",
    "baseline_terminal_p99_ns",
    "candidate_terminal_p99_ns",
    "peer",
    "cpu_scope",
    "order",
)

RATIO_FIELDS = {"wall_speedup", "cpu_ratio"}
TEXT_FIELDS = {
    "version",
    "workload",
    "baseline_checksum",
    "candidate_checksum",
    "peer",
    "cpu_scope",
    "order",
}
INTEGER_FIELDS = set(FIELD_ORDER) - RATIO_FIELDS - TEXT_FIELDS


@dataclass(frozen=True)
class MatrixCell:
    workload: str
    nodes: int
    concurrency: int
    payload: int
    inline_budget: int


@dataclass(frozen=True)
class PairRow:
    workload: str
    nodes: int
    concurrency: int
    payload: int
    inline_budget: int
    activations: int
    min_mode_ns: int
    blocks_per_mode: int
    baseline_wall_ns: int
    candidate_wall_ns: int
    baseline_cpu_ns: int
    candidate_cpu_ns: int
    wall_speedup: float
    cpu_ratio: float
    baseline_ctx_switches: int
    candidate_ctx_switches: int
    baseline_task_io_submits: int
    candidate_task_io_submits: int
    baseline_task_io_completions: int
    candidate_task_io_completions: int
    candidate_backend_submits: int
    effect_completions: int
    direct_completions: int
    terminal_publications: int
    resumes_avoided: int
    fairness_resubmits: int
    heap_requests: int
    hot_allocations: int
    baseline_checksum: str
    candidate_checksum: str
    pending_path_valid: int
    baseline_service_gap_p99_ns: int
    candidate_service_gap_p99_ns: int
    baseline_terminal_p99_ns: int
    candidate_terminal_p99_ns: int
    peer: str
    cpu_scope: str
    order: str

    @property
    def cell(self) -> MatrixCell:
        return MatrixCell(
            self.workload,
            self.nodes,
            self.concurrency,
            self.payload,
            self.inline_budget,
        )


@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: PairRow


@dataclass(frozen=True)
class SummaryRow:
    workload: str
    nodes: int
    concurrency: int
    payload: int
    inline_budget: int
    sample_count: int
    min_mode_ns: int
    wall_speedup: float
    cpu_ratio: float
    wall_ratio_spread: float
    cpu_ratio_spread: float
    baseline_ctx_switches: float
    candidate_ctx_switches: float
    baseline_task_io_submits: float
    candidate_task_io_submits: float
    effect_completions: float
    terminal_publications: float
    resumes_avoided: float
    baseline_service_gap_p99_ns: float
    candidate_service_gap_p99_ns: float
    service_gap_p99_ratio: float
    baseline_terminal_p99_ns: float
    candidate_terminal_p99_ns: float
    terminal_p99_ratio: float
    path_valid: bool
    checksum_valid: bool
    allocation_valid: bool
    mechanism_valid: bool
    peer: str
    cpu_scope: str

    @property
    def cell(self) -> MatrixCell:
        return MatrixCell(
            self.workload,
            self.nodes,
            self.concurrency,
            self.payload,
            self.inline_budget,
        )


class MatrixRunError(RuntimeError):
    def __init__(
        self,
        message: str,
        samples: Sequence[SampleRow] = (),
    ) -> None:
        super().__init__(message)
        self.samples = list(samples)


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (
        cell.workload,
        cell.nodes,
        cell.concurrency,
        cell.payload,
        cell.inline_budget,
    )


def _split_output(text: str) -> dict[str, str]:
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one LEIR_PAIR result row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty LEIR_PAIR result row")

    fields: dict[str, str] = {}
    observed_order: list[str] = []
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed LEIR_PAIR field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(
                f"duplicate or empty LEIR_PAIR field: {key}"
            )
        fields[key] = value
        observed_order.append(key)
    if tuple(observed_order) != FIELD_ORDER:
        raise ValueError(
            "LEIR_PAIR field order/schema mismatch: "
            f"observed={observed_order}"
        )
    return fields


def _parse_integer(name: str, text: str) -> int:
    if DECIMAL_RE.fullmatch(text) is None:
        raise ValueError(f"invalid decimal integer field {name}")
    value = int(text, 10)
    if value > 0xFFFF_FFFF_FFFF_FFFF:
        raise ValueError(f"integer field out of uint64 range: {name}")
    return value


def _parse_ratio(name: str, text: str) -> float:
    if RATIO_RE.fullmatch(text) is None:
        raise ValueError(f"invalid fixed-point ratio field {name}")
    value = float(text)
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError(f"non-positive or non-finite ratio field {name}")
    return value


def _ratio_matches(printed: float, actual: float) -> bool:
    return (
        math.isfinite(actual)
        and actual > 0.0
        and math.isclose(
            printed,
            actual,
            rel_tol=1e-8,
            abs_tol=5e-10,
        )
    )


def parse_output(
    text: str,
    *,
    allow_combined_cpu: bool = False,
) -> PairRow:
    fields = _split_output(text)
    if fields["version"] != "1":
        raise ValueError("unsupported LEIR_PAIR version")
    integers = {
        name: _parse_integer(name, fields[name])
        for name in INTEGER_FIELDS
    }
    ratios = {
        name: _parse_ratio(name, fields[name])
        for name in RATIO_FIELDS
    }

    workload = fields["workload"]
    if workload not in WORKLOADS:
        raise ValueError("unknown LEIR workload")
    if integers["nodes"] not in NODES:
        raise ValueError("unsupported LEIR node count")
    if workload == "graph_break" and integers["nodes"] != 1:
        raise ValueError("graph_break must use one LEIR node")
    if not 1 <= integers["concurrency"] <= 512:
        raise ValueError("concurrency out of range")
    if not 64 <= integers["payload"] <= 16384:
        raise ValueError("payload out of range")
    if not 1 <= integers["inline_budget"] <= 32:
        raise ValueError("inline budget out of range")
    if integers["activations"] <= 0:
        raise ValueError("activations must be positive")
    if not 1_000_000 <= integers["min_mode_ns"] <= 60_000_000_000:
        raise ValueError("minimum mode duration out of range")
    if integers["blocks_per_mode"] != BLOCK_COUNT:
        raise ValueError("unexpected balanced block count")

    positive_fields = (
        "baseline_wall_ns",
        "candidate_wall_ns",
        "baseline_cpu_ns",
        "candidate_cpu_ns",
        "baseline_task_io_submits",
        "candidate_task_io_submits",
        "baseline_task_io_completions",
        "candidate_task_io_completions",
        "effect_completions",
        "terminal_publications",
        "baseline_service_gap_p99_ns",
        "candidate_service_gap_p99_ns",
        "baseline_terminal_p99_ns",
        "candidate_terminal_p99_ns",
    )
    if any(integers[name] <= 0 for name in positive_fields):
        raise ValueError("zero-valued required LEIR measurement")
    if not _ratio_matches(
        ratios["wall_speedup"],
        integers["baseline_wall_ns"]
        / integers["candidate_wall_ns"],
    ):
        raise ValueError("wall speedup disagrees with native clocks")
    if not _ratio_matches(
        ratios["cpu_ratio"],
        integers["candidate_cpu_ns"]
        / integers["baseline_cpu_ns"],
    ):
        raise ValueError("CPU ratio disagrees with native clocks")
    if (
        integers["baseline_task_io_submits"]
        != integers["baseline_task_io_completions"]
        or integers["candidate_task_io_submits"]
        != integers["candidate_task_io_completions"]
    ):
        raise ValueError("task-level I/O counters are unbalanced")
    if (
        integers["candidate_backend_submits"]
        + integers["direct_completions"]
        != integers["effect_completions"]
    ):
        raise ValueError("candidate effect/backend counters are unbalanced")
    if integers["heap_requests"] != 0 or integers["hot_allocations"] != 0:
        raise ValueError("allocation-bearing LEIR row")
    if integers["pending_path_valid"] != 1:
        raise ValueError("LEIR pending path was not validated")

    for name in ("baseline_checksum", "candidate_checksum"):
        if CHECKSUM_RE.fullmatch(fields[name]) is None:
            raise ValueError(f"invalid checksum field {name}")
    if fields["baseline_checksum"] != fields["candidate_checksum"]:
        raise ValueError("baseline/candidate checksum mismatch")
    if TOKEN_RE.fullmatch(fields["peer"]) is None:
        raise ValueError("invalid peer kind")
    if fields["peer"] not in {"process", "thread"}:
        raise ValueError("unknown peer kind")
    if fields["cpu_scope"] not in {"server", "combined"}:
        raise ValueError("unknown CPU clock scope")
    if (
        fields["peer"] == "process"
        and fields["cpu_scope"] != "server"
    ) or (
        fields["peer"] == "thread"
        and fields["cpu_scope"] != "combined"
    ):
        raise ValueError("peer kind and CPU scope disagree")
    if fields["cpu_scope"] == "combined" and not allow_combined_cpu:
        raise ValueError("combined CPU scope cannot drive a gate row")
    if fields["order"] not in {"ABBA", "BAAB"}:
        raise ValueError("invalid balanced block order")

    return PairRow(
        workload=workload,
        nodes=integers["nodes"],
        concurrency=integers["concurrency"],
        payload=integers["payload"],
        inline_budget=integers["inline_budget"],
        activations=integers["activations"],
        min_mode_ns=integers["min_mode_ns"],
        blocks_per_mode=integers["blocks_per_mode"],
        baseline_wall_ns=integers["baseline_wall_ns"],
        candidate_wall_ns=integers["candidate_wall_ns"],
        baseline_cpu_ns=integers["baseline_cpu_ns"],
        candidate_cpu_ns=integers["candidate_cpu_ns"],
        wall_speedup=ratios["wall_speedup"],
        cpu_ratio=ratios["cpu_ratio"],
        baseline_ctx_switches=integers["baseline_ctx_switches"],
        candidate_ctx_switches=integers["candidate_ctx_switches"],
        baseline_task_io_submits=integers[
            "baseline_task_io_submits"
        ],
        candidate_task_io_submits=integers[
            "candidate_task_io_submits"
        ],
        baseline_task_io_completions=integers[
            "baseline_task_io_completions"
        ],
        candidate_task_io_completions=integers[
            "candidate_task_io_completions"
        ],
        candidate_backend_submits=integers[
            "candidate_backend_submits"
        ],
        effect_completions=integers["effect_completions"],
        direct_completions=integers["direct_completions"],
        terminal_publications=integers["terminal_publications"],
        resumes_avoided=integers["resumes_avoided"],
        fairness_resubmits=integers["fairness_resubmits"],
        heap_requests=integers["heap_requests"],
        hot_allocations=integers["hot_allocations"],
        baseline_checksum=fields["baseline_checksum"],
        candidate_checksum=fields["candidate_checksum"],
        pending_path_valid=integers["pending_path_valid"],
        baseline_service_gap_p99_ns=integers[
            "baseline_service_gap_p99_ns"
        ],
        candidate_service_gap_p99_ns=integers[
            "candidate_service_gap_p99_ns"
        ],
        baseline_terminal_p99_ns=integers[
            "baseline_terminal_p99_ns"
        ],
        candidate_terminal_p99_ns=integers[
            "candidate_terminal_p99_ns"
        ],
        peer=fields["peer"],
        cpu_scope=fields["cpu_scope"],
        order=fields["order"],
    )


def screen_matrix() -> list[MatrixCell]:
    cells = [
        MatrixCell(workload, nodes, concurrency, payload, budget)
        for workload in CORE_WORKLOADS
        for nodes in NODES
        for concurrency in CONCURRENCY
        for payload in PAYLOADS
        for budget in INLINE_BUDGETS
    ]
    cells.extend(
        MatrixCell("graph_break", 1, concurrency, payload, 8)
        for concurrency in CONCURRENCY
        for payload in PAYLOADS
    )
    return cells


def gate_matrix() -> list[MatrixCell]:
    cells = [
        MatrixCell(workload, nodes, concurrency, payload, 8)
        for workload in CORE_WORKLOADS
        for nodes in GATE_NODES
        for concurrency in GATE_CONCURRENCY
        for payload in GATE_PAYLOADS
    ]
    cells.extend(
        MatrixCell("graph_break", 1, concurrency, payload, 8)
        for concurrency in GATE_CONCURRENCY
        for payload in GATE_PAYLOADS
    )
    return cells


def benchmark_command(
    binary: Path,
    cell: MatrixCell,
    *,
    activations: int,
    min_mode_ms: int,
    order: str,
) -> list[str]:
    if activations <= 0 or min_mode_ms <= 0:
        raise ValueError("benchmark counts must be positive")
    if order not in {"ABBA", "BAAB"}:
        raise ValueError("invalid benchmark order")
    return [
        str(binary.resolve()),
        "--workload",
        cell.workload,
        "--nodes",
        str(cell.nodes),
        "--concurrency",
        str(cell.concurrency),
        "--payload",
        str(cell.payload),
        "--inline-budget",
        str(cell.inline_budget),
        "--activations",
        str(activations),
        "--min-mode-ms",
        str(min_mode_ms),
        "--order",
        order,
    ]


Runner = Callable[..., CapturedProcess]


def run_one(
    binary: Path,
    cell: MatrixCell,
    *,
    process_sample: int,
    activations: int,
    min_mode_ms: int,
    runner: Runner = run_capture,
    timeout: float | None = None,
) -> SampleRow:
    if process_sample <= 0:
        raise ValueError("process sample index must be positive")
    order = "ABBA" if process_sample % 2 else "BAAB"
    command = benchmark_command(
        binary,
        cell,
        activations=activations,
        min_mode_ms=min_mode_ms,
        order=order,
    )
    effective_timeout = (
        max(30.0, min_mode_ms / 1000.0 * 20.0)
        if timeout is None
        else timeout
    )
    try:
        result = runner(command, timeout=effective_timeout)
    except (OSError, ProcessTimeoutError) as exc:
        raise MatrixRunError(
            f"sample {process_sample} process failure: {exc}"
        ) from exc
    if result.returncode != 0:
        raise MatrixRunError(
            f"sample {process_sample} native exit "
            f"{result.returncode}: {result.stderr!r}"
        )
    if result.stderr:
        raise MatrixRunError(
            f"sample {process_sample} emitted stderr diagnostics: "
            f"{result.stderr!r}"
        )
    try:
        row = parse_output(
            result.stdout,
            allow_combined_cpu=(os.name == "nt"),
        )
    except ValueError as exc:
        raise MatrixRunError(
            f"sample {process_sample} result contract: {exc}"
        ) from exc
    if (
        _cell_key(row.cell) != _cell_key(cell)
        or row.order != order
        or row.min_mode_ns != min_mode_ms * 1_000_000
        or row.activations < activations
    ):
        raise MatrixRunError(
            f"sample {process_sample} native row disagrees with command"
        )
    return SampleRow(process_sample=process_sample, row=row)


def run_matrix(
    binary: Path,
    cells: Sequence[MatrixCell],
    *,
    samples: int,
    activations: int,
    min_mode_ms: int,
) -> list[SampleRow]:
    if samples <= 0 or samples % 2 == 0:
        raise ValueError("an odd positive sample count is required")
    rows: list[SampleRow] = []
    total = len(cells) * samples
    for cell_index, cell in enumerate(cells, start=1):
        for process_sample in range(1, samples + 1):
            try:
                sample = run_one(
                    binary,
                    cell,
                    process_sample=process_sample,
                    activations=activations,
                    min_mode_ms=min_mode_ms,
                )
            except MatrixRunError as exc:
                raise MatrixRunError(
                    f"cell {cell_index}/{len(cells)} "
                    f"{_cell_key(cell)}: {exc}",
                    rows,
                ) from exc
            rows.append(sample)
            print(
                f"[leir-phase0a] sample {len(rows)}/{total} "
                f"cell={_cell_key(cell)} order={sample.row.order}",
                file=sys.stderr,
                flush=True,
            )
    return rows


def _mechanism_valid(row: PairRow) -> bool:
    blocks_for_each_mode = row.blocks_per_mode // 2
    aggregate_activations = row.activations * blocks_for_each_mode
    effect_nodes = 1 if row.workload == "graph_break" else row.nodes
    transactions = 1 if effect_nodes == 1 else effect_nodes // 2
    expected_baseline_io = aggregate_activations * transactions * 2
    expected_candidate_io = aggregate_activations * (
        2 if effect_nodes == 1 else 1
    )
    return (
        row.blocks_per_mode == BLOCK_COUNT
        and row.baseline_task_io_submits == expected_baseline_io
        and row.baseline_task_io_completions == expected_baseline_io
        and row.candidate_task_io_submits == expected_candidate_io
        and row.candidate_task_io_completions == expected_candidate_io
        and row.effect_completions
        == aggregate_activations * effect_nodes
        and row.terminal_publications == aggregate_activations
        and row.resumes_avoided
        == aggregate_activations * (effect_nodes - 1)
        and row.candidate_backend_submits + row.direct_completions
        == row.effect_completions
        and row.pending_path_valid == 1
        and row.heap_requests == 0
        and row.hot_allocations == 0
    )


def _median(values: Sequence[int | float]) -> float:
    return float(statistics.median(values))


def summarize(samples: Sequence[SampleRow]) -> list[SummaryRow]:
    grouped: dict[tuple[object, ...], list[SampleRow]] = {}
    for sample in samples:
        grouped.setdefault(_cell_key(sample.row.cell), []).append(sample)

    summaries: list[SummaryRow] = []
    for key in sorted(grouped):
        group = sorted(
            grouped[key], key=lambda sample: sample.process_sample
        )
        if len({sample.process_sample for sample in group}) != len(group):
            raise ValueError(f"duplicate process sample index for {key}")
        rows = [sample.row for sample in group]
        wall = [row.wall_speedup for row in rows]
        cpu = [row.cpu_ratio for row in rows]
        service_ratios = [
            row.candidate_service_gap_p99_ns
            / row.baseline_service_gap_p99_ns
            for row in rows
        ]
        terminal_ratios = [
            row.candidate_terminal_p99_ns
            / row.baseline_terminal_p99_ns
            for row in rows
        ]
        peers = {row.peer for row in rows}
        scopes = {row.cpu_scope for row in rows}
        first = rows[0]
        summaries.append(
            SummaryRow(
                workload=first.workload,
                nodes=first.nodes,
                concurrency=first.concurrency,
                payload=first.payload,
                inline_budget=first.inline_budget,
                sample_count=len(rows),
                min_mode_ns=min(row.min_mode_ns for row in rows),
                wall_speedup=_median(wall),
                cpu_ratio=_median(cpu),
                wall_ratio_spread=max(wall) / min(wall),
                cpu_ratio_spread=max(cpu) / min(cpu),
                baseline_ctx_switches=_median(
                    [row.baseline_ctx_switches for row in rows]
                ),
                candidate_ctx_switches=_median(
                    [row.candidate_ctx_switches for row in rows]
                ),
                baseline_task_io_submits=_median(
                    [row.baseline_task_io_submits for row in rows]
                ),
                candidate_task_io_submits=_median(
                    [row.candidate_task_io_submits for row in rows]
                ),
                effect_completions=_median(
                    [row.effect_completions for row in rows]
                ),
                terminal_publications=_median(
                    [row.terminal_publications for row in rows]
                ),
                resumes_avoided=_median(
                    [row.resumes_avoided for row in rows]
                ),
                baseline_service_gap_p99_ns=_median(
                    [row.baseline_service_gap_p99_ns for row in rows]
                ),
                candidate_service_gap_p99_ns=_median(
                    [row.candidate_service_gap_p99_ns for row in rows]
                ),
                service_gap_p99_ratio=_median(service_ratios),
                baseline_terminal_p99_ns=_median(
                    [row.baseline_terminal_p99_ns for row in rows]
                ),
                candidate_terminal_p99_ns=_median(
                    [row.candidate_terminal_p99_ns for row in rows]
                ),
                terminal_p99_ratio=_median(terminal_ratios),
                path_valid=all(
                    row.pending_path_valid == 1 for row in rows
                ),
                checksum_valid=all(
                    row.baseline_checksum == row.candidate_checksum
                    for row in rows
                ),
                allocation_valid=all(
                    row.heap_requests == 0
                    and row.hot_allocations == 0
                    for row in rows
                ),
                mechanism_valid=all(
                    _mechanism_valid(row) for row in rows
                ),
                peer=next(iter(peers)) if len(peers) == 1 else "mixed",
                cpu_scope=(
                    next(iter(scopes)) if len(scopes) == 1 else "mixed"
                ),
            )
        )
    return summaries


def _is_gate_core(row: SummaryRow) -> bool:
    return (
        row.workload in CORE_WORKLOADS
        and row.nodes in {4, 8}
        and row.concurrency in GATE_CONCURRENCY
        and row.payload in GATE_PAYLOADS
        and row.inline_budget == GATE_INLINE_BUDGET
    )


def _is_short_control(row: SummaryRow) -> bool:
    return (
        (
            row.workload in CORE_WORKLOADS
            and row.nodes == 1
            and row.concurrency in GATE_CONCURRENCY
            and row.payload in GATE_PAYLOADS
            and row.inline_budget == GATE_INLINE_BUDGET
        )
        or (
            row.workload == "graph_break"
            and row.nodes == 1
            and row.concurrency in GATE_CONCURRENCY
            and row.payload in GATE_PAYLOADS
            and row.inline_budget == GATE_INLINE_BUDGET
        )
    )


def classify(
    summaries: Sequence[SummaryRow],
    *,
    phase: str,
    expected_samples: int,
    min_mode_ns: int,
    expected_cells: Sequence[MatrixCell] | None = None,
) -> tuple[str, list[str]]:
    if phase not in {"screen", "gate"}:
        raise ValueError("classification phase must be screen or gate")
    cells = (
        list(expected_cells)
        if expected_cells is not None
        else (screen_matrix() if phase == "screen" else gate_matrix())
    )
    expected = {_cell_key(cell) for cell in cells}
    by_key: dict[tuple[object, ...], SummaryRow] = {}
    duplicate_keys: set[tuple[object, ...]] = set()
    for row in summaries:
        key = _cell_key(row.cell)
        if key in by_key:
            duplicate_keys.add(key)
        by_key[key] = row
    actual = set(by_key)
    integrity_reasons: list[str] = []
    if duplicate_keys:
        integrity_reasons.append(
            f"duplicate summary cells: {sorted(duplicate_keys)}"
        )
    if actual != expected:
        integrity_reasons.append(
            "matrix coverage mismatch: "
            f"missing={sorted(expected - actual)} "
            f"extra={sorted(actual - expected)}"
        )
    for key in sorted(actual & expected):
        row = by_key[key]
        if row.sample_count != expected_samples:
            integrity_reasons.append(
                f"sample count {row.sample_count}/{expected_samples} for {key}"
            )
        if row.min_mode_ns < min_mode_ns:
            integrity_reasons.append(
                f"minimum duration below {min_mode_ns} ns for {key}"
            )
        if not (
            row.path_valid
            and row.checksum_valid
            and row.allocation_valid
            and row.mechanism_valid
        ):
            integrity_reasons.append(
                f"mechanism/correctness control failed for {key}"
            )
        if row.wall_ratio_spread > 1.10:
            integrity_reasons.append(
                f"wall spread above 1.10x for {key}"
            )
        if row.cpu_ratio_spread > 1.10:
            integrity_reasons.append(
                f"CPU spread above 1.10x for {key}"
            )
    if integrity_reasons:
        return "INCONCLUSIVE", integrity_reasons

    relevant = [
        row
        for row in summaries
        if _is_gate_core(row) or _is_short_control(row)
    ]
    if not relevant:
        return "INCONCLUSIVE", ["no Phase 0A gate-driving rows"]
    if any(row.cpu_scope != "server" for row in relevant):
        return (
            "INCONCLUSIVE",
            [
                "server-only CPU evidence is unavailable; "
                "combined peer/server CPU rows are smoke-only"
            ],
        )

    latency_failures = [
        _cell_key(row.cell)
        for row in relevant
        if row.service_gap_p99_ratio > 1.10
        or row.terminal_p99_ratio > 1.10
    ]
    short_rows = [row for row in relevant if _is_short_control(row)]
    short_regressions = [
        _cell_key(row.cell)
        for row in short_rows
        if row.wall_speedup < 0.95 or row.cpu_ratio > 1.05
    ]
    if short_regressions:
        return (
            "ADVANCE_REJECT",
            [
                "short-region guard regressed for "
                f"{short_regressions}"
            ],
        )
    if latency_failures:
        return (
            "INCONCLUSIVE",
            [
                "p99 terminal/service-gap degradation above 1.10x for "
                f"{latency_failures}"
            ],
        )

    core_rows = [row for row in relevant if _is_gate_core(row)]
    pass_core = all(
        row.wall_speedup >= 1.25 and row.cpu_ratio <= 0.85
        for row in core_rows
    )
    pass_short = all(
        row.wall_speedup >= 0.95 and row.cpu_ratio <= 1.05
        for row in short_rows
    )
    if pass_core and pass_short:
        return (
            "ADVANCE_PASS",
            [
                "all core continuation, short-region, latency, "
                "correctness, allocation, and spread gates passed"
            ],
        )

    reject_workloads: list[str] = []
    for workload in CORE_WORKLOADS:
        workload_rows = [
            row for row in core_rows if row.workload == workload
        ]
        if (
            workload_rows
            and min(row.wall_speedup for row in workload_rows) < 1.10
            and max(row.cpu_ratio for row in workload_rows) > 0.95
        ):
            reject_workloads.append(workload)
    if reject_workloads:
        return (
            "ADVANCE_REJECT",
            [
                "stable core evidence is below the reject bound for "
                f"{reject_workloads}"
            ],
        )
    return (
        "INCONCLUSIVE",
        [
            "valid evidence lies between Phase 0A pass and reject bounds"
        ],
    )


def _csv_text(rows: Sequence[dict[str, object]]) -> str:
    if not rows:
        return ""
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue()


def _report_text(
    summaries: Sequence[SummaryRow],
    *,
    phase: str,
    verdict: str,
    reasons: Sequence[str],
) -> str:
    lines = [
        "# LEIR Phase 0A Userspace Advancement Evidence",
        "",
        f"- Phase: `{phase}`",
        f"- Verdict: **{verdict}**",
        (
            "- Meaning: `ADVANCE_PASS` authorizes only the next LEIR "
            "research phase; it is not `CATEGORY` and is not a production "
            "readiness verdict."
        ),
        "",
        "## Decision targets",
        "",
        "| Target | Final LEIR spec | Phase 0A continuation |",
        "|---|---:|---:|",
        "| Core wall speedup floor | 1.50x | 1.25x |",
        "| Core CPU ratio ceiling | 0.70x | 0.85x |",
        "| Short-region wall floor | 0.95x | 0.95x |",
        "| Short-region CPU ceiling | 1.05x | 1.05x |",
        "| Terminal/service p99 ratio ceiling | 1.10x | 1.10x |",
        "| Paired wall/CPU spread ceiling | 1.10x | 1.10x |",
        "",
        "## Classifier reasons",
        "",
    ]
    lines.extend(f"- {reason}" for reason in reasons)
    lines.extend(
        [
            "",
            "## Cell summaries",
            "",
            (
                "| workload | nodes | concurrency | payload | budget | "
                "samples | wall | CPU | wall spread | CPU spread | "
                "service p99 | terminal p99 | mechanism | CPU scope |"
            ),
            (
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"
                "---:|---:|---|---|"
            ),
        ]
    )
    for row in summaries:
        lines.append(
            f"| {row.workload} | {row.nodes} | {row.concurrency} | "
            f"{row.payload} | {row.inline_budget} | {row.sample_count} | "
            f"{row.wall_speedup:.6f}x | {row.cpu_ratio:.6f}x | "
            f"{row.wall_ratio_spread:.6f}x | "
            f"{row.cpu_ratio_spread:.6f}x | "
            f"{row.service_gap_p99_ratio:.6f}x | "
            f"{row.terminal_p99_ratio:.6f}x | "
            f"{'valid' if row.mechanism_valid else 'invalid'} | "
            f"{row.cpu_scope} |"
        )
    lines.extend(
        [
            "",
            "All ratios are medians of fresh-process native pairs. Spread is "
            "the retained maximum paired ratio divided by the retained "
            "minimum; no outlier is discarded.",
            "",
        ]
    )
    return "\n".join(lines)


def write_evidence(
    output_dir: Path,
    tracked_report: Path | None,
    samples: Sequence[SampleRow],
    summaries: Sequence[SummaryRow],
    *,
    phase: str,
    verdict: str,
    reasons: Sequence[str],
    metadata: dict[str, object],
) -> None:
    sample_dicts = [
        {"process_sample": sample.process_sample, **asdict(sample.row)}
        for sample in samples
    ]
    summary_dicts = [asdict(row) for row in summaries]
    report = _report_text(
        summaries,
        phase=phase,
        verdict=verdict,
        reasons=reasons,
    )
    complete_metadata = {
        **metadata,
        "phase": phase,
        "verdict": verdict,
        "reasons": list(reasons),
        "sample_rows": len(samples),
        "summary_rows": len(summaries),
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "python": sys.version.split()[0],
        "platform": platform.platform(),
    }
    payloads = {
        output_dir / "leir_phase0a_samples.csv": _csv_text(
            sample_dicts
        ),
        output_dir / "leir_phase0a_summary.csv": _csv_text(
            summary_dicts
        ),
        output_dir / "leir_phase0a_metadata.json": (
            json.dumps(complete_metadata, indent=2, sort_keys=True)
            + "\n"
        ),
        output_dir / "leir_phase0a_report.md": report,
    }
    for path, text in payloads.items():
        write_text_safely(path, text)
    if tracked_report is not None:
        write_text_safely(tracked_report, report)


def _positive_int(text: str) -> int:
    value = int(text, 10)
    if value <= 0:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run and classify LEIR Phase 0A native evidence."
    )
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument(
        "--phase",
        choices=("screen", "gate"),
        default="screen",
    )
    parser.add_argument("--samples", type=_positive_int)
    parser.add_argument(
        "--activations", type=_positive_int, default=128
    )
    parser.add_argument("--min-mode-ms", type=_positive_int)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--tracked-report", type=Path)
    return parser


def _source_commit() -> str:
    try:
        result = run_capture(
            ["git", "rev-parse", "HEAD"], timeout=5.0
        )
    except (OSError, ProcessTimeoutError):
        return "unavailable"
    return (
        result.stdout.strip()
        if result.returncode == 0 and not result.stderr
        else "unavailable"
    )


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if not args.binary.is_file():
        print(
            "[bench_leir_phase0.py] binary does not exist",
            file=sys.stderr,
        )
        return 2
    if args.phase == "screen":
        cells = screen_matrix()
        samples = args.samples or SCREEN_SAMPLES
        min_mode_ms = args.min_mode_ms or (
            SCREEN_MIN_MODE_NS // 1_000_000
        )
    else:
        cells = gate_matrix()
        samples = args.samples or GATE_SAMPLES
        min_mode_ms = args.min_mode_ms or (
            GATE_MIN_MODE_NS // 1_000_000
        )
    if samples % 2 == 0:
        print(
            "[bench_leir_phase0.py] samples must be odd",
            file=sys.stderr,
        )
        return 2

    try:
        raw = run_matrix(
            args.binary,
            cells,
            samples=samples,
            activations=args.activations,
            min_mode_ms=min_mode_ms,
        )
        summaries = summarize(raw)
        verdict, reasons = classify(
            summaries,
            phase=args.phase,
            expected_samples=samples,
            min_mode_ns=min_mode_ms * 1_000_000,
            expected_cells=cells,
        )
    except (ValueError, MatrixRunError) as exc:
        print(f"[bench_leir_phase0.py] {exc}", file=sys.stderr)
        return 2

    command_text = shlex.join(
        [sys.executable, str(Path(__file__)), *(argv or sys.argv[1:])]
    )
    write_evidence(
        args.output_dir,
        args.tracked_report,
        raw,
        summaries,
        phase=args.phase,
        verdict=verdict,
        reasons=reasons,
        metadata={
            "command": command_text,
            "binary": str(args.binary),
            "source_commit": _source_commit(),
            "matrix_cells": len(cells),
            "samples": samples,
            "activations": args.activations,
            "min_mode_ms": min_mode_ms,
            "cpu_gate": "server-only",
        },
    )
    print(
        f"[bench_leir_phase0.py] phase={args.phase} "
        f"verdict={verdict} cells={len(cells)} samples={samples}"
    )
    for reason in reasons:
        print(f"[bench_leir_phase0.py] {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
