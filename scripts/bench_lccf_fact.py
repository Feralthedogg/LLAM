#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

RESULT_PREFIX = "LCCF_FACT_SAMPLE "

PAIR_BASELINES = {
    "shared_fact_queue": "recompute_queue",
    "shared_fact_fused": "recompute_fused",
    "mixed_shared_fact": "mixed_recompute",
}
ALL_MODES = frozenset(PAIR_BASELINES) | frozenset(PAIR_BASELINES.values())
ALL_WORKLOADS = (
    "completion_io_pipeline",
    "completion_rpc_state",
    "completion_timer_cancel",
    "completion_mixed_fairness",
)

STRING_FIELDS = frozenset({"workload", "mode", "checksum"})
BOOL_FIELDS = frozenset({"instructions_supported", "refs_balanced"})
INTEGER_FIELDS = frozenset({
    "version", "instances", "frame_bytes", "cell_bytes", "sites",
    "chain", "rounds", "operations", "callbacks", "wall_ns", "cpu_ns",
    "p50_ns", "p99_ns", "instructions", "completions", "claims",
    "stale_tickets", "queue_pushes", "queue_pops", "resume_calls",
    "direct_calls", "forced_escapes", "hot_allocations",
    "facts_attempted", "facts_built", "facts_build_failed",
    "fact_normalizations", "fact_site_lookups", "fact_module_pins",
    "fact_payload_pins", "fact_stale_losers", "fact_guard_rechecks",
    "fact_queue_forwards", "fact_generation_mismatches",
    "fact_reuse_delays", "fact_hot_bytes", "fact_sidecar_bytes",
})
EXPECTED_FIELDS = STRING_FIELDS | BOOL_FIELDS | INTEGER_FIELDS


@dataclass(frozen=True)
class FactSample:
    version: int
    workload: str
    mode: str
    instances: int
    frame_bytes: int
    cell_bytes: int
    sites: int
    chain: int
    rounds: int
    operations: int
    callbacks: int
    wall_ns: int
    cpu_ns: int
    p50_ns: int
    p99_ns: int
    instructions_supported: bool
    instructions: int
    checksum: str
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
    fact_module_pins: int
    fact_payload_pins: int
    fact_stale_losers: int
    fact_guard_rechecks: int
    fact_queue_forwards: int
    fact_generation_mismatches: int
    fact_reuse_delays: int
    fact_hot_bytes: int
    fact_sidecar_bytes: int
    refs_balanced: bool

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class PairResult:
    status: str
    reasons: tuple[str, ...]
    wall_speedup: float
    cpu_ratio: float
    p99_ratio: float
    instruction_ratio: float | None

    def as_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class Cell:
    workload: str
    candidate: str
    baseline: str
    frame_bytes: int
    cell_bytes: int
    sites: int

    @property
    def slug(self) -> str:
        return (
            f"{self.workload}__{self.candidate}__f{self.frame_bytes}"
            f"__c{self.cell_bytes}__s{self.sites}"
        )


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _validate_sample(sample: FactSample) -> None:
    positive = (
        sample.instances, sample.frame_bytes, sample.cell_bytes, sample.sites,
        sample.chain, sample.rounds, sample.operations, sample.callbacks,
        sample.wall_ns, sample.cpu_ns, sample.p50_ns, sample.p99_ns,
        sample.completions, sample.claims, sample.resume_calls,
        sample.facts_attempted, sample.facts_built, sample.fact_hot_bytes,
    )
    if sample.version != 1:
        raise ValueError("unsupported sample version")
    if sample.mode not in ALL_MODES:
        raise ValueError("unknown fact mode")
    if sample.workload not in ALL_WORKLOADS:
        raise ValueError("unknown workload")
    if any(value <= 0 for value in positive):
        raise ValueError("positive sample field is zero or negative")
    if sample.frame_bytes not in (64, 128, 256):
        raise ValueError("unsupported frame layout")
    if sample.cell_bytes not in (64, 96, 128) or sample.sites not in (1, 8):
        raise ValueError("unsupported fact layout")
    if len(sample.checksum) != 16 or any(
        char not in "0123456789abcdef" for char in sample.checksum
    ):
        raise ValueError("checksum must be 16 lowercase hex digits")
    expected_completions = sample.rounds * sample.instances
    expected_callbacks = expected_completions * sample.chain
    if sample.completions != expected_completions:
        raise ValueError("completion count mismatch")
    if sample.operations != expected_callbacks or sample.callbacks != expected_callbacks:
        raise ValueError("operation count mismatch")
    if sample.resume_calls != expected_callbacks or sample.claims != expected_completions:
        raise ValueError("callback or claim count mismatch")
    if sample.facts_built != expected_completions:
        raise ValueError("fact build count mismatch")
    if sample.facts_attempted < sample.facts_built:
        raise ValueError("fact attempt count is below fact build count")
    if sample.queue_pushes != sample.queue_pops:
        raise ValueError("queue ownership is unbalanced")
    if sample.direct_calls + sample.queue_pops != sample.resume_calls:
        raise ValueError("callback routing count mismatch")
    if sample.p50_ns > sample.p99_ns or sample.p99_ns > sample.wall_ns:
        raise ValueError("latency quantiles are inconsistent")
    if sample.instructions_supported != (sample.instructions > 0):
        raise ValueError("instruction support/value mismatch")
    if "shared_fact" in sample.mode and (
        sample.fact_normalizations != expected_completions
        or sample.fact_site_lookups != expected_completions
    ):
        raise ValueError("shared mode did not materialize exactly once")


