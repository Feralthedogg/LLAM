#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import json
import math
import os
import subprocess
from copy import deepcopy
from pathlib import Path

from bench_lccf_repr import (
    bootstrap_median_ci,
    classify_lines,
    collapse_process_medians,
    parse_raw_row,
)


REPRESENTATIONS = {
    "A": 0,
    "B": 48,
    "C": 64,
}
CONTRASTS = {
    "A/B": ("A", "B"),
    "A/C": ("A", "C"),
    "B/C": ("B", "C"),
}


def _routing(route: str, completions: int, callbacks: int) -> dict[str, int]:
    if route == "queue":
        return {
            "queue_pushes": callbacks,
            "queue_pops": callbacks,
            "direct_calls": 0,
            "forced_escapes": 0,
            "materializations": callbacks + completions,
        }
    if route == "fused":
        return {
            "queue_pushes": 0,
            "queue_pops": 0,
            "direct_calls": callbacks,
            "forced_escapes": 0,
            "materializations": callbacks,
        }
    forced = completions // 4
    queued_callbacks = forced * 8
    return {
        "queue_pushes": queued_callbacks,
        "queue_pops": queued_callbacks,
        "direct_calls": callbacks - queued_callbacks,
        "forced_escapes": forced,
        "materializations": callbacks + forced,
    }


def _side(
    representation: str,
    route: str,
    *,
    workload: str,
    rounds: int,
    wall_ns: int,
    cpu_ns: int,
    p99_ns: int,
) -> dict[str, object]:
    instances = 17
    chain = 8
    completions = rounds * instances
    callbacks = completions * chain
    attempts = completions * (3 if workload == "completion_timer_cancel" else 1)
    stale = attempts - completions
    routing = _routing(route, completions, callbacks)
    materializations = routing.pop("materializations")
    changed = callbacks - completions - 7
    if representation == "A":
        normalizations = materializations
        lookups = materializations
    elif representation == "B":
        normalizations = completions
        lookups = materializations
    else:
        normalizations = completions
        lookups = completions + changed
    return {
        "representation": representation,
        "wall_ns": wall_ns,
        "cpu_ns": cpu_ns,
        "p50_ns": p99_ns // 2,
        "p99_ns": p99_ns,
        "checksum": "0123456789abcdef",
        "operations": callbacks,
        "callbacks": callbacks,
        "completions": completions,
        "claims": completions,
        "stale_tickets": stale,
        **routing,
        "resume_calls": callbacks,
        "hot_allocations": 0,
        "facts_attempted": attempts,
        "facts_built": completions,
        "facts_build_failed": 0,
        "fact_normalizations": normalizations,
        "fact_site_lookups": lookups,
        "fact_changed_site_materializations": changed,
        "fact_module_pins": completions,
        "fact_payload_pins": completions,
        "fact_stale_losers": stale,
        "fact_guard_rechecks": materializations,
        "fact_queue_forwards": 0,
        "fact_generation_mismatches": 0,
        "fact_reuse_delays": 0,
        "fact_hot_bytes": 64,
        "fact_sidecar_bytes": REPRESENTATIONS[representation],
        "fact_overflow_pushes": 0,
        "fact_overflow_pops": 0,
        "refs_balanced": True,
        "queue_balanced": True,
        "overflow_balanced": True,
        "module_balanced": True,
        "payload_balanced": True,
        "backend_balanced": True,
        "tickets_balanced": True,
    }


