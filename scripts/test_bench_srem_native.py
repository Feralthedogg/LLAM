#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import math
import os
import subprocess
from pathlib import Path


PREFIX = "SREM_PAIR "
REQUIRED_FIELDS = {
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


def command(binary: str, *, pair: str = "adaptive_srem") -> list[str]:
    producers = "2" if pair == "remote_adaptive_srem" else "0"
    return [
        binary,
        "--workload",
        "srem_http_pipeline",
        "--pair",
        pair,
        "--instances",
        "257",
        "--frame-bytes",
        "128",
        "--tile-width",
        "16",
        "--active-lanes",
        "8",
        "--sites",
        "8",
        "--divergence-eighths",
        "1",
        "--threshold",
        "8",
        "--producers",
        producers,
        "--seed",
        "6043432235128363791",
        "--min-mode-ms",
        "2",
        "--order",
        "abba",
        "--warmup-rounds",
        "3",
        "--owner-cpu",
        "none",
    ]


def run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        argv,
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )


def parse_row(stdout: str) -> dict[str, str]:
    lines = stdout.splitlines()
    assert len(lines) == 1
    assert lines[0].startswith(PREFIX)
    fields: dict[str, str] = {}
    for token in lines[0][len(PREFIX) :].split(" "):
        key, separator, value = token.partition("=")
        assert separator and key and value and key not in fields
        fields[key] = value
    assert set(fields) == REQUIRED_FIELDS
    return fields


def expected_active(instances: int, width: int, active: int) -> int:
    full, remainder = divmod(instances, width)
    return full * active + min(remainder, active)


def check_valid(binary: str, pair: str, order: str) -> None:
    argv = command(binary, pair=pair)
    argv[argv.index("--order") + 1] = order.lower()
    result = run(argv)
    assert result.returncode == 0, result.stderr
    assert result.stderr == ""
    row = parse_row(result.stdout)
    assert row["version"] == "2"
    assert row["candidate"] == pair
    assert row["baseline"] == (
        "remote_waker_frame"
        if pair == "remote_adaptive_srem"
        else "waker_frame"
    )
    assert row["order"] == order
    assert row["baseline_checksum"] == row["candidate_checksum"]
    assert int(row["blocks_per_mode"]) == 16
    rounds = (
        int(row["rounds_per_block"])
        * int(row["blocks_per_mode"])
    )
    operations = (
        expected_active(
            int(row["instances"]),
            int(row["tile_width"]),
            int(row["active_lanes"]),
        )
        * rounds
    )
    assert int(row["ops_per_mode"]) == operations
    assert int(row["baseline_completions"]) == operations
    assert int(row["candidate_completions"]) == operations
    assert int(row["baseline_wall_ns"]) >= int(row["min_mode_ns"])
    assert int(row["candidate_wall_ns"]) >= int(row["min_mode_ns"])
    speedup = int(row["baseline_wall_ns"]) / int(row["candidate_wall_ns"])
    cpu_ratio = int(row["candidate_cpu_ns"]) / int(row["baseline_cpu_ns"])
    assert math.isclose(float(row["wall_speedup"]), speedup, rel_tol=1e-9)
    assert math.isclose(float(row["cpu_ratio"]), cpu_ratio, rel_tol=1e-9)
    assert int(row["candidate_queue_pushes"]) == int(
        row["candidate_queue_pops"]
    )
    assert int(row["candidate_tile_dispatches"]) == int(
        row["candidate_queue_pops"]
    )
    assert int(row["candidate_scalar_lanes"]) + int(
        row["candidate_vector_lanes"]
    ) == operations
    assert int(row["hot_allocations"]) == 0
    if pair == "remote_adaptive_srem":
        assert int(row["baseline_remote_pushes"]) == operations
        assert int(row["candidate_remote_pushes"]) == operations
    else:
        assert int(row["baseline_remote_pushes"]) == 0
        assert int(row["candidate_remote_pushes"]) == 0


def check_invalid(binary: str) -> None:
    base = command(binary)
    mutations: list[list[str]] = []

    mutations.append(base[:-2])
    mutations.append(base + ["--seed", "9"])
    mutations.append(base + ["trailing"])
    for option, value in (
        ("--instances", "0"),
        ("--instances", "nan"),
        ("--active-lanes", "32"),
        ("--producers", "2"),
        ("--seed", "0"),
        ("--warmup-rounds", "0"),
        ("--min-mode-ms", "0"),
    ):
        mutated = base.copy()
        mutated[mutated.index(option) + 1] = value
        mutations.append(mutated)
    unknown = base.copy()
    unknown[unknown.index("--threshold")] = "--unknown"
    mutations.append(unknown)
    remote_without_producers = command(
        binary, pair="remote_adaptive_srem"
    )
    remote_without_producers[
        remote_without_producers.index("--producers") + 1
    ] = "0"
    mutations.append(remote_without_producers)

    for argv in mutations:
        result = run(argv)
        assert result.returncode != 0, argv
        assert result.stdout == ""


def check_fairness(binary: str) -> None:
    argv = command(binary)
    argv[argv.index("--workload") + 1] = "srem_mixed_fairness"
    result = run(argv)
    assert result.returncode == 0, result.stderr
    row = parse_row(result.stdout)
    assert int(row["baseline_fair_samples"]) > 0
    assert row["baseline_fair_samples"] == row["candidate_fair_samples"]
    assert int(row["baseline_fair_p99_gap"]) > 0
    assert int(row["candidate_fair_p99_gap"]) <= (
        int(row["baseline_fair_p99_gap"]) * 110 + 99
    ) // 100


def main() -> int:
    binary = os.environ.get("SREM_MODEL_TEST_BINARY")
    if not binary:
        print("[test_bench_srem_native] binary smoke skipped")
        return 0
    assert Path(binary).is_file()
    check_valid(binary, "tile_scalar", "ABBA")
    check_valid(binary, "tile_vector", "BAAB")
    check_valid(binary, "adaptive_srem", "ABBA")
    check_valid(binary, "remote_adaptive_srem", "BAAB")
    check_fairness(binary)
    check_invalid(binary)
    print("[test_bench_srem_native] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
