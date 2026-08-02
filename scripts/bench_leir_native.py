#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run and classify Linux-native compiled LEIR segment evidence."""

from __future__ import annotations

import argparse
import csv
import io
import math
import platform
import re
import statistics
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Sequence

if __package__:
    from .evidence_bundle import (
        CLASSIFIER_SCHEMA,
        EVIDENCE_SCHEMA,
        VERDICT_SCHEMA,
        EvidenceBundle,
        EvidenceError,
        RecomputedArtifacts,
        audit_bundle,
        canonical_json_bytes,
        git_source_dirty_digest,
        git_source_provenance,
        normalize_architecture,
    )
    from .process_utils import (
        CapturedProcess,
        ProcessTimeoutError,
        run_capture,
    )
else:
    from evidence_bundle import (
        CLASSIFIER_SCHEMA,
        EVIDENCE_SCHEMA,
        VERDICT_SCHEMA,
        EvidenceBundle,
        EvidenceError,
        RecomputedArtifacts,
        audit_bundle,
        canonical_json_bytes,
        git_source_dirty_digest,
        git_source_provenance,
        normalize_architecture,
    )
    from process_utils import (
        CapturedProcess,
        ProcessTimeoutError,
        run_capture,
    )


RESULT_PREFIX = "LEIR_NATIVE_PAIR "
CANDIDATES = ("link", "link_skip")
OPS = (1, 2, 4, 8)
CONCURRENCY = (1, 64, 512)
PAYLOADS = (64, 1024, 16384)
CORE_OPS = (4, 8)
CORE_CONCURRENCY = (64, 512)
CORE_PAYLOADS = (64, 1024)
SCREEN_SAMPLES = 5
GATE_SAMPLES = 9
SCREEN_MIN_MODE_NS = 100_000_000
GATE_MIN_MODE_NS = 250_000_000
BLOCKS_PER_MODE = 8
MAX_OUTPUT_BYTES = 64 * 1024
UINT64_MAX = 0xFFFF_FFFF_FFFF_FFFF

DECIMAL_RE = re.compile(r"[0-9]+")
RATIO_RE = re.compile(r"[0-9]+\.[0-9]{9}")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")

FIELD_ORDER = (
    "version",
    "candidate",
    "ops",
    "concurrency",
    "payload",
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
    "logical_operations",
    "queue_publications",
    "prepared_sqes",
    "ring_submit_calls",
    "ring_submit_syscalls",
    "expected_cqes",
    "observed_cqes",
    "suppressed_success_cqes",
    "baseline_task_parks",
    "candidate_task_parks",
    "terminal_wakes",
    "resumes_avoided",
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
    "platform",
    "order",
)

RATIO_FIELDS = {"wall_speedup", "cpu_ratio"}
TEXT_FIELDS = {
    "version",
    "candidate",
    "baseline_checksum",
    "candidate_checksum",
    "peer",
    "cpu_scope",
    "platform",
    "order",
}
INTEGER_FIELDS = set(FIELD_ORDER) - RATIO_FIELDS - TEXT_FIELDS


@dataclass(frozen=True)
class MatrixCell:
    candidate: str
    ops: int
    concurrency: int
    payload: int


@dataclass(frozen=True)
class PairRow:
    candidate: str
    ops: int
    concurrency: int
    payload: int
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
    logical_operations: int
    queue_publications: int
    prepared_sqes: int
    ring_submit_calls: int
    ring_submit_syscalls: int
    expected_cqes: int
    observed_cqes: int
    suppressed_success_cqes: int
    baseline_task_parks: int
    candidate_task_parks: int
    terminal_wakes: int
    resumes_avoided: int
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
    platform: str
    order: str

    @property
    def cell(self) -> MatrixCell:
        return MatrixCell(
            self.candidate,
            self.ops,
            self.concurrency,
            self.payload,
        )


@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: PairRow


@dataclass(frozen=True)
class SummaryRow:
    candidate: str
    ops: int
    concurrency: int
    payload: int
    sample_count: int
    min_mode_ns: int
    wall_speedup: float
    cpu_ratio: float
    wall_ratio_spread: float
    cpu_ratio_spread: float
    service_gap_p99_ratio: float
    terminal_p99_ratio: float
    logical_operations: float
    queue_publications: float
    prepared_sqes: float
    ring_submit_calls: float
    ring_submit_syscalls: float
    observed_cqes: float
    suppressed_success_cqes: float
    candidate_task_parks: float
    terminal_wakes: float
    resumes_avoided: float
    path_valid: bool
    checksum_valid: bool
    allocation_valid: bool
    mechanism_valid: bool
    peer: str
    cpu_scope: str
    platform: str

    @property
    def cell(self) -> MatrixCell:
        return MatrixCell(
            self.candidate,
            self.ops,
            self.concurrency,
            self.payload,
        )


class MatrixRunError(RuntimeError):
    def __init__(
        self,
        message: str,
        rows: Sequence[SampleRow] = (),
    ) -> None:
        super().__init__(message)
        self.rows = list(rows)


class NativeUnavailable(MatrixRunError):
    """The benchmark binary cannot execute the Linux-native experiment."""


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (
        cell.candidate,
        cell.ops,
        cell.concurrency,
        cell.payload,
    )


