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
# See LICENSES/Apache-2.0.txt.

"""Regression tests for the CPU-spike comparison output contract."""

from __future__ import annotations

import contextlib
import io
import os
import tempfile
import sys
from pathlib import Path

import cpu_spike_compare as spike


LLAM_SAMPLE = """\
[cpu-spike] runtime=LLAM metric=timer workers=8 rounds=5 spike_ms=200 settle_ms=100 p50_us=190000.00 p99_us=201000.00 max_us=201000.00
[cpu-spike] runtime=LLAM metric=io workers=8 rounds=5 spike_ms=200 settle_ms=100 p50_us=191000.00 p99_us=202000.00 max_us=202000.00
[cpu-spike] runtime=LLAM metric=control workers=8 rounds=5 spike_ms=200 settle_ms=100 p50_us=192000.00 p99_us=203000.00 max_us=203000.00
"""

GO_SAMPLE = """\
[cpu-spike] runtime=Go metric=timer workers=8 rounds=5 spike_ms=200 settle_ms=100 p50_us=1000.00 p99_us=2000.00 max_us=2000.00
[cpu-spike] runtime=Go metric=io workers=8 rounds=5 spike_ms=200 settle_ms=100 p50_us=2000.00 p99_us=4000.00 max_us=4000.00
[cpu-spike] runtime=Go metric=control workers=8 rounds=5 spike_ms=200 settle_ms=100 p50_us=3000.00 p99_us=5000.00 max_us=5000.00
"""


def expect_rejected(output: str, runtime: str = "LLAM") -> None:
    try:
        spike.parse_output(runtime, output)
    except ValueError:
        return
    raise AssertionError(f"expected parser rejection: {output!r}")


def test_parse_output_accepts_exact_three_metric_contract() -> None:
    rows = spike.parse_output("LLAM", LLAM_SAMPLE)

    assert [row.metric for row in rows] == ["timer", "io", "control"]
    assert all(row.runtime == "LLAM" for row in rows)
    assert all(row.workers == 8 for row in rows)
    assert all(row.settle_ms == 100 for row in rows)
    assert rows[2].p99_us == 203000.0


def test_parse_output_rejects_missing_duplicate_and_wrong_runtime_rows() -> None:
    expect_rejected(LLAM_SAMPLE.rsplit("\n", 2)[0] + "\n")
    expect_rejected(LLAM_SAMPLE + LLAM_SAMPLE.splitlines()[0] + "\n")
    expect_rejected(LLAM_SAMPLE.replace("runtime=LLAM", "runtime=Go", 1))


def test_parse_output_rejects_ambiguous_or_nonfinite_fields() -> None:
    expect_rejected(LLAM_SAMPLE.replace("p50_us=190000.00", "p50_us=1 p50_us=2"))
    expect_rejected(LLAM_SAMPLE.replace("p99_us=201000.00", "p99_us=nan"))
    expect_rejected(LLAM_SAMPLE.replace("workers=8", "workers=0", 1))
    expect_rejected(LLAM_SAMPLE.replace("metric=timer", "metric=unknown", 1))


def test_parse_output_rejects_garbage_ordering_and_nonmonotonic_latency() -> None:
    expect_rejected(LLAM_SAMPLE.replace("[cpu-spike] runtime=LLAM", "[cpu-spike] garbage runtime=LLAM", 1))
    lines = LLAM_SAMPLE.splitlines()
    expect_rejected("\n".join([lines[1], lines[0], lines[2]]) + "\n")
    expect_rejected(LLAM_SAMPLE.replace("p50_us=190000.00", "p50_us=202000.00"))


def test_compare_rows_uses_go_as_latency_baseline() -> None:
    comparisons = spike.compare_rows(
        spike.parse_output("LLAM", LLAM_SAMPLE),
        spike.parse_output("Go", GO_SAMPLE),
    )

    assert [row.metric for row in comparisons] == ["timer", "io", "control"]
    assert comparisons[0].p50_ratio == 190.0
    assert comparisons[1].p99_ratio == 50.5
    assert comparisons[2].max_ratio == 40.6


def test_write_csv_records_raw_and_ratio_columns() -> None:
    llam_rows = spike.parse_output("LLAM", LLAM_SAMPLE)
    go_rows = spike.parse_output("Go", GO_SAMPLE)

    with tempfile.TemporaryDirectory(prefix="llam-cpu-spike-test-") as tmp:
        path = Path(tmp) / "cpu_spike_compare.csv"
        spike.write_csv(path, llam_rows, go_rows, spike.compare_rows(llam_rows, go_rows))
        lines = path.read_text(encoding="utf-8").strip().splitlines()

    assert lines[0] == (
        "metric,workers,rounds,spike_ms,settle_ms,llam_p50_us,llam_p99_us,llam_max_us,go_p50_us,go_p99_us,go_max_us,"
        "llam_over_go_p50,llam_over_go_p99,llam_over_go_max"
    )
    assert lines[1] == "timer,8,5,200,100,190000.00,201000.00,201000.00,1000.00,2000.00,2000.00,190.00,100.50,100.50"


