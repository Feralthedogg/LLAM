#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import pathlib
import platform
import re
import shutil
import tempfile
import time
from dataclasses import asdict, dataclass
from typing import Any, Sequence

from process_utils import ProcessTimeoutError, run_capture


RESULT_FIELDS = {
    "schema_version", "status", "seed", "lanes", "workers",
    "rounds_requested", "rounds_completed", "coupling", "fault",
    "signature", "lane_executions", "elapsed_ns", "failures",
    "first_failure_round", "oracle", "armed_total", "winner_total",
    "cancel_total", "timeout_total", "discard_total", "trace_entries",
    "trace_truncated", "cleanup_complete",
}
INTEGER_FIELDS = {
    "schema_version", "seed", "lanes", "workers", "rounds_requested",
    "rounds_completed", "lane_executions", "elapsed_ns", "failures",
    "oracle", "armed_total", "winner_total", "cancel_total",
    "timeout_total", "discard_total", "trace_entries",
}
BOOL_FIELDS = {"trace_truncated", "cleanup_complete"}
COUPLINGS = {"independent", "shared_object", "ring", "colored_graph"}
FAULTS = {"none", "select_skip_winner_cas", "stale_generation_reuse"}
STATUSES = {"ok", "oracle_failure", "timeout", "platform_error"}
SIGNATURE_PATTERN = re.compile(r"^[0-9a-f]{16}$")


class ResultSchemaError(ValueError):
    pass


class SampleTimeout(RuntimeError):
    pass


@dataclass(frozen=True)
class RunConfig:
    seed: int
    lanes: int
    workers: int
    rounds: int
    coupling: str
    queue_capacity: int
    timeout_ms: int
    fault: str
    allowed_outcomes: int
    generate_perturbations: bool = True

    def command(self, binary: pathlib.Path, artifact_dir: pathlib.Path) -> list[str]:
        command = [
            str(binary.resolve()), "--seed", str(self.seed),
            "--lanes", str(self.lanes),
            "--workers", str(self.workers), "--rounds", str(self.rounds),
            "--coupling", self.coupling, "--queue-capacity",
            str(self.queue_capacity), "--timeout-ms", str(self.timeout_ms),
            "--fault", self.fault, "--allowed-outcomes",
            str(self.allowed_outcomes), "--artifact-dir", str(artifact_dir),
        ]
        if self.generate_perturbations:
            command.append("--generate-perturbations")
        return command


@dataclass(frozen=True)
class SampleResult:
    config: RunConfig
    document: dict[str, Any]
    artifact_dir: pathlib.Path
    replayed: bool
    wall_time_ns: int


def _reject_constant(value: str) -> None:
    raise ResultSchemaError(f"non-finite JSON constant: {value}")


def parse_result_document(
    text: str, *, expected_signature: str | None = None,
) -> dict[str, Any]:
    lines = text.splitlines()
    if len(lines) != 1 or not lines[0].strip():
        raise ResultSchemaError("stdout must contain exactly one JSON document")
    try:
        document = json.loads(lines[0], parse_constant=_reject_constant)
    except (json.JSONDecodeError, ResultSchemaError) as error:
        raise ResultSchemaError(f"invalid result JSON: {error}") from error
    if type(document) is not dict:
        raise ResultSchemaError("result must be a JSON object")
    fields = set(document)
    if fields != RESULT_FIELDS:
        missing = sorted(RESULT_FIELDS - fields)
        unknown = sorted(fields - RESULT_FIELDS)
        raise ResultSchemaError(f"field mismatch missing={missing} unknown={unknown}")
    for field in INTEGER_FIELDS:
        value = document[field]
        if type(value) is not int or value < 0:
            raise ResultSchemaError(f"{field} must be a non-negative integer")
    for field in BOOL_FIELDS:
        if type(document[field]) is not bool:
            raise ResultSchemaError(f"{field} must be boolean")
    failure_round = document["first_failure_round"]
    if failure_round is not None and (
        type(failure_round) is not int or failure_round < 0
    ):
        raise ResultSchemaError("first_failure_round must be null or non-negative")
    if document["schema_version"] != 1:
        raise ResultSchemaError("unsupported schema_version")
    if document["status"] not in STATUSES:
        raise ResultSchemaError("invalid status")
    if document["coupling"] not in COUPLINGS:
        raise ResultSchemaError("invalid coupling")
    if document["fault"] not in FAULTS:
        raise ResultSchemaError("invalid fault")
    signature = document["signature"]
    if type(signature) is not str or not SIGNATURE_PATTERN.fullmatch(signature):
        raise ResultSchemaError("signature must be 16 lowercase hexadecimal digits")
    if expected_signature is not None and signature != expected_signature:
        raise ResultSchemaError(
            f"signature mismatch expected={expected_signature} actual={signature}"
        )
    if document["winner_total"] > document["lane_executions"]:
        raise ResultSchemaError("winner_total exceeds lane_executions")
    if document["discard_total"] > document["lane_executions"]:
        raise ResultSchemaError("discard_total exceeds lane_executions")
    if document["cancel_total"] + document["timeout_total"] > document["winner_total"]:
        raise ResultSchemaError("terminal subtype accounting exceeds winners")
    if document["status"] == "ok":
        if document["failures"] != 0 or signature != "0000000000000000":
            raise ResultSchemaError("successful result carries failure evidence")
        if failure_round is not None:
            raise ResultSchemaError("successful result has first_failure_round")
        if document["rounds_completed"] != document["rounds_requested"]:
            raise ResultSchemaError("successful result did not finish every round")
        if not document["cleanup_complete"]:
            raise ResultSchemaError("successful result did not clean up")
        if document["winner_total"] + document["discard_total"] != document["lane_executions"]:
            raise ResultSchemaError("successful result accounting does not balance")
    elif document["status"] == "oracle_failure":
        if document["failures"] == 0 or signature == "0000000000000000":
            raise ResultSchemaError("oracle failure lacks failure evidence")
        if failure_round is None:
            raise ResultSchemaError("oracle failure lacks first_failure_round")
    return document