def _split_output(text: str) -> dict[str, str]:
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError(
            "expected exactly one LEIR_NATIVE_PAIR result row"
        )
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty LEIR_NATIVE_PAIR result row")

    fields: dict[str, str] = {}
    observed_order: list[str] = []
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed LEIR_NATIVE_PAIR field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(
                f"duplicate or empty LEIR_NATIVE_PAIR field: {key}"
            )
        fields[key] = value
        observed_order.append(key)
    if tuple(observed_order) != FIELD_ORDER:
        raise ValueError(
            "LEIR_NATIVE_PAIR field order/schema mismatch: "
            f"observed={observed_order}"
        )
    return fields


def _parse_integer(name: str, text: str) -> int:
    if DECIMAL_RE.fullmatch(text) is None:
        raise ValueError(f"invalid decimal integer field {name}")
    value = int(text, 10)
    if value > UINT64_MAX:
        raise ValueError(
            f"integer field out of uint64 range: {name}"
        )
    return value


def _parse_ratio(name: str, text: str) -> float:
    if RATIO_RE.fullmatch(text) is None:
        raise ValueError(f"invalid fixed-point ratio field {name}")
    value = float(text)
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError(
            f"non-positive or non-finite ratio field {name}"
        )
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


def _mechanism_valid(row: PairRow) -> bool:
    logical = row.activations * row.ops
    expected_cqes = (
        logical if row.candidate == "link" else row.activations
    )
    return (
        logical <= UINT64_MAX
        and row.blocks_per_mode == BLOCKS_PER_MODE
        and row.logical_operations == logical
        and row.queue_publications == row.activations
        and row.prepared_sqes == logical
        and row.ring_submit_calls > 0
        and 0 < row.ring_submit_syscalls <= row.ring_submit_calls
        and row.expected_cqes == expected_cqes
        and row.observed_cqes == expected_cqes
        and row.suppressed_success_cqes
        == logical - expected_cqes
        and row.candidate_task_parks == row.activations
        and row.terminal_wakes == row.activations
        and row.resumes_avoided == logical - row.activations
        and row.hot_allocations == 0
        and row.pending_path_valid == 1
    )


def parse_output(text: str) -> PairRow:
    fields = _split_output(text)
    if fields["version"] != "1":
        raise ValueError(
            "unsupported LEIR_NATIVE_PAIR version"
        )
    integers = {
        name: _parse_integer(name, fields[name])
        for name in INTEGER_FIELDS
    }
    ratios = {
        name: _parse_ratio(name, fields[name])
        for name in RATIO_FIELDS
    }

    candidate = fields["candidate"]
    if candidate not in CANDIDATES:
        raise ValueError("unknown native candidate")
    if integers["ops"] not in OPS:
        raise ValueError("unsupported native operation count")
    if not 1 <= integers["concurrency"] <= 512:
        raise ValueError("concurrency out of range")
    if not 64 <= integers["payload"] <= 16384:
        raise ValueError("payload out of range")
    if integers["activations"] <= 0:
        raise ValueError("activations must be positive")
    if not 1_000_000 <= integers["min_mode_ns"] <= 60_000_000_000:
        raise ValueError("minimum mode duration out of range")
    if integers["blocks_per_mode"] != BLOCKS_PER_MODE:
        raise ValueError("unexpected balanced block count")

    positive_fields = (
        "baseline_wall_ns",
        "candidate_wall_ns",
        "baseline_cpu_ns",
        "candidate_cpu_ns",
        "baseline_service_gap_p99_ns",
        "candidate_service_gap_p99_ns",
        "baseline_terminal_p99_ns",
        "candidate_terminal_p99_ns",
    )
    if any(integers[name] <= 0 for name in positive_fields):
        raise ValueError(
            "zero-valued required native measurement"
        )
    if (
        integers["baseline_wall_ns"]
        < integers["min_mode_ns"]
        or integers["candidate_wall_ns"]
        < integers["min_mode_ns"]
    ):
        raise ValueError(
            "measured wall duration is below the declared minimum"
        )
    if not _ratio_matches(
        ratios["wall_speedup"],
        integers["baseline_wall_ns"]
        / integers["candidate_wall_ns"],
    ):
        raise ValueError(
            "wall speedup disagrees with native clocks"
        )
    if not _ratio_matches(
        ratios["cpu_ratio"],
        integers["candidate_cpu_ns"]
        / integers["baseline_cpu_ns"],
    ):
        raise ValueError(
            "CPU ratio disagrees with native clocks"
        )

    for name in ("baseline_checksum", "candidate_checksum"):
        if CHECKSUM_RE.fullmatch(fields[name]) is None:
            raise ValueError(f"invalid checksum field {name}")
    if fields["baseline_checksum"] != fields["candidate_checksum"]:
        raise ValueError(
            "baseline/candidate checksum mismatch"
        )
    if fields["peer"] != "process":
        raise ValueError(
            "native evidence requires an external process peer"
        )
    if fields["cpu_scope"] != "server":
        raise ValueError(
            "native evidence requires server-only CPU time"
        )
    if fields["platform"] != "linux_io_uring":
        raise ValueError(
            "native evidence has the wrong platform boundary"
        )
    if fields["order"] not in {"ABBA", "BAAB"}:
        raise ValueError("invalid balanced block order")

    row = PairRow(
        candidate=candidate,
        ops=integers["ops"],
        concurrency=integers["concurrency"],
        payload=integers["payload"],
        activations=integers["activations"],
        min_mode_ns=integers["min_mode_ns"],
        blocks_per_mode=integers["blocks_per_mode"],
        baseline_wall_ns=integers["baseline_wall_ns"],
        candidate_wall_ns=integers["candidate_wall_ns"],
        baseline_cpu_ns=integers["baseline_cpu_ns"],
        candidate_cpu_ns=integers["candidate_cpu_ns"],
        wall_speedup=ratios["wall_speedup"],
        cpu_ratio=ratios["cpu_ratio"],
        baseline_ctx_switches=integers[
            "baseline_ctx_switches"
        ],
        candidate_ctx_switches=integers[
            "candidate_ctx_switches"
        ],
        logical_operations=integers["logical_operations"],
        queue_publications=integers["queue_publications"],
        prepared_sqes=integers["prepared_sqes"],
        ring_submit_calls=integers["ring_submit_calls"],
        ring_submit_syscalls=integers["ring_submit_syscalls"],
        expected_cqes=integers["expected_cqes"],
        observed_cqes=integers["observed_cqes"],
        suppressed_success_cqes=integers[
            "suppressed_success_cqes"
        ],
        baseline_task_parks=integers["baseline_task_parks"],
        candidate_task_parks=integers["candidate_task_parks"],
        terminal_wakes=integers["terminal_wakes"],
        resumes_avoided=integers["resumes_avoided"],
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
        platform=fields["platform"],
        order=fields["order"],
    )
    if not _mechanism_valid(row):
        raise ValueError(
            "native queue/SQE/CQE/park/wake counters are inconsistent"
        )
    return row


