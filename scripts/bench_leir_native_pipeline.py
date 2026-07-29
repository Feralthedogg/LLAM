#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run, classify, and audit connected Linux io_uring LEIR evidence."""

from __future__ import annotations

import argparse
import csv
import hashlib
import io
import math
import platform
import random
import re
import statistics
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Callable, Sequence

try:
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
        normalize_architecture,
    )
    from process_utils import (
        CapturedProcess,
        ProcessTimeoutError,
        run_capture,
    )
except ModuleNotFoundError:
    from scripts.evidence_bundle import (
        CLASSIFIER_SCHEMA,
        EVIDENCE_SCHEMA,
        VERDICT_SCHEMA,
        EvidenceBundle,
        EvidenceError,
        RecomputedArtifacts,
        audit_bundle,
        canonical_json_bytes,
        git_source_dirty_digest,
        normalize_architecture,
    )
    from scripts.process_utils import (
        CapturedProcess,
        ProcessTimeoutError,
        run_capture,
    )


RESULT_PREFIX = "RESULT "
ALL_CANDIDATES = ("link", "link_skip", "fixed_link_skip")
CANDIDATES = ("link_skip", "fixed_link_skip")
WIDTHS = (1, 2, 4, 8)
CONCURRENCY = (1, 4, 16)
PAYLOADS = (64, 512, 4096)
SAMPLES = 9
DEFAULT_ACTIVATIONS = 32
DEFAULT_MIN_MODE_MS = 20
BLOCKS_PER_MODE = 8
TIMEOUT_SECONDS = 120.0
MAX_OUTPUT_BYTES = 64 * 1024
BOOTSTRAP_RESAMPLES = 10_000
UINT64_MAX = 0xFFFF_FFFF_FFFF_FFFF
DECIMAL_RE = re.compile(r"[0-9]+")

FIELD_ORDER = (
    "baseline_wall_ns",
    "candidate_wall_ns",
    "baseline_cpu_ns",
    "candidate_cpu_ns",
    "baseline_p99_ns",
    "candidate_p99_ns",
    "activations",
    "segments",
    "logical_ops",
    "batch_width",
    "queue_publications",
    "task_parks",
    "terminal_wakes",
    "operation_sqes",
    "operation_cqes",
    "suppressed_success_cqes",
    "cancel_sqes",
    "cancel_cqes",
    "fixed_file_attachments",
    "fixed_buffer_attachments",
    "submit_calls",
    "submit_syscalls",
    "checksum",
    "status",
)
INTEGER_FIELDS = FIELD_ORDER[:-1]


@dataclass(frozen=True)
class MatrixCell:
    candidate: str
    batch_width: int
    concurrency: int
    payload: int


@dataclass(frozen=True)
class ResultRow:
    baseline_wall_ns: int
    candidate_wall_ns: int
    baseline_cpu_ns: int
    candidate_cpu_ns: int
    baseline_p99_ns: int
    candidate_p99_ns: int
    activations: int
    segments: int
    logical_ops: int
    batch_width: int
    queue_publications: int
    task_parks: int
    terminal_wakes: int
    operation_sqes: int
    operation_cqes: int
    suppressed_success_cqes: int
    cancel_sqes: int
    cancel_cqes: int
    fixed_file_attachments: int
    fixed_buffer_attachments: int
    submit_calls: int
    submit_syscalls: int
    checksum: int
    status: str


@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    order: str
    cell: MatrixCell
    result: ResultRow


@dataclass(frozen=True)
class SummaryRow:
    candidate: str
    batch_width: int
    concurrency: int
    payload: int
    sample_count: int
    wall_ratio: float
    wall_ci_low: float
    wall_ci_high: float
    cpu_ratio: float
    cpu_ci_low: float
    cpu_ci_high: float
    p99_ratio: float
    p99_ci_low: float
    p99_ci_high: float
    structural_valid: bool

    @property
    def cell(self) -> MatrixCell:
        return MatrixCell(
            self.candidate,
            self.batch_width,
            self.concurrency,
            self.payload,
        )


class MatrixRunError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        command: Sequence[str] = (),
        stdout: str = "",
        stderr: str = "",
        rows: Sequence[SampleRow] = (),
    ) -> None:
        super().__init__(message)
        self.command = tuple(str(part) for part in command)
        self.stdout = stdout
        self.stderr = stderr
        self.rows = list(rows)


class NativeUnavailable(MatrixRunError):
    """The requested native benchmark candidate is unavailable."""


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (
        cell.candidate,
        cell.batch_width,
        cell.concurrency,
        cell.payload,
    )


def full_matrix() -> list[MatrixCell]:
    return [
        MatrixCell(candidate, width, concurrency, payload)
        for candidate in CANDIDATES
        for width in WIDTHS
        for concurrency in CONCURRENCY
        for payload in PAYLOADS
    ]


