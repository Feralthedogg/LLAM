#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import os
import tempfile
from dataclasses import replace
from pathlib import Path

from bench_srem_model import (
    FULL_MIN_MODE_NS,
    FULL_SAMPLES,
    SCREEN_MIN_MODE_NS,
    SCREEN_SAMPLES,
    MatrixCell,
    PairRow,
    SampleRow,
    ScreeningSelection,
    SummaryRow,
    _build_parser,
    benchmark_command,
    classify_full,
    full_matrix,
    parse_output,
    quick_matrix,
    screen_candidate_bound,
    screening_matrix,
    select_screening,
    summarize,
    write_evidence,
)
from process_utils import run_capture


SAMPLE_ROW = (
    "SREM_PAIR version=1 workload=srem_http_pipeline "
    "candidate=adaptive_srem baseline=waker_frame instances=4096 "
    "frame_bytes=128 tile_width=16 active_lanes=8 sites=1 "
    "divergence_eighths=0 threshold=8 producers=0 "
    "seed=6043432235128363791 min_mode_ns=250000000 "
    "warmup_rounds=7 rounds_per_block=128 ops_per_mode=524288 "
    "baseline_wall_ns=750000000 candidate_wall_ns=500000000 "
    "baseline_cpu_ns=700000000 candidate_cpu_ns=490000000 "
    "wall_speedup=1.500000000000 cpu_ratio=0.700000000000 "
    "baseline_checksum=0123456789abcdef "
    "candidate_checksum=0123456789abcdef "
    "baseline_completions=524288 candidate_completions=524288 "
    "baseline_queue_pushes=524288 candidate_queue_pushes=65536 "
    "baseline_queue_pops=524288 candidate_queue_pops=65536 "
    "candidate_tile_dispatches=65536 candidate_scalar_lanes=0 "
    "candidate_vector_lanes=524288 candidate_vector_blocks=65536 "
    "baseline_remote_pushes=0 candidate_remote_pushes=0 "
    "baseline_fair_samples=0 candidate_fair_samples=0 "
    "baseline_fair_p99_gap=0 candidate_fair_p99_gap=0 "
    "candidate_forced_escapes=0 hot_allocations=0 order=ABBA "
    "affinity=none clock=monotonic+process_cpu compiler=clang-21.0.0 "
    "host=os=darwin_active_processors=10"
)


def expect_rejected(text: str) -> None:
    try:
        parse_output(text)
    except ValueError:
        return
    raise AssertionError(f"expected parser rejection: {text!r}")


def test_exact_parser_contract() -> None:
    row = parse_output(SAMPLE_ROW)
    assert isinstance(row, PairRow)
    assert row.workload == "srem_http_pipeline"
    assert row.candidate == "adaptive_srem"
    assert row.baseline == "waker_frame"
    assert row.ops_per_mode == 524_288
    assert row.wall_speedup == 1.5
    assert row.cpu_ratio == 0.7
    assert row.baseline_checksum == row.candidate_checksum
    assert row.host == "os=darwin_active_processors=10"


def test_parser_rejections() -> None:
    bad_rows = (
        "",
        "noise\n" + SAMPLE_ROW,
        SAMPLE_ROW + "\n" + SAMPLE_ROW,
        SAMPLE_ROW.replace("version=1", "version=2"),
        SAMPLE_ROW.replace("wall_speedup=1.500000000000", "wall_speedup=nan"),
        SAMPLE_ROW.replace("candidate_wall_ns=500000000", "candidate_wall_ns=499000000"),
        SAMPLE_ROW.replace("ops_per_mode=524288", "ops_per_mode=524287"),
        SAMPLE_ROW.replace(
            "candidate_checksum=0123456789abcdef",
            "candidate_checksum=fedcba9876543210",
        ),
        SAMPLE_ROW.replace("candidate_completions=524288", "candidate_completions=1"),
        SAMPLE_ROW.replace("candidate_queue_pops=65536", "candidate_queue_pops=2"),
        SAMPLE_ROW.replace("hot_allocations=0", "hot_allocations=1"),
        SAMPLE_ROW.replace("order=ABBA", "order=AABB"),
        SAMPLE_ROW.replace("candidate=adaptive_srem", "candidate=remote_adaptive_srem"),
        SAMPLE_ROW.replace("min_mode_ns=250000000", "min_mode_ns=999999"),
        SAMPLE_ROW + " extra=1",
    )
    for row in bad_rows:
        expect_rejected(row)

    fairness = SAMPLE_ROW.replace(
        "workload=srem_http_pipeline", "workload=srem_mixed_fairness"
    )
    expect_rejected(fairness)


