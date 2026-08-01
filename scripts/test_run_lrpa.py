#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import run_lrpa


def valid_document() -> dict[str, object]:
    return {
        "schema_version": 1,
        "status": "ok",
        "seed": 7,
        "lanes": 2,
        "workers": 4,
        "rounds_requested": 3,
        "rounds_completed": 3,
        "coupling": "ring",
        "fault": "none",
        "signature": "0000000000000000",
        "lane_executions": 24,
        "elapsed_ns": 1000,
        "failures": 0,
        "first_failure_round": None,
        "oracle": 0,
        "armed_total": 6,
        "winner_total": 6,
        "cancel_total": 1,
        "timeout_total": 1,
        "discard_total": 18,
        "trace_entries": 20,
        "trace_truncated": False,
        "cleanup_complete": True,
    }


def encode(document: dict[str, object]) -> str:
    return json.dumps(document, separators=(",", ":"), allow_nan=False) + "\n"


class ResultSchemaTests(unittest.TestCase):
    def test_accepts_exact_versioned_document(self) -> None:
        parsed = run_lrpa.parse_result_document(encode(valid_document()))
        self.assertEqual(parsed["seed"], 7)

    def test_rejects_unknown_and_missing_fields(self) -> None:
        unknown = valid_document()
        unknown["extra"] = 1
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(encode(unknown))

        missing = valid_document()
        del missing["winner_total"]
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(encode(missing))

    def test_rejects_invalid_enums_and_non_integer_metrics(self) -> None:
        invalid = valid_document()
        invalid["coupling"] = "central_hotspot"
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(encode(invalid))

        invalid = valid_document()
        invalid["fault"] = "mystery"
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(encode(invalid))

        invalid = valid_document()
        invalid["elapsed_ns"] = 1.5
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(encode(invalid))

        raw_nan = encode(valid_document()).replace("1000", "NaN", 1)
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(raw_nan)

    def test_rejects_partial_multiple_and_signature_mismatch(self) -> None:
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document('{"schema_version":1')
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(
                encode(valid_document()) + encode(valid_document())
            )
        with self.assertRaises(run_lrpa.ResultSchemaError):
            run_lrpa.parse_result_document(
                encode(valid_document()), expected_signature="0123456789abcdef"
            )

    def test_timeout_uses_process_cleanup_helper(self) -> None:
        with self.assertRaises(run_lrpa.SampleTimeout):
            run_lrpa.execute_result_document(
                [sys.executable, "-c", "import time; time.sleep(5)"],
                timeout=0.05,
            )


class RunnerIntegrationTests(unittest.TestCase):
    def test_real_driver_preserves_manifest_and_result(self) -> None:
        binary = os.environ.get("LRPA_BENCH_BINARY")
        if not binary:
            self.skipTest("LRPA_BENCH_BINARY is not configured")
        config = run_lrpa.RunConfig(
            seed=17,
            lanes=2,
            workers=4,
            rounds=4,
            coupling="ring",
            queue_capacity=2048,
            timeout_ms=2000,
            fault="none",
            allowed_outcomes=30,
            generate_perturbations=True,
        )
        with tempfile.TemporaryDirectory() as directory:
            sample = run_lrpa.run_sample(
                pathlib.Path(binary), config, pathlib.Path(directory),
                process_timeout=5.0, replay_failures=True,
            )
            self.assertEqual(sample.document["status"], "ok")
            self.assertTrue((sample.artifact_dir / "manifest.json").is_file())
            self.assertTrue((sample.artifact_dir / "result.json").is_file())
            self.assertTrue((sample.artifact_dir / "trace.bin").is_file())
            self.assertTrue((sample.artifact_dir / "trace.txt").is_file())
            replay_document, _, _ = run_lrpa.execute_result_document(
                [binary, "--manifest",
                 str(sample.artifact_dir / "manifest.json")],
                timeout=5.0,
            )
            self.assertEqual(replay_document["status"], "ok")

    def test_fault_driver_replays_same_signature(self) -> None:
        binary = os.environ.get("LRPA_FAULT_BENCH_BINARY")
        if not binary:
            self.skipTest("LRPA_FAULT_BENCH_BINARY is not configured")
        config = run_lrpa.RunConfig(
            seed=29,
            lanes=4,
            workers=4,
            rounds=4,
            coupling="independent",
            queue_capacity=4096,
            timeout_ms=2000,
            fault="select_skip_winner_cas",
            allowed_outcomes=30,
            generate_perturbations=True,
        )
        with tempfile.TemporaryDirectory() as directory:
            sample = run_lrpa.run_sample(
                pathlib.Path(binary), config, pathlib.Path(directory),
                process_timeout=5.0, replay_failures=True,
            )
            self.assertEqual(sample.document["status"], "oracle_failure")
            self.assertTrue(sample.replayed)
            self.assertTrue((sample.artifact_dir / "replay" / "result.json").is_file())


if __name__ == "__main__":
    unittest.main()
