#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import os
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from scripts.bench_leir_phase0 import (
    MatrixCell,
    MatrixRunError,
    PairRow,
    SampleRow,
    _build_parser,
    benchmark_command,
    classify,
    classify_focused,
    gate_matrix,
    parse_output,
    parse_focus_cell,
    run_one,
    screen_matrix,
    summarize,
    write_evidence,
)
from scripts.process_utils import (
    CapturedProcess,
    ProcessTimeoutError,
    run_capture,
)


VALID_ROW = (
    "LEIR_PAIR version=1 workload=socket_relay nodes=4 "
    "concurrency=64 payload=64 inline_budget=8 activations=4096 "
    "min_mode_ns=100000000 blocks_per_mode=16 "
    "baseline_wall_ns=200 candidate_wall_ns=100 "
    "baseline_cpu_ns=170 candidate_cpu_ns=100 "
    "wall_speedup=2.000000000 cpu_ratio=0.588235294 "
    "baseline_ctx_switches=8 candidate_ctx_switches=2 "
    "baseline_task_io_submits=131072 candidate_task_io_submits=32768 "
    "baseline_task_io_completions=131072 "
    "candidate_task_io_completions=32768 "
    "candidate_backend_submits=131072 effect_completions=131072 "
    "direct_completions=0 terminal_publications=32768 "
    "resumes_avoided=98304 fairness_resubmits=0 heap_requests=0 "
    "hot_allocations=0 baseline_checksum=0123456789abcdef "
    "candidate_checksum=0123456789abcdef pending_path_valid=1 "
    "baseline_service_gap_p99_ns=100 "
    "candidate_service_gap_p99_ns=100 "
    "baseline_terminal_p99_ns=100 candidate_terminal_p99_ns=100 "
    "peer=process cpu_scope=server order=ABBA"
)


def _expect_rejected(
    testcase: unittest.TestCase,
    text: str,
    *,
    allow_combined_cpu: bool = False,
) -> None:
    with testcase.assertRaises(ValueError):
        parse_output(text, allow_combined_cpu=allow_combined_cpu)


def _cell_key(cell: MatrixCell) -> tuple[object, ...]:
    return (
        cell.workload,
        cell.nodes,
        cell.concurrency,
        cell.payload,
        cell.inline_budget,
    )


def _pair_for_cell(
    cell: MatrixCell,
    *,
    wall_speedup: float,
    cpu_ratio: float,
    min_mode_ns: int = 250_000_000,
    service_ratio: float = 1.0,
    terminal_ratio: float = 1.0,
    cpu_scope: str = "server",
    order: str = "ABBA",
) -> PairRow:
    aggregate_activations = 64
    activations = 8
    blocks = 16
    effect_nodes = 1 if cell.workload == "graph_break" else cell.nodes
    transactions = 1 if effect_nodes == 1 else effect_nodes // 2
    baseline_io = aggregate_activations * transactions * 2
    candidate_io = aggregate_activations * (
        2 if effect_nodes == 1 else 1
    )
    effect_completions = aggregate_activations * effect_nodes
    checksum = (
        f"{abs(hash(_cell_key(cell))) & 0xFFFF_FFFF_FFFF_FFFF:016x}"
    )
    baseline_wall_ns = 1_000_000_000
    candidate_wall_ns = round(baseline_wall_ns / wall_speedup)
    baseline_cpu_ns = 1_000_000_000
    candidate_cpu_ns = round(baseline_cpu_ns * cpu_ratio)
    peer = "thread" if cpu_scope == "combined" else "process"

    return PairRow(
        workload=cell.workload,
        nodes=cell.nodes,
        concurrency=cell.concurrency,
        payload=cell.payload,
        inline_budget=cell.inline_budget,
        activations=activations,
        min_mode_ns=min_mode_ns,
        blocks_per_mode=blocks,
        baseline_wall_ns=baseline_wall_ns,
        candidate_wall_ns=candidate_wall_ns,
        baseline_cpu_ns=baseline_cpu_ns,
        candidate_cpu_ns=candidate_cpu_ns,
        wall_speedup=baseline_wall_ns / candidate_wall_ns,
        cpu_ratio=candidate_cpu_ns / baseline_cpu_ns,
        baseline_ctx_switches=256,
        candidate_ctx_switches=64,
        baseline_task_io_submits=baseline_io,
        candidate_task_io_submits=candidate_io,
        baseline_task_io_completions=baseline_io,
        candidate_task_io_completions=candidate_io,
        candidate_backend_submits=effect_completions,
        effect_completions=effect_completions,
        direct_completions=0,
        terminal_publications=aggregate_activations,
        resumes_avoided=aggregate_activations * (effect_nodes - 1),
        fairness_resubmits=0,
        heap_requests=0,
        hot_allocations=0,
        baseline_checksum=checksum,
        candidate_checksum=checksum,
        pending_path_valid=1,
        baseline_service_gap_p99_ns=1_000,
        candidate_service_gap_p99_ns=round(1_000 * service_ratio),
        baseline_terminal_p99_ns=2_000,
        candidate_terminal_p99_ns=round(2_000 * terminal_ratio),
        peer=peer,
        cpu_scope=cpu_scope,
        order=order,
    )


