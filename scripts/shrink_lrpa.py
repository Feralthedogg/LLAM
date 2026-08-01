#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import pathlib
import tempfile
from dataclasses import dataclass
from typing import Any, Callable, Sequence

from run_lrpa import (
    ResultSchemaError,
    SampleTimeout,
    execute_result_document,
)


@dataclass(frozen=True)
class CandidateEvaluation:
    matched: bool
    reason: str
    signature: str | None = None
    status: str | None = None


def candidate_matches_signature(
    command_prefix: Sequence[str], manifest: dict[str, Any],
    target_signature: str, *, timeout: float,
) -> CandidateEvaluation:
    with tempfile.TemporaryDirectory(prefix="lrpa-shrink-") as directory:
        manifest_path = pathlib.Path(directory) / "candidate.json"
        manifest_path.write_text(
            json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n"
        )
        command = [*map(str, command_prefix), "--manifest", str(manifest_path)]
        try:
            document, _, _ = execute_result_document(command, timeout=timeout)
        except SampleTimeout:
            return CandidateEvaluation(False, "timeout")
        except (OSError, ResultSchemaError):
            return CandidateEvaluation(False, "invalid_result")
    signature = document["signature"]
    if document["status"] != "oracle_failure":
        return CandidateEvaluation(False, "no_failure", signature,
                                   document["status"])
    if signature != target_signature:
        return CandidateEvaluation(False, "different_signature", signature,
                                   document["status"])
    return CandidateEvaluation(True, "signature_preserved", signature,
                               document["status"])


def _manifest_digest(manifest: dict[str, Any]) -> str:
    encoded = json.dumps(
        manifest, sort_keys=True, separators=(",", ":"), allow_nan=False,
    ).encode()
    return hashlib.sha256(encoded).hexdigest()


class ShrinkSession:
    def __init__(
        self, command_prefix: Sequence[str], target_signature: str,
        output_dir: pathlib.Path, timeout: float,
    ) -> None:
        self.command_prefix = [str(part) for part in command_prefix]
        self.target_signature = target_signature
        self.output_dir = output_dir
        self.timeout = timeout
        output_dir.mkdir(parents=True, exist_ok=True)
        self.log_path = output_dir / "shrink-log.jsonl"
        self.log_path.write_text("")
        self.attempt = 0

    def evaluate(self, phase: str, candidate: dict[str, Any]) -> bool:
        evaluation = candidate_matches_signature(
            self.command_prefix, candidate, self.target_signature,
            timeout=self.timeout,
        )
        self.attempt += 1
        row = {
            "attempt": self.attempt,
            "phase": phase,
            "accepted": evaluation.matched,
            "reason": evaluation.reason,
            "signature": evaluation.signature,
            "status": evaluation.status,
            "manifest_sha256": _manifest_digest(candidate),
            "dimensions": {
                "lane_count": candidate.get("lane_count"),
                "perturbation_count": len(candidate.get("perturbations", [])),
                "rounds": candidate.get("rounds"),
                "coupling": candidate.get("coupling"),
                "worker_count": candidate.get("worker_count"),
                "allowed_outcomes": candidate.get("allowed_outcomes"),
                "payload_size": candidate.get("payload_size"),
                "queue_capacity": candidate.get("queue_capacity"),
            },
        }
        with self.log_path.open("a") as stream:
            stream.write(json.dumps(row, sort_keys=True) + "\n")
        return evaluation.matched


def _minimize_integer(
    session: ShrinkSession, current: dict[str, Any], field: str,
    minimum: int, phase: str,
) -> dict[str, Any]:
    low = minimum
    high = int(current[field])
    best = current
    while low < high:
        middle = low + (high - low) // 2
        candidate = copy.deepcopy(best)
        candidate[field] = middle
        if session.evaluate(phase, candidate):
            best = candidate
            high = middle
        else:
            low = middle + 1
    if int(best[field]) != low:
        candidate = copy.deepcopy(best)
        candidate[field] = low
        if session.evaluate(phase, candidate):
            best = candidate
    return best


