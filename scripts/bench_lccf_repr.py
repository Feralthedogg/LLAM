#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence


SCHEMA_VERSION = 1
REPRESENTATION_BYTES = {"A": 0, "B": 48, "C": 64}
CONTRAST_REPRESENTATIONS = {
    "A/B": ("A", "B"),
    "A/C": ("A", "C"),
    "B/C": ("B", "C"),
}
WORKLOADS = frozenset({
    "completion_io_pipeline",
    "completion_rpc_state",
    "completion_timer_cancel",
    "completion_mixed_fairness",
})
ROUTES = frozenset({"queue", "fused", "mixed"})
ORDERS = frozenset({"ABBA", "BAAB"})

TOP_FIELDS = frozenset({
    "schema_version",
    "process_id",
    "cell",
    "contrast",
    "block",
    "pair",
    "order",
    "minimum_window_ns",
    "rounds",
    "seed",
    "left",
    "right",
})
CELL_FIELDS = frozenset({
    "workload",
    "route",
    "frame_bytes",
    "instances",
    "sites",
    "chain",
})
SIDE_STRING_FIELDS = frozenset({"representation", "checksum"})
SIDE_BOOL_FIELDS = frozenset({
    "refs_balanced",
    "queue_balanced",
    "overflow_balanced",
    "module_balanced",
    "payload_balanced",
    "backend_balanced",
    "tickets_balanced",
})
SIDE_INT_FIELDS = frozenset({
    "wall_ns",
    "cpu_ns",
    "p50_ns",
    "p99_ns",
    "operations",
    "callbacks",
    "completions",
    "claims",
    "stale_tickets",
    "queue_pushes",
    "queue_pops",
    "resume_calls",
    "direct_calls",
    "forced_escapes",
    "hot_allocations",
    "facts_attempted",
    "facts_built",
    "facts_build_failed",
    "fact_normalizations",
    "fact_site_lookups",
    "fact_changed_site_materializations",
    "fact_module_pins",
    "fact_payload_pins",
    "fact_stale_losers",
    "fact_guard_rechecks",
    "fact_queue_forwards",
    "fact_generation_mismatches",
    "fact_reuse_delays",
    "fact_hot_bytes",
    "fact_sidecar_bytes",
    "fact_overflow_pushes",
    "fact_overflow_pops",
})
SIDE_FIELDS = SIDE_STRING_FIELDS | SIDE_BOOL_FIELDS | SIDE_INT_FIELDS

COMMON_LOGICAL_FIELDS = (
    "checksum",
    "operations",
    "callbacks",
    "completions",
    "claims",
    "stale_tickets",
    "queue_pushes",
    "queue_pops",
    "resume_calls",
    "direct_calls",
    "forced_escapes",
    "hot_allocations",
    "facts_attempted",
    "facts_built",
    "facts_build_failed",
    "fact_changed_site_materializations",
    "fact_module_pins",
    "fact_payload_pins",
    "fact_stale_losers",
    "fact_guard_rechecks",
    "fact_queue_forwards",
    "fact_generation_mismatches",
    "fact_reuse_delays",
    "fact_hot_bytes",
    "fact_overflow_pushes",
    "fact_overflow_pops",
    "refs_balanced",
    "queue_balanced",
    "overflow_balanced",
    "module_balanced",
    "payload_balanced",
    "backend_balanced",
    "tickets_balanced",
)


@dataclass(frozen=True, order=True)
class Cell:
    workload: str
    route: str
    frame_bytes: int
    instances: int
    sites: int
    chain: int


@dataclass(frozen=True)
class Side:
    representation: str
    wall_ns: int
    cpu_ns: int
    p50_ns: int
    p99_ns: int
    checksum: str
    operations: int
    callbacks: int
    completions: int
    claims: int
    stale_tickets: int
    queue_pushes: int
    queue_pops: int
    resume_calls: int
    direct_calls: int
    forced_escapes: int
    hot_allocations: int
    facts_attempted: int
    facts_built: int
    facts_build_failed: int
    fact_normalizations: int
    fact_site_lookups: int
    fact_changed_site_materializations: int
    fact_module_pins: int
    fact_payload_pins: int
    fact_stale_losers: int
    fact_guard_rechecks: int
    fact_queue_forwards: int
    fact_generation_mismatches: int
    fact_reuse_delays: int
    fact_hot_bytes: int
    fact_sidecar_bytes: int
    fact_overflow_pushes: int
    fact_overflow_pops: int
    refs_balanced: bool
    queue_balanced: bool
    overflow_balanced: bool
    module_balanced: bool
    payload_balanced: bool
    backend_balanced: bool
    tickets_balanced: bool


