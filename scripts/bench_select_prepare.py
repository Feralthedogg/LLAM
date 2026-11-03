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

"""Matched small/large select measurements with exact results and latency histograms."""

import argparse
from collections import Counter
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import sys

sys.dont_write_bytecode = True
from process_utils import run_capture


def run_sample(binary, mode, count, unique, iterations):
    command = [str(binary), mode, str(count), str(unique), str(iterations)]
    env = {key: value for key, value in os.environ.items() if not key.startswith("LLAM_")}
    process = run_capture(command, env=env, timeout=120)
    if process.returncode:
        raise RuntimeError(f"{command}: exit {process.returncode}: {process.stderr}")
    row = json.loads(process.stdout)
    samples = row.pop("latencies_ns")
    expected_checksum = iterations * (2 * row["warmup"] + iterations + 1) // 2
    if (row["operations"] != count or row["channels"] != unique or row["workers"] != 1 or
            row["iterations"] != iterations or len(samples) != iterations or
            samples != sorted(samples) or row["checksum"] != expected_checksum or
            row["p99_ns"] != samples[(len(samples) * 99 + 99) // 100 - 1] or
            (mode == "park" and row["parks"] < iterations)):
        raise RuntimeError("work, mapping, checksum, percentile or actual parking mismatch")
    row["histogram_ns"] = sorted(Counter(samples).items())
    row["command"] = command
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, default=Path("bench_select_prepare"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--counts", type=int, nargs="+", default=[1, 2, 4, 8, 9, 32, 64, 65, 128, 512, 2048, 4096])
    parser.add_argument("--modes", choices=("ready", "park"), nargs="+", default=["ready", "park"])
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--skip-duplicates", action="store_true")
    parser.add_argument("--iteration-scale", type=int, default=1)
    args = parser.parse_args()
    if (args.samples < 3 or args.warmup < 0 or not 1 <= args.iteration_scale <= 16 or
            any(n < 1 or n > 4096 for n in args.counts) or len(set(args.counts)) != len(args.counts)):
        parser.error("invalid workload/sampling settings")
    if args.output.exists() or args.output.with_name(args.output.name + ".license").exists():
        parser.error("choose a new result path")
    binaries = {"baseline": args.baseline.resolve(strict=True), "prepared": args.candidate.resolve(strict=True)}
    cases = [(mode, count, count) for mode in args.modes for count in args.counts]
    if not args.skip_duplicates and "park" in args.modes:
        cases.extend(("park", count, unique) for count in (512, 4096) for unique in (1, 8, 9, 64))
    report = {"status": "running", "metadata": {
        "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(), "command": sys.argv,
        "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                     for name, path in binaries.items()},
        "environment": "LLAM_* overrides removed; RELEASE_FAST, one P, lock-free normal queue",
        "throughput": "completed exact select exchanges per second; setup/warmup/teardown excluded",
        "p99": "nearest rank over every select entry-to-return observation; exact value/count histograms",
        "ready": "buffer fill precedes each ready select; throughput includes fill and result checks",
        "park": "sender waits for PARKED; every measured call exercises wait preparation and cleanup",
        "limits": "single-P closed-loop benchmark; no timing guarantee under arbitrary OS load",
    }, "raw": [], "summaries": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_name(args.output.name + ".license").write_text(
        "SPDX-FileCopyrightText: 2026 Feralthedogg\nSPDX-License-Identifier: Apache-2.0\n")

    def save():
        args.output.write_text(json.dumps(report, separators=(",", ":")) + "\n")

    save()
    for mode, count, unique in cases:
        base_iterations = (max(1024, 1048576 * 8 // max(count, 8)) if mode == "ready" else
                           max(512, 1048576 // max(count, 8)))
        iterations = min(2000000, base_iterations * args.iteration_scale)
        measured = {name: [] for name in binaries}
        ratios = {"throughput": [], "p99": []}
        for pair in range(args.warmup + args.samples):
            order = ("baseline", "prepared") if pair % 2 == 0 else ("prepared", "baseline")
            rows = {}
            for name in order:
                row = run_sample(binaries[name], mode, count, unique, iterations)
                row.update(variant=name, pair=pair, warmup_pair=pair < args.warmup)
                report["raw"].append(row)
                rows[name] = row
                if pair >= args.warmup:
                    measured[name].append(row)
            if rows["baseline"]["checksum"] != rows["prepared"]["checksum"]:
                raise RuntimeError("mismatched work")
            if pair >= args.warmup:
                ratios["throughput"].append(rows["baseline"]["elapsed_ns"] / rows["prepared"]["elapsed_ns"])
                ratios["p99"].append(rows["prepared"]["p99_ns"] / rows["baseline"]["p99_ns"])
            save()
        summary = {"mode": mode, "operations": count, "channels": unique, "iterations": iterations}
        for name, rows in measured.items():
            summary[name + "_selects_per_second"] = statistics.median(
                row["iterations"] * 1e9 / row["elapsed_ns"] for row in rows)
            summary[name + "_p99_ns"] = statistics.median(row["p99_ns"] for row in rows)
        for metric, values in ratios.items():
            summary[metric + "_change_pct"] = (statistics.median(values) - 1) * 100
            summary[metric + "_ratio_range"] = [min(values), max(values)]
        report["summaries"].append(summary)
        save()
        print(json.dumps(summary), flush=True)
    report["status"] = "complete"
    save()


if __name__ == "__main__":
    main()