def execute_result_document(
    command: Sequence[str], *, timeout: float,
    expected_signature: str | None = None,
) -> tuple[dict[str, Any], int, str]:
    try:
        completed = run_capture(command, timeout=timeout)
    except ProcessTimeoutError as error:
        raise SampleTimeout(str(error)) from error
    document = parse_result_document(
        completed.stdout, expected_signature=expected_signature,
    )
    expected_returncode = 0 if document["status"] == "ok" else 10
    if completed.returncode != expected_returncode:
        raise ResultSchemaError(
            f"exit/status mismatch exit={completed.returncode} status={document['status']} "
            f"stderr={completed.stderr!r}"
        )
    return document, completed.returncode, completed.stderr


def _capture_first_line(command: Sequence[str]) -> str:
    try:
        completed = run_capture(command, timeout=2.0)
    except (OSError, ProcessTimeoutError):
        return "unavailable"
    if completed.returncode != 0:
        return "unavailable"
    lines = completed.stdout.splitlines()
    return lines[0].strip() if lines else "unavailable"


def _metadata(binary: pathlib.Path) -> dict[str, Any]:
    affinity: list[int] | None = None
    if hasattr(os, "sched_getaffinity"):
        affinity = sorted(os.sched_getaffinity(0))
    return {
        "os": platform.system(),
        "os_release": platform.release(),
        "architecture": platform.machine(),
        "python": platform.python_version(),
        "repository_commit": _capture_first_line(["git", "rev-parse", "HEAD"]),
        "compiler": _capture_first_line([os.environ.get("CC", "cc"), "--version"]),
        "cflags": os.environ.get("CFLAGS", ""),
        "affinity": affinity,
        "binary": str(binary.resolve()),
        "runtime_profile": "test-only",
        "experimental_flags": os.environ.get("LRPA_EXPERIMENTAL_FLAGS", ""),
        "sanitizer": {
            "asan": os.environ.get("ASAN_OPTIONS", ""),
            "ubsan": os.environ.get("UBSAN_OPTIONS", ""),
            "tsan": os.environ.get("TSAN_OPTIONS", ""),
        },
    }


def _write_runner_manifest(
    path: pathlib.Path, config: RunConfig, binary: pathlib.Path,
) -> None:
    document = {
        "runner_schema_version": 1,
        "config": asdict(config),
        "environment": _metadata(binary),
    }
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n")


def _run_once(
    binary: pathlib.Path, config: RunConfig, staging: pathlib.Path,
    process_timeout: float, expected_signature: str | None = None,
) -> dict[str, Any]:
    staging.mkdir(parents=True, exist_ok=False)
    _write_runner_manifest(staging / "runner-manifest.json", config, binary)
    document, _, stderr = execute_result_document(
        config.command(binary, staging), timeout=process_timeout,
        expected_signature=expected_signature,
    )
    if stderr:
        (staging / "stderr.txt").write_text(stderr)
    (staging / "result.json").write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n"
    )
    return document