def _parse_integer(name: str, text: str) -> int:
    if DECIMAL_RE.fullmatch(text) is None:
        raise ValueError(f"invalid decimal integer field {name}")
    value = int(text, 10)
    if value > UINT64_MAX:
        raise ValueError(f"integer field out of uint64 range: {name}")
    return value


def _split_output(text: str) -> dict[str, str]:
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one RESULT row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty RESULT row")

    fields: dict[str, str] = {}
    observed: list[str] = []
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed RESULT field")
        name, value = token.split("=", 1)
        if not name or not value or name in fields:
            raise ValueError(f"duplicate or empty RESULT field: {name}")
        fields[name] = value
        observed.append(name)
    if tuple(observed) != FIELD_ORDER:
        raise ValueError(
            "RESULT field order/schema mismatch: "
            f"observed={observed}"
        )
    return fields


def _structural_valid(row: ResultRow, cell: MatrixCell) -> bool:
    if row.activations <= 0 or row.activations > UINT64_MAX // 2:
        return False
    logical = row.activations * 2
    effective_width = min(cell.batch_width, cell.concurrency)
    publications = (
        row.activations + effective_width - 1
    ) // effective_width
    uses_skip = cell.candidate != "link"
    uses_fixed = cell.candidate == "fixed_link_skip"
    fixed_attachments = (
        cell.concurrency * BLOCKS_PER_MODE
        if uses_fixed
        else 0
    )
    return (
        row.status == "OK"
        and row.batch_width == cell.batch_width
        and row.segments == row.activations
        and row.logical_ops == logical
        and row.queue_publications == publications
        and row.task_parks == publications
        and row.terminal_wakes == publications
        and row.operation_sqes == logical
        and row.operation_cqes
        == (row.activations if uses_skip else logical)
        and row.suppressed_success_cqes
        == (row.activations if uses_skip else 0)
        and row.cancel_sqes == 0
        and row.cancel_cqes == 0
        and row.fixed_file_attachments == fixed_attachments
        and row.fixed_buffer_attachments == fixed_attachments
        and row.submit_calls > 0
        and 0 < row.submit_syscalls <= row.submit_calls
    )


def parse_output(
    text: str,
    cell: MatrixCell,
    *,
    min_mode_ns: int,
) -> ResultRow:
    if cell.candidate not in ALL_CANDIDATES:
        raise ValueError("unknown native pipeline candidate")
    if min_mode_ns <= 0:
        raise ValueError("minimum mode duration must be positive")
    fields = _split_output(text)
    integers = {
        name: _parse_integer(name, fields[name])
        for name in INTEGER_FIELDS
    }
    row = ResultRow(
        **integers,
        status=fields["status"],
    )
    required_positive = (
        row.baseline_wall_ns,
        row.candidate_wall_ns,
        row.baseline_cpu_ns,
        row.candidate_cpu_ns,
        row.baseline_p99_ns,
        row.candidate_p99_ns,
        row.activations,
    )
    if any(value <= 0 for value in required_positive):
        raise ValueError("zero-valued required pipeline measurement")
    if (
        row.baseline_wall_ns < min_mode_ns
        or row.candidate_wall_ns < min_mode_ns
    ):
        raise ValueError(
            "measured wall duration is below the declared minimum"
        )
    if not _structural_valid(row, cell):
        raise ValueError(
            "pipeline queue/SQE/CQE/fixed-resource counters "
            "are inconsistent"
        )
    return row