def _gate_summaries(
    *,
    core_wall: float,
    core_cpu: float,
    short_wall: float = 1.0,
    short_cpu: float = 1.0,
    samples: int = 3,
    cpu_scope: str = "server",
) -> list[object]:
    raw: list[SampleRow] = []
    for cell in gate_matrix():
        is_short = cell.workload == "graph_break" or cell.nodes == 1
        for sample in range(1, samples + 1):
            raw.append(
                SampleRow(
                    process_sample=sample,
                    row=_pair_for_cell(
                        cell,
                        wall_speedup=(
                            short_wall if is_short else core_wall
                        ),
                        cpu_ratio=(
                            short_cpu if is_short else core_cpu
                        ),
                        cpu_scope=cpu_scope,
                        order="ABBA" if sample % 2 else "BAAB",
                    ),
                )
            )
    return summarize(raw)


class ParserContractTests(unittest.TestCase):
    def test_exact_valid_row(self) -> None:
        row = parse_output(VALID_ROW)
        self.assertEqual(row.workload, "socket_relay")
        self.assertEqual(row.nodes, 4)
        self.assertEqual(row.blocks_per_mode, 16)
        self.assertEqual(row.wall_speedup, 2.0)
        self.assertEqual(row.cpu_scope, "server")

    def test_rejects_malformed_and_unsafe_rows(self) -> None:
        bad_rows = (
            "",
            "noise\n" + VALID_ROW,
            VALID_ROW + "\n" + VALID_ROW,
            VALID_ROW.replace("version=1", "version=2"),
            VALID_ROW.replace(
                "version=1 workload=socket_relay",
                "workload=socket_relay version=1",
            ),
            VALID_ROW.replace("wall_speedup=2.000000000", "wall_speedup=nan"),
            VALID_ROW.replace("candidate_wall_ns=100", "candidate_wall_ns=-1"),
            VALID_ROW.replace(
                "candidate_checksum=0123456789abcdef",
                "candidate_checksum=fedcba9876543210",
            ),
            VALID_ROW.replace("pending_path_valid=1", "pending_path_valid=0"),
            VALID_ROW.replace("heap_requests=0", "heap_requests=1"),
            VALID_ROW.replace("hot_allocations=0", "hot_allocations=1"),
            VALID_ROW.replace(
                "resumes_avoided=98304", "resumes_avoided=98303"
            ),
            VALID_ROW.replace("order=ABBA", "order=AABB"),
            VALID_ROW + " unknown=1",
            VALID_ROW.replace(" nodes=4", ""),
            VALID_ROW.replace(
                "workload=socket_relay", "workload=unknown"
            ),
            VALID_ROW.replace(
                "wall_speedup=2.000000000", "wall_speedup=1.999000000"
            ),
        )
        for row in bad_rows:
            with self.subTest(row=row):
                _expect_rejected(self, row)

    def test_rejects_duplicate_and_combined_gate_cpu(self) -> None:
        duplicate = VALID_ROW.replace(
            "version=1", "version=1 version=1"
        )
        _expect_rejected(self, duplicate)
        combined = VALID_ROW.replace(
            "peer=process cpu_scope=server",
            "peer=thread cpu_scope=combined",
        )
        _expect_rejected(self, combined)
        parsed = parse_output(combined, allow_combined_cpu=True)
        self.assertEqual(parsed.cpu_scope, "combined")