@dataclass(frozen=True)
class RawPair:
    schema_version: int
    process_id: int
    cell: Cell
    contrast: str
    block: int
    pair: int
    order: str
    minimum_window_ns: int
    rounds: int
    seed: int
    left: Side
    right: Side

    @property
    def wall_speedup(self) -> float:
        return _finite_ratio(self.left.wall_ns, self.right.wall_ns)

    @property
    def cpu_ratio(self) -> float:
        return _finite_ratio(self.right.cpu_ns, self.left.cpu_ns)

    @property
    def p99_ratio(self) -> float:
        return _finite_ratio(self.right.p99_ns, self.left.p99_ns)


@dataclass(frozen=True)
class ProcessMedian:
    process_id: int
    cell: Cell
    contrast: str
    rounds: int
    pair_count: int
    wall_speedup: float
    cpu_ratio: float
    p99_ratio: float


@dataclass(frozen=True)
class Interval:
    lower: float
    upper: float


@dataclass(frozen=True)
class ContrastAssessment:
    contrast: str
    status: str
    reasons: tuple[str, ...]
    intervals: dict[str, Interval]


@dataclass(frozen=True)
class Decision:
    status: str
    selected_representation: str | None
    b_eligibility: str
    c_eligibility: str
    reasons: tuple[str, ...]
    intervals: dict[str, Interval]


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _reject_constant(value: str) -> Any:
    raise ValueError(f"non-finite JSON constant: {value}")


def _exact_fields(value: Any, expected: frozenset[str], label: str) -> None:
    if not isinstance(value, dict) or set(value) != expected:
        actual = set(value) if isinstance(value, dict) else set()
        raise ValueError(
            f"{label} schema mismatch: "
            f"missing={sorted(expected - actual)} "
            f"extra={sorted(actual - expected)}"
        )


def _integer(value: Any, label: str, *, positive: bool = False) -> int:
    if type(value) is not int or value < (1 if positive else 0):
        qualifier = "positive" if positive else "non-negative"
        raise ValueError(f"{label} must be a {qualifier} integer")
    return value


def _string(value: Any, label: str) -> str:
    if type(value) is not str:
        raise ValueError(f"{label} must be a string")
    return value


def _finite_ratio(numerator: int, denominator: int) -> float:
    if numerator <= 0 or denominator <= 0:
        raise ValueError("ratio operands must be positive")
    try:
        value = numerator / denominator
    except OverflowError as exc:
        raise ValueError("ratio overflow") from exc
    if not math.isfinite(value) or value <= 0.0:
        raise ValueError("ratio must be finite and positive")
    return value


def _parse_cell(value: Any) -> Cell:
    _exact_fields(value, CELL_FIELDS, "cell")
    workload = _string(value["workload"], "cell.workload")
    route = _string(value["route"], "cell.route")
    frame_bytes = _integer(value["frame_bytes"], "cell.frame_bytes", positive=True)
    instances = _integer(value["instances"], "cell.instances", positive=True)
    sites = _integer(value["sites"], "cell.sites", positive=True)
    chain = _integer(value["chain"], "cell.chain", positive=True)
    if workload not in WORKLOADS:
        raise ValueError("unknown workload")
    if route not in ROUTES:
        raise ValueError("unknown route family")
    if frame_bytes not in (64, 256):
        raise ValueError("unsupported representation frame size")
    if sites != 8:
        raise ValueError("representation evidence requires eight sites")
    return Cell(workload, route, frame_bytes, instances, sites, chain)


def _parse_side(value: Any, label: str) -> Side:
    _exact_fields(value, SIDE_FIELDS, label)
    parsed: dict[str, Any] = {}
    for field in SIDE_STRING_FIELDS:
        parsed[field] = _string(value[field], f"{label}.{field}")
    for field in SIDE_BOOL_FIELDS:
        if type(value[field]) is not bool:
            raise ValueError(f"{label}.{field} must be a boolean")
        parsed[field] = value[field]
    for field in SIDE_INT_FIELDS:
        parsed[field] = _integer(value[field], f"{label}.{field}")
    representation = parsed["representation"]
    if representation not in REPRESENTATION_BYTES:
        raise ValueError("unknown representation")
    checksum = parsed["checksum"]
    if len(checksum) != 16 or any(
        character not in "0123456789abcdef" for character in checksum
    ):
        raise ValueError("checksum must be 16 lowercase hexadecimal digits")
    for field in ("wall_ns", "cpu_ns", "p50_ns", "p99_ns"):
        if parsed[field] <= 0:
            raise ValueError(f"{label}.{field} must be positive")
    if parsed["p50_ns"] > parsed["p99_ns"] or \
            parsed["p99_ns"] > parsed["wall_ns"]:
        raise ValueError(f"{label} latency quantiles are inconsistent")
    return Side(**parsed)