def benchmark_command(
    binary: Path,
    cell: MatrixCell,
    *,
    activations: int,
    min_mode_ms: int,
    order: str,
) -> list[str]:
    if (
        cell.candidate not in ALL_CANDIDATES
        or cell.batch_width not in WIDTHS
        or cell.concurrency not in CONCURRENCY
        or cell.payload not in PAYLOADS
    ):
        raise ValueError("invalid native pipeline benchmark cell")
    if activations <= 0 or min_mode_ms <= 0:
        raise ValueError("benchmark counts must be positive")
    if order not in {"ABBA", "BAAB"}:
        raise ValueError("invalid benchmark order")
    return [
        str(binary.resolve()),
        "--candidate",
        cell.candidate,
        "--batch-width",
        str(cell.batch_width),
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
    return (
        len(text.encode("utf-8", errors="replace"))
        > MAX_OUTPUT_BYTES
    )


def _run_error(
    message: str,
    command: Sequence[str],
    result: CapturedProcess,
) -> MatrixRunError:
    return MatrixRunError(
        message,
        command=command,
        stdout=result.stdout,
        stderr=result.stderr,
    )


def run_one(
    binary: Path,
    cell: MatrixCell,
    *,
    process_sample: int,
    activations: int,
    min_mode_ms: int,
    runner: Runner = run_capture,
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
    try:
        result = runner(
            command,
            timeout=TIMEOUT_SECONDS,
            max_output_bytes=MAX_OUTPUT_BYTES,
        )
    except ProcessTimeoutError as exc:
        raise MatrixRunError(
            f"sample {process_sample} process timeout: {exc}",
            command=getattr(exc, "args_list", command),
            stdout=exc.stdout,
            stderr=exc.stderr,
        ) from exc
    except OSError as exc:
        raise MatrixRunError(
            f"sample {process_sample} process failure: {exc}",
            command=command,
        ) from exc
    if (
        result.stdout_truncated
        or result.stderr_truncated
        or _output_too_large(result.stdout)
        or _output_too_large(result.stderr)
    ):
        raise _run_error(
            f"sample {process_sample} exceeded the output cap",
            command,
            result,
        )
    if result.returncode == 77:
        prefix = (
            f"LEIR_PIPELINE_SKIP candidate={cell.candidate} "
            "reason="
        )
        reasons = {
            "backend_unavailable",
            "exact_result_semantic_barrier",
        }
        reason = (
            result.stderr[len(prefix) : -1]
            if (
                not result.stdout
                and result.stderr.startswith(prefix)
                and result.stderr.endswith("\n")
            )
            else ""
        )
        if (
            reason not in reasons
            or (
                reason == "exact_result_semantic_barrier"
                and cell.candidate == "fixed_link_skip"
            )
        ):
            raise _run_error(
                f"sample {process_sample} malformed pipeline skip",
                command,
                result,
            )
        raise NativeUnavailable(
            f"native pipeline unavailable for "
            f"{_cell_key(cell)}: {reason}",
            command=command,
            stdout=result.stdout,
            stderr=result.stderr,
        )
    if result.returncode != 0:
        raise _run_error(
            f"sample {process_sample} pipeline exit "
            f"{result.returncode}",
            command,
            result,
        )
    if result.stderr:
        raise _run_error(
            f"sample {process_sample} emitted stderr diagnostics",
            command,
            result,
        )
    try:
        row = parse_output(
            result.stdout,
            cell,
            min_mode_ns=min_mode_ms * 1_000_000,
        )
    except ValueError as exc:
        raise _run_error(
            f"sample {process_sample} result contract: {exc}",
            command,
            result,
        ) from exc
    minimum_activations = (
        max(activations, cell.concurrency) * BLOCKS_PER_MODE
    )
    if row.activations < minimum_activations:
        raise _run_error(
            f"sample {process_sample} activation count disagrees "
            "with the command",
            command,
            result,
        )
    return SampleRow(process_sample, order, cell, row)


def run_matrix(
    binary: Path,
    cells: Sequence[MatrixCell],
    *,
    samples: int,
    activations: int,
    min_mode_ms: int,
) -> tuple[list[SampleRow], list[dict[str, object]]]:
    if samples <= 0 or samples % 2 == 0:
        raise ValueError("an odd positive sample count is required")
    rows: list[SampleRow] = []
    unavailable: list[dict[str, object]] = []
    total = len(cells) * samples
    for cell_index, cell in enumerate(cells, start=1):
        try:
            # Each cell starts from a fresh, discarded process so loader,
            # allocator, and ring setup effects cannot become sample 1.
            run_one(
                binary,
                cell,
                process_sample=1,
                activations=activations,
                min_mode_ms=min_mode_ms,
            )
        except NativeUnavailable as exc:
            unavailable.append(
                {
                    "candidate": cell.candidate,
                    "batch_width": cell.batch_width,
                    "concurrency": cell.concurrency,
                    "payload": cell.payload,
                    "reason": str(exc),
                    "stderr": exc.stderr,
                }
            )
            print(
                f"[leir-native-pipeline] unavailable "
                f"cell={_cell_key(cell)}",
                file=sys.stderr,
                flush=True,
            )
            continue
        except MatrixRunError as exc:
            raise MatrixRunError(
                f"warmup cell {cell_index}/{len(cells)} "
                f"{_cell_key(cell)}: {exc}",
                command=exc.command,
                stdout=exc.stdout,
                stderr=exc.stderr,
                rows=rows,
            ) from exc

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
                raise MatrixRunError(
                    "native availability changed after warmup for "
                    f"{_cell_key(cell)}",
                    command=exc.command,
                    stdout=exc.stdout,
                    stderr=exc.stderr,
                    rows=rows,
                ) from exc
            except MatrixRunError as exc:
                raise MatrixRunError(
                    f"cell {cell_index}/{len(cells)} "
                    f"{_cell_key(cell)}: {exc}",
                    command=exc.command,
                    stdout=exc.stdout,
                    stderr=exc.stderr,
                    rows=rows,
                ) from exc
            rows.append(sample)
            print(
                f"[leir-native-pipeline] sample "
                f"{len(rows)}/{total} "
                f"cell={_cell_key(cell)} order={sample.order}",
                file=sys.stderr,
                flush=True,
            )
    return rows, unavailable


def _stable_seed(
    cell: MatrixCell,
    metric: str,
) -> int:
    payload = (
        f"{cell.candidate}:{cell.batch_width}:"
        f"{cell.concurrency}:{cell.payload}:{metric}"
    ).encode("ascii")
    return int.from_bytes(
        hashlib.sha256(payload).digest()[:8],
        byteorder="big",
    )


def _percentile(
    sorted_values: Sequence[float],
    probability: float,
) -> float:
    if not sorted_values:
        raise ValueError("cannot take a percentile of no values")
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    position = probability * (len(sorted_values) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    fraction = position - lower
    return float(
        sorted_values[lower] * (1.0 - fraction)
        + sorted_values[upper] * fraction
    )


def _bootstrap_geometric_mean(
    ratios: Sequence[float],
    *,
    seed: int,
    resamples: int = BOOTSTRAP_RESAMPLES,
) -> tuple[float, float, float]:
    if not ratios:
        raise ValueError("cannot summarize an empty ratio sequence")
    if any(
        not math.isfinite(value) or value <= 0.0
        for value in ratios
    ):
        raise ValueError("ratios must be finite and positive")
    logs = [math.log(value) for value in ratios]
    estimate = math.exp(statistics.fmean(logs))
    if len(logs) == 1:
        return estimate, estimate, estimate
    rng = random.Random(seed)
    sample_count = len(logs)
    bootstrapped = []
    for _ in range(resamples):
        mean_log = statistics.fmean(
            logs[rng.randrange(sample_count)]
            for _ in range(sample_count)
        )
        bootstrapped.append(math.exp(mean_log))
    bootstrapped.sort()
    return (
        estimate,
        _percentile(bootstrapped, 0.025),
        _percentile(bootstrapped, 0.975),
    )


def summarize(
    samples: Sequence[SampleRow],
) -> list[SummaryRow]:
    grouped: dict[tuple[object, ...], list[SampleRow]] = {}
    for sample in samples:
        if sample.process_sample <= 0:
            raise ValueError("invalid process sample index")
        if sample.order not in {"ABBA", "BAAB"}:
            raise ValueError("invalid sample order")
        grouped.setdefault(
            _cell_key(sample.cell), []
        ).append(sample)

    summaries: list[SummaryRow] = []
    for key in sorted(grouped):
        group = sorted(
            grouped[key],
            key=lambda sample: sample.process_sample,
        )
        indices = [sample.process_sample for sample in group]
        if len(indices) != len(set(indices)):
            raise ValueError(
                f"duplicate process sample index for {key}"
            )
        cell = group[0].cell
        structural = all(
            sample.cell == cell
            and sample.order
            == (
                "ABBA"
                if sample.process_sample % 2
                else "BAAB"
            )
            and _structural_valid(sample.result, cell)
            for sample in group
        )
        wall_ratios = [
            sample.result.candidate_wall_ns
            / sample.result.baseline_wall_ns
            for sample in group
        ]
        cpu_ratios = [
            sample.result.candidate_cpu_ns
            / sample.result.baseline_cpu_ns
            for sample in group
        ]
        p99_ratios = [
            sample.result.candidate_p99_ns
            / sample.result.baseline_p99_ns
            for sample in group
        ]
        wall = _bootstrap_geometric_mean(
            wall_ratios,
            seed=_stable_seed(cell, "wall"),
        )
        cpu = _bootstrap_geometric_mean(
            cpu_ratios,
            seed=_stable_seed(cell, "cpu"),
        )
        p99 = _bootstrap_geometric_mean(
            p99_ratios,
            seed=_stable_seed(cell, "p99"),
        )
        summaries.append(
            SummaryRow(
                candidate=cell.candidate,
                batch_width=cell.batch_width,
                concurrency=cell.concurrency,
                payload=cell.payload,
                sample_count=len(group),
                wall_ratio=wall[0],
                wall_ci_low=wall[1],
                wall_ci_high=wall[2],
                cpu_ratio=cpu[0],
                cpu_ci_low=cpu[1],
                cpu_ci_high=cpu[2],
                p99_ratio=p99[0],
                p99_ci_low=p99[1],
                p99_ci_high=p99[2],
                structural_valid=structural,
            )
        )
    return summaries


def classify(
    summaries: Sequence[SummaryRow],
    *,
    expected_samples: int,
    expected_cells: Sequence[MatrixCell] | None = None,
) -> tuple[str, list[str]]:
    if expected_samples <= 0:
        raise ValueError("expected sample count must be positive")
    by_key: dict[tuple[object, ...], SummaryRow] = {}
    duplicate_keys: set[tuple[object, ...]] = set()
    for row in summaries:
        key = _cell_key(row.cell)
        if key in by_key:
            duplicate_keys.add(key)
        by_key[key] = row

    structural_reasons: list[str] = []
    if duplicate_keys:
        structural_reasons.append(
            f"duplicate summary cells: {sorted(duplicate_keys)}"
        )
    for key, row in sorted(by_key.items()):
        ratios = (
            row.wall_ratio,
            row.wall_ci_low,
            row.wall_ci_high,
            row.cpu_ratio,
            row.cpu_ci_low,
            row.cpu_ci_high,
            row.p99_ratio,
            row.p99_ci_low,
            row.p99_ci_high,
        )
        if (
            not row.structural_valid
            or any(
                not math.isfinite(value) or value <= 0.0
                for value in ratios
            )
            or row.wall_ci_low > row.wall_ci_high
            or row.cpu_ci_low > row.cpu_ci_high
            or row.p99_ci_low > row.p99_ci_high
        ):
            structural_reasons.append(
                f"structural/statistical contract failed for {key}"
            )
    if structural_reasons:
        return "REJECT", structural_reasons

    wall_regressions = [
        key
        for key, row in sorted(by_key.items())
        if row.wall_ci_low > 1.0
    ]
    if wall_regressions:
        return (
            "REJECT",
            [
                "statistically supported wall regression for "
                f"{key}"
                for key in wall_regressions
            ],
        )

    if summaries:
        median_cpu = float(
            statistics.median(
                row.cpu_ratio for row in summaries
            )
        )
        if median_cpu > 1.03:
            return (
                "REJECT",
                [
                    "matrix median CPU ratio "
                    f"{median_cpu:.6f} exceeds 1.03"
                ],
            )
        median_p99 = float(
            statistics.median(
                row.p99_ratio for row in summaries
            )
        )
        if median_p99 > 1.10:
            return (
                "REJECT",
                [
                    "matrix median p99 ratio "
                    f"{median_p99:.6f} exceeds 1.10"
                ],
            )

    coverage_reasons: list[str] = []
    if not summaries:
        coverage_reasons.append("no native pipeline samples")
    for key, row in sorted(by_key.items()):
        if row.sample_count != expected_samples:
            coverage_reasons.append(
                f"sample count {row.sample_count}/"
                f"{expected_samples} for {key}"
            )
    if expected_cells is not None:
        expected = {
            _cell_key(cell) for cell in expected_cells
        }
        actual = set(by_key)
        if actual != expected:
            coverage_reasons.append(
                "matrix coverage mismatch: "
                f"missing={sorted(expected - actual)} "
                f"extra={sorted(actual - expected)}"
            )
    if coverage_reasons:
        return "INCONCLUSIVE", coverage_reasons

    winning_regions = {
        (row.concurrency, row.payload)
        for row in summaries
        if row.wall_ci_high <= 0.95
    }

    fixed_win = False
    batch_win = False
    for row in summaries:
        if row.candidate == "fixed_link_skip":
            link_key = (
                "link_skip",
                row.batch_width,
                row.concurrency,
                row.payload,
            )
            link = by_key.get(link_key)
            if (
                link is not None
                and row.wall_ratio < link.wall_ratio
            ):
                fixed_win = True
        if (
            row.batch_width in {4, 8}
            and min(row.batch_width, row.concurrency) > 1
        ):
            width_one = by_key.get(
                (
                    row.candidate,
                    1,
                    row.concurrency,
                    row.payload,
                )
            )
            if (
                width_one is not None
                and row.wall_ratio < width_one.wall_ratio
            ):
                batch_win = True

    misses: list[str] = []
    if len(winning_regions) < 2:
        misses.append(
            "fewer than two concurrency/payload regions have "
            "a 95% wall-ratio upper bound at or below 0.95"
        )
    if not fixed_win:
        misses.append(
            "no fixed-resource cell improves its comparable "
            "link_skip wall ratio"
        )
    if not batch_win:
        misses.append(
            "no width-4/8 cell improves its comparable width-1 "
            "wall ratio"
        )
    if misses:
        return "INCONCLUSIVE", misses
    return (
        "SPECIALIZED",
        [
            "two or more Linux/io_uring regions show at least 5% "
            "supported wall improvement",
            "fixed-resource and multi-segment batching wins are "
            "both present",
            "matrix CPU, p99, structural, and regression gates pass",
        ],
    )


RAW_FIELD_ORDER = (
    "process_sample",
    "order",
    "candidate",
    "concurrency",
    "payload",
    *FIELD_ORDER,
)
SUMMARY_FIELD_ORDER = tuple(
    SummaryRow.__dataclass_fields__.keys()
)


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
            "order": sample.order,
            "candidate": sample.cell.candidate,
            "concurrency": sample.cell.concurrency,
            "payload": sample.cell.payload,
            **asdict(sample.result),
        }
        for sample in samples
    ]


def _report_text(
    summaries: Sequence[SummaryRow],
    *,
    verdict: str,
    reasons: Sequence[str],
) -> str:
    lines = [
        "# Linux/io_uring specialized evidence",
        "",
        f"- Verdict: **{verdict}**",
        (
            "- Scope: connected Linux `io_uring` RECV→SEND "
            "compiled-effect segments only. A `SPECIALIZED` verdict "
            "is not a portable-runtime claim by itself."
        ),
        "- Ratios are candidate / public-API baseline; lower is better.",
        "",
        "## Precommitted gates",
        "",
        "| Gate | Threshold |",
        "|---|---:|",
        (
            "| Supported wall improvement | 95% ratio upper bound "
            "<= 0.95 in at least 2 regions |"
        ),
        "| Matrix median CPU ratio | <= 1.03 |",
        "| Matrix median p99 ratio | <= 1.10 |",
        "| Supported wall regression | none (95% lower bound > 1.00) |",
        "| Fixed-resource comparison | at least one wall-ratio win |",
        "| Width-4/8 batching comparison | at least one width-1 win |",
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
                "| candidate | width | concurrency | payload | samples "
                "| wall ratio [95% CI] | CPU ratio [95% CI] | "
                "p99 ratio [95% CI] | structure |"
            ),
            (
                "|---|---:|---:|---:|---:|---:|---:|---:|---|"
            ),
        ]
    )
    for row in summaries:
        lines.append(
            f"| {row.candidate} | {row.batch_width} | "
            f"{row.concurrency} | {row.payload} | "
            f"{row.sample_count} | "
            f"{row.wall_ratio:.6f} "
            f"[{row.wall_ci_low:.6f}, {row.wall_ci_high:.6f}] | "
            f"{row.cpu_ratio:.6f} "
            f"[{row.cpu_ci_low:.6f}, {row.cpu_ci_high:.6f}] | "
            f"{row.p99_ratio:.6f} "
            f"[{row.p99_ci_low:.6f}, {row.p99_ci_high:.6f}] | "
            f"{'valid' if row.structural_valid else 'invalid'} |"
        )
    lines.extend(
        [
            "",
            (
                "Confidence intervals use 10,000 deterministic "
                "bootstrap resamples of paired fresh-process log ratios."
            ),
            "",
        ]
    )
    return "\n".join(lines)


