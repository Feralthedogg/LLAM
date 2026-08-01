#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import shrink_lrpa
import run_lrpa


TARGET_SIGNATURE = "deadbeefcafefeed"


FAKE_EXECUTABLE = r"""
import argparse
import json
import pathlib
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--manifest", type=pathlib.Path, required=True)
args = parser.parse_args()
manifest = json.loads(args.manifest.read_text())

if manifest["lane_count"] == 1:
    time.sleep(1.0)

step_ids = {step["value"] for step in manifest["perturbations"]}
fails = (
    manifest["lane_count"] >= 2
    and {2, 5}.issubset(step_ids)
    and manifest["rounds"] >= 3
    and manifest["worker_count"] >= 2
    and (manifest["allowed_outcomes"] & 2) != 0
    and manifest["queue_capacity"] >= 8
)
executions = manifest["lane_count"] * manifest["worker_count"] * manifest["rounds"]
if fails:
    status = "oracle_failure"
    signature = "deadbeefcafefeed"
    failures = 1
    failure_round = 0
    oracle = 1
    returncode = 10
else:
    status = "ok"
    signature = "0000000000000000"
    failures = 0
    failure_round = None
    oracle = 0
    returncode = 0
document = {
    "schema_version": 1,
    "status": status,
    "seed": manifest["seed"],
    "lanes": manifest["lane_count"],
    "workers": manifest["worker_count"],
    "rounds_requested": manifest["rounds"],
    "rounds_completed": manifest["rounds"],
    "coupling": manifest["coupling"],
    "fault": manifest["fault"],
    "signature": signature,
    "lane_executions": executions,
    "elapsed_ns": 1,
    "failures": failures,
    "first_failure_round": failure_round,
    "oracle": oracle,
    "armed_total": 0 if fails else manifest["lane_count"] * manifest["rounds"],
    "winner_total": 0 if fails else manifest["lane_count"] * manifest["rounds"],
    "cancel_total": 0,
    "timeout_total": 0,
    "discard_total": 0 if fails else executions - manifest["lane_count"] * manifest["rounds"],
    "trace_entries": 1,
    "trace_truncated": False,
    "cleanup_complete": True,
}
print(json.dumps(document, separators=(",", ":")))
raise SystemExit(returncode)
"""


class ShrinkerTests(unittest.TestCase):
    def test_semantic_shrinker_preserves_literal_signature(self) -> None:
        original = {
            "version": 1,
            "gadget": "select",
            "seed": 99,
            "lane_count": 16,
            "worker_count": 4,
            "rounds": 32,
            "coupling": "colored_graph",
            "timeout_ns": 2_000_000_000,
            "fault": "select_skip_winner_cas",
            "generate_perturbations": False,
            "perturbations": [
                {
                    "kind": 0,
                    "lane_mask": 1,
                    "sequence": index,
                    "value": index,
                }
                for index in range(8)
            ],
            "perturbation_count": 8,
            "perturbation_hash": "0000000000000000",
            "allowed_outcomes": 30,
            "queue_capacity": 1024,
        }
        with tempfile.TemporaryDirectory() as directory_text:
            directory = pathlib.Path(directory_text)
            fake = directory / "fake_lrpa.py"
            fake.write_text(textwrap.dedent(FAKE_EXECUTABLE))
            output = directory / "shrink"
            minimized = shrink_lrpa.shrink_manifest(
                [sys.executable, str(fake)], original, TARGET_SIGNATURE,
                output, timeout=0.2,
            )

            self.assertEqual(minimized["lane_count"], 2)
            self.assertEqual(
                [step["value"] for step in minimized["perturbations"]], [2, 5]
            )
            self.assertEqual(minimized["rounds"], 3)
            self.assertEqual(minimized["coupling"], "independent")
            self.assertEqual(minimized["worker_count"], 2)
            self.assertEqual(minimized["allowed_outcomes"], 2)
            self.assertEqual(minimized["queue_capacity"], 8)
            self.assertEqual(minimized["perturbation_count"], 2)
            self.assertNotEqual(
                minimized["perturbation_hash"], "0000000000000000"
            )

            minimized_path = output / "minimized-manifest.json"
            log_path = output / "shrink-log.jsonl"
            self.assertEqual(json.loads(minimized_path.read_text()), minimized)
            log_rows = [json.loads(line) for line in log_path.read_text().splitlines()]
            self.assertGreater(len(log_rows), 8)
            self.assertTrue(any(row["reason"] == "timeout" for row in log_rows))
            self.assertTrue(any(row["accepted"] for row in log_rows))
            self.assertEqual(log_rows[-1]["phase"], "verify")
            self.assertTrue(log_rows[-1]["accepted"])

    def test_different_signature_never_counts_as_preserved(self) -> None:
        document = shrink_lrpa.candidate_matches_signature(
            [sys.executable, "-c", "print('not json')"],
            {"seed": 1}, TARGET_SIGNATURE, timeout=1.0,
        )
        self.assertFalse(document.matched)
        self.assertEqual(document.reason, "invalid_result")

    def test_real_fault_manifest_is_shrinkable(self) -> None:
        binary = os.environ.get("LRPA_FAULT_BENCH_BINARY")
        if not binary:
            self.skipTest("LRPA_FAULT_BENCH_BINARY is not configured")
        config = run_lrpa.RunConfig(
            seed=3, lanes=8, workers=4, rounds=4,
            coupling="independent", queue_capacity=4096,
            timeout_ms=2000, fault="select_skip_winner_cas",
            allowed_outcomes=30, generate_perturbations=True,
        )
        with tempfile.TemporaryDirectory() as directory_text:
            directory = pathlib.Path(directory_text)
            sample = run_lrpa.run_sample(
                pathlib.Path(binary), config, directory / "sample",
                process_timeout=5.0, replay_failures=True,
            )
            original = json.loads(
                (sample.artifact_dir / "manifest.json").read_text()
            )
            minimized = shrink_lrpa.shrink_manifest(
                [binary], original, sample.document["signature"],
                directory / "shrink", timeout=5.0,
            )
            evaluation = shrink_lrpa.candidate_matches_signature(
                [binary], minimized, sample.document["signature"],
                timeout=5.0,
            )
            self.assertTrue(evaluation.matched)
            self.assertLessEqual(minimized["lane_count"], 8)


if __name__ == "__main__":
    unittest.main()