def _materialization_count(cell: Cell, side: Side) -> int:
    if cell.route == "queue":
        return side.callbacks + side.completions
    if cell.route == "mixed":
        return side.callbacks + side.forced_escapes
    return side.callbacks


def _validate_side(
    side: Side,
    *,
    cell: Cell,
    rounds: int,
    minimum_window_ns: int,
) -> None:
    expected_completions = rounds * cell.instances
    expected_callbacks = expected_completions * cell.chain
    expected_attempts = expected_completions * (
        3 if cell.workload == "completion_timer_cancel" else 1
    )
    expected_stale = expected_attempts - expected_completions
    materializations = _materialization_count(cell, side)

    if side.wall_ns < minimum_window_ns:
        raise ValueError("measured duration is below the declared minimum")
    if side.operations != expected_callbacks or \
            side.callbacks != expected_callbacks or \
            side.completions != expected_completions or \
            side.claims != expected_completions or \
            side.resume_calls != expected_callbacks:
        raise ValueError("operation, completion, or callback count mismatch")
    if side.facts_attempted != expected_attempts or \
            side.facts_built != expected_completions or \
            side.stale_tickets != expected_stale or \
            side.fact_stale_losers != expected_stale:
        raise ValueError("fact claim or stale-ticket count mismatch")
    if side.facts_build_failed != 0 or side.hot_allocations != 0 or \
            side.fact_queue_forwards != 0 or \
            side.fact_generation_mismatches != 0 or \
            side.fact_reuse_delays != 0:
        raise ValueError("fact lifecycle failure counter is nonzero")
    if side.fact_module_pins != expected_completions or \
            side.fact_payload_pins != expected_completions:
        raise ValueError("module or payload lifetime count mismatch")
    if side.queue_pushes != side.queue_pops or \
            side.fact_overflow_pushes != side.fact_overflow_pops:
        raise ValueError("queue or overflow ownership is unbalanced")
    if side.direct_calls + side.queue_pops != expected_callbacks:
        raise ValueError("callback routing count mismatch")
    if cell.route == "queue" and (
        side.direct_calls != 0 or side.forced_escapes != 0
    ):
        raise ValueError("queue route counters are inconsistent")
    if cell.route == "fused" and (
        side.direct_calls != expected_callbacks or
        side.queue_pops != 0 or side.forced_escapes != 0
    ):
        raise ValueError("fused route counters are inconsistent")
    if cell.route == "mixed" and (
        side.forced_escapes <= 0 or
        side.queue_pops != side.forced_escapes * cell.chain
    ):
        raise ValueError("mixed route counters are inconsistent")
    if side.fact_guard_rechecks != materializations:
        raise ValueError("materialization/guard count mismatch")
    if side.fact_changed_site_materializations < 0 or \
            side.fact_changed_site_materializations > \
            expected_callbacks - expected_completions:
        raise ValueError("changed-site materialization count is invalid")
    if side.fact_hot_bytes != 64 or \
            side.fact_sidecar_bytes != REPRESENTATION_BYTES[side.representation]:
        raise ValueError("representation storage byte count mismatch")
    if not all(getattr(side, field) for field in SIDE_BOOL_FIELDS):
        raise ValueError("reference-balance assertion failed")

    if side.representation == "A":
        expected_normalizations = materializations
        expected_lookups = materializations
    elif side.representation == "B":
        expected_normalizations = expected_completions
        expected_lookups = materializations
    else:
        expected_normalizations = expected_completions
        expected_lookups = (
            expected_completions + side.fact_changed_site_materializations
        )
    if side.fact_normalizations != expected_normalizations or \
            side.fact_site_lookups != expected_lookups:
        raise ValueError("representation work count mismatch")


def _validate_pair(row: RawPair) -> None:
    expected_representations = CONTRAST_REPRESENTATIONS.get(row.contrast)
    if expected_representations is None:
        raise ValueError("unknown representation contrast")
    if (row.left.representation, row.right.representation) != \
            expected_representations:
        raise ValueError("contrast side representation mismatch")
    _validate_side(
        row.left,
        cell=row.cell,
        rounds=row.rounds,
        minimum_window_ns=row.minimum_window_ns,
    )
    _validate_side(
        row.right,
        cell=row.cell,
        rounds=row.rounds,
        minimum_window_ns=row.minimum_window_ns,
    )
    for field in COMMON_LOGICAL_FIELDS:
        if getattr(row.left, field) != getattr(row.right, field):
            raise ValueError(f"paired logical metric mismatch: {field}")
    _ = row.wall_speedup
    _ = row.cpu_ratio
    _ = row.p99_ratio


