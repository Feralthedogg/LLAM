#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import os

from bench_lcwe_model import ModelRow, parse_output
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
        "64",
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
    assert row.instances == 64
    assert row.ops == 256

    invalid = command.copy()
    invalid[invalid.index("--lanes") + 1] = "3"
    result = run_capture(invalid, timeout=10.0)
    assert result.returncode != 0
    assert "[lcwe-model]" not in result.stdout
    assert "--lanes" in result.stderr


def main() -> int:
    test_exact_sample()
    test_missing_duplicate_and_extra_fields()
    test_invalid_numbers_and_checksum()
    test_result_row_cardinality()
    test_binary_smoke()
    print("[test_bench_lcwe_model] all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
