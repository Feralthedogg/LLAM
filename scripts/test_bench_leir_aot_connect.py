# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Copyright 2026 Feralthedogg

import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from scripts import bench_leir_aot_connect as bench


def sample(candidate: str, **overrides: object) -> dict[str, object]:
    record: dict[str, object] = {
        "version": 2,
        "candidate": candidate,
        "ring_profile": "submit_all",
        "family": "tcp",
        "concurrency": 4,
        "payload": 64,
        "activations": 100,
        "wall_ns": 1_000_000 if candidate == "portable" else 900_000,
        "cpu_ns": 900_000 if candidate == "portable" else 600_000,
        "p99_ns": 20_000 if candidate == "portable" else 19_000,
        "bind_ns": 10_000 if candidate == "portable" else 8_000,
        "execute_ns": 900_000 if candidate == "portable" else 850_000,
        "aot_prepare_ns": 0 if candidate == "portable" else 10_000,
        "aot_ring_ns": 0 if candidate == "portable" else 800_000,
        "aot_resume_ns": 0 if candidate == "portable" else 10_000,
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
            "LEIR_AOT_CONNECT_SAMPLE version=2 candidate=native "
            "ring_profile=submit_all family=tcp "
            "concurrency=4 payload=64 activations=100 wall_ns=900000 "
            "cpu_ns=600000 p99_ns=19000 bind_ns=8000 execute_ns=850000 "
            "aot_prepare_ns=10000 aot_ring_ns=800000 aot_resume_ns=10000 "
            "correctness=1 "
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

    def test_rejects_schema_profile_and_timing_contract_violations(self) -> None:
        complete = bench.format_sample(sample("native"))
        cases = (
            (complete.replace("version=2", "version=1"), "version"),
            (
                complete.replace(
                    "ring_profile=submit_all", "ring_profile=unknown"
                ),
                "ring_profile",
            ),
            (complete.replace(" bind_ns=8000", ""), "missing"),
            (
                bench.format_sample(sample("portable")).replace(
                    "aot_prepare_ns=0", "aot_prepare_ns=1"
                ),
                "portable",
            ),
            (complete.replace("aot_ring_ns=800000", "aot_ring_ns=0"), "native"),
            (
                complete.replace(
                    "aot_resume_ns=10000", "aot_resume_ns=50000"
                ),
                "execute_ns",
            ),
        )
        for line, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    bench.parse_sample(line)


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
        with self.assertRaisesRegex(ValueError, "cell"):
            bench.classify_mechanism(
                [sample("portable", ring_profile="submit_all")],
                [sample("native", ring_profile="coop_taskrun")],
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
            ring_profile="coop_taskrun",
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
                "--ring-profile",
                "coop_taskrun",
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


class ProfileScreenTests(unittest.TestCase):
    @staticmethod
    def _command_record(
        command: list[str],
        *,
        native_cpu_ns: int = 600_000,
        correctness: int = 1,
    ) -> dict[str, object]:
        arguments = {
            command[index]: command[index + 1]
            for index in range(1, len(command), 2)
        }
        candidate = arguments["--candidate"]
        activations = int(arguments["--activations"])
        record = sample(
            candidate,
            ring_profile=arguments["--ring-profile"],
            family=arguments["--family"],
            concurrency=int(arguments["--concurrency"]),
            payload=int(arguments["--payload"]),
            activations=activations,
            correctness=correctness,
            cpu_ns=(900_000 if candidate == "portable" else native_cpu_ns),
            queue_publications=(0 if candidate == "portable" else activations),
            prepared_sqes=activations * 2,
            observed_cqes=(
                activations * 2 if candidate == "portable" else activations
            ),
            suppressed_success_cqes=(
                0 if candidate == "portable" else activations
            ),
            task_parks=activations,
            terminal_wakes=activations,
        )
        return record

    def _run_screen(
        self,
        side_effect: object,
        *,
        profiles: tuple[str, ...] = (
            "submit_all",
            "coop_taskrun",
            "defer_taskrun",
        ),
    ) -> dict[str, object]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            with mock.patch.object(
                bench, "_run_sample", side_effect=side_effect
            ), mock.patch.object(
                bench,
                "_metadata",
                return_value={"schema_version": 2},
            ):
                return bench.run_screen(
                    binary=binary,
                    output_dir=root / "evidence",
                    profiles=profiles,
                    families=("unix",),
                    concurrencies=(1,),
                    payloads=(64,),
                    activations=8,
                    samples=1,
                    timeout_seconds=1,
                )

    def test_all_profiles_continue_independently(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            return self._command_record(command), None

        verdict = self._run_screen(run)
        self.assertEqual(verdict["mechanism_verdict"], "CONTINUE")
        self.assertFalse(verdict["release_authorized"])
        self.assertEqual(
            {name: result["verdict"] for name, result in verdict["profiles"].items()},
            {
                "submit_all": "CONTINUE",
                "coop_taskrun": "CONTINUE",
                "defer_taskrun": "CONTINUE",
            },
        )

    def test_unavailable_profile_does_not_change_control(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            if "coop_taskrun" in command:
                return None, "skip: Operation not supported"
            return self._command_record(command), None

        verdict = self._run_screen(run)
        coop = verdict["profiles"]["coop_taskrun"]
        self.assertEqual(verdict["mechanism_verdict"], "CONTINUE")
        self.assertEqual(coop["capability"], "UNAVAILABLE")
        self.assertEqual(coop["verdict"], "UNAVAILABLE")

    def test_one_sided_unavailability_is_incomplete(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            if "coop_taskrun" in command and "native" in command:
                return None, "skip: Operation not supported"
            return self._command_record(command), None

        verdict = self._run_screen(run)
        coop = verdict["profiles"]["coop_taskrun"]
        self.assertEqual(coop["capability"], "PARTIAL")
        self.assertEqual(coop["verdict"], "INCOMPLETE")
        self.assertEqual(verdict["mechanism_verdict"], "CONTINUE")

    def test_mixed_skip_classes_are_not_unavailable(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            if "coop_taskrun" in command:
                reason = (
                    "skip: Operation not supported"
                    if "portable" in command
                    else "skip: Permission denied"
                )
                return None, reason
            return self._command_record(command), None

        verdict = self._run_screen(run)
        coop = verdict["profiles"]["coop_taskrun"]
        self.assertEqual(coop["capability"], "PARTIAL")
        self.assertEqual(coop["verdict"], "INCOMPLETE")

    def test_correctness_failure_rejects_only_its_profile(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            failed = "defer_taskrun" in command and "native" in command
            return self._command_record(
                command, correctness=0 if failed else 1
            ), None

        verdict = self._run_screen(run)
        self.assertEqual(verdict["profiles"]["defer_taskrun"]["verdict"], "REJECT")
        self.assertEqual(verdict["profiles"]["submit_all"]["verdict"], "CONTINUE")
        self.assertEqual(verdict["mechanism_verdict"], "CONTINUE")

    def test_recommends_by_cpu_then_p99_then_wall_ratio(self) -> None:
        cpu_by_profile = {
            "submit_all": 650_000,
            "coop_taskrun": 500_000,
            "defer_taskrun": 550_000,
        }

        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            profile = command[command.index("--ring-profile") + 1]
            return self._command_record(
                command, native_cpu_ns=cpu_by_profile[profile]
            ), None

        verdict = self._run_screen(run)
        self.assertEqual(verdict["recommended_profile"], "coop_taskrun")
        self.assertLess(
            verdict["profiles"]["coop_taskrun"]["cpu_ratio"],
            verdict["profiles"]["defer_taskrun"]["cpu_ratio"],
        )

    def test_writes_profile_aware_projections_and_refuses_reuse(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            return self._command_record(command), None

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "bench"
            output_dir = root / "evidence"
            binary.write_bytes(b"fixture")
            with mock.patch.object(
                bench, "_run_sample", side_effect=run
            ), mock.patch.object(
                bench,
                "_metadata",
                return_value={"schema_version": 2},
            ):
                verdict = bench.run_screen(
                    binary=binary,
                    output_dir=output_dir,
                    profiles=("submit_all",),
                    families=("unix",),
                    concurrencies=(1,),
                    payloads=(64,),
                    activations=8,
                    samples=1,
                    timeout_seconds=1,
                )
                with self.assertRaises(FileExistsError):
                    bench.run_screen(
                        binary=binary,
                        output_dir=output_dir,
                        profiles=("submit_all",),
                        families=("unix",),
                        concurrencies=(1,),
                        payloads=(64,),
                        activations=8,
                        samples=1,
                        timeout_seconds=1,
                    )

            metadata = json.loads(
                (output_dir / "metadata.json").read_text(encoding="utf-8")
            )
            saved_verdict = json.loads(
                (output_dir / "verdict.json").read_text(encoding="utf-8")
            )
            self.assertEqual(verdict, saved_verdict)
            self.assertEqual(metadata["parameters"]["profiles"], ["submit_all"])
            self.assertIn(
                "ring_profile", (output_dir / "raw.csv").read_text().splitlines()[0]
            )
            self.assertTrue(
                (output_dir / "summary.csv")
                .read_text(encoding="utf-8")
                .splitlines()[0]
                .startswith("ring_profile,")
            )
            self.assertIn(
                "submit_all",
                (output_dir / "summary.md").read_text(encoding="utf-8"),
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