CLASSIFIER_THRESHOLDS = {
    "supported_wall_ratio_upper_max": 0.95,
    "minimum_winning_regions": 2,
    "matrix_median_cpu_ratio_max": 1.03,
    "matrix_median_p99_ratio_max": 1.10,
    "supported_wall_regression_lower_max": 1.00,
    "requires_fixed_resource_win": True,
    "requires_width_4_or_8_win": True,
    "bootstrap_resamples": BOOTSTRAP_RESAMPLES,
}


def _verdict_bytes(verdict: str, reasons: Sequence[str]) -> bytes:
    return canonical_json_bytes(
        {
            "schema": VERDICT_SCHEMA,
            "verdict": verdict,
            "reasons": list(reasons),
        }
    )


def _bundle_metadata(
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
        "unavailable_cells",
    }
    if set(metadata) != required:
        raise ValueError(
            "pipeline evidence metadata fields mismatch: "
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
            "benchmark": "leir_native_pipeline",
            "cells": [asdict(cell) for cell in full_matrix()],
        },
        "sample_schedule": {
            "samples": metadata["samples"],
            "activations": metadata["activations"],
            "min_mode_ms": metadata["min_mode_ms"],
            "process_warmups_per_cell": 1,
            "orders": ["ABBA", "BAAB"],
            "unavailable_cells": metadata["unavailable_cells"],
        },
        "classifier": {
            "schema": CLASSIFIER_SCHEMA,
            "thresholds": CLASSIFIER_THRESHOLDS,
        },
    }


