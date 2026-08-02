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
    process = str(
        overrides.pop(
            "process",
            "linux" if candidate == "linux" else "portable",
        )
    )
    activations = int(overrides.get("activations", 100))
    compiled = candidate != "oracle"
    record: dict[str, object] = {
        "version": 3,
        "candidate": candidate,
        "transport": "tcp",
        "workload": "connect_write",
        "process": process,
        "ring_profile": (
            "submit_all" if process == "linux" else "portable_control"
        ),
        "block": 0,
        "order": 0 if candidate in {"oracle", "portable"} else 1,
        "seed": 17,
        "concurrency": 4,
        "payload": 64,
        "activations": activations,
        "logical_operations": activations * 2,
        "correctness": 1,
        "result_checksum": "0123456789abcdef",
        "peer_checksum": "fedcba9876543210",
        "wall_ns": {
            "oracle": 2_000_000,
            "portable": 1_800_000,
            "linux": 1_500_000,
        }[candidate],
        "cpu_ns": {
            "oracle": 1_800_000,
            "portable": 1_500_000,
            "linux": 1_100_000,
        }[candidate],
        "p50_ns": {
            "oracle": 20_000,
            "portable": 19_000,
            "linux": 16_000,
        }[candidate],
        "p99_ns": {
            "oracle": 40_000,
            "portable": 38_000,
            "linux": 34_000,
        }[candidate],
        "interpreter_dispatches": 3 * activations if not compiled else 0,
        "normalizations": activations if compiled else 0,
        "site_lookups": activations if compiled else 0,
        "parks": activations * (2 if candidate == "portable" else 1),
        "wakes": activations * (2 if candidate == "portable" else 1),
        "hot_allocations": 0,
    }
    if candidate == "linux":
        record.update(
            {
                "prepared_sqes": activations * 2,
                "observed_cqes": activations,
                "suppressed_success_cqes": activations,
                "queue_publications": activations,
                "submit_syscalls": max(1, activations // 4),
            }
        )
    record.update(overrides)
    return record


def pair_sample(candidate: str, process: str, block: int) -> dict[str, object]:
    first = "oracle" if process == "portable" else "portable"
    first_runs_first = block % 2 == 0
    order = 0 if (candidate == first) == first_runs_first else 1
    return sample(
        candidate,
        process=process,
        ring_profile=(
            "portable_control" if process == "portable" else "submit_all"
        ),
        block=block,
        order=order,
        seed=1000 + block,
    )


class ParseSampleTests(unittest.TestCase):
    def test_parses_candidate_specific_exact_schemas(self) -> None:
        for candidate in ("oracle", "portable", "linux"):
            record = sample(candidate)
            with self.subTest(candidate=candidate):
                self.assertEqual(
                    bench.parse_sample(bench.format_sample(record)), record
                )

    def test_portable_rows_reject_linux_only_fields(self) -> None:
        record = sample("portable")
        record["prepared_sqes"] = 200
        with self.assertRaisesRegex(ValueError, "unexpected"):
            bench.format_sample(record)

    def test_linux_rows_require_every_kernel_field(self) -> None:
        record = sample("linux")
        del record["observed_cqes"]
        with self.assertRaisesRegex(ValueError, "missing"):
            bench.format_sample(record)

    def test_rejects_missing_duplicate_unknown_and_nonfinite_fields(self) -> None:
        complete = bench.format_sample(sample("linux"))
        with self.assertRaisesRegex(ValueError, "missing"):
            bench.parse_sample(complete.replace(" wall_ns=1500000", ""))
        with self.assertRaisesRegex(ValueError, "duplicate"):
            bench.parse_sample(complete + " wall_ns=1")
        with self.assertRaisesRegex(ValueError, "unexpected"):
            bench.parse_sample(complete + " invented=1")
        with self.assertRaisesRegex(ValueError, "integer"):
            bench.parse_sample(
                complete.replace("wall_ns=1500000", "wall_ns=nan")
            )

    def test_rejects_identity_and_counter_domain_violations(self) -> None:
        cases = (
            (sample("portable", version=2), "version"),
            (sample("portable", workload="other"), "workload"),
            (
                sample(
                    "oracle", process="linux", ring_profile="submit_all"
                ),
                "process",
            ),
            (sample("linux", ring_profile="portable_control"), "ring_profile"),
            (sample("portable", result_checksum="ABC"), "checksum"),
            (sample("portable", p50_ns=40_000, p99_ns=30_000), "p50"),
            (sample("portable", correctness=2), "correctness"),
        )
        for record, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    bench.format_sample(record)


class PortableClassifierTests(unittest.TestCase):
    def test_passes_compiled_semantics_and_reports_all_ratios(self) -> None:
        result = bench.classify_portable(
            [pair_sample("oracle", "portable", 0)],
            [pair_sample("portable", "portable", 0)],
        )
        self.assertEqual(result["correctness_verdict"], "PASS")
        self.assertEqual(result["performance_verdict"], "PASS")
        self.assertEqual(result["verdict"], "PASS")
        for name in ("wall_ratio", "cpu_ratio", "p50_ratio", "p99_ratio"):
            self.assertIn(name, result)

    def test_fails_checksum_output_and_ownership_mismatches(self) -> None:
        cases = (
            ("result_checksum", "1111111111111111"),
            ("peer_checksum", "2222222222222222"),
            ("logical_operations", 199),
            ("correctness", 0),
        )
        for field, value in cases:
            portable = pair_sample("portable", "portable", 0)
            portable[field] = value
            with self.subTest(field=field):
                result = bench.classify_portable(
                    [pair_sample("oracle", "portable", 0)], [portable]
                )
                self.assertEqual(result["correctness_verdict"], "FAIL")
                self.assertIn(field, result["reasons"])

    def test_compiled_rows_require_zero_dispatch_and_one_B_per_activation(self) -> None:
        cases = (
            ("interpreter_dispatches", 1),
            ("normalizations", 99),
            ("site_lookups", 99),
            ("hot_allocations", 1),
        )
        for field, value in cases:
            portable = pair_sample("portable", "portable", 0)
            portable[field] = value
            with self.subTest(field=field):
                result = bench.classify_portable(
                    [pair_sample("oracle", "portable", 0)], [portable]
                )
                self.assertEqual(result["correctness_verdict"], "FAIL")
                self.assertIn(field, result["reasons"])

    def test_short_or_dispersed_windows_are_inconclusive_not_incorrect(self) -> None:
        short_oracle = pair_sample("oracle", "portable", 0)
        short_portable = pair_sample("portable", "portable", 0)
        short_oracle["wall_ns"] = 100
        short_portable["wall_ns"] = 90
        short = bench.classify_portable([short_oracle], [short_portable])
        self.assertEqual(short["correctness_verdict"], "PASS")
        self.assertEqual(short["performance_verdict"], "INCONCLUSIVE")
        self.assertIn("minimum_duration", short["performance_reasons"])

        oracle = [pair_sample("oracle", "portable", block) for block in range(5)]
        portable = [
            pair_sample("portable", "portable", block) for block in range(5)
        ]
        for row, ratio in zip(portable, (0.70, 0.80, 0.90, 1.00, 1.10)):
            row["wall_ns"] = round(2_000_000 * ratio)
        dispersed = bench.classify_portable(oracle, portable)
        self.assertEqual(dispersed["correctness_verdict"], "PASS")
        self.assertEqual(dispersed["performance_verdict"], "INCONCLUSIVE")
        self.assertIn("ratio_spread", dispersed["performance_reasons"])


class LinuxClassifierTests(unittest.TestCase):
    def test_passes_only_with_exact_kernel_and_common_receipts(self) -> None:
        result = bench.classify_linux(
            [pair_sample("portable", "linux", 0)],
            [pair_sample("linux", "linux", 0)],
        )
        self.assertEqual(result["correctness_verdict"], "PASS")
        self.assertEqual(result["verdict"], "PASS")

        for field, value in (
            ("prepared_sqes", 199),
            ("observed_cqes", 101),
            ("suppressed_success_cqes", 99),
            ("queue_publications", 99),
            ("submit_syscalls", 0),
            ("normalizations", 99),
            ("interpreter_dispatches", 1),
        ):
            linux = pair_sample("linux", "linux", 0)
            linux[field] = value
            with self.subTest(field=field):
                failed = bench.classify_linux(
                    [pair_sample("portable", "linux", 0)], [linux]
                )
                self.assertEqual(failed["correctness_verdict"], "FAIL")
                self.assertIn(field, failed["reasons"])

    def test_linux_result_cannot_upgrade_failed_portable_axis(self) -> None:
        failed_portable = bench.classify_portable(
            [pair_sample("oracle", "portable", 0)],
            [
                pair_sample(
                    "portable",
                    "portable",
                    0,
                )
            ],
        )
        failed_portable["correctness_verdict"] = "FAIL"
        failed_portable["verdict"] = "FAIL"
        linux = bench.classify_linux(
            [pair_sample("portable", "linux", 0)],
            [pair_sample("linux", "linux", 0)],
        )
        combined = bench.combine_verdicts(failed_portable, linux)
        self.assertEqual(linux["verdict"], "PASS")
        self.assertEqual(combined["overall_verdict"], "FAIL")
        self.assertEqual(combined["linux_effective_verdict"], "BLOCKED")

    def test_performance_failure_does_not_relabel_correctness(self) -> None:
        aggregate = bench._aggregate_cells(
            [
                {
                    "verdict": "FAIL",
                    "correctness_verdict": "PASS",
                    "performance_verdict": "FAIL",
                    "reasons": [],
                    "performance_reasons": ["wall_ns"],
                    "sample_count": 5,
                },
                {
                    "verdict": "INCONCLUSIVE",
                    "correctness_verdict": "PASS",
                    "performance_verdict": "INCONCLUSIVE",
                    "reasons": [],
                    "performance_reasons": ["ratio_spread"],
                    "sample_count": 5,
                },
            ]
        )

        self.assertEqual(aggregate["verdict"], "FAIL")
        self.assertEqual(aggregate["correctness_verdict"], "PASS")


class DriverContractTests(unittest.TestCase):
    def test_balanced_order_is_ABBA_then_BAAB(self) -> None:
        self.assertEqual(
            bench.balanced_order(("oracle", "portable"), 5),
            [
                "oracle",
                "portable",
                "portable",
                "oracle",
                "oracle",
                "portable",
                "portable",
                "oracle",
                "oracle",
                "portable",
            ],
        )

    def test_benchmark_command_carries_pair_identity_and_seed(self) -> None:
        command = bench.benchmark_command(
            Path("/tmp/bench-leir"),
            candidate="linux",
            process="linux",
            ring_profile="coop_taskrun",
            transport="unix",
            block=3,
            order=1,
            seed=99,
            concurrency=16,
            payload=4096,
            activations=512,
        )
        self.assertEqual(
            command,
            [
                str(Path("/tmp/bench-leir").resolve()),
                "--candidate",
                "linux",
                "--process",
                "linux",
                "--ring-profile",
                "coop_taskrun",
                "--transport",
                "unix",
                "--block",
                "3",
                "--order",
                "1",
                "--seed",
                "99",
                "--concurrency",
                "16",
                "--payload",
                "4096",
                "--activations",
                "512",
            ],
        )

    def test_transient_skip_is_not_collapsed_into_unsupported(self) -> None:
        self.assertEqual(
            bench._skip_class("skip: Resource temporarily unavailable"),
            "transient",
        )


class MetadataTests(unittest.TestCase):
    def test_explicit_source_identity_survives_detached_container_mount(self) -> None:
        revision = "5" * 40
        digest = "a" * 64
        failed = bench.subprocess.CompletedProcess([], 1, "", "unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            with mock.patch.dict(
                bench.os.environ,
                {
                    "LLAM_SOURCE_REVISION": revision,
                    "LLAM_SOURCE_TREE_DIGEST_SHA256": digest,
                    "LLAM_SOURCE_TREE_DIRTY": "1",
                },
                clear=False,
            ), mock.patch.object(
                bench.subprocess, "run", return_value=failed
            ), mock.patch.object(
                bench,
                "_source_tree_digest",
                side_effect=AssertionError("override was ignored"),
            ):
                metadata = bench._metadata(binary, root)

        self.assertEqual(metadata["source_revision"], revision)
        self.assertEqual(metadata["source_tree_digest_sha256"], digest)
        self.assertTrue(metadata["source_tree_dirty"])


class ScreenAndAuditTests(unittest.TestCase):
    @staticmethod
    def _command_record(command: list[str]) -> dict[str, object]:
        arguments = {
            command[index]: command[index + 1]
            for index in range(1, len(command), 2)
        }
        candidate = arguments["--candidate"]
        return sample(
            candidate,
            process=arguments["--process"],
            ring_profile=arguments["--ring-profile"],
            transport=arguments["--transport"],
            block=int(arguments["--block"]),
            order=int(arguments["--order"]),
            seed=int(arguments["--seed"]),
            concurrency=int(arguments["--concurrency"]),
            payload=int(arguments["--payload"]),
            activations=int(arguments["--activations"]),
            logical_operations=int(arguments["--activations"]) * 2,
            interpreter_dispatches=(
                int(arguments["--activations"]) * 3
                if candidate == "oracle"
                else 0
            ),
            normalizations=(
                0 if candidate == "oracle" else int(arguments["--activations"])
            ),
            site_lookups=(
                0 if candidate == "oracle" else int(arguments["--activations"])
            ),
            prepared_sqes=(
                int(arguments["--activations"]) * 2
                if candidate == "linux"
                else None
            ),
            observed_cqes=(
                int(arguments["--activations"])
                if candidate == "linux"
                else None
            ),
            suppressed_success_cqes=(
                int(arguments["--activations"])
                if candidate == "linux"
                else None
            ),
            queue_publications=(
                int(arguments["--activations"])
                if candidate == "linux"
                else None
            ),
            submit_syscalls=(1 if candidate == "linux" else None),
        )

    def _run_screen(
        self,
        root: Path,
        side_effect: object,
    ) -> tuple[Path, dict[str, object]]:
        binary = root / "bench"
        output_dir = root / "evidence"
        binary.write_bytes(b"fixture")
        with mock.patch.object(
            bench, "_run_sample", side_effect=side_effect
        ), mock.patch.object(
            bench,
            "_metadata",
            return_value={
                "schema_version": 3,
                "scope": "PORTABLE_AND_LINUX_SEPARATE",
                "release_authorized": False,
                "source_revision": "5" * 40,
                "source_tree_digest_sha256": "a" * 64,
                "source_tree_dirty": True,
                "binary_sha256": "b" * 64,
            },
        ):
            verdict = bench.run_screen(
                binary=binary,
                output_dir=output_dir,
                profiles=("submit_all",),
                transports=("unix",),
                concurrencies=(1,),
                payloads=(64,),
                activations=8,
                samples=1,
                timeout_seconds=1,
            )
        return output_dir, verdict

    def test_writes_independent_axes_and_replays_artifacts(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            record = self._command_record(command)
            record = {key: value for key, value in record.items() if value is not None}
            return record, None

        with tempfile.TemporaryDirectory() as directory:
            output_dir, verdict = self._run_screen(Path(directory), run)
            self.assertEqual(verdict["portable"]["verdict"], "PASS")
            self.assertEqual(
                verdict["linux_profiles"]["submit_all"]["verdict"], "PASS"
            )
            self.assertEqual(verdict["overall_verdict"], "PASS")
            self.assertFalse(verdict["release_authorized"])
            self.assertEqual(bench.audit_screen(output_dir), verdict)
            raw_header = (output_dir / "raw.csv").read_text().splitlines()[0]
            self.assertIn("result_checksum", raw_header)
            self.assertIn("prepared_sqes", raw_header)
            self.assertEqual(
                json.loads((output_dir / "verdict.json").read_text()), verdict
            )

    def test_audit_rejects_a_coordinated_worklist_shrink(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            record = self._command_record(command)
            return {key: value for key, value in record.items() if value is not None}, None

        with tempfile.TemporaryDirectory() as directory:
            output_dir, _ = self._run_screen(Path(directory), run)
            attempts = json.loads((output_dir / "attempts.json").read_text())
            attempts = attempts[:-2]
            verdict, rows = bench._project_attempts(
                attempts,
                profiles=("submit_all",),
            )
            bench._write_projections(output_dir, attempts, verdict, rows)

            with self.assertRaisesRegex(ValueError, "worklist"):
                bench.audit_screen(output_dir)

    def test_linux_unavailable_does_not_erase_portable_result(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            if command[command.index("--candidate") + 1] == "linux":
                return None, "skip: Operation not supported"
            record = self._command_record(command)
            record = {key: value for key, value in record.items() if value is not None}
            return record, None

        with tempfile.TemporaryDirectory() as directory:
            _, verdict = self._run_screen(Path(directory), run)
            self.assertEqual(verdict["portable"]["verdict"], "PASS")
            self.assertEqual(
                verdict["linux_profiles"]["submit_all"]["verdict"],
                "UNAVAILABLE",
            )
            self.assertEqual(verdict["overall_verdict"], "PASS")

    def test_refuses_output_directory_reuse(self) -> None:
        def run(command: list[str], *, timeout_seconds: int) -> object:
            del timeout_seconds
            record = self._command_record(command)
            return {key: value for key, value in record.items() if value is not None}, None

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output_dir, _ = self._run_screen(root, run)
            with self.assertRaises(FileExistsError):
                bench.run_screen(
                    binary=root / "bench",
                    output_dir=output_dir,
                    profiles=("submit_all",),
                    transports=("unix",),
                    concurrencies=(1,),
                    payloads=(64,),
                    activations=8,
                    samples=1,
                    timeout_seconds=1,
                )


class EnforcementTests(unittest.TestCase):
    def test_enforcement_returns_nonzero_for_every_non_pass_decision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "bench"
            binary.write_bytes(b"fixture")
            for verdict_name in ("FAIL", "INCONCLUSIVE"):
                with self.subTest(verdict=verdict_name), mock.patch.object(
                    bench,
                    "run_screen",
                    return_value={"overall_verdict": verdict_name},
                ):
                    rc = bench.main(
                        [
                            "--binary",
                            str(binary),
                            "--output-dir",
                            str(root / verdict_name.lower()),
                            "--profiles",
                            "submit_all",
                            "--transports",
                            "unix",
                            "--concurrency",
                            "1",
                            "--payloads",
                            "64",
                            "--activations",
                            "8",
                            "--samples",
                            "1",
                            "--enforce",
                        ]
                    )
                    self.assertNotEqual(rc, 0)

    def test_nonfinite_mapping_is_rejected_before_classification(self) -> None:
        portable = pair_sample("portable", "portable", 0)
        portable["wall_ns"] = math.inf
        with self.assertRaisesRegex(ValueError, "integer"):
            bench.classify_portable(
                [pair_sample("oracle", "portable", 0)], [portable]
            )


if __name__ == "__main__":
    unittest.main()
