#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for benchmark output parsers."""

from __future__ import annotations

import bench_guard
import bench_matrix
import cli_numbers


def fail(message: str) -> None:
    raise SystemExit(f"[test_bench_parsers] {message}")


def expect_guard_rejects(line: str) -> None:
    try:
        bench_guard.parse_bench_output(line)
    except ValueError:
        return
    fail(f"bench_guard accepted malformed benchmark row: {line!r}")


def expect_matrix_rejects(output: str) -> None:
    try:
        bench_matrix.parse_bench_output(output)
    except ValueError:
        return
    fail(f"bench_matrix accepted malformed benchmark output: {output!r}")


def test_bench_guard_rejects_malformed_metrics() -> None:
    expect_guard_rejects("[bench] name=spawn_join ops_per_sec=123abc p50_us=1 p99_us=2")
    expect_guard_rejects("[bench] name=spawn_join ops_per_sec=1.2.3 p50_us=1 p99_us=2")
    expect_guard_rejects("[bench] name=spawn_join ops_per_sec=nan p50_us=1 p99_us=2")
    expect_guard_rejects("[bench] name=spawn_join ops_per_sec=inf p50_us=1 p99_us=2")
    expect_guard_rejects("[bench] name=spawn_join ops_per_sec=0 p50_us=1 p99_us=2")
    expect_guard_rejects("[bench] name=spawn_join ops_per_sec=100 ops_per_sec=999 p50_us=1 p99_us=2")
    expect_guard_rejects(
        "[bench] name=spawn_join ops_per_sec=100 p50_us=1 p99_us=2\n"
        "[bench] name=spawn_join ops_per_sec=999 p50_us=1 p99_us=2"
    )


def test_bench_matrix_rejects_malformed_metrics() -> None:
    config = "[bench] config rounds=1 warmup=0"
    row = "[bench] name=spawn_join ops_per_sec=1000 p50_us=1 p99_us=2 io_submit_syscalls=0"

    if "spawn_join" not in bench_matrix.parse_bench_output(config + "\n" + row)["workloads"]:
        fail("bench_matrix rejected a valid benchmark row")

    expect_matrix_rejects(config + "\n[bench] name=spawn_join ops_per_sec=nan p50_us=1 p99_us=2 io_submit_syscalls=0")
    expect_matrix_rejects(config + "\n[bench] name=spawn_join ops_per_sec=1000 p50_us=inf p99_us=2 io_submit_syscalls=0")
    expect_matrix_rejects(config + "\n[bench] name=spawn_join ops_per_sec=1000 p50_us=1 p99_us=nan io_submit_syscalls=0")
    expect_matrix_rejects(config + "\n[bench] name=spawn_join ops_per_sec=1000 p50_us=1 p99_us=2 io_submit_syscalls=-1")
    expect_matrix_rejects(config + "\n[bench] name=spawn_join ops_per_sec=1000 p50_us=1 p99_us=2")
    expect_matrix_rejects(
        config + "\n[bench] name=spawn_join ops_per_sec=1000 ops_per_sec=9999 p50_us=1 p99_us=2 io_submit_syscalls=0"
    )
    expect_matrix_rejects(
        config
        + "\n[bench] name=spawn_join ops_per_sec=100 p50_us=1 p99_us=2 io_submit_syscalls=0"
        + "\n[bench] name=spawn_join ops_per_sec=999 p50_us=1 p99_us=2 io_submit_syscalls=0"
    )
    expect_matrix_rejects(
        "[bench] config rounds=1 warmup=0\n"
        "[bench] config rounds=999 warmup=0\n"
        + row
    )


def test_bench_matrix_rejects_duplicate_profile_requests() -> None:
    try:
        bench_matrix.parse_profile_names("baseline,spin,baseline")
    except Exception:
        return
    fail("bench_matrix accepted duplicate profile requests")


def test_bench_guard_rejects_ambiguous_threshold_config() -> None:
    bad_min_ops = [
        "spawn_join=1,spawn_join=2",
        "spwan_join=1",
    ]

    for raw in bad_min_ops:
        try:
            bench_guard.parse_min_ops(raw)
        except SystemExit:
            continue
        fail(f"bench_guard accepted ambiguous --min-ops config: {raw!r}")


def test_bench_guard_rejects_unknown_case_requests() -> None:
    try:
        bench_guard.parse_cases("spawn_join,spwan_join")
    except Exception:
        return
    fail("bench_guard accepted unknown benchmark case request")


def expect_number_rejects(converter: object, raw: str) -> None:
    try:
        converter(raw)  # type: ignore[operator]
    except Exception:
        return
    fail(f"numeric parser accepted ambiguous value: {raw!r}")


def test_shared_cli_number_parsers_reject_coercions() -> None:
    int_converters = (
        cli_numbers.positive_int,
        cli_numbers.nonnegative_int,
        cli_numbers.positive_int_at_most(10),
        cli_numbers.nonnegative_int_at_most(10),
    )
    float_converters = (
        cli_numbers.finite_positive_float,
        cli_numbers.finite_nonnegative_float,
        cli_numbers.finite_positive_float_at_most(10.0),
        cli_numbers.finite_nonnegative_float_at_most(10.0),
    )

    for converter in int_converters:
        for raw in (" 1", "1 ", "+1", "1_000"):
            expect_number_rejects(converter, raw)

    for converter in float_converters:
        for raw in (" 1.0", "1.0 ", "+1.0", "-0.0", "1_000.0", "nan", "inf"):
            expect_number_rejects(converter, raw)

    for raw in (" 1", "1 ", "+1", "1_000"):
        expect_number_rejects(cli_numbers.integer, raw)
        expect_number_rejects(cli_numbers.integer_at_least(-10), raw)


def main() -> int:
    test_bench_guard_rejects_malformed_metrics()
    test_bench_matrix_rejects_malformed_metrics()
    test_bench_matrix_rejects_duplicate_profile_requests()
    test_bench_guard_rejects_ambiguous_threshold_config()
    test_bench_guard_rejects_unknown_case_requests()
    test_shared_cli_number_parsers_reject_coercions()
    print("[test_bench_parsers] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