def parse_sample_output(output: str) -> FactSample:
    lines = output.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one LCCF fact sample row")
    payload = lines[0][len(RESULT_PREFIX):]
    try:
        values = json.loads(payload, object_pairs_hook=_unique_object)
    except (json.JSONDecodeError, ValueError) as exc:
        raise ValueError("malformed LCCF fact sample JSON") from exc
    if not isinstance(values, dict) or set(values) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(values) if isinstance(values, dict) else EXPECTED_FIELDS)
        extra = sorted(set(values) - EXPECTED_FIELDS if isinstance(values, dict) else ())
        raise ValueError(f"sample schema mismatch: missing={missing} extra={extra}")
    for key in STRING_FIELDS:
        if type(values[key]) is not str:
            raise ValueError(f"{key} must be a string")
    for key in BOOL_FIELDS:
        if type(values[key]) is not bool:
            raise ValueError(f"{key} must be a boolean")
    for key in INTEGER_FIELDS:
        if type(values[key]) is not int or values[key] < 0:
            raise ValueError(f"{key} must be a non-negative integer")
    sample = FactSample(**values)
    _validate_sample(sample)
    return sample


def _same_experiment(left: FactSample, right: FactSample) -> bool:
    fields = (
        "version", "workload", "instances", "frame_bytes", "cell_bytes",
        "sites", "chain", "rounds", "operations", "callbacks",
    )
    return all(getattr(left, field) == getattr(right, field) for field in fields)


def classify_pair(baseline: FactSample, candidate: FactSample) -> PairResult:
    correctness: list[str] = []
    performance: list[str] = []
    expected_baseline = PAIR_BASELINES.get(candidate.mode)

    if expected_baseline != baseline.mode:
        correctness.append("candidate/baseline mode mismatch")
    if not _same_experiment(baseline, candidate):
        correctness.append("experiment configuration mismatch")
    if baseline.checksum != candidate.checksum:
        correctness.append("canonical checksum mismatch")
    if not baseline.refs_balanced or not candidate.refs_balanced:
        correctness.append("fact references are unbalanced")
    if baseline.hot_allocations != 0 or candidate.hot_allocations != 0:
        correctness.append("hot-path allocation observed")
    common_metrics = (
        "completions", "claims", "stale_tickets", "queue_pushes",
        "queue_pops", "resume_calls", "direct_calls", "forced_escapes",
        "facts_attempted", "facts_built", "facts_build_failed",
        "fact_module_pins", "fact_payload_pins", "fact_stale_losers",
        "fact_guard_rechecks", "fact_queue_forwards",
        "fact_generation_mismatches", "fact_reuse_delays",
        "fact_hot_bytes", "fact_sidecar_bytes",
    )
    if any(getattr(baseline, field) != getattr(candidate, field)
           for field in common_metrics):
        correctness.append("paired routing/lifetime metrics differ")
    if candidate.fact_normalizations != candidate.completions or \
            candidate.fact_site_lookups != candidate.completions:
        correctness.append("candidate did not perform one normalization and lookup per generation")
    if candidate.facts_build_failed != 0 or \
            candidate.fact_generation_mismatches != 0 or \
            candidate.fact_reuse_delays != 0:
        correctness.append("candidate reported fact lifecycle failures")

    wall_speedup = baseline.wall_ns / candidate.wall_ns
    cpu_ratio = candidate.cpu_ns / baseline.cpu_ns
    p99_ratio = candidate.p99_ns / baseline.p99_ns
    instruction_ratio: float | None = None
    if baseline.instructions_supported and candidate.instructions_supported:
        instruction_ratio = candidate.instructions / baseline.instructions
    if not all(math.isfinite(value) and value > 0
               for value in (wall_speedup, cpu_ratio, p99_ratio)):
        correctness.append("non-finite paired ratio")

    if not correctness:
        if p99_ratio > 1.05:
            performance.append("p99 latency regression exceeds 5%")
        if candidate.mode == "shared_fact_queue" and wall_speedup < 0.98:
            performance.append("queued throughput is below 98% of recompute baseline")
        elif candidate.mode == "shared_fact_fused" and wall_speedup < 0.98:
            performance.append("direct-path regression exceeds 2%")
        elif candidate.mode == "mixed_shared_fact":
            instruction_pass = instruction_ratio is not None and instruction_ratio <= 0.95
            if cpu_ratio > 0.95 and not instruction_pass:
                performance.append("mixed CPU/instruction improvement is below 5%")

    status = "FAIL_CORRECTNESS" if correctness else (
        "FAIL_PERFORMANCE" if performance else "PASS"
    )
    return PairResult(
        status=status,
        reasons=tuple(correctness + performance),
        wall_speedup=wall_speedup,
        cpu_ratio=cpu_ratio,
        p99_ratio=p99_ratio,
        instruction_ratio=instruction_ratio,
    )


