#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import hashlib
import os
import tempfile
from dataclasses import replace
from pathlib import Path

from bench_lccf_model import (
    MatrixCell,
    PairRow,
    SampleRow,
    SummaryRow,
    _build_parser,
    benchmark_command,
    classify,
    full_matrix,
    parse_output,
    quick_matrix,
    summarize,
    write_evidence,
)
from process_utils import run_capture


SAMPLE_ROW = (
    "LCCF_PAIR version=1 workload=completion_io_pipeline "
    "candidate=fused_causal_cell baseline=waker_queue instances=4096 "
    "frame_bytes=128 cell_bytes=96 sites=8 budget=8 chain=1 producers=2 "
    "min_mode_ns=250000000 rounds_per_block=1024 ops_per_mode=8388608 "
    "baseline_wall_ns=450000000 candidate_wall_ns=300000000 "
    "baseline_cpu_ns=400000000 candidate_cpu_ns=280000000 "
    "wall_speedup=1.500000000 cpu_ratio=0.700000000 "
    "baseline_fair_p99_ns=0 candidate_fair_p99_ns=0 "
    "checksum=0123456789abcdef hot_allocations=0 forced_escapes=0 "
    "direct_calls=8388608 order=ABBA affinity=none"
)


def expect_rejected(text: str) -> None:
    try:
        parse_output(text)
    except ValueError:
        return
    raise AssertionError(f"expected parser rejection: {text!r}")


def test_exact_parser_contract() -> None:
    row = parse_output(SAMPLE_ROW)
    assert row == PairRow(
        workload="completion_io_pipeline",
        candidate="fused_causal_cell",
        baseline="waker_queue",
        instances=4096,
        frame_bytes=128,
        cell_bytes=96,
        sites=8,
        budget=8,
        chain=1,
        producers=2,
        min_mode_ns=250_000_000,
        rounds_per_block=1024,
        ops_per_mode=8_388_608,
        baseline_wall_ns=450_000_000,
        candidate_wall_ns=300_000_000,
        baseline_cpu_ns=400_000_000,
        candidate_cpu_ns=280_000_000,
        wall_speedup=1.5,
        cpu_ratio=0.7,
        baseline_fair_p99_ns=0,
        candidate_fair_p99_ns=0,
        checksum="0123456789abcdef",
        hot_allocations=0,
        forced_escapes=0,
        direct_calls=8_388_608,
        order="ABBA",
        affinity="none",
    )


def test_parser_rejections() -> None:
    bad_rows = (
        "",
        "noise\nLCCF_PAIR version=1",
        SAMPLE_ROW + "\n" + SAMPLE_ROW,
        SAMPLE_ROW.replace("version=1", "version=2"),
        SAMPLE_ROW.replace(
            "wall_speedup=1.500000000", "wall_speedup=nan"
        ),
        SAMPLE_ROW.replace("hot_allocations=0", "hot_allocations=1"),
        SAMPLE_ROW.replace("order=ABBA", "order=AABB"),
        SAMPLE_ROW.replace(
            "candidate=fused_causal_cell",
            "candidate=remote_causal_cell",
        ),
        SAMPLE_ROW.replace(
            "candidate_wall_ns=300000000",
            "candidate_wall_ns=299000000",
        ),
        SAMPLE_ROW.replace(
            "candidate_wall_ns=300000000",
            "candidate_wall_ns=249999999",
        ),
        SAMPLE_ROW + " extra=1",
    )
    for row in bad_rows:
        expect_rejected(row)


def _checksum(cell: MatrixCell) -> str:
    identity = (
        f"{cell.workload}:{cell.frame_bytes}:"
        f"{cell.sites}:{cell.chain}"
    )
    return hashlib.sha256(identity.encode()).hexdigest()[:16]