def run_sample(
    binary: pathlib.Path, config: RunConfig, artifact_root: pathlib.Path,
    *, process_timeout: float, replay_failures: bool,
) -> SampleResult:
    artifact_root.mkdir(parents=True, exist_ok=True)
    staging_parent = artifact_root / ".staging"
    staging_parent.mkdir(exist_ok=True)
    staging = pathlib.Path(tempfile.mkdtemp(prefix="sample-", dir=staging_parent))
    shutil.rmtree(staging)
    wall_start = time.monotonic_ns()
    document = _run_once(binary, config, staging, process_timeout)
    wall_time_ns = time.monotonic_ns() - wall_start
    signature = document["signature"]
    if document["status"] == "oracle_failure":
        destination = artifact_root / "failures" / signature / str(config.seed)
    else:
        destination = (
            artifact_root / "results" /
            f"{config.coupling}-l{config.lanes}" / str(config.seed)
        )
    if destination.exists():
        collision = destination.with_name(
            f"{destination.name}-{config.coupling}-l{config.lanes}"
        )
        suffix = 2
        while collision.exists():
            collision = destination.with_name(
                f"{destination.name}-{config.coupling}-l{config.lanes}-{suffix}"
            )
            suffix += 1
        destination = collision
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(str(staging), str(destination))

    replayed = False
    if document["status"] == "oracle_failure" and replay_failures:
        replay_staging = destination / "replay"
        replay_document = _run_once(
            binary, config, replay_staging, process_timeout,
            expected_signature=signature,
        )
        if replay_document["status"] != "oracle_failure":
            raise ResultSchemaError("failure replay unexpectedly passed")
        replayed = True
    return SampleResult(config, document, destination, replayed, wall_time_ns)


def _percentile_99(values: list[int]) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = max(0, math.ceil(len(ordered) * 0.99) - 1)
    return ordered[index]


