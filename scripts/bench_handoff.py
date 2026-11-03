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

"""Synthesize bounded handoff bursts, train, then compare a frozen policy in fresh runs."""

import argparse
from collections import Counter
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments/handoff"))
from handoff_model import check_wake, synthesize
from process_utils import run_capture

CASES = (("pair", 1, 0), ("contended", 4, 1), ("timers", 4, 2))


def percentile99(values):
    return sorted(values)[math.ceil(len(values) * 0.99) - 1]


def summarize(case, rows, candidate):
    baseline = {row["pair"]: row for row in rows if row["burst"] == 64}
    proposed = {row["pair"]: row for row in rows if row["burst"] == candidate}
    throughput = []
    latency = []
    for pair, value in proposed.items():
        reference = baseline[pair]
        throughput.append(reference["elapsed_ns"] / value["elapsed_ns"])
        latency.append(value["p99_ns"] / reference["p99_ns"])
    return {
        "case": case, "candidate_burst": candidate,
        "baseline_messages_per_second": statistics.median(
            v["messages"] * 1e9 / v["elapsed_ns"] for v in baseline.values()),
        "candidate_messages_per_second": statistics.median(
            v["messages"] * 1e9 / v["elapsed_ns"] for v in proposed.values()),
        "baseline_p99_ns": statistics.median(v["p99_ns"] for v in baseline.values()),
        "candidate_p99_ns": statistics.median(v["p99_ns"] for v in proposed.values()),
        "paired_throughput_change_pct": (statistics.median(throughput) - 1) * 100,
        "paired_p99_change_pct": (statistics.median(latency) - 1) * 100,
        "throughput_ratio_range": [min(throughput), max(throughput)],
        "p99_ratio_range": [min(latency), max(latency)],
        "baseline_observer_p99_ns": statistics.median(v["observer_p99_ns"] for v in baseline.values()),
        "candidate_observer_p99_ns": statistics.median(v["observer_p99_ns"] for v in proposed.values()),
        "minimum_observer_samples": min(v["observer_sample_count"] for v in rows),
    }


def run_sample(binary, case, rounds, burst, pair, phase):
    name, pairs, observer = case
    env = {key: value for key, value in os.environ.items() if not key.startswith("LLAM_")}
    env.update(LLAM_AUTOTUNE="off", LLAM_DIRECT_HANDOFF_STATS="1")
    # Baseline exercises the platform/profile default, rather than imposing a replacement.
    if burst != 64:
        env["LLAM_YIELD_DIRECT_HANDOFF_BURST"] = str(burst)
    command = [str(binary), str(pairs), str(rounds), str(observer)]
    run = run_capture(command, env=env, timeout=60)
    if run.returncode != 0:
        raise RuntimeError(f"{name}/{burst}: exit {run.returncode}: {run.stderr}")
    row = json.loads(run.stdout)
    if (row["burst"] != burst or row["workers"] != 1 or
            row["messages"] != pairs * rounds or
            row["checksum"] != pairs * rounds * (rounds + 1) // 2 or
            row["p99_ns"] != percentile99(row["latencies_ns"]) or
            row["max_observed_streak"] > burst):
        raise RuntimeError(f"{name}/{burst}: mismatched work, policy, percentile or burst bound")
    if row["wake_handoff_hits"] == 0:
        raise RuntimeError(f"{name}/{burst}: direct wake handoff was not exercised")
    compact_samples(row)
    row.update(case=name, pair=pair, phase=phase, command=command,
               environment={key: value for key, value in env.items() if key.startswith("LLAM_")})
    return row


def compact_samples(row):
    """Losslessly retain sorted observations as exact nanosecond/count pairs."""
    for source, prefix in (("latencies_ns", "latency"), ("observer_latencies_ns", "observer")):
        samples = row.pop(source)
        row[f"{prefix}_sample_count"] = len(samples)
        row[f"{prefix}_histogram_ns"] = sorted(Counter(samples).items())