def screen_matrix() -> list[MatrixCell]:
    return [
        MatrixCell(candidate, ops, concurrency, payload)
        for candidate in CANDIDATES
        for ops in OPS
        for concurrency in CONCURRENCY
        for payload in PAYLOADS
    ]


def benchmark_command(
    binary: Path,
    cell: MatrixCell,
    *,
    activations: int,
    min_mode_ms: int,
    order: str,
) -> list[str]:
    if (
        cell.candidate not in CANDIDATES
        or cell.ops not in OPS
        or not 1 <= cell.concurrency <= 512
        or not 64 <= cell.payload <= 16384
    ):
        raise ValueError("invalid native benchmark cell")
    if activations <= 0 or min_mode_ms <= 0:
        raise ValueError(
            "benchmark counts must be positive"
        )
    if order not in {"ABBA", "BAAB"}:
        raise ValueError("invalid benchmark order")
    return [
        str(binary.resolve()),
        "--candidate",
        cell.candidate,
        "--ops",
        str(cell.ops),
        "--concurrency",
        str(cell.concurrency),
        "--payload",
        str(cell.payload),
        "--activations",
        str(activations),
        "--min-mode-ms",
        str(min_mode_ms),
        "--order",
        order,
    ]


Runner = Callable[..., CapturedProcess]


def _output_too_large(text: str) -> bool:
    return len(text.encode("utf-8", errors="replace")) > MAX_OUTPUT_BYTES


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
        raise ValueError(
            "process sample index must be positive"
        )
    order = "ABBA" if process_sample % 2 else "BAAB"
    command = benchmark_command(
        binary,
        cell,
        activations=activations,
        min_mode_ms=min_mode_ms,
        order=order,
    )
    effective_timeout = (
        max(120.0, min_mode_ms / 1000.0 * 24.0)
        if timeout is None
        else timeout
    )
    try:
        result = runner(
            command,
            timeout=effective_timeout,
            max_output_bytes=MAX_OUTPUT_BYTES,
        )
    except (OSError, ProcessTimeoutError) as exc:
        raise MatrixRunError(
            f"sample {process_sample} process failure: {exc}"
        ) from exc
    if result.stdout_truncated or result.stderr_truncated:
        raise MatrixRunError(
            f"sample {process_sample} exceeded the output cap"
        )
    if _output_too_large(result.stdout) or _output_too_large(
        result.stderr
    ):
        raise MatrixRunError(
            f"sample {process_sample} exceeded the output cap"
        )
    if result.returncode == 77:
        expected = (
            f"LEIR_NATIVE_SKIP candidate={cell.candidate} "
            "reason=backend_unavailable\n"
        )
        if result.stdout or result.stderr != expected:
            raise MatrixRunError(
                f"sample {process_sample} malformed native skip"
            )
        raise NativeUnavailable(
            f"native backend unavailable for {_cell_key(cell)}"
        )
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
        row = parse_output(result.stdout)
    except ValueError as exc:
        raise MatrixRunError(
            f"sample {process_sample} result contract: {exc}"
        ) from exc
    minimum_activations = activations * BLOCKS_PER_MODE
    if (
        _cell_key(row.cell) != _cell_key(cell)
        or row.order != order
        or row.min_mode_ns != min_mode_ms * 1_000_000
        or row.activations < minimum_activations
    ):
        raise MatrixRunError(
            f"sample {process_sample} native row disagrees "
            "with the command"
        )
    return SampleRow(
        process_sample=process_sample,
        row=row,
    )


def run_matrix(
    binary: Path,
    cells: Sequence[MatrixCell],
    *,
    samples: int,
    activations: int,
    min_mode_ms: int,
) -> list[SampleRow]:
    if samples <= 0 or samples % 2 == 0:
        raise ValueError(
            "an odd positive sample count is required"
        )
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
            except NativeUnavailable as exc:
                raise NativeUnavailable(
                    f"cell {cell_index}/{len(cells)}: {exc}",
                    rows,
                ) from exc
            except MatrixRunError as exc:
                raise MatrixRunError(
                    f"cell {cell_index}/{len(cells)} "
                    f"{_cell_key(cell)}: {exc}",
                    rows,
                ) from exc
            rows.append(sample)
            print(
                f"[leir-native] sample {len(rows)}/{total} "
                f"cell={_cell_key(cell)} order={sample.row.order}",
                file=sys.stderr,
                flush=True,
            )
    return rows