def _summary(
    cell: MatrixCell,
    *,
    wall: float = 1.0,
    cpu: float = 1.0,
    samples: int = FULL_SAMPLES,
    min_mode_ns: int = FULL_MIN_MODE_NS,
    spread: float = 1.02,
    fairness: float = 1.0,
) -> SummaryRow:
    return SummaryRow(
        workload=cell.workload,
        candidate=cell.candidate,
        baseline=cell.baseline,
        instances=4096,
        frame_bytes=cell.frame_bytes,
        tile_width=cell.tile_width,
        active_lanes=cell.active_lanes,
        sites=cell.sites,
        divergence_eighths=cell.divergence_eighths,
        threshold=cell.threshold,
        producers=cell.producers,
        seed=cell.seed,
        warmup_rounds=7,
        sample_count=samples,
        wall_speedup=wall,
        cpu_ratio=cpu,
        wall_best_speedup=wall * 1.01,
        cpu_best_ratio=cpu / 1.01,
        wall_ratio_spread=spread,
        cpu_ratio_spread=spread,
        baseline_wall_ns_per_op=100.0,
        candidate_wall_ns_per_op=100.0 / wall,
        baseline_cpu_ns_per_op=100.0,
        candidate_cpu_ns_per_op=100.0 * cpu,
        fairness_p99_ratio=fairness,
        checksum="0123456789abcdef",
        min_mode_ns=min_mode_ns,
    )


def test_matrices_are_deterministic() -> None:
    first = screening_matrix()
    second = screening_matrix()
    assert first == second
    assert len(first) == len(set(first))
    assert {
        cell.tile_width for cell in first
    } == {8, 16, 32}
    assert {
        cell.candidate for cell in first
    } == {"tile_scalar", "tile_vector", "adaptive_srem"}
    assert all(cell.seed != 0 for cell in first)

    gate = full_matrix(16, 8)
    assert gate == full_matrix(16, 8)
    assert len(gate) == len(set(gate))
    assert any(cell.candidate == "remote_adaptive_srem" for cell in gate)
    assert any(cell.workload == "srem_mixed_fairness" for cell in gate)
    assert quick_matrix(16, 8)


def test_summarize_keeps_process_local_pair() -> None:
    base = parse_output(SAMPLE_ROW)
    rows: list[SampleRow] = []
    for index, speedup in enumerate((1.40, 1.60, 1.50), start=1):
        baseline_wall = 600_000_000
        candidate_wall = round(baseline_wall / speedup)
        cpu_ratio = 0.60 + index / 100
        rows.append(
            SampleRow(
                process_sample=index,
                row=replace(
                    base,
                    baseline_wall_ns=baseline_wall,
                    candidate_wall_ns=candidate_wall,
                    wall_speedup=baseline_wall / candidate_wall,
                    baseline_cpu_ns=600_000_000,
                    candidate_cpu_ns=round(600_000_000 * cpu_ratio),
                    cpu_ratio=cpu_ratio,
                ),
            )
        )
    summary = summarize(rows)
    assert len(summary) == 1
    selected = summary[0]
    assert abs(selected.wall_speedup - 1.5) < 1e-6
    assert abs(selected.cpu_ratio - 0.63) < 1e-12
    assert selected.wall_best_speedup > selected.wall_speedup
    assert selected.cpu_best_ratio == 0.61
    assert selected.wall_ratio_spread > 1.14


