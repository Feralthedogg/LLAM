#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Run, retain, select, and classify the SREM Phase 0 cost model."""

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
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Sequence

from process_utils import ProcessTimeoutError, run_capture
from safe_output import write_text_safely


RESULT_PREFIX = "SREM_PAIR "
HOMOGENEOUS_WORKLOADS = (
    "srem_http_pipeline",
    "srem_rpc_pipeline",
)
DIVERGENT_WORKLOAD = "srem_divergent_cancel"
MIXED_WORKLOAD = "srem_mixed_fairness"
WORKLOADS = {
    *HOMOGENEOUS_WORKLOADS,
    DIVERGENT_WORKLOAD,
    MIXED_WORKLOAD,
}
CANDIDATE_BASELINES = {
    "tile_scalar": "waker_frame",
    "tile_vector": "waker_frame",
    "adaptive_srem": "waker_frame",
    "remote_adaptive_srem": "remote_waker_frame",
}
FRAME_BYTES = (64, 128, 256)
TILE_WIDTHS = (8, 16, 32)
SITE_COUNTS = (1, 8)
DIVERGENCE_EIGHTHS = (0, 1, 4)
SCREEN_SAMPLES = 5
FULL_SAMPLES = 9
SCREEN_MIN_MODE_NS = 100_000_000
FULL_MIN_MODE_NS = 250_000_000
QUICK_MIN_MODE_NS = 2_000_000
PAIR_BLOCKS_PER_MODE = 16
SCREEN_SEED = 6_043_432_235_128_363_791
FULL_SEED = 12_833_226_585_820_451_329
QUICK_SEED = 8_909_327_414_507_219_281

EXPECTED_FIELDS = {
    "version",
    "workload",
    "candidate",
    "baseline",
    "instances",
    "frame_bytes",
    "tile_width",
    "active_lanes",
    "sites",
    "divergence_eighths",
    "threshold",
    "producers",
    "seed",
    "min_mode_ns",
    "warmup_rounds",
    "rounds_per_block",
    "blocks_per_mode",
    "ops_per_mode",
    "baseline_wall_ns",
    "candidate_wall_ns",
    "baseline_cpu_ns",
    "candidate_cpu_ns",
    "wall_speedup",
    "cpu_ratio",
    "baseline_checksum",
    "candidate_checksum",
    "baseline_completions",
    "candidate_completions",
    "baseline_queue_pushes",
    "candidate_queue_pushes",
    "baseline_queue_pops",
    "candidate_queue_pops",
    "candidate_tile_dispatches",
    "candidate_scalar_lanes",
    "candidate_vector_lanes",
    "candidate_vector_blocks",
    "baseline_remote_pushes",
    "candidate_remote_pushes",
    "baseline_fair_samples",
    "candidate_fair_samples",
    "baseline_fair_p99_gap",
    "candidate_fair_p99_gap",
    "candidate_forced_escapes",
    "hot_allocations",
    "order",
    "affinity",
    "clock",
    "compiler",
    "host",
}
INTEGER_FIELDS = EXPECTED_FIELDS - {
    "version",
    "workload",
    "candidate",
    "baseline",
    "wall_speedup",
    "cpu_ratio",
    "baseline_checksum",
    "candidate_checksum",
    "order",
    "affinity",
    "clock",
    "compiler",
    "host",
}
DECIMAL_RE = re.compile(r"(?:0|[1-9][0-9]*)")
RATIO_RE = re.compile(r"(?:0|[1-9][0-9]*)\.[0-9]{12}")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")
TOKEN_RE = re.compile(r"[^\s]+")


@dataclass(frozen=True)
class PairRow:
    workload: str
    candidate: str
    baseline: str
    instances: int
    frame_bytes: int
    tile_width: int
    active_lanes: int
    sites: int
    divergence_eighths: int
    threshold: int
    producers: int
    seed: int
    min_mode_ns: int
    warmup_rounds: int
    rounds_per_block: int
    blocks_per_mode: int
    ops_per_mode: int
    baseline_wall_ns: int
    candidate_wall_ns: int
    baseline_cpu_ns: int
    candidate_cpu_ns: int
    wall_speedup: float
    cpu_ratio: float
    baseline_checksum: str
    candidate_checksum: str
    baseline_completions: int
    candidate_completions: int
    baseline_queue_pushes: int
    candidate_queue_pushes: int
    baseline_queue_pops: int
    candidate_queue_pops: int
    candidate_tile_dispatches: int
    candidate_scalar_lanes: int
    candidate_vector_lanes: int
    candidate_vector_blocks: int
    baseline_remote_pushes: int
    candidate_remote_pushes: int
    baseline_fair_samples: int
    candidate_fair_samples: int
    baseline_fair_p99_gap: int
    candidate_fair_p99_gap: int
    candidate_forced_escapes: int
    hot_allocations: int
    order: str
    affinity: str
    clock: str
    compiler: str
    host: str


@dataclass(frozen=True)
class SampleRow:
    process_sample: int
    row: PairRow


@dataclass(frozen=True)
class SummaryRow:
    workload: str
    candidate: str
    baseline: str
    instances: int
    frame_bytes: int
    tile_width: int
    active_lanes: int
    sites: int
    divergence_eighths: int
    threshold: int
    producers: int
    seed: int
    warmup_rounds: int
    sample_count: int
    wall_speedup: float
    cpu_ratio: float
    wall_best_speedup: float
    cpu_best_ratio: float
    wall_ratio_spread: float
    cpu_ratio_spread: float
    baseline_wall_ns_per_op: float
    candidate_wall_ns_per_op: float
    baseline_cpu_ns_per_op: float
    candidate_cpu_ns_per_op: float
    fairness_p99_ratio: float
    checksum: str
    min_mode_ns: int