def _median(values: Sequence[int | float]) -> float:
    return float(statistics.median(values))


def summarize(
    samples: Sequence[SampleRow],
) -> list[SummaryRow]:
    grouped: dict[tuple[object, ...], list[SampleRow]] = {}
    for sample in samples:
        grouped.setdefault(
            _cell_key(sample.row.cell), []
        ).append(sample)

    summaries: list[SummaryRow] = []
    for key in sorted(grouped):
        group = sorted(
            grouped[key],
            key=lambda sample: sample.process_sample,
        )
        if len(
            {sample.process_sample for sample in group}
        ) != len(group):
            raise ValueError(
                f"duplicate process sample index for {key}"
            )
        rows = [sample.row for sample in group]
        wall = [row.wall_speedup for row in rows]
        cpu = [row.cpu_ratio for row in rows]
        service = [
            row.candidate_service_gap_p99_ns
            / row.baseline_service_gap_p99_ns
            for row in rows
        ]
        terminal = [
            row.candidate_terminal_p99_ns
            / row.baseline_terminal_p99_ns
            for row in rows
        ]
        peers = {row.peer for row in rows}
        scopes = {row.cpu_scope for row in rows}
        platforms = {row.platform for row in rows}
        first = rows[0]
        summaries.append(
            SummaryRow(
                candidate=first.candidate,
                ops=first.ops,
                concurrency=first.concurrency,
                payload=first.payload,
                sample_count=len(rows),
                min_mode_ns=min(
                    row.min_mode_ns for row in rows
                ),
                wall_speedup=_median(wall),
                cpu_ratio=_median(cpu),
                wall_ratio_spread=max(wall) / min(wall),
                cpu_ratio_spread=max(cpu) / min(cpu),
                service_gap_p99_ratio=_median(service),
                terminal_p99_ratio=_median(terminal),
                logical_operations=_median(
                    [row.logical_operations for row in rows]
                ),
                queue_publications=_median(
                    [row.queue_publications for row in rows]
                ),
                prepared_sqes=_median(
                    [row.prepared_sqes for row in rows]
                ),
                ring_submit_calls=_median(
                    [row.ring_submit_calls for row in rows]
                ),
                ring_submit_syscalls=_median(
                    [row.ring_submit_syscalls for row in rows]
                ),
                observed_cqes=_median(
                    [row.observed_cqes for row in rows]
                ),
                suppressed_success_cqes=_median(
                    [
                        row.suppressed_success_cqes
                        for row in rows
                    ]
                ),
                candidate_task_parks=_median(
                    [row.candidate_task_parks for row in rows]
                ),
                terminal_wakes=_median(
                    [row.terminal_wakes for row in rows]
                ),
                resumes_avoided=_median(
                    [row.resumes_avoided for row in rows]
                ),
                path_valid=all(
                    row.pending_path_valid == 1
                    for row in rows
                ),
                checksum_valid=all(
                    row.baseline_checksum
                    == row.candidate_checksum
                    for row in rows
                ),
                allocation_valid=all(
                    row.hot_allocations == 0
                    for row in rows
                ),
                mechanism_valid=all(
                    _mechanism_valid(row) for row in rows
                ),
                peer=(
                    next(iter(peers))
                    if len(peers) == 1
                    else "mixed"
                ),
                cpu_scope=(
                    next(iter(scopes))
                    if len(scopes) == 1
                    else "mixed"
                ),
                platform=(
                    next(iter(platforms))
                    if len(platforms) == 1
                    else "mixed"
                ),
            )
        )
    return summaries


def _is_core(row: SummaryRow) -> bool:
    return (
        row.candidate == "link_skip"
        and row.ops in CORE_OPS
        and row.concurrency in CORE_CONCURRENCY
        and row.payload in CORE_PAYLOADS
    )


def _is_control(row: SummaryRow) -> bool:
    return row.ops == 1


