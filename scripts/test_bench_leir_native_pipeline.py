#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import hashlib
import io
import json
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock

from scripts import bench_leir_native_pipeline as pipeline
from scripts.evidence_bundle import PublicationUncertainError
from scripts.bench_leir_native_pipeline import (
    FIELD_ORDER,
    MatrixCell,
    MatrixRunError,
    NativeUnavailable,
    ResultRow,
    SampleRow,
    SummaryRow,
    _build_parser,
    _classify_evidence,
    _source_dirty_digest,
    audit_existing,
    benchmark_command,
    classify,
    full_matrix,
    main,
    parse_output,
    run_one,
    summarize,
    write_evidence,
)
from scripts.process_utils import CapturedProcess, ProcessTimeoutError, run_capture


SOURCE_COMMIT = "0123456789abcdef0123456789abcdef01234567"


def _reseal_bundle(directory: Path) -> None:
    names = (
        "metadata.json",
        "raw.csv",
        "report.md",
        "summary.csv",
        "verdict.json",
    )
    records = [
        f"{hashlib.sha256((directory / name).read_bytes()).hexdigest()}"
        f"  {name}\n"
        for name in names
    ]
    (directory / "MANIFEST.sha256").write_text(
        "".join(records),
        encoding="ascii",
    )


def _evidence_metadata(samples: int) -> dict[str, object]:
    return {
        "source_commit": SOURCE_COMMIT,
        "source_dirty_digest": "clean",
        "architecture": "arm64",
        "kernel": "test-kernel",
        "toolchain": "test-toolchain",
        "commands": [["bench", "--samples", str(samples)]],
        "cpu_policy": {"scope": "server"},
        "samples": samples,
        "activations": 8,
        "min_mode_ms": 1,
        "unavailable_cells": [],
    }


def _cell(
    candidate: str = "link_skip",
    width: int = 4,
    concurrency: int = 4,
    payload: int = 64,
) -> MatrixCell:
    return MatrixCell(candidate, width, concurrency, payload)


def _unavailable(cell: MatrixCell) -> dict[str, object]:
    key = (
        cell.candidate,
        cell.batch_width,
        cell.concurrency,
        cell.payload,
    )
    skip_reason = "backend_unavailable"
    return {
        "candidate": cell.candidate,
        "batch_width": cell.batch_width,
        "concurrency": cell.concurrency,
        "payload": cell.payload,
        "reason": (
            f"native pipeline unavailable for {key}: {skip_reason}"
        ),
        "stderr": (
            f"LEIR_PIPELINE_SKIP candidate={cell.candidate} "
            f"reason={skip_reason}\n"
        ),
    }


