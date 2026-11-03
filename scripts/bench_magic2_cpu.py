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

"""Isolated manual/Magic2/auto CPU comparisons with exact-result checks."""

import argparse
import datetime
import json
import os
import pathlib
import platform
import statistics
import subprocess


CASES = [
    ("tiny_copy", 64, 64, 1000000, 1, 0),
    ("copy_64k_tiles", 65536, 8192, 2048, 1, 0),
    ("copy_64k_tiles_4_tasks", 65536, 8192, 512, 4, 0),
    ("compute_32_rounds", 65536, 4096, 16, 1, 32),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, default=pathlib.Path("bench_magic2_cpu"))
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--modes", nargs="+", choices=("manual", "magic2", "auto"),
                        default=["manual", "magic2", "auto"])
    args = parser.parse_args()
    if args.samples < 3 or args.warmup < 0:
        parser.error("at least three samples and nonnegative warmup are required")
    if "manual" not in args.modes or len(set(args.modes)) != len(args.modes):
        parser.error("modes must include manual and contain no duplicates")
    binary = args.binary.resolve(strict=True)
    environment = {key: value for key, value in os.environ.items() if not key.startswith("LLAM_")}
    metadata = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "binary": str(binary),
        "samples": args.samples,
        "warmup_pairs": args.warmup,
        "modes": args.modes,
        "environment": "LLAM_* overrides removed; other host environment inherited",
        "timer": "inside managed controller, after task/channel/token setup; release/join included",
        "policy": "one P, RELEASE_FAST, automatic preemption OFF; identical tile/yield/cancel boundaries",
    }
    rows = []
    summaries = []
    for case, *parameters in CASES:
        ratios = {mode: [] for mode in args.modes if mode != "manual"}
        timings = {mode: [] for mode in args.modes}
        for pair in range(args.warmup + args.samples):
            order = args.modes if pair % 2 == 0 else list(reversed(args.modes))
            measured = {}
            for mode in order:
                command = [str(binary), mode, *(str(value) for value in parameters)]
                run = subprocess.run(command, env=environment, check=True, capture_output=True,
                                     text=True, timeout=60)
                value = json.loads(run.stdout)
                value.update(case=case, pair=pair, warmup=pair < args.warmup, command=command)
                rows.append(value)
                measured[mode] = value
            for mode in args.modes:
                for field in ("checksum", "yields", "workers", "blocking_calls"):
                    if measured["manual"][field] != measured[mode][field]:
                        raise RuntimeError(f"{case} pair {pair}: {mode} unmatched {field}")
            if pair >= args.warmup:
                baseline = measured["manual"]["elapsed_ns"]
                for mode in args.modes:
                    timings[mode].append(measured[mode]["elapsed_ns"])
                    if mode != "manual":
                        ratios[mode].append(measured[mode]["elapsed_ns"] / baseline)
        for mode in ratios:
            summary = {
                "case": case,
                "mode": mode,
                "manual_median_ms": statistics.median(timings["manual"]) / 1e6,
                "mode_median_ms": statistics.median(timings[mode]) / 1e6,
                "paired_median_overhead_percent": (statistics.median(ratios[mode]) - 1) * 100,
                "paired_min_overhead_percent": (min(ratios[mode]) - 1) * 100,
                "paired_max_overhead_percent": (max(ratios[mode]) - 1) * 100,
            }
            summaries.append(summary)
            print(json.dumps(summary), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({"metadata": metadata, "summaries": summaries, "raw": rows},
                                     indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