def write_evidence(
    output_dir: Path,
    tracked_report: Path | None,
    samples: Sequence[SampleRow],
    summaries: Sequence[SummaryRow],
    *,
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
        verdict=verdict,
        reasons=reasons,
    ).encode("utf-8")
    complete_metadata = _bundle_metadata(metadata)
    recomputed = _recompute_artifacts(raw_csv, complete_metadata)
    if (
        recomputed.summary_csv != summary_csv
        or recomputed.verdict_json != verdict_json
        or recomputed.report_md != report
    ):
        raise ValueError(
            "pipeline evidence inputs do not match deterministic "
            "recomputation"
        )
    bundle = EvidenceBundle.create(output_dir, complete_metadata)
    bundle.write_bytes("raw.csv", raw_csv)
    bundle.write_bytes("summary.csv", summary_csv)
    bundle.write_bytes("verdict.json", verdict_json)
    bundle.write_bytes("report.md", report)
    bundle.finalize()


def _classify_evidence(
    summaries: Sequence[SummaryRow],
    *,
    expected_samples: int,
    unavailable: Sequence[dict[str, object]],
) -> tuple[str, list[str]]:
    verdict, reasons = classify(
        summaries,
        expected_samples=expected_samples,
        expected_cells=full_matrix(),
    )
    if unavailable:
        if verdict == "SPECIALIZED":
            verdict = "INCONCLUSIVE"
        reasons = [
            *reasons,
            f"{len(unavailable)} matrix cells were unavailable",
        ]
    return verdict, reasons


