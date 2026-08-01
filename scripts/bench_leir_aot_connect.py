# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Copyright 2026 Feralthedogg

"""Parse and classify the LEIR AOT CONNECT -> WRITE experiment."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import random
import statistics
import subprocess
import sys
from collections.abc import Iterable, Mapping, Sequence
from typing import Final


SAMPLE_PREFIX: Final = "LEIR_AOT_CONNECT_SAMPLE"
RING_PROFILES: Final = frozenset(
    {"submit_all", "coop_taskrun", "defer_taskrun"}
)
FIELD_ORDER: Final = (
    "version",
    "candidate",
    "ring_profile",
    "family",
    "concurrency",
    "payload",
    "activations",
    "wall_ns",
    "cpu_ns",
    "p99_ns",
    "bind_ns",
    "execute_ns",
    "aot_prepare_ns",
    "aot_ring_ns",
    "aot_resume_ns",
    "correctness",
    "queue_publications",
    "prepared_sqes",
    "observed_cqes",
    "suppressed_success_cqes",
    "task_parks",
    "terminal_wakes",
    "hot_allocations",
    "checksum",
)
INTEGER_FIELDS: Final = frozenset(
    {
        "version",
        "concurrency",
        "payload",
        "activations",
        "wall_ns",
        "cpu_ns",
        "p99_ns",
        "bind_ns",
        "execute_ns",
        "aot_prepare_ns",
        "aot_ring_ns",
        "aot_resume_ns",
        "correctness",
        "queue_publications",
        "prepared_sqes",
        "observed_cqes",
        "suppressed_success_cqes",
        "task_parks",
        "terminal_wakes",
        "hot_allocations",
    }
)
CELL_FIELDS: Final = (
    "ring_profile",
    "family",
    "concurrency",
    "payload",
    "activations",
)
CHECKSUM_PATTERN: Final = re.compile(r"[0-9a-f]{16}\Z")


def _integer(name: str, value: object) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{name} must be an integer")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and re.fullmatch(r"[0-9]+", value):
        return int(value)
    raise ValueError(f"{name} must be an integer")


def _validate_sample(
    record: Mapping[str, object], *, candidate: str | None = None
) -> dict[str, object]:
    missing = [field for field in FIELD_ORDER if field not in record]
    if missing:
        raise ValueError(f"missing fields: {', '.join(missing)}")
    unexpected = sorted(set(record) - set(FIELD_ORDER))
    if unexpected:
        raise ValueError(f"unexpected fields: {', '.join(unexpected)}")

    validated = dict(record)
    for field in INTEGER_FIELDS:
        validated[field] = _integer(field, validated[field])

    if validated["version"] != 2:
        raise ValueError("version must be 2")
    if validated["candidate"] not in {"portable", "native"}:
        raise ValueError("candidate must be portable or native")
    if candidate is not None and validated["candidate"] != candidate:
        raise ValueError(f"expected {candidate} candidate")
    if validated["ring_profile"] not in RING_PROFILES:
        raise ValueError("ring_profile is not recognized")
    if validated["family"] not in {"tcp", "unix"}:
        raise ValueError("family must be tcp or unix")
    for field in (
        "concurrency",
        "payload",
        "activations",
        "wall_ns",
        "cpu_ns",
        "p99_ns",
        "bind_ns",
        "execute_ns",
    ):
        if validated[field] <= 0:
            raise ValueError(f"{field} must be a positive integer")
    if validated["correctness"] not in {0, 1}:
        raise ValueError("correctness must be 0 or 1")
    for field in (
        "queue_publications",
        "prepared_sqes",
        "observed_cqes",
        "suppressed_success_cqes",
        "task_parks",
        "terminal_wakes",
        "hot_allocations",
        "aot_prepare_ns",
        "aot_ring_ns",
        "aot_resume_ns",
    ):
        if validated[field] < 0:
            raise ValueError(f"{field} must be a non-negative integer")
    aot_fields = ("aot_prepare_ns", "aot_ring_ns", "aot_resume_ns")
    if validated["candidate"] == "portable":
        if any(validated[field] != 0 for field in aot_fields):
            raise ValueError("portable AOT timings must be zero")
    else:
        if any(validated[field] <= 0 for field in aot_fields):
            raise ValueError("native AOT timings must be positive")
        remaining = int(validated["execute_ns"])
        for field in aot_fields:
            value = int(validated[field])
            if value > remaining:
                raise ValueError("native AOT timings exceed execute_ns")
            remaining -= value
    checksum = validated["checksum"]
    if not isinstance(checksum, str) or CHECKSUM_PATTERN.fullmatch(checksum) is None:
        raise ValueError("checksum must be 16 lowercase hexadecimal characters")
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
    fields = " ".join(f"{name}={validated[name]}" for name in FIELD_ORDER)
    return f"{SAMPLE_PREFIX} {fields}"


def _cell(record: Mapping[str, object]) -> tuple[object, ...]:
    return tuple(record[field] for field in CELL_FIELDS)


def _paired_samples(
    portable_samples: Sequence[Mapping[str, object]],
    native_samples: Sequence[Mapping[str, object]],
) -> list[tuple[dict[str, object], dict[str, object]]]:
    if len(portable_samples) != len(native_samples):
        raise ValueError("portable and native sample counts differ")
    if not portable_samples:
        raise ValueError("sample count must be positive")

    pairs = []
    for portable_raw, native_raw in zip(portable_samples, native_samples):
        portable = _validate_sample(portable_raw, candidate="portable")
        native = _validate_sample(native_raw, candidate="native")
        if _cell(portable) != _cell(native):
            raise ValueError("portable and native sample cells differ")
        pairs.append((portable, native))
    return pairs


def balanced_order(samples_per_candidate: int) -> list[str]:
    if (
        isinstance(samples_per_candidate, bool)
        or not isinstance(samples_per_candidate, int)
        or samples_per_candidate <= 0
    ):
        raise ValueError("sample count must be a positive integer")
    pattern = ("portable", "native", "native", "portable")
    counts = {"portable": 0, "native": 0}
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
    ring_profile: str,
    family: str,
    concurrency: int,
    payload: int,
    activations: int,
) -> list[str]:
    if candidate not in {"portable", "native"}:
        raise ValueError("invalid benchmark candidate")
    if ring_profile not in RING_PROFILES:
        raise ValueError("invalid ring profile")
    if family not in {"tcp", "unix"}:
        raise ValueError("invalid benchmark family")
    for name, value in (
        ("concurrency", concurrency),
        ("payload", payload),
        ("activations", activations),
    ):
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            raise ValueError(f"{name} must be a positive integer")
    if concurrency > 256 or payload < 8 or payload > 65536:
        raise ValueError("benchmark cell is outside the supported range")
    if activations < concurrency:
        raise ValueError("activations must be at least concurrency")
    return [
        str(binary.resolve()),
        "--candidate",
        candidate,
        "--ring-profile",
        ring_profile,
        "--family",
        family,
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
        resample = [
            values[generator.randrange(len(values))]
            for _ in values
        ]
        medians.append(float(statistics.median(resample)))
    medians.sort()
    lower = medians[int((len(medians) - 1) * 0.025)]
    upper = medians[int((len(medians) - 1) * 0.975)]
    return lower, upper


def classify_mechanism(
    portable_samples: Sequence[Mapping[str, object]],
    native_samples: Sequence[Mapping[str, object]],
) -> dict[str, object]:
    pairs = _paired_samples(portable_samples, native_samples)
    reasons: set[str] = set()
    wall_ratios: list[float] = []

    for portable, native in pairs:
        activations = int(native["activations"])
        expected_native = {
            "queue_publications": activations,
            "prepared_sqes": activations * 2,
            "observed_cqes": activations,
            "suppressed_success_cqes": activations,
            "task_parks": activations,
            "terminal_wakes": activations,
            "hot_allocations": 0,
        }
        for field, expected in expected_native.items():
            if native[field] != expected:
                reasons.add(field)
        if portable["correctness"] != 1 or native["correctness"] != 1:
            reasons.add("correctness")
        if portable["hot_allocations"] != 0:
            reasons.add("hot_allocations")
        if portable["checksum"] != native["checksum"]:
            reasons.add("checksum")

        wall_ratio = int(native["wall_ns"]) / int(portable["wall_ns"])
        wall_ratios.append(wall_ratio)

    wall_ratio = float(statistics.median(wall_ratios))
    wall_interval = _bootstrap_median_interval(wall_ratios)
    if wall_interval[0] > 1.05:
        reasons.add("wall_ns")
    elif wall_interval[1] > 1.05:
        reasons.add("wall_confidence")

    performance_incomplete = reasons == {"wall_confidence"}

    return {
        "verdict": (
            "CONTINUE"
            if not reasons
            else "INCOMPLETE"
            if performance_incomplete
            else "STOP"
        ),
        "reasons": sorted(reasons),
        "wall_ratio": wall_ratio,
        "wall_ratio_ci_low": wall_interval[0],
        "wall_ratio_ci_high": wall_interval[1],
        "sample_count": len(pairs),
    }


def classify_release_gate(
    pairs: Iterable[tuple[Mapping[str, object], Mapping[str, object]]],
    *,
    machine_count: int,
) -> dict[str, object]:
    if isinstance(machine_count, bool) or not isinstance(machine_count, int):
        raise ValueError("machine_count must be an integer")

    raw_pairs = list(pairs)
    if not raw_pairs:
        raise ValueError("sample count must be positive")
    portable_samples = [pair[0] for pair in raw_pairs]
    native_samples = [pair[1] for pair in raw_pairs]
    validated = _paired_samples(portable_samples, native_samples)

    reasons: set[str] = set()
    wall_speedups: list[float] = []
    cpu_ratios: list[float] = []
    p99_ratios: list[float] = []
    if machine_count < 2:
        reasons.add("machine_count")

    mechanism = classify_mechanism(portable_samples, native_samples)
    reasons.update(str(reason) for reason in mechanism["reasons"])
    for portable, native in validated:
        wall_speedup = int(portable["wall_ns"]) / int(native["wall_ns"])
        cpu_ratio = int(native["cpu_ns"]) / int(portable["cpu_ns"])
        p99_ratio = int(native["p99_ns"]) / int(portable["p99_ns"])
        wall_speedups.append(wall_speedup)
        cpu_ratios.append(cpu_ratio)
        p99_ratios.append(p99_ratio)
        if wall_speedup < 1.50:
            reasons.add("wall_speedup")
        if cpu_ratio > 0.70:
            reasons.add("cpu_ns")
        if p99_ratio > 1.10:
            reasons.add("p99_ns")

    return {
        "verdict": "READY" if not reasons else "BLOCKED",
        "reasons": sorted(reasons),
        "machine_count": machine_count,
        "sample_count": len(validated),
        "wall_speedup": statistics.median(wall_speedups),
        "cpu_ratio": statistics.median(cpu_ratios),
        "p99_ratio": statistics.median(p99_ratios),
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
    digest = hashlib.sha256()
    if completed.returncode == 0:
        raw_paths = sorted(completed.stdout.split(b"\0"))
    else:
        source_suffixes = {
            ".S",
            ".c",
            ".cmake",
            ".h",
            ".inc",
            ".json",
            ".md",
            ".py",
            ".sh",
            ".txt",
            ".yaml",
            ".yml",
        }
        special_names = {"CMakeLists.txt", "LICENSE", "Makefile"}
        excluded_parts = {
            ".artifacts",
            ".git",
            "__pycache__",
            "artifacts",
            "build",
            "target",
        }
        raw_paths = []
        for path in root.rglob("*"):
            relative = path.relative_to(root)
            if not path.is_file() or any(
                part in excluded_parts or part.startswith("object")
                for part in relative.parts
            ):
                continue
            if path.suffix not in source_suffixes and (
                path.name not in special_names
            ):
                continue
            raw_paths.append(os.fsencode(relative.as_posix()))
        raw_paths.sort()
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
    revision = os.environ.get("LLAM_SOURCE_REVISION") or (
        revision_result.stdout.strip()
        if revision_result.returncode == 0
        else "unavailable"
    )
    status_result = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=root,
        check=False,
        capture_output=True,
        text=True,
    )
    source_dirty = (
        os.environ.get("LLAM_SOURCE_TREE_DIRTY") == "1"
        if status_result.returncode != 0
        else bool(status_result.stdout)
    )
    return {
        "schema_version": 2,
        "scope": "SPECIALIZED",
        "portable_performance_claim": False,
        "release_authorized": False,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "source_revision": revision,
        "source_tree_digest_sha256": _source_tree_digest(root),
        "source_tree_dirty": source_dirty,
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
    command: Sequence[str],
    *,
    timeout_seconds: int,
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
        reason = completed.stderr.strip() or "platform unavailable"
        return None, f"skip: {reason}"
    if len(lines) != 1:
        reason = completed.stderr.strip() or (
            f"expected one sample line, got {len(lines)}"
        )
        return None, f"exit {completed.returncode}: {reason}"
    try:
        record = parse_sample(lines[0])
    except ValueError as error:
        return None, f"invalid sample: {error}"
    if completed.returncode != 0 and record["correctness"] == 1:
        return None, f"exit {completed.returncode} with success sample"
    return record, None


def _write_csv(
    path: Path,
    rows: Sequence[Mapping[str, object]],
    fields: Sequence[str],
) -> None:
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def _write_json(path: Path, value: object) -> None:
    path.write_text(
        json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def _parse_csv_choices(
    value: str, *, allowed: set[str], name: str
) -> list[str]:
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


def _skip_class(error: str) -> str | None:
    if not error.startswith("skip:"):
        return None
    detail = error.removeprefix("skip:").strip().lower()
    if any(
        marker in detail
        for marker in (
            "not supported",
            "unavailable",
            "enotsup",
            "enosys",
            "not implemented",
        )
    ):
        return "unsupported"
    if any(
        marker in detail
        for marker in ("permission denied", "not permitted", "eperm", "eacces")
    ):
        return "permission"
    if any(marker in detail for marker in ("temporarily unavailable", "eagain")):
        return "transient"
    return "platform"


def _profile_ratios(
    pairs: Sequence[tuple[Mapping[str, object], Mapping[str, object]]],
) -> dict[str, float | None]:
    if not pairs:
        return {
            "wall_ratio": None,
            "cpu_ratio": None,
            "p99_ratio": None,
        }
    return {
        "wall_ratio": float(
            statistics.median(
                int(native["wall_ns"]) / int(portable["wall_ns"])
                for portable, native in pairs
            )
        ),
        "cpu_ratio": float(
            statistics.median(
                int(native["cpu_ns"]) / int(portable["cpu_ns"])
                for portable, native in pairs
            )
        ),
        "p99_ratio": float(
            statistics.median(
                int(native["p99_ns"]) / int(portable["p99_ns"])
                for portable, native in pairs
            )
        ),
    }


def run_screen(
    *,
    binary: Path,
    output_dir: Path,
    profiles: Sequence[str],
    families: Sequence[str],
    concurrencies: Sequence[int],
    payloads: Sequence[int],
    activations: int,
    samples: int,
    timeout_seconds: int,
) -> dict[str, object]:
    if not binary.is_file():
        raise ValueError("benchmark binary does not exist")
    if (
        not profiles
        or len(set(profiles)) != len(profiles)
        or any(profile not in RING_PROFILES for profile in profiles)
    ):
        raise ValueError("invalid ring profile list")
    if "submit_all" not in profiles:
        raise ValueError("submit_all control profile is required")
    output_dir.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    raw_rows: list[dict[str, object]] = []
    summary_rows: list[dict[str, object]] = []
    profile_results: dict[str, dict[str, object]] = {}
    pairs_by_profile: dict[
        str,
        list[tuple[Mapping[str, object], Mapping[str, object]]],
    ] = {}
    execution_index = 0

    for profile in profiles:
        cell_verdicts: list[dict[str, object]] = []
        profile_pairs: list[
            tuple[Mapping[str, object], Mapping[str, object]]
        ] = []
        profile_errors: list[str] = []
        profile_skip_classes: list[str | None] = []
        profile_attempts = 0
        profile_sample_count = 0

        for family in families:
            for concurrency in concurrencies:
                for payload in payloads:
                    by_candidate: dict[str, list[dict[str, object]]] = {
                        "portable": [],
                        "native": [],
                    }
                    cell_errors: list[str] = []
                    cell_skip_classes: list[str | None] = []
                    order = balanced_order(samples)
                    for candidate in order:
                        profile_attempts += 1
                        command = benchmark_command(
                            binary,
                            candidate=candidate,
                            ring_profile=profile,
                            family=family,
                            concurrency=concurrency,
                            payload=payload,
                            activations=activations,
                        )
                        record, error = _run_sample(
                            command, timeout_seconds=timeout_seconds
                        )
                        if error is not None:
                            labelled_error = f"{candidate}: {error}"
                            skip_class = _skip_class(error)
                            cell_errors.append(labelled_error)
                            cell_skip_classes.append(skip_class)
                            profile_errors.append(labelled_error)
                            profile_skip_classes.append(skip_class)
                            continue
                        assert record is not None
                        try:
                            record = _validate_sample(
                                record, candidate=candidate
                            )
                        except ValueError as validation_error:
                            labelled_error = (
                                f"{candidate}: invalid sample: "
                                f"{validation_error}"
                            )
                            cell_errors.append(labelled_error)
                            cell_skip_classes.append(None)
                            profile_errors.append(labelled_error)
                            profile_skip_classes.append(None)
                            continue
                        expected_cell = (
                            profile,
                            family,
                            concurrency,
                            payload,
                            activations,
                        )
                        if _cell(record) != expected_cell:
                            labelled_error = (
                                f"{candidate}: sample identity mismatch"
                            )
                            cell_errors.append(labelled_error)
                            cell_skip_classes.append(None)
                            profile_errors.append(labelled_error)
                            profile_skip_classes.append(None)
                            continue
                        by_candidate[candidate].append(record)
                        profile_sample_count += 1
                        raw_rows.append(
                            {
                                "execution_index": execution_index,
                                **record,
                            }
                        )
                        execution_index += 1

                    cell_label = (
                        f"{family}/c{concurrency}/p{payload}/a{activations}"
                    )
                    cell_sample_count = sum(
                        len(records) for records in by_candidate.values()
                    )
                    cell_unavailable = (
                        cell_sample_count == 0
                        and len(cell_errors) == len(order)
                        and all(
                            skip_class is not None
                            for skip_class in cell_skip_classes
                        )
                        and len(set(cell_skip_classes)) == 1
                    )
                    if cell_unavailable:
                        reasons = sorted(set(cell_errors))
                        cell_result: dict[str, object] = {
                            "cell": cell_label,
                            "verdict": "UNAVAILABLE",
                            "reasons": reasons,
                            "skip_class": cell_skip_classes[0],
                            "portable_samples": 0,
                            "native_samples": 0,
                        }
                    elif cell_errors or any(
                        len(by_candidate[candidate]) != samples
                        for candidate in ("portable", "native")
                    ):
                        reasons = sorted(set(cell_errors)) or [
                            "incomplete sample count"
                        ]
                        cell_result = {
                            "cell": cell_label,
                            "verdict": "INCOMPLETE",
                            "reasons": reasons,
                            "portable_samples": len(
                                by_candidate["portable"]
                            ),
                            "native_samples": len(by_candidate["native"]),
                        }
                    else:
                        cell_result = classify_mechanism(
                            by_candidate["portable"],
                            by_candidate["native"],
                        )
                        cell_result = {"cell": cell_label, **cell_result}
                        profile_pairs.extend(
                            zip(
                                by_candidate["portable"],
                                by_candidate["native"],
                            )
                        )
                    cell_verdicts.append(cell_result)
                    summary_rows.append(
                        {
                            "ring_profile": profile,
                            "family": family,
                            "concurrency": concurrency,
                            "payload": payload,
                            "activations": activations,
                            "verdict": cell_result["verdict"],
                            "reasons": ";".join(
                                str(reason)
                                for reason in cell_result["reasons"]
                            ),
                            "wall_ratio": cell_result.get(
                                "wall_ratio", ""
                            ),
                            "wall_ratio_ci_low": cell_result.get(
                                "wall_ratio_ci_low", ""
                            ),
                            "wall_ratio_ci_high": cell_result.get(
                                "wall_ratio_ci_high", ""
                            ),
                            "portable_samples": len(
                                by_candidate["portable"]
                            ),
                            "native_samples": len(
                                by_candidate["native"]
                            ),
                        }
                    )

        profile_unavailable = (
            profile_sample_count == 0
            and len(profile_errors) == profile_attempts
            and all(
                skip_class is not None
                for skip_class in profile_skip_classes
            )
            and len(set(profile_skip_classes)) == 1
        )
        cell_verdict_names = {
            str(item["verdict"]) for item in cell_verdicts
        }
        if "STOP" in cell_verdict_names:
            profile_verdict = "REJECT"
        elif profile_unavailable:
            profile_verdict = "UNAVAILABLE"
        elif cell_verdict_names == {"CONTINUE"}:
            profile_verdict = "CONTINUE"
        else:
            profile_verdict = "INCOMPLETE"
        capability = (
            "UNAVAILABLE"
            if profile_unavailable
            else "AVAILABLE"
            if not ({"INCOMPLETE", "UNAVAILABLE"} & cell_verdict_names)
            else "PARTIAL"
        )
        profile_reasons = sorted(
            {
                str(reason)
                for cell in cell_verdicts
                for reason in cell["reasons"]
            }
        )
        profile_results[profile] = {
            "capability": capability,
            "verdict": profile_verdict,
            "reasons": profile_reasons,
            "sample_count": len(profile_pairs),
            **_profile_ratios(profile_pairs),
            "cells": cell_verdicts,
        }
        pairs_by_profile[profile] = profile_pairs

    control = profile_results["submit_all"]
    control_verdict = str(control["verdict"])
    overall = (
        "CONTINUE"
        if control_verdict == "CONTINUE"
        else "STOP"
        if control_verdict == "REJECT"
        else "INCOMPLETE"
    )
    recommended_candidates = [
        (
            float(result["cpu_ratio"]),
            float(result["p99_ratio"]),
            float(result["wall_ratio"]),
            profile,
        )
        for profile, result in profile_results.items()
        if result["verdict"] == "CONTINUE"
        and result["cpu_ratio"] is not None
        and result["p99_ratio"] is not None
        and result["wall_ratio"] is not None
    ]
    recommended_profile = (
        min(recommended_candidates)[3]
        if recommended_candidates
        else None
    )
    release_gate: dict[str, object]
    control_pairs = pairs_by_profile["submit_all"]
    if control_pairs and control["capability"] == "AVAILABLE":
        release_gate = classify_release_gate(
            control_pairs, machine_count=1
        )
    else:
        release_gate = {
            "verdict": "BLOCKED",
            "reasons": ["incomplete_screen", "machine_count"],
            "machine_count": 1,
        }
    verdict = {
        "schema_version": 2,
        "scope": "SPECIALIZED",
        "control_profile": "submit_all",
        "mechanism_verdict": overall,
        "recommended_profile": recommended_profile,
        "profiles": profile_results,
        "release_gate": release_gate,
        "release_authorized": False,
        "cells": control["cells"],
    }
    metadata = _metadata(binary, root)
    metadata["parameters"] = {
        "profiles": list(profiles),
        "families": list(families),
        "concurrencies": list(concurrencies),
        "payloads": list(payloads),
        "activations": activations,
        "samples_per_candidate": samples,
        "order": "ABBA",
    }
    _write_csv(
        output_dir / "raw.csv",
        raw_rows,
        ("execution_index", *FIELD_ORDER),
    )
    _write_csv(
        output_dir / "summary.csv",
        summary_rows,
        (
            "ring_profile",
            "family",
            "concurrency",
            "payload",
            "activations",
            "verdict",
            "reasons",
            "wall_ratio",
            "wall_ratio_ci_low",
            "wall_ratio_ci_high",
            "portable_samples",
            "native_samples",
        ),
    )
    _write_json(output_dir / "metadata.json", metadata)
    _write_json(output_dir / "verdict.json", verdict)
    report_lines = [
        "# LEIR AOT CONNECT mechanism screen",
        "",
        f"- `submit_all` control verdict: **{overall}**",
        f"- Recommended profile: **{recommended_profile or 'none'}**",
        f"- 3.0.0 release gate: **{release_gate['verdict']}**",
        "- Scope: Linux-specialized; no portable performance claim",
        "- Release authorized: **no**",
        "",
        "| Profile | Capability | Verdict | CPU ratio | p99 ratio | Wall ratio |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for profile, profile_result in profile_results.items():
        report_lines.append(
            f"| {profile} | {profile_result['capability']} | "
            f"{profile_result['verdict']} | "
            f"{profile_result['cpu_ratio'] or 'n/a'} | "
            f"{profile_result['p99_ratio'] or 'n/a'} | "
            f"{profile_result['wall_ratio'] or 'n/a'} |"
        )
    report_lines.extend(
        [
            "",
            "| Profile | Cell | Verdict | Median wall ratio | "
            "95% bootstrap interval |",
            "|---|---|---:|---:|---:|",
        ]
    )
    for profile, profile_result in profile_results.items():
        for cell in profile_result["cells"]:
            report_lines.append(
                f"| {profile} | {cell['cell']} | {cell['verdict']} | "
                f"{cell.get('wall_ratio', 'n/a')} | "
                f"{cell.get('wall_ratio_ci_low', 'n/a')}–"
                f"{cell.get('wall_ratio_ci_high', 'n/a')} |"
            )
    report_lines.extend(
        [
            "",
            "A `CONTINUE` result only permits broader research. It does not "
            "authorize a version bump, tag, package, or release.",
            "",
        ]
    )
    (output_dir / "summary.md").write_text(
        "\n".join(report_lines), encoding="utf-8"
    )
    return verdict


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run the LEIR AOT CONNECT mechanism screen"
    )
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--profiles",
        default="submit_all,coop_taskrun,defer_taskrun",
    )
    parser.add_argument("--families", default="tcp,unix")
    parser.add_argument("--concurrency", default="1,16")
    parser.add_argument("--payloads", default="64,4096")
    parser.add_argument("--activations", type=int, default=256)
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        profiles = _parse_csv_choices(
            args.profiles,
            allowed=set(RING_PROFILES),
            name="ring profile",
        )
        families = _parse_csv_choices(
            args.families, allowed={"tcp", "unix"}, name="family"
        )
        concurrencies = _parse_csv_integers(
            args.concurrency, name="concurrency"
        )
        payloads = _parse_csv_integers(args.payloads, name="payload")
        if args.activations <= 0 or args.samples <= 0 or (
            args.timeout_seconds <= 0
        ):
            raise ValueError("counts and timeout must be positive")
        verdict = run_screen(
            binary=args.binary,
            output_dir=args.output_dir,
            profiles=profiles,
            families=families,
            concurrencies=concurrencies,
            payloads=payloads,
            activations=args.activations,
            samples=args.samples,
            timeout_seconds=args.timeout_seconds,
        )
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"LEIR AOT CONNECT screen failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps(verdict, sort_keys=True, allow_nan=False))
    return 3 if verdict["mechanism_verdict"] == "INCOMPLETE" else 0


if __name__ == "__main__":
    raise SystemExit(main())
