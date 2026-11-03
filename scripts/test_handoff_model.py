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

"""Protocol-model mutation checks, synthesis boundaries, and optional native smoke."""

import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "experiments/handoff"))
from handoff_model import budget_counterexample, check_wake, synthesize
from bench_handoff import compact_samples, percentile99


class ModelTests(unittest.TestCase):
    def test_native_protocol_abstraction(self):
        result = check_wake()
        self.assertTrue(result["passed"])
        self.assertGreater(result["terminal_states"], 1)

    def test_checker_rejects_double_enqueue(self):
        result = check_wake("duplicate")
        self.assertFalse(result["passed"])
        self.assertTrue(result["counterexample"])

    def test_checker_rejects_waking_unarmed_running_task(self):
        self.assertFalse(check_wake("queue_inline")["passed"])

    def test_zero_is_unbounded(self):
        self.assertEqual(len(budget_counterexample(0, 16)), 17)

    def test_exact_service_boundary(self):
        for bound in (1, 4, 16, 64):
            self.assertIsNone(budget_counterexample(bound, bound))
            self.assertEqual(len(budget_counterexample(bound + 1, bound)), bound + 1)

    def test_counterexample_eliminates_other_admitting_policies(self):
        result = synthesize(service_bound=16)
        self.assertEqual(result["accepted_bursts"], [16, 8, 4, 1])
        self.assertEqual(result["rounds"][0]["reject"], [0, 256, 64, 32])

    def test_empty_solution_reported(self):
        self.assertEqual(synthesize((0, 64), 16)["accepted_bursts"], [])

    def test_invalid_bounds(self):
        with self.assertRaises(ValueError):
            budget_counterexample(1, 0)
        with self.assertRaises(ValueError):
            synthesize((1, 1))

    def test_raw_histogram_is_lossless(self):
        samples = [4, 1, 4, 2, 9] * 21
        row = {"latencies_ns": samples.copy(), "observer_latencies_ns": []}
        compact_samples(row)
        restored = [value for value, count in row["latency_histogram_ns"] for _ in range(count)]
        self.assertEqual(restored, sorted(samples))
        self.assertEqual(percentile99(restored), percentile99(samples))
        self.assertEqual(row["latency_sample_count"], len(samples))
        self.assertEqual(row["observer_histogram_ns"], [])


@unittest.skipUnless(os.environ.get("HANDOFF_BENCH_BINARY"), "native benchmark not requested")
class NativeSmokeTests(unittest.TestCase):
    def test_exact_delivery_and_native_burst(self):
        binary = str(Path(os.environ["HANDOFF_BENCH_BINARY"]).resolve(strict=True))
        environment = {k: v for k, v in os.environ.items() if not k.startswith("LLAM_")}
        environment.update(LLAM_DIRECT_HANDOFF_STATS="1", LLAM_AUTOTUNE="off")
        for burst in (64, 16, 1):
            environment["LLAM_YIELD_DIRECT_HANDOFF_BURST"] = str(burst)
            run = subprocess.run([binary, "2", "512", "1"], env=environment,
                                 capture_output=True, text=True, timeout=30, check=True)
            row = json.loads(run.stdout)
            self.assertEqual(row["messages"], 1024)
            self.assertEqual(row["checksum"], 2 * 512 * 513 // 2)
            self.assertEqual(row["burst"], burst)
            self.assertLessEqual(row["max_observed_streak"], burst)
            self.assertEqual(len(row["latencies_ns"]), 64)

    def test_bad_cli(self):
        binary = str(Path(os.environ["HANDOFF_BENCH_BINARY"]).resolve(strict=True))
        for args in (("0", "100", "0"), ("1", "-1", "0"), ("1", "100", "3")):
            run = subprocess.run([binary, *args], capture_output=True, timeout=5)
            self.assertEqual(run.returncode, 2)


if __name__ == "__main__":
    unittest.main()
