#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scripts.bench_leir_native_pipeline import (
    FIELD_ORDER,
    MatrixCell,
    MatrixRunError,
    NativeUnavailable,
    ResultRow,
    SampleRow,
    SummaryRow,
    audit_existing,
    benchmark_command,
    classify,
    full_matrix,
    parse_output,
    run_one,
    summarize,
    write_evidence,
)
from scripts.process_utils import CapturedProcess, ProcessTimeoutError


def _cell(
    candidate: str = "link_skip",
    width: int = 4,
    concurrency: int = 4,
    payload: int = 64,
) -> MatrixCell:
    return MatrixCell(candidate, width, concurrency, payload)


def _fields(cell: MatrixCell | None = None) -> dict[str, str]:
    cell = cell or _cell()
    activations = 64
    logical = activations * 2
    effective = min(cell.batch_width, cell.concurrency)
    publications = activations // effective
    skip = cell.candidate != "link"
    fixed = cell.candidate == "fixed_link_skip"
    values = {
        "baseline_wall_ns": "2000000",
        "candidate_wall_ns": "1800000",
        "baseline_cpu_ns": "1500000",
        "candidate_cpu_ns": "1450000",
        "baseline_p99_ns": "10000",
        "candidate_p99_ns": "9500",
        "activations": str(activations),
        "segments": str(activations),
        "logical_ops": str(logical),
        "batch_width": str(cell.batch_width),
        "queue_publications": str(publications),
        "task_parks": str(publications),
        "terminal_wakes": str(publications),
        "operation_sqes": str(logical),
        "operation_cqes": str(activations if skip else logical),
        "suppressed_success_cqes": str(activations if skip else 0),
        "cancel_sqes": "0",
        "cancel_cqes": "0",
        "fixed_file_attachments": str(
            cell.concurrency * 8 if fixed else 0
        ),
        "fixed_buffer_attachments": str(
            cell.concurrency * 8 if fixed else 0
        ),
        "submit_calls": str(publications),
        "submit_syscalls": str(max(1, publications // 2)),
        "checksum": "123456789",
        "status": "OK",
    }
    return values


def _output(
    cell: MatrixCell | None = None,
    updates: dict[str, str] | None = None,
) -> str:
    fields = _fields(cell)
    fields.update(updates or {})
    return "RESULT " + " ".join(
        f"{name}={fields[name]}" for name in FIELD_ORDER
    ) + "\n"


def _summary(
    candidate: str,
    width: int,
    concurrency: int,
    payload: int,
    *,
    wall: float,
    wall_low: float | None = None,
    wall_high: float | None = None,
    cpu: float = 1.0,
    p99: float = 1.0,
    structural: bool = True,
    samples: int = 9,
) -> SummaryRow:
    return SummaryRow(
        candidate=candidate,
        batch_width=width,
        concurrency=concurrency,
        payload=payload,
        sample_count=samples,
        wall_ratio=wall,
        wall_ci_low=wall if wall_low is None else wall_low,
        wall_ci_high=wall if wall_high is None else wall_high,
        cpu_ratio=cpu,
        cpu_ci_low=cpu,
        cpu_ci_high=cpu,
        p99_ratio=p99,
        p99_ci_low=p99,
        p99_ci_high=p99,
        structural_valid=structural,
    )


def _specialized() -> list[SummaryRow]:
    return [
        _summary(
            "link_skip", 1, 4, 64,
            wall=0.94, wall_low=0.92, wall_high=0.95,
        ),
        _summary(
            "link_skip", 4, 4, 64,
            wall=0.88, wall_low=0.86, wall_high=0.90,
        ),
        _summary(
            "fixed_link_skip", 4, 4, 64,
            wall=0.84, wall_low=0.82, wall_high=0.87,
        ),
        _summary(
            "link_skip", 1, 16, 512,
            wall=0.93, wall_low=0.91, wall_high=0.95,
        ),
        _summary(
            "link_skip", 8, 16, 512,
            wall=0.87, wall_low=0.84, wall_high=0.90,
        ),
        _summary(
            "fixed_link_skip", 8, 16, 512,
            wall=0.83, wall_low=0.80, wall_high=0.86,
        ),
    ]


class ParserContractTests(unittest.TestCase):
    def test_parses_exact_result(self) -> None:
        cell = _cell()
        row = parse_output(
            _output(cell), cell, min_mode_ns=1_000_000
        )
        self.assertEqual(row.activations, 64)
        self.assertEqual(row.queue_publications, 16)

    def test_rejects_missing_duplicate_and_extra_keys(self) -> None:
        cell = _cell()
        fields = _fields(cell)
        missing = "RESULT " + " ".join(
            f"{key}={fields[key]}"
            for key in FIELD_ORDER
            if key != "checksum"
        ) + "\n"
        with self.assertRaises(ValueError):
            parse_output(missing, cell, min_mode_ns=1_000_000)
        with self.assertRaises(ValueError):
            parse_output(
                _output(cell).rstrip()
                + " checksum=9\n",
                cell,
                min_mode_ns=1_000_000,
            )
        with self.assertRaises(ValueError):
            parse_output(
                _output(cell).rstrip() + " extra=1\n",
                cell,
                min_mode_ns=1_000_000,
            )

    def test_rejects_non_integer_negative_and_zero_duration(self) -> None:
        cell = _cell()
        for update in (
            {"activations": "x"},
            {"activations": "-1"},
            {"baseline_wall_ns": "0"},
            {"candidate_cpu_ns": "0"},
        ):
            with self.subTest(update=update):
                with self.assertRaises(ValueError):
                    parse_output(
                        _output(cell, update),
                        cell,
                        min_mode_ns=1_000_000,
                    )

    def test_rejects_multiple_lines_and_wrong_status(self) -> None:
        cell = _cell()
        with self.assertRaises(ValueError):
            parse_output(
                _output(cell) + _output(cell),
                cell,
                min_mode_ns=1_000_000,
            )
        with self.assertRaises(ValueError):
            parse_output(
                _output(cell, {"status": "BROKEN"}),
                cell,
                min_mode_ns=1_000_000,
            )

    def test_rejects_inconsistent_structural_counters(self) -> None:
        cell = _cell()
        bad_updates = (
            {"segments": "63"},
            {"logical_ops": "127"},
            {"queue_publications": "17"},
            {"task_parks": "15"},
            {"operation_cqes": "128"},
            {"fixed_file_attachments": "1"},
            {"cancel_sqes": "1"},
            {"submit_syscalls": "99"},
        )
        for update in bad_updates:
            with self.subTest(update=update):
                with self.assertRaises(ValueError):
                    parse_output(
                        _output(cell, update),
                        cell,
                        min_mode_ns=1_000_000,
                    )


class RunnerContractTests(unittest.TestCase):
    def test_command_contains_exact_cell(self) -> None:
        command = benchmark_command(
            Path("bench"),
            _cell("fixed_link_skip", 8, 16, 4096),
            activations=32,
            min_mode_ms=5,
            order="BAAB",
        )
        self.assertEqual(command[-2:], ["--order", "BAAB"])
        self.assertIn("fixed_link_skip", command)

    def test_run_one_uses_bounds_and_parses(self) -> None:
        cell = _cell()
        observed: dict[str, object] = {}

        def runner(command: list[str], **kwargs: object) -> CapturedProcess:
            observed["command"] = command
            observed.update(kwargs)
            return CapturedProcess(command, 0, _output(cell), "")

        sample = run_one(
            Path("bench"),
            cell,
            process_sample=1,
            activations=8,
            min_mode_ms=1,
            runner=runner,
        )
        self.assertEqual(sample.order, "ABBA")
        self.assertEqual(observed["timeout"], 120.0)
        self.assertEqual(observed["max_output_bytes"], 64 * 1024)

    def test_timeout_is_preserved(self) -> None:
        cell = _cell()

        def runner(command: list[str], **_: object) -> CapturedProcess:
            raise ProcessTimeoutError(command, 120.0, "partial", "error")

        with self.assertRaises(MatrixRunError) as caught:
            run_one(
                Path("bench"),
                cell,
                process_sample=1,
                activations=8,
                min_mode_ms=1,
                runner=runner,
            )
        self.assertIn("partial", caught.exception.stdout)
        self.assertIn("error", caught.exception.stderr)

    def test_return_77_is_unavailable(self) -> None:
        cell = _cell("fixed_link_skip")

        def runner(command: list[str], **_: object) -> CapturedProcess:
            return CapturedProcess(
                command,
                77,
                "",
                "LEIR_PIPELINE_SKIP candidate=fixed_link_skip "
                "reason=backend_unavailable\n",
            )

        with self.assertRaises(NativeUnavailable):
            run_one(
                Path("bench"),
                cell,
                process_sample=1,
                activations=8,
                min_mode_ms=1,
                runner=runner,
            )

    def test_return_77_semantic_barrier_is_unavailable(self) -> None:
        cell = _cell("link_skip")

        def runner(command: list[str], **_: object) -> CapturedProcess:
            return CapturedProcess(
                command,
                77,
                "",
                "LEIR_PIPELINE_SKIP candidate=link_skip "
                "reason=exact_result_semantic_barrier\n",
            )

        with self.assertRaises(NativeUnavailable):
            run_one(
                Path("bench"),
                cell,
                process_sample=1,
                activations=8,
                min_mode_ms=1,
                runner=runner,
            )

    def test_truncated_output_is_rejected(self) -> None:
        cell = _cell()

        def runner(command: list[str], **_: object) -> CapturedProcess:
            return CapturedProcess(
                command, 0, _output(cell), "",
                stdout_truncated=True,
            )

        with self.assertRaises(MatrixRunError):
            run_one(
                Path("bench"),
                cell,
                process_sample=1,
                activations=8,
                min_mode_ms=1,
                runner=runner,
            )


class SummaryAndClassifierTests(unittest.TestCase):
    def test_bootstrap_summary_is_deterministic(self) -> None:
        cell = _cell()
        rows = []
        for index, candidate_wall in enumerate(
            (1_700_000, 1_750_000, 1_800_000, 1_850_000, 1_900_000),
            start=1,
        ):
            result = parse_output(
                _output(
                    cell,
                    {"candidate_wall_ns": str(candidate_wall)},
                ),
                cell,
                min_mode_ns=1_000_000,
            )
            rows.append(
                SampleRow(index, "ABBA" if index % 2 else "BAAB", cell, result)
            )
        self.assertEqual(summarize(rows), summarize(rows))

    def test_two_regions_and_fixed_batch_wins_are_specialized(self) -> None:
        self.assertEqual(
            classify(_specialized(), expected_samples=9)[0],
            "SPECIALIZED",
        )

    def test_one_region_only_is_inconclusive(self) -> None:
        one_region = [
            row
            for row in _specialized()
            if (row.concurrency, row.payload) == (4, 64)
        ]
        self.assertEqual(
            classify(one_region, expected_samples=9)[0],
            "INCONCLUSIVE",
        )

    def test_cpu_regression_over_three_percent_is_reject(self) -> None:
        rows = [
            SummaryRow(**{**row.__dict__, "cpu_ratio": 1.031})
            for row in _specialized()
        ]
        self.assertEqual(
            classify(rows, expected_samples=9)[0],
            "REJECT",
        )

    def test_p99_regression_over_ten_percent_is_reject(self) -> None:
        rows = [
            SummaryRow(**{**row.__dict__, "p99_ratio": 1.101})
            for row in _specialized()
        ]
        self.assertEqual(
            classify(rows, expected_samples=9)[0],
            "REJECT",
        )

    def test_no_fixed_or_batch_win_is_inconclusive(self) -> None:
        rows = [
            SummaryRow(
                **{
                    **row.__dict__,
                    "wall_ratio": (
                        0.90
                        if row.batch_width == 1
                        else 0.92
                    ),
                }
            )
            for row in _specialized()
        ]
        self.assertEqual(
            classify(rows, expected_samples=9)[0],
            "INCONCLUSIVE",
        )

    def test_supported_wall_regression_is_reject(self) -> None:
        rows = _specialized()
        rows.append(
            _summary(
                "link_skip",
                2,
                16,
                4096,
                wall=1.05,
                wall_low=1.01,
                wall_high=1.08,
            )
        )
        self.assertEqual(
            classify(rows, expected_samples=9)[0],
            "REJECT",
        )

    def test_structural_failure_is_reject(self) -> None:
        rows = _specialized()
        rows[0] = SummaryRow(
            **{**rows[0].__dict__, "structural_valid": False}
        )
        self.assertEqual(
            classify(rows, expected_samples=9)[0],
            "REJECT",
        )


class EvidenceTests(unittest.TestCase):
    def test_writes_separate_linux_pipeline_artifacts(self) -> None:
        summaries = _specialized()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "pipeline"
            tracked = Path(temporary) / "tracked.md"
            write_evidence(
                output,
                tracked,
                [],
                summaries,
                verdict="SPECIALIZED",
                reasons=["synthetic"],
                metadata={"source_commit": "abc"},
            )
            self.assertTrue((output / "raw.csv").is_file())
            self.assertTrue((output / "summary.csv").is_file())
            report = (
                output / "leir_native_pipeline_report.md"
            ).read_text()
            self.assertIn(
                "Linux/io_uring specialized evidence",
                report,
            )
            self.assertEqual(report, tracked.read_text())

    def test_audit_recomputes_artifacts_and_rejects_tamper(self) -> None:
        samples = []
        for cell in full_matrix():
            result = parse_output(
                _output(cell),
                cell,
                min_mode_ns=1_000_000,
            )
            samples.append(SampleRow(1, "ABBA", cell, result))
        summaries = summarize(samples)
        verdict, reasons = classify(
            summaries,
            expected_samples=1,
            expected_cells=full_matrix(),
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "pipeline"
            write_evidence(
                output,
                None,
                samples,
                summaries,
                verdict=verdict,
                reasons=reasons,
                metadata={
                    "samples": 1,
                    "min_mode_ms": 1,
                    "unavailable_cells": [],
                },
            )
            self.assertEqual(
                audit_existing(output),
                (verdict, reasons),
            )
            summary_path = output / "summary.csv"
            summary_path.write_text(
                summary_path.read_text().replace(
                    "0.9,0.9,0.9",
                    "0.8,0.8,0.8",
                    1,
                )
            )
            with self.assertRaises(ValueError):
                audit_existing(output)


class LinuxIntegrationContractTests(unittest.TestCase):
    def test_workflow_verify_script_and_docs_cover_pipeline(self) -> None:
        root = Path(__file__).resolve().parents[1]
        workflow = (
            root / ".github/workflows/leir-native-research.yml"
        ).read_text(encoding="utf-8")
        verify_linux = (
            root / "scripts/verify_linux.sh"
        ).read_text(encoding="utf-8")
        benchmarks = (
            root / "docs/operations/benchmarks.md"
        ).read_text(encoding="utf-8")

        workflow_fragments = (
            '"scripts/bench_leir_native_pipeline.py"',
            '"scripts/test_bench_leir_native_pipeline.py"',
            "ulimit -l",
            "/proc/sys/kernel/io_uring_disabled",
            "bench_leir_native_pipeline",
            "for iteration in 1 2 3 4 5",
            "asan-linux-test-$iteration.log",
            "asan-pipeline-bench.log",
            "tsan-pipeline-bench.log",
            "benchmark_cpus=",
            "expected at least two benchmark CPUs",
            'taskset -c "$benchmark_cpus"',
            "--output-dir \"$OUT_DIR/pipeline\"",
            "--audit-existing \"$OUT_DIR/pipeline\"",
            "leir_native_pipeline_tracked_report.md",
        )
        for fragment in workflow_fragments:
            with self.subTest(workflow=fragment):
                self.assertIn(fragment, workflow)
        self.assertNotIn(
            "python3 scripts/bench_leir_native.py",
            workflow,
        )

        verify_fragments = (
            "bench_leir_native_pipeline",
            "test_bench_leir_native_pipeline.py",
            "./test_leir_native_linux",
            '"--batch-width", "4"',
        )
        for fragment in verify_fragments:
            with self.subTest(verify_linux=fragment):
                self.assertIn(fragment, verify_linux)

        doc_fragments = (
            "Linux/io_uring specialized evidence",
            "SPECIALIZED",
            "INCONCLUSIVE",
            "REJECT",
            "fixed_link_skip",
            "portable LLAM speedup",
            "leir_native_pipeline_report.md",
            "leir_native_pipeline_metadata.json",
        )
        for fragment in doc_fragments:
            with self.subTest(benchmarks=fragment):
                self.assertIn(fragment, benchmarks)


if __name__ == "__main__":
    unittest.main()