def _summary_for_cell(
    cell: MatrixCell,
    *,
    fused_workloads: set[str],
    passing_cells: set[int],
    samples: int = 9,
    min_mode_ns: int = 250_000_000,
) -> SummaryRow:
    wall_speedup = 1.0
    cpu_ratio = 1.0
    fairness_ratio = 1.0

    if (
        cell.candidate == "fused_causal_cell"
        and cell.cell_bytes in passing_cells
        and cell.workload in fused_workloads
    ):
        wall_speedup = (
            1.6
            if len(fused_workloads) > 1
            else 1.3
        )
        cpu_ratio = 0.75
    elif cell.candidate == "causal_cell_queue":
        wall_speedup = 1.0
        cpu_ratio = 1.0
    elif cell.candidate == "remote_causal_cell":
        wall_speedup = 1.0
        cpu_ratio = 1.0
    elif cell.workload == "completion_mixed_fairness":
        fairness_ratio = 1.05

    return SummaryRow(
        workload=cell.workload,
        candidate=cell.candidate,
        baseline=cell.baseline,
        frame_bytes=cell.frame_bytes,
        cell_bytes=cell.cell_bytes,
        sites=cell.sites,
        budget=cell.budget,
        chain=cell.chain,
        producers=cell.producers,
        sample_count=samples,
        wall_speedup=wall_speedup,
        cpu_ratio=cpu_ratio,
        wall_ratio_spread=1.02 if samples > 1 else 1.0,
        cpu_ratio_spread=1.02 if samples > 1 else 1.0,
        baseline_wall_ns_per_op=100.0,
        candidate_wall_ns_per_op=100.0 / wall_speedup,
        baseline_cpu_ns_per_op=100.0,
        candidate_cpu_ns_per_op=100.0 * cpu_ratio,
        fairness_p99_ratio=fairness_ratio,
        checksum=_checksum(cell),
        min_mode_ns=min_mode_ns,
    )


def verdict_fixture(
    *,
    fused_workloads: set[str],
    passing_cells: set[int],
) -> list[SummaryRow]:
    return [
        _summary_for_cell(
            cell,
            fused_workloads=fused_workloads,
            passing_cells=passing_cells,
        )
        for cell in full_matrix()
    ]


def test_layout_selection_and_verdicts() -> None:
    functional = {
        "completion_io_pipeline",
        "completion_rpc_state",
        "completion_timer_cancel",
    }
    rows = verdict_fixture(
        fused_workloads=functional,
        passing_cells={96, 128},
    )
    verdict, reasons, selected = classify(rows, 9)
    assert verdict == "PROMISING"
    assert selected == 96
    assert reasons

    rows = verdict_fixture(
        fused_workloads={"completion_io_pipeline"},
        passing_cells={96},
    )
    verdict, reasons, selected = classify(rows, 9)
    assert verdict == "NARROW"
    assert selected == 96
    assert any("completion_io_pipeline" in reason for reason in reasons)

    rows = verdict_fixture(
        fused_workloads=set(),
        passing_cells=set(),
    )
    verdict, reasons, selected = classify(rows, 9)
    assert verdict == "REJECT"
    assert selected is None
    assert reasons


def test_inconclusive_integrity_gates() -> None:
    functional = {
        "completion_io_pipeline",
        "completion_rpc_state",
        "completion_timer_cancel",
    }
    rows = verdict_fixture(
        fused_workloads=functional,
        passing_cells={96},
    )

    assert classify(rows[:-1], 9)[0] == "INCONCLUSIVE"

    wrong_samples = rows.copy()
    wrong_samples[0] = replace(wrong_samples[0], sample_count=8)
    assert classify(wrong_samples, 9)[0] == "INCONCLUSIVE"

    short = rows.copy()
    short[0] = replace(short[0], min_mode_ns=249_999_999)
    assert classify(short, 9)[0] == "INCONCLUSIVE"

    mismatch = rows.copy()
    mismatch[1] = replace(
        mismatch[1], checksum="MISMATCH"
    )
    assert classify(mismatch, 9)[0] == "INCONCLUSIVE"

    wall_spread = rows.copy()
    wall_spread[2] = replace(
        wall_spread[2], wall_ratio_spread=1.100001
    )
    assert classify(wall_spread, 9)[0] == "INCONCLUSIVE"

    cpu_spread = rows.copy()
    cpu_spread[3] = replace(
        cpu_spread[3], cpu_ratio_spread=1.100001
    )
    assert classify(cpu_spread, 9)[0] == "INCONCLUSIVE"