def classify(
    summaries: Sequence[SummaryRow],
    *,
    expected_samples: int,
    min_mode_ns: int,
    expected_cells: Sequence[MatrixCell] | None = None,
) -> tuple[str, list[str]]:
    cells = (
        list(expected_cells)
        if expected_cells is not None
        else screen_matrix()
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

    integrity: list[str] = []
    if duplicate_keys:
        integrity.append(
            f"duplicate summary cells: {sorted(duplicate_keys)}"
        )
    if actual != expected:
        integrity.append(
            "matrix coverage mismatch: "
            f"missing={sorted(expected - actual)} "
            f"extra={sorted(actual - expected)}"
        )
    for key in sorted(actual & expected):
        row = by_key[key]
        if row.sample_count != expected_samples:
            integrity.append(
                f"sample count {row.sample_count}/"
                f"{expected_samples} for {key}"
            )
        if row.min_mode_ns < min_mode_ns:
            integrity.append(
                f"minimum duration below {min_mode_ns} ns "
                f"for {key}"
            )
        if not (
            row.path_valid
            and row.checksum_valid
            and row.allocation_valid
            and row.mechanism_valid
        ):
            integrity.append(
                "mechanism/correctness control failed "
                f"for {key}"
            )
        if (
            row.peer != "process"
            or row.cpu_scope != "server"
            or row.platform != "linux_io_uring"
        ):
            integrity.append(
                f"platform/process/CPU scope invalid for {key}"
            )
        if row.wall_ratio_spread > 1.10:
            integrity.append(
                f"wall spread above 1.10x for {key}"
            )
        if row.cpu_ratio_spread > 1.10:
            integrity.append(
                f"CPU spread above 1.10x for {key}"
            )
    if integrity:
        return "INCONCLUSIVE", integrity

    core = [row for row in summaries if _is_core(row)]
    controls = [
        row for row in summaries if _is_control(row)
    ]
    if not core or not controls:
        return (
            "INCONCLUSIVE",
            ["no native core or length-1 control rows"],
        )

    misses: list[str] = []
    for row in core:
        key = _cell_key(row.cell)
        if row.wall_speedup < 1.50:
            misses.append(
                f"core wall speedup below 1.50x for {key}"
            )
        if row.cpu_ratio > 0.70:
            misses.append(
                f"core CPU ratio above 0.70x for {key}"
            )
        if row.terminal_p99_ratio > 1.10:
            misses.append(
                f"core terminal p99 above 1.10x for {key}"
            )
    for row in controls:
        key = _cell_key(row.cell)
        if row.wall_speedup < 0.95:
            misses.append(
                f"length-1 wall speedup below 0.95x for {key}"
            )
        if row.cpu_ratio > 1.05:
            misses.append(
                f"length-1 CPU ratio above 1.05x for {key}"
            )
        if row.service_gap_p99_ratio > 1.10:
            misses.append(
                f"length-1 service p99 above 1.10x for {key}"
            )
    if misses:
        return "REJECT", misses
    return (
        "SPECIALIZED",
        [
            "all Linux-native core, length-1 control, latency, "
            "correctness, allocation, and stability gates passed"
        ],
    )


RAW_FIELD_ORDER = (
    "process_sample",
    *PairRow.__dataclass_fields__.keys(),
)
SUMMARY_FIELD_ORDER = tuple(SummaryRow.__dataclass_fields__.keys())
CLASSIFIER_THRESHOLDS = {
    "core_wall_speedup_min": 1.50,
    "core_cpu_ratio_max": 0.70,
    "core_terminal_p99_ratio_max": 1.10,
    "control_wall_speedup_min": 0.95,
    "control_cpu_ratio_max": 1.05,
    "control_service_gap_p99_ratio_max": 1.10,
    "paired_ratio_spread_max": 1.10,
}


def _csv_text(
    rows: Sequence[dict[str, object]],
    fieldnames: Sequence[str],
) -> str:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(
        output,
        fieldnames=list(fieldnames),
        extrasaction="raise",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return output.getvalue()


def _raw_rows(
    samples: Sequence[SampleRow],
) -> list[dict[str, object]]:
    return [
        {
            "process_sample": sample.process_sample,
            **asdict(sample.row),
        }
        for sample in samples
    ]


def _pair_from_csv(fields: dict[str, str]) -> PairRow:
    values: dict[str, object] = {}
    text_fields = {
        "candidate",
        "baseline_checksum",
        "candidate_checksum",
        "peer",
        "cpu_scope",
        "platform",
        "order",
    }
    ratio_fields = {"wall_speedup", "cpu_ratio"}
    for name in PairRow.__dataclass_fields__:
        text = fields[name]
        if name in text_fields:
            values[name] = text
        elif name in ratio_fields:
            values[name] = _parse_ratio(name, f"{float(text):.9f}")
        else:
            values[name] = _parse_integer(name, text)
    candidate = PairRow(**values)  # type: ignore[arg-type]
    serialized: dict[str, str] = {"version": "1"}
    for name in FIELD_ORDER[1:]:
        value = getattr(candidate, name)
        serialized[name] = (
            f"{value:.9f}" if name in ratio_fields else str(value)
        )
    output = RESULT_PREFIX + " ".join(
        f"{name}={serialized[name]}" for name in FIELD_ORDER
    )
    return parse_output(output)


def _samples_from_raw_bytes(
    raw_csv: bytes,
    *,
    min_mode_ms: int,
) -> list[SampleRow]:
    try:
        text = raw_csv.decode("utf-8")
    except UnicodeError as exc:
        raise ValueError("raw.csv is not UTF-8") from exc
    reader = csv.DictReader(io.StringIO(text, newline=""))
    if tuple(reader.fieldnames or ()) != RAW_FIELD_ORDER:
        raise ValueError("raw.csv header/schema mismatch")
    samples: list[SampleRow] = []
    observed: set[tuple[object, ...]] = set()
    for line_number, fields in enumerate(reader, start=2):
        if (
            None in fields
            or any(value is None for value in fields.values())
        ):
            raise ValueError(
                f"malformed raw.csv row at line {line_number}"
            )
        process_sample = _parse_integer(
            "process_sample",
            fields["process_sample"] or "",
        )
        if process_sample <= 0:
            raise ValueError("process sample index must be positive")
        row = _pair_from_csv(
            {
                name: fields[name] or ""
                for name in PairRow.__dataclass_fields__
            }
        )
        if row.min_mode_ns != min_mode_ms * 1_000_000:
            raise ValueError(
                "raw sample minimum duration disagrees with metadata"
            )
        expected_order = "ABBA" if process_sample % 2 else "BAAB"
        if row.order != expected_order:
            raise ValueError(
                "raw sample order disagrees with sample index"
            )
        key = (process_sample, *_cell_key(row.cell))
        if key in observed:
            raise ValueError("duplicate raw sample key")
        observed.add(key)
        samples.append(SampleRow(process_sample, row))
    canonical = _csv_text(
        _raw_rows(samples),
        RAW_FIELD_ORDER,
    ).encode("utf-8")
    if canonical != raw_csv:
        raise ValueError("raw.csv is not canonical typed CSV")
    return samples


def _report_text(
    summaries: Sequence[SummaryRow],
    *,
    phase: str,
    verdict: str,
    reasons: Sequence[str],
) -> str:
    lines = [
        "# LEIR Linux Native Segment Evidence",
        "",
        f"- Phase: `{phase}`",
        f"- Verdict: **{verdict}**",
        (
            "- Scope: Linux `io_uring` specialization only. "
            "`SPECIALIZED` is not `CATEGORY`, a portable runtime "
            "claim, a version bump, or release authorization."
        ),
        "",
        "## Precommitted gates",
        "",
        "| Gate | Threshold |",
        "|---|---:|",
        "| Required 4/8-op `link_skip` wall speedup | >= 1.50x |",
        "| Required 4/8-op `link_skip` CPU ratio | <= 0.70x |",
        "| Required terminal p99 ratio | <= 1.10x |",
        "| Length-1 wall speedup | >= 0.95x |",
        "| Length-1 CPU ratio | <= 1.05x |",
        "| Length-1 service-gap p99 ratio | <= 1.10x |",
        "| Paired wall/CPU spread | <= 1.10x |",
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
                "| candidate | ops | concurrency | payload | samples | "
                "wall | CPU | wall spread | CPU spread | service p99 | "
                "terminal p99 | mechanism |"
            ),
            (
                "|---|---:|---:|---:|---:|---:|---:|---:|---:|"
                "---:|---:|---|"
            ),
        ]
    )
    for row in summaries:
        lines.append(
            f"| {row.candidate} | {row.ops} | {row.concurrency} | "
            f"{row.payload} | {row.sample_count} | "
            f"{row.wall_speedup:.6f}x | {row.cpu_ratio:.6f}x | "
            f"{row.wall_ratio_spread:.6f}x | "
            f"{row.cpu_ratio_spread:.6f}x | "
            f"{row.service_gap_p99_ratio:.6f}x | "
            f"{row.terminal_p99_ratio:.6f}x | "
            f"{'valid' if row.mechanism_valid else 'invalid'} |"
        )
    lines.extend(
        [
            "",
            "Ratios are medians of fresh-process paired samples. "
            "Spread is maximum divided by minimum with no discarded "
            "outliers.",
            "",
        ]
    )
    return "\n".join(lines)