@dataclass(frozen=True)
class MatrixCell:
    workload: str
    candidate: str
    baseline: str
    frame_bytes: int
    tile_width: int
    active_lanes: int
    sites: int
    divergence_eighths: int
    threshold: int
    producers: int
    seed: int


@dataclass(frozen=True)
class ScreeningSelection:
    width: int
    threshold: int
    wall_floor: float | None
    cpu_ceiling: float | None


@dataclass(frozen=True)
class ScreeningBound:
    wall_floor: float
    cpu_ceiling: float
    can_reach_category: bool
    details: tuple[str, ...]


class MatrixRunError(RuntimeError):
    def __init__(self, message: str, samples: list[SampleRow]) -> None:
        super().__init__(message)
        self.samples = samples


def _split_output(text: str) -> dict[str, str]:
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one SREM_PAIR result row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty SREM_PAIR result row")

    fields: dict[str, str] = {}
    for token in payload.split(" "):
        key, separator, value = token.partition("=")
        if (
            not separator
            or not key
            or not value
            or key in fields
            or TOKEN_RE.fullmatch(value) is None
        ):
            raise ValueError(f"malformed or duplicate SREM_PAIR field: {key}")
        fields[key] = value
    if set(fields) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(fields))
        extra = sorted(set(fields) - EXPECTED_FIELDS)
        raise ValueError(
            f"SREM_PAIR field mismatch: missing={missing} extra={extra}"
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
        raise ValueError(f"invalid positive ratio field {name}")
    return parsed


def _ratio_matches(printed: float, recomputed: float) -> bool:
    return (
        math.isfinite(recomputed)
        and recomputed > 0.0
        and abs(printed - recomputed) / recomputed <= 1e-9
    )


def _active_per_round(instances: int, width: int, active: int) -> int:
    full, remainder = divmod(instances, width)
    return full * active + min(remainder, active)


def parse_output(text: str) -> PairRow:
    fields = _split_output(text)
    if fields["version"] != "2":
        raise ValueError("unsupported SREM_PAIR version")
    integers = {
        name: _parse_integer(name, fields[name])
        for name in INTEGER_FIELDS
    }
    wall_speedup = _parse_ratio("wall_speedup", fields["wall_speedup"])
    cpu_ratio = _parse_ratio("cpu_ratio", fields["cpu_ratio"])
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
    if integers["tile_width"] not in TILE_WIDTHS:
        raise ValueError("unsupported tile width")
    if not (
        1
        <= integers["active_lanes"]
        <= integers["tile_width"]
    ):
        raise ValueError("active lanes out of range")
    if integers["sites"] not in SITE_COUNTS:
        raise ValueError("unsupported site count")
    if integers["divergence_eighths"] not in DIVERGENCE_EIGHTHS:
        raise ValueError("unsupported divergence")
    if not (
        1 <= integers["threshold"] <= integers["tile_width"]
    ):
        raise ValueError("threshold out of range")
    expected_producers = (
        2 if candidate == "remote_adaptive_srem" else 0
    )
    if integers["producers"] != expected_producers:
        raise ValueError("producer count disagrees with pair")
    if integers["seed"] == 0 or integers["warmup_rounds"] == 0:
        raise ValueError("zero seed or warmup")
    if not (
        1_000_000
        <= integers["min_mode_ns"]
        <= 60_000_000_000
    ):
        raise ValueError("minimum duration out of range")
    if integers["rounds_per_block"] <= 0:
        raise ValueError("non-positive round count")
    if integers["blocks_per_mode"] != PAIR_BLOCKS_PER_MODE:
        raise ValueError("unexpected paired block count")

    active = _active_per_round(
        integers["instances"],
        integers["tile_width"],
        integers["active_lanes"],
    )
    expected_ops = (
        active
        * integers["rounds_per_block"]
        * integers["blocks_per_mode"]
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
            raise ValueError(f"non-positive duration {name}")
    if (
        integers["baseline_wall_ns"] < integers["min_mode_ns"]
        or integers["candidate_wall_ns"] < integers["min_mode_ns"]
    ):
        raise ValueError("wall duration below declared minimum")
    recomputed_wall = (
        integers["baseline_wall_ns"]
        / integers["candidate_wall_ns"]
    )
    recomputed_cpu = (
        integers["candidate_cpu_ns"]
        / integers["baseline_cpu_ns"]
    )
    if not _ratio_matches(wall_speedup, recomputed_wall):
        raise ValueError("printed wall ratio mismatch")
    if not _ratio_matches(cpu_ratio, recomputed_cpu):
        raise ValueError("printed CPU ratio mismatch")

    for name in ("baseline_checksum", "candidate_checksum"):
        if CHECKSUM_RE.fullmatch(fields[name]) is None:
            raise ValueError(f"invalid checksum field {name}")
    if fields["baseline_checksum"] != fields["candidate_checksum"]:
        raise ValueError("canonical checksum mismatch")
    if (
        integers["baseline_completions"] != expected_ops
        or integers["candidate_completions"] != expected_ops
        or integers["baseline_queue_pushes"] != expected_ops
        or integers["baseline_queue_pops"] != expected_ops
        or integers["candidate_queue_pushes"]
        != integers["candidate_queue_pops"]
        or not (
            0
            < integers["candidate_queue_pushes"]
            <= expected_ops
        )
        or integers["candidate_tile_dispatches"]
        != integers["candidate_queue_pops"]
        or integers["candidate_scalar_lanes"]
        + integers["candidate_vector_lanes"]
        != expected_ops
    ):
        raise ValueError("completion or queue accounting mismatch")
    if candidate == "tile_scalar" and (
        integers["candidate_vector_lanes"] != 0
        or integers["candidate_vector_blocks"] != 0
    ):
        raise ValueError("scalar candidate reported vector execution")
    if candidate == "tile_vector" and (
        integers["candidate_scalar_lanes"] != 0
        or integers["candidate_vector_blocks"] == 0
    ):
        raise ValueError("vector candidate accounting mismatch")
    if (
        integers["candidate_vector_lanes"] == 0
        and integers["candidate_vector_blocks"] != 0
    ) or (
        integers["candidate_vector_lanes"] != 0
        and integers["candidate_vector_blocks"] == 0
    ):
        raise ValueError("vector lane/block mismatch")

    expected_remote = (
        expected_ops
        if candidate == "remote_adaptive_srem"
        else 0
    )
    if (
        integers["baseline_remote_pushes"] != expected_remote
        or integers["candidate_remote_pushes"] != expected_remote
    ):
        raise ValueError("remote accounting mismatch")
    if workload == MIXED_WORKLOAD:
        if (
            integers["baseline_fair_samples"] <= 0
            or integers["baseline_fair_samples"]
            != integers["candidate_fair_samples"]
            or integers["baseline_fair_p99_gap"] <= 0
            or integers["candidate_fair_p99_gap"] <= 0
        ):
            raise ValueError("mixed fairness accounting mismatch")
    elif any(
        integers[name] != 0
        for name in (
            "baseline_fair_samples",
            "candidate_fair_samples",
            "baseline_fair_p99_gap",
            "candidate_fair_p99_gap",
        )
    ):
        raise ValueError("non-fair row carries fairness metrics")
    if integers["hot_allocations"] != 0:
        raise ValueError("measured hot allocation")
    if fields["order"] not in {"ABBA", "BAAB"}:
        raise ValueError("invalid pair order")
    if fields["clock"] != "monotonic+process_cpu":
        raise ValueError("unsupported clock contract")
    for name in ("affinity", "compiler", "host"):
        if TOKEN_RE.fullmatch(fields[name]) is None:
            raise ValueError(f"invalid token field {name}")

    return PairRow(
        workload=workload,
        candidate=candidate,
        baseline=baseline,
        instances=integers["instances"],
        frame_bytes=integers["frame_bytes"],
        tile_width=integers["tile_width"],
        active_lanes=integers["active_lanes"],
        sites=integers["sites"],
        divergence_eighths=integers["divergence_eighths"],
        threshold=integers["threshold"],
        producers=integers["producers"],
        seed=integers["seed"],
        min_mode_ns=integers["min_mode_ns"],
        warmup_rounds=integers["warmup_rounds"],
        rounds_per_block=integers["rounds_per_block"],
        blocks_per_mode=integers["blocks_per_mode"],
        ops_per_mode=integers["ops_per_mode"],
        baseline_wall_ns=integers["baseline_wall_ns"],
        candidate_wall_ns=integers["candidate_wall_ns"],
        baseline_cpu_ns=integers["baseline_cpu_ns"],
        candidate_cpu_ns=integers["candidate_cpu_ns"],
        wall_speedup=wall_speedup,
        cpu_ratio=cpu_ratio,
        baseline_checksum=fields["baseline_checksum"],
        candidate_checksum=fields["candidate_checksum"],
        baseline_completions=integers["baseline_completions"],
        candidate_completions=integers["candidate_completions"],
        baseline_queue_pushes=integers["baseline_queue_pushes"],
        candidate_queue_pushes=integers["candidate_queue_pushes"],
        baseline_queue_pops=integers["baseline_queue_pops"],
        candidate_queue_pops=integers["candidate_queue_pops"],
        candidate_tile_dispatches=integers["candidate_tile_dispatches"],
        candidate_scalar_lanes=integers["candidate_scalar_lanes"],
        candidate_vector_lanes=integers["candidate_vector_lanes"],
        candidate_vector_blocks=integers["candidate_vector_blocks"],
        baseline_remote_pushes=integers["baseline_remote_pushes"],
        candidate_remote_pushes=integers["candidate_remote_pushes"],
        baseline_fair_samples=integers["baseline_fair_samples"],
        candidate_fair_samples=integers["candidate_fair_samples"],
        baseline_fair_p99_gap=integers["baseline_fair_p99_gap"],
        candidate_fair_p99_gap=integers["candidate_fair_p99_gap"],
        candidate_forced_escapes=integers["candidate_forced_escapes"],
        hot_allocations=integers["hot_allocations"],
        order=fields["order"],
        affinity=fields["affinity"],
        clock=fields["clock"],
        compiler=fields["compiler"],
        host=fields["host"],
    )


def _thresholds(width: int) -> tuple[int, ...]:
    return tuple(sorted({max(1, width // 4), width // 2, width}))


def _occupancies(width: int) -> tuple[int, ...]:
    return tuple(sorted({1, width // 4, width // 2, width}))


def _cell(
    workload: str,
    candidate: str,
    frame_bytes: int,
    width: int,
    active: int,
    sites: int,
    divergence: int,
    threshold: int,
    seed: int,
) -> MatrixCell:
    return MatrixCell(
        workload=workload,
        candidate=candidate,
        baseline=CANDIDATE_BASELINES[candidate],
        frame_bytes=frame_bytes,
        tile_width=width,
        active_lanes=active,
        sites=sites,
        divergence_eighths=divergence,
        threshold=threshold,
        producers=2 if candidate == "remote_adaptive_srem" else 0,
        seed=seed,
    )


def screening_matrix(seed: int = SCREEN_SEED) -> list[MatrixCell]:
    cells: list[MatrixCell] = []
    for workload in HOMOGENEOUS_WORKLOADS:
        for width in TILE_WIDTHS:
            for candidate in ("tile_scalar", "tile_vector"):
                for active in _occupancies(width):
                    cells.append(
                        _cell(
                            workload,
                            candidate,
                            128,
                            width,
                            active,
                            1,
                            0,
                            width // 2,
                            seed,
                        )
                    )
            for threshold in _thresholds(width):
                for active in _occupancies(width):
                    cells.append(
                        _cell(
                            workload,
                            "adaptive_srem",
                            128,
                            width,
                            active,
                            1,
                            0,
                            threshold,
                            seed,
                        )
                    )
    return cells


def full_matrix(
    width: int,
    threshold: int,
    seed: int = FULL_SEED,
) -> list[MatrixCell]:
    if width not in TILE_WIDTHS or not (1 <= threshold <= width):
        raise ValueError("invalid selected width or threshold")
    cells: list[MatrixCell] = []
    half = width // 2

    for workload in HOMOGENEOUS_WORKLOADS:
        for active in (half, width):
            for frame_bytes in FRAME_BYTES:
                for sites in SITE_COUNTS:
                    for divergence in (0, 1):
                        cells.append(
                            _cell(
                                workload,
                                "adaptive_srem",
                                frame_bytes,
                                width,
                                active,
                                sites,
                                divergence,
                                threshold,
                                seed,
                            )
                        )
        for sites in SITE_COUNTS:
            cells.append(
                _cell(
                    workload,
                    "adaptive_srem",
                    128,
                    width,
                    1,
                    sites,
                    0,
                    threshold,
                    seed,
                )
            )
        for active in (half, width):
            cells.append(
                _cell(
                    workload,
                    "adaptive_srem",
                    128,
                    width,
                    active,
                    8,
                    4,
                    threshold,
                    seed,
                )
            )
            cells.append(
                _cell(
                    workload,
                    "remote_adaptive_srem",
                    128,
                    width,
                    active,
                    8,
                    0,
                    threshold,
                    seed,
                )
            )
    for active in _occupancies(width):
        cells.append(
            _cell(
                MIXED_WORKLOAD,
                "adaptive_srem",
                128,
                width,
                active,
                8,
                1,
                threshold,
                seed,
            )
        )
    if len(cells) != len(set(cells)):
        raise RuntimeError("full matrix contains duplicate cells")
    return cells


def quick_matrix(
    width: int = 16,
    threshold: int = 8,
    seed: int = QUICK_SEED,
) -> list[MatrixCell]:
    return [
        _cell(
            "srem_http_pipeline",
            "adaptive_srem",
            128,
            width,
            width // 2,
            1,
            0,
            threshold,
            seed,
        ),
        _cell(
            "srem_rpc_pipeline",
            "tile_vector",
            128,
            width,
            width,
            1,
            0,
            threshold,
            seed,
        ),
        _cell(
            "srem_http_pipeline",
            "remote_adaptive_srem",
            128,
            width,
            width // 2,
            8,
            0,
            threshold,
            seed,
        ),
        _cell(
            MIXED_WORKLOAD,
            "adaptive_srem",
            128,
            width,
            width // 2,
            8,
            1,
            threshold,
            seed,
        ),
    ]


def _pair_key(row: PairRow) -> tuple[object, ...]:
    return (
        row.workload,
        row.candidate,
        row.baseline,
        row.frame_bytes,
        row.tile_width,
        row.active_lanes,
        row.sites,
        row.divergence_eighths,
        row.threshold,
        row.producers,
        row.seed,
    )


def _summary_key(row: SummaryRow) -> tuple[object, ...]:
    return (
        row.workload,
        row.candidate,
        row.baseline,
        row.frame_bytes,
        row.tile_width,
        row.active_lanes,
        row.sites,
        row.divergence_eighths,
        row.threshold,
        row.producers,
        row.seed,
    )


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (
        cell.workload,
        cell.candidate,
        cell.baseline,
        cell.frame_bytes,
        cell.tile_width,
        cell.active_lanes,
        cell.sites,
        cell.divergence_eighths,
        cell.threshold,
        cell.producers,
        cell.seed,
    )


def _median(values: Sequence[float]) -> float:
    ordered = sorted(values)
    if not ordered or len(ordered) % 2 == 0:
        raise ValueError("an odd non-empty sample set is required")
    return ordered[len(ordered) // 2]


def _summarize_group(samples: list[SampleRow]) -> SummaryRow:
    if not samples or len(samples) % 2 == 0:
        raise ValueError("summary requires an odd non-empty sample group")
    identity = _pair_key(samples[0].row)
    instances = samples[0].row.instances
    min_mode_ns = samples[0].row.min_mode_ns
    warmup_rounds = samples[0].row.warmup_rounds
    seen: set[int] = set()
    checksums: dict[int, set[str]] = {}

    for sample in samples:
        row = sample.row
        if _pair_key(row) != identity:
            raise ValueError("sample group mixes matrix cells")
        if (
            row.instances != instances
            or row.min_mode_ns != min_mode_ns
            or row.warmup_rounds != warmup_rounds
        ):
            raise ValueError("sample group mixes run controls")
        if sample.process_sample <= 0 or sample.process_sample in seen:
            raise ValueError("duplicate process sample")
        seen.add(sample.process_sample)
        checksums.setdefault(row.rounds_per_block, set()).add(
            row.baseline_checksum
        )

    selected = sorted(
        samples, key=lambda sample: sample.row.wall_speedup
    )[len(samples) // 2].row
    wall = [sample.row.wall_speedup for sample in samples]
    cpu = [sample.row.cpu_ratio for sample in samples]
    fairness_ratios = [
        sample.row.candidate_fair_p99_gap
        / sample.row.baseline_fair_p99_gap
        for sample in samples
        if sample.row.workload == MIXED_WORKLOAD
    ]
    checksum = (
        "MISMATCH"
        if any(len(values) != 1 for values in checksums.values())
        else selected.baseline_checksum
    )
    return SummaryRow(
        workload=selected.workload,
        candidate=selected.candidate,
        baseline=selected.baseline,
        instances=selected.instances,
        frame_bytes=selected.frame_bytes,
        tile_width=selected.tile_width,
        active_lanes=selected.active_lanes,
        sites=selected.sites,
        divergence_eighths=selected.divergence_eighths,
        threshold=selected.threshold,
        producers=selected.producers,
        seed=selected.seed,
        warmup_rounds=selected.warmup_rounds,
        sample_count=len(samples),
        wall_speedup=selected.wall_speedup,
        cpu_ratio=selected.cpu_ratio,
        wall_best_speedup=max(wall),
        cpu_best_ratio=min(cpu),
        wall_ratio_spread=max(wall) / min(wall),
        cpu_ratio_spread=max(cpu) / min(cpu),
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
        fairness_p99_ratio=(
            _median(fairness_ratios) if fairness_ratios else 1.0
        ),
        checksum=checksum,
        min_mode_ns=min_mode_ns,
    )


def summarize(rows: list[SampleRow]) -> list[SummaryRow]:
    grouped: dict[tuple[object, ...], list[SampleRow]] = {}
    for sample in rows:
        grouped.setdefault(_pair_key(sample.row), []).append(sample)
    return [
        _summarize_group(grouped[key])
        for key in sorted(
            grouped, key=lambda item: tuple(str(value) for value in item)
        )
    ]


def _summary_map(
    rows: Sequence[SummaryRow],
) -> dict[tuple[object, ...], SummaryRow]:
    result: dict[tuple[object, ...], SummaryRow] = {}
    for row in rows:
        key = _summary_key(row)
        if key in result:
            raise ValueError(f"duplicate summary cell: {key}")
        result[key] = row
    return result


def _screen_integrity(rows: Sequence[SummaryRow]) -> None:
    expected = {_cell_key(cell) for cell in screening_matrix()}
    actual = _summary_map(rows)
    if set(actual) != expected:
        raise ValueError(
            f"screen matrix mismatch: missing={len(expected-set(actual))} "
            f"extra={len(set(actual)-expected)}"
        )
    for row in rows:
        if (
            row.sample_count != SCREEN_SAMPLES
            or row.min_mode_ns < SCREEN_MIN_MODE_NS
            or not (1.0 <= row.wall_ratio_spread <= 1.10)
            or not (1.0 <= row.cpu_ratio_spread <= 1.10)
            or CHECKSUM_RE.fullmatch(row.checksum) is None
        ):
            raise ValueError("screen summary failed integrity controls")


def select_screening(
    rows: Sequence[SummaryRow],
) -> ScreeningSelection:
    _screen_integrity(rows)
    by_key = _summary_map(rows)
    candidates: list[ScreeningSelection] = []

    for width in TILE_WIDTHS:
        selected_threshold: int | None = None
        for threshold in _thresholds(width):
            passes = True
            for workload in HOMOGENEOUS_WORKLOADS:
                adaptive_sparse = by_key[
                    _cell_key(
                        _cell(
                            workload,
                            "adaptive_srem",
                            128,
                            width,
                            1,
                            1,
                            0,
                            threshold,
                            SCREEN_SEED,
                        )
                    )
                ]
                adaptive_dense = by_key[
                    _cell_key(
                        _cell(
                            workload,
                            "adaptive_srem",
                            128,
                            width,
                            width,
                            1,
                            0,
                            threshold,
                            SCREEN_SEED,
                        )
                    )
                ]
                forced_vector = by_key[
                    _cell_key(
                        _cell(
                            workload,
                            "tile_vector",
                            128,
                            width,
                            width,
                            1,
                            0,
                            width // 2,
                            SCREEN_SEED,
                        )
                    )
                ]
                if (
                    adaptive_sparse.wall_speedup < 0.95
                    or adaptive_dense.wall_speedup
                    < forced_vector.wall_speedup * 0.95
                ):
                    passes = False
                    break
            if passes:
                selected_threshold = threshold
                break
        if selected_threshold is None:
            continue
        gate_rows = [
            row
            for row in rows
            if (
                row.candidate == "adaptive_srem"
                and row.tile_width == width
                and row.threshold == selected_threshold
                and row.workload in HOMOGENEOUS_WORKLOADS
                and row.active_lanes in {width // 2, width}
            )
        ]
        if len(gate_rows) != 4:
            raise ValueError("screen selection rows are incomplete")
        candidates.append(
            ScreeningSelection(
                width=width,
                threshold=selected_threshold,
                wall_floor=min(row.wall_speedup for row in gate_rows),
                cpu_ceiling=max(row.cpu_ratio for row in gate_rows),
            )
        )
    if not candidates:
        raise ValueError("no width has a valid adaptive threshold")
    return max(
        candidates,
        key=lambda item: (
            item.wall_floor,
            -item.cpu_ceiling,
            -item.width,
            -item.threshold,
        ),
    )


def screen_candidate_bound(
    rows: Sequence[SummaryRow],
    selection: ScreeningSelection,
) -> ScreeningBound:
    _screen_integrity(rows)
    details: list[str] = []
    workload_possible: list[bool] = []
    wall_values: list[float] = []
    cpu_values: list[float] = []

    for workload in HOMOGENEOUS_WORKLOADS:
        selected = [
            row
            for row in rows
            if (
                row.workload == workload
                and row.candidate == "adaptive_srem"
                and row.tile_width == selection.width
                and row.threshold == selection.threshold
                and row.active_lanes
                in {selection.width // 2, selection.width}
            )
        ]
        if len(selected) != 2:
            raise ValueError("candidate-bound rows are incomplete")
        wall_floor = min(row.wall_best_speedup for row in selected)
        cpu_ceiling = max(row.cpu_best_ratio for row in selected)
        wall_values.append(wall_floor)
        cpu_values.append(cpu_ceiling)
        possible = wall_floor >= 1.50 and cpu_ceiling <= 0.70
        workload_possible.append(possible)
        details.append(
            f"{workload}: favorable wall floor={wall_floor:.6f}x "
            f"CPU ceiling={cpu_ceiling:.6f}x possible={possible}"
        )
    return ScreeningBound(
        wall_floor=min(wall_values),
        cpu_ceiling=max(cpu_values),
        can_reach_category=all(workload_possible),
        details=tuple(details),
    )


def _integrity_reasons(
    rows: Sequence[SummaryRow],
    width: int,
    threshold: int,
    expected_samples: int,
) -> list[str]:
    expected = {
        _cell_key(cell) for cell in full_matrix(width, threshold)
    }
    actual: dict[tuple[object, ...], SummaryRow] = {}
    reasons: list[str] = []

    if expected_samples != FULL_SAMPLES:
        reasons.append(
            f"full gate requires exactly {FULL_SAMPLES} samples"
        )
    for row in rows:
        key = _summary_key(row)
        if key in actual:
            reasons.append(f"duplicate summary cell: {key}")
        actual[key] = row
        if row.sample_count != expected_samples:
            reasons.append(f"sample count mismatch for {key}")
        if row.min_mode_ns < FULL_MIN_MODE_NS:
            reasons.append(f"insufficient duration for {key}")
        if not (1.0 <= row.wall_ratio_spread <= 1.10):
            reasons.append(f"wall spread above 1.10x for {key}")
        if not (1.0 <= row.cpu_ratio_spread <= 1.10):
            reasons.append(f"CPU spread above 1.10x for {key}")
        if CHECKSUM_RE.fullmatch(row.checksum) is None:
            reasons.append(f"checksum mismatch for {key}")
        numeric = (
            row.wall_speedup,
            row.cpu_ratio,
            row.wall_best_speedup,
            row.cpu_best_ratio,
            row.baseline_wall_ns_per_op,
            row.candidate_wall_ns_per_op,
            row.baseline_cpu_ns_per_op,
            row.candidate_cpu_ns_per_op,
            row.fairness_p99_ratio,
        )
        if any(
            not math.isfinite(value) or value <= 0.0
            for value in numeric
        ):
            reasons.append(f"invalid numeric summary value for {key}")
    missing = expected - set(actual)
    extra = set(actual) - expected
    if missing:
        reasons.append(f"missing {len(missing)} required matrix cells")
    if extra:
        reasons.append(f"found {len(extra)} unexpected matrix cells")
    return reasons


def classify_full(
    rows: Sequence[SummaryRow],
    width: int,
    threshold: int,
    *,
    expected_samples: int = FULL_SAMPLES,
    vectorized: bool,
    correctness: bool,
) -> tuple[str, list[str], str | None]:
    reasons = _integrity_reasons(
        rows, width, threshold, expected_samples
    )
    if not vectorized:
        reasons.append("required homogeneous superblocks were not widened")
    if not correctness:
        reasons.append("required correctness controls did not pass")
    if reasons:
        return "INCONCLUSIVE", reasons, None

    core = [
        row
        for row in rows
        if (
            row.workload in HOMOGENEOUS_WORKLOADS
            and row.candidate == "adaptive_srem"
            and row.active_lanes in {width // 2, width}
            and row.divergence_eighths in {0, 1}
        )
    ]
    sparse = [
        row
        for row in rows
        if (
            row.workload in HOMOGENEOUS_WORKLOADS
            and row.candidate == "adaptive_srem"
            and row.active_lanes == 1
        )
    ]
    divergent = [
        row
        for row in rows
        if (
            row.workload in HOMOGENEOUS_WORKLOADS
            and row.candidate == "adaptive_srem"
            and row.divergence_eighths == 4
        )
    ]
    remote = [
        row
        for row in rows
        if row.candidate == "remote_adaptive_srem"
    ]
    fairness = [
        row for row in rows if row.workload == MIXED_WORKLOAD
    ]
    controls_pass = (
        bool(sparse)
        and min(row.wall_speedup for row in sparse) >= 0.95
        and bool(divergent)
        and min(row.wall_speedup for row in divergent) >= 0.95
        and bool(remote)
        and min(row.wall_speedup for row in remote) >= 0.90
        and max(row.cpu_ratio for row in remote) <= 1.10
        and bool(fairness)
        and max(row.fairness_p99_ratio for row in fairness) <= 1.10
    )
    core_pass = (
        bool(core)
        and min(row.wall_speedup for row in core) >= 1.50
        and max(row.cpu_ratio for row in core) <= 0.70
    )
    if core_pass and controls_pass:
        return "CATEGORY", [], None

    envelopes: dict[
        tuple[int, int, int, int], list[SummaryRow]
    ] = {}
    for row in core:
        key = (
            row.active_lanes,
            row.frame_bytes,
            row.sites,
            row.divergence_eighths,
        )
        envelopes.setdefault(key, []).append(row)
    passing_envelopes: list[tuple[int, int, int, int]] = []
    if controls_pass:
        for key, envelope_rows in envelopes.items():
            if (
                {row.workload for row in envelope_rows}
                == set(HOMOGENEOUS_WORKLOADS)
                and min(row.wall_speedup for row in envelope_rows)
                >= 1.50
                and max(row.cpu_ratio for row in envelope_rows)
                <= 0.70
            ):
                passing_envelopes.append(key)
    if passing_envelopes:
        active, frame, sites, divergence = sorted(
            passing_envelopes
        )[0]
        envelope = (
            f"active={active} frame={frame} sites={sites} "
            f"divergence_eighths={divergence}"
        )
        return (
            "SPECIALIZED",
            ["full category envelope failed", f"passing envelope: {envelope}"],
            envelope,
        )

    failure_reasons = [
        "category wall floor "
        f"{min((row.wall_speedup for row in core), default=0.0):.6f}x",
        "category CPU ceiling "
        f"{max((row.cpu_ratio for row in core), default=math.inf):.6f}x",
        "sparse wall floor "
        f"{min((row.wall_speedup for row in sparse), default=0.0):.6f}x",
        "divergent wall floor "
        f"{min((row.wall_speedup for row in divergent), default=0.0):.6f}x",
        "remote wall floor "
        f"{min((row.wall_speedup for row in remote), default=0.0):.6f}x",
        "remote CPU ceiling "
        f"{max((row.cpu_ratio for row in remote), default=math.inf):.6f}x",
        "fairness ratio ceiling "
        f"{max((row.fairness_p99_ratio for row in fairness), default=math.inf):.6f}x",
    ]
    return "REJECT", failure_reasons, None


def benchmark_command(
    binary: Path,
    cell: MatrixCell,
    *,
    instances: int,
    min_mode_ms: int,
    warmup_rounds: int,
    order: str,
    owner_cpu: str,
) -> list[str]:
    if order not in {"ABBA", "BAAB"}:
        raise ValueError("invalid pair order")
    return [
        str(binary.resolve()),
        "--workload",
        cell.workload,
        "--pair",
        cell.candidate,
        "--instances",
        str(instances),
        "--frame-bytes",
        str(cell.frame_bytes),
        "--tile-width",
        str(cell.tile_width),
        "--active-lanes",
        str(cell.active_lanes),
        "--sites",
        str(cell.sites),
        "--divergence-eighths",
        str(cell.divergence_eighths),
        "--threshold",
        str(cell.threshold),
        "--producers",
        str(cell.producers),
        "--seed",
        str(cell.seed),
        "--min-mode-ms",
        str(min_mode_ms),
        "--order",
        order.lower(),
        "--warmup-rounds",
        str(warmup_rounds),
        "--owner-cpu",
        owner_cpu,
    ]


def run_matrix(
    binary: Path,
    cells: Sequence[MatrixCell],
    *,
    samples: int,
    instances: int,
    min_mode_ms: int,
    warmup_rounds: int,
    owner_cpu: str,
) -> list[SampleRow]:
    if samples <= 0 or samples % 2 == 0:
        raise ValueError("an odd positive process sample count is required")
    rows: list[SampleRow] = []
    timeout = max(30.0, min_mode_ms / 1000.0 * 12.0)

    for cell_index, cell in enumerate(cells, start=1):
        for process_sample in range(1, samples + 1):
            order = "ABBA" if process_sample % 2 == 1 else "BAAB"
            command = benchmark_command(
                binary,
                cell,
                instances=instances,
                min_mode_ms=min_mode_ms,
                warmup_rounds=warmup_rounds,
                order=order,
                owner_cpu=owner_cpu,
            )
            try:
                result = run_capture(command, timeout=timeout)
            except (OSError, ProcessTimeoutError) as exc:
                raise MatrixRunError(
                    f"cell {cell_index} sample {process_sample}: {exc}",
                    rows,
                ) from exc
            if result.returncode != 0 or result.stderr:
                raise MatrixRunError(
                    f"cell {cell_index} sample {process_sample} failed: "
                    f"rc={result.returncode} stderr={result.stderr!r}",
                    rows,
                )
            try:
                row = parse_output(result.stdout)
            except ValueError as exc:
                raise MatrixRunError(
                    f"cell {cell_index} sample {process_sample}: {exc}",
                    rows,
                ) from exc
            if (
                _pair_key(row) != _cell_key(cell)
                or row.instances != instances
                or row.min_mode_ns != min_mode_ms * 1_000_000
                or row.warmup_rounds != warmup_rounds
                or row.order != order
            ):
                raise MatrixRunError(
                    f"cell {cell_index} sample {process_sample}: "
                    "native row disagrees with command",
                    rows,
                )
            rows.append(
                SampleRow(process_sample=process_sample, row=row)
            )
    return rows


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
    selection: ScreeningSelection | None,
) -> str:
    lines = [
        "# SREM Phase 0 Results",
        "",
        f"- Phase: `{phase}`",
        f"- Verdict: `{verdict}`",
    ]
    if selection is not None:
        lines.extend(
            [
                f"- Selected width: `{selection.width}`",
                f"- Selected threshold: `{selection.threshold}`",
            ]
        )
        if (
            selection.wall_floor is not None
            and selection.cpu_ceiling is not None
        ):
            lines.extend(
                [
                    "- Screening wall floor: "
                    f"`{selection.wall_floor:.6f}x`",
                    "- Screening CPU ceiling: "
                    f"`{selection.cpu_ceiling:.6f}x`",
                ]
            )
    lines.extend(["", "## Reasons", ""])
    lines.extend(
        [f"- {reason}" for reason in reasons]
        or ["- All declared gates passed."]
    )
    lines.extend(
        [
            "",
            "## Summary",
            "",
            "| workload | candidate | width | active | frame | sites | div/8 | "
            "threshold | wall | CPU | wall spread | CPU spread | fairness |",
            "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for row in summaries:
        lines.append(
            f"| {row.workload} | {row.candidate} | {row.tile_width} | "
            f"{row.active_lanes} | {row.frame_bytes} | {row.sites} | "
            f"{row.divergence_eighths} | {row.threshold} | "
            f"{row.wall_speedup:.6f}x | {row.cpu_ratio:.6f}x | "
            f"{row.wall_ratio_spread:.6f}x | "
            f"{row.cpu_ratio_spread:.6f}x | "
            f"{row.fairness_p99_ratio:.6f}x |"
        )
    lines.extend(
        [
            "",
            "This is a standalone cost-model result, not production-runtime "
            "validation.",
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
    selection: ScreeningSelection | None,
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
        selection=selection,
    )
    complete_metadata = {
        **metadata,
        "phase": phase,
        "verdict": verdict,
        "reasons": list(reasons),
        "selection": (
            None if selection is None else asdict(selection)
        ),
        "sample_rows": len(samples),
        "summary_rows": len(summaries),
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "python": sys.version.split()[0],
        "platform": platform.platform(),
    }
    payloads = {
        output_dir / "srem_phase0_samples.csv": _csv_text(sample_dicts),
        output_dir / "srem_phase0_summary.csv": _csv_text(summary_dicts),
        output_dir / "srem_phase0_metadata.json": (
            json.dumps(
                complete_metadata,
                indent=2,
                sort_keys=True,
            )
            + "\n"
        ),
        output_dir / "srem_phase0_report.md": report,
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
        description="Run and classify the paired SREM Phase 0 model."
    )
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument(
        "--phase",
        choices=("quick", "screen", "gate"),
        default="screen",
    )
    parser.add_argument("--samples", type=_positive_int)
    parser.add_argument("--instances", type=_positive_int, default=65_536)
    parser.add_argument("--min-mode-ms", type=_positive_int)
    parser.add_argument("--warmup-rounds", type=_positive_int, default=7)
    parser.add_argument("--owner-cpu", default="none")
    parser.add_argument("--selected-width", type=int)
    parser.add_argument("--threshold", type=int)
    parser.add_argument(
        "--vectorized",
        choices=("yes", "no"),
        default="no",
    )
    parser.add_argument(
        "--correctness",
        choices=("yes", "no"),
        default="no",
    )
    parser.add_argument("--output-dir", type=Path)
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
        print("[bench_srem_model.py] binary does not exist", file=sys.stderr)
        return 2

    if args.phase == "quick":
        width = args.selected_width or 16
        threshold = args.threshold or width // 2
        cells = quick_matrix(width, threshold)
        samples = args.samples or 1
        min_mode_ms = args.min_mode_ms or 2
    elif args.phase == "screen":
        cells = screening_matrix()
        samples = args.samples or SCREEN_SAMPLES
        min_mode_ms = args.min_mode_ms or (
            SCREEN_MIN_MODE_NS // 1_000_000
        )
    else:
        if args.selected_width is None or args.threshold is None:
            print(
                "[bench_srem_model.py] gate requires selected width "
                "and threshold",
                file=sys.stderr,
            )
            return 2
        cells = full_matrix(args.selected_width, args.threshold)
        samples = args.samples or FULL_SAMPLES
        min_mode_ms = args.min_mode_ms or (
            FULL_MIN_MODE_NS // 1_000_000
        )

    try:
        raw_rows = run_matrix(
            args.binary,
            cells,
            samples=samples,
            instances=args.instances,
            min_mode_ms=min_mode_ms,
            warmup_rounds=args.warmup_rounds,
            owner_cpu=args.owner_cpu,
        )
        summaries = summarize(raw_rows)
    except (ValueError, MatrixRunError) as exc:
        print(f"[bench_srem_model.py] {exc}", file=sys.stderr)
        return 2

    selection: ScreeningSelection | None = None
    reasons: list[str] = []
    if args.phase == "quick":
        verdict = "SMOKE_ONLY"
        reasons = ["quick mode cannot produce a research verdict"]
    elif args.phase == "screen":
        try:
            selection = select_screening(summaries)
            bound = screen_candidate_bound(summaries, selection)
        except ValueError as exc:
            verdict = "INCONCLUSIVE"
            reasons = [str(exc)]
        else:
            verdict = (
                "SCREEN_CONTINUE"
                if bound.can_reach_category
                else "SCREEN_STOP"
            )
            reasons = list(bound.details)
    else:
        selection = ScreeningSelection(
            width=args.selected_width,
            threshold=args.threshold,
            wall_floor=None,
            cpu_ceiling=None,
        )
        verdict, reasons, envelope = classify_full(
            summaries,
            args.selected_width,
            args.threshold,
            expected_samples=samples,
            vectorized=args.vectorized == "yes",
            correctness=args.correctness == "yes",
        )
        if envelope is not None:
            reasons.append(f"specialized envelope: {envelope}")

    output_dir = args.output_dir or Path(
        f"object/srem-phase0-{args.phase}"
    )
    command_text = shlex.join(
        [sys.executable, str(Path(__file__)), *(argv or sys.argv[1:])]
    )
    write_evidence(
        output_dir,
        args.tracked_report,
        raw_rows,
        summaries,
        phase=args.phase,
        verdict=verdict,
        reasons=reasons,
        selection=selection,
        metadata={
            "command": command_text,
            "binary": str(args.binary),
            "source_commit": _source_commit(),
            "instances": args.instances,
            "samples": samples,
            "min_mode_ms": min_mode_ms,
            "warmup_rounds": args.warmup_rounds,
            "owner_cpu": args.owner_cpu,
            "vectorized": args.vectorized,
            "correctness": args.correctness,
        },
    )
    print(
        f"[bench_srem_model.py] phase={args.phase} "
        f"verdict={verdict} samples={len(raw_rows)} "
        f"summary={len(summaries)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