class MatrixAndSummaryTests(unittest.TestCase):
    def test_deterministic_matrix_contracts(self) -> None:
        screen = screen_matrix()
        gate = gate_matrix()
        self.assertEqual(len(screen), 225)
        self.assertEqual(len(gate), 28)
        self.assertEqual(len({_cell_key(cell) for cell in screen}), 225)
        self.assertEqual(len({_cell_key(cell) for cell in gate}), 28)
        controls = [
            cell for cell in screen if cell.workload == "graph_break"
        ]
        self.assertEqual(len(controls), 9)
        self.assertTrue(all(cell.nodes == 1 for cell in controls))
        self.assertTrue(all(cell.inline_budget == 8 for cell in controls))

    def test_summary_uses_paired_medians_and_spread(self) -> None:
        cell = gate_matrix()[0]
        speedups = (1.25, 1.50, 1.75)
        rows = [
            SampleRow(
                process_sample=index,
                row=_pair_for_cell(
                    cell,
                    wall_speedup=speedup,
                    cpu_ratio=0.7 + index / 100,
                    order="ABBA" if index % 2 else "BAAB",
                ),
            )
            for index, speedup in enumerate(speedups, start=1)
        ]
        summary = summarize(rows)[0]
        self.assertAlmostEqual(summary.wall_speedup, 1.5, places=6)
        self.assertAlmostEqual(summary.cpu_ratio, 0.72, places=6)
        self.assertGreater(summary.wall_ratio_spread, 1.39)
        self.assertTrue(summary.mechanism_valid)

    def test_partial_completions_preserve_mechanism_balance(self) -> None:
        cell = MatrixCell("socket_relay", 1, 64, 16384, 8)
        row = _pair_for_cell(
            cell,
            wall_speedup=1.0,
            cpu_ratio=1.0,
        )
        partial = replace(
            row,
            candidate_backend_submits=row.effect_completions * 2,
            effect_completions=row.effect_completions * 2,
            resumes_avoided=row.effect_completions,
        )
        summary = summarize(
            [SampleRow(process_sample=1, row=partial)]
        )[0]
        self.assertTrue(summary.mechanism_valid)

        unbalanced = replace(
            partial,
            resumes_avoided=partial.resumes_avoided - 1,
        )
        bad_summary = summarize(
            [SampleRow(process_sample=1, row=unbalanced)]
        )[0]
        self.assertFalse(bad_summary.mechanism_valid)