def _verdict_bytes(verdict: str, reasons: Sequence[str]) -> bytes:
    return canonical_json_bytes(
        {
            "schema": VERDICT_SCHEMA,
            "verdict": verdict,
            "reasons": list(reasons),
        }
    )


def _bundle_metadata(
    phase: str,
    metadata: dict[str, object],
) -> dict[str, object]:
    required = {
        "source_commit",
        "source_dirty_digest",
        "architecture",
        "kernel",
        "toolchain",
        "commands",
        "cpu_policy",
        "samples",
        "activations",
        "min_mode_ms",
        "unavailable_reason",
    }
    if set(metadata) != required:
        raise ValueError(
            "native evidence metadata fields mismatch: "
            f"missing={sorted(required - set(metadata))} "
            f"unknown={sorted(set(metadata) - required)}"
        )
    return {
        "schema": EVIDENCE_SCHEMA,
        "source_commit": metadata["source_commit"],
        "source_dirty_digest": metadata["source_dirty_digest"],
        "architecture": metadata["architecture"],
        "kernel": metadata["kernel"],
        "toolchain": metadata["toolchain"],
        "commands": metadata["commands"],
        "cpu_policy": metadata["cpu_policy"],
        "matrix": {
            "benchmark": "leir_native_segment",
            "phase": phase,
            "cells": [asdict(cell) for cell in screen_matrix()],
        },
        "sample_schedule": {
            "samples": metadata["samples"],
            "activations": metadata["activations"],
            "min_mode_ms": metadata["min_mode_ms"],
            "orders": ["ABBA", "BAAB"],
            "unavailable_reason": metadata["unavailable_reason"],
        },
        "classifier": {
            "schema": CLASSIFIER_SCHEMA,
            "thresholds": CLASSIFIER_THRESHOLDS,
        },
    }


