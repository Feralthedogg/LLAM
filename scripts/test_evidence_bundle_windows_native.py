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
from unittest import mock

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
            incomplete_owned_identity = incomplete._stage_identity
            incomplete.write_bytes("raw.csv", _RAW)
            with self.assertRaises(EvidenceError):
                incomplete.finalize()
            self.assertTrue(incomplete_stage.is_dir())
            self.assertRegex(
                incomplete_stage.name,
                r"^\.incomplete\.staging-\d+-[0-9a-f]{32}$",
            )

            retained_parent = root / "retained-parent"
            nested_parent = retained_parent / "nested"
            nested_parent.mkdir(parents=True)
            moved_parent = root / "retained-parent-moved"
            original_final = nested_parent / "x"
            writer = EvidenceBundle.create(original_final, _metadata())

            def assert_retained_parent_rename_blocked(
                boundary: str,
            ) -> None:
                with self.subTest(boundary=boundary):
                    with self.assertRaises(OSError) as caught:
                        retained_parent.rename(moved_parent)
                    self.assertIn(
                        getattr(caught.exception, "winerror", None),
                        {
                            evidence_bundle._WIN_ERROR_ACCESS_DENIED,
                            32,
                        },
                    )
                    self.assertTrue(retained_parent.is_dir())
                    self.assertFalse(moved_parent.exists())

            assert_retained_parent_rename_blocked("after-create")
            _populate(writer)
            assert_retained_parent_rename_blocked("after-populate")
            self.assertEqual(writer.finalize(), original_final)
            retained_parent.rename(moved_parent)
            self.assertFalse(retained_parent.exists())
            final = moved_parent / "nested" / "x"
            self.assertTrue(final.is_dir())
            result = audit_bundle(final, recompute=_recompute)
            self.assertEqual(result.verdict, "REJECT")
            self.assertIn(
                f"{hashlib.sha256(_RAW).hexdigest()}  raw.csv\n",
                (final / "MANIFEST.sha256").read_text("ascii"),
            )

            api = evidence_bundle._WindowsAPI()
            incomplete_handle = (
                evidence_bundle._win_open_directory(
                    incomplete_stage,
                    api=api,
                    require_private=True,
                )
            )
            try:
                incomplete_identity = api.identity(incomplete_handle)
                self.assertEqual(
                    incomplete_identity,
                    incomplete_owned_identity,
                )
                self.assertEqual(len(incomplete_identity[1]), 16)
            finally:
                api.close(incomplete_handle)
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
                raw_identity = api.identity(raw_handle)
                self.assertEqual(len(raw_identity[1]), 16)
                filesystem_name = api.filesystem_name(raw_handle)
                self.assertIn(
                    filesystem_name,
                    {"NTFS", "ReFS"},
                )
                print(
                    "NATIVE_WINDOWS_EVIDENCE_FILESYSTEM="
                    f"{filesystem_name}"
                )
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
            race_errors: list[BaseException] = []

            def finish(racer: EvidenceBundle) -> None:
                try:
                    barrier.wait(timeout=10)
                    racer.finalize()
                except FileExistsError:
                    outcomes.append("exists")
                except BaseException as exc:
                    race_errors.append(exc)
                else:
                    outcomes.append("won")

            threads = [
                threading.Thread(target=finish, args=(racer,))
                for racer in racers
            ]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(timeout=20)
                self.assertFalse(
                    thread.is_alive(),
                    "native Windows evidence race thread hung",
                )
            self.assertEqual(race_errors, [])
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

            hostile = EvidenceBundle.create(
                root / "hostile",
                _metadata(),
            )
            hostile_stage = hostile._stage_path
            hostile_moved = root / "hostile-owned-stage"
            hostile_owned_identity = hostile._stage_identity
            hostile_stage.rename(hostile_moved)
            hostile_stage.mkdir()
            hostile_foreign = hostile_stage / "foreign-sentinel"
            hostile_foreign.write_bytes(b"foreign")

            with mock.patch.object(
                hostile,
                "_publication_state",
                side_effect=AssertionError(
                    "Windows abort must not classify a mutable pathname"
                ),
            ):
                with self.assertRaises(ValueError):
                    hostile.write_bytes("../invalid", b"x")
            self.assertEqual(hostile_foreign.read_bytes(), b"foreign")
            self.assertTrue((hostile_moved / "metadata.json").is_file())
            hostile_owned_handle = evidence_bundle._win_open_directory(
                hostile_moved,
                api=api,
                require_private=True,
            )
            try:
                self.assertEqual(
                    api.identity(hostile_owned_handle),
                    hostile_owned_identity,
                )
            finally:
                api.close(hostile_owned_handle)

            created_after_error: list[Path] = []
            real_create_directory = (
                evidence_bundle._WindowsAPI.create_directory
            )

            def fail_after_directory_side_effect(
                hooked_api: evidence_bundle._WindowsAPI,
                path: Path,
            ) -> None:
                real_create_directory(hooked_api, path)
                created_after_error.append(path)
                raise OSError(
                    evidence_bundle._WIN_ERROR_ACCESS_DENIED,
                    "injected CreateDirectoryW side-effect error",
                )

            with mock.patch.object(
                evidence_bundle._WindowsAPI,
                "create_directory",
                new=fail_after_directory_side_effect,
            ):
                with self.assertRaises(OSError):
                    EvidenceBundle.create(
                        root / "side-effect",
                        _metadata(),
                    )
            self.assertEqual(len(created_after_error), 1)
            side_effect_stage = created_after_error[0]
            self.assertTrue(side_effect_stage.is_dir())
            side_effect_handle = evidence_bundle._win_open_directory(
                side_effect_stage,
                api=api,
                require_private=True,
            )
            api.close(side_effect_handle)

            identity_calls = 0
            real_identity = evidence_bundle._WindowsAPI.identity

            def fail_stage_identity(
                hooked_api: evidence_bundle._WindowsAPI,
                handle: object,
            ) -> tuple[int, bytes]:
                nonlocal identity_calls
                identity_calls += 1
                if identity_calls == 3:
                    raise OSError(
                        evidence_bundle._WIN_ERROR_ACCESS_DENIED,
                        "injected stage identity failure",
                    )
                return real_identity(hooked_api, handle)

            with mock.patch.object(
                evidence_bundle._WindowsAPI,
                "identity",
                new=fail_stage_identity,
            ):
                with self.assertRaises(OSError):
                    EvidenceBundle.create(
                        root / "identity-failure",
                        _metadata(),
                    )
            identity_stages = list(
                root.glob(".identity-failure.staging-*")
            )
            self.assertEqual(len(identity_stages), 1)
            identity_handle = evidence_bundle._win_open_directory(
                identity_stages[0],
                api=api,
                require_private=True,
            )
            api.close(identity_handle)


if __name__ == "__main__":
    unittest.main()