def parse_raw_row(text: str) -> RawPair:
    try:
        value = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_constant=_reject_constant,
        )
    except (json.JSONDecodeError, ValueError) as exc:
        raise ValueError("malformed representation JSON row") from exc
    _exact_fields(value, TOP_FIELDS, "row")
    schema_version = _integer(value["schema_version"], "schema_version")
    process_id = _integer(value["process_id"], "process_id")
    block = _integer(value["block"], "block")
    pair = _integer(value["pair"], "pair")
    minimum_window_ns = _integer(
        value["minimum_window_ns"], "minimum_window_ns", positive=True
    )
    rounds = _integer(value["rounds"], "rounds", positive=True)
    seed = _integer(value["seed"], "seed")
    contrast = _string(value["contrast"], "contrast")
    order = _string(value["order"], "order")
    if schema_version != SCHEMA_VERSION:
        raise ValueError("unsupported representation schema version")
    if pair not in (0, 1):
        raise ValueError("pair index must be zero or one")
    if order not in ORDERS:
        raise ValueError("unknown paired execution order")
    row = RawPair(
        schema_version=schema_version,
        process_id=process_id,
        cell=_parse_cell(value["cell"]),
        contrast=contrast,
        block=block,
        pair=pair,
        order=order,
        minimum_window_ns=minimum_window_ns,
        rounds=rounds,
        seed=seed,
        left=_parse_side(value["left"], "left"),
        right=_parse_side(value["right"], "right"),
    )
    _validate_pair(row)
    return row


def collapse_process_medians(rows: Sequence[RawPair]) -> list[ProcessMedian]:
    grouped: dict[tuple[int, Cell, str], list[RawPair]] = {}
    for row in rows:
        _validate_pair(row)
        grouped.setdefault(
            (row.process_id, row.cell, row.contrast), []
        ).append(row)
    result: list[ProcessMedian] = []
    for (process_id, cell, contrast), group in grouped.items():
        if len(group) != 8:
            raise ValueError("each process/cell/contrast requires eight pairs")
        blocks: dict[int, list[RawPair]] = {}
        identities: set[tuple[int, int]] = set()
        for row in group:
            identity = (row.block, row.pair)
            if identity in identities:
                raise ValueError("duplicate block/pair identity")
            identities.add(identity)
            blocks.setdefault(row.block, []).append(row)
        if sorted(blocks) != [0, 1, 2, 3]:
            raise ValueError("process evidence requires four numbered blocks")
        block_orders: list[str] = []
        for block in sorted(blocks):
            block_rows = blocks[block]
            if sorted(row.pair for row in block_rows) != [0, 1] or \
                    len({row.order for row in block_rows}) != 1:
                raise ValueError("block pair/order structure is invalid")
            block_orders.append(block_rows[0].order)
        if block_orders.count("ABBA") != 2 or \
                block_orders.count("BAAB") != 2:
            raise ValueError("ABBA/BAAB block order is unbalanced")
        if len({row.rounds for row in group}) != 1 or \
                len({row.seed for row in group}) != 1:
            raise ValueError("round count or seed changed within a process")
        result.append(ProcessMedian(
            process_id=process_id,
            cell=cell,
            contrast=contrast,
            rounds=group[0].rounds,
            pair_count=len(group),
            wall_speedup=statistics.median(
                row.wall_speedup for row in group
            ),
            cpu_ratio=statistics.median(row.cpu_ratio for row in group),
            p99_ratio=statistics.median(row.p99_ratio for row in group),
        ))
    return sorted(
        result,
        key=lambda value: (
            value.contrast,
            value.cell.route,
            value.cell.workload,
            value.cell.frame_bytes,
            value.process_id,
        ),
    )


def bootstrap_median_ci(
    values: Sequence[float],
    *,
    seed: int,
    resamples: int = 20_000,
) -> tuple[float, float]:
    if not values or resamples <= 0 or any(
        not math.isfinite(value) or value <= 0.0 for value in values
    ):
        raise ValueError("bootstrap inputs must be finite and positive")
    rng = random.Random(seed)
    medians = [
        statistics.median(
            values[rng.randrange(len(values))] for _ in values
        )
        for _ in range(resamples)
    ]
    medians.sort()
    lower_index = int(0.025 * resamples)
    upper_index = int(0.975 * resamples) - 1
    if upper_index < lower_index:
        raise ValueError("bootstrap resample count is too small")
    return medians[lower_index], medians[upper_index]


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def derived_seed(seed: int, key: str) -> int:
    if seed < 0:
        raise ValueError("seed must be non-negative")
    return fnv1a64(seed.to_bytes(8, "little", signed=False) + key.encode("utf-8"))


