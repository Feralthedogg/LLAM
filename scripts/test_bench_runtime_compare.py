#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for benchmark comparison diagnostics."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
from pathlib import Path
from typing import Callable

import bench_runtime_compare as bench


def fail(message: str) -> None:
    raise SystemExit(f"[test_bench_runtime_compare] {message}")


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8").strip().splitlines()


@contextlib.contextmanager
def patched_main(fake_run_command: Callable[..., list[bench.BenchRow]]):
    original_argv = sys.argv
    original_run_command = bench.run_command
    original_llam_bench_command = bench.llam_bench_command
    original_plt = bench.plt

    bench.run_command = fake_run_command
    bench.llam_bench_command = lambda _root, _no_build: ["fake-bench"]
    bench.plt = None
    try:
        yield
    finally:
        sys.argv = original_argv
        bench.run_command = original_run_command
        bench.llam_bench_command = original_llam_bench_command
        bench.plt = original_plt


def test_sample_csv_filters_requested_cases_and_warns() -> None:
    def fake_run_command(
        _root: Path,
        runtime: str,
        _command: list[str],
        _env: dict[str, str],
        _timeout: int,
        sample: int,
        _samples: int,
        case: str | None = None,
    ) -> list[bench.BenchRow]:
        if runtime != "LLAM":
            fail(f"unexpected runtime: {runtime}")
        if case is not None:
            fail("non-isolated test should not pass a single case")

        return [
            bench.BenchRow("LLAM", "spawn_join", 1000.0 * sample, 10.0, 20.0),
            bench.BenchRow("LLAM", "channel_pingpong", 2000.0, 30.0, 40.0),
            bench.BenchRow("LLAM", "io_echo", 3000.0, 50.0, 60.0),
        ]

    with tempfile.TemporaryDirectory(prefix="llam-bench-compare-test-") as tmp:
        out_dir = Path(tmp)
        stdout = io.StringIO()
        stderr = io.StringIO()

        with patched_main(fake_run_command):
            sys.argv = [
                "bench_runtime_compare.py",
                "--runtime",
                "llam",
                "--no-build",
                "--samples",
                "2",
                "--cases",
                "spawn_join",
                "channel_pingpong",
                "--spread-warning-ratio",
                "1.50",
                "--out-dir",
                str(out_dir),
            ]
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = bench.main()

        if rc != 0:
            fail(f"main returned {rc}")

        selected = read_lines(out_dir / "runtime_compare.csv")
        samples = read_lines(out_dir / "runtime_compare_samples.csv")
        stderr_text = stderr.getvalue()
        stdout_text = stdout.getvalue()

        if any("io_echo" in line for line in selected + samples):
            fail("--cases filter leaked io_echo into CSV output")
        if len(selected) != 3:
            fail(f"selected CSV should contain header plus two rows, got {len(selected)} lines")
        if len(samples) != 5:
            fail(f"samples CSV should contain header plus four rows, got {len(samples)} lines")
        if "LLAM spawn_join sample spread 2.00x" not in stderr_text:
            fail(f"spread warning missing from stderr: {stderr_text!r}")
        if "use --isolate-cases" not in stderr_text:
            fail("non-isolated spread warning should recommend --isolate-cases")
        if "Samples CSV:" not in stdout_text:
            fail("stdout did not print samples CSV path")


def test_isolated_warning_omits_isolation_hint() -> None:
    def fake_run_command(
        _root: Path,
        runtime: str,
        _command: list[str],
        _env: dict[str, str],
        _timeout: int,
        sample: int,
        _samples: int,
        case: str | None = None,
    ) -> list[bench.BenchRow]:
        if runtime != "LLAM":
            fail(f"unexpected runtime: {runtime}")
        if case != "spawn_join":
            fail(f"unexpected isolated case: {case}")
        return [bench.BenchRow("LLAM", "spawn_join", 1000.0 * sample, 10.0, 20.0)]

    with tempfile.TemporaryDirectory(prefix="llam-bench-compare-test-") as tmp:
        stderr = io.StringIO()

        with patched_main(fake_run_command):
            sys.argv = [
                "bench_runtime_compare.py",
                "--runtime",
                "llam",
                "--no-build",
                "--samples",
                "2",
                "--cases",
                "spawn_join",
                "--isolate-cases",
                "--spread-warning-ratio",
                "1.50",
                "--out-dir",
                tmp,
            ]
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                rc = bench.main()

        if rc != 0:
            fail(f"main returned {rc}")

        stderr_text = stderr.getvalue()
        if "LLAM spawn_join sample spread 2.00x" not in stderr_text:
            fail(f"isolated spread warning missing from stderr: {stderr_text!r}")
        if "use --isolate-cases" in stderr_text:
            fail("isolated spread warning should not recommend --isolate-cases")