def _screen_fixture() -> list[SummaryRow]:
    rows: list[SummaryRow] = []
    vector_dense = {8: 1.35, 16: 1.70, 32: 1.55}
    for cell in screening_matrix():
        wall = 1.0
        cpu = 1.0
        if cell.candidate == "tile_vector":
            wall = vector_dense[cell.tile_width]
            cpu = 0.68
        elif cell.candidate == "tile_scalar":
            wall = 1.05
            cpu = 0.95
        else:
            if cell.active_lanes == 1 and cell.threshold < 4:
                wall = 0.85
                cpu = 1.10
            elif cell.active_lanes == cell.tile_width:
                wall = vector_dense[cell.tile_width] * 0.98
                cpu = 0.69
            elif cell.tile_width == 16:
                wall = 1.62
                cpu = 0.69
            else:
                wall = 1.30
                cpu = 0.76
        rows.append(
            _summary(
                cell,
                wall=wall,
                cpu=cpu,
                samples=SCREEN_SAMPLES,
                min_mode_ns=SCREEN_MIN_MODE_NS,
            )
        )
    return rows


def test_screen_selection_and_candidate_bound() -> None:
    rows = _screen_fixture()
    selected = select_screening(rows)
    assert selected.width == 16
    assert selected.threshold == 4
    assert selected.wall_floor > 1.5
    assert selected.cpu_ceiling <= 0.70

    bound = screen_candidate_bound(rows, selected)
    assert bound.can_reach_category

    weakened = [
        replace(
            row,
            wall_best_speedup=1.20,
            cpu_best_ratio=0.90,
        )
        if (
            row.candidate == "adaptive_srem"
            and row.tile_width == selected.width
            and row.threshold == selected.threshold
            and row.workload == "srem_rpc_pipeline"
            and row.active_lanes in {8, 16}
        )
        else row
        for row in rows
    ]
    assert not screen_candidate_bound(
        weakened, selected
    ).can_reach_category


def _full_fixture(kind: str) -> list[SummaryRow]:
    rows: list[SummaryRow] = []
    for cell in full_matrix(16, 8):
        wall = 1.0
        cpu = 1.0
        fairness = 1.0
        if cell.workload in {
            "srem_http_pipeline",
            "srem_rpc_pipeline",
        } and cell.candidate == "adaptive_srem":
            is_core = (
                cell.active_lanes in {8, 16}
                and cell.divergence_eighths in {0, 1}
            )
            if is_core and kind == "category":
                wall, cpu = 1.60, 0.65
            elif is_core and kind == "specialized":
                if (
                    cell.active_lanes == 16
                    and cell.frame_bytes == 128
                    and cell.sites == 1
                    and cell.divergence_eighths == 0
                ):
                    wall, cpu = 1.60, 0.65
                else:
                    wall, cpu = 1.20, 0.90
            elif is_core:
                wall, cpu = 1.20, 0.90
            elif cell.active_lanes == 1:
                wall, cpu = 0.98, 1.02
            elif cell.divergence_eighths == 4:
                wall, cpu = 0.97, 1.03
        elif cell.candidate == "remote_adaptive_srem":
            wall, cpu = 0.95, 1.05
        elif cell.workload == "srem_mixed_fairness":
            wall, cpu, fairness = 0.98, 1.02, 1.05
        rows.append(_summary(cell, wall=wall, cpu=cpu, fairness=fairness))
    return rows


