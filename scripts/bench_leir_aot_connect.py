# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Copyright 2026 Feralthedogg

"""Run, validate, and replay the split LEIR compiled-executor evidence."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import statistics
import subprocess
import sys
from collections.abc import Mapping, Sequence
from typing import Final


SAMPLE_PREFIX: Final = "LEIR_AOT_CONNECT_SAMPLE"
LINUX_PROFILES: Final = frozenset(
    {"submit_all", "coop_taskrun", "defer_taskrun"}
)
PORTABLE_PROFILE: Final = "portable_control"
CANDIDATES: Final = frozenset({"oracle", "portable", "linux"})
MIN_WINDOW_NS: Final = 1_000_000
MAX_RATIO_SPREAD: Final = 0.20
PERFORMANCE_LIMITS: Final = {
    "wall_ns": 1.05,
    "cpu_ns": 1.10,
    "p50_ns": 1.15,
    "p99_ns": 1.15,
}
COMMON_FIELDS: Final = (
    "version",
    "candidate",
    "transport",
    "workload",
    "process",
    "ring_profile",
    "block",
    "order",
    "seed",
    "concurrency",
    "payload",
    "activations",
    "logical_operations",
    "correctness",
    "result_checksum",
    "peer_checksum",
    "wall_ns",
    "cpu_ns",
    "p50_ns",
    "p99_ns",
    "interpreter_dispatches",
    "normalizations",
    "site_lookups",
    "parks",
    "wakes",
    "hot_allocations",
)
LINUX_FIELDS: Final = (
    "prepared_sqes",
    "observed_cqes",
    "suppressed_success_cqes",
    "queue_publications",
    "submit_syscalls",
)
FIELD_ORDER: Final = (*COMMON_FIELDS, *LINUX_FIELDS)
INTEGER_FIELDS: Final = frozenset(
    {
        "version",
        "block",
        "order",
        "seed",
        "concurrency",
        "payload",
        "activations",
        "logical_operations",
        "correctness",
        "wall_ns",
        "cpu_ns",
        "p50_ns",
        "p99_ns",
        "interpreter_dispatches",
        "normalizations",
        "site_lookups",
        "parks",
        "wakes",
        "hot_allocations",
        *LINUX_FIELDS,
    }
)
PAIR_FIELDS: Final = (
    "transport",
    "workload",
    "process",
    "ring_profile",
    "block",
    "seed",
    "concurrency",
    "payload",
    "activations",
)
CHECKSUM_PATTERN: Final = re.compile(r"[0-9a-f]{16}\Z")
REVISION_PATTERN: Final = re.compile(r"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
SHA256_PATTERN: Final = re.compile(r"[0-9a-f]{64}\Z")
ATTEMPT_FIELDS: Final = frozenset(
    {
        "execution_index",
        "candidate",
        "process",
        "ring_profile",
        "transport",
        "block",
        "order",
        "seed",
        "concurrency",
        "payload",
        "activations",
        "record",
        "error",
    }
)


def _integer(name: str, value: object) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{name} must be an integer")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and re.fullmatch(r"[0-9]+", value):
        return int(value)
    raise ValueError(f"{name} must be an integer")


def _expected_fields(candidate: str) -> tuple[str, ...]:
    if candidate not in CANDIDATES:
        raise ValueError("candidate must be oracle, portable, or linux")
    return FIELD_ORDER if candidate == "linux" else COMMON_FIELDS


def _validate_sample(
    record: Mapping[str, object], *, candidate: str | None = None
) -> dict[str, object]:
    observed_candidate = record.get("candidate")
    if not isinstance(observed_candidate, str):
        raise ValueError("missing fields: candidate")
    expected = _expected_fields(observed_candidate)
    missing = [field for field in expected if field not in record]
    if missing:
        raise ValueError(f"missing fields: {', '.join(missing)}")
    unexpected = sorted(set(record) - set(expected))
    if unexpected:
        raise ValueError(f"unexpected fields: {', '.join(unexpected)}")

    validated = dict(record)
    for field in INTEGER_FIELDS & set(expected):
        validated[field] = _integer(field, validated[field])

    if validated["version"] != 3:
        raise ValueError("version must be 3")
    if candidate is not None and observed_candidate != candidate:
        raise ValueError(f"expected {candidate} candidate")
    if validated["transport"] not in {"tcp", "unix"}:
        raise ValueError("transport must be tcp or unix")
    if validated["workload"] != "connect_write":
        raise ValueError("workload must be connect_write")
    process = validated["process"]
    profile = validated["ring_profile"]
    if process == "portable":
        if observed_candidate not in {"oracle", "portable"}:
            raise ValueError("process portable requires oracle or portable")
        if profile != PORTABLE_PROFILE:
            raise ValueError("ring_profile must be portable_control")
    elif process == "linux":
        if observed_candidate not in {"portable", "linux"}:
            raise ValueError("process linux requires portable or linux")
        if profile not in LINUX_PROFILES:
            raise ValueError("ring_profile is not a Linux profile")
    else:
        raise ValueError("process must be portable or linux")

    if validated["order"] not in {0, 1}:
        raise ValueError("order must be 0 or 1")
    for field in ("block", "seed"):
        if validated[field] < 0:
            raise ValueError(f"{field} must be non-negative")
    if not 1 <= validated["concurrency"] <= 256:
        raise ValueError("concurrency is outside the supported range")
    if not 8 <= validated["payload"] <= 65536:
        raise ValueError("payload is outside the supported range")
    if validated["activations"] < validated["concurrency"]:
        raise ValueError("activations must be at least concurrency")
    for field in (
        "logical_operations",
        "wall_ns",
        "cpu_ns",
        "p50_ns",
        "p99_ns",
    ):
        if validated[field] <= 0:
            raise ValueError(f"{field} must be a positive integer")
    if validated["p50_ns"] > validated["p99_ns"]:
        raise ValueError("p50_ns cannot exceed p99_ns")
    if validated["correctness"] not in {0, 1}:
        raise ValueError("correctness must be 0 or 1")
    for field in (
        "interpreter_dispatches",
        "normalizations",
        "site_lookups",
        "parks",
        "wakes",
        "hot_allocations",
        *LINUX_FIELDS,
    ):
        if field in validated and validated[field] < 0:
            raise ValueError(f"{field} must be non-negative")
    for field in ("result_checksum", "peer_checksum"):
        value = validated[field]
        if not isinstance(value, str) or CHECKSUM_PATTERN.fullmatch(value) is None:
            raise ValueError(
                f"{field} must be 16 lowercase hexadecimal characters"
            )
    return validated


def parse_sample(line: str) -> dict[str, object]:
    tokens = line.strip().split()
    if not tokens or tokens[0] != SAMPLE_PREFIX:
        raise ValueError(f"sample must start with {SAMPLE_PREFIX}")
    record: dict[str, object] = {}
    for token in tokens[1:]:
        if "=" not in token:
            raise ValueError(f"malformed field: {token}")
        name, value = token.split("=", 1)
        if name in record:
            raise ValueError(f"duplicate field: {name}")
        if name not in FIELD_ORDER:
            raise ValueError(f"unexpected field: {name}")
        record[name] = value
    return _validate_sample(record)


def format_sample(record: Mapping[str, object]) -> str:
    validated = _validate_sample(record)
    fields = _expected_fields(str(validated["candidate"]))
    encoded = " ".join(f"{name}={validated[name]}" for name in fields)
    return f"{SAMPLE_PREFIX} {encoded}"


def _pair_key(record: Mapping[str, object]) -> tuple[object, ...]:
    return tuple(record[field] for field in PAIR_FIELDS)


def _paired_samples(
    left_samples: Sequence[Mapping[str, object]],
    right_samples: Sequence[Mapping[str, object]],
    *,
    left_candidate: str,
    right_candidate: str,
    process: str,
) -> list[tuple[dict[str, object], dict[str, object]]]:
    if len(left_samples) != len(right_samples):
        raise ValueError("paired sample counts differ")
    if not left_samples:
        raise ValueError("sample count must be positive")

    def indexed(
        values: Sequence[Mapping[str, object]], expected_candidate: str
    ) -> dict[tuple[object, ...], dict[str, object]]:
        result: dict[tuple[object, ...], dict[str, object]] = {}
        for raw in values:
            record = _validate_sample(raw, candidate=expected_candidate)
            if record["process"] != process:
                raise ValueError("paired sample process differs")
            key = _pair_key(record)
            if key in result:
                raise ValueError("duplicate paired sample")
            result[key] = record
        return result

    left = indexed(left_samples, left_candidate)
    right = indexed(right_samples, right_candidate)
    if set(left) != set(right):
        raise ValueError("paired sample cells differ")
    pairs = []
    for key in sorted(left, key=lambda item: tuple(map(str, item))):
        if left[key]["order"] == right[key]["order"]:
            raise ValueError("paired sample order is not distinct")
        pairs.append((left[key], right[key]))
    return pairs


def balanced_order(
    candidates: Sequence[str], samples_per_candidate: int
) -> list[str]:
    if len(candidates) != 2 or len(set(candidates)) != 2:
        raise ValueError("balanced order requires two distinct candidates")
    if (
        isinstance(samples_per_candidate, bool)
        or not isinstance(samples_per_candidate, int)
        or samples_per_candidate <= 0
    ):
        raise ValueError("sample count must be a positive integer")
    left, right = candidates
    pattern = (left, right, right, left)
    counts = {left: 0, right: 0}
    order: list[str] = []
    index = 0
    while min(counts.values()) < samples_per_candidate:
        candidate = pattern[index % len(pattern)]
        index += 1
        if counts[candidate] >= samples_per_candidate:
            continue
        order.append(candidate)
        counts[candidate] += 1
    return order


def benchmark_command(
    binary: Path,
    *,
    candidate: str,
    process: str,
    ring_profile: str,
    transport: str,
    block: int,
    order: int,
    seed: int,
    concurrency: int,
    payload: int,
    activations: int,
) -> list[str]:
    probe: dict[str, object] = {
        "version": 3,
        "candidate": candidate,
        "transport": transport,
        "workload": "connect_write",
        "process": process,
        "ring_profile": ring_profile,
        "block": block,
        "order": order,
        "seed": seed,
        "concurrency": concurrency,
        "payload": payload,
        "activations": activations,
        "logical_operations": 1,
        "correctness": 1,
        "result_checksum": "0000000000000000",
        "peer_checksum": "0000000000000000",
        "wall_ns": 1,
        "cpu_ns": 1,
        "p50_ns": 1,
        "p99_ns": 1,
        "interpreter_dispatches": 0,
        "normalizations": 0,
        "site_lookups": 0,
        "parks": 0,
        "wakes": 0,
        "hot_allocations": 0,
    }
    if candidate == "linux":
        probe.update({field: 0 for field in LINUX_FIELDS})
    _validate_sample(probe)
    return [
        str(binary.resolve()),
        "--candidate",
        candidate,
        "--process",
        process,
        "--ring-profile",
        ring_profile,
        "--transport",
        transport,
        "--block",
        str(block),
        "--order",
        str(order),
        "--seed",
        str(seed),
        "--concurrency",
        str(concurrency),
        "--payload",
        str(payload),
        "--activations",
        str(activations),
    ]


def _bootstrap_median_interval(values: Sequence[float]) -> tuple[float, float]:
    if len(values) == 1:
        return values[0], values[0]
    generator = random.Random(0x4C454952)
    medians = []
    for _ in range(4096):
        resample = [values[generator.randrange(len(values))] for _ in values]
        medians.append(float(statistics.median(resample)))
    medians.sort()
    lower = medians[int((len(medians) - 1) * 0.025)]
    upper = medians[int((len(medians) - 1) * 0.975)]
    return lower, upper


def _performance_evidence(
    pairs: Sequence[tuple[Mapping[str, object], Mapping[str, object]]]
) -> dict[str, object]:
    reasons: set[str] = set()
    evidence: dict[str, object] = {}
    if any(
        int(base["wall_ns"]) < MIN_WINDOW_NS
        or int(candidate["wall_ns"]) < MIN_WINDOW_NS
        for base, candidate in pairs
    ):
        reasons.add("minimum_duration")
    for metric, limit in PERFORMANCE_LIMITS.items():
        ratios = [
            int(candidate[metric]) / int(base[metric])
            for base, candidate in pairs
        ]
        median = float(statistics.median(ratios))
        low, high = _bootstrap_median_interval(ratios)
        spread = (max(ratios) - min(ratios)) / median
        label = metric.removesuffix("_ns")
        evidence[f"{label}_ratio"] = median
        evidence[f"{label}_ratio_ci_low"] = low
        evidence[f"{label}_ratio_ci_high"] = high
        evidence[f"{label}_ratio_spread"] = spread
        if spread > MAX_RATIO_SPREAD:
            reasons.add("ratio_spread")
        elif low > limit:
            reasons.add(metric)
        elif high > limit:
            reasons.add(f"{label}_confidence")
    hard = reasons & set(PERFORMANCE_LIMITS)
    verdict = "FAIL" if hard else "INCONCLUSIVE" if reasons else "PASS"
    return {
        "performance_verdict": verdict,
        "performance_reasons": sorted(reasons),
        **evidence,
    }


def _compiled_common_reasons(
    record: Mapping[str, object], *, exact_parks: bool
) -> set[str]:
    activations = int(record["activations"])
    reasons: set[str] = set()
    expected = {
        "logical_operations": activations * 2,
        "correctness": 1,
        "interpreter_dispatches": 0,
        "normalizations": activations,
        "site_lookups": activations,
        "hot_allocations": 0,
    }
    for field, value in expected.items():
        if record[field] != value:
            reasons.add(field)
    parks = int(record["parks"])
    wakes = int(record["wakes"])
    if exact_parks:
        if parks != activations:
            reasons.add("parks")
        if wakes != activations:
            reasons.add("wakes")
    else:
        if not activations <= parks <= activations * 2:
            reasons.add("parks")
        if wakes != parks:
            reasons.add("wakes")
    return reasons


def _finalize_classifier(
    pairs: Sequence[tuple[Mapping[str, object], Mapping[str, object]]],
    reasons: set[str],
) -> dict[str, object]:
    performance = _performance_evidence(pairs)
    correctness = "PASS" if not reasons else "FAIL"
    verdict = (
        "FAIL"
        if correctness == "FAIL"
        else str(performance["performance_verdict"])
    )
    return {
        "verdict": verdict,
        "correctness_verdict": correctness,
        "reasons": sorted(reasons),
        "sample_count": len(pairs),
        **performance,
    }


def classify_portable(
    oracle_samples: Sequence[Mapping[str, object]],
    portable_samples: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    pairs = _paired_samples(
        oracle_samples,
        portable_samples,
        left_candidate="oracle",
        right_candidate="portable",
        process="portable",
    )
    reasons: set[str] = set()
    for oracle, portable in pairs:
        activations = int(oracle["activations"])
        if oracle["correctness"] != 1:
            reasons.add("correctness")
        if oracle["logical_operations"] != activations * 2:
            reasons.add("logical_operations")
        if oracle["interpreter_dispatches"] != activations * 3:
            reasons.add("interpreter_dispatches")
        if oracle["normalizations"] != 0:
            reasons.add("normalizations")
        if oracle["site_lookups"] != 0:
            reasons.add("site_lookups")
        if oracle["parks"] != activations or oracle["wakes"] != activations:
            reasons.add("ownership")
        reasons.update(_compiled_common_reasons(portable, exact_parks=False))
        for field in ("result_checksum", "peer_checksum"):
            if oracle[field] != portable[field]:
                reasons.add(field)
    return _finalize_classifier(pairs, reasons)


def classify_linux(
    portable_samples: Sequence[Mapping[str, object]],
    linux_samples: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    pairs = _paired_samples(
        portable_samples,
        linux_samples,
        left_candidate="portable",
        right_candidate="linux",
        process="linux",
    )
    reasons: set[str] = set()
    for portable, linux in pairs:
        activations = int(linux["activations"])
        reasons.update(_compiled_common_reasons(portable, exact_parks=False))
        reasons.update(_compiled_common_reasons(linux, exact_parks=True))
        expected = {
            "prepared_sqes": activations * 2,
            "observed_cqes": activations,
            "suppressed_success_cqes": activations,
            "queue_publications": activations,
        }
        for field, value in expected.items():
            if linux[field] != value:
                reasons.add(field)
        submit_syscalls = int(linux["submit_syscalls"])
        if not 1 <= submit_syscalls <= int(linux["prepared_sqes"]):
            reasons.add("submit_syscalls")
        for field in ("result_checksum", "peer_checksum"):
            if portable[field] != linux[field]:
                reasons.add(field)
    return _finalize_classifier(pairs, reasons)


def combine_verdicts(
    portable: Mapping[str, object], linux: Mapping[str, object]
) -> dict[str, object]:
    portable_verdict = str(portable.get("verdict", "INCONCLUSIVE"))
    linux_verdict = str(linux.get("verdict", "INCONCLUSIVE"))
    overall = (
        portable_verdict
        if portable_verdict in {"PASS", "FAIL"}
        else "INCONCLUSIVE"
    )
    return {
        "overall_verdict": overall,
        "linux_effective_verdict": (
            linux_verdict if portable_verdict == "PASS" else "BLOCKED"
        ),
    }


def _command_text(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return "unavailable"
    output = completed.stdout.strip() or completed.stderr.strip()
    return output.splitlines()[0] if output else "unavailable"


def _source_tree_digest(root: Path) -> str:
    completed = subprocess.run(
        [
            "git",
            "ls-files",
            "--cached",
            "--others",
            "--exclude-standard",
            "-z",
        ],
        cwd=root,
        check=False,
        capture_output=True,
    )
    if completed.returncode != 0:
        raise ValueError("cannot enumerate the source snapshot")
    digest = hashlib.sha256()
    raw_paths = sorted(completed.stdout.split(b"\0"))
    if not any(raw_paths):
        raise ValueError("source snapshot is empty")
    for raw_path in raw_paths:
        if not raw_path:
            continue
        path = root / os.fsdecode(raw_path)
        if not path.is_file():
            continue
        digest.update(raw_path)
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def _metadata(binary: Path, root: Path) -> dict[str, object]:
    revision_result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    status_result = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    revision_override = os.environ.get("LLAM_SOURCE_REVISION")
    digest_override = os.environ.get("LLAM_SOURCE_TREE_DIGEST_SHA256")
    dirty_override = os.environ.get("LLAM_SOURCE_TREE_DIRTY")
    revision = (
        revision_override
        if revision_override is not None
        else revision_result.stdout.strip()
    )
    if REVISION_PATTERN.fullmatch(revision) is None:
        raise ValueError("source revision is unavailable or invalid")
    if digest_override is not None and SHA256_PATTERN.fullmatch(digest_override) is None:
        raise ValueError("source tree digest override is invalid")
    if dirty_override not in {None, "0", "1"}:
        raise ValueError("source tree dirty override must be 0 or 1")
    if dirty_override is None and status_result.returncode != 0:
        raise ValueError("source tree status is unavailable")
    return {
        "schema_version": 3,
        "scope": "PORTABLE_AND_LINUX_SEPARATE",
        "release_authorized": False,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_revision": revision,
        "source_tree_digest_sha256": (
            digest_override or _source_tree_digest(root)
        ),
        "source_tree_dirty": (
            dirty_override == "1"
            if dirty_override is not None
            else bool(status_result.stdout)
        ),
        "binary": str(binary.resolve()),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "system": platform.system(),
        "kernel": platform.release(),
        "machine": platform.machine(),
        "processor": platform.processor(),
        "python": platform.python_version(),
        "compiler": _command_text(["cc", "--version"]),
        "liburing": _command_text(
            ["pkg-config", "--modversion", "liburing"]
        ),
    }


def _run_sample(
    command: Sequence[str], *, timeout_seconds: int
) -> tuple[dict[str, object] | None, str | None]:
    try:
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired:
        return None, "timeout"
    lines = [
        line
        for line in completed.stdout.splitlines()
        if line.startswith(f"{SAMPLE_PREFIX} ")
    ]
    if completed.returncode == 77:
        return None, f"skip: {completed.stderr.strip() or 'platform unavailable'}"
    if len(lines) != 1:
        detail = completed.stderr.strip() or f"expected one sample line, got {len(lines)}"
        return None, f"exit {completed.returncode}: {detail}"
    try:
        record = parse_sample(lines[0])
    except ValueError as error:
        return None, f"invalid sample: {error}"
    if completed.returncode != 0 and record["correctness"] == 1:
        return None, f"exit {completed.returncode} with success sample"
    return record, None


def _skip_class(error: str) -> str | None:
    if not error.startswith("skip:"):
        return None
    detail = error.removeprefix("skip:").strip().lower()
    if "temporarily unavailable" in detail:
        return "transient"
    if any(marker in detail for marker in ("not supported", "unavailable", "enosys")):
        return "unsupported"
    if any(marker in detail for marker in ("permission denied", "not permitted")):
        return "permission"
    return "platform"


def _seed_for(
    process: str,
    profile: str,
    transport: str,
    concurrency: int,
    payload: int,
    block: int,
) -> int:
    encoded = (
        f"{process}|{profile}|{transport}|{concurrency}|{payload}|{block}"
    ).encode("ascii")
    return int.from_bytes(hashlib.sha256(encoded).digest()[:8], "big")


def _attempt(
    *,
    execution_index: int,
    candidate: str,
    process: str,
    profile: str,
    transport: str,
    block: int,
    order: int,
    seed: int,
    concurrency: int,
    payload: int,
    activations: int,
    record: Mapping[str, object] | None,
    error: str | None,
) -> dict[str, object]:
    return {
        "execution_index": execution_index,
        "candidate": candidate,
        "process": process,
        "ring_profile": profile,
        "transport": transport,
        "block": block,
        "order": order,
        "seed": seed,
        "concurrency": concurrency,
        "payload": payload,
        "activations": activations,
        "record": dict(record) if record is not None else None,
        "error": error,
    }


def _validate_attempt(raw: Mapping[str, object]) -> dict[str, object]:
    if set(raw) != ATTEMPT_FIELDS:
        raise ValueError("attempt fields do not match schema")
    attempt = dict(raw)
    for field in (
        "execution_index",
        "block",
        "order",
        "seed",
        "concurrency",
        "payload",
        "activations",
    ):
        attempt[field] = _integer(field, attempt[field])
    record_raw = attempt["record"]
    error = attempt["error"]
    if (record_raw is None) == (error is None):
        raise ValueError("attempt must contain exactly one record or error")
    if error is not None and not isinstance(error, str):
        raise ValueError("attempt error must be text")
    if record_raw is not None:
        if not isinstance(record_raw, Mapping):
            raise ValueError("attempt record must be an object")
        record = _validate_sample(
            record_raw, candidate=str(attempt["candidate"])
        )
        identity = {
            "process": attempt["process"],
            "ring_profile": attempt["ring_profile"],
            "transport": attempt["transport"],
            "block": attempt["block"],
            "order": attempt["order"],
            "seed": attempt["seed"],
            "concurrency": attempt["concurrency"],
            "payload": attempt["payload"],
            "activations": attempt["activations"],
        }
        if any(record[field] != value for field, value in identity.items()):
            raise ValueError("attempt record identity mismatch")
        attempt["record"] = record
    return attempt


def _cell_label(key: tuple[object, ...]) -> str:
    process, profile, transport, concurrency, payload, activations = key
    return (
        f"{process}/{profile}/{transport}/c{concurrency}/"
        f"p{payload}/a{activations}"
    )


def _metric_median(cells: Sequence[Mapping[str, object]], field: str) -> float | None:
    values = [float(cell[field]) for cell in cells if field in cell]
    return float(statistics.median(values)) if values else None


def _aggregate_cells(cells: Sequence[Mapping[str, object]]) -> dict[str, object]:
    verdicts = {str(cell["verdict"]) for cell in cells}
    correctness_verdicts = {
        str(cell.get("correctness_verdict", "INCONCLUSIVE"))
        for cell in cells
    }
    performance_verdicts = {
        str(cell.get("performance_verdict", "INCONCLUSIVE"))
        for cell in cells
    }
    if "FAIL" in correctness_verdicts:
        correctness = "FAIL"
    elif correctness_verdicts == {"PASS"}:
        correctness = "PASS"
    else:
        correctness = "INCONCLUSIVE"
    if "FAIL" in performance_verdicts:
        performance = "FAIL"
    elif performance_verdicts == {"PASS"}:
        performance = "PASS"
    else:
        performance = "INCONCLUSIVE"
    if correctness == "FAIL" or performance == "FAIL":
        verdict = "FAIL"
    elif correctness == "PASS" and performance == "PASS":
        verdict = "PASS"
    elif verdicts == {"UNAVAILABLE"}:
        verdict = "UNAVAILABLE"
    else:
        verdict = "INCONCLUSIVE"
    reasons = sorted(
        {
            str(reason)
            for cell in cells
            for reason in (*cell.get("reasons", []), *cell.get("performance_reasons", []))
        }
    )
    return {
        "verdict": verdict,
        "correctness_verdict": correctness,
        "performance_verdict": performance,
        "reasons": reasons,
        "sample_count": sum(int(cell.get("sample_count", 0)) for cell in cells),
        "wall_ratio": _metric_median(cells, "wall_ratio"),
        "cpu_ratio": _metric_median(cells, "cpu_ratio"),
        "p50_ratio": _metric_median(cells, "p50_ratio"),
        "p99_ratio": _metric_median(cells, "p99_ratio"),
        "cells": list(cells),
    }


def _classify_attempt_cell(
    attempts: Sequence[Mapping[str, object]], process: str
) -> dict[str, object]:
    errors = [str(item["error"]) for item in attempts if item["error"] is not None]
    records = [item["record"] for item in attempts if item["record"] is not None]
    skip_classes = [_skip_class(error) for error in errors]
    if not records and errors and all(value is not None for value in skip_classes) and len(set(skip_classes)) == 1:
        return {
            "verdict": "UNAVAILABLE",
            "correctness_verdict": "INCONCLUSIVE",
            "performance_verdict": "INCONCLUSIVE",
            "reasons": sorted(set(errors)),
            "performance_reasons": [],
            "sample_count": 0,
        }
    expected_candidates = (
        ("oracle", "portable")
        if process == "portable"
        else ("portable", "linux")
    )
    by_candidate = {
        candidate: [
            record
            for record in records
            if record is not None and record["candidate"] == candidate
        ]
        for candidate in expected_candidates
    }
    expected_count = len(attempts) // 2
    if process == "linux" and not by_candidate["linux"]:
        linux_errors = [
            str(item["error"])
            for item in attempts
            if item["candidate"] == "linux" and item["error"] is not None
        ]
        linux_skip_classes = [_skip_class(error) for error in linux_errors]
        if (
            len(linux_errors) == expected_count
            and all(value is not None for value in linux_skip_classes)
            and len(set(linux_skip_classes)) == 1
        ):
            return {
                "verdict": "UNAVAILABLE",
                "correctness_verdict": "INCONCLUSIVE",
                "performance_verdict": "INCONCLUSIVE",
                "reasons": sorted(set(linux_errors)),
                "performance_reasons": [],
                "sample_count": 0,
            }
    if errors or any(len(rows) != expected_count for rows in by_candidate.values()):
        return {
            "verdict": "INCONCLUSIVE",
            "correctness_verdict": "INCONCLUSIVE",
            "performance_verdict": "INCONCLUSIVE",
            "reasons": sorted(set(errors or ["incomplete sample count"])),
            "performance_reasons": [],
            "sample_count": min(len(rows) for rows in by_candidate.values()),
        }
    if process == "portable":
        return classify_portable(
            by_candidate["oracle"], by_candidate["portable"]
        )
    return classify_linux(by_candidate["portable"], by_candidate["linux"])


def _project_attempts(
    attempts_raw: Sequence[Mapping[str, object]],
    *,
    profiles: Sequence[str],
) -> tuple[dict[str, object], list[dict[str, object]]]:
    attempts = [_validate_attempt(raw) for raw in attempts_raw]
    if [item["execution_index"] for item in attempts] != list(range(len(attempts))):
        raise ValueError("attempt execution indexes are not contiguous")
    grouped: dict[tuple[object, ...], list[dict[str, object]]] = {}
    for attempt in attempts:
        key = (
            attempt["process"],
            attempt["ring_profile"],
            attempt["transport"],
            attempt["concurrency"],
            attempt["payload"],
            attempt["activations"],
        )
        grouped.setdefault(key, []).append(attempt)

    portable_cells: list[dict[str, object]] = []
    linux_cells: dict[str, list[dict[str, object]]] = {
        profile: [] for profile in profiles
    }
    summary_rows: list[dict[str, object]] = []
    for key in sorted(grouped, key=lambda value: tuple(map(str, value))):
        process = str(key[0])
        profile = str(key[1])
        result = _classify_attempt_cell(grouped[key], process)
        result = {"cell": _cell_label(key), **result}
        if process == "portable":
            portable_cells.append(result)
        else:
            if profile not in linux_cells:
                raise ValueError("attempt references an undeclared profile")
            linux_cells[profile].append(result)
        summary_rows.append(
            {
                "process": process,
                "ring_profile": profile,
                "cell": result["cell"],
                "verdict": result["verdict"],
                "correctness_verdict": result["correctness_verdict"],
                "performance_verdict": result["performance_verdict"],
                "reasons": ";".join(
                    str(reason)
                    for reason in (*result["reasons"], *result["performance_reasons"])
                ),
                "wall_ratio": result.get("wall_ratio", ""),
                "cpu_ratio": result.get("cpu_ratio", ""),
                "p50_ratio": result.get("p50_ratio", ""),
                "p99_ratio": result.get("p99_ratio", ""),
                "sample_count": result["sample_count"],
            }
        )
    if not portable_cells:
        raise ValueError("portable comparison axis is missing")
    portable = _aggregate_cells(portable_cells)
    linux_profiles = {
        profile: _aggregate_cells(cells)
        if cells
        else {
            "verdict": "INCONCLUSIVE",
            "correctness_verdict": "INCONCLUSIVE",
            "performance_verdict": "INCONCLUSIVE",
            "reasons": ["profile missing"],
            "sample_count": 0,
            "wall_ratio": None,
            "cpu_ratio": None,
            "p50_ratio": None,
            "p99_ratio": None,
            "cells": [],
        }
        for profile, cells in linux_cells.items()
    }
    for result in linux_profiles.values():
        result["effective_verdict"] = combine_verdicts(portable, result)[
            "linux_effective_verdict"
        ]
    recommended = [
        (
            float(result["cpu_ratio"]),
            float(result["p99_ratio"]),
            float(result["wall_ratio"]),
            profile,
        )
        for profile, result in linux_profiles.items()
        if result["verdict"] == "PASS"
        and result["cpu_ratio"] is not None
        and result["p99_ratio"] is not None
        and result["wall_ratio"] is not None
    ]
    overall = combine_verdicts(
        portable,
        linux_profiles.get("submit_all", {"verdict": "INCONCLUSIVE"}),
    )["overall_verdict"]
    verdict = {
        "schema_version": 3,
        "scope": "PORTABLE_AND_LINUX_SEPARATE",
        "portable": portable,
        "linux_profiles": linux_profiles,
        "overall_verdict": overall,
        "recommended_linux_profile": min(recommended)[3] if recommended else None,
        "release_authorized": False,
    }
    return verdict, summary_rows


def _json_text(value: object) -> str:
    return json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n"


def _csv_text(rows: Sequence[Mapping[str, object]], fields: Sequence[str]) -> str:
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(stream, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow({field: row.get(field, "") for field in fields})
    return stream.getvalue()


def _raw_rows(attempts: Sequence[Mapping[str, object]]) -> list[dict[str, object]]:
    return [
        {"execution_index": item["execution_index"], **dict(item["record"])}
        for item in attempts
        if item["record"] is not None
    ]


def _summary_markdown(verdict: Mapping[str, object]) -> str:
    portable = verdict["portable"]
    assert isinstance(portable, Mapping)
    lines = [
        "# LEIR compiled executor evidence",
        "",
        f"- Portable compiled verdict: **{portable['verdict']}**",
        f"- Overall portable direction: **{verdict['overall_verdict']}**",
        f"- Recommended Linux profile: **{verdict['recommended_linux_profile'] or 'none'}**",
        "- Release authorized: **no**",
        "",
        "| Axis | Profile | Correctness | Performance | Raw verdict | Effective verdict | Wall | CPU | p50 | p99 |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|",
        (
            f"| portable | portable_control | "
            f"{portable['correctness_verdict']} | "
            f"{portable['performance_verdict']} | {portable['verdict']} | "
            f"{portable['verdict']} | {portable['wall_ratio']} | "
            f"{portable['cpu_ratio']} | {portable['p50_ratio']} | "
            f"{portable['p99_ratio']} |"
        ),
    ]
    profiles = verdict["linux_profiles"]
    assert isinstance(profiles, Mapping)
    for profile, result_raw in profiles.items():
        assert isinstance(result_raw, Mapping)
        lines.append(
            f"| Linux | {profile} | {result_raw['correctness_verdict']} | "
            f"{result_raw['performance_verdict']} | {result_raw['verdict']} | "
            f"{result_raw['effective_verdict']} | {result_raw['wall_ratio']} | "
            f"{result_raw['cpu_ratio']} | {result_raw['p50_ratio']} | "
            f"{result_raw['p99_ratio']} |"
        )
    lines.extend(
        [
            "",
            "Linux specialization is evaluated separately and cannot upgrade a "
            "failed or inconclusive portable result.",
            "",
            "No result authorizes production promotion, a version bump, tagging, "
            "packaging, or a 3.0.0 release.",
            "",
        ]
    )
    return "\n".join(lines)


def _write_projections(
    output_dir: Path,
    attempts: Sequence[Mapping[str, object]],
    verdict: Mapping[str, object],
    summary_rows: Sequence[Mapping[str, object]],
) -> None:
    raw_fields = ("execution_index", *FIELD_ORDER)
    summary_fields = (
        "process",
        "ring_profile",
        "cell",
        "verdict",
        "correctness_verdict",
        "performance_verdict",
        "reasons",
        "wall_ratio",
        "cpu_ratio",
        "p50_ratio",
        "p99_ratio",
        "sample_count",
    )
    (output_dir / "attempts.json").write_text(_json_text(attempts), encoding="utf-8")
    (output_dir / "raw.csv").write_text(
        _csv_text(_raw_rows(attempts), raw_fields), encoding="utf-8"
    )
    (output_dir / "summary.csv").write_text(
        _csv_text(summary_rows, summary_fields), encoding="utf-8"
    )
    (output_dir / "verdict.json").write_text(_json_text(verdict), encoding="utf-8")
    (output_dir / "summary.md").write_text(
        _summary_markdown(verdict), encoding="utf-8"
    )


def _parse_csv_choices(value: str, *, allowed: set[str], name: str) -> list[str]:
    choices = value.split(",")
    if not choices or any(choice not in allowed for choice in choices):
        raise ValueError(f"invalid {name} list")
    if len(set(choices)) != len(choices):
        raise ValueError(f"duplicate {name} entry")
    return choices


def _parse_csv_integers(value: str, *, name: str) -> list[int]:
    try:
        values = [int(item, 10) for item in value.split(",")]
    except ValueError as error:
        raise ValueError(f"invalid {name} list") from error
    if not values or any(item <= 0 for item in values):
        raise ValueError(f"invalid {name} list")
    if len(set(values)) != len(values):
        raise ValueError(f"duplicate {name} entry")
    return values


def _validated_metadata_parameters(
    metadata: Mapping[str, object],
) -> dict[str, object]:
    if metadata.get("schema_version") != 3:
        raise ValueError("metadata schema version is invalid")
    if metadata.get("scope") != "PORTABLE_AND_LINUX_SEPARATE":
        raise ValueError("metadata scope is invalid")
    if metadata.get("release_authorized") is not False:
        raise ValueError("metadata must not authorize a release")
    revision = metadata.get("source_revision")
    digest = metadata.get("source_tree_digest_sha256")
    binary_digest = metadata.get("binary_sha256")
    if not isinstance(revision, str) or REVISION_PATTERN.fullmatch(revision) is None:
        raise ValueError("metadata source revision is invalid")
    for name, value in (
        ("source tree", digest),
        ("binary", binary_digest),
    ):
        if not isinstance(value, str) or SHA256_PATTERN.fullmatch(value) is None:
            raise ValueError(f"metadata {name} digest is invalid")
    if not isinstance(metadata.get("source_tree_dirty"), bool):
        raise ValueError("metadata source tree state is invalid")

    raw = metadata.get("parameters")
    expected_fields = {
        "profiles",
        "transports",
        "concurrencies",
        "payloads",
        "activations",
        "samples_per_candidate",
        "order",
        "minimum_window_ns",
        "maximum_ratio_spread",
    }
    if not isinstance(raw, Mapping) or set(raw) != expected_fields:
        raise ValueError("metadata parameters do not match the schema")
    profiles = raw["profiles"]
    transports = raw["transports"]
    concurrencies = raw["concurrencies"]
    payloads = raw["payloads"]
    if (
        not isinstance(profiles, list)
        or not profiles
        or any(not isinstance(value, str) for value in profiles)
        or len(set(profiles)) != len(profiles)
        or any(value not in LINUX_PROFILES for value in profiles)
        or profiles.count("submit_all") != 1
    ):
        raise ValueError("metadata profiles are invalid")
    if (
        not isinstance(transports, list)
        or not transports
        or any(not isinstance(value, str) for value in transports)
        or len(set(transports)) != len(transports)
        or any(value not in {"tcp", "unix"} for value in transports)
    ):
        raise ValueError("metadata transports are invalid")

    def integer_list(name: str, values: object) -> list[int]:
        if not isinstance(values, list) or not values:
            raise ValueError(f"metadata {name} are invalid")
        normalized = [_integer(name, value) for value in values]
        if len(set(normalized)) != len(normalized):
            raise ValueError(f"metadata {name} contain duplicates")
        return normalized

    normalized_concurrency = integer_list("concurrencies", concurrencies)
    normalized_payloads = integer_list("payloads", payloads)
    activations = _integer("activations", raw["activations"])
    samples = _integer("samples_per_candidate", raw["samples_per_candidate"])
    if any(not 1 <= value <= 256 for value in normalized_concurrency):
        raise ValueError("metadata concurrency is outside the supported range")
    if any(not 8 <= value <= 65536 for value in normalized_payloads):
        raise ValueError("metadata payload is outside the supported range")
    if samples <= 0 or activations < max(normalized_concurrency):
        raise ValueError("metadata measurement counts are invalid")
    spread = raw["maximum_ratio_spread"]
    if (
        raw["order"] != "ABBA_BAAB"
        or _integer("minimum_window_ns", raw["minimum_window_ns"])
        != MIN_WINDOW_NS
        or isinstance(spread, bool)
        or not isinstance(spread, (int, float))
        or not math.isfinite(float(spread))
        or float(spread) != MAX_RATIO_SPREAD
    ):
        raise ValueError("metadata measurement policy is invalid")
    return {
        "profiles": list(profiles),
        "transports": list(transports),
        "concurrencies": normalized_concurrency,
        "payloads": normalized_payloads,
        "activations": activations,
        "samples_per_candidate": samples,
        "order": "ABBA_BAAB",
        "minimum_window_ns": MIN_WINDOW_NS,
        "maximum_ratio_spread": MAX_RATIO_SPREAD,
    }


def _expected_attempt_worklist(
    parameters: Mapping[str, object],
) -> list[dict[str, object]]:
    profiles = parameters["profiles"]
    transports = parameters["transports"]
    concurrencies = parameters["concurrencies"]
    payloads = parameters["payloads"]
    activations = int(parameters["activations"])
    samples = int(parameters["samples_per_candidate"])
    assert isinstance(profiles, list)
    assert isinstance(transports, list)
    assert isinstance(concurrencies, list)
    assert isinstance(payloads, list)
    worklist: list[dict[str, object]] = []

    def append_axis(
        process: str,
        profile: str,
        candidates: tuple[str, str],
        transport: str,
        concurrency: int,
        payload: int,
    ) -> None:
        for index, candidate in enumerate(balanced_order(candidates, samples)):
            block = index // 2
            worklist.append(
                {
                    "execution_index": len(worklist),
                    "candidate": candidate,
                    "process": process,
                    "ring_profile": profile,
                    "transport": transport,
                    "block": block,
                    "order": index % 2,
                    "seed": _seed_for(
                        process,
                        profile,
                        transport,
                        concurrency,
                        payload,
                        block,
                    ),
                    "concurrency": concurrency,
                    "payload": payload,
                    "activations": activations,
                }
            )

    for transport in transports:
        for concurrency in concurrencies:
            for payload in payloads:
                append_axis(
                    "portable",
                    PORTABLE_PROFILE,
                    ("oracle", "portable"),
                    transport,
                    concurrency,
                    payload,
                )
                for profile in profiles:
                    append_axis(
                        "linux",
                        profile,
                        ("portable", "linux"),
                        transport,
                        concurrency,
                        payload,
                    )
    return worklist


def _validate_attempt_worklist(
    attempts: Sequence[Mapping[str, object]],
    parameters: Mapping[str, object],
) -> None:
    expected = _expected_attempt_worklist(parameters)
    fields = tuple(expected[0])
    observed = [
        {field: attempt[field] for field in fields}
        for attempt in attempts
    ]
    if observed != expected:
        raise ValueError("attempt worklist does not match metadata")


def run_screen(
    *,
    binary: Path,
    output_dir: Path,
    profiles: Sequence[str],
    transports: Sequence[str],
    concurrencies: Sequence[int],
    payloads: Sequence[int],
    activations: int,
    samples: int,
    timeout_seconds: int,
) -> dict[str, object]:
    if not binary.is_file():
        raise ValueError("benchmark binary does not exist")
    if not profiles or any(profile not in LINUX_PROFILES for profile in profiles):
        raise ValueError("invalid Linux profile list")
    if len(set(profiles)) != len(profiles) or "submit_all" not in profiles:
        raise ValueError("submit_all control profile is required exactly once")
    if not transports or any(value not in {"tcp", "unix"} for value in transports):
        raise ValueError("invalid transport list")
    if activations <= 0 or samples <= 0 or timeout_seconds <= 0:
        raise ValueError("counts and timeout must be positive")
    output_dir.mkdir(parents=True, exist_ok=False)
    attempts: list[dict[str, object]] = []
    execution_index = 0

    def collect_axis(
        *,
        process: str,
        profile: str,
        candidates: tuple[str, str],
        transport: str,
        concurrency: int,
        payload: int,
    ) -> None:
        nonlocal execution_index
        order = balanced_order(candidates, samples)
        for index, candidate in enumerate(order):
            block = index // 2
            order_index = index % 2
            seed = _seed_for(
                process, profile, transport, concurrency, payload, block
            )
            command = benchmark_command(
                binary,
                candidate=candidate,
                process=process,
                ring_profile=profile,
                transport=transport,
                block=block,
                order=order_index,
                seed=seed,
                concurrency=concurrency,
                payload=payload,
                activations=activations,
            )
            record, error = _run_sample(
                command, timeout_seconds=timeout_seconds
            )
            if record is not None:
                try:
                    record = _validate_sample(record, candidate=candidate)
                except ValueError as validation_error:
                    record = None
                    error = f"invalid sample: {validation_error}"
            item = _attempt(
                execution_index=execution_index,
                candidate=candidate,
                process=process,
                profile=profile,
                transport=transport,
                block=block,
                order=order_index,
                seed=seed,
                concurrency=concurrency,
                payload=payload,
                activations=activations,
                record=record,
                error=error,
            )
            try:
                attempts.append(_validate_attempt(item))
            except ValueError as validation_error:
                item["record"] = None
                item["error"] = f"invalid sample identity: {validation_error}"
                attempts.append(_validate_attempt(item))
            execution_index += 1

    for transport in transports:
        for concurrency in concurrencies:
            for payload in payloads:
                collect_axis(
                    process="portable",
                    profile=PORTABLE_PROFILE,
                    candidates=("oracle", "portable"),
                    transport=transport,
                    concurrency=concurrency,
                    payload=payload,
                )
                for profile in profiles:
                    collect_axis(
                        process="linux",
                        profile=profile,
                        candidates=("portable", "linux"),
                        transport=transport,
                        concurrency=concurrency,
                        payload=payload,
                    )

    verdict, summary_rows = _project_attempts(attempts, profiles=profiles)
    root = Path(__file__).resolve().parents[1]
    metadata = _metadata(binary, root)
    parameters = {
        "profiles": list(profiles),
        "transports": list(transports),
        "concurrencies": list(concurrencies),
        "payloads": list(payloads),
        "activations": activations,
        "samples_per_candidate": samples,
        "order": "ABBA_BAAB",
        "minimum_window_ns": MIN_WINDOW_NS,
        "maximum_ratio_spread": MAX_RATIO_SPREAD,
    }
    metadata["parameters"] = parameters
    normalized_parameters = _validated_metadata_parameters(metadata)
    _validate_attempt_worklist(attempts, normalized_parameters)
    (output_dir / "metadata.json").write_text(
        _json_text(metadata), encoding="utf-8"
    )
    _write_projections(output_dir, attempts, verdict, summary_rows)
    return verdict


def audit_screen(output_dir: Path) -> dict[str, object]:
    metadata = json.loads((output_dir / "metadata.json").read_text(encoding="utf-8"))
    if not isinstance(metadata, Mapping):
        raise ValueError("metadata must be an object")
    parameters = _validated_metadata_parameters(metadata)
    profiles_raw = parameters["profiles"]
    assert isinstance(profiles_raw, list)
    attempts_raw = json.loads(
        (output_dir / "attempts.json").read_text(encoding="utf-8")
    )
    if not isinstance(attempts_raw, list):
        raise ValueError("attempts artifact must be a list")
    attempts = [_validate_attempt(item) for item in attempts_raw]
    _validate_attempt_worklist(attempts, parameters)
    verdict, summary_rows = _project_attempts(
        attempts, profiles=profiles_raw
    )
    expected = {
        "raw.csv": _csv_text(
            _raw_rows(attempts), ("execution_index", *FIELD_ORDER)
        ),
        "summary.csv": _csv_text(
            summary_rows,
            (
                "process",
                "ring_profile",
                "cell",
                "verdict",
                "correctness_verdict",
                "performance_verdict",
                "reasons",
                "wall_ratio",
                "cpu_ratio",
                "p50_ratio",
                "p99_ratio",
                "sample_count",
            ),
        ),
        "verdict.json": _json_text(verdict),
        "summary.md": _summary_markdown(verdict),
    }
    for name, content in expected.items():
        if (output_dir / name).read_text(encoding="utf-8") != content:
            raise ValueError(f"artifact projection mismatch: {name}")
    return verdict


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run or replay split LEIR compiled-executor evidence"
    )
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--audit-only", action="store_true")
    parser.add_argument("--enforce", action="store_true")
    parser.add_argument(
        "--profiles", default="submit_all,coop_taskrun,defer_taskrun"
    )
    parser.add_argument("--transports", default="tcp,unix")
    parser.add_argument("--concurrency", default="1,16")
    parser.add_argument("--payloads", default="64,4096")
    parser.add_argument("--activations", type=int, default=256)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        if args.audit_only:
            verdict = audit_screen(args.output_dir)
        else:
            if args.binary is None:
                raise ValueError("--binary is required unless --audit-only is used")
            profiles = _parse_csv_choices(
                args.profiles,
                allowed=set(LINUX_PROFILES),
                name="Linux profile",
            )
            transports = _parse_csv_choices(
                args.transports,
                allowed={"tcp", "unix"},
                name="transport",
            )
            concurrencies = _parse_csv_integers(
                args.concurrency, name="concurrency"
            )
            payloads = _parse_csv_integers(args.payloads, name="payload")
            verdict = run_screen(
                binary=args.binary,
                output_dir=args.output_dir,
                profiles=profiles,
                transports=transports,
                concurrencies=concurrencies,
                payloads=payloads,
                activations=args.activations,
                samples=args.samples,
                timeout_seconds=args.timeout_seconds,
            )
    except (OSError, subprocess.SubprocessError, ValueError, json.JSONDecodeError) as error:
        print(f"LEIR compiled-executor evidence failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(verdict, sort_keys=True, allow_nan=False))
    return 3 if args.enforce and verdict["overall_verdict"] != "PASS" else 0


if __name__ == "__main__":
    raise SystemExit(main())