def _read_raw_samples(
    raw_csv: bytes,
    *,
    min_mode_ns: int,
) -> list[SampleRow]:
    try:
        text = raw_csv.decode("utf-8")
    except UnicodeError as exc:
        raise ValueError("raw.csv is not UTF-8") from exc
    reader = csv.DictReader(io.StringIO(text))
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
        order = fields["order"] or ""
        if order not in {"ABBA", "BAAB"}:
            raise ValueError("invalid raw sample order")
        cell = MatrixCell(
            fields["candidate"] or "",
            _parse_integer(
                "batch_width",
                fields["batch_width"] or "",
            ),
            _parse_integer(
                "concurrency",
                fields["concurrency"] or "",
            ),
            _parse_integer(
                "payload",
                fields["payload"] or "",
            ),
        )
        output = RESULT_PREFIX + " ".join(
            f"{name}={fields[name]}" for name in FIELD_ORDER
        )
        result = parse_output(
            output,
            cell,
            min_mode_ns=min_mode_ns,
        )
        expected_order = "ABBA" if process_sample % 2 else "BAAB"
        if order != expected_order:
            raise ValueError(
                "raw sample order disagrees with sample index"
            )
        key = (process_sample, *_cell_key(cell))
        if key in observed:
            raise ValueError("duplicate raw sample key")
        observed.add(key)
        samples.append(
            SampleRow(process_sample, order, cell, result)
        )
    return samples