def _minimize_perturbations(
    session: ShrinkSession, current: dict[str, Any],
) -> dict[str, Any]:
    steps = list(current.get("perturbations", []))
    granularity = 2
    while len(steps) >= 2:
        chunk_size = math.ceil(len(steps) / granularity)
        accepted = False
        for begin in range(0, len(steps), chunk_size):
            candidate_steps = steps[:begin] + steps[begin + chunk_size:]
            candidate = copy.deepcopy(current)
            candidate["perturbations"] = candidate_steps
            if session.evaluate("perturbations", candidate):
                current = candidate
                steps = candidate_steps
                granularity = max(2, granularity - 1)
                accepted = True
                break
        if accepted:
            continue
        if granularity >= len(steps):
            break
        granularity = min(len(steps), granularity * 2)
    return current


def _simplify_coupling(
    session: ShrinkSession, current: dict[str, Any],
) -> dict[str, Any]:
    order = ["colored_graph", "ring", "shared_object", "independent"]
    coupling = current["coupling"]
    if coupling not in order:
        return current
    start = order.index(coupling)
    for simpler in order[start + 1:]:
        candidate = copy.deepcopy(current)
        candidate["coupling"] = simpler
        if session.evaluate("coupling", candidate):
            current = candidate
    return current


def _minimize_allowed_outcomes(
    session: ShrinkSession, current: dict[str, Any],
) -> dict[str, Any]:
    for bit in (2, 4, 8, 16):
        allowed = int(current["allowed_outcomes"])
        if (allowed & bit) == 0 or allowed == bit:
            continue
        candidate = copy.deepcopy(current)
        candidate["allowed_outcomes"] = allowed & ~bit
        if session.evaluate("allowed_outcomes", candidate):
            current = candidate
    return current


def _validate_input_manifest(manifest: dict[str, Any]) -> None:
    required = {
        "schema_version", "seed", "lane_count", "worker_count", "rounds",
        "coupling", "perturbations", "allowed_outcomes", "payload_size",
        "queue_capacity",
    }
    if set(manifest) != required:
        raise ValueError(
            f"manifest fields mismatch missing={sorted(required - set(manifest))} "
            f"unknown={sorted(set(manifest) - required)}"
        )
    if manifest["schema_version"] != 1:
        raise ValueError("unsupported manifest schema")
    for field in (
        "seed", "lane_count", "worker_count", "rounds", "allowed_outcomes",
        "payload_size", "queue_capacity",
    ):
        if type(manifest[field]) is not int or manifest[field] < 0:
            raise ValueError(f"{field} must be a non-negative integer")
    if type(manifest["perturbations"]) is not list:
        raise ValueError("perturbations must be a list")


def shrink_manifest(
    command_prefix: Sequence[str], original: dict[str, Any],
    target_signature: str, output_dir: pathlib.Path, *, timeout: float,
) -> dict[str, Any]:
    _validate_input_manifest(original)
    current = copy.deepcopy(original)
    session = ShrinkSession(command_prefix, target_signature, output_dir,
                            timeout)
    if not session.evaluate("baseline", current):
        raise RuntimeError("original manifest does not reproduce target signature")

    current = _minimize_integer(session, current, "lane_count", 1, "lanes")
    current = _minimize_perturbations(session, current)
    current = _minimize_integer(session, current, "rounds", 1, "rounds")
    current = _simplify_coupling(session, current)
    current = _minimize_integer(session, current, "worker_count", 1, "workers")
    current = _minimize_allowed_outcomes(session, current)
    current = _minimize_integer(session, current, "payload_size", 1, "payload")
    current = _minimize_integer(session, current, "queue_capacity", 1,
                                "queue_capacity")

    minimized_path = output_dir / "minimized-manifest.json"
    minimized_path.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
    if not session.evaluate("verify", current):
        raise RuntimeError("minimized manifest did not reproduce target signature")
    return current


def main() -> int:
    parser = argparse.ArgumentParser(description="Shrink an LRPA failure manifest")
    parser.add_argument("--binary", required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--signature", required=True)
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    original = json.loads(args.manifest.read_text())
    minimized = shrink_manifest(
        [args.binary], original, args.signature, args.output_dir,
        timeout=args.timeout,
    )
    print(json.dumps({
        "signature": args.signature,
        "minimized_manifest": str(args.output_dir / "minimized-manifest.json"),
        "lane_count": minimized["lane_count"],
        "perturbation_count": len(minimized["perturbations"]),
    }, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
