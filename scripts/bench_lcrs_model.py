#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

"""Run and classify the standalone LCRS Phase 0 selector model."""

from __future__ import annotations

import argparse
import csv
import json
import os
import platform
import re
import shlex
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

from process_utils import ProcessTimeoutError, run_capture
from safe_output import open_text_for_write


RESULT_PREFIX = "LCRS_SAMPLE "
WORKLOADS = (
    "balanced",
    "one_hot",
    "rotating_hotspot",
    "drain_tail",
    "uneven_numa",
    "offline_churn",
)
POLICIES = (
    "global_deepest",
    "deterministic_random_k",
    "coded_rotation_k",
    "coded_rotation_k_plus_fullscan",
)
INTEGER_FIELDS = {
    "version",
    "shards",
    "nodes",
    "iterations",
    "thief_attempts",
    "selections",
    "probes",
    "candidate_tries",
    "fullscan_fallbacks",
    "remote_selections",
    "productive_units",
    "p50_fanin",
    "p99_fanin",
    "max_fanin",
    "wall_ns",
    "cpu_ns",
}
EXPECTED_FIELDS = {
    *INTEGER_FIELDS,
    "workload",
    "policy",
    "input_checksum",
    "decision_checksum",
}
DECIMAL_RE = re.compile(r"[0-9]+")
CHECKSUM_RE = re.compile(r"[0-9a-f]{16}")


@dataclass(frozen=True)
class ModelRow:
    version: int
    workload: str
    policy: str
    shards: int
    nodes: int
    iterations: int
    thief_attempts: int
    selections: int
    probes: int
    candidate_tries: int
    fullscan_fallbacks: int
    remote_selections: int
    productive_units: int
    p50_fanin: int
    p99_fanin: int
    max_fanin: int
    wall_ns: int
    cpu_ns: int
    input_checksum: str
    decision_checksum: str


def _split_result(output: str) -> dict[str, str]:
    lines = output.splitlines()
    if len(lines) != 1 or not lines[0].startswith(RESULT_PREFIX):
        raise ValueError("expected exactly one LCRS model result row")
    payload = lines[0][len(RESULT_PREFIX) :]
    if not payload:
        raise ValueError("empty LCRS model result row")
    fields: dict[str, str] = {}
    for token in payload.split(" "):
        if not token or token.count("=") != 1:
            raise ValueError("malformed LCRS model field")
        key, value = token.split("=", 1)
        if not key or not value or key in fields:
            raise ValueError(f"duplicate or empty LCRS model field: {key}")
        fields[key] = value
    if set(fields) != EXPECTED_FIELDS:
        missing = sorted(EXPECTED_FIELDS - set(fields))
        extra = sorted(set(fields) - EXPECTED_FIELDS)
        raise ValueError(f"LCRS field mismatch: missing={missing} extra={extra}")
    return fields


def _integer(name: str, value: str) -> int:
    if DECIMAL_RE.fullmatch(value) is None:
        raise ValueError(f"invalid integer field {name}")
    return int(value, 10)


def parse_output(output: str) -> ModelRow:
    fields = _split_result(output)
    values = {name: _integer(name, fields[name]) for name in INTEGER_FIELDS}
    if values["version"] != 1:
        raise ValueError("unsupported LCRS sample version")
    if fields["workload"] not in WORKLOADS:
        raise ValueError("unknown LCRS workload")
    if fields["policy"] not in POLICIES:
        raise ValueError("unknown LCRS policy")
    if not 1 <= values["shards"] <= 4096:
        raise ValueError("shards out of range")
    if not 1 <= values["nodes"] <= values["shards"]:
        raise ValueError("nodes out of range")
    if values["iterations"] <= 0 or values["thief_attempts"] <= 0:
        raise ValueError("empty LCRS sample")
    if values["selections"] > values["thief_attempts"]:
        raise ValueError("selection accounting overflow")
    if values["fullscan_fallbacks"] > values["thief_attempts"]:
        raise ValueError("fallback accounting overflow")
    if values["remote_selections"] > values["selections"]:
        raise ValueError("remote selection accounting overflow")
    if not (
        0 <= values["p50_fanin"]
        <= values["p99_fanin"]
        <= values["max_fanin"]
        <= values["shards"]
    ):
        raise ValueError("fan-in quantiles out of range")
    if values["wall_ns"] <= 0 or values["cpu_ns"] <= 0:
        raise ValueError("non-positive timing")
    for name in ("input_checksum", "decision_checksum"):
        if CHECKSUM_RE.fullmatch(fields[name]) is None:
            raise ValueError(f"invalid {name}")
    return ModelRow(
        workload=fields["workload"],
        policy=fields["policy"],
        input_checksum=fields["input_checksum"],
        decision_checksum=fields["decision_checksum"],
        **values,
    )