class ClassifierTests(unittest.TestCase):
    def _classify(
        self, summaries: list[object]
    ) -> tuple[str, list[str]]:
        verdict, reasons = classify(
            summaries,
            phase="gate",
            expected_samples=3,
            min_mode_ns=250_000_000,
            expected_cells=gate_matrix(),
        )
        return verdict, reasons

    def test_pass_reject_and_inconclusive_bounds(self) -> None:
        passed = _gate_summaries(core_wall=1.30, core_cpu=0.80)
        self.assertEqual(self._classify(passed)[0], "ADVANCE_PASS")

        rejected = _gate_summaries(core_wall=1.05, core_cpu=1.00)
        self.assertEqual(self._classify(rejected)[0], "ADVANCE_REJECT")

        middle = _gate_summaries(core_wall=1.15, core_cpu=0.90)
        self.assertEqual(self._classify(middle)[0], "INCONCLUSIVE")

        short_regression = _gate_summaries(
            core_wall=1.30,
            core_cpu=0.80,
            short_wall=0.94,
            short_cpu=1.00,
        )
        self.assertEqual(
            self._classify(short_regression)[0], "ADVANCE_REJECT"
        )

    def test_integrity_duration_and_missing_sample_are_inconclusive(self) -> None:
        rows = _gate_summaries(core_wall=1.30, core_cpu=0.80)
        self.assertEqual(self._classify(rows[:-1])[0], "INCONCLUSIVE")

        short_duration = rows.copy()
        short_duration[0] = replace(
            short_duration[0], min_mode_ns=249_999_999
        )
        self.assertEqual(
            self._classify(short_duration)[0], "INCONCLUSIVE"
        )

        missing_sample = rows.copy()
        missing_sample[0] = replace(
            missing_sample[0], sample_count=2
        )
        self.assertEqual(
            self._classify(missing_sample)[0], "INCONCLUSIVE"
        )

        bad_mechanism = rows.copy()
        bad_mechanism[0] = replace(
            bad_mechanism[0], mechanism_valid=False
        )
        self.assertEqual(
            self._classify(bad_mechanism)[0], "INCONCLUSIVE"
        )

    def test_exact_spread_boundary(self) -> None:
        rows = _gate_summaries(core_wall=1.30, core_cpu=0.80)
        at_limit = rows.copy()
        at_limit[0] = replace(
            at_limit[0],
            wall_ratio_spread=1.100000,
            cpu_ratio_spread=1.100000,
        )
        self.assertEqual(self._classify(at_limit)[0], "ADVANCE_PASS")

        above = rows.copy()
        above[0] = replace(
            above[0], wall_ratio_spread=1.100001
        )
        self.assertEqual(self._classify(above)[0], "INCONCLUSIVE")

    def test_latency_guard_and_combined_cpu(self) -> None:
        rows = _gate_summaries(core_wall=1.30, core_cpu=0.80)
        latency = rows.copy()
        latency[0] = replace(latency[0], terminal_p99_ratio=1.100001)
        self.assertEqual(self._classify(latency)[0], "INCONCLUSIVE")

        combined = _gate_summaries(
            core_wall=1.30,
            core_cpu=9.0,
            cpu_scope="combined",
        )
        verdict, reasons = self._classify(combined)
        self.assertEqual(verdict, "INCONCLUSIVE")
        self.assertTrue(any("server-only CPU" in reason for reason in reasons))

    def test_focused_diagnostic_ignores_performance_verdicts(self) -> None:
        cell = MatrixCell("socket_relay", 1, 512, 64, 8)
        samples = [
            SampleRow(
                process_sample=index,
                row=_pair_for_cell(
                    cell,
                    wall_speedup=0.25,
                    cpu_ratio=3.0,
                    min_mode_ns=100_000_000,
                    order="ABBA" if index % 2 else "BAAB",
                ),
            )
            for index in range(1, 4)
        ]
        summaries = summarize(samples)
        verdict, reasons = classify_focused(
            summaries,
            expected_cell=cell,
            expected_samples=3,
            min_mode_ns=100_000_000,
        )
        self.assertEqual(verdict, "DIAGNOSTIC_PASS")
        self.assertIn("completed", reasons[0])

        invalid = [replace(summaries[0], mechanism_valid=False)]
        self.assertEqual(
            classify_focused(
                invalid,
                expected_cell=cell,
                expected_samples=3,
                min_mode_ns=100_000_000,
            )[0],
            "DIAGNOSTIC_INCONCLUSIVE",
        )


class RunnerFailureTests(unittest.TestCase):
    def setUp(self) -> None:
        self.cell = MatrixCell("socket_relay", 4, 64, 64, 8)

    def _run_with(self, result: CapturedProcess) -> SampleRow:
        def runner(*args: object, **kwargs: object) -> CapturedProcess:
            del args, kwargs
            return result

        return run_one(
            Path("/tmp/bench_leir_phase0"),
            self.cell,
            process_sample=1,
            activations=128,
            min_mode_ms=100,
            runner=runner,
        )

    def test_timeout_nonzero_stderr_and_two_rows_fail(self) -> None:
        def timeout_runner(
            *args: object, **kwargs: object
        ) -> CapturedProcess:
            del args, kwargs
            raise ProcessTimeoutError(["bench"], 1.0, "", "")

        with self.assertRaises(MatrixRunError):
            run_one(
                Path("/tmp/bench"),
                self.cell,
                process_sample=1,
                activations=128,
                min_mode_ms=100,
                runner=timeout_runner,
            )

        cases = (
            CapturedProcess(["bench"], 3, "", "failed"),
            CapturedProcess(["bench"], 0, VALID_ROW, "diagnostic"),
            CapturedProcess(
                ["bench"], 0, VALID_ROW + "\n" + VALID_ROW, ""
            ),
        )
        for result in cases:
            with self.subTest(result=result):
                with self.assertRaises(MatrixRunError):
                    self._run_with(result)

    def test_valid_row_is_bound_to_command(self) -> None:
        sample = self._run_with(
            CapturedProcess(["bench"], 0, VALID_ROW + "\n", "")
        )
        self.assertEqual(sample.process_sample, 1)
        self.assertEqual(_cell_key(sample.row.cell), _cell_key(self.cell))


