#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import io
import os
import tempfile
import unittest
from contextlib import redirect_stderr
from dataclasses import replace
from pathlib import Path
from unittest import mock

from scripts.evidence_bundle import PublicationUncertainError
from scripts.bench_leir_native import (
    MAX_OUTPUT_BYTES,
    MatrixCell,
    MatrixRunError,
    NativeUnavailable,
    PairRow,
    SampleRow,
    _build_parser,
    _source_dirty_digest,
    audit_existing,
    benchmark_command,
    classify,
    parse_output,
    main,
    run_one,
    screen_matrix,
    summarize,
    write_evidence,
)
from scripts.process_utils import CapturedProcess, ProcessTimeoutError, run_capture


VALID_ROW = (
    "LEIR_NATIVE_PAIR version=1 candidate=link_skip ops=4 "
    "concurrency=64 payload=64 activations=64 min_mode_ns=100000000 "
    "blocks_per_mode=8 baseline_wall_ns=200000000 "
    "candidate_wall_ns=100000000 baseline_cpu_ns=170000000 "
    "candidate_cpu_ns=100000000 wall_speedup=2.000000000 "
    "cpu_ratio=0.588235294 baseline_ctx_switches=512 "
    "candidate_ctx_switches=128 logical_operations=256 "
    "queue_publications=64 prepared_sqes=256 ring_submit_calls=16 "
    "ring_submit_syscalls=16 expected_cqes=64 observed_cqes=64 "
    "suppressed_success_cqes=192 baseline_task_parks=128 "
    "candidate_task_parks=64 terminal_wakes=64 resumes_avoided=192 "
    "hot_allocations=0 baseline_checksum=0123456789abcdef "
    "candidate_checksum=0123456789abcdef pending_path_valid=1 "
    "baseline_service_gap_p99_ns=1000 "
    "candidate_service_gap_p99_ns=1000 "
    "baseline_terminal_p99_ns=2000 candidate_terminal_p99_ns=2000 "
    "peer=process cpu_scope=server platform=linux_io_uring order=ABBA"
)


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (cell.candidate, cell.ops, cell.concurrency, cell.payload)


def _pair_for_cell(
    cell: MatrixCell,
    *,
    wall_speedup: float,
    cpu_ratio: float,
    min_mode_ns: int = 100_000_000,
    service_ratio: float = 1.0,
    terminal_ratio: float = 1.0,
    order: str = "ABBA",
) -> PairRow:
    activations = 64
    logical = activations * cell.ops
    observed = logical if cell.candidate == "link" else activations
    checksum = f"{abs(hash(_cell_key(cell))) & 0xFFFF_FFFF_FFFF_FFFF:016x}"
    baseline_wall_ns = 1_000_000_000
    candidate_wall_ns = round(baseline_wall_ns / wall_speedup)
    baseline_cpu_ns = 1_000_000_000
    candidate_cpu_ns = round(baseline_cpu_ns * cpu_ratio)
    return PairRow(
        candidate=cell.candidate,
        ops=cell.ops,
        concurrency=cell.concurrency,
        payload=cell.payload,
        activations=activations,
        min_mode_ns=min_mode_ns,
        blocks_per_mode=8,
        baseline_wall_ns=baseline_wall_ns,
        candidate_wall_ns=candidate_wall_ns,
        baseline_cpu_ns=baseline_cpu_ns,
        candidate_cpu_ns=candidate_cpu_ns,
        wall_speedup=baseline_wall_ns / candidate_wall_ns,
        cpu_ratio=candidate_cpu_ns / baseline_cpu_ns,
        baseline_ctx_switches=512,
        candidate_ctx_switches=128,
        logical_operations=logical,
        queue_publications=activations,
        prepared_sqes=logical,
        ring_submit_calls=16,
        ring_submit_syscalls=16,
        expected_cqes=observed,
        observed_cqes=observed,
        suppressed_success_cqes=logical - observed,
        baseline_task_parks=logical,
        candidate_task_parks=activations,
        terminal_wakes=activations,
        resumes_avoided=logical - activations,
        hot_allocations=0,
        baseline_checksum=checksum,
        candidate_checksum=checksum,
        pending_path_valid=1,
        baseline_service_gap_p99_ns=1_000,
        candidate_service_gap_p99_ns=round(1_000 * service_ratio),
        baseline_terminal_p99_ns=2_000,
        candidate_terminal_p99_ns=round(2_000 * terminal_ratio),
        peer="process",
        cpu_scope="server",
        platform="linux_io_uring",
        order=order,
    )