def _recompute_artifacts(
    raw_csv: bytes,
    metadata: dict[str, object],
) -> RecomputedArtifacts:
    matrix = metadata["matrix"]
    schedule = metadata["sample_schedule"]
    classifier = metadata["classifier"]
    if matrix != {
        "benchmark": "leir_native_pipeline",
        "cells": [asdict(cell) for cell in full_matrix()],
    }:
        raise ValueError("pipeline matrix metadata mismatch")
    if not isinstance(schedule, dict) or set(schedule) != {
        "samples",
        "activations",
        "min_mode_ms",
        "process_warmups_per_cell",
        "orders",
        "unavailable_cells",
    }:
        raise ValueError("pipeline sample schedule metadata mismatch")
    expected_samples = schedule["samples"]
    activations = schedule["activations"]
    min_mode_ms = schedule["min_mode_ms"]
    unavailable = schedule["unavailable_cells"]
    if (
        not isinstance(expected_samples, int)
        or isinstance(expected_samples, bool)
        or expected_samples <= 0
        or expected_samples % 2 == 0
        or not isinstance(activations, int)
        or isinstance(activations, bool)
        or activations <= 0
        or not isinstance(min_mode_ms, int)
        or isinstance(min_mode_ms, bool)
        or min_mode_ms <= 0
        or schedule["process_warmups_per_cell"] != 1
        or schedule["orders"] != ["ABBA", "BAAB"]
        or not isinstance(unavailable, list)
    ):
        raise ValueError("invalid pipeline sample schedule")
    expected_unavailable_fields = {
        "candidate",
        "batch_width",
        "concurrency",
        "payload",
        "reason",
        "stderr",
    }
    valid_cells = {_cell_key(cell): cell for cell in full_matrix()}
    observed_unavailable: set[tuple[object, ...]] = set()
    for item in unavailable:
        if (
            not isinstance(item, dict)
            or set(item) != expected_unavailable_fields
        ):
            raise ValueError("invalid unavailable cell metadata")
        candidate = item["candidate"]
        integer_fields = (
            item["batch_width"],
            item["concurrency"],
            item["payload"],
        )
        if (
            not isinstance(candidate, str)
            or any(
                isinstance(value, bool) or not isinstance(value, int)
                for value in integer_fields
            )
        ):
            raise ValueError("invalid unavailable cell identity")
        cell_key = (candidate, *integer_fields)
        if (
            cell_key not in valid_cells
            or cell_key in observed_unavailable
        ):
            raise ValueError("invalid unavailable cell identity")
        observed_unavailable.add(cell_key)
        stderr = item["stderr"]
        if not isinstance(stderr, str):
            raise ValueError("invalid unavailable cell text")
        prefix = f"LEIR_PIPELINE_SKIP candidate={candidate} reason="
        skip_reason = (
            stderr[len(prefix) : -1]
            if stderr.startswith(prefix) and stderr.endswith("\n")
            else ""
        )
        if (
            skip_reason
            not in {"backend_unavailable", "exact_result_semantic_barrier"}
            or (
                skip_reason == "exact_result_semantic_barrier"
                and candidate == "fixed_link_skip"
            )
            or item["reason"]
            != (
                f"native pipeline unavailable for {cell_key}: "
                f"{skip_reason}"
            )
        ):
            raise ValueError("invalid unavailable cell reason")
    if classifier != {
        "schema": CLASSIFIER_SCHEMA,
        "thresholds": CLASSIFIER_THRESHOLDS,
    }:
        raise ValueError("pipeline classifier metadata mismatch")
    samples = _read_raw_samples(
        raw_csv,
        min_mode_ns=min_mode_ms * 1_000_000,
    )
    summaries = summarize(samples)
    verdict, reasons = _classify_evidence(
        summaries,
        expected_samples=expected_samples,
        unavailable=unavailable,
    )
    summary_csv = _csv_text(
        [asdict(row) for row in summaries],
        SUMMARY_FIELD_ORDER,
    ).encode("utf-8")
    return RecomputedArtifacts(
        summary_csv=summary_csv,
        verdict_json=_verdict_bytes(verdict, reasons),
        report_md=_report_text(
            summaries,
            verdict=verdict,
            reasons=reasons,
        ).encode("utf-8"),
    )


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
            "Run or audit connected Linux io_uring LEIR evidence."
        )
    )
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--tracked-report", type=Path)
    parser.add_argument("--audit-existing", type=Path)
    parser.add_argument("--source-commit")
    parser.add_argument("--source-dirty-digest")
    parser.add_argument("--require-source-commit")
    parser.add_argument("--require-source-dirty-digest")
    parser.add_argument(
        "--samples",
        type=_positive_int,
        default=SAMPLES,
    )
    parser.add_argument(
        "--activations",
        type=_positive_int,
        default=DEFAULT_ACTIVATIONS,
    )
    parser.add_argument(
        "--min-mode-ms",
        type=_positive_int,
        default=DEFAULT_MIN_MODE_MS,
    )
    return parser