def test_median_keeps_one_process_pair() -> None:
    base = parse_output(SAMPLE_ROW)
    rows: list[SampleRow] = []
    speedups = [10.0, 1.40, 1.41, 1.42, 1.43, 1.44, 1.45, 1.46, 1.47]
    for index, speedup in enumerate(speedups, start=1):
        baseline_wall = 450_000_000
        candidate_wall = round(baseline_wall / speedup)
        actual_speedup = baseline_wall / candidate_wall
        rows.append(
            SampleRow(
                index,
                replace(
                    base,
                    baseline_wall_ns=baseline_wall,
                    candidate_wall_ns=candidate_wall,
                    wall_speedup=actual_speedup,
                    cpu_ratio=0.50 + index / 100.0,
                    candidate_cpu_ns=round(
                        base.baseline_cpu_ns
                        * (0.50 + index / 100.0)
                    ),
                ),
            )
        )
    summary = summarize(rows)
    assert len(summary) == 1
    selected = summary[0]
    assert abs(selected.wall_speedup - 1.44) < 0.001
    assert abs(selected.cpu_ratio - 0.56) < 1e-12
    assert selected.wall_ratio_spread > 6.0


def test_quick_is_smoke_only() -> None:
    rows = [
        _summary_for_cell(
            cell,
            fused_workloads=set(),
            passing_cells=set(),
            samples=1,
            min_mode_ns=20_000_000,
        )
        for cell in quick_matrix()
    ]
    verdict, reasons, selected = classify(rows, 1)
    assert verdict == "SMOKE_ONLY"
    assert selected == 96
    assert reasons


def test_command_and_binary_smoke() -> None:
    cell = quick_matrix()[0]
    command = benchmark_command(
        Path("/tmp/bench_lccf_model"),
        cell,
        instances=4096,
        min_mode_ms=20,
        seed=7,
        order="ABBA",
        owner_cpu="none",
    )
    assert command[0] == "/tmp/bench_lccf_model"
    assert command[command.index("--candidate") + 1] == cell.candidate
    assert command[command.index("--order") + 1] == "abba"
    assert command[command.index("--owner-cpu") + 1] == "none"

    binary = os.environ.get("LCCF_MODEL_TEST_BINARY")
    if not binary:
        return
    command[0] = binary
    command[command.index("--min-mode-ms") + 1] = "5"
    result = run_capture(command, timeout=30.0)
    assert result.returncode == 0, result.stderr
    row = parse_output(result.stdout)
    assert row.candidate == cell.candidate
    assert row.instances == 4096


def test_reproduction_cli_contract() -> None:
    args = _build_parser().parse_args(
        [
            "--binary",
            "/tmp/bench_lccf_model",
            "--samples",
            "9",
            "--instances",
            "65536",
            "--min-mode-ms",
            "250",
            "--budget",
            "8",
            "--chain",
            "18",
            "--producers",
            "2",
        ]
    )
    assert args.budget == 8
    assert args.chain == 18
    assert args.producers == 2


def test_evidence_contract() -> None:
    rows = verdict_fixture(
        fused_workloads=set(),
        passing_cells=set(),
    )
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        tracked = root / "tracked.md"
        write_evidence(
            root / "evidence",
            tracked,
            [],
            rows,
            "REJECT",
            ["no workload passed"],
            None,
            {"command": "python3 scripts/bench_lccf_model.py --quick"},
        )
        report = (
            root / "evidence" / "lccf_phase0_report.md"
        ).read_text(encoding="utf-8")
        metadata = (
            root / "evidence" / "lccf_phase0_metadata.json"
        ).read_text(encoding="utf-8")
        assert (root / "evidence" / "lccf_phase0_samples.csv").is_file()
        assert (root / "evidence" / "lccf_phase0_summary.csv").is_file()
        assert tracked.read_text(encoding="utf-8") == report
    assert "REJECT" in report
    assert "not production validation" in report
    assert "command" in metadata


def main() -> int:
    test_exact_parser_contract()
    test_parser_rejections()
    test_layout_selection_and_verdicts()
    test_inconclusive_integrity_gates()
    test_median_keeps_one_process_pair()
    test_quick_is_smoke_only()
    test_command_and_binary_smoke()
    test_reproduction_cli_contract()
    test_evidence_contract()
    print("[test_bench_lccf_model] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
