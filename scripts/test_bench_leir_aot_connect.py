# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Copyright 2026 Feralthedogg

import math
from pathlib import Path
import unittest

from scripts import bench_leir_aot_connect as bench


def sample(candidate: str, **overrides: object) -> dict[str, object]:
    record: dict[str, object] = {
        "version": 1,
        "candidate": candidate,
        "family": "tcp",
        "concurrency": 4,
        "payload": 64,
        "activations": 100,
        "wall_ns": 1_000_000 if candidate == "portable" else 900_000,
        "cpu_ns": 900_000 if candidate == "portable" else 600_000,
        "p99_ns": 20_000 if candidate == "portable" else 19_000,
        "correctness": 1,
        "queue_publications": 0 if candidate == "portable" else 100,
        "prepared_sqes": 0 if candidate == "portable" else 200,
        "observed_cqes": 200 if candidate == "portable" else 100,
        "suppressed_success_cqes": 0 if candidate == "portable" else 100,
        "task_parks": 100,
        "terminal_wakes": 100,
        "hot_allocations": 0,
        "checksum": "0123456789abcdef",
    }
    record.update(overrides)
    return record


class ParseSampleTests(unittest.TestCase):
    def test_parses_complete_sample(self) -> None:
        line = (
            "LEIR_AOT_CONNECT_SAMPLE version=1 candidate=native family=tcp "
            "concurrency=4 payload=64 activations=100 wall_ns=900000 "
            "cpu_ns=600000 p99_ns=19000 correctness=1 "
            "queue_publications=100 prepared_sqes=200 observed_cqes=100 "
            "suppressed_success_cqes=100 task_parks=100 terminal_wakes=100 "
            "hot_allocations=0 checksum=0123456789abcdef"
        )
        self.assertEqual(bench.parse_sample(line), sample("native"))

    def test_rejects_missing_duplicate_and_nonfinite_fields(self) -> None:
        complete = bench.format_sample(sample("native"))
        with self.assertRaisesRegex(ValueError, "missing"):
            bench.parse_sample(complete.replace(" wall_ns=900000", ""))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            bench.parse_sample(complete + " wall_ns=1")
        with self.assertRaisesRegex(ValueError, "integer"):
            bench.parse_sample(complete.replace("wall_ns=900000", "wall_ns=nan"))

    def test_accepts_unix_family(self) -> None:
        record = sample("native", family="unix")
        self.assertEqual(bench.parse_sample(bench.format_sample(record)), record)


class MechanismVerdictTests(unittest.TestCase):
    def test_continues_only_with_exact_native_counters(self) -> None:
        verdict = bench.classify_mechanism(
            [sample("portable")], [sample("native")]
        )
        self.assertEqual(verdict["verdict"], "CONTINUE")
        self.assertAlmostEqual(verdict["wall_ratio"], 0.9)

        bad = sample("native", observed_cqes=101)
        verdict = bench.classify_mechanism([sample("portable")], [bad])
        self.assertEqual(verdict["verdict"], "STOP")
        self.assertIn("observed_cqes", verdict["reasons"])

    def test_stops_on_correctness_allocation_or_wall_regression(self) -> None:
        cases = [
            sample("native", correctness=0),
            sample("native", hot_allocations=1),
            sample("native", wall_ns=1_050_001),
        ]
        for candidate in cases:
            with self.subTest(candidate=candidate):
                verdict = bench.classify_mechanism(
                    [sample("portable")], [candidate]
                )
                self.assertEqual(verdict["verdict"], "STOP")

    def test_rejects_mismatched_or_missing_cells(self) -> None:
        with self.assertRaisesRegex(ValueError, "sample count"):
            bench.classify_mechanism(
                [sample("portable"), sample("portable")],
                [sample("native")],
            )
        with self.assertRaisesRegex(ValueError, "cell"):
            bench.classify_mechanism(
                [sample("portable")],
                [sample("native", payload=4096)],
            )

    def test_marks_threshold_crossing_confidence_as_incomplete(self) -> None:
        portable = [sample("portable") for _ in range(5)]
        ratios = (0.90, 0.95, 1.00, 1.08, 1.10)
        native = [
            sample("native", wall_ns=round(1_000_000 * ratio))
            for ratio in ratios
        ]
        verdict = bench.classify_mechanism(portable, native)
        self.assertEqual(verdict["verdict"], "INCOMPLETE")
        self.assertIn("wall_confidence", verdict["reasons"])


class DriverContractTests(unittest.TestCase):
    def test_balanced_order_preserves_equal_sample_counts(self) -> None:
        order = bench.balanced_order(5)
        self.assertEqual(
            order,
            [
                "portable",
                "native",
                "native",
                "portable",
                "portable",
                "native",
                "native",
                "portable",
                "portable",
                "native",
            ],
        )
        self.assertEqual(order.count("portable"), 5)
        self.assertEqual(order.count("native"), 5)

    def test_benchmark_command_carries_the_exact_cell(self) -> None:
        command = bench.benchmark_command(
            Path("/tmp/bench-leir"),
            candidate="native",
            family="unix",
            concurrency=16,
            payload=4096,
            activations=512,
        )
        self.assertEqual(
            command,
            [
                str(Path("/tmp/bench-leir").resolve()),
                "--candidate",
                "native",
                "--family",
                "unix",
                "--concurrency",
                "16",
                "--payload",
                "4096",
                "--activations",
                "512",
            ],
        )


class ReleaseGateTests(unittest.TestCase):
    def test_release_gate_is_stricter_and_requires_two_machines(self) -> None:
        portable = sample("portable", wall_ns=1_500_000, cpu_ns=1_000_000)
        native = sample("native", wall_ns=1_000_000, cpu_ns=700_000)
        single = bench.classify_release_gate(
            [(portable, native)], machine_count=1
        )
        self.assertEqual(single["verdict"], "BLOCKED")
        self.assertIn("machine_count", single["reasons"])

        two = bench.classify_release_gate(
            [(portable, native)], machine_count=2
        )
        self.assertEqual(two["verdict"], "READY")

    def test_release_gate_rejects_nonfinite_metrics(self) -> None:
        native = sample("native")
        native["wall_ns"] = math.inf
        with self.assertRaisesRegex(ValueError, "integer"):
            bench.classify_release_gate(
                [(sample("portable"), native)], machine_count=2
            )


if __name__ == "__main__":
    unittest.main()