def raw_row(
    contrast: str = "A/B",
    route: str = "queue",
    *,
    process_id: int = 0,
    block: int = 0,
    pair: int = 0,
    order: str = "ABBA",
    workload: str = "completion_io_pipeline",
    frame_bytes: int = 64,
    wall_speedup: float = 1.0,
    cpu_ratio: float = 0.9,
    p99_ratio: float = 1.0,
) -> dict[str, object]:
    left_representation, right_representation = CONTRASTS[contrast]
    rounds = 4
    left_wall = 1_000_000
    left_cpu = 1_000_000
    left_p99 = 100_000
    return {
        "schema_version": 1,
        "process_id": process_id,
        "cell": {
            "workload": workload,
            "route": route,
            "frame_bytes": frame_bytes,
            "instances": 17,
            "sites": 8,
            "chain": 8,
        },
        "contrast": contrast,
        "block": block,
        "pair": pair,
        "order": order,
        "minimum_window_ns": 1_000,
        "rounds": rounds,
        "seed": 0x6C6363662D726570 + process_id,
        "left": _side(
            left_representation,
            route,
            workload=workload,
            rounds=rounds,
            wall_ns=left_wall,
            cpu_ns=left_cpu,
            p99_ns=left_p99,
        ),
        "right": _side(
            right_representation,
            route,
            workload=workload,
            rounds=rounds,
            wall_ns=round(left_wall / wall_speedup),
            cpu_ns=round(left_cpu * cpu_ratio),
            p99_ns=round(left_p99 * p99_ratio),
        ),
    }


def encoded(row: dict[str, object]) -> str:
    return json.dumps(row, allow_nan=False, separators=(",", ":"))