def _recompute_artifacts(
    raw_csv: bytes,
    metadata: dict[str, object],
) -> RecomputedArtifacts:
    matrix = metadata["matrix"]
    schedule = metadata["sample_schedule"]
    classifier = metadata["classifier"]
    if (
        not isinstance(matrix, dict)
        or matrix
        != {
            "benchmark": "leir_native_segment",
            "phase": matrix.get("phase"),
            "cells": [asdict(cell) for cell in screen_matrix()],
        }
        or matrix.get("phase") not in {"screen", "gate"}
    ):
        raise ValueError("native matrix metadata mismatch")
    if not isinstance(schedule, dict) or set(schedule) != {
        "samples",
        "activations",
        "min_mode_ms",
        "orders",
        "unavailable_reason",
    }:
        raise ValueError("native sample schedule metadata mismatch")
    samples_count = schedule["samples"]
    activations = schedule["activations"]
    min_mode_ms = schedule["min_mode_ms"]
    if (
        not isinstance(samples_count, int)
        or isinstance(samples_count, bool)
        or samples_count <= 0
        or samples_count % 2 == 0
        or not isinstance(activations, int)
        or isinstance(activations, bool)
        or activations <= 0
        or not isinstance(min_mode_ms, int)
        or isinstance(min_mode_ms, bool)
        or min_mode_ms <= 0
        or schedule["orders"] != ["ABBA", "BAAB"]
        or (
            schedule["unavailable_reason"] is not None
            and (
                not isinstance(schedule["unavailable_reason"], str)
                or not schedule["unavailable_reason"]
            )
        )
    ):
        raise ValueError("invalid native sample schedule")
    if classifier != {
        "schema": CLASSIFIER_SCHEMA,
        "thresholds": CLASSIFIER_THRESHOLDS,
    }:
        raise ValueError("native classifier metadata mismatch")
    samples = _samples_from_raw_bytes(
        raw_csv,
        min_mode_ms=min_mode_ms,
    )
    cells = screen_matrix()
    unavailable_reason = schedule["unavailable_reason"]
    if unavailable_reason is None:
        measured_cells = cells
    else:
        reasons_by_index = {
            (
                f"cell {index}/{len(cells)}: "
                "native backend unavailable for "
                f"{_cell_key(cell)}"
            ): index - 1
            for index, cell in enumerate(cells, start=1)
        }
        missing_index = reasons_by_index.get(unavailable_reason)
        if missing_index is None:
            raise ValueError(
                "native unavailable reason does not name an exact cell"
            )
        measured_cells = cells[:missing_index]
    expected_sequence = [
        (process_sample, *_cell_key(cell))
        for cell in measured_cells
        for process_sample in range(1, samples_count + 1)
    ]
    observed_sequence = [
        (sample.process_sample, *_cell_key(sample.row.cell))
        for sample in samples
    ]
    if observed_sequence != expected_sequence:
        raise ValueError(
            "raw samples are not the exact measured matrix prefix"
        )
    for sample in samples:
        minimum_activations = (
            max(activations, sample.row.concurrency)
            * sample.row.blocks_per_mode
        )
        if (
            sample.row.blocks_per_mode != BLOCKS_PER_MODE
            or sample.row.activations < minimum_activations
            or sample.row.activations % BLOCKS_PER_MODE != 0
        ):
            raise ValueError(
                "raw sample activation count disagrees with schedule"
            )
    summaries = summarize(samples)
    if unavailable_reason is None:
        verdict, reasons = classify(
            summaries,
            expected_samples=samples_count,
            min_mode_ns=min_mode_ms * 1_000_000,
            expected_cells=screen_matrix(),
        )
    else:
        verdict = "INCONCLUSIVE"
        reasons = [unavailable_reason]
    summary_csv = _csv_text(
        [asdict(row) for row in summaries],
        SUMMARY_FIELD_ORDER,
    ).encode("utf-8")
    return RecomputedArtifacts(
        summary_csv=summary_csv,
        verdict_json=_verdict_bytes(verdict, reasons),
        report_md=_report_text(
            summaries,
            phase=matrix["phase"],  # type: ignore[arg-type]
            verdict=verdict,
            reasons=reasons,
        ).encode("utf-8"),
    )


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
    raw_csv = _csv_text(
        _raw_rows(samples),
        RAW_FIELD_ORDER,
    ).encode("utf-8")
    summary_csv = _csv_text(
        [asdict(row) for row in summaries],
        SUMMARY_FIELD_ORDER,
    ).encode("utf-8")
    verdict_json = _verdict_bytes(verdict, reasons)
    report = _report_text(
        summaries,
        phase=phase,
        verdict=verdict,
        reasons=reasons,
    ).encode("utf-8")
    complete_metadata = _bundle_metadata(phase, metadata)
    recomputed = _recompute_artifacts(raw_csv, complete_metadata)
    if (
        recomputed.summary_csv != summary_csv
        or recomputed.verdict_json != verdict_json
        or recomputed.report_md != report
    ):
        raise ValueError(
            "native evidence inputs do not match deterministic recomputation"
        )
    bundle = EvidenceBundle.create(output_dir, complete_metadata)
    bundle.write_bytes("raw.csv", raw_csv)
    bundle.write_bytes("summary.csv", summary_csv)
    bundle.write_bytes("verdict.json", verdict_json)
    bundle.write_bytes("report.md", report)
    bundle.finalize()


def audit_existing(
    directory: Path,
    *,
    required_source_commit: str | None = None,
    required_source_dirty_digest: str | None = None,
) -> tuple[str, list[str]]:
    result = audit_bundle(
        directory,
        recompute=_recompute_artifacts,
        required_source_commit=required_source_commit,
        required_source_dirty_digest=required_source_dirty_digest,
    )
    return result.verdict, list(result.reasons)


def _positive_int(text: str) -> int:
    value = int(text, 10)
    if value <= 0:
        raise argparse.ArgumentTypeError(
            "expected a positive integer"
        )
    return value


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Run and classify Linux-native compiled LEIR segments."
        )
    )
    parser.add_argument(
        "--binary",
        type=Path,
    )
    parser.add_argument(
        "--phase",
        choices=("screen", "gate"),
        default="screen",
    )
    parser.add_argument(
        "--samples",
        type=_positive_int,
    )
    parser.add_argument(
        "--activations",
        type=_positive_int,
        default=128,
    )
    parser.add_argument(
        "--min-mode-ms",
        type=_positive_int,
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
    )
    parser.add_argument(
        "--tracked-report",
        type=Path,
    )
    parser.add_argument("--audit-existing", type=Path)
    parser.add_argument("--source-commit")
    parser.add_argument("--source-dirty-digest")
    parser.add_argument("--require-source-commit")
    parser.add_argument("--require-source-dirty-digest")
    return parser