def _source_commit() -> str:
    try:
        result = run_capture(
            ["git", "rev-parse", "HEAD"],
            timeout=5.0,
            max_output_bytes=4096,
        )
    except (OSError, ProcessTimeoutError):
        return "unavailable"
    if (
        result.returncode != 0
        or result.stderr
        or result.stdout_truncated
        or result.stderr_truncated
    ):
        return "unavailable"
    return result.stdout.strip() or "unavailable"


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
                "[bench_leir_native_pipeline.py] audit mode cannot "
                "also run a binary",
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
                f"[bench_leir_native_pipeline.py] audit failed: {exc}",
                file=sys.stderr,
            )
            return 2
        print(
            "[bench_leir_native_pipeline.py] "
            f"audit=OK verdict={verdict}"
        )
        for reason in reasons:
            print(f"[bench_leir_native_pipeline.py] {reason}")
        return 0

    if (
        args.binary is None
        or args.output_dir is None
        or args.require_source_commit is not None
        or args.require_source_dirty_digest is not None
    ):
        print(
            "[bench_leir_native_pipeline.py] --binary and "
            "--output-dir are required in run mode; require-source "
            "flags are audit-only",
            file=sys.stderr,
        )
        return 2
    if not args.binary.is_file():
        print(
            "[bench_leir_native_pipeline.py] binary does not exist",
            file=sys.stderr,
        )
        return 2
    if args.samples % 2 == 0:
        print(
            "[bench_leir_native_pipeline.py] samples must be odd",
            file=sys.stderr,
        )
        return 2

    cells = full_matrix()
    try:
        samples, unavailable = run_matrix(
            args.binary,
            cells,
            samples=args.samples,
            activations=args.activations,
            min_mode_ms=args.min_mode_ms,
        )
        summaries = summarize(samples)
        verdict, reasons = _classify_evidence(
            summaries,
            expected_samples=args.samples,
            unavailable=unavailable,
        )
    except MatrixRunError as exc:
        print(
            f"[bench_leir_native_pipeline.py] {exc}",
            file=sys.stderr,
        )
        return 2
    except ValueError as exc:
        print(
            f"[bench_leir_native_pipeline.py] {exc}",
            file=sys.stderr,
        )
        return 2

    invocation = [
        sys.executable,
        str(Path(__file__)),
        *(argv or sys.argv[1:]),
    ]
    source_commit = args.source_commit or _source_commit()
    dirty_digest = (
        args.source_dirty_digest or _source_dirty_digest()
    )
    try:
        write_evidence(
            args.output_dir,
            args.tracked_report,
            samples,
            summaries,
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
                "samples": args.samples,
                "activations": args.activations,
                "min_mode_ms": args.min_mode_ms,
                "unavailable_cells": unavailable,
            },
        )
    except (EvidenceError, OSError, RuntimeError, ValueError) as exc:
        print(
            "[bench_leir_native_pipeline.py] evidence write failed: "
            f"{exc}",
            file=sys.stderr,
        )
        return 2
    if args.tracked_report is not None:
        print(
            "[bench_leir_native_pipeline.py] --tracked-report is "
            "deprecated; use output bundle report.md",
            file=sys.stderr,
        )
    print(
        "[bench_leir_native_pipeline.py] "
        f"verdict={verdict} cells={len(cells)} "
        f"samples={args.samples} unavailable={len(unavailable)}"
    )
    for reason in reasons:
        print(f"[bench_leir_native_pipeline.py] {reason}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