def _interval(
    values: Sequence[float],
    *,
    seed: int,
    key: str,
    resamples: int,
) -> Interval:
    lower, upper = bootstrap_median_ci(
        values,
        seed=derived_seed(seed, key),
        resamples=resamples,
    )
    return Interval(lower, upper)


def _lower_bound_gate(interval: Interval, threshold: float) -> str:
    if interval.lower >= threshold:
        return "PASS"
    if interval.upper < threshold:
        return "FAIL"
    return "INCONCLUSIVE"


def _upper_bound_gate(interval: Interval, threshold: float) -> str:
    if interval.upper <= threshold:
        return "PASS"
    if interval.lower > threshold:
        return "FAIL"
    return "INCONCLUSIVE"


def _route_intervals(
    medians: Sequence[ProcessMedian],
    *,
    contrast: str,
    seed: int,
    resamples: int,
) -> dict[str, Interval]:
    selected = [value for value in medians if value.contrast == contrast]
    by_route: dict[str, list[ProcessMedian]] = {}
    for value in selected:
        by_route.setdefault(value.cell.route, []).append(value)
    if set(by_route) != ROUTES or any(not values for values in by_route.values()):
        raise ValueError(f"contrast {contrast} lacks a complete route matrix")
    intervals: dict[str, Interval] = {}
    for route in sorted(ROUTES):
        for metric in ("wall_speedup", "cpu_ratio", "p99_ratio"):
            key = f"{contrast}|{route}|{metric}"
            intervals[f"{route}.{metric}"] = _interval(
                [getattr(value, metric) for value in by_route[route]],
                seed=seed,
                key=key,
                resamples=resamples,
            )
    return intervals


def assess_eligibility(
    medians: Sequence[ProcessMedian],
    *,
    contrast: str,
    seed: int,
    resamples: int = 20_000,
) -> ContrastAssessment:
    if contrast not in ("A/B", "A/C"):
        raise ValueError("eligibility contrast must compare against A")
    intervals = _route_intervals(
        medians,
        contrast=contrast,
        seed=seed,
        resamples=resamples,
    )
    gates = {
        "queue wall": _lower_bound_gate(
            intervals["queue.wall_speedup"], 0.98
        ),
        "fused wall": _lower_bound_gate(
            intervals["fused.wall_speedup"], 1.0 / 1.02
        ),
        "mixed CPU": _upper_bound_gate(
            intervals["mixed.cpu_ratio"], 0.95
        ),
    }
    for route in ROUTES:
        gates[f"{route} p99"] = _upper_bound_gate(
            intervals[f"{route}.p99_ratio"], 1.05
        )
    failed = sorted(name for name, status in gates.items() if status == "FAIL")
    uncertain = sorted(
        name for name, status in gates.items() if status == "INCONCLUSIVE"
    )
    if failed:
        status = "INELIGIBLE"
        reasons = tuple(f"{name} gate failed" for name in failed)
    elif uncertain:
        status = "INCONCLUSIVE"
        reasons = tuple(f"{name} confidence interval crosses gate" for name in uncertain)
    else:
        status = "ELIGIBLE"
        reasons = ()
    return ContrastAssessment(contrast, status, reasons, intervals)


def _validate_matrix(medians: Sequence[ProcessMedian]) -> None:
    contrasts = {value.contrast for value in medians}
    if contrasts != set(CONTRAST_REPRESENTATIONS):
        raise ValueError("evidence requires A/B, A/C, and B/C contrasts")
    identity_sets = {
        contrast: {
            (value.process_id, value.cell)
            for value in medians
            if value.contrast == contrast
        }
        for contrast in contrasts
    }
    first = identity_sets["A/B"]
    if not first or any(values != first for values in identity_sets.values()):
        raise ValueError("contrast process/cell coverage differs")
    cells = {cell for _, cell in first}
    configurations = {
        (cell.instances, cell.sites, cell.chain) for cell in cells
    }
    if len(configurations) != 1:
        raise ValueError("cell instance/site/chain configuration differs")
    instances, sites, chain = next(iter(configurations))
    expected_cells = {
        Cell(workload, route, frame_bytes, instances, sites, chain)
        for workload in WORKLOADS
        for route in ROUTES
        for frame_bytes in (64, 256)
    }
    if cells != expected_cells:
        raise ValueError("evidence does not cover the declared 24-cell matrix")
    for contrast in CONTRAST_REPRESENTATIONS:
        for cell in expected_cells:
            processes = {
                value.process_id
                for value in medians
                if value.contrast == contrast and value.cell == cell
            }
            if len(processes) != 5:
                raise ValueError(
                    "each contrast/cell requires five process repetitions"
                )