def _fields(cell: MatrixCell | None = None) -> dict[str, str]:
    cell = cell or _cell()
    activations = max(64, cell.concurrency * 8)
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

    def test_malformed_return_77_is_not_unavailable(self) -> None:
        cell = _cell("fixed_link_skip")

        def runner(command: list[str], **_: object) -> CapturedProcess:
            return CapturedProcess(
                command,
                77,
                "",
                "LEIR_PIPELINE_SKIP candidate=fixed_link_skip "
                "reason=exact_result_semantic_barrier\n",
            )

        with self.assertRaises(MatrixRunError) as caught:
            run_one(
                Path("bench"),
                cell,
                process_sample=1,
                activations=8,
                min_mode_ms=1,
                runner=runner,
            )
        self.assertNotIsInstance(caught.exception, NativeUnavailable)

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

    def test_only_explicit_nontrivial_cells_count_as_winning_regions(
        self,
    ) -> None:
        rows = [
            SummaryRow(
                **{
                    **row.__dict__,
                    "wall_ci_high": (
                        0.94 if row.batch_width == 1 else 0.99
                    ),
                }
            )
            for row in _specialized()
        ]
        self.assertEqual(
            classify(rows, expected_samples=9)[0],
            "INCONCLUSIVE",
        )

    def test_promotion_cell_predicate_is_explicit(self) -> None:
        cases = (
            (_cell("link_skip", 1, 16, 64), False),
            (_cell("fixed_link_skip", 8, 1, 4096), False),
            (_cell("link_skip", 2, 4, 64), True),
            (_cell("fixed_link_skip", 8, 16, 4096), True),
        )
        for cell, expected in cases:
            with self.subTest(cell=cell):
                self.assertEqual(
                    pipeline._is_nontrivial_promotion_cell(cell),
                    expected,
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

    def test_required_cell_and_sample_coverage_are_inconclusive(
        self,
    ) -> None:
        specialized = _specialized()
        cases = (
            (
                "missing nontrivial cell",
                specialized,
                [
                    *(row.cell for row in specialized),
                    _cell("link_skip", 2, 4, 512),
                ],
            ),
            (
                "insufficient sample",
                [
                    SummaryRow(
                        **{
                            **specialized[0].__dict__,
                            "sample_count": 8,
                        }
                    ),
                    *specialized[1:],
                ],
                [row.cell for row in specialized],
            ),
        )
        for name, rows, required in cases:
            with self.subTest(case=name):
                self.assertEqual(
                    classify(
                        rows,
                        expected_samples=9,
                        expected_cells=required,
                    )[0],
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

    def test_fixed_resource_and_batch_width_gates_are_independent(
        self,
    ) -> None:
        specialized = _specialized()
        no_fixed = [
            SummaryRow(
                **{
                    **row.__dict__,
                    "wall_ratio": (
                        0.91
                        if row.candidate == "fixed_link_skip"
                        else row.wall_ratio
                    ),
                }
            )
            for row in specialized
        ]
        no_batch = [
            SummaryRow(
                **{
                    **row.__dict__,
                    "wall_ratio": (
                        0.88
                        if row.batch_width == 1
                        else (
                            0.89
                            if row.candidate == "fixed_link_skip"
                            else 0.90
                        )
                    ),
                }
            )
            for row in specialized
        ]
        for name, rows in (
            ("fixed resource", no_fixed),
            ("batch width", no_batch),
        ):
            with self.subTest(gate=name):
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
    def _one_sample(self, *, process_sample: int = 1) -> SampleRow:
        cell = full_matrix()[0]
        order = "ABBA" if process_sample % 2 else "BAAB"
        result = parse_output(
            _output(cell),
            cell,
            min_mode_ns=1_000_000,
        )
        return SampleRow(process_sample, order, cell, result)

    def _complete_metadata(
        self,
        *,
        samples: int = 1,
        activations: int = 8,
        measured_cells: int = 1,
    ) -> dict[str, object]:
        metadata = _evidence_metadata(samples)
        metadata["activations"] = activations
        metadata["unavailable_cells"] = [
            _unavailable(cell)
            for cell in full_matrix()[measured_cells:]
        ]
        return pipeline._bundle_metadata(metadata)

    def test_recompute_requires_canonical_pipeline_raw_csv(
        self,
    ) -> None:
        sample = self._one_sample()
        raw = pipeline._csv_text(
            pipeline._raw_rows([sample]),
            pipeline.RAW_FIELD_ORDER,
        ).encode("utf-8")
        with self.assertRaisesRegex(ValueError, "canonical"):
            pipeline._recompute_artifacts(
                raw.replace(b"\n", b"\r\n"),
                self._complete_metadata(),
            )

    def test_pipeline_unavailable_exactly_matches_zero_sample_cells(
        self,
    ) -> None:
        metadata = _evidence_metadata(1)
        metadata["activations"] = 8
        metadata["unavailable_cells"] = [
            _unavailable(full_matrix()[0])
        ]
        with self.assertRaisesRegex(ValueError, "unavailable"):
            pipeline._recompute_artifacts(
                pipeline._csv_text(
                    [],
                    pipeline.RAW_FIELD_ORDER,
                ).encode("utf-8"),
                pipeline._bundle_metadata(metadata),
            )

    def test_pipeline_process_indices_and_activations_bind_schedule(
        self,
    ) -> None:
        wrong_index = self._one_sample(process_sample=2)
        raw = pipeline._csv_text(
            pipeline._raw_rows([wrong_index]),
            pipeline.RAW_FIELD_ORDER,
        ).encode("utf-8")
        with self.assertRaisesRegex(ValueError, "sample"):
            pipeline._recompute_artifacts(
                raw,
                self._complete_metadata(),
            )

        sample = self._one_sample()
        raw = pipeline._csv_text(
            pipeline._raw_rows([sample]),
            pipeline.RAW_FIELD_ORDER,
        ).encode("utf-8")
        with self.assertRaisesRegex(ValueError, "activation"):
            pipeline._recompute_artifacts(
                raw,
                self._complete_metadata(activations=9),
            )

    def test_cli_reports_audit_recovery_for_uncertain_publication(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "pipeline"
            error = PublicationUncertainError(
                output,
                OSError("parent flush failed"),
            )
            stderr = io.StringIO()
            with mock.patch(
                "scripts.bench_leir_native_pipeline.run_matrix",
                return_value=([], []),
            ), mock.patch(
                "scripts.bench_leir_native_pipeline.write_evidence",
                side_effect=error,
            ), redirect_stderr(stderr):
                status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(output),
                        "--source-commit",
                        SOURCE_COMMIT,
                        "--source-dirty-digest",
                        "clean",
                        "--samples",
                        "1",
                    ]
                )
            self.assertEqual(status, 2)
            self.assertIn(str(output), stderr.getvalue())
            self.assertIn("--audit-existing", stderr.getvalue())

    def test_source_dirty_digest_includes_untracked_content_and_ignores_ignored(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for command in (
                ["git", "init", "--quiet"],
                ["git", "config", "user.email", "test@example.invalid"],
                ["git", "config", "user.name", "LLAM Test"],
            ):
                self.assertEqual(
                    run_capture(command, cwd=root).returncode,
                    0,
                )
            (root / ".gitignore").write_text(
                "ignored.bin\n",
                encoding="utf-8",
            )
            (root / "tracked.txt").write_text("tracked\n", encoding="utf-8")
            self.assertEqual(
                run_capture(
                    ["git", "add", ".gitignore", "tracked.txt"],
                    cwd=root,
                ).returncode,
                0,
            )
            self.assertEqual(
                run_capture(
                    ["git", "commit", "--quiet", "-m", "fixture"],
                    cwd=root,
                ).returncode,
                0,
            )
            self.assertEqual(_source_dirty_digest(cwd=root), "clean")

            untracked = root / "untracked.txt"
            untracked.write_bytes(b"first")
            first = _source_dirty_digest(cwd=root)
            self.assertRegex(first, r"[0-9a-f]{64}")
            untracked.write_bytes(b"second")
            second = _source_dirty_digest(cwd=root)
            self.assertRegex(second, r"[0-9a-f]{64}")
            self.assertNotEqual(first, second)

            (root / "ignored.bin").write_bytes(b"ignored")
            self.assertEqual(_source_dirty_digest(cwd=root), second)

    def test_cli_rejects_partial_source_provenance_override(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "pipeline"
            unavailable = [_unavailable(cell) for cell in full_matrix()]
            with mock.patch(
                "scripts.bench_leir_native_pipeline.run_matrix",
                return_value=([], unavailable),
            ):
                status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(output),
                        "--source-dirty-digest",
                        "clean",
                    ]
                )
            self.assertEqual(status, 2)
            self.assertFalse(output.exists())

    def test_cli_rejects_source_change_during_measurement(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "pipeline"
            unavailable = [_unavailable(cell) for cell in full_matrix()]
            first = (SOURCE_COMMIT, "clean")
            second = ("f" * 40, "clean")
            with mock.patch(
                "scripts.bench_leir_native_pipeline.git_source_provenance",
                create=True,
                side_effect=[first, second],
            ), mock.patch(
                "scripts.bench_leir_native_pipeline.run_matrix",
                return_value=([], unavailable),
            ):
                status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(output),
                    ]
                )
            self.assertEqual(status, 2)
            self.assertFalse(output.exists())

    def test_complete_source_override_is_atomic_and_skips_git(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "pipeline"
            unavailable = [_unavailable(cell) for cell in full_matrix()]
            with mock.patch(
                "scripts.bench_leir_native_pipeline.git_source_provenance",
                create=True,
                side_effect=AssertionError(
                    "complete override must not inspect Git"
                ),
            ), mock.patch(
                "scripts.bench_leir_native_pipeline.run_matrix",
                return_value=([], unavailable),
            ):
                status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(output),
                        "--source-commit",
                        SOURCE_COMMIT,
                        "--source-dirty-digest",
                        "clean",
                    ]
                )
            self.assertEqual(status, 0)
            self.assertEqual(
                audit_existing(
                    output,
                    required_source_commit=SOURCE_COMMIT,
                    required_source_dirty_digest="clean",
                )[0],
                "INCONCLUSIVE",
            )

    def test_run_mode_finalizes_valid_reject_before_gate_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            screen_output = root / "screen"
            gate_output = root / "gate"
            samples = [
                SampleRow(
                    1,
                    "ABBA",
                    cell,
                    parse_output(
                        _output(
                            cell,
                            {"candidate_cpu_ns": "1600000"},
                        ),
                        cell,
                        min_mode_ns=1_000_000,
                    ),
                )
                for cell in full_matrix()
            ]
            with mock.patch(
                "scripts.bench_leir_native_pipeline.run_matrix",
                return_value=(samples, []),
            ):
                screen_status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(screen_output),
                        "--source-commit",
                        SOURCE_COMMIT,
                        "--source-dirty-digest",
                        "clean",
                        "--samples",
                        "1",
                        "--activations",
                        "8",
                        "--min-mode-ms",
                        "1",
                    ]
                )
                gate_status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(gate_output),
                        "--source-commit",
                        SOURCE_COMMIT,
                        "--source-dirty-digest",
                        "clean",
                        "--samples",
                        "1",
                        "--activations",
                        "8",
                        "--min-mode-ms",
                        "1",
                        "--require-verdict",
                        "SPECIALIZED",
                    ]
                )
            self.assertEqual(screen_status, 0)
            self.assertEqual(audit_existing(screen_output)[0], "REJECT")
            self.assertEqual(gate_status, 1)
            self.assertTrue(gate_output.is_dir())
            self.assertEqual(audit_existing(gate_output)[0], "REJECT")

    def test_unavailable_cells_form_auditable_inconclusive_bundle(
        self,
    ) -> None:
        unavailable = [_unavailable(cell) for cell in full_matrix()]
        portable, platform_verdict, reasons = _classify_evidence(
            [],
            expected_samples=1,
            unavailable=unavailable,
        )
        metadata = _evidence_metadata(1)
        metadata["unavailable_cells"] = unavailable
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "pipeline"
            write_evidence(
                output,
                None,
                [],
                [],
                portable_verdict=portable,
                platform_verdict=platform_verdict,
                reasons=reasons,
                metadata=metadata,
            )
            self.assertEqual(
                audit_existing(output),
                (portable, platform_verdict, reasons),
            )

    def test_invalid_unavailable_cell_metadata_is_not_sealed(self) -> None:
        unavailable = [
            {
                "candidate": "unknown",
                "batch_width": 1,
                "concurrency": 1,
                "payload": 64,
                "reason": "invented",
                "stderr": "invented\n",
            }
        ]
        metadata = _evidence_metadata(1)
        metadata["unavailable_cells"] = unavailable
        portable, platform_verdict, reasons = _classify_evidence(
            [],
            expected_samples=1,
            unavailable=unavailable,
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "pipeline"
            with self.assertRaises(ValueError):
                write_evidence(
                    output,
                    None,
                    [],
                    [],
                    portable_verdict=portable,
                    platform_verdict=platform_verdict,
                    reasons=reasons,
                    metadata=metadata,
                )
            self.assertFalse(output.exists())

    def test_matrix_failure_does_not_finalize_partial_evidence(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "pipeline"
            failure = MatrixRunError("malformed result", rows=[])
            with mock.patch(
                "scripts.bench_leir_native_pipeline.run_matrix",
                side_effect=failure,
            ):
                status = main(
                    [
                        "--binary",
                        str(binary),
                        "--output-dir",
                        str(output),
                        "--source-commit",
                        SOURCE_COMMIT,
                        "--source-dirty-digest",
                        "clean",
                    ]
                )
            self.assertEqual(status, 2)
            self.assertFalse(output.exists())
            self.assertEqual(
                list(root.glob(".pipeline.staging-*")),
                [],
            )

    def test_audit_cli_accepts_required_provenance(self) -> None:
        args = _build_parser().parse_args(
            [
                "--audit-existing",
                "/tmp/evidence",
                "--require-source-commit",
                SOURCE_COMMIT,
                "--require-source-dirty-digest",
                "clean",
            ]
        )
        self.assertEqual(args.require_source_commit, SOURCE_COMMIT)
        self.assertEqual(args.require_source_dirty_digest, "clean")

    def test_optional_promotion_gate_uses_portable_verdict(self) -> None:
        cases = (
            ("SPECIALIZED", "SPECIALIZED", 0),
            ("REJECT", "SPECIALIZED", 1),
            ("INCONCLUSIVE", "SPECIALIZED", 1),
        )
        for portable, required, expected in cases:
            with self.subTest(portable=portable, required=required):
                with mock.patch(
                    "scripts.bench_leir_native_pipeline.audit_existing",
                    return_value=(
                        portable,
                        "SPECIALIZED",
                        ["fixture verdict"],
                    ),
                ):
                    status = main(
                        [
                            "--audit-existing",
                            "/tmp/evidence",
                            "--require-verdict",
                            required,
                        ]
                    )
                self.assertEqual(status, expected)

    def test_screening_accepts_valid_reject_without_promotion_gate(
        self,
    ) -> None:
        with mock.patch(
            "scripts.bench_leir_native_pipeline.audit_existing",
            return_value=(
                "REJECT",
                "SPECIALIZED",
                ["portable regression"],
            ),
        ):
            status = main(["--audit-existing", "/tmp/evidence"])
        self.assertEqual(status, 0)

    def test_platform_specialized_cannot_override_portable_reject(
        self,
    ) -> None:
        with mock.patch(
            "scripts.bench_leir_native_pipeline.audit_existing",
            return_value=(
                "REJECT",
                "SPECIALIZED",
                ["portable regression"],
            ),
        ):
            status = main(
                [
                    "--audit-existing",
                    "/tmp/evidence",
                    "--require-verdict",
                    "SPECIALIZED",
                ]
            )
        self.assertEqual(status, 1)

    def test_promotion_gate_keeps_invalid_evidence_at_exit_two(
        self,
    ) -> None:
        with mock.patch(
            "scripts.bench_leir_native_pipeline.audit_existing",
            side_effect=pipeline.EvidenceError("invalid fixture"),
        ):
            status = main(
                [
                    "--audit-existing",
                    "/tmp/evidence",
                    "--require-verdict",
                    "SPECIALIZED",
                ]
            )
        self.assertEqual(status, 2)

    def test_writes_exact_bundle_without_tracked_overwrite(self) -> None:
        samples = []
        for cell in full_matrix():
            result = parse_output(
                _output(cell),
                cell,
                min_mode_ns=1_000_000,
            )
            samples.append(SampleRow(1, "ABBA", cell, result))
        summaries = summarize(samples)
        portable, platform_verdict, reasons = _classify_evidence(
            summaries,
            expected_samples=1,
            unavailable=[],
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            output = root / "pipeline"
            tracked = root / "tracked.md"
            write_evidence(
                output,
                tracked,
                samples,
                summaries,
                portable_verdict=portable,
                platform_verdict=platform_verdict,
                reasons=reasons,
                metadata=_evidence_metadata(1),
            )
            self.assertEqual(
                {entry.name for entry in output.iterdir()},
                {
                    "raw.csv",
                    "summary.csv",
                    "metadata.json",
                    "verdict.json",
                    "report.md",
                    "MANIFEST.sha256",
                },
            )
            report = (output / "report.md").read_text()
            self.assertIn(
                "Linux/io_uring specialized evidence",
                report,
            )
            verdict_payload = json.loads(
                (output / "verdict.json").read_text(encoding="utf-8")
            )
            self.assertEqual(
                set(verdict_payload),
                {
                    "schema",
                    "verdict",
                    "reasons",
                    "portable_verdict",
                    "platform_verdict",
                    "required_cells",
                    "classifier_thresholds",
                },
            )
            self.assertEqual(
                verdict_payload["schema"],
                "llam.performance-verdict.v2",
            )
            self.assertEqual(
                verdict_payload["verdict"],
                verdict_payload["portable_verdict"],
            )
            self.assertEqual(
                verdict_payload["classifier_thresholds"],
                pipeline.CLASSIFIER_THRESHOLDS,
            )
            self.assertEqual(
                verdict_payload["required_cells"],
                [
                    {
                        "candidate": cell.candidate,
                        "batch_width": cell.batch_width,
                        "concurrency": cell.concurrency,
                        "payload": cell.payload,
                    }
                    for cell in full_matrix()
                    if pipeline._is_nontrivial_promotion_cell(cell)
                ],
            )
            self.assertFalse(tracked.exists())

    def test_scoped_verdict_variants_and_derived_fields_fail_closed(
        self,
    ) -> None:
        unavailable = [_unavailable(cell) for cell in full_matrix()]
        portable, platform_verdict, reasons = _classify_evidence(
            [],
            expected_samples=1,
            unavailable=unavailable,
        )

        def mutate(
            payload: dict[str, object],
            case: str,
        ) -> None:
            if case == "missing scoped field":
                payload.pop("portable_verdict")
            elif case == "extra scoped field":
                payload["unexpected"] = True
            elif case == "portable alias mismatch":
                payload["verdict"] = "REJECT"
            elif case == "malformed required cell":
                required = payload["required_cells"]
                assert isinstance(required, list)
                cell = required[0]
                assert isinstance(cell, dict)
                cell["batch_width"] = 0
            elif case == "tampered required cells":
                required = payload["required_cells"]
                assert isinstance(required, list)
                required.pop()
            elif case == "tampered thresholds":
                thresholds = payload["classifier_thresholds"]
                assert isinstance(thresholds, dict)
                thresholds["minimum_winning_regions"] = 1
            else:
                raise AssertionError(case)

        for case in (
            "missing scoped field",
            "extra scoped field",
            "portable alias mismatch",
            "malformed required cell",
            "tampered required cells",
            "tampered thresholds",
        ):
            with self.subTest(case=case):
                with tempfile.TemporaryDirectory() as temporary:
                    output = Path(temporary).resolve() / "pipeline"
                    metadata = _evidence_metadata(1)
                    metadata["unavailable_cells"] = unavailable
                    write_evidence(
                        output,
                        None,
                        [],
                        [],
                        portable_verdict=portable,
                        platform_verdict=platform_verdict,
                        reasons=reasons,
                        metadata=metadata,
                    )
                    verdict_path = output / "verdict.json"
                    payload = json.loads(
                        verdict_path.read_text(encoding="utf-8")
                    )
                    mutate(payload, case)
                    verdict_path.write_bytes(
                        pipeline.canonical_json_bytes(payload)
                    )
                    _reseal_bundle(output)
                    with self.assertRaises(
                        pipeline.EvidenceError
                    ):
                        audit_existing(output)

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
        portable, platform_verdict, reasons = _classify_evidence(
            summaries,
            expected_samples=1,
            unavailable=[],
        )
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary).resolve() / "pipeline"
            write_evidence(
                output,
                None,
                samples,
                summaries,
                portable_verdict=portable,
                platform_verdict=platform_verdict,
                reasons=reasons,
                metadata=_evidence_metadata(1),
            )
            self.assertEqual(
                audit_existing(
                    output,
                    required_source_commit=SOURCE_COMMIT,
                    required_source_dirty_digest="clean",
                ),
                (portable, platform_verdict, reasons),
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
            "Audit connected pipeline screen",
            "Enforce connected pipeline promotion",
            "github.event_name == 'pull_request'",
            "github.event_name == 'workflow_dispatch'",
            '--require-source "$GITHUB_SHA"',
            "--require-verdict SPECIALIZED",
        )
        for fragment in workflow_fragments:
            with self.subTest(workflow=fragment):
                self.assertIn(fragment, workflow)
        self.assertNotIn(
            "python3 scripts/bench_leir_native.py",
            workflow,
        )
        self.assertNotIn("--tracked-report", workflow)
        self.assertNotIn(
            "leir_native_pipeline_tracked_report.md",
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
            "portable_verdict",
            "platform_verdict",
            "required_cells",
            "classifier_thresholds",
            "--require-verdict SPECIALIZED",
            "report.md",
            "metadata.json",
            "verdict.json",
        )
        for fragment in doc_fragments:
            with self.subTest(benchmarks=fragment):
                self.assertIn(fragment, benchmarks)


if __name__ == "__main__":
    unittest.main()