def select_median(rows: Sequence[ModelRow]) -> ModelRow:
    if not rows:
        raise ValueError("cannot select median of empty rows")
    first = rows[0]
    if any(
        row.workload != first.workload
        or row.policy != first.policy
        or row.shards != first.shards
        or row.nodes != first.nodes
        or row.iterations != first.iterations
        for row in rows
    ):
        raise ValueError("median rows do not describe one configuration")
    return sorted(rows, key=lambda row: row.wall_ns)[len(rows) // 2]


def _ratio(numerator: int, denominator: int) -> float:
    if denominator == 0:
        return 1.0 if numerator == 0 else float("inf")
    return numerator / denominator


def classify(rows: Sequence[ModelRow]) -> tuple[str, list[str]]:
    reasons: list[str] = []
    major_failure = False
    complexity_failure = False
    by_key: dict[tuple[str, str], ModelRow] = {}

    for row in rows:
        key = (row.workload, row.policy)
        if key in by_key:
            return "REJECT", [f"duplicate summary row for {row.workload}/{row.policy}"]
        by_key[key] = row
    expected = {(workload, policy) for workload in WORKLOADS for policy in POLICIES}
    if set(by_key) != expected:
        return "REJECT", ["incomplete workload/policy matrix"]

    total_baseline_probes = sum(
        by_key[(workload, "global_deepest")].probes for workload in WORKLOADS
    )
    total_coded_probes = sum(
        by_key[(workload, "coded_rotation_k_plus_fullscan")].probes
        for workload in WORKLOADS
    )
    aggregate_probe_reduction = 1.0 - _ratio(
        total_coded_probes, total_baseline_probes
    )
    if aggregate_probe_reduction < 0.60:
        major_failure = True
        reasons.append(
            "aggregate probe reduction "
            f"{aggregate_probe_reduction:.1%} is below 60%"
        )

    complexity_wins = 0
    for workload in WORKLOADS:
        baseline = by_key[(workload, "global_deepest")]
        random = by_key[(workload, "deterministic_random_k")]
        coded = by_key[(workload, "coded_rotation_k")]
        coded_full = by_key[(workload, "coded_rotation_k_plus_fullscan")]
        workload_rows = (baseline, random, coded, coded_full)
        if len({row.input_checksum for row in workload_rows}) != 1:
            major_failure = True
            reasons.append(f"{workload}: input checksum mismatch")
            continue
        fanin_reduction = 1.0 - _ratio(coded_full.p99_fanin, baseline.p99_fanin)
        productive_ratio = _ratio(
            coded_full.productive_units, baseline.productive_units
        )
        remote_ratio = _ratio(coded_full.remote_selections, baseline.remote_selections)
        fallback_ratio = _ratio(
            coded_full.fullscan_fallbacks, coded_full.thief_attempts
        )
        if workload in {"balanced", "uneven_numa", "offline_churn"} and (
            fanin_reduction < 0.50
        ):
            major_failure = True
            reasons.append(
                f"{workload}: steady-state p99 fan-in reduction "
                f"{fanin_reduction:.1%} is below 50%"
            )
        if productive_ratio < 0.95 or (
            workload == "balanced" and productive_ratio < 0.98
        ):
            major_failure = True
            reasons.append(
                f"{workload}: productive ratio {productive_ratio:.3f} is below gate"
            )
        if remote_ratio > 1.05:
            major_failure = True
            reasons.append(
                f"{workload}: remote selection ratio {remote_ratio:.3f} exceeds 1.05"
            )
        if workload == "balanced" and fallback_ratio > 0.15:
            major_failure = True
            reasons.append(
                f"balanced: fallback ratio {fallback_ratio:.1%} exceeds 15%"
            )
        if workload in {"one_hot", "rotating_hotspot"} and fanin_reduction < 0.10:
            major_failure = True
            reasons.append(f"{workload}: hotspot p99 did not improve by 10%")

        p99_win = coded.p99_fanin <= random.p99_fanin * 0.97
        productive_win = coded.productive_units >= random.productive_units * 1.03
        if p99_win or productive_win:
            complexity_wins += 1

    if complexity_wins == 0:
        complexity_failure = True
        reasons.append("coded rotation does not beat deterministic random-k by 3%")
    if major_failure:
        return "REJECT", reasons
    if complexity_failure:
        return "NARROW", reasons
    if not reasons:
        reasons.append("all standalone model and random-k complexity gates passed")
    return "PASS", reasons


def write_markdown(
    path: Path,
    rows: Sequence[ModelRow],
    verdict: str,
    reasons: Sequence[str],
    metadata: dict[str, str],
) -> None:
    lines = [
        "# LCRS Phase 0 Standalone Model",
        "",
        f"Verdict: **{verdict}**",
        "",
        "This report evaluates a standalone model; it is not runtime integration evidence.",
        "The coded policy is retained only when it clears the deterministic random-k complexity gate.",
        "",
        "## Decision reasons",
        "",
    ]
    lines.extend(f"- {reason}" for reason in reasons)
    lines.extend(
        [
            "",
            "## Median process rows",
            "",
            "| Workload | Policy | Probes | p99 fan-in | Productive units | Fallbacks |",
            "|---|---:|---:|---:|---:|---:|",
        ]
    )
    for row in sorted(rows, key=lambda item: (item.workload, item.policy)):
        lines.append(
            f"| {row.workload} | {row.policy} | {row.probes} | "
            f"{row.p99_fanin} | {row.productive_units} | "
            f"{row.fullscan_fallbacks} |"
        )
    lines.extend(["", "## Metadata", ""])
    lines.extend(f"- {key}: `{value}`" for key, value in sorted(metadata.items()))
    lines.append("")
    with open_text_for_write(path) as handle:
        handle.write("\n".join(lines))


def _write_csv(path: Path, rows: Iterable[ModelRow]) -> None:
    fieldnames = list(ModelRow.__dataclass_fields__)
    with open_text_for_write(path, newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def _run_sample(
    binary: Path,
    workload: str,
    policy: str,
    shards: int,
    nodes: int,
    iterations: int,
    seed: int,
    timeout: float,
) -> ModelRow:
    command = [
        str(binary),
        "--workload",
        workload,
        "--policy",
        policy,
        "--shards",
        str(shards),
        "--nodes",
        str(nodes),
        "--iterations",
        str(iterations),
        "--seed",
        str(seed),
    ]
    result = run_capture(command, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(
            f"LCRS model failed ({result.returncode}): {result.stderr.strip()}"
        )
    return parse_output(result.stdout.rstrip("\n"))


def run_campaign(args: argparse.Namespace) -> int:
    if args.samples <= 0 or args.samples % 2 == 0:
        raise ValueError("samples must be a positive odd number")
    out_dir: Path = args.out_dir
    raw: list[ModelRow] = []
    orders = (POLICIES, tuple(reversed(POLICIES)), POLICIES, tuple(reversed(POLICIES)))
    for sample in range(args.samples):
        order = orders[sample % len(orders)]
        for workload in WORKLOADS:
            for policy in order:
                raw.append(
                    _run_sample(
                        args.binary,
                        workload,
                        policy,
                        args.shards,
                        args.nodes,
                        args.iterations,
                        args.seed,
                        args.timeout,
                    )
                )
    summaries = [
        select_median(
            [
                row
                for row in raw
                if row.workload == workload and row.policy == policy
            ]
        )
        for workload in WORKLOADS
        for policy in POLICIES
    ]
    verdict, reasons = classify(summaries)
    metadata = {
        "command": " ".join(shlex.quote(value) for value in sys.argv),
        "host": platform.node(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "binary": str(args.binary),
    }
    _write_csv(out_dir / "lcrs_samples.csv", raw)
    _write_csv(out_dir / "lcrs_summary.csv", summaries)
    with open_text_for_write(out_dir / "lcrs_metadata.json") as handle:
        json.dump(metadata, handle, indent=2, sort_keys=True)
        handle.write("\n")
    write_markdown(out_dir / "lcrs_report.md", summaries, verdict, reasons, metadata)
    print(f"LCRS_VERDICT verdict={verdict} out_dir={out_dir}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, default=Path("object/lcrs-phase0"))
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--shards", type=int, default=32)
    parser.add_argument("--nodes", type=int, default=2)
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--quick", action="store_true")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.quick:
        args.samples = 1
        args.iterations = min(args.iterations, 64)
    try:
        return run_campaign(args)
    except (OSError, RuntimeError, ValueError, ProcessTimeoutError) as exc:
        print(f"bench_lcrs_model: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