def build_command(
    binary: Path,
    cell: Cell,
    mode: str,
    instances: int,
    chain: int,
    rounds: int,
    warmup_rounds: int,
    seed: int,
) -> list[str]:
    return [
        str(binary),
        "--workload", cell.workload,
        "--mode", mode,
        "--instances", str(instances),
        "--frame-bytes", str(cell.frame_bytes),
        "--cell-bytes", str(cell.cell_bytes),
        "--sites", str(cell.sites),
        "--chain", str(chain),
        "--rounds", str(rounds),
        "--warmup-rounds", str(warmup_rounds),
        "--seed", str(seed),
    ]


def run_sample(command: Sequence[str], timeout: float) -> FactSample:
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"sample failed ({completed.returncode}): {' '.join(command)}\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )
    if completed.stderr:
        raise RuntimeError(f"sample wrote stderr: {completed.stderr}")
    return parse_sample_output(completed.stdout.rstrip("\n"))


def matrix_cells(
    workloads: Iterable[str],
    candidates: Iterable[str],
    layouts: Iterable[int],
    sites: Iterable[int],
) -> list[Cell]:
    frames = (64, 128, 256)
    result: list[Cell] = []
    for workload_index, workload in enumerate(workloads):
        for candidate in candidates:
            for layout_index, layout in enumerate(layouts):
                for site_count in sites:
                    result.append(Cell(
                        workload=workload,
                        candidate=candidate,
                        baseline=PAIR_BASELINES[candidate],
                        frame_bytes=frames[(workload_index + layout_index) % len(frames)],
                        cell_bytes=layout,
                        sites=site_count,
                    ))
    return result


def _aggregate_cell(cell: Cell, results: list[PairResult]) -> dict[str, Any]:
    correctness = [result for result in results if result.status == "FAIL_CORRECTNESS"]
    wall_speedup = statistics.median(result.wall_speedup for result in results)
    cpu_ratio = statistics.median(result.cpu_ratio for result in results)
    p99_ratio = statistics.median(result.p99_ratio for result in results)
    instruction_values = [
        result.instruction_ratio for result in results
        if result.instruction_ratio is not None
    ]
    instruction_ratio = statistics.median(instruction_values) if instruction_values else None
    reasons: list[str] = []
    if correctness:
        status = "FAIL_CORRECTNESS"
        reasons = sorted({reason for result in correctness for reason in result.reasons})
    else:
        if p99_ratio > 1.05:
            reasons.append("median p99 latency regression exceeds 5%")
        if cell.candidate in ("shared_fact_queue", "shared_fact_fused"):
            if wall_speedup < 0.98:
                reasons.append("median throughput is below the 98% gate")
        elif cpu_ratio > 0.95 and not (
            instruction_ratio is not None and instruction_ratio <= 0.95
        ):
            reasons.append("median mixed CPU/instruction improvement is below 5%")
        status = "FAIL_PERFORMANCE" if reasons else "PASS"
    return {
        "cell": asdict(cell),
        "status": status,
        "reasons": reasons,
        "pair_count": len(results),
        "median_wall_speedup": wall_speedup,
        "median_cpu_ratio": cpu_ratio,
        "median_p99_ratio": p99_ratio,
        "median_instruction_ratio": instruction_ratio,
    }