class EvidenceAndCliTests(unittest.TestCase):
    def test_report_is_byte_identical_to_tracked_copy(self) -> None:
        summaries = _gate_summaries(core_wall=1.30, core_cpu=0.80)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tracked = root / "tracked.md"
            write_evidence(
                root / "evidence",
                tracked,
                [],
                summaries,
                phase="gate",
                verdict="ADVANCE_PASS",
                reasons=["all continuation gates passed"],
                metadata={"command": "test"},
            )
            raw = (
                root / "evidence" / "leir_phase0a_report.md"
            ).read_bytes()
            self.assertEqual(raw, tracked.read_bytes())
            self.assertTrue(
                (
                    root / "evidence" / "leir_phase0a_samples.csv"
                ).is_file()
            )
            self.assertTrue(
                (
                    root / "evidence" / "leir_phase0a_summary.csv"
                ).is_file()
            )
            report = raw.decode()
            self.assertIn("ADVANCE_PASS", report)
            self.assertIn("not `CATEGORY`", report)
            self.assertIn("1.50x", report)
            self.assertIn("1.25x", report)

    def test_cli_and_command_contract(self) -> None:
        args = _build_parser().parse_args(
            [
                "--binary",
                "/tmp/bench_leir_phase0",
                "--phase",
                "gate",
                "--samples",
                "9",
                "--min-mode-ms",
                "250",
                "--output-dir",
                "/tmp/evidence",
            ]
        )
        self.assertEqual(args.phase, "gate")
        command = benchmark_command(
            Path("/tmp/bench_leir_phase0"),
            gate_matrix()[0],
            activations=128,
            min_mode_ms=250,
            order="BAAB",
        )
        self.assertEqual(command[command.index("--order") + 1], "BAAB")
        self.assertEqual(
            command[command.index("--activations") + 1], "128"
        )

    def test_focused_cell_cli_contract(self) -> None:
        args = _build_parser().parse_args(
            [
                "--binary",
                "/tmp/bench_leir_phase0",
                "--focus-cell",
                "socket_relay,1,512,64,8",
                "--samples",
                "101",
                "--output-dir",
                "/tmp/evidence",
            ]
        )
        self.assertEqual(
            parse_focus_cell(args.focus_cell),
            MatrixCell("socket_relay", 1, 512, 64, 8),
        )
        invalid = (
            "",
            "socket_relay,1,512,64",
            "unknown,1,512,64,8",
            "graph_break,2,512,64,8",
            "socket_relay,3,512,64,8",
            "socket_relay,1,0,64,8",
            "socket_relay,1,512,63,8",
            "socket_relay,1,512,64,0",
        )
        for value in invalid:
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    parse_focus_cell(value)

    def test_optional_native_binary_smoke(self) -> None:
        binary = os.environ.get("LEIR_PHASE0_TEST_BINARY")
        if not binary:
            self.skipTest("LEIR_PHASE0_TEST_BINARY is not set")
        cell = MatrixCell("socket_relay", 4, 4, 64, 8)
        command = benchmark_command(
            Path(binary),
            cell,
            activations=8,
            min_mode_ms=1,
            order="ABBA",
        )
        result = run_capture(command, timeout=30.0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        row = parse_output(
            result.stdout,
            allow_combined_cpu=(os.name == "nt"),
        )
        self.assertEqual(_cell_key(row.cell), _cell_key(cell))

    def test_optional_native_high_fanout_peer_progress(self) -> None:
        binary = os.environ.get("LEIR_PHASE0_TEST_BINARY")
        if not binary:
            self.skipTest("LEIR_PHASE0_TEST_BINARY is not set")
        cell = MatrixCell("socket_relay", 1, 64, 16384, 1)
        command = benchmark_command(
            Path(binary),
            cell,
            activations=64,
            min_mode_ms=1,
            order="ABBA",
        )
        result = run_capture(command, timeout=10.0)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        row = parse_output(
            result.stdout,
            allow_combined_cpu=(os.name == "nt"),
        )
        self.assertEqual(_cell_key(row.cell), _cell_key(cell))


if __name__ == "__main__":
    unittest.main()