def compare_equal_cost(
    binary: pathlib.Path, artifact_root: pathlib.Path, *, fault: str,
    lane_budget: int, workers: int, rounds: int, coupling: str,
    queue_capacity: int, timeout_ms: int, process_timeout: float,
    seed_base: int = 1, lane_counts: Sequence[int] = (1, 2, 4, 8),
) -> dict[str, Any]:
    if lane_budget <= 0 or any(lanes <= 0 or lane_budget % lanes != 0
                               for lanes in lane_counts):
        raise ValueError("lane_budget must be positive and divisible by lane counts")
    samples_by_lane: dict[int, list[SampleResult]] = {
        lanes: [] for lanes in lane_counts
    }
    counts = {lanes: lane_budget // lanes for lanes in lane_counts}
    max_samples = max(counts.values())
    for sample_index in range(max_samples):
        order = list(lane_counts)
        if sample_index % 2:
            order.reverse()
        for lanes in order:
            if sample_index >= counts[lanes]:
                continue
            config = RunConfig(
                seed=seed_base + sample_index,
                lanes=lanes,
                workers=workers,
                rounds=rounds,
                coupling=coupling,
                queue_capacity=queue_capacity,
                timeout_ms=timeout_ms,
                fault=fault,
                allowed_outcomes=30,
                generate_perturbations=True,
            )
            samples_by_lane[lanes].append(run_sample(
                binary, config, artifact_root,
                process_timeout=process_timeout, replay_failures=True,
            ))

    profiles: list[dict[str, Any]] = []
    for lanes in lane_counts:
        samples = sorted(samples_by_lane[lanes], key=lambda item: item.config.seed)
        failures = [sample for sample in samples
                    if sample.document["status"] == "oracle_failure"]
        cumulative_budgeted_executions = 0
        cumulative_actual_executions = 0
        cumulative_wall = 0
        budgeted_executions_to_first: int | None = None
        actual_executions_to_first: int | None = None
        wall_to_first: int | None = None
        for sample in samples:
            cumulative_budgeted_executions += lanes * workers * rounds
            cumulative_actual_executions += int(
                sample.document["lane_executions"]
            )
            cumulative_wall += sample.wall_time_ns
            if sample.document["status"] == "oracle_failure":
                budgeted_executions_to_first = cumulative_budgeted_executions
                actual_executions_to_first = cumulative_actual_executions
                wall_to_first = cumulative_wall
                break
        budgeted_executions = len(samples) * lanes * workers * rounds
        actual_executions = sum(
            int(sample.document["lane_executions"]) for sample in samples
        )
        signatures = [str(sample.document["signature"]) for sample in failures]
        signature_counts = {
            signature: signatures.count(signature) for signature in sorted(set(signatures))
        }
        profile = {
            "lanes": lanes,
            "processes": len(samples),
            "failures": len(failures),
            "process_failure_rate": len(failures) / len(samples),
            "budgeted_lane_executions": budgeted_executions,
            "actual_lane_executions": actual_executions,
            "detections_per_million_lane_executions": (
                len(failures) * 1_000_000.0 / budgeted_executions
            ),
            "budgeted_executions_to_first_detection":
                budgeted_executions_to_first,
            "actual_executions_to_first_detection": actual_executions_to_first,
            "wall_ns_to_first_detection": wall_to_first,
            "replay_rate": (
                sum(sample.replayed for sample in failures) / len(failures)
                if failures else None
            ),
            "signature_counts": signature_counts,
            "signature_stability": (
                max(signature_counts.values()) / len(failures)
                if failures else None
            ),
            "cleanup_failures": sum(
                not bool(sample.document["cleanup_complete"]) for sample in samples
            ),
            "mean_process_elapsed_ns": (
                sum(int(sample.document["elapsed_ns"]) for sample in samples) // len(samples)
            ),
            "p99_process_elapsed_ns": _percentile_99(
                [int(sample.document["elapsed_ns"]) for sample in samples]
            ),
        }
        profiles.append(profile)

    ordinary = profiles[0]
    best = max(profiles[1:], key=lambda row: row["failures"])
    material_improvement = (
        best["failures"] >= max(2, ordinary["failures"] * 2)
        and best["detections_per_million_lane_executions"] >
            ordinary["detections_per_million_lane_executions"]
    )
    summary = {
        "schema_version": 1,
        "fault": fault,
        "coupling": coupling,
        "lane_budget_per_profile": lane_budget,
        "workers": workers,
        "rounds": rounds,
        "profiles": profiles,
        "material_improvement": material_improvement,
        "independence_assumed": False,
    }
    artifact_root.mkdir(parents=True, exist_ok=True)
    (artifact_root / "comparison.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n"
    )
    with (artifact_root / "comparison.csv").open("w", newline="") as stream:
        fieldnames = [
            "lanes", "processes", "failures", "process_failure_rate",
            "budgeted_lane_executions", "actual_lane_executions",
            "detections_per_million_lane_executions",
            "budgeted_executions_to_first_detection",
            "actual_executions_to_first_detection",
            "wall_ns_to_first_detection",
            "replay_rate", "signature_stability", "cleanup_failures",
            "mean_process_elapsed_ns", "p99_process_elapsed_ns",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames,
                                extrasaction="ignore")
        writer.writeheader()
        writer.writerows(profiles)
    return summary


def _positive_int(text: str) -> int:
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("value must be positive")
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="Run bounded LRPA samples")
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--artifact-root", type=pathlib.Path,
                        default=pathlib.Path("object/lrpa"))
    parser.add_argument("--seeds", type=_positive_int, default=4)
    parser.add_argument("--seed-base", type=int, default=1)
    parser.add_argument("--lanes", type=_positive_int, default=4)
    parser.add_argument("--workers", type=_positive_int, default=4)
    parser.add_argument("--rounds", type=_positive_int, default=16)
    parser.add_argument("--coupling", choices=sorted(COUPLINGS), default="ring")
    parser.add_argument("--queue-capacity", type=_positive_int, default=8192)
    parser.add_argument("--timeout-ms", type=_positive_int, default=2000)
    parser.add_argument("--process-timeout", type=float, default=10.0)
    parser.add_argument("--fault", choices=sorted(FAULTS), default="none")
    parser.add_argument("--compare-equal-cost", action="store_true")
    parser.add_argument("--lane-budget", type=_positive_int, default=64)
    args = parser.parse_args()

    if args.compare_equal_cost:
        summary = compare_equal_cost(
            args.binary, args.artifact_root, fault=args.fault,
            lane_budget=args.lane_budget, workers=args.workers,
            rounds=args.rounds, coupling=args.coupling,
            queue_capacity=args.queue_capacity, timeout_ms=args.timeout_ms,
            process_timeout=args.process_timeout, seed_base=args.seed_base,
        )
        print(json.dumps(summary, sort_keys=True))
        return 0

    samples: list[SampleResult] = []
    for offset in range(args.seeds):
        config = RunConfig(
            seed=args.seed_base + offset, lanes=args.lanes,
            workers=args.workers, rounds=args.rounds,
            coupling=args.coupling, queue_capacity=args.queue_capacity,
            timeout_ms=args.timeout_ms, fault=args.fault,
            allowed_outcomes=30, generate_perturbations=True,
        )
        samples.append(run_sample(
            args.binary, config, args.artifact_root,
            process_timeout=args.process_timeout, replay_failures=True,
        ))

    rows = [sample.document for sample in samples]
    summary_path = args.artifact_root / "campaign.json"
    summary_path.write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n")
    csv_path = args.artifact_root / "campaign.csv"
    with csv_path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=sorted(RESULT_FIELDS))
        writer.writeheader()
        writer.writerows(rows)
    print(json.dumps({
        "samples": len(samples),
        "failures": sum(row["status"] == "oracle_failure" for row in rows),
        "replayed": sum(sample.replayed for sample in samples),
        "artifacts": str(args.artifact_root),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