def run_evidence(args: argparse.Namespace) -> dict[str, Any]:
    output_dir = args.output_dir.resolve()
    raw_dir = output_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    summaries: list[dict[str, Any]] = []
    for cell_index, cell in enumerate(matrix_cells(
        args.workloads, args.candidates, args.layouts, args.sites
    )):
        pair_results: list[PairResult] = []
        raw_path = raw_dir / f"{cell.slug}.jsonl"
        with raw_path.open("w", encoding="utf-8") as raw_file:
            for repetition in range(args.samples):
                order = (
                    (cell.baseline, cell.candidate, cell.candidate, cell.baseline)
                    if repetition % 2 == 0 else
                    (cell.candidate, cell.baseline, cell.baseline, cell.candidate)
                )
                observed: list[FactSample] = []
                seed = args.seed ^ (cell_index * 0x9E3779B1) ^ repetition
                for slot, mode in enumerate(order):
                    command = build_command(
                        args.binary, cell, mode, args.instances, args.chain,
                        args.rounds, args.warmup_rounds, seed,
                    )
                    value = run_sample(command, args.timeout)
                    observed.append(value)
                    raw_file.write(json.dumps({
                        "repetition": repetition,
                        "slot": slot,
                        "order": "ABBA" if repetition % 2 == 0 else "BAAB",
                        "sample": value.as_dict(),
                    }, sort_keys=True) + "\n")
                baseline_samples = [value for value in observed if value.mode == cell.baseline]
                candidate_samples = [value for value in observed if value.mode == cell.candidate]
                if len(baseline_samples) != 2 or len(candidate_samples) != 2:
                    raise RuntimeError("paired order did not produce two samples per mode")
                pair_results.extend(
                    classify_pair(base, candidate)
                    for base, candidate in zip(baseline_samples, candidate_samples)
                )
        summaries.append(_aggregate_cell(cell, pair_results))
        print(
            f"[{cell_index + 1}] {cell.slug}: {summaries[-1]['status']} "
            f"wall={summaries[-1]['median_wall_speedup']:.4f} "
            f"cpu={summaries[-1]['median_cpu_ratio']:.4f}",
            flush=True,
        )
    if any(row["status"] == "FAIL_CORRECTNESS" for row in summaries):
        decision = "REJECT"
    elif all(row["status"] == "PASS" for row in summaries):
        decision = "PASS"
    else:
        decision = "NARROW"
    result = {
        "schema_version": 1,
        "decision": decision,
        "sample_pairs_per_cell": args.samples * 2,
        "cells": summaries,
    }
    (output_dir / "summary.json").write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return result


def _csv_values(text: str, allowed: set[str]) -> tuple[str, ...]:
    values = tuple(part for part in text.split(",") if part)
    if not values or any(value not in allowed for value in values):
        raise argparse.ArgumentTypeError(f"invalid selection: {text}")
    return values


def _csv_ints(text: str, allowed: set[int]) -> tuple[int, ...]:
    try:
        values = tuple(int(part) for part in text.split(",") if part)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer selection: {text}") from exc
    if not values or any(value not in allowed for value in values):
        raise argparse.ArgumentTypeError(f"invalid selection: {text}")
    return values


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run paired LCCF fact evidence")
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--rounds", type=int, default=256)
    parser.add_argument("--warmup-rounds", type=int, default=16)
    parser.add_argument("--instances", type=int, default=257)
    parser.add_argument("--chain", type=int, default=8)
    parser.add_argument("--seed", type=int, default=0x6C6363662D636673)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--workloads", type=lambda value: _csv_values(value, set(ALL_WORKLOADS)),
        default=ALL_WORKLOADS,
    )
    parser.add_argument(
        "--candidates", type=lambda value: _csv_values(value, set(PAIR_BASELINES)),
        default=tuple(PAIR_BASELINES),
    )
    parser.add_argument(
        "--layouts", type=lambda value: _csv_ints(value, {64, 96, 128}),
        default=(64, 96, 128),
    )
    parser.add_argument(
        "--sites", type=lambda value: _csv_ints(value, {1, 8}),
        default=(1, 8),
    )
    args = parser.parse_args(argv)
    for name in ("samples", "rounds", "instances", "chain"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if args.warmup_rounds < 0 or args.timeout <= 0:
        parser.error("warmup and timeout values are invalid")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    result = run_evidence(args)
    print(json.dumps({"decision": result["decision"],
                      "cells": len(result["cells"])}, sort_keys=True))
    return 1 if result["decision"] == "REJECT" else 0


if __name__ == "__main__":
    raise SystemExit(main())
