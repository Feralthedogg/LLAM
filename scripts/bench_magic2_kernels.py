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

"""Compare fixed uint32 mixing kernels with real cold/warm Magic2 selection."""

import argparse
import datetime
import hashlib
import json
import os
import pathlib
import platform
import statistics
import subprocess


# name, uint32 elements, common grain, measured activations per G, Gs, rounds
CASES = [
    ("tiny", 64, 64, 200000, 1, 1),
    ("compute_256k", 65536, 4096, 32, 1, 16),
    ("compute_2m", 524288, 4096, 8, 1, 16),
    ("tail_unaligned", 65539, 4093, 32, 1, 16),
    ("shared_family_4_tasks", 65536, 4096, 8, 4, 16),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, default=pathlib.Path("bench_magic2_kernels"))
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--process-warmup", type=int, default=2)
    parser.add_argument("--training", type=int, default=16)
    parser.add_argument("--cases", nargs="+", choices=[case[0] for case in CASES])
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.samples < 3 or args.process_warmup < 0 or not 0 <= args.training <= 256:
        parser.error("require samples >= 3, process-warmup >= 0 and training in 0..256")
    binary = args.binary.resolve(strict=True)
    environment = {k: v for k, v in os.environ.items() if not k.startswith("LLAM_")}
    kernels = json.loads(subprocess.check_output([str(binary), "--list"], env=environment, text=True))["kernels"]
    modes = kernels + ["auto-cold", "auto-warm"]
    cases = [case for case in CASES if args.cases is None or case[0] in args.cases]
    result = {
        "metadata": {
            "copyright": "Copyright 2026 Feralthedogg", "spdx_license_identifier": "Apache-2.0",
            "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "platform": platform.platform(), "machine": platform.machine(),
            "binary": str(binary), "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "samples": args.samples, "discarded_process_warmups": args.process_warmup,
            "training_activations_per_G": args.training, "kernels": kernels, "modes": modes,
            "cases": cases, "environment": "LLAM_* overrides removed; other host environment inherited",
            "policy": "one P; RELEASE_FAST; preemption OFF; same grain, yields, cancellations, input and exact uint32 outputs",
            "training": "All modes warm buffers with the same activation count. Auto-cold uses scalar during untimed warmup, leaving the family untrained; auto-warm uses real family observations. No synthetic timing or profile injection.",
            "timing": "elapsed excludes plan setup and training; setup and warmup/readiness wall times are retained separately; release/join are timed",
            "baseline": "ordinary C under identical compiler flags, without vectorization-disabling options",
        },
        "summaries": [], "raw": [],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)

    def save():
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    try:
        for case, *parameters in cases:
            measured = {mode: [] for mode in modes}
            for pair in range(args.process_warmup + args.samples):
                shift = pair % len(modes)
                order = modes[shift:] + modes[:shift]
                if pair % 2:
                    order = list(reversed(order))
                group = {}
                for mode in order:
                    command = [str(binary), mode, *(str(p) for p in parameters), str(args.training)]
                    run = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=60)
                    if run.returncode:
                        result["failure"] = {"command": command, "exit_code": run.returncode,
                                             "stdout": run.stdout, "stderr": run.stderr}
                        raise RuntimeError(f"{case}/{mode} exited {run.returncode}: {run.stderr}")
                    row = json.loads(run.stdout)
                    row.update(case=case, pair=pair, process_warmup=pair < args.process_warmup,
                               command=command, stderr=run.stderr)
                    result["raw"].append(row)
                    group[mode] = row
                    if pair >= args.process_warmup:
                        measured[mode].append(row)
                for mode, row in group.items():
                    for field in ("checksum", "workers", "blocking_calls", "yields"):
                        if row[field] != group["scalar"][field]:
                            raise RuntimeError(f"{case}/{pair}: {mode} unmatched {field}")
                    if row["workers"] != 1 or row["blocking_calls"] != 0:
                        raise RuntimeError("unexpected worker/blocking budget")
                    if mode.startswith("auto-"):
                        counts = row["measured"]
                        if sum(counts["routes"]) != row["iterations"] * row["tasks"]:
                            raise RuntimeError("missing automatic admission records")
                        if sum(counts["selected"]) != counts["routes"][1] or counts["observations"] != counts["routes"][1]:
                            raise RuntimeError("missing real family execution observations")
            best = min(kernels, key=lambda m: statistics.median(r["elapsed_ns"] for r in measured[m]))
            for mode in modes:
                rows = measured[mode]
                speedups = [b["elapsed_ns"] / r["elapsed_ns"] for b, r in zip(measured["scalar"], rows)]
                relative_best = [r["elapsed_ns"] / b["elapsed_ns"] for b, r in zip(measured[best], rows)]
                summary = {
                    "case": case, "mode": mode, "median_ms": statistics.median(r["elapsed_ns"] for r in rows) / 1e6,
                    "paired_speedup_vs_scalar": statistics.median(speedups),
                    "paired_speedup_range": [min(speedups), max(speedups)],
                    "best_fixed_kernel_by_median": best,
                    "paired_overhead_vs_best_fixed_percent": (statistics.median(relative_best) - 1) * 100,
                    "median_setup_us": statistics.median(r["plan_setup_ns"] for r in rows) / 1e3,
                    "median_warmup_readiness_ms": statistics.median(r["warmup_wall_ns"] for r in rows) / 1e6,
                    "selected": {name: sum(r["measured"]["selected"][i] for r in rows) for i, name in enumerate(kernels)},
                    "routes": [sum(r["measured"]["routes"][i] for r in rows) for i in range(4)],
                    "training_selected": {name: sum(r["training"]["selected"][i] for r in rows) for i, name in enumerate(kernels)},
                }
                result["summaries"].append(summary)
                print(json.dumps(summary), flush=True)
            save()
    except (RuntimeError, subprocess.SubprocessError, ValueError) as exc:
        result["error"] = str(exc)
        save()
        raise


if __name__ == "__main__":
    main()
