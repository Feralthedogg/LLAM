# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import json
import sys
from dataclasses import replace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from bench_lccf_fact import (  # noqa: E402
    Cell,
    FactSample,
    PairResult,
    _aggregate_cell,
    classify_pair,
    parse_sample_output,
)


def sample(mode: str, **updates: object) -> FactSample:
    shared = "shared_fact" in mode
    queued = mode.endswith("queue")
    mixed = mode.startswith("mixed")
    completions = 68
    callbacks = 340
    forced = 17 if mixed else 0
    queue_pops = callbacks if queued else (85 if mixed else 0)
    direct_calls = callbacks - queue_pops
    work = completions if shared else callbacks + (completions if queued else forced)
    site_work = callbacks if shared else work
    value = FactSample(
        version=2,
        workload="completion_io_pipeline",
        mode=mode,
        instances=17,
        frame_bytes=128,
        cell_bytes=64,
        sites=8,
        chain=5,
        rounds=4,
        operations=callbacks,
        callbacks=callbacks,
        wall_ns=1_000_000,
        cpu_ns=900_000,
        p50_ns=240_000,
        p99_ns=260_000,
        instructions_supported=False,
        instructions=0,
        checksum="0123456789abcdef",
        completions=completions,
        claims=completions,
        stale_tickets=0,
        queue_pushes=queue_pops,
        queue_pops=queue_pops,
        resume_calls=callbacks,
        direct_calls=direct_calls,
        forced_escapes=forced,
        hot_allocations=0,
        facts_attempted=completions,
        facts_built=completions,
        facts_build_failed=0,
        fact_normalizations=work,
        fact_site_lookups=site_work,
        fact_module_pins=completions,
        fact_payload_pins=completions,
        fact_stale_losers=0,
        fact_guard_rechecks=callbacks + (completions if queued else forced),
        fact_queue_forwards=0,
        fact_generation_mismatches=0,
        fact_reuse_delays=0,
        fact_hot_bytes=64,
        fact_sidecar_bytes=64,
        fact_overflow_pushes=0,
        fact_overflow_pops=0,
        refs_balanced=True,
    )
    return replace(value, **updates)


def encoded(value: FactSample) -> str:
    return "LCCF_FACT_SAMPLE " + json.dumps(value.as_dict(), sort_keys=True)


def test_strict_parser() -> None:
    value = sample("shared_fact_queue")
    assert parse_sample_output(encoded(value)) == value

    bad_rows = [
        "noise\n" + encoded(value),
        encoded(value) + "\nnoise",
        "LCCF_FACT_SAMPLE {}",
        encoded(value).replace('"version": 2', '"version": 1'),
        encoded(value).replace('"rounds": 4', '"rounds": true'),
        encoded(value).replace('"checksum": "0123456789abcdef"',
                               '"checksum": "xyz"'),
        encoded(value).replace('"mode": "shared_fact_queue"',
                               '"mode": "unknown"'),
    ]
    duplicate = encoded(value).replace(
        '"version": 2', '"version": 2, "version": 2', 1
    )
    bad_rows.append(duplicate)
    for row in bad_rows:
        try:
            parse_sample_output(row)
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted malformed row: {row}")


def test_queue_classification() -> None:
    baseline = sample("recompute_queue")
    candidate = sample(
        "shared_fact_queue",
        wall_ns=1_015_000,
        cpu_ns=890_000,
        p99_ns=270_000,
    )
    result = classify_pair(baseline, candidate)
    assert result.status == "PASS"
    assert result.wall_speedup > 0.98
    assert result.p99_ratio < 1.05


def test_fused_and_mixed_gates() -> None:
    fused = classify_pair(
        sample("recompute_fused"),
        sample("shared_fact_fused", wall_ns=1_020_000, p99_ns=270_000),
    )
    assert fused.status == "PASS"

    mixed = classify_pair(
        sample("mixed_recompute"),
        sample("mixed_shared_fact", cpu_ns=850_000, p99_ns=270_000),
    )
    assert mixed.status == "PASS"
    slow_mixed = classify_pair(
        sample("mixed_recompute"),
        sample("mixed_shared_fact", cpu_ns=880_000, p99_ns=270_000),
    )
    assert slow_mixed.status == "FAIL_PERFORMANCE"
    assert any("mixed CPU/instruction improvement" in reason
               for reason in slow_mixed.reasons)


def test_correctness_precedes_performance() -> None:
    baseline = sample("recompute_queue")
    wrong_checksum = sample(
        "shared_fact_queue",
        checksum="fedcba9876543210",
        wall_ns=500_000,
    )
    result = classify_pair(baseline, wrong_checksum)
    assert result.status == "FAIL_CORRECTNESS"
    assert any("checksum" in reason for reason in result.reasons)

    repeated_work = sample(
        "shared_fact_queue",
        fact_normalizations=69,
    )
    result = classify_pair(baseline, repeated_work)
    assert result.status == "FAIL_CORRECTNESS"
    assert any("fact/site work" in reason for reason in result.reasons)

    missing_baseline_work = sample(
        "recompute_queue",
        fact_normalizations=68,
        fact_site_lookups=68,
    )
    result = classify_pair(
        missing_baseline_work, sample("shared_fact_queue")
    )
    assert result.status == "FAIL_CORRECTNESS"
    assert any("baseline recompute" in reason for reason in result.reasons)


def test_spread_is_inconclusive() -> None:
    cell = Cell(
        workload="completion_io_pipeline",
        candidate="shared_fact_fused",
        baseline="recompute_fused",
        frame_bytes=128,
        cell_bytes=64,
        sites=8,
    )
    stable = PairResult("PASS", (), 1.1, 0.8, 0.9, None)
    unstable = PairResult("PASS", (), 1.3, 0.8, 0.9, None)
    result = _aggregate_cell(cell, [stable, unstable])
    assert result["status"] == "INCONCLUSIVE"
    assert result["wall_ratio_spread"] > 1.10


def main() -> int:
    test_strict_parser()
    test_queue_classification()
    test_fused_and_mixed_gates()
    test_correctness_precedes_performance()
    test_spread_is_inconclusive()
    print("[test_bench_lccf_fact] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