def test_formal_classifier() -> None:
    verdict, reasons, envelope = classify_full(
        _full_fixture("category"),
        16,
        8,
        vectorized=True,
        correctness=True,
    )
    assert verdict == "CATEGORY"
    assert not reasons
    assert envelope is None

    verdict, reasons, envelope = classify_full(
        _full_fixture("specialized"),
        16,
        8,
        vectorized=True,
        correctness=True,
    )
    assert verdict == "SPECIALIZED"
    assert envelope is not None
    assert "active=16" in envelope
    assert reasons

    verdict, reasons, envelope = classify_full(
        _full_fixture("reject"),
        16,
        8,
        vectorized=True,
        correctness=True,
    )
    assert verdict == "REJECT"
    assert reasons
    assert envelope is None

    category = _full_fixture("category")
    for broken in (
        category[:-1],
        [replace(category[0], sample_count=8), *category[1:]],
        [replace(category[0], wall_ratio_spread=1.11), *category[1:]],
        [replace(category[0], checksum="MISMATCH"), *category[1:]],
        [replace(category[0], min_mode_ns=1_000_000), *category[1:]],
    ):
        verdict, reasons, _ = classify_full(
            broken, 16, 8, vectorized=True, correctness=True
        )
        assert verdict == "INCONCLUSIVE"
        assert reasons
    for vectorized, correctness in ((False, True), (True, False)):
        verdict, reasons, _ = classify_full(
            category,
            16,
            8,
            vectorized=vectorized,
            correctness=correctness,
        )
        assert verdict == "INCONCLUSIVE"
        assert reasons


def test_command_binary_smoke_and_cli() -> None:
    cell = quick_matrix(16, 8)[0]
    relative_command = benchmark_command(
        Path("bench_srem_model"),
        cell,
        instances=4096,
        min_mode_ms=20,
        warmup_rounds=3,
        order="ABBA",
        owner_cpu="none",
    )
    assert Path(relative_command[0]).is_absolute()

    command = benchmark_command(
        Path("/tmp/bench_srem_model"),
        cell,
        instances=4096,
        min_mode_ms=20,
        warmup_rounds=3,
        order="ABBA",
        owner_cpu="none",
    )
    assert command[0] == str(Path("/tmp/bench_srem_model").resolve())
    assert command[command.index("--pair") + 1] == cell.candidate
    assert command[command.index("--order") + 1] == "abba"

    binary = os.environ.get("SREM_MODEL_TEST_BINARY")
    if binary:
        command[0] = binary
        command[command.index("--min-mode-ms") + 1] = "2"
        result = run_capture(command, timeout=30.0)
        assert result.returncode == 0, result.stderr
        row = parse_output(result.stdout)
        assert row.candidate == cell.candidate

    args = _build_parser().parse_args(
        [
            "--binary",
            "/tmp/bench_srem_model",
            "--phase",
            "screen",
            "--samples",
            "5",
            "--instances",
            "4096",
            "--min-mode-ms",
            "100",
            "--warmup-rounds",
            "7",
        ]
    )
    assert args.phase == "screen"
    assert args.samples == 5


def test_evidence_is_atomic_and_byte_identical() -> None:
    rows = _full_fixture("reject")
    selection = ScreeningSelection(
        width=16,
        threshold=8,
        wall_floor=1.2,
        cpu_ceiling=0.9,
    )
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        tracked = root / "tracked.md"
        write_evidence(
            root / "evidence",
            tracked,
            [],
            rows,
            phase="gate",
            verdict="REJECT",
            reasons=["core gate failed"],
            selection=selection,
            metadata={"command": "python3 scripts/bench_srem_model.py"},
        )
        raw = root / "evidence" / "srem_phase0_report.md"
        assert raw.read_bytes() == tracked.read_bytes()
        assert (root / "evidence" / "srem_phase0_samples.csv").is_file()
        assert (root / "evidence" / "srem_phase0_summary.csv").is_file()
        assert (root / "evidence" / "srem_phase0_metadata.json").is_file()
        assert "REJECT" in raw.read_text(encoding="utf-8")


def main() -> int:
    test_exact_parser_contract()
    test_parser_rejections()
    test_matrices_are_deterministic()
    test_summarize_keeps_process_local_pair()
    test_screen_selection_and_candidate_bound()
    test_formal_classifier()
    test_command_binary_smoke_and_cli()
    test_evidence_is_atomic_and_byte_identical()
    print("[test_bench_srem_model] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
