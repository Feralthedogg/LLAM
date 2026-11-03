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
# See LICENSE.
"""Record matched managed CPU/yield/I/O/timer costs and native landing samples."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
NOTICE = 'SPDX-FileCopyrightText: 2026 Feralthedogg\nSPDX-License-Identifier: Apache-2.0\nLicensed under the Apache License, Version 2.0 (the "License");\nyou may not use this file except in compliance with the License.\nYou may obtain a copy of the License at\n\n    https://www.apache.org/licenses/LICENSE-2.0\n\nUnless required by applicable law or agreed to in writing, software\ndistributed under the License is distributed on an "AS IS" BASIS,\nWITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\nSee the License for the specific language governing permissions and\nlimitations under the License.\nSee LICENSE.\n'


INPUT_TREES = ("src", "include", "tools/preempt", "experiments/async_compare", "tests")
INPUT_SUFFIXES = {".c", ".h", ".cpp", ".S", ".s", ".asm", ".inc", ".py", ".def"}


def source_snapshot(root: Path) -> dict[str, str]:
    paths = {root / "CMakeLists.txt", root / "Makefile"}
    for directory in INPUT_TREES:
        for path in (root / directory).rglob("*"):
            if path.is_symlink():
                raise ValueError(f"benchmark input scope must not contain symbolic links: {path}")
            if path.suffix not in INPUT_SUFFIXES:
                continue
            if path.is_file():
                paths.add(path)
    if any(path.is_symlink() for path in paths):
        raise ValueError("benchmark build inputs must not be symbolic links")
    return {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(paths)}


def snapshot_changes(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return sorted(name for name in before.keys() | after.keys() if before.get(name) != after.get(name))


def percentile(values: list[int], fraction: float) -> int | None:
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)] if values else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--cases", nargs="+", choices=("cpu", "yield", "timer", "timer-hog", "io", "io-hog"),
                        default=["cpu", "yield", "timer", "timer-hog", "io", "io-hog"])
    parser.add_argument("--iterations", type=int, default=20000000)
    parser.add_argument("--quantum-ns", type=int, default=0,
                        help="0 measures the managed constructor's native default")
    options = parser.parse_args()
    if not 3 <= options.samples <= 100 or not 1 <= options.iterations <= 1000000000:
        parser.error("samples must be 3..100 and iterations 1..1000000000")
    if not 0 <= options.quantum_ns <= 1000000000:
        parser.error("quantum must be 0..1000000000 ns")
    if len(options.cases) != len(set(options.cases)):
        parser.error("cases must be unique")
    processes = subprocess.check_output(["ps", "-ax", "-o", "command="], text=True)
    if any(line.lstrip().startswith(("qemu-system", "/opt/homebrew/bin/qemu-system"))
           for line in processes.splitlines()):
        parser.error("stop the task's VM before collecting isolated host measurements")
    binary_source, image_source = options.binary.resolve(strict=True), options.image.resolve(strict=True)
    options.output.mkdir(parents=True, exist_ok=False)
    # Keep one exact pair for all samples even if another build replaces its
    # outputs. These two artifacts are also the reproducible measurement input.
    binary, image = options.output.resolve() / binary_source.name, options.output.resolve() / image_source.name
    shutil.copy2(binary_source, binary)
    shutil.copy2(image_source, image)
    report = {
        "notice": NOTICE, "platform": platform.platform(), "machine": platform.machine(),
        "started_at_unix": time.time(), "load_average": os.getloadavg(),
        "samples_per_case_policy": options.samples, "warmups_per_case_policy": 1,
        "cases": options.cases, "artifact_sources": [str(binary_source), str(image_source)],
        "instrumentation": "production runtime objects; test hooks disabled; bounded native events with normal-M arrival clock",
        "hashes": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (binary, image)},
        "source_hashes": source_snapshot(ROOT),
        "source_input_scope": ["CMakeLists.txt", "Makefile", *INPUT_TREES],
        "raw": [], "summary": {},
    }
    report["source_input_count"] = len(report["source_hashes"])
    report["source_manifest_sha256"] = hashlib.sha256(
        json.dumps(report["source_hashes"], sort_keys=True).encode()).hexdigest()
    raw_path = options.output / "results.json"
    raw_path.with_suffix(".json.license").write_text(NOTICE)
    journal_path = options.output / "samples.jsonl"
    raw_path.write_text(json.dumps(report, indent=2) + "\n")
    policies = ["cooperative", "auto", "required"]
    for case in options.cases:
        for sample in range(options.samples + 1):
            order = policies if sample % 2 == 0 else list(reversed(policies))
            for policy in order:
                command = [str(binary), str(image), policy, case, str(options.iterations), str(options.quantum_ns)]
                started_at = time.time()
                process = subprocess.run(command, capture_output=True, text=True, timeout=90)
                if process.returncode != 0:
                    report["failure"] = {"command": command, "exit": process.returncode,
                                         "stdout": process.stdout, "stderr": process.stderr}
                    raw_path.write_text(json.dumps(report, indent=2) + "\n")
                    journal_path.unlink(missing_ok=True)
                    raise SystemExit(f"benchmark failed: {policy}/{case}: {process.stderr.strip()}")
                item = json.loads(process.stdout)
                item.update(sample=sample, warmup=sample == 0, command=command, stderr=process.stderr,
                            started_at_unix=started_at, finished_at_unix=time.time())
                report["raw"].append(item)
                # Append each raw sample for crash recovery without rewriting
                # an increasingly large report between timed processes.
                with journal_path.open("a", encoding="utf-8") as journal:
                    journal.write(json.dumps(item) + "\n")
        for policy in policies:
            rows = [r for r in report["raw"] if r["case"] == case and r["policy"] == policy and not r["warmup"]]
            timings = [r["work_ns"] for r in rows]
            landing = [value for r in rows for value in r["request_to_landing_ns"]]
            timer = [value for r in rows for value in r["timer_lag_ns"]]
            io = [value for r in rows for value in r["io_lag_ns"]]
            report["summary"][case + "/" + policy] = {
                "median_work_ns": statistics.median(timings), "min_work_ns": min(timings), "max_work_ns": max(timings),
                "median_run_ns": statistics.median(r["run_ns"] for r in rows),
                "landing_count": len(landing), "landing_p99_ns": percentile(landing, .99),
                "landing_p999_ns": percentile(landing, .999), "landing_max_ns": max(landing, default=None),
                "timer_count": len(timer), "timer_p99_ns": percentile(timer, .99), "timer_p999_ns": percentile(timer, .999),
                "io_count": len(io), "io_p99_ns": percentile(io, .99), "io_p999_ns": percentile(io, .999),
                "effective_quantum_ns": rows[0]["quantum_ns"],
                "max_rss_bytes": max(r["max_rss_bytes"] for r in rows),
                "max_reserved_capsule_bytes": max(r["reserved_capsule_bytes"] for r in rows),
                "charge_per_task": rows[0]["charge_per_task"],
            }
        print(f"recorded {case}: {options.samples} matched samples per policy", flush=True)
        raw_path.write_text(json.dumps(report, indent=2) + "\n")
    for case in ("cpu", "yield"):
        if case not in options.cases:
            continue
        baseline = report["summary"][case + "/cooperative"]["median_work_ns"]
        for policy in ("auto", "required"):
            row = report["summary"][case + "/" + policy]
            row["throughput_regression_percent"] = 100 * (1 - baseline / row["median_work_ns"])
            row["initial_5_percent_gate_passed"] = row["throughput_regression_percent"] <= 5.0
    for probe in ("timer", "io"):
        if probe not in options.cases or probe + "-hog" not in options.cases:
            continue
        for policy in policies:
            idle = report["summary"][probe + "/" + policy]
            hog = report["summary"][probe + "-hog/" + policy]
            for tail in ("p99", "p999"):
                hog[probe + "_" + tail + "_hog_delta_ns"] = max(
                    0, hog[probe + "_" + tail + "_ns"] - idle[probe + "_" + tail + "_ns"])
    cpu_rows = [report["summary"]["cpu/" + policy] for policy in ("auto", "required")] if "cpu" in options.cases else []
    report["gates"] = {
        "normal_throughput_5_percent": all(report["summary"][case + "/" + policy]["initial_5_percent_gate_passed"]
                                           for case in ("cpu", "yield") for policy in ("auto", "required"))
                                           if "cpu" in options.cases and "yield" in options.cases else None,
        "cpu_landing_p99_2Q": all(row["landing_count"] > 0 and
                                  row["landing_p99_ns"] <= 2 * row["effective_quantum_ns"] for row in cpu_rows) if cpu_rows else None,
        "cpu_landing_p999_4Q": all(row["landing_count"] > 0 and
                                   row["landing_p999_ns"] <= 4 * row["effective_quantum_ns"] for row in cpu_rows) if cpu_rows else None,
    }
    changed_sources = snapshot_changes(report["source_hashes"], source_snapshot(ROOT))
    changed_artifacts = [str(path) for path in (binary, image)
                         if hashlib.sha256(path.read_bytes()).hexdigest() != report["hashes"][str(path)]]
    report["input_consistency"] = {
        "changed_sources": changed_sources, "changed_artifacts": changed_artifacts,
        "passed": not changed_sources and not changed_artifacts,
    }
    report["gates"]["source_and_artifacts_unchanged"] = report["input_consistency"]["passed"]
    report["finished_at_unix"] = time.time()
    report["finished_load_average"] = os.getloadavg()
    raw_path.write_text(json.dumps(report, indent=2) + "\n")
    journal_path.unlink(missing_ok=True)
    print(json.dumps(report["summary"], indent=2))
    return 1 if any(value is False for value in report["gates"].values()) else 0


if __name__ == "__main__":
    raise SystemExit(main())