SOURCE_COMMIT = "0123456789abcdef0123456789abcdef01234567"


def _specialized_samples(samples: int = 5) -> list[SampleRow]:
    raw: list[SampleRow] = []
    for cell in screen_matrix():
        core = (
            cell.candidate == "link_skip"
            and cell.ops in {4, 8}
            and cell.concurrency in {64, 512}
            and cell.payload in {64, 1024}
        )
        control = cell.ops == 1
        for sample in range(1, samples + 1):
            raw.append(
                SampleRow(
                    process_sample=sample,
                    row=_pair_for_cell(
                        cell,
                        wall_speedup=1.60 if core else 1.0,
                        cpu_ratio=0.65 if core else 1.0,
                        service_ratio=1.05 if control else 1.0,
                        terminal_ratio=1.05 if core else 1.0,
                        order="ABBA" if sample % 2 else "BAAB",
                    ),
                )
            )
    return raw


def _specialized_summaries(samples: int = 5) -> list[object]:
    return summarize(_specialized_samples(samples))


def _evidence_metadata(samples: int = 5) -> dict[str, object]:
    return {
        "source_commit": SOURCE_COMMIT,
        "source_dirty_digest": "clean",
        "architecture": "arm64",
        "kernel": "test-kernel",
        "toolchain": "test-toolchain",
        "commands": [["bench", "--samples", str(samples)]],
        "cpu_policy": {"scope": "server"},
        "samples": samples,
        "activations": 128,
        "min_mode_ms": 100,
        "unavailable_reason": None,
    }


class ParserContractTests(unittest.TestCase):
    def test_exact_valid_row(self) -> None:
        row = parse_output(VALID_ROW)
        self.assertEqual(row.candidate, "link_skip")
        self.assertEqual(row.ops, 4)
        self.assertEqual(row.observed_cqes, 64)
        self.assertEqual(row.suppressed_success_cqes, 192)
        self.assertEqual(row.platform, "linux_io_uring")

    def test_rejects_malformed_unsafe_and_inconsistent_rows(self) -> None:
        bad_rows = (
            "",
            "noise\n" + VALID_ROW,
            VALID_ROW + "\n" + VALID_ROW,
            VALID_ROW.replace("version=1", "version=2"),
            VALID_ROW.replace(
                "version=1 candidate=link_skip",
                "candidate=link_skip version=1",
            ),
            VALID_ROW.replace("wall_speedup=2.000000000", "wall_speedup=nan"),
            VALID_ROW.replace("cpu_ratio=0.588235294", "cpu_ratio=inf"),
            VALID_ROW.replace("candidate_wall_ns=100000000", "candidate_wall_ns=-1"),
            VALID_ROW.replace(
                "candidate_cpu_ns=100000000",
                "candidate_cpu_ns=18446744073709551616",
            ),
            VALID_ROW.replace("candidate=link_skip", "candidate=unknown"),
            VALID_ROW.replace("ops=4", "ops=3"),
            VALID_ROW.replace("blocks_per_mode=8", "blocks_per_mode=16"),
            VALID_ROW.replace("min_mode_ns=100000000", "min_mode_ns=200000001"),
            VALID_ROW.replace("logical_operations=256", "logical_operations=255"),
            VALID_ROW.replace("queue_publications=64", "queue_publications=63"),
            VALID_ROW.replace("prepared_sqes=256", "prepared_sqes=255"),
            VALID_ROW.replace("ring_submit_syscalls=16", "ring_submit_syscalls=17"),
            VALID_ROW.replace("expected_cqes=64", "expected_cqes=65"),
            VALID_ROW.replace("observed_cqes=64", "observed_cqes=65"),
            VALID_ROW.replace(
                "suppressed_success_cqes=192",
                "suppressed_success_cqes=191",
            ),
            VALID_ROW.replace("candidate_task_parks=64", "candidate_task_parks=63"),
            VALID_ROW.replace("terminal_wakes=64", "terminal_wakes=63"),
            VALID_ROW.replace("resumes_avoided=192", "resumes_avoided=191"),
            VALID_ROW.replace("hot_allocations=0", "hot_allocations=1"),
            VALID_ROW.replace("pending_path_valid=1", "pending_path_valid=0"),
            VALID_ROW.replace(
                "candidate_checksum=0123456789abcdef",
                "candidate_checksum=fedcba9876543210",
            ),
            VALID_ROW.replace("peer=process", "peer=thread"),
            VALID_ROW.replace("cpu_scope=server", "cpu_scope=combined"),
            VALID_ROW.replace("platform=linux_io_uring", "platform=portable"),
            VALID_ROW.replace("order=ABBA", "order=AABB"),
            VALID_ROW.replace(
                "wall_speedup=2.000000000",
                "wall_speedup=1.999000000",
            ),
            VALID_ROW + " unknown=1",
            VALID_ROW.replace(" candidate=link_skip", ""),
        )
        for row in bad_rows:
            with self.subTest(row=row):
                with self.assertRaises(ValueError):
                    parse_output(row)

    def test_rejects_duplicate_field(self) -> None:
        with self.assertRaises(ValueError):
            parse_output(VALID_ROW.replace("version=1", "version=1 version=1"))