def save(path, data):
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    notice = path.with_name(path.name + ".license")
    if not notice.exists():
        notice.write_text("SPDX-FileCopyrightText: 2026 Feralthedogg\n"
                          "SPDX-License-Identifier: Apache-2.0\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=ROOT / "bench_handoff")
    parser.add_argument("--protocol-binary", type=Path, default=ROOT / "test_handoff_protocol")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--training-samples", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--rounds", type=int, default=65536)
    parser.add_argument("--service-bound", type=int, default=16)
    parser.add_argument("--max-training-throughput-loss", type=float, default=0.10)
    args = parser.parse_args()
    if (args.samples < 3 or args.training_samples < 2 or args.warmup < 0 or
            not 4096 <= args.rounds <= 1000000 or not 1 <= args.service_bound < 64 or
            not 0 <= args.max_training_throughput_loss < 1):
        parser.error("invalid sample, round, service-bound, or throughput-loss setting")
    binary = args.binary.resolve(strict=True)
    protocol_binary = args.protocol_binary.resolve(strict=True)
    if args.output.exists() or args.output.with_name(args.output.name + ".license").exists():
        parser.error("output or its license notice already exists; choose a new result file")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    synthesis = synthesize(service_bound=args.service_bound)
    proof = check_wake()
    mutants = {name: check_wake(name) for name in ("duplicate", "queue_inline")}
    if not proof["passed"] or any(value["passed"] for value in mutants.values()):
        raise RuntimeError("model verification/mutation checks failed")
    native = run_capture([str(protocol_binary)], timeout=30)
    if native.returncode != 0:
        raise RuntimeError(f"native wake protocol check failed: {native.returncode}: {native.stderr}")
    signature = ROOT / "object/.build-signature"
    sources = [ROOT / "experiments/handoff/handoff_model.py", Path(__file__),
               ROOT / "experiments/handoff/bench_handoff.c",
               ROOT / "experiments/handoff/verify_handoff.c",
               ROOT / "src/core/wait/wait_tracking.c", ROOT / "src/core/sync/sync.c",
               ROOT / "src/core/sched/reinject.c", ROOT / "src/core/task/yield_join_sleep.c"]
    report = {
        "status": "running", "metadata": {
            "time_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "platform": platform.platform(), "machine": platform.machine(),
            "command": sys.argv, "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "build_signature": signature.read_text() if signature.exists() else None,
            "source_sha256": {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},
            "latency": "send entry to receive return; one mixed phase per 16-message block, nearest-rank p99 of samples",
            "throughput": "validated request/ack exchanges per second; setup/join excluded",
            "sample_storage": "lossless sorted [nanoseconds, count] histograms; no bucket approximation",
            "baseline": "RELEASE_FAST, one P, AUTO preemption, lock-free normal queue, default burst=64; autotune off",
            "limitations": "SC finite protocol model, not full C11/ABI/lifetime proof; one-P performance; no ETH solver",
        }, "model": proof, "mutation_checks": mutants, "synthesis": synthesis,
        "native_protocol": json.loads(native.stdout), "raw": [], "training_scores": [], "summaries": []}
    save(args.output, report)
    accepted = synthesis["accepted_bursts"]
    if not accepted:
        raise RuntimeError("no policy satisfies the requested experiment service bound")

    # Rotate training order to reduce fixed position/thermal bias. Confirmation
    # data are fresh and cannot change the frozen winner.
    policies = [64, *accepted]
    training = []
    for case in CASES:
        for pair in range(args.training_samples + args.warmup):
            shift = pair % len(policies)
            order = policies[shift:] + policies[:shift]
            if pair % 2:
                order.reverse()
            for burst in order:
                phase = "training-warmup" if pair < args.warmup else "training"
                row = run_sample(binary, case, max(4096, args.rounds // 4), burst, pair, phase)
                report["raw"].append(row)
                if phase == "training":
                    training.append(row)
        save(args.output, report)
    for burst in accepted:
        summaries = [summarize(case[0], [r for r in training if r["case"] == case[0]
                     and r["burst"] in (64, burst)], burst) for case in CASES]
        latency_ratio = math.prod(1 + s["paired_p99_change_pct"] / 100 for s in summaries) ** (1 / len(summaries))
        feasible = all(s["paired_throughput_change_pct"] >= -100 * args.max_training_throughput_loss
                       for s in summaries)
        report["training_scores"].append({"burst": burst, "p99_ratio_geomean": latency_ratio,
                                           "within_throughput_budget": feasible, "cases": summaries})
    scores = report["training_scores"]
    eligible = [score for score in scores if score["within_throughput_budget"]]
    winner = min(eligible or scores, key=lambda score: (score["p99_ratio_geomean"], -score["burst"]))
    candidate = winner["burst"]
    report["selected_burst"] = candidate
    report["training_feasible"] = bool(eligible)
    save(args.output, report)
    print(json.dumps({"selected_burst": candidate, "training_feasible": bool(eligible)}), flush=True)

    for case in CASES:
        rows = []
        for pair in range(args.samples + args.warmup):
            order = (64, candidate) if pair % 2 == 0 else (candidate, 64)
            for burst in order:
                phase = "confirmation-warmup" if pair < args.warmup else "confirmation"
                row = run_sample(binary, case, args.rounds, burst, pair, phase)
                report["raw"].append(row)
                if phase == "confirmation":
                    rows.append(row)
        summary = summarize(case[0], rows, candidate)
        report["summaries"].append(summary)
        save(args.output, report)
        print(json.dumps(summary), flush=True)
    report["native_service_witness"] = {
        "baseline_exceeds_experiment_bound": any(r["burst"] == 64 and
            r["max_observed_streak"] > args.service_bound for r in report["raw"]),
        "candidate_within_bound": all(r["max_observed_streak"] <= args.service_bound
            for r in report["raw"] if r["burst"] == candidate),
        "meaning": "Observed native chain witness; not deterministic replay of every atomic model step"}
    report["status"] = "complete"
    save(args.output, report)


if __name__ == "__main__":
    main()