def process_rows(
    contrast: str,
    route: str,
    process_id: int,
    *,
    workload: str = "completion_io_pipeline",
    frame_bytes: int = 64,
    wall_speedup: float = 1.0,
    cpu_ratio: float = 0.9,
    p99_ratio: float = 1.0,
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for block in range(4):
        order = "ABBA" if block % 2 == 0 else "BAAB"
        for pair in range(2):
            rows.append(raw_row(
                contrast,
                route,
                process_id=process_id,
                block=block,
                pair=pair,
                order=order,
                workload=workload,
                frame_bytes=frame_bytes,
                wall_speedup=wall_speedup,
                cpu_ratio=cpu_ratio,
                p99_ratio=p99_ratio,
            ))
    return rows


def evidence_rows(
    *,
    ab_queue_walls: list[float] | None = None,
    ab_queue_wall: float = 1.0,
    ac_queue_wall: float = 1.0,
    bc_cpu: float = 0.99,
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    workloads = (
        "completion_io_pipeline",
        "completion_rpc_state",
        "completion_timer_cancel",
        "completion_mixed_fairness",
    )
    for process_id in range(5):
        for workload in workloads:
            for frame_bytes in (64, 256):
                for route in ("queue", "fused", "mixed"):
                    ab_wall = (
                        ab_queue_walls[process_id]
                        if route == "queue" and ab_queue_walls is not None
                        else ab_queue_wall if route == "queue" else 1.0
                    )
                    ac_wall = ac_queue_wall if route == "queue" else 1.0
                    rows.extend(process_rows(
                        "A/B",
                        route,
                        process_id,
                        workload=workload,
                        frame_bytes=frame_bytes,
                        wall_speedup=ab_wall,
                        cpu_ratio=0.90 if route == "mixed" else 1.0,
                        p99_ratio=1.0,
                    ))
                    rows.extend(process_rows(
                        "A/C",
                        route,
                        process_id,
                        workload=workload,
                        frame_bytes=frame_bytes,
                        wall_speedup=ac_wall,
                        cpu_ratio=0.90 if route == "mixed" else 1.0,
                        p99_ratio=1.0,
                    ))
                    rows.extend(process_rows(
                        "B/C",
                        route,
                        process_id,
                        workload=workload,
                        frame_bytes=frame_bytes,
                        wall_speedup=1.0,
                        cpu_ratio=bc_cpu,
                        p99_ratio=1.0,
                    ))
    return rows


def test_strict_raw_parser() -> None:
    row = raw_row()
    parsed = parse_raw_row(encoded(row))
    assert parsed.left.representation == "A"
    assert parsed.right.representation == "B"

    duplicate = encoded(row).replace(
        '"schema_version":1',
        '"schema_version":1,"schema_version":1',
        1,
    )
    malformed = []
    malformed.append(duplicate)
    changed = deepcopy(row)
    changed["unknown"] = 1
    malformed.append(encoded(changed))
    changed = deepcopy(row)
    changed["left"]["wall_ns"] = 999  # type: ignore[index]
    malformed.append(encoded(changed))
    changed = deepcopy(row)
    changed["left"]["wall_ns"] = 10**400  # type: ignore[index]
    malformed.append(encoded(changed))
    changed = deepcopy(row)
    changed["right"]["checksum"] = "fedcba9876543210"  # type: ignore[index]
    malformed.append(encoded(changed))
    changed = deepcopy(row)
    changed["right"]["fact_sidecar_bytes"] = 49  # type: ignore[index]
    malformed.append(encoded(changed))
    changed = deepcopy(row)
    changed["right"]["fact_normalizations"] += 1  # type: ignore[index,operator]
    malformed.append(encoded(changed))
    changed = deepcopy(row)
    changed["right"]["refs_balanced"] = False  # type: ignore[index]
    malformed.append(encoded(changed))
    for value in malformed:
        try:
            parse_raw_row(value)
        except ValueError:
            pass
        else:
            raise AssertionError("accepted malformed representation row")


def test_process_median_and_order_balance() -> None:
    rows = process_rows("A/B", "queue", 0)
    parsed = [parse_raw_row(encoded(row)) for row in rows]
    medians = collapse_process_medians(parsed)
    assert len(medians) == 1
    assert math.isclose(medians[0].wall_speedup, 1.0)

    unbalanced = deepcopy(rows)
    for row in unbalanced:
        row["order"] = "ABBA"
    try:
        collapse_process_medians(
            [parse_raw_row(encoded(row)) for row in unbalanced]
        )
    except ValueError:
        pass
    else:
        raise AssertionError("accepted unbalanced ABBA/BAAB process")


def test_bootstrap_contract() -> None:
    first = bootstrap_median_ci(
        [0.9, 0.95, 1.0, 1.05, 1.1], seed=7, resamples=2_000
    )
    second = bootstrap_median_ci(
        [0.9, 0.95, 1.0, 1.05, 1.1], seed=7, resamples=2_000
    )
    assert first == second
    assert first[0] <= 1.0 <= first[1]
    try:
        bootstrap_median_ci([1.0, math.inf], seed=7, resamples=10)
    except ValueError:
        pass
    else:
        raise AssertionError("accepted a non-finite bootstrap ratio")


def classify(rows: list[dict[str, object]], *, resamples: int = 2_000):
    return classify_lines(
        [encoded(row) for row in rows],
        seed=0x72657072,
        resamples=resamples,
    )


def test_selection_fixtures() -> None:
    eligible = classify(evidence_rows(bc_cpu=0.95))
    assert eligible.b_eligibility == "ELIGIBLE"
    assert eligible.c_eligibility == "ELIGIBLE"

    prefer_b = classify(evidence_rows(bc_cpu=0.99))
    assert prefer_b.status == "SELECT_B"
    assert prefer_b.selected_representation == "B"

    only_c = classify(evidence_rows(ab_queue_wall=0.90, bc_cpu=0.95))
    assert only_c.b_eligibility == "INELIGIBLE"
    assert only_c.c_eligibility == "ELIGIBLE"
    assert only_c.status == "SELECT_C"

    only_b = classify(evidence_rows(ac_queue_wall=0.90, bc_cpu=0.95))
    assert only_b.b_eligibility == "ELIGIBLE"
    assert only_b.c_eligibility == "INELIGIBLE"
    assert only_b.status == "SELECT_B"

    retain_a = classify(evidence_rows(
        ab_queue_wall=0.90,
        ac_queue_wall=0.90,
    ))
    assert retain_a.status == "SELECT_A"
    assert retain_a.selected_representation == "A"

    prefer_c = classify(evidence_rows(bc_cpu=0.95))
    assert prefer_c.status == "SELECT_C"
    assert prefer_c.selected_representation == "C"

    crossing = classify(evidence_rows(
        ab_queue_walls=[0.96, 0.97, 0.99, 1.0, 1.01],
        ac_queue_wall=0.90,
    ))
    assert crossing.b_eligibility == "INCONCLUSIVE"
    assert crossing.c_eligibility == "INELIGIBLE"
    assert crossing.status == "INCONCLUSIVE"

    bad = evidence_rows()
    bad[0]["right"]["checksum"] = "fedcba9876543210"  # type: ignore[index]
    rejected = classify(bad)
    assert rejected.status == "REJECT"
    assert rejected.selected_representation is None


def test_binary_contract() -> None:
    binary_value = os.environ.get("LCCF_REPR_TEST_BINARY")
    if binary_value is None:
        return
    binary = Path(binary_value).resolve()
    command = [
        str(binary),
        "--workload", "completion_io_pipeline",
        "--route", "queue",
        "--process-id", "0",
        "--instances", "17",
        "--frame-bytes", "64",
        "--sites", "8",
        "--chain", "8",
        "--blocks", "2",
        "--minimum-window-ns", "1000000",
        "--warmup-rounds", "1",
        "--seed", "7810769610227647856",
    ]
    completed = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert completed.returncode == 0, completed.stderr
    assert completed.stderr == ""
    lines = completed.stdout.splitlines()
    assert len(lines) == 12
    rows = [parse_raw_row(line) for line in lines]
    assert {row.contrast for row in rows} == set(CONTRASTS)
    assert {row.order for row in rows} == {"ABBA", "BAAB"}
    for contrast in CONTRASTS:
        selected = [row for row in rows if row.contrast == contrast]
        assert len(selected) == 4
        assert {(row.block, row.pair) for row in selected} == {
            (0, 0), (0, 1), (1, 0), (1, 1)
        }
        assert len({row.rounds for row in selected}) == 1
        assert selected[0].rounds > 0

    invalid_commands = [
        command + ["--blocks", "2"],
        ["0" if value == "2" and command[index - 1] == "--blocks" else value
         for index, value in enumerate(command)],
        ["1" if value == "8" and command[index - 1] == "--sites" else value
         for index, value in enumerate(command)],
        ["0" if value == "1000000" else value for value in command],
        ["18446744073709551615"
         if value == "17" and command[index - 1] == "--instances" else value
         for index, value in enumerate(command)],
    ]
    for invalid in invalid_commands:
        rejected = subprocess.run(
            invalid,
            check=False,
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert rejected.returncode != 0
        assert rejected.stdout == ""


def test_binary_fairness_latency_is_not_logical_identity() -> None:
    binary_value = os.environ.get("LCCF_REPR_TEST_BINARY")
    if binary_value is None:
        return
    binary = str(Path(binary_value).resolve())
    cases = (
        ("fused", "64", "12723167542156389659"),
        ("mixed", "256", "9312776134767968772"),
    )
    for route, frame_bytes, seed in cases:
        command = [
            binary,
            "--workload", "completion_mixed_fairness",
            "--route", route,
            "--process-id", "0",
            "--instances", "257",
            "--frame-bytes", frame_bytes,
            "--sites", "8",
            "--chain", "8",
            "--blocks", "1",
            "--minimum-window-ns", "25000000",
            "--warmup-rounds", "4",
            "--seed", seed,
        ]
        completed = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=120,
        )
        assert completed.returncode == 0, completed.stderr
        assert completed.stderr == ""
        rows = [parse_raw_row(line)
                for line in completed.stdout.splitlines()]
        assert len(rows) == 6
        assert {row.contrast for row in rows} == set(CONTRASTS)


def main() -> int:
    test_strict_raw_parser()
    test_process_median_and_order_balance()
    test_bootstrap_contract()
    test_selection_fixtures()
    test_binary_contract()
    test_binary_fairness_latency_is_not_logical_identity()
    print("[test_bench_lccf_repr] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
