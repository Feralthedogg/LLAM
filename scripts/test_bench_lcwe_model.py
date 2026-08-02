#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/OLD-LICENSE/Apache-2.0.txt.

from __future__ import annotations

import os
import tempfile
from dataclasses import replace
from pathlib import Path

from bench_lcwe_model import (
    ModelRow,
    SummaryRow,
    classify,
    parse_output,
    select_median,
    write_markdown,
)
from process_utils import run_capture


SAMPLE_ROW = (
    "[lcwe-model] workload=exec_io_pipeline mode=wave_capsule "
    "instances=65536 sites=1 lanes=8 rounds=31 warmup=5 ops=1703936 "
    "wall_ns=123456789 cpu_ns=120000000 wall_ns_per_op=72.45 "
    "cpu_ns_per_op=70.42 p50_ns_per_op=71.90 p99_ns_per_op=78.10 "
    "checksum=0123456789abcdef"
)


def expect_rejected(text: str) -> None:
    try:
        parse_output(text)
    except ValueError:
        return
    raise AssertionError(f"expected parser rejection: {text!r}")


def test_exact_sample() -> None:
    row = parse_output(SAMPLE_ROW)
    expected = ModelRow(
        workload="exec_io_pipeline",
        mode="wave_capsule",
        instances=65536,
        sites=1,
        lanes=8,
        rounds=31,
        warmup=5,
        ops=1703936,
        wall_ns=123456789,
        cpu_ns=120000000,
        wall_ns_per_op=72.45,
        cpu_ns_per_op=70.42,
        p50_ns_per_op=71.90,
        p99_ns_per_op=78.10,
        checksum="0123456789abcdef",
    )
    assert row == expected


def test_missing_duplicate_and_extra_fields() -> None:
    expect_rejected(SAMPLE_ROW.replace(" cpu_ns=120000000", ""))
    expect_rejected(
        SAMPLE_ROW.replace(
            " workload=exec_io_pipeline",
            " workload=exec_io_pipeline workload=exec_rpc_state",
        )
    )
    expect_rejected(f"{SAMPLE_ROW} extra=1")


def test_invalid_numbers_and_checksum() -> None:
    expect_rejected(SAMPLE_ROW.replace("wall_ns_per_op=72.45", "wall_ns_per_op=nan"))
    expect_rejected(SAMPLE_ROW.replace("cpu_ns_per_op=70.42", "cpu_ns_per_op=inf"))
    expect_rejected(SAMPLE_ROW.replace("ops=1703936", "ops=0"))
    expect_rejected(SAMPLE_ROW.replace("wall_ns=123456789", "wall_ns=-1"))
    expect_rejected(SAMPLE_ROW.replace("checksum=0123456789abcdef", "checksum=xyz"))
    expect_rejected(SAMPLE_ROW.replace("checksum=0123456789abcdef", "checksum=ABCDEF0123456789"))


def test_result_row_cardinality() -> None:
    expect_rejected("")
    expect_rejected(f"{SAMPLE_ROW}\n{SAMPLE_ROW}")
    expect_rejected(f"noise on stdout\n{SAMPLE_ROW}")


def test_binary_smoke() -> None:
    binary = os.environ.get("LCWE_MODEL_TEST_BINARY")
    if not binary:
        return
    command = [
        binary,
        "--workload",
        "exec_io_pipeline",
        "--mode",
        "scalar",
        "--instances",
        "4096",
        "--sites",
        "1",
        "--lanes",
        "1",
        "--rounds",
        "5",
        "--warmup",
        "1",
        "--seed",
        "7",
    ]
    result = run_capture(command, timeout=10.0)
    assert result.returncode == 0, result.stderr
    row = parse_output(result.stdout)
    assert row.workload == "exec_io_pipeline"
    assert row.mode == "scalar"
    assert row.instances == 4096
    assert row.ops == 16384

    invalid = command.copy()
    invalid[invalid.index("--lanes") + 1] = "3"
    result = run_capture(invalid, timeout=10.0)
    assert result.returncode != 0
    assert "[lcwe-model]" not in result.stdout
    assert "--lanes" in result.stderr


def summary_row(
    workload: str,
    mode: str,
    wall: float,
    cpu: float,
    *,
    spread: float = 1.01,
    checksum: str = "0123456789abcdef",
) -> SummaryRow:
    return SummaryRow(
        workload=workload,
        mode=mode,
        sites=1,
        lanes=1 if mode in {"scalar", "cohort"} else 8,
        sample_count=7,
        wall_ns_per_op=wall,
        cpu_ns_per_op=cpu,
        p50_ns_per_op=wall,
        p99_ns_per_op=wall * 1.05,
        spread=spread,
        checksum=checksum,
    )


