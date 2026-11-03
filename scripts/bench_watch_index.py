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

"""Paired native watch lookup and socket I/O comparisons across connection counts."""

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


def run(binary, mode, connections, operations):
    env = {key: value for key, value in os.environ.items() if not key.startswith("LLAM_")}
    command = [str(binary), mode, str(connections), str(operations)]
    process = run_capture(command, env=env, timeout=120)
    if process.returncode != 0:
        raise RuntimeError(f"{command}: exit {process.returncode}: {process.stderr}")
    row = json.loads(process.stdout)
    samples = row.pop("latencies_ns")
    if (row["initial_watches"] != connections or row["workers"] != 1 or
            row["operations"] != len(samples) or not samples or
            samples != sorted(samples) or
            samples[(len(samples) * 99 + 99) // 100 - 1] != row["p99_ns"]):
        raise RuntimeError("incomplete workload or invalid latency sample record")
    row["histogram_ns"] = sorted(Counter(samples).items())
    row["command"] = command
    return row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, default=Path("bench_watch_index"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--connections", type=int, nargs="+", default=[1, 16, 128, 1024, 4096])
    parser.add_argument("--modes", nargs="+", choices=("lookup", "io"), default=["lookup", "io"])
    parser.add_argument("--lookup-operations", type=int, default=32768)
    parser.add_argument("--io-operations", type=int, default=8192)
    args = parser.parse_args()
    if (args.samples < 3 or args.warmup < 0 or
            any(n < 1 or n > 4096 or n & (n - 1) for n in args.connections) or
            not 100 <= args.lookup_operations <= 1048576 or
            not 100 <= args.io_operations <= 1048576):
        parser.error("invalid sample or workload settings")
    if args.output.exists() or args.output.with_name(args.output.name + ".license").exists():
        parser.error("choose a new output path")
    binaries = {"baseline": args.baseline.resolve(strict=True), "indexed": args.candidate.resolve(strict=True)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    result = {"status": "running", "metadata": {
        "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(), "command": sys.argv,
        "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                     for name, path in binaries.items()},
        "environment": "LLAM_* inherited overrides removed; RELEASE_FAST, deterministic one P",
        "lookup": "real socket descriptors, fstat identity validation and watch_lock included",
        "io": "real managed poll/read/echo per connection and native blocking client; RTT from write to reply",
        "p99": "nearest rank of every individual operation; exact nanosecond/count histogram",
        "limits": "AF_UNIX socketpairs; one P/node; sequential client requests; setup/teardown excluded",
    }, "raw": [], "summaries": []}

    def save():
        args.output.write_text(json.dumps(result, separators=(",", ":")) + "\n")

    save()
    args.output.with_name(args.output.name + ".license").write_text(
        "SPDX-FileCopyrightText: 2026 Feralthedogg\nSPDX-License-Identifier: Apache-2.0\n")
    for mode in args.modes:
        operations = args.lookup_operations if mode == "lookup" else args.io_operations
        for connections in args.connections:
            measured = {name: [] for name in binaries}
            ratios = {"throughput": [], "p99": []}
            for pair in range(args.warmup + args.samples):
                order = ("baseline", "indexed") if pair % 2 == 0 else ("indexed", "baseline")
                rows = {}
                for name in order:
                    row = run(binaries[name], mode, connections, operations)
                    row.update(variant=name, pair=pair, warmup=pair < args.warmup)
                    result["raw"].append(row)
                    rows[name] = row
                    if pair >= args.warmup:
                        measured[name].append(row)
                if rows["baseline"]["operations"] != rows["indexed"]["operations"]:
                    raise RuntimeError("mismatched work")
                if pair >= args.warmup:
                    ratios["throughput"].append(rows["baseline"]["elapsed_ns"] / rows["indexed"]["elapsed_ns"])
                    ratios["p99"].append(rows["indexed"]["p99_ns"] / rows["baseline"]["p99_ns"])
                save()
            summary = {"mode": mode, "connections": connections}
            for name, rows in measured.items():
                summary[name + "_ops_per_second"] = statistics.median(
                    row["operations"] * 1e9 / row["elapsed_ns"] for row in rows)
                summary[name + "_p99_ns"] = statistics.median(row["p99_ns"] for row in rows)
            for metric, values in ratios.items():
                summary[metric + "_change_pct"] = (statistics.median(values) - 1) * 100
                summary[metric + "_ratio_range"] = [min(values), max(values)]
            result["summaries"].append(summary)
            save()
            print(json.dumps(summary), flush=True)
    result["status"] = "complete"
    save()


if __name__ == "__main__":
    main()
