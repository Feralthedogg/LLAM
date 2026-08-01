#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import os
import tempfile
from dataclasses import replace
from pathlib import Path

from bench_lcrs_model import (
    ModelRow,
    classify,
    parse_output,
    select_median,
    write_markdown,
)
from process_utils import run_capture


SAMPLE_ROW = (
    "LCRS_SAMPLE version=1 workload=balanced policy=global_deepest "
    "shards=32 nodes=2 iterations=1000 thief_attempts=16000 "
    "selections=15000 probes=496000 candidate_tries=15000 "
    "fullscan_fallbacks=16000 remote_selections=7000 "
    "productive_units=120000 p50_fanin=8 p99_fanin=16 max_fanin=16 "
    "wall_ns=123456789 cpu_ns=120000000 "
    "input_checksum=0123456789abcdef decision_checksum=fedcba9876543210"
)


def expect_rejected(text: str) -> None:
    try:
        parse_output(text)
    except ValueError:
        return
    raise AssertionError(f"expected parser rejection: {text!r}")


def test_exact_sample() -> None:
    row = parse_output(SAMPLE_ROW)
    assert row == ModelRow(
        version=1,
        workload="balanced",
        policy="global_deepest",
        shards=32,
        nodes=2,
        iterations=1000,
        thief_attempts=16000,
        selections=15000,
        probes=496000,
        candidate_tries=15000,
        fullscan_fallbacks=16000,
        remote_selections=7000,
        productive_units=120000,
        p50_fanin=8,
        p99_fanin=16,
        max_fanin=16,
        wall_ns=123456789,
        cpu_ns=120000000,
        input_checksum="0123456789abcdef",
        decision_checksum="fedcba9876543210",
    )


def test_strict_schema() -> None:
    expect_rejected("")
    expect_rejected(f"{SAMPLE_ROW}\n{SAMPLE_ROW}")
    expect_rejected(f"noise\n{SAMPLE_ROW}")
    expect_rejected(SAMPLE_ROW.replace(" probes=496000", ""))
    expect_rejected(SAMPLE_ROW.replace(" version=1", " version=1 version=1"))
    expect_rejected(f"{SAMPLE_ROW} extra=1")
    expect_rejected(SAMPLE_ROW.replace("version=1", "version=2"))
    expect_rejected(SAMPLE_ROW.replace("workload=balanced", "workload=unknown"))
    expect_rejected(SAMPLE_ROW.replace("policy=global_deepest", "policy=unknown"))
    expect_rejected(SAMPLE_ROW.replace("shards=32", "shards=0"))
    expect_rejected(SAMPLE_ROW.replace("selections=15000", "selections=16001"))
    expect_rejected(SAMPLE_ROW.replace("p99_fanin=16", "p99_fanin=33"))
    expect_rejected(
        SAMPLE_ROW.replace("input_checksum=0123456789abcdef", "input_checksum=XYZ")
    )


def _row(workload: str, policy: str) -> ModelRow:
    return replace(
        parse_output(SAMPLE_ROW),
        workload=workload,
        policy=policy,
        probes=496000,
        p99_fanin=16,
        max_fanin=16,
        productive_units=120000,
        remote_selections=7000,
        fullscan_fallbacks=0,
        input_checksum="0123456789abcdef",
    )


def passing_rows() -> list[ModelRow]:
    rows: list[ModelRow] = []
    for workload in (
        "balanced",
        "one_hot",
        "rotating_hotspot",
        "drain_tail",
        "uneven_numa",
        "offline_churn",
    ):
        baseline = _row(workload, "global_deepest")
        random = replace(
            baseline,
            policy="deterministic_random_k",
            probes=128000,
            p99_fanin=8,
            max_fanin=10,
            productive_units=121000,
            remote_selections=7000,
        )
        coded = replace(
            baseline,
            policy="coded_rotation_k",
            probes=112000,
            p99_fanin=6,
            max_fanin=8,
            productive_units=124000,
            remote_selections=7000,
        )
        coded_full = replace(
            coded,
            policy="coded_rotation_k_plus_fullscan",
            probes=120000,
            productive_units=123000,
            fullscan_fallbacks=800,
        )
        rows.extend((baseline, random, coded, coded_full))
    return rows


def test_classification_gates() -> None:
    verdict, reasons = classify(passing_rows())
    assert verdict == "PASS", reasons

    probe_failure = [
        replace(row, probes=300000)
        if row.policy == "coded_rotation_k_plus_fullscan"
        else row
        for row in passing_rows()
    ]
    verdict, reasons = classify(probe_failure)
    assert verdict == "REJECT"
    assert any("probe" in reason for reason in reasons)

    complexity_failure = [
        replace(row, p99_fanin=8, productive_units=121000)
        if row.policy in {"coded_rotation_k", "coded_rotation_k_plus_fullscan"}
        else row
        for row in passing_rows()
    ]
    verdict, reasons = classify(complexity_failure)
    assert verdict == "NARROW"
    assert any("random-k" in reason for reason in reasons)

    checksum_failure = [
        replace(row, input_checksum="1111111111111111")
        if row.workload == "balanced" and row.policy == "coded_rotation_k"
        else row
        for row in passing_rows()
    ]
    verdict, reasons = classify(checksum_failure)
    assert verdict == "REJECT"
    assert any("checksum" in reason for reason in reasons)

    sparse_tail = [
        replace(row, probes=208000, p99_fanin=14)
        if row.workload == "drain_tail"
        and row.policy == "coded_rotation_k_plus_fullscan"
        else replace(row, probes=208000, p99_fanin=14)
        if row.workload == "drain_tail" and row.policy == "coded_rotation_k"
        else row
        for row in passing_rows()
    ]
    verdict, reasons = classify(sparse_tail)
    assert verdict == "PASS", reasons


def test_median_is_one_real_process() -> None:
    base = parse_output(SAMPLE_ROW)
    selected = select_median(
        [
            replace(base, wall_ns=300, cpu_ns=301, probes=3),
            replace(base, wall_ns=100, cpu_ns=101, probes=1),
            replace(base, wall_ns=200, cpu_ns=201, probes=2),
        ]
    )
    assert selected.wall_ns == 200
    assert selected.cpu_ns == 201
    assert selected.probes == 2


def test_markdown_contract() -> None:
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "report.md"
        write_markdown(
            path,
            passing_rows(),
            "PASS",
            ["all model gates passed"],
            {"command": "python3 scripts/bench_lcrs_model.py --quick", "host": "test"},
        )
        text = path.read_text(encoding="utf-8")
    assert "# LCRS Phase 0" in text
    assert "PASS" in text
    assert "random-k" in text
    assert "standalone model" in text


def test_binary_smoke() -> None:
    binary = os.environ.get("LCRS_MODEL_TEST_BINARY")
    if not binary:
        return
    result = run_capture(
        [
            binary,
            "--workload",
            "balanced",
            "--policy",
            "coded_rotation_k",
            "--shards",
            "32",
            "--nodes",
            "2",
            "--iterations",
            "8",
            "--seed",
            "7",
        ],
        timeout=10.0,
    )
    assert result.returncode == 0, result.stderr
    row = parse_output(result.stdout.rstrip("\n"))
    assert row.workload == "balanced"
    assert row.policy == "coded_rotation_k"
    assert row.shards == 32
    assert row.nodes == 2


def main() -> int:
    test_exact_sample()
    test_strict_schema()
    test_classification_gates()
    test_median_is_one_real_process()
    test_markdown_contract()
    test_binary_smoke()
    print("[test_bench_lcrs_model] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
