#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
"""Native-Windows integration coverage for the evidence publication contract.

Wine is intentionally not an acceptance environment for this suite: it does
not currently preserve SE_DACL_PROTECTED or implement the handle-relative
directory rename contract used here.  scripts/verify_windows.ps1 -Native sets
the explicit gate below on a native Windows CI runner.
"""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

from scripts import evidence_bundle
from scripts.evidence_bundle import (
    EvidenceBundle,
    EvidenceError,
    RecomputedArtifacts,
    audit_bundle,
    canonical_json_bytes,
)


_NATIVE_GATE = (
    sys.platform == "win32"
    and os.environ.get("LLAM_NATIVE_WINDOWS_EVIDENCE") == "1"
)
_RAW = b"value\n1\n"
_SUMMARY = b"value\n1\n"
_VERDICT = canonical_json_bytes(
    {
        "schema": "llam.performance-verdict.v1",
        "verdict": "REJECT",
        "reasons": ["native Windows evidence probe"],
    }
)
_REPORT = b"# Native Windows evidence probe\n"


def _metadata() -> dict[str, object]:
    return {
        "schema": "llam.performance-evidence.v1",
        "source_commit": "1" * 40,
        "source_dirty_digest": "clean",
        "architecture": "x86_64",
        "kernel": "native-windows",
        "toolchain": f"python-{sys.version_info.major}.{sys.version_info.minor}",
        "commands": [["python", "-m", "unittest"]],
        "cpu_policy": {},
        "matrix": {},
        "sample_schedule": {},
        "classifier": {
            "schema": "llam.native-classifier.v1",
            "thresholds": {},
        },
    }


def _populate(writer: EvidenceBundle) -> None:
    writer.write_bytes("raw.csv", _RAW)
    writer.write_bytes("summary.csv", _SUMMARY)
    writer.write_bytes("verdict.json", _VERDICT)
    writer.write_bytes("report.md", _REPORT)


def _recompute(
    raw: bytes,
    metadata: dict[str, object],
) -> RecomputedArtifacts:
    if raw != _RAW or metadata["source_commit"] != "1" * 40:
        raise ValueError("unexpected native Windows evidence input")
    return RecomputedArtifacts(_SUMMARY, _VERDICT, _REPORT)


@unittest.skipUnless(
    _NATIVE_GATE,
    "requires scripts/verify_windows.ps1 -Native on native Windows",
)
class NativeWindowsEvidenceTests(unittest.TestCase):
    def test_acl_sharing_identity_cleanup_no_replace_and_race(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(
            prefix="llam-native-windows-evidence-"
        ) as temporary:
            root = Path(temporary).resolve()

            incomplete_final = root / "incomplete"
            incomplete = EvidenceBundle.create(
                incomplete_final,
                _metadata(),
            )
            incomplete_stage = incomplete._stage_path
            incomplete.write_bytes("raw.csv", _RAW)
            with self.assertRaises(EvidenceError):
                incomplete.finalize()
            self.assertFalse(incomplete_stage.exists())

            final = root / "evidence"
            writer = EvidenceBundle.create(final, _metadata())
            _populate(writer)
            self.assertEqual(writer.finalize(), final)
            result = audit_bundle(final, recompute=_recompute)
            self.assertEqual(result.verdict, "REJECT")
            self.assertIn(
                f"{hashlib.sha256(_RAW).hexdigest()}  raw.csv\n",
                (final / "MANIFEST.sha256").read_text("ascii"),
            )

            api = evidence_bundle._WindowsAPI()
            _, directory_handles = (
                evidence_bundle._win_open_directory_chain(
                    final,
                    api=api,
                    require_private_leaf=True,
                    deny_delete=True,
                )
            )
            evidence_bundle._win_close_handles(
                api,
                directory_handles,
            )
            raw_handle = api.create_file(
                final / "raw.csv",
                creation_disposition=evidence_bundle._WIN_OPEN_EXISTING,
                flags=(
                    evidence_bundle._WIN_FILE_ATTRIBUTE_NORMAL
                    | evidence_bundle._WIN_FILE_FLAG_OPEN_REPARSE_POINT
                ),
                access=(
                    evidence_bundle._WIN_GENERIC_READ
                    | evidence_bundle._WIN_FILE_READ_ATTRIBUTES
                    | evidence_bundle._WIN_READ_CONTROL
                ),
                share=evidence_bundle._WIN_FILE_SHARE_READ,
            )
            try:
                api.require_regular_single_link(raw_handle)
                api.require_private_acl(raw_handle)
            finally:
                api.close(raw_handle)

            with self.assertRaises(FileExistsError):
                EvidenceBundle.create(final, _metadata())

            race_final = root / "race"
            racers = [
                EvidenceBundle.create(race_final, _metadata())
                for _ in range(2)
            ]
            for racer in racers:
                _populate(racer)
            barrier = threading.Barrier(2)
            outcomes: list[str] = []

            def finish(racer: EvidenceBundle) -> None:
                barrier.wait()
                try:
                    racer.finalize()
                except FileExistsError:
                    outcomes.append("exists")
                else:
                    outcomes.append("won")

            threads = [
                threading.Thread(target=finish, args=(racer,))
                for racer in racers
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            self.assertEqual(sorted(outcomes), ["exists", "won"])
            self.assertEqual(
                audit_bundle(race_final, recompute=_recompute).verdict,
                "REJECT",
            )

            external_link = root / "raw-hardlink.csv"
            os.link(final / "raw.csv", external_link)
            try:
                with self.assertRaises(EvidenceError):
                    audit_bundle(final, recompute=_recompute)
            finally:
                external_link.unlink()

            reparse_target = root / "reparse-target"
            reparse_target.mkdir()
            reparse_link = root / "reparse-link"
            junction = subprocess.run(
                [
                    "cmd.exe",
                    "/d",
                    "/c",
                    "mklink",
                    "/J",
                    os.fspath(reparse_link),
                    os.fspath(reparse_target),
                ],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                junction.returncode,
                0,
                junction.stdout + junction.stderr,
            )
            try:
                with self.assertRaises(EvidenceError):
                    EvidenceBundle.create(
                        reparse_link / "bundle",
                        _metadata(),
                    )
            finally:
                reparse_link.rmdir()


if __name__ == "__main__":
    unittest.main()