def _assess_c_over_b(
    medians: Sequence[ProcessMedian],
    *,
    seed: int,
    resamples: int,
) -> ContrastAssessment:
    intervals = _route_intervals(
        medians,
        contrast="B/C",
        seed=seed,
        resamples=resamples,
    )
    overall_cpu = _interval(
        [value.cpu_ratio for value in medians if value.contrast == "B/C"],
        seed=seed,
        key="B/C|overall|cpu_ratio",
        resamples=resamples,
    )
    intervals["overall.cpu_ratio"] = overall_cpu
    gates = {
        "overall CPU": _upper_bound_gate(overall_cpu, 0.98),
    }
    for route in ROUTES:
        gates[f"{route} wall"] = _lower_bound_gate(
            intervals[f"{route}.wall_speedup"], 0.98
        )
        gates[f"{route} p99"] = _upper_bound_gate(
            intervals[f"{route}.p99_ratio"], 1.05
        )
    failed = sorted(name for name, status in gates.items() if status == "FAIL")
    uncertain = sorted(
        name for name, status in gates.items() if status == "INCONCLUSIVE"
    )
    if failed:
        return ContrastAssessment(
            "B/C",
            "INELIGIBLE",
            tuple(f"{name} gate failed" for name in failed),
            intervals,
        )
    if uncertain:
        return ContrastAssessment(
            "B/C",
            "INCONCLUSIVE",
            tuple(
                f"{name} confidence interval crosses gate"
                for name in uncertain
            ),
            intervals,
        )
    return ContrastAssessment("B/C", "ELIGIBLE", (), intervals)


def classify_evidence(
    rows: Sequence[RawPair],
    *,
    seed: int,
    resamples: int = 20_000,
) -> Decision:
    medians = collapse_process_medians(rows)
    _validate_matrix(medians)
    b = assess_eligibility(
        medians,
        contrast="A/B",
        seed=seed,
        resamples=resamples,
    )
    c = assess_eligibility(
        medians,
        contrast="A/C",
        seed=seed,
        resamples=resamples,
    )
    intervals = {
        f"A/B.{key}": value for key, value in b.intervals.items()
    }
    intervals.update({
        f"A/C.{key}": value for key, value in c.intervals.items()
    })
    reasons = list(b.reasons + c.reasons)

    if b.status == "INELIGIBLE" and c.status == "INELIGIBLE":
        return Decision(
            "SELECT_A", "A", b.status, c.status,
            tuple(reasons), intervals,
        )
    if b.status == "ELIGIBLE" and c.status == "INELIGIBLE":
        return Decision(
            "SELECT_B", "B", b.status, c.status,
            tuple(reasons), intervals,
        )
    if b.status == "INELIGIBLE" and c.status == "ELIGIBLE":
        return Decision(
            "SELECT_C", "C", b.status, c.status,
            tuple(reasons), intervals,
        )
    if b.status != "ELIGIBLE" or c.status != "ELIGIBLE":
        return Decision(
            "INCONCLUSIVE", None, b.status, c.status,
            tuple(reasons), intervals,
        )

    c_over_b = _assess_c_over_b(
        medians,
        seed=seed,
        resamples=resamples,
    )
    intervals.update({
        f"B/C.{key}": value
        for key, value in c_over_b.intervals.items()
    })
    reasons.extend(c_over_b.reasons)
    if c_over_b.status == "ELIGIBLE":
        return Decision(
            "SELECT_C", "C", b.status, c.status,
            tuple(reasons), intervals,
        )
    if c_over_b.status == "INELIGIBLE":
        return Decision(
            "SELECT_B", "B", b.status, c.status,
            tuple(reasons), intervals,
        )
    return Decision(
        "INCONCLUSIVE", None, b.status, c.status,
        tuple(reasons), intervals,
    )


def classify_lines(
    lines: Iterable[str],
    *,
    seed: int,
    resamples: int = 20_000,
) -> Decision:
    try:
        rows = [parse_raw_row(line) for line in lines]
        return classify_evidence(rows, seed=seed, resamples=resamples)
    except (ValueError, OverflowError) as exc:
        return Decision(
            status="REJECT",
            selected_representation=None,
            b_eligibility="REJECTED",
            c_eligibility="REJECTED",
            reasons=(str(exc),),
            intervals={},
        )