def _source_dirty_digest(*, cwd: Path | None = None) -> str:
    return git_source_dirty_digest(run_capture, cwd=cwd)


def main(argv: Sequence[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if args.audit_existing is not None:
        if (
            args.binary is not None
            or args.output_dir is not None
            or args.source_commit is not None
            or args.source_dirty_digest is not None
        ):
            print(
                "[bench_leir_native.py] audit mode cannot also run "
                "a binary",
                file=sys.stderr,
            )
            return 2
        try:
            verdict, reasons = audit_existing(
                args.audit_existing,
                required_source_commit=args.require_source_commit,
                required_source_dirty_digest=(
                    args.require_source_dirty_digest
                ),
            )
        except (EvidenceError, OSError, ValueError) as exc:
            print(
                f"[bench_leir_native.py] audit failed: {exc}",
                file=sys.stderr,
            )
            return 2
        print(
            f"[bench_leir_native.py] audit=OK verdict={verdict}"
        )
        for reason in reasons:
            print(f"[bench_leir_native.py] {reason}")
        return 0
    if (
        args.binary is None
        or args.output_dir is None
        or args.require_source_commit is not None
        or args.require_source_dirty_digest is not None
    ):
        print(
            "[bench_leir_native.py] --binary and --output-dir are "
            "required in run mode; require-source flags are audit-only",
            file=sys.stderr,
        )
        return 2
    if not args.binary.is_file():
        print(
            "[bench_leir_native.py] binary does not exist",
            file=sys.stderr,
        )
        return 2
    if args.phase == "screen":
        samples = args.samples or SCREEN_SAMPLES
        min_mode_ms = args.min_mode_ms or (
            SCREEN_MIN_MODE_NS // 1_000_000
        )
    else:
        samples = args.samples or GATE_SAMPLES
        min_mode_ms = args.min_mode_ms or (
            GATE_MIN_MODE_NS // 1_000_000
        )
    if samples % 2 == 0:
        print(
            "[bench_leir_native.py] samples must be odd",
            file=sys.stderr,
        )
        return 2
    provenance_override = (
        args.source_commit is not None,
        args.source_dirty_digest is not None,
    )
    if provenance_override[0] != provenance_override[1]:
        print(
            "[bench_leir_native.py] --source-commit and "
            "--source-dirty-digest must be supplied together",
            file=sys.stderr,
        )
        return 2
    automatic_provenance = not provenance_override[0]
    if automatic_provenance:
        try:
            source_provenance = git_source_provenance(run_capture)
        except (EvidenceError, OSError, RuntimeError, ValueError) as exc:
            print(
                "[bench_leir_native.py] source provenance capture "
                f"failed: {exc}",
                file=sys.stderr,
            )
            return 2
    else:
        source_provenance = (
            args.source_commit,
            args.source_dirty_digest,
        )

    cells = screen_matrix()
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
            expected_samples=samples,
            min_mode_ns=min_mode_ms * 1_000_000,
            expected_cells=cells,
        )
    except NativeUnavailable as exc:
        raw = exc.rows
        summaries = summarize(raw)
        verdict = "INCONCLUSIVE"
        reasons = [str(exc)]
    except (ValueError, MatrixRunError) as exc:
        print(
            f"[bench_leir_native.py] {exc}",
            file=sys.stderr,
        )
        return 2
    if automatic_provenance:
        try:
            source_provenance_after = git_source_provenance(run_capture)
        except (EvidenceError, OSError, RuntimeError, ValueError) as exc:
            print(
                "[bench_leir_native.py] source provenance capture "
                f"failed after measurement: {exc}",
                file=sys.stderr,
            )
            return 2
        if source_provenance_after != source_provenance:
            print(
                "[bench_leir_native.py] source provenance changed "
                "during measurement",
                file=sys.stderr,
            )
            return 2

    invocation = [
        sys.executable,
        str(Path(__file__)),
        *(argv or sys.argv[1:]),
    ]
    source_commit, dirty_digest = source_provenance
    unavailable_reason = (
        reasons[0] if verdict == "INCONCLUSIVE" and len(reasons) == 1
        and reasons[0].startswith("cell ")
        else None
    )
    try:
        write_evidence(
            args.output_dir,
            args.tracked_report,
            raw,
            summaries,
            phase=args.phase,
            verdict=verdict,
            reasons=reasons,
            metadata={
                "source_commit": source_commit,
                "source_dirty_digest": dirty_digest,
                "architecture": normalize_architecture(
                    platform.machine()
                ),
                "kernel": platform.release(),
                "toolchain": (
                    platform.python_compiler()
                    or "unknown-python-compiler"
                ),
                "commands": [invocation],
                "cpu_policy": {
                    "scope": "server",
                    "affinity": "caller-controlled",
                },
                "samples": samples,
                "activations": args.activations,
                "min_mode_ms": min_mode_ms,
                "unavailable_reason": unavailable_reason,
            },
        )
    except (EvidenceError, OSError, RuntimeError, ValueError) as exc:
        print(
            f"[bench_leir_native.py] evidence write failed: {exc}",
            file=sys.stderr,
        )
        return 2
    if args.tracked_report is not None:
        print(
            "[bench_leir_native.py] --tracked-report is deprecated; "
            "use output bundle report.md",
            file=sys.stderr,
        )
    print(
        f"[bench_leir_native.py] phase={args.phase} "
        f"verdict={verdict} cells={len(cells)} samples={samples}"
    )
    for reason in reasons:
        print(f"[bench_leir_native.py] {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