def test_run_pair_passes_llam_worker_count_to_go_process() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-cpu-spike-test-") as tmp:
        root = Path(tmp)
        llam_script = root / "fake_llam.py"
        go_script = root / "fake_go.py"
        llam_script.write_text(f"print({LLAM_SAMPLE!r})\n", encoding="utf-8")
        go_script.write_text(
            "import os\n"
            "if os.environ.get('LLAM_CPU_SPIKE_WORKERS') != '8':\n"
            "    raise SystemExit(9)\n"
            f"print({GO_SAMPLE!r})\n",
            encoding="utf-8",
        )

        llam_rows, go_rows = spike.run_pair(
            root,
            [sys.executable, str(llam_script)],
            [sys.executable, str(go_script)],
            {},
            timeout=5.0,
        )

    assert llam_rows[0].workers == 8
    assert go_rows[0].workers == 8


def test_run_pair_rejects_stale_reported_configuration() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-cpu-spike-test-") as tmp:
        root = Path(tmp)
        llam_script = root / "fake_llam.py"
        go_script = root / "fake_go.py"
        llam_script.write_text(f"print({LLAM_SAMPLE!r})\n", encoding="utf-8")
        go_script.write_text(f"print({GO_SAMPLE!r})\n", encoding="utf-8")

        for overrides in (
            {"LLAM_CPU_SPIKE_ROUNDS": "6", "LLAM_CPU_SPIKE_MS": "200"},
            {
                "LLAM_CPU_SPIKE_ROUNDS": "5",
                "LLAM_CPU_SPIKE_MS": "200",
                "LLAM_CPU_SPIKE_SETTLE_MS": "99",
            },
        ):
            try:
                spike.run_pair(
                    root,
                    [sys.executable, str(llam_script)],
                    [sys.executable, str(go_script)],
                    overrides,
                    timeout=5.0,
                )
            except ValueError:
                continue
            raise AssertionError(f"run_pair accepted stale CPU-spike config: {overrides}")


def test_format_summary_keeps_latency_units_and_ratio_direction_explicit() -> None:
    llam_rows = spike.parse_output("LLAM", LLAM_SAMPLE)
    go_rows = spike.parse_output("Go", GO_SAMPLE)
    summary = spike.format_summary(llam_rows, go_rows, spike.compare_rows(llam_rows, go_rows))

    assert "metric   LLAM p50 us   Go p50 us   LLAM/Go p50" in summary
    assert "timer      190000.00      1000.00         190.00x" in summary
    assert "control    192000.00      3000.00          64.00x" in summary


def test_force_go_async_preemption_preserves_other_godebug_settings() -> None:
    assert spike.force_go_async_preemption("") == "asyncpreemptoff=0"
    assert spike.force_go_async_preemption("gctrace=1,asyncpreemptoff=1") == "gctrace=1,asyncpreemptoff=0"


def test_run_program_normalizes_launch_failure() -> None:
    try:
        spike._run_program(Path.cwd(), ["definitely-not-a-real-cpu-spike-command"], {}, 1.0)
    except RuntimeError as exc:
        assert "could not start" in str(exc)
        return
    raise AssertionError("launch failure escaped the CPU-spike command boundary")


def test_run_program_normalizes_timeout() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-cpu-spike-test-") as tmp:
        script = Path(tmp) / "slow.py"
        script.write_text("import time\ntime.sleep(10)\n", encoding="utf-8")
        try:
            spike._run_program(Path(tmp), [sys.executable, str(script)], {}, 0.01)
        except RuntimeError as exc:
            assert "timed out" in str(exc)
            return
    raise AssertionError("timeout escaped the CPU-spike command boundary")


def test_main_reports_missing_no_build_binary_without_traceback() -> None:
    if os.name == "nt":
        return
    original_argv = sys.argv
    stderr = io.StringIO()
    with tempfile.TemporaryDirectory(prefix="llam-cpu-spike-test-") as tmp:
        try:
            sys.argv = [
                "cpu_spike_compare.py",
                "--no-build",
                "--out-dir",
                tmp,
                "--llam-bin",
                str(Path(tmp) / "missing-llam"),
                "--go-bin",
                str(Path(tmp) / "missing-go"),
            ]
            with contextlib.redirect_stderr(stderr):
                rc = spike.main()
        finally:
            sys.argv = original_argv

    assert rc == 1
    assert "binary not found" in stderr.getvalue()
    assert "Traceback" not in stderr.getvalue()


def main() -> int:
    test_parse_output_accepts_exact_three_metric_contract()
    test_parse_output_rejects_missing_duplicate_and_wrong_runtime_rows()
    test_parse_output_rejects_ambiguous_or_nonfinite_fields()
    test_parse_output_rejects_garbage_ordering_and_nonmonotonic_latency()
    test_compare_rows_uses_go_as_latency_baseline()
    test_write_csv_records_raw_and_ratio_columns()
    test_run_pair_passes_llam_worker_count_to_go_process()
    test_run_pair_rejects_stale_reported_configuration()
    test_format_summary_keeps_latency_units_and_ratio_direction_explicit()
    test_force_go_async_preemption_preserves_other_godebug_settings()
    test_run_program_normalizes_launch_failure()
    test_run_program_normalizes_timeout()
    test_main_reports_missing_no_build_binary_without_traceback()
    print("[test_cpu_spike_compare] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