def _cell_slug(cell: Cell) -> str:
    return (
        f"{cell.workload}__{cell.route}__f{cell.frame_bytes}"
        f"__i{cell.instances}__s{cell.sites}__c{cell.chain}"
    )


def _binary_command(
    binary: Path,
    cell: Cell,
    *,
    process_id: int,
    blocks: int,
    minimum_window_ns: int,
    warmup_rounds: int,
    seed: int,
) -> list[str]:
    return [
        str(binary),
        "--workload", cell.workload,
        "--route", cell.route,
        "--process-id", str(process_id),
        "--instances", str(cell.instances),
        "--frame-bytes", str(cell.frame_bytes),
        "--sites", str(cell.sites),
        "--chain", str(cell.chain),
        "--blocks", str(blocks),
        "--minimum-window-ns", str(minimum_window_ns),
        "--warmup-rounds", str(warmup_rounds),
        "--seed", str(seed),
    ]


def _run_binary(
    command: Sequence[str],
    *,
    timeout: float,
    expected_rows: int,
) -> list[str]:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"representation process failed ({completed.returncode}): "
            f"{' '.join(command)}\nstdout={completed.stdout}\n"
            f"stderr={completed.stderr}"
        )
    if completed.stderr:
        raise RuntimeError(
            f"representation process wrote stderr: {completed.stderr}"
        )
    lines = completed.stdout.splitlines()
    if len(lines) != expected_rows:
        raise RuntimeError(
            f"representation process emitted {len(lines)} rows; "
            f"expected {expected_rows}"
        )
    return lines


def _summary_payload(
    decision: Decision,
    *,
    configuration: dict[str, Any],
    raw_rows: int,
    process_medians: int,
) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "decision": decision.status,
        "selected_representation": decision.selected_representation,
        "eligibility": {
            "B": decision.b_eligibility,
            "C": decision.c_eligibility,
        },
        "reasons": list(decision.reasons),
        "intervals": {
            key: {"lower": value.lower, "upper": value.upper}
            for key, value in sorted(decision.intervals.items())
        },
        "counts": {
            "cells": 24,
            "processes": 120,
            "raw_pair_rows": raw_rows,
            "process_medians": process_medians,
        },
        "configuration": configuration,
    }


def _summary_json(payload: dict[str, Any]) -> str:
    return json.dumps(payload, indent=2, sort_keys=True) + "\n"


def _summary_csv(decision: Decision) -> str:
    from io import StringIO

    stream = StringIO(newline="")
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(("interval", "lower", "upper"))
    for key, value in sorted(decision.intervals.items()):
        writer.writerow((key, repr(value.lower), repr(value.upper)))
    return stream.getvalue()


def _read_raw_lines(raw_dir: Path) -> list[str]:
    paths = sorted(raw_dir.glob("*.jsonl"))
    if len(paths) != 24:
        raise ValueError("raw evidence directory must contain 24 cell files")
    lines: list[str] = []
    for path in paths:
        lines.extend(path.read_text(encoding="utf-8").splitlines())
    return lines