def verdict_fixture(
    *,
    realistic_passes: int,
    aosoa_passes: int,
    cohort_speedup: float,
) -> list[SummaryRow]:
    workloads = [
        "exec_io_pipeline",
        "exec_rpc_state",
        "exec_event_fanout",
    ]
    rows: list[SummaryRow] = []
    for index, workload in enumerate(workloads):
        rows.append(summary_row(workload, "scalar", 100.0, 100.0))
        rows.append(
            summary_row(
                workload,
                "cohort",
                100.0 / cohort_speedup if index == 0 else 100.0,
                100.0,
            )
        )
        realistic_wall = 62.5 if index < realistic_passes else 100.0
        realistic_cpu = 65.0 if index < realistic_passes else 100.0
        rows.append(
            summary_row(
                workload,
                "wave_pointers",
                realistic_wall,
                realistic_cpu,
            )
        )
        rows.append(
            summary_row(
                workload,
                "wave_capsule",
                realistic_wall + 1.0,
                realistic_cpu,
            )
        )
        rows.append(
            summary_row(
                workload,
                "wave_aosoa",
                62.5 if index < aosoa_passes else 100.0,
                65.0 if index < aosoa_passes else 100.0,
            )
        )
    return rows


def test_verdicts() -> None:
    verdict, reasons = classify(
        verdict_fixture(
            realistic_passes=2,
            aosoa_passes=2,
            cohort_speedup=1.08,
        )
    )
    assert verdict == "PROMISING"
    assert reasons

    verdict, _ = classify(
        verdict_fixture(
            realistic_passes=0,
            aosoa_passes=2,
            cohort_speedup=1.08,
        )
    )
    assert verdict == "LAYOUT_BLOCKED"

    verdict, _ = classify(
        verdict_fixture(
            realistic_passes=0,
            aosoa_passes=0,
            cohort_speedup=1.08,
        )
    )
    assert verdict == "REJECT"

    verdict, reasons = classify(
        verdict_fixture(
            realistic_passes=2,
            aosoa_passes=2,
            cohort_speedup=1.0,
        )
    )
    assert verdict == "REJECT"
    assert any("cohort" in reason.lower() for reason in reasons)


def test_inconclusive_integrity_gates() -> None:
    rows = verdict_fixture(
        realistic_passes=2,
        aosoa_passes=2,
        cohort_speedup=1.08,
    )
    unstable = rows.copy()
    unstable[0] = replace(unstable[0], spread=1.1501)
    assert classify(unstable)[0] == "INCONCLUSIVE"

    missing = [row for row in rows if not (
        row.workload == "exec_io_pipeline" and row.mode == "scalar"
    )]
    assert classify(missing)[0] == "INCONCLUSIVE"

    mismatch = rows.copy()
    mismatch[3] = replace(mismatch[3], checksum="fedcba9876543210")
    assert classify(mismatch)[0] == "INCONCLUSIVE"


def test_median_keeps_one_process_row() -> None:
    base = parse_output(SAMPLE_ROW)
    fastest = replace(
        base,
        wall_ns_per_op=1.0,
        cpu_ns_per_op=101.0,
        p50_ns_per_op=1.0,
        p99_ns_per_op=1.1,
    )
    median = replace(
        base,
        wall_ns_per_op=10.0,
        cpu_ns_per_op=202.0,
        p50_ns_per_op=9.0,
        p99_ns_per_op=11.0,
    )
    slowest = replace(
        base,
        wall_ns_per_op=11.0,
        cpu_ns_per_op=303.0,
        p50_ns_per_op=10.0,
        p99_ns_per_op=12.0,
    )
    selected = select_median([slowest, fastest, median])
    assert selected.wall_ns_per_op == 10.0
    assert selected.cpu_ns_per_op == 202.0
    assert selected.p50_ns_per_op == 9.0
    assert selected.p99_ns_per_op == 11.0
    assert selected.spread == 11.0

    checksum_mismatch = select_median(
        [fastest, replace(median, checksum="fedcba9876543210"), slowest]
    )
    assert checksum_mismatch.checksum == "MISMATCH"


def test_markdown_contract() -> None:
    rows = verdict_fixture(
        realistic_passes=2,
        aosoa_passes=2,
        cohort_speedup=1.08,
    )
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "report.md"
        write_markdown(
            path,
            rows,
            "PROMISING",
            ["two realistic workloads pass"],
            {
                "command": "python3 scripts/bench_lcwe_model.py --quick",
                "host": "test-host",
            },
        )
        text = path.read_text(encoding="utf-8")
    assert "# LCWE Phase 1 Cost Model" in text
    assert "PROMISING" in text
    assert "realistic" in text
    assert "upper-bound only" in text
    assert "python3 scripts/bench_lcwe_model.py --quick" in text
    assert "Phase 1 is not production validation" in text


def main() -> int:
    test_exact_sample()
    test_missing_duplicate_and_extra_fields()
    test_invalid_numbers_and_checksum()
    test_result_row_cardinality()
    test_binary_smoke()
    test_verdicts()
    test_inconclusive_integrity_gates()
    test_median_keeps_one_process_row()
    test_markdown_contract()
    print("[test_bench_lcwe_model] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