def test_parse_output_rejects_nonfinite_metrics() -> None:
    cases = [
        "[bench] name=spawn_join ops_per_sec=nan p50_us=1.0 p99_us=2.0",
        "[bench] name=spawn_join ops_per_sec=inf p50_us=1.0 p99_us=2.0",
        "[bench] name=spawn_join ops_per_sec=1.0 p50_us=nan p99_us=2.0",
        "[bench] name=spawn_join ops_per_sec=1.0 p50_us=1.0 p99_us=inf",
    ]

    for line in cases:
        try:
            bench.parse_output("LLAM", line)
        except ValueError:
            continue
        fail(f"parse_output accepted non-finite benchmark metric: {line!r}")


def test_parse_output_rejects_incomplete_metric_rows() -> None:
    line = "[bench] name=spawn_join ops_per_sec=1000.0 p50_us=1.0"

    try:
        bench.parse_output("LLAM", line)
    except ValueError:
        return
    fail(f"parse_output accepted incomplete benchmark metric row: {line!r}")


def test_parse_output_rejects_duplicate_case_rows() -> None:
    output = (
        "[bench] name=spawn_join ops_per_sec=100.0 p50_us=1.0 p99_us=2.0\n"
        "[bench] name=spawn_join ops_per_sec=999.0 p50_us=1.0 p99_us=2.0"
    )

    try:
        bench.parse_output("LLAM", output)
    except ValueError:
        return
    fail("parse_output accepted duplicate benchmark case rows")


def test_parse_output_rejects_duplicate_metric_fields() -> None:
    line = "[bench] name=spawn_join ops_per_sec=100.0 ops_per_sec=999.0 p50_us=1.0 p99_us=2.0"

    try:
        bench.parse_output("LLAM", line)
    except ValueError:
        return
    fail("parse_output accepted duplicate metric fields in one benchmark row")


def test_parse_cases_rejects_duplicate_requests() -> None:
    try:
        bench.parse_cases("spawn_join,channel_pingpong,spawn_join")
    except Exception:
        return
    fail("parse_cases accepted duplicate benchmark case requests")


def test_main_rejects_duplicate_case_args_without_traceback() -> None:
    original_argv = sys.argv
    stderr = io.StringIO()

    try:
        sys.argv = [
            "bench_runtime_compare.py",
            "--runtime",
            "llam",
            "--no-build",
            "--cases",
            "spawn_join",
            "spawn_join",
        ]
        with contextlib.redirect_stderr(stderr):
            try:
                bench.main()
            except SystemExit as exc:
                if exc.code == 0:
                    fail("main accepted duplicate benchmark case args")
            else:
                fail("main accepted duplicate benchmark case args")
    finally:
        sys.argv = original_argv

    stderr_text = stderr.getvalue()
    if "duplicate benchmark case" not in stderr_text:
        fail(f"duplicate case diagnostic missing: {stderr_text!r}")
    if "Traceback" in stderr_text:
        fail(f"duplicate case diagnostic leaked traceback: {stderr_text!r}")


def main() -> int:
    test_sample_csv_filters_requested_cases_and_warns()
    test_isolated_warning_omits_isolation_hint()
    test_parse_output_rejects_nonfinite_metrics()
    test_parse_output_rejects_incomplete_metric_rows()
    test_parse_output_rejects_duplicate_case_rows()
    test_parse_output_rejects_duplicate_metric_fields()
    test_parse_cases_rejects_duplicate_requests()
    test_main_rejects_duplicate_case_args_without_traceback()
    print("[test_bench_runtime_compare] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