def run_evidence(args: argparse.Namespace) -> dict[str, Any]:
    binary = args.binary.resolve()
    output_dir = args.output_dir.resolve()
    if not binary.is_file():
        raise FileNotFoundError(f"benchmark binary not found: {binary}")
    raw_dir = output_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    cells = [
        Cell(workload, route, frame_bytes,
             args.instances, 8, args.chain)
        for workload in sorted(WORKLOADS)
        for route in sorted(ROUTES)
        for frame_bytes in args.frame_bytes
    ]
    all_rows: list[RawPair] = []
    raw_line_count = 0
    expected_rows = len(CONTRAST_REPRESENTATIONS) * args.blocks * 2
    for cell_index, cell in enumerate(cells):
        raw_path = raw_dir / f"{_cell_slug(cell)}.jsonl"
        with raw_path.open("w", encoding="utf-8", newline="\n") as output:
            for process_id in range(args.samples):
                process_seed = derived_seed(
                    args.seed,
                    f"{_cell_slug(cell)}|process={process_id}",
                )
                command = _binary_command(
                    binary,
                    cell,
                    process_id=process_id,
                    blocks=args.blocks,
                    minimum_window_ns=args.minimum_window_ms * 1_000_000,
                    warmup_rounds=args.warmup_rounds,
                    seed=process_seed,
                )
                lines = _run_binary(
                    command,
                    timeout=args.timeout,
                    expected_rows=expected_rows,
                )
                for line in lines:
                    row = parse_raw_row(line)
                    if row.process_id != process_id or row.cell != cell or \
                            row.seed != process_seed:
                        raise ValueError(
                            "binary row identity differs from requested cell"
                        )
                    all_rows.append(row)
                    output.write(line + "\n")
                    raw_line_count += 1
        print(
            f"[{cell_index + 1}/{len(cells)}] {_cell_slug(cell)} complete",
            flush=True,
        )
    decision = classify_evidence(
        all_rows,
        seed=args.seed,
        resamples=args.resamples,
    )
    process_medians = collapse_process_medians(all_rows)
    configuration = {
        "seed": args.seed,
        "resamples": args.resamples,
        "samples": args.samples,
        "blocks": args.blocks,
        "minimum_window_ms": args.minimum_window_ms,
        "instances": args.instances,
        "chain": args.chain,
        "frame_bytes": list(args.frame_bytes),
        "warmup_rounds": args.warmup_rounds,
    }
    payload = _summary_payload(
        decision,
        configuration=configuration,
        raw_rows=raw_line_count,
        process_medians=len(process_medians),
    )
    (output_dir / "run-config.json").write_text(
        json.dumps(configuration, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    (output_dir / "summary.json").write_text(
        _summary_json(payload), encoding="utf-8"
    )
    (output_dir / "summary.csv").write_text(
        _summary_csv(decision), encoding="utf-8"
    )
    return payload


def audit_evidence(output_dir: Path) -> dict[str, Any]:
    output_dir = output_dir.resolve()
    configuration = json.loads(
        (output_dir / "run-config.json").read_text(encoding="utf-8"),
        object_pairs_hook=_unique_object,
        parse_constant=_reject_constant,
    )
    lines = _read_raw_lines(output_dir / "raw")
    rows = [parse_raw_row(line) for line in lines]
    decision = classify_evidence(
        rows,
        seed=_integer(configuration["seed"], "seed"),
        resamples=_integer(
            configuration["resamples"], "resamples", positive=True
        ),
    )
    payload = _summary_payload(
        decision,
        configuration=configuration,
        raw_rows=len(rows),
        process_medians=len(collapse_process_medians(rows)),
    )
    expected_json = _summary_json(payload)
    expected_csv = _summary_csv(decision)
    if (output_dir / "summary.json").read_text(encoding="utf-8") != \
            expected_json or \
            (output_dir / "summary.csv").read_text(encoding="utf-8") != \
            expected_csv:
        raise ValueError("stored summaries do not reproduce from raw evidence")
    return payload


def _parse_frames(text: str) -> tuple[int, ...]:
    try:
        values = tuple(int(value) for value in text.split(",") if value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("invalid frame byte list") from exc
    if values != (64, 256):
        raise argparse.ArgumentTypeError(
            "representation evidence requires frame bytes 64,256"
        )
    return values


def _parse_seed(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("invalid seed") from exc
    if value < 0 or value > 0xFFFFFFFFFFFFFFFF:
        raise argparse.ArgumentTypeError("seed must fit uint64")
    return value


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run or audit paired LCCF representation evidence"
    )
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--audit-only", action="store_true")
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--blocks", type=int, default=4)
    parser.add_argument("--minimum-window-ms", type=int, default=25)
    parser.add_argument("--instances", type=int, default=257)
    parser.add_argument("--chain", type=int, default=8)
    parser.add_argument("--frame-bytes", type=_parse_frames,
                        default=(64, 256))
    parser.add_argument("--warmup-rounds", type=int, default=4)
    parser.add_argument("--resamples", type=int, default=20_000)
    parser.add_argument("--seed", type=_parse_seed,
                        default=0x6C6363662D726570)
    parser.add_argument("--timeout", type=float, default=300.0)
    args = parser.parse_args(argv)
    if args.audit_only:
        return args
    if args.binary is None:
        parser.error("--binary is required unless --audit-only is set")
    if args.samples != 5 or args.blocks != 4:
        parser.error("the declared evidence contract requires 5 samples and 4 blocks")
    for name in (
        "minimum_window_ms", "instances", "chain", "resamples"
    ):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.warmup_rounds < 0 or args.timeout <= 0:
        parser.error("warmup rounds and timeout must be valid")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.audit_only:
        payload = audit_evidence(args.output_dir)
    else:
        payload = run_evidence(args)
    print(json.dumps({
        "decision": payload["decision"],
        "selected_representation": payload["selected_representation"],
        "cells": payload["counts"]["cells"],
    }, sort_keys=True))
    return 2 if payload["decision"] == "REJECT" else 0


if __name__ == "__main__":
    raise SystemExit(main())