class MatrixAndClassifierTests(unittest.TestCase):
    def test_screen_matrix_is_complete_and_unique(self) -> None:
        matrix = screen_matrix()
        self.assertEqual(len(matrix), 72)
        self.assertEqual(len({_cell_key(cell) for cell in matrix}), 72)
        self.assertEqual({cell.candidate for cell in matrix}, {"link", "link_skip"})
        self.assertEqual({cell.ops for cell in matrix}, {1, 2, 4, 8})
        self.assertEqual({cell.concurrency for cell in matrix}, {1, 64, 512})
        self.assertEqual({cell.payload for cell in matrix}, {64, 1024, 16384})

    def test_summary_uses_medians_and_retains_spread(self) -> None:
        cell = MatrixCell("link_skip", 4, 64, 64)
        rows = [
            SampleRow(
                process_sample=index,
                row=_pair_for_cell(
                    cell,
                    wall_speedup=speedup,
                    cpu_ratio=0.60 + index / 100,
                    order="ABBA" if index % 2 else "BAAB",
                ),
            )
            for index, speedup in enumerate((1.50, 1.60, 1.65), start=1)
        ]
        summary = summarize(rows)[0]
        self.assertAlmostEqual(summary.wall_speedup, 1.60, places=6)
        self.assertAlmostEqual(summary.cpu_ratio, 0.62, places=6)
        self.assertAlmostEqual(summary.wall_ratio_spread, 1.10, places=6)
        self.assertTrue(summary.mechanism_valid)

    def test_specialized_and_stable_reject(self) -> None:
        rows = _specialized_summaries()
        verdict, _ = classify(
            rows,
            expected_samples=5,
            min_mode_ns=100_000_000,
        )
        self.assertEqual(verdict, "SPECIALIZED")

        core_index = next(
            index
            for index, row in enumerate(rows)
            if row.candidate == "link_skip"
            and row.ops == 4
            and row.concurrency == 64
            and row.payload == 64
        )
        rejected = rows.copy()
        rejected[core_index] = replace(rejected[core_index], wall_speedup=1.49)
        self.assertEqual(
            classify(
                rejected,
                expected_samples=5,
                min_mode_ns=100_000_000,
            )[0],
            "REJECT",
        )

        control_index = next(
            index
            for index, row in enumerate(rows)
            if row.candidate == "link" and row.ops == 1
        )
        control_reject = rows.copy()
        control_reject[control_index] = replace(
            control_reject[control_index], cpu_ratio=1.051
        )
        self.assertEqual(
            classify(
                control_reject,
                expected_samples=5,
                min_mode_ns=100_000_000,
            )[0],
            "REJECT",
        )

    def test_integrity_and_instability_are_inconclusive(self) -> None:
        rows = _specialized_summaries()
        cases = (
            rows[:-1],
            [replace(rows[0], sample_count=4), *rows[1:]],
            [replace(rows[0], min_mode_ns=99_999_999), *rows[1:]],
            [replace(rows[0], mechanism_valid=False), *rows[1:]],
            [replace(rows[0], wall_ratio_spread=1.100001), *rows[1:]],
            [replace(rows[0], cpu_ratio_spread=1.100001), *rows[1:]],
        )
        for summaries in cases:
            with self.subTest(summaries=len(summaries)):
                self.assertEqual(
                    classify(
                        summaries,
                        expected_samples=5,
                        min_mode_ns=100_000_000,
                    )[0],
                    "INCONCLUSIVE",
                )

    def test_latency_thresholds_are_stable_rejects(self) -> None:
        rows = _specialized_summaries()
        core_index = next(
            i
            for i, row in enumerate(rows)
            if row.candidate == "link_skip"
            and row.ops == 8
            and row.concurrency == 512
            and row.payload == 1024
        )
        core_bad = rows.copy()
        core_bad[core_index] = replace(
            core_bad[core_index], terminal_p99_ratio=1.100001
        )
        self.assertEqual(
            classify(core_bad, expected_samples=5, min_mode_ns=100_000_000)[0],
            "REJECT",
        )

        control_index = next(i for i, row in enumerate(rows) if row.ops == 1)
        control_bad = rows.copy()
        control_bad[control_index] = replace(
            control_bad[control_index], service_gap_p99_ratio=1.100001
        )
        self.assertEqual(
            classify(
                control_bad,
                expected_samples=5,
                min_mode_ns=100_000_000,
            )[0],
            "REJECT",
        )


class RunnerContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cell = MatrixCell("link_skip", 4, 64, 64)

    def _run_with(self, result: CapturedProcess) -> SampleRow:
        def runner(*args: object, **kwargs: object) -> CapturedProcess:
            self.assertTrue(args)
            self.assertEqual(
                kwargs.get("max_output_bytes"),
                MAX_OUTPUT_BYTES,
            )
            self.assertEqual(kwargs.get("timeout"), 120.0)
            return result

        return run_one(
            Path("/tmp/bench_leir_native_segment"),
            self.cell,
            process_sample=1,
            activations=8,
            min_mode_ms=100,
            runner=runner,
        )

    def test_valid_row_is_bound_to_command(self) -> None:
        sample = self._run_with(CapturedProcess(["bench"], 0, VALID_ROW + "\n", ""))
        self.assertEqual(_cell_key(sample.row.cell), _cell_key(self.cell))
        self.assertEqual(sample.row.order, "ABBA")

    def test_timeout_nonzero_stderr_multiple_rows_and_output_caps_fail(self) -> None:
        def timeout_runner(*args: object, **kwargs: object) -> CapturedProcess:
            del args, kwargs
            raise ProcessTimeoutError(["bench"], 1.0, "", "")

        with self.assertRaises(MatrixRunError):
            run_one(
                Path("/tmp/bench"),
                self.cell,
                process_sample=1,
                activations=8,
                min_mode_ms=100,
                runner=timeout_runner,
            )

        cases = (
            CapturedProcess(["bench"], 3, "", "failed"),
            CapturedProcess(["bench"], 0, VALID_ROW, "diagnostic"),
            CapturedProcess(["bench"], 0, VALID_ROW + "\n" + VALID_ROW, ""),
            CapturedProcess(["bench"], 0, "x" * (MAX_OUTPUT_BYTES + 1), ""),
            CapturedProcess(["bench"], 0, VALID_ROW, "x" * (MAX_OUTPUT_BYTES + 1)),
            CapturedProcess(
                ["bench"],
                0,
                VALID_ROW,
                "",
                stdout_truncated=True,
            ),
        )
        for result in cases:
            with self.subTest(returncode=result.returncode):
                with self.assertRaises(MatrixRunError):
                    self._run_with(result)

    def test_backend_unavailable_is_distinct(self) -> None:
        with self.assertRaises(NativeUnavailable):
            self._run_with(
                CapturedProcess(
                    ["bench"],
                    77,
                    "",
                    "LEIR_NATIVE_SKIP candidate=link_skip "
                    "reason=backend_unavailable\n",
                )
            )

    def test_malformed_exit_77_is_not_unavailable(self) -> None:
        with self.assertRaises(MatrixRunError) as caught:
            self._run_with(
                CapturedProcess(
                    ["bench"],
                    77,
                    "",
                    "LEIR_NATIVE_SKIP candidate=link_skip "
                    "reason=unexpected\n",
                )
            )
        self.assertNotIsInstance(caught.exception, NativeUnavailable)


class EvidenceAndCliTests(unittest.TestCase):
    def test_cli_reports_audit_recovery_for_uncertain_publication(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "evidence"
            error = PublicationUncertainError(
                output,
                OSError("parent fsync failed"),
            )
            stderr = io.StringIO()
            with mock.patch(
                "scripts.bench_leir_native.run_matrix",
                side_effect=NativeUnavailable(
                    "cell 1/72: native backend unavailable",
                    (),
                ),
            ), mock.patch(
                "scripts.bench_leir_native.write_evidence",
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

    def test_cli_unavailable_contract_finalizes_inconclusive_bundle(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            output = root / "evidence"
            unavailable = NativeUnavailable(
                "cell 1/72: native backend unavailable",
                (),
            )
            with mock.patch(
                "scripts.bench_leir_native.run_matrix",
                side_effect=unavailable,
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
            verdict, reasons = audit_existing(output)
            self.assertEqual(verdict, "INCONCLUSIVE")
            self.assertEqual(
                reasons,
                ["cell 1/72: native backend unavailable"],
            )

    def test_evidence_bundle_names_audit_and_no_tracked_overwrite(
        self,
    ) -> None:
        samples = _specialized_samples()
        summaries = summarize(samples)
        verdict, reasons = classify(
            summaries,
            expected_samples=5,
            min_mode_ns=100_000_000,
            expected_cells=screen_matrix(),
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            tracked = root / "tracked.md"
            write_evidence(
                root / "evidence",
                tracked,
                samples,
                summaries,
                phase="screen",
                verdict=verdict,
                reasons=reasons,
                metadata=_evidence_metadata(),
            )
            evidence = root / "evidence"
            self.assertEqual(
                {entry.name for entry in evidence.iterdir()},
                {
                    "raw.csv",
                    "summary.csv",
                    "metadata.json",
                    "verdict.json",
                    "report.md",
                    "MANIFEST.sha256",
                },
            )
            report = evidence / "report.md"
            self.assertFalse(tracked.exists())
            text = report.read_text(encoding="utf-8")
            self.assertIn("SPECIALIZED", text)
            self.assertIn("not `CATEGORY`", text)
            self.assertIn("1.50x", text)
            self.assertIn("0.70x", text)
            self.assertEqual(
                audit_existing(
                    evidence,
                    required_source_commit=SOURCE_COMMIT,
                    required_source_dirty_digest="clean",
                ),
                (verdict, reasons),
            )
            before = report.stat().st_mtime_ns
            self.assertEqual(
                audit_existing(evidence),
                (verdict, reasons),
            )
            self.assertEqual(before, report.stat().st_mtime_ns)

    def test_cli_and_command_use_argument_arrays(self) -> None:
        args = _build_parser().parse_args(
            [
                "--binary",
                "/tmp/bench",
                "--phase",
                "gate",
                "--samples",
                "9",
                "--output-dir",
                "/tmp/evidence",
            ]
        )
        self.assertEqual(args.phase, "gate")
        audit_args = _build_parser().parse_args(
            [
                "--audit-existing",
                "/tmp/evidence",
                "--require-source-commit",
                SOURCE_COMMIT,
                "--require-source-dirty-digest",
                "clean",
            ]
        )
        self.assertEqual(
            audit_args.require_source_commit,
            SOURCE_COMMIT,
        )
        command = benchmark_command(
            Path("/tmp/bench"),
            MatrixCell("link_skip", 8, 512, 1024),
            activations=128,
            min_mode_ms=250,
            order="BAAB",
        )
        self.assertEqual(command[command.index("--candidate") + 1], "link_skip")
        self.assertEqual(command[command.index("--order") + 1], "BAAB")

    def test_optional_native_binary_smoke(self) -> None:
        binary = os.environ.get("LEIR_NATIVE_TEST_BINARY")
        if not binary:
            self.skipTest("LEIR_NATIVE_TEST_BINARY is not set")
        cell = MatrixCell("link_skip", 8, 4, 64)
        result = run_capture(
            benchmark_command(
                Path(binary),
                cell,
                activations=8,
                min_mode_ms=1,
                order="ABBA",
            ),
            timeout=30.0,
            max_output_bytes=MAX_OUTPUT_BYTES,
        )
        if result.returncode == 77:
            self.skipTest("Linux io_uring native backend is unavailable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        row = parse_output(result.stdout)
        self.assertEqual(_cell_key(row.cell), _cell_key(cell))


class WorkflowContractTests(unittest.TestCase):
    def test_linux_native_research_workflow_is_source_pinned_and_complete(
        self,
    ) -> None:
        root = Path(__file__).resolve().parents[1]
        workflow_path = (
            root
            / ".github"
            / "workflows"
            / "leir-native-research.yml"
        )
        self.assertTrue(workflow_path.is_file())
        workflow = workflow_path.read_text(encoding="utf-8")
        required_fragments = (
            "runs-on: ubuntu-24.04",
            "permissions:\n  contents: read",
            'echo "OUT_DIR=$RUNNER_TEMP/leir-native-screen" >> "$GITHUB_ENV"',
            "actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803",
            "actions/setup-python@ece7cb06caefa5fff74198d8649806c4678c61a1",
            "actions/upload-artifact@b7c566a772e6b6bfb58ed0dc250532a479d7789f",
            'test "$(git rev-parse HEAD)" = "$GITHUB_SHA"',
            "liburing-dev",
            "Make native pipeline correctness gate",
            "test_leir_native_plan",
            "test_leir_native_segment",
            "test_leir_native_linux",
            "bench_leir_native_segment",
            "bench_leir_native_pipeline",
            "-fsanitize=address,undefined",
            "-fsanitize=thread",
            "asan-native-bench.log",
            "make -j2 test",
            "audit-shared-exports",
            "audit-production-test-hooks",
            "scripts/verify_linux.sh",
            "benchmark_cpus=",
            "expected at least two benchmark CPUs",
            'taskset -c "$benchmark_cpus"',
            "--samples 9",
            "--min-mode-ms 20",
            "uname -a",
            "lscpu",
            "gcc --version",
            "clang --version",
            "pkg-config --modversion liburing",
            "ulimit -l",
            "/proc/sys/kernel/io_uring_disabled",
            "feature-status.txt",
            "max_output_bytes=64 * 1024",
            "timeout=30",
            "if: always()",
            "path: ${{ runner.temp }}/leir-native-screen",
        )
        for fragment in required_fragments:
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, workflow)
        self.assertNotIn(
            "python3 scripts/bench_leir_native.py",
            workflow,
        )
        self.assertNotIn("uses: actions/checkout@v", workflow)
        self.assertNotIn("uses: actions/setup-python@v", workflow)
        self.assertNotIn("uses: actions/upload-artifact@v", workflow)


if __name__ == "__main__":
    unittest.main()
