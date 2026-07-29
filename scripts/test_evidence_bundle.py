#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import errno
import hashlib
import json
import os
import shutil
import stat
import subprocess
import tempfile
import threading
import types
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest import mock

from scripts import evidence_bundle
from scripts.evidence_bundle import (
    MAX_FILE_BYTES,
    AuditResult,
    EvidenceBundle,
    EvidenceError,
    PublicationUncertainError,
    RecomputedArtifacts,
    UnsupportedPlatformError,
    audit_bundle,
)


RAW = b"key,value\nalpha,1\n"
SUMMARY = b"key,median\nalpha,1.0\n"
VERDICT = (
    b'{\n  "reasons": [\n    "threshold missed"\n  ],\n'
    b'  "schema": "llam.performance-verdict.v1",\n'
    b'  "verdict": "REJECT"\n}\n'
)
REPORT = b"# Deterministic report\n\nThe threshold was missed.\n"
SOURCE_COMMIT = "0123456789abcdef0123456789abcdef01234567"
DIRTY_DIGEST = "a" * 64


def _metadata(
    *,
    source_commit: str = SOURCE_COMMIT,
    source_dirty_digest: str = DIRTY_DIGEST,
) -> dict[str, object]:
    return {
        "schema": "llam.performance-evidence.v1",
        "source_commit": source_commit,
        "source_dirty_digest": source_dirty_digest,
        "architecture": "arm64",
        "kernel": "Darwin 25.5.0",
        "toolchain": "clang 18.0.0",
        "commands": [["bench", "--samples", "1"]],
        "cpu_policy": {"affinity": [0]},
        "matrix": {"cells": 1},
        "sample_schedule": {"samples": 1},
        "classifier": {
            "schema": "llam.native-classifier.v1",
            "thresholds": {"wall_ratio_max": 0.95},
        },
    }


def _recompute(
    raw_csv: bytes,
    metadata: dict[str, object],
) -> RecomputedArtifacts:
    if raw_csv != RAW:
        raise ValueError("unexpected raw fixture")
    if metadata["source_commit"] != SOURCE_COMMIT:
        raise ValueError("unexpected provenance")
    return RecomputedArtifacts(
        summary_csv=SUMMARY,
        verdict_json=VERDICT,
        report_md=REPORT,
    )


def _build_bundle(
    root: Path,
    *,
    name: str = "bundle",
    metadata: dict[str, object] | None = None,
) -> Path:
    final = root / name
    bundle = EvidenceBundle.create(final, metadata or _metadata())
    bundle.write_bytes("raw.csv", RAW)
    bundle.write_bytes("summary.csv", SUMMARY)
    bundle.write_bytes("verdict.json", VERDICT)
    bundle.write_bytes("report.md", REPORT)
    return bundle.finalize()


def _manifest_bytes(directory: Path) -> bytes:
    records = []
    for name in (
        "metadata.json",
        "raw.csv",
        "report.md",
        "summary.csv",
        "verdict.json",
    ):
        digest = hashlib.sha256((directory / name).read_bytes()).hexdigest()
        records.append(f"{digest}  {name}\n")
    return "".join(records).encode("ascii")


def _reseal(directory: Path) -> None:
    (directory / "MANIFEST.sha256").write_bytes(
        _manifest_bytes(directory)
    )


def _snapshot(directory: Path) -> dict[str, tuple[int, int, bytes]]:
    result: dict[str, tuple[int, int, bytes]] = {}
    for entry in sorted(directory.iterdir()):
        metadata = entry.lstat()
        result[entry.name] = (
            stat.S_IMODE(metadata.st_mode),
            metadata.st_mtime_ns,
            entry.read_bytes(),
        )
    directory_metadata = directory.lstat()
    result["."] = (
        stat.S_IMODE(directory_metadata.st_mode),
        directory_metadata.st_mtime_ns,
        b"",
    )
    return result


def _run_capture(
    argv: list[str],
    *,
    cwd: Path | None,
    timeout: float,
    max_output_bytes: int,
) -> object:
    completed = subprocess.run(
        argv,
        cwd=cwd,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    return types.SimpleNamespace(
        returncode=completed.returncode,
        stdout=completed.stdout,
        stderr=completed.stderr,
        stdout_truncated=(
            len(completed.stdout.encode("utf-8"))
            > max_output_bytes
        ),
        stderr_truncated=(
            len(completed.stderr.encode("utf-8"))
            > max_output_bytes
        ),
    )


class ProvenanceTests(unittest.TestCase):
    def _repository(self, root: Path) -> None:
        subprocess.run(
            ["git", "init", "-q"],
            cwd=root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.email", "test@example.invalid"],
            cwd=root,
            check=True,
        )
        subprocess.run(
            ["git", "config", "user.name", "Evidence Test"],
            cwd=root,
            check=True,
        )
        (root / "tracked.txt").write_text("before\n")
        subprocess.run(
            ["git", "add", "tracked.txt"],
            cwd=root,
            check=True,
        )
        subprocess.run(
            ["git", "commit", "-qm", "fixture"],
            cwd=root,
            check=True,
        )

    def test_mutation_after_first_tracked_snapshot_never_reports_clean(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self._repository(root)
            tracked_snapshots = 0

            def mutate_after_first_diff(
                argv: list[str],
                **kwargs: object,
            ) -> object:
                nonlocal tracked_snapshots
                result = _run_capture(
                    argv,
                    cwd=kwargs.get("cwd"),  # type: ignore[arg-type]
                    timeout=kwargs["timeout"],  # type: ignore[arg-type]
                    max_output_bytes=kwargs[
                        "max_output_bytes"
                    ],  # type: ignore[arg-type]
                )
                if argv[:3] == ["git", "diff", "--binary"]:
                    tracked_snapshots += 1
                    if tracked_snapshots == 1:
                        (root / "tracked.txt").write_text("after\n")
                return result

            self.assertEqual(
                evidence_bundle.git_source_dirty_digest(
                    mutate_after_first_diff,
                    cwd=root,
                ),
                "unavailable",
            )

    def test_untracked_executable_mode_is_bound_into_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            self._repository(root)
            source = root / "untracked.sh"
            source.write_bytes(b"#!/bin/sh\nexit 0\n")
            source.chmod(0o600)
            non_executable = evidence_bundle.git_source_dirty_digest(
                _run_capture,
                cwd=root,
            )
            source.chmod(0o700)
            executable = evidence_bundle.git_source_dirty_digest(
                _run_capture,
                cwd=root,
            )
            self.assertRegex(non_executable, r"[0-9a-f]{64}\Z")
            self.assertRegex(executable, r"[0-9a-f]{64}\Z")
            self.assertNotEqual(non_executable, executable)


class CreationAndFinalizationTests(unittest.TestCase):
    def test_finalize_rejects_stage_name_substitution_without_publishing_it(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            bundle = EvidenceBundle.create(final, _metadata())
            bundle.write_bytes("raw.csv", RAW)
            bundle.write_bytes("summary.csv", SUMMARY)
            bundle.write_bytes("verdict.json", VERDICT)
            bundle.write_bytes("report.md", REPORT)
            owned = root / "owned-stage"
            bundle._stage_path.rename(owned)
            bundle._stage_path.mkdir(mode=0o700)
            (bundle._stage_path / "attacker").write_bytes(b"foreign")

            with self.assertRaises(
                (EvidenceError, PublicationUncertainError)
            ):
                bundle.finalize()

            self.assertFalse(final.exists())
            self.assertEqual(
                (bundle._stage_path / "attacker").read_bytes(),
                b"foreign",
            )
            self.assertEqual((owned / "raw.csv").read_bytes(), RAW)

    def test_creation_collision_never_removes_foreign_stage_name(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            stage = root / f".bundle.staging-{os.getpid()}-fixed"
            stage.mkdir(mode=0o700)
            with mock.patch.object(
                evidence_bundle.secrets,
                "token_hex",
                return_value="fixed",
            ):
                with self.assertRaises(FileExistsError):
                    EvidenceBundle.create(final, _metadata())
            self.assertTrue(stage.is_dir())

    def test_abort_never_removes_replacement_or_owned_moved_stage(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(root / "bundle", _metadata())
            owned = root / "owned-stage"
            bundle._stage_path.rename(owned)
            bundle._stage_path.mkdir(mode=0o700)
            (bundle._stage_path / "foreign").write_bytes(b"keep")

            with self.assertRaises(ValueError):
                bundle.write_bytes("../escape", b"x")

            self.assertEqual(
                (bundle._stage_path / "foreign").read_bytes(),
                b"keep",
            )
            self.assertTrue((owned / "metadata.json").is_file())

    def test_real_rename_then_base_exception_preserves_published_bundle(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            bundle = EvidenceBundle.create(final, _metadata())
            bundle.write_bytes("raw.csv", RAW)
            bundle.write_bytes("summary.csv", SUMMARY)
            bundle.write_bytes("verdict.json", VERDICT)
            bundle.write_bytes("report.md", REPORT)
            real_rename = evidence_bundle._atomic_rename_noreplace

            def rename_then_interrupt(*args: object) -> None:
                real_rename(*args)  # type: ignore[arg-type]
                raise KeyboardInterrupt("after real rename")

            with mock.patch.object(
                evidence_bundle,
                "_atomic_rename_noreplace",
                side_effect=rename_then_interrupt,
            ):
                with self.assertRaises(PublicationUncertainError):
                    bundle.finalize()

            self.assertEqual(
                {entry.name for entry in final.iterdir()},
                {
                    "raw.csv",
                    "summary.csv",
                    "metadata.json",
                    "verdict.json",
                    "report.md",
                    "MANIFEST.sha256",
                },
            )
            self.assertEqual(
                audit_bundle(final, recompute=_recompute).verdict,
                "REJECT",
            )

    def test_post_rename_state_interrupt_closes_writer_and_preserves_bundle(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            bundle = EvidenceBundle.create(final, _metadata())
            bundle.write_bytes("raw.csv", RAW)
            bundle.write_bytes("summary.csv", SUMMARY)
            bundle.write_bytes("verdict.json", VERDICT)
            bundle.write_bytes("report.md", REPORT)
            stage_fd = bundle._stage_fd
            parent_fd = bundle._parent_fd
            real_publication_state = bundle._publication_state

            def interrupt_after_publication() -> str:
                if final.exists():
                    raise KeyboardInterrupt(
                        "post-rename classification interrupted"
                    )
                return real_publication_state()

            with mock.patch.object(
                bundle,
                "_publication_state",
                side_effect=interrupt_after_publication,
            ):
                with self.assertRaises(PublicationUncertainError):
                    bundle.finalize()

            self.assertFalse(bundle._active)
            for descriptor in (stage_fd, parent_fd):
                with self.assertRaises(OSError):
                    os.fstat(descriptor)
            self.assertEqual(
                audit_bundle(final, recompute=_recompute).verdict,
                "REJECT",
            )

    def test_abort_state_interrupt_does_not_mask_write_error(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(
                root / "bundle",
                _metadata(),
            )
            with mock.patch.object(
                bundle,
                "_write_owned",
                side_effect=OSError(errno.EIO, "primary write failure"),
            ), mock.patch.object(
                bundle,
                "_publication_state",
                side_effect=KeyboardInterrupt(
                    "abort classification interrupted"
                ),
            ):
                with self.assertRaisesRegex(
                    OSError,
                    "primary write failure",
                ):
                    bundle.write_bytes("raw.csv", RAW)
            self.assertFalse(bundle._active)

    def test_rejects_parent_without_trusted_rename_authority(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            root.chmod(0o777)
            with self.assertRaisesRegex(
                EvidenceError,
                "rename authority",
            ):
                EvidenceBundle.create(root / "bundle", _metadata())
            self.assertEqual(list(root.iterdir()), [])

    def test_parent_rename_authority_accepts_safe_common_modes(
        self,
    ) -> None:
        for mode in (0o700, 0o755, 0o1770):
            with self.subTest(mode=oct(mode)), tempfile.TemporaryDirectory(
            ) as temporary:
                root = Path(temporary).resolve()
                root.chmod(mode)
                bundle = EvidenceBundle.create(
                    root / "bundle",
                    _metadata(),
                )
                with self.assertRaises(ValueError):
                    bundle.write_bytes("../abort", b"x")
                self.assertFalse(bundle._stage_path.exists())

    def test_parent_rename_authority_rejects_nonsticky_writers(
        self,
    ) -> None:
        for mode in (0o775, 0o777):
            with self.subTest(mode=oct(mode)), tempfile.TemporaryDirectory(
            ) as temporary:
                root = Path(temporary).resolve()
                root.chmod(mode)
                with self.assertRaisesRegex(
                    EvidenceError,
                    "rename authority",
                ):
                    EvidenceBundle.create(
                        root / "bundle",
                        _metadata(),
                    )

    def test_publication_state_parent_path_close_failure_is_ambiguous(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(
                root / "bundle",
                _metadata(),
            )
            path_parent_fd = os.open(root, os.O_RDONLY)
            real_close = os.close

            def close_with_error(descriptor: int) -> None:
                real_close(descriptor)
                if descriptor == path_parent_fd:
                    raise OSError(
                        errno.EIO,
                        "path parent close failed",
                    )

            try:
                with mock.patch.object(
                    evidence_bundle,
                    "_open_directory_nofollow",
                    return_value=path_parent_fd,
                ), mock.patch.object(
                    evidence_bundle.os,
                    "close",
                    side_effect=close_with_error,
                ):
                    self.assertEqual(
                        bundle._publication_state(),
                        evidence_bundle._AMBIGUOUS,
                    )
            finally:
                bundle._abort()

    def test_publication_state_close_failure_does_not_mask_interrupt(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(
                root / "bundle",
                _metadata(),
            )
            path_parent_fd = os.open(root, os.O_RDONLY)
            real_close = os.close
            real_fstat = os.fstat

            def inspect_with_interrupt(
                descriptor: int,
            ) -> os.stat_result:
                if descriptor == path_parent_fd:
                    raise KeyboardInterrupt(
                        "path parent inspection interrupted"
                    )
                return real_fstat(descriptor)

            def close_with_error(descriptor: int) -> None:
                real_close(descriptor)
                if descriptor == path_parent_fd:
                    raise OSError(
                        errno.EIO,
                        "path parent close failed",
                    )

            try:
                with mock.patch.object(
                    evidence_bundle,
                    "_open_directory_nofollow",
                    return_value=path_parent_fd,
                ), mock.patch.object(
                    evidence_bundle.os,
                    "fstat",
                    side_effect=inspect_with_interrupt,
                ), mock.patch.object(
                    evidence_bundle.os,
                    "close",
                    side_effect=close_with_error,
                ):
                    with self.assertRaisesRegex(
                        KeyboardInterrupt,
                        "inspection interrupted",
                    ):
                        bundle._publication_state()
            finally:
                bundle._abort()

    def test_parent_fsync_failure_reports_publication_uncertain_and_auditable(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            bundle = EvidenceBundle.create(final, _metadata())
            bundle.write_bytes("raw.csv", RAW)
            bundle.write_bytes("summary.csv", SUMMARY)
            bundle.write_bytes("verdict.json", VERDICT)
            bundle.write_bytes("report.md", REPORT)
            with mock.patch.object(
                evidence_bundle,
                "_fsync_directory",
                side_effect=[
                    None,
                    None,
                    OSError(errno.EIO, "parent fsync failed"),
                ],
            ):
                with self.assertRaises(
                    PublicationUncertainError
                ) as caught:
                    bundle.finalize()
            self.assertEqual(caught.exception.final_path, final)
            self.assertIn("audit", str(caught.exception))
            self.assertTrue(final.is_dir())
            self.assertEqual(
                audit_bundle(final, recompute=_recompute).verdict,
                "REJECT",
            )
            before_retry = _snapshot(final)
            with self.assertRaises(RuntimeError):
                bundle.finalize()
            self.assertEqual(before_retry, _snapshot(final))
            self.assertEqual(list(root.glob(".bundle.staging-*")), [])

    def test_post_rename_stage_close_failure_preserves_final_bundle(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            bundle = EvidenceBundle.create(final, _metadata())
            bundle.write_bytes("raw.csv", RAW)
            bundle.write_bytes("summary.csv", SUMMARY)
            bundle.write_bytes("verdict.json", VERDICT)
            bundle.write_bytes("report.md", REPORT)
            stage_fd = bundle._stage_fd
            real_close = os.close

            def injected_close(descriptor: int) -> None:
                if descriptor == stage_fd and final.exists():
                    real_close(descriptor)
                    raise OSError(errno.EIO, "stage close failed")
                real_close(descriptor)

            with mock.patch.object(
                evidence_bundle.os,
                "close",
                side_effect=injected_close,
            ):
                with self.assertRaises(PublicationUncertainError):
                    bundle.finalize()
            self.assertTrue(final.is_dir())
            self.assertEqual(
                audit_bundle(final, recompute=_recompute).verdict,
                "REJECT",
            )
            with self.assertRaises(RuntimeError):
                bundle.finalize()

    def test_post_rename_parent_close_failure_is_publication_uncertain(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            bundle = EvidenceBundle.create(final, _metadata())
            bundle.write_bytes("raw.csv", RAW)
            bundle.write_bytes("summary.csv", SUMMARY)
            bundle.write_bytes("verdict.json", VERDICT)
            bundle.write_bytes("report.md", REPORT)
            parent_fd = bundle._parent_fd
            real_close = os.close

            def injected_close(descriptor: int) -> None:
                real_close(descriptor)
                if descriptor == parent_fd and final.exists():
                    raise OSError(errno.EIO, "parent close failed")

            with mock.patch.object(
                evidence_bundle.os,
                "close",
                side_effect=injected_close,
            ):
                with self.assertRaises(PublicationUncertainError):
                    bundle.finalize()
            self.assertTrue(final.is_dir())
            self.assertEqual(
                audit_bundle(final, recompute=_recompute).verdict,
                "REJECT",
            )

    def test_final_bundle_has_exact_names_hashes_and_private_modes(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(Path(temporary).resolve())
            self.assertEqual(
                {entry.name for entry in final.iterdir()},
                {
                    "raw.csv",
                    "summary.csv",
                    "metadata.json",
                    "verdict.json",
                    "report.md",
                    "MANIFEST.sha256",
                },
            )
            self.assertEqual(
                stat.S_IMODE(final.stat().st_mode),
                0o700,
            )
            for entry in final.iterdir():
                self.assertTrue(entry.is_file())
                self.assertEqual(
                    stat.S_IMODE(entry.stat().st_mode),
                    0o600,
                )
            self.assertEqual(
                (final / "MANIFEST.sha256").read_bytes(),
                _manifest_bytes(final),
            )

    def test_refuses_existing_empty_or_nonempty_destination(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name, nonempty in (("empty", False), ("full", True)):
                final = root / name
                final.mkdir()
                if nonempty:
                    (final / "sentinel").write_text("keep")
                with self.subTest(nonempty=nonempty):
                    with self.assertRaises(FileExistsError):
                        EvidenceBundle.create(final, _metadata())
                if nonempty:
                    self.assertEqual(
                        (final / "sentinel").read_text(),
                        "keep",
                    )

    def test_atomic_finalize_does_not_replace_destination_created_late(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for label, nonempty in (("empty", False), ("full", True)):
                final = root / f"bundle-{label}"
                bundle = EvidenceBundle.create(final, _metadata())
                bundle.write_bytes("raw.csv", RAW)
                bundle.write_bytes("summary.csv", SUMMARY)
                bundle.write_bytes("verdict.json", VERDICT)
                bundle.write_bytes("report.md", REPORT)
                final.mkdir()
                if nonempty:
                    (final / "sentinel").write_text("keep")
                with self.subTest(nonempty=nonempty):
                    with self.assertRaises(FileExistsError):
                        bundle.finalize()
                    self.assertTrue(final.is_dir())
                    if nonempty:
                        self.assertEqual(
                            (final / "sentinel").read_text(),
                            "keep",
                        )
                self.assertEqual(
                    list(root.glob(f".bundle-{label}.staging-*")),
                    [],
                )

    def test_two_concurrent_finalizers_have_exactly_one_winner(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = root / "bundle"
            barrier = threading.Barrier(2)

            def create_and_finalize(label: str) -> str:
                metadata = _metadata()
                metadata["commands"] = [["bench", label]]
                bundle = EvidenceBundle.create(final, metadata)
                bundle.write_bytes("raw.csv", RAW)
                bundle.write_bytes("summary.csv", SUMMARY)
                bundle.write_bytes("verdict.json", VERDICT)
                bundle.write_bytes("report.md", REPORT)
                barrier.wait(timeout=5)
                try:
                    bundle.finalize()
                except FileExistsError:
                    return "lost"
                return "won"

            with ThreadPoolExecutor(max_workers=2) as executor:
                outcomes = list(
                    executor.map(create_and_finalize, ("one", "two"))
                )
            self.assertEqual(sorted(outcomes), ["lost", "won"])
            self.assertEqual(
                list(root.glob(".bundle.staging-*")),
                [],
            )
            self.assertEqual(
                {entry.name for entry in final.iterdir()},
                {
                    "raw.csv",
                    "summary.csv",
                    "metadata.json",
                    "verdict.json",
                    "report.md",
                    "MANIFEST.sha256",
                },
            )

    def test_staging_and_entry_creation_are_exclusive_and_private(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(
                root / "bundle",
                _metadata(),
            )
            stages = list(root.glob(".bundle.staging-*"))
            self.assertEqual(len(stages), 1)
            self.assertEqual(
                stat.S_IMODE(stages[0].stat().st_mode),
                0o700,
            )
            bundle.write_bytes("raw.csv", RAW)
            self.assertEqual(
                stat.S_IMODE((stages[0] / "raw.csv").stat().st_mode),
                0o600,
            )
            with self.assertRaises(EvidenceError):
                bundle.write_bytes("raw.csv", RAW)
            self.assertFalse(stages[0].exists())

    def test_rejects_unsafe_unknown_and_library_owned_names(self) -> None:
        bad_names = (
            "/absolute",
            "../escape",
            "sub/file",
            "sub\\file",
            ".",
            "..",
            "bad\x00name",
            "bad\nname",
            "unknown.txt",
            "metadata.json",
            "MANIFEST.sha256",
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for index, name in enumerate(bad_names):
                with self.subTest(name=repr(name)):
                    bundle = EvidenceBundle.create(
                        root / f"bundle-{index}",
                        _metadata(),
                    )
                    with self.assertRaises((EvidenceError, ValueError)):
                        bundle.write_bytes(name, b"x")
                    self.assertEqual(
                        list(root.glob(f".bundle-{index}.staging-*")),
                        [],
                    )

    def test_partial_write_failure_cleans_only_own_staging_directory(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            foreign = root / ".bundle.staging-foreign"
            foreign.mkdir()
            (foreign / "sentinel").write_text("keep")
            bundle = EvidenceBundle.create(
                root / "bundle",
                _metadata(),
            )
            with mock.patch.object(
                evidence_bundle.os,
                "write",
                side_effect=[1, OSError("injected write failure")],
            ):
                with self.assertRaises(OSError):
                    bundle.write_bytes("raw.csv", RAW)
            self.assertEqual((foreign / "sentinel").read_text(), "keep")
            self.assertEqual(
                [
                    path.name
                    for path in root.glob(".bundle.staging-*")
                ],
                [foreign.name],
            )

    def test_artifact_close_failure_is_not_masked_by_double_close(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(root / "bundle", _metadata())
            protected = {bundle._stage_fd, bundle._parent_fd}
            real_close = os.close
            injected = False

            def close_once(descriptor: int) -> None:
                nonlocal injected
                if not injected and descriptor not in protected:
                    injected = True
                    real_close(descriptor)
                    raise OSError(errno.EIO, "artifact close failed")
                real_close(descriptor)

            with mock.patch.object(
                evidence_bundle.os,
                "close",
                side_effect=close_once,
            ):
                with self.assertRaisesRegex(
                    OSError,
                    "artifact close failed",
                ):
                    bundle.write_bytes("raw.csv", RAW)
            self.assertEqual(
                list(root.glob(".bundle.staging-*")),
                [],
            )

    def test_metadata_write_failure_does_not_mask_error_or_leak_stage(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            with mock.patch.object(
                evidence_bundle.os,
                "write",
                side_effect=OSError(errno.EIO, "injected metadata write"),
            ):
                with self.assertRaisesRegex(
                    OSError,
                    "injected metadata write",
                ):
                    EvidenceBundle.create(
                        root / "bundle",
                        _metadata(),
                    )
            self.assertEqual(
                list(root.glob(".bundle.staging-*")),
                [],
            )

    def test_missing_artifact_failure_cleans_and_finalize_is_single_use(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            bundle = EvidenceBundle.create(
                root / "bundle",
                _metadata(),
            )
            bundle.write_bytes("raw.csv", RAW)
            with self.assertRaises(EvidenceError):
                bundle.finalize()
            with self.assertRaises(RuntimeError):
                bundle.finalize()
            self.assertEqual(
                list(root.glob(".bundle.staging-*")),
                [],
            )

    def test_rejects_symlink_or_non_directory_ancestor(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            real = root / "real"
            real.mkdir()
            link = root / "link"
            link.symlink_to(real, target_is_directory=True)
            with self.assertRaises(EvidenceError):
                EvidenceBundle.create(link / "bundle", _metadata())
            regular = root / "regular"
            regular.write_text("not a directory")
            with self.assertRaises(EvidenceError):
                EvidenceBundle.create(regular / "bundle", _metadata())

    def test_fails_closed_when_no_follow_is_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch.object(
                evidence_bundle.os,
                "O_NOFOLLOW",
                0,
            ):
                with self.assertRaises(UnsupportedPlatformError):
                    EvidenceBundle.create(
                        Path(temporary).resolve() / "bundle",
                        _metadata(),
                    )


class MetadataValidationTests(unittest.TestCase):
    def test_deep_json_is_rejected_with_controlled_evidence_error(
        self,
    ) -> None:
        deep: object = "leaf"
        for _ in range(2000):
            deep = {"nested": deep}
        metadata = _metadata()
        metadata["matrix"] = {"cells": deep}
        with self.assertRaises(EvidenceError):
            evidence_bundle.canonical_json_bytes(metadata)
        with self.assertRaises(EvidenceError):
            evidence_bundle._validate_metadata(metadata)

        encoded = (
            b'{"nested":' * 2000
            + b"null"
            + b"}" * 2000
        )
        with self.assertRaises(EvidenceError):
            evidence_bundle._strict_json_object(
                encoded,
                where="deep.json",
            )

    def test_json_node_complexity_is_bounded(self) -> None:
        value = {
            "items": [
                index
                for index in range(
                    evidence_bundle.MAX_JSON_NODES + 1
                )
            ]
        }
        with self.assertRaisesRegex(EvidenceError, "complex"):
            evidence_bundle.canonical_json_bytes(value)

    def test_rejects_unknown_missing_or_invalid_metadata_fields(
        self,
    ) -> None:
        cases: list[dict[str, object]] = []
        unknown = _metadata()
        unknown["extra"] = True
        cases.append(unknown)
        missing = _metadata()
        del missing["kernel"]
        cases.append(missing)
        cases.extend(
            [
                _metadata(source_commit="ABC"),
                _metadata(source_dirty_digest="dirty"),
                {**_metadata(), "architecture": "AMD64"},
                {**_metadata(), "kernel": ""},
                {**_metadata(), "toolchain": ""},
                {**_metadata(), "commands": ["shell command"]},
                {**_metadata(), "cpu_policy": []},
                {**_metadata(), "matrix": []},
                {**_metadata(), "sample_schedule": []},
                {
                    **_metadata(),
                    "classifier": {
                        "schema": "unknown",
                        "thresholds": {},
                    },
                },
                {
                    **_metadata(),
                    "classifier": {
                        "schema": "llam.native-classifier.v1",
                        "thresholds": {},
                        "extra": True,
                    },
                },
                {
                    **_metadata(),
                    "classifier": {
                        "schema": "llam.native-classifier.v1",
                        "thresholds": {"bad": float("nan")},
                    },
                },
            ]
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for index, metadata in enumerate(cases):
                with self.subTest(index=index):
                    with self.assertRaises(EvidenceError):
                        EvidenceBundle.create(
                            root / f"bundle-{index}",
                            metadata,
                        )
            self.assertEqual(list(root.iterdir()), [])

    def test_accepts_clean_dirty_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(
                Path(temporary).resolve(),
                metadata=_metadata(source_dirty_digest="clean"),
            )
            observed = json.loads(
                (final / "metadata.json").read_text("utf-8")
            )
            self.assertEqual(observed["source_dirty_digest"], "clean")


class AuditTests(unittest.TestCase):
    def test_audit_requires_trusted_immediate_parent_modes(
        self,
    ) -> None:
        for mode, accepted in (
            (0o700, True),
            (0o755, True),
            (0o1770, True),
            (0o775, False),
            (0o777, False),
        ):
            with self.subTest(mode=oct(mode)), tempfile.TemporaryDirectory(
            ) as temporary:
                root = Path(temporary).resolve()
                final = _build_bundle(root)
                root.chmod(mode)
                if accepted:
                    self.assertEqual(
                        audit_bundle(
                            final,
                            recompute=_recompute,
                        ).verdict,
                        "REJECT",
                    )
                else:
                    with self.assertRaisesRegex(
                        EvidenceError,
                        "rename authority",
                    ):
                        audit_bundle(final, recompute=_recompute)

    def test_audit_revalidates_parent_authority_before_success(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = _build_bundle(root)

            def relax_parent(
                raw_csv: bytes,
                metadata: dict[str, object],
            ) -> RecomputedArtifacts:
                root.chmod(0o777)
                return _recompute(raw_csv, metadata)

            with self.assertRaisesRegex(
                EvidenceError,
                "rename authority",
            ):
                audit_bundle(final, recompute=relax_parent)

    def test_audit_rejects_artifact_mutation_during_recompute(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(Path(temporary).resolve())

            def mutate(
                raw_csv: bytes,
                metadata: dict[str, object],
            ) -> RecomputedArtifacts:
                (final / "raw.csv").write_bytes(
                    b"key,value\nalpha,9\n"
                )
                return _recompute(raw_csv, metadata)

            with self.assertRaisesRegex(
                EvidenceError,
                "changed",
            ):
                audit_bundle(final, recompute=mutate)

    def test_audit_rejects_bundle_path_replacement_during_recompute(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = _build_bundle(root)
            moved = root / "moved"

            def replace_bundle(
                raw_csv: bytes,
                metadata: dict[str, object],
            ) -> RecomputedArtifacts:
                final.rename(moved)
                shutil.copytree(moved, final)
                return _recompute(raw_csv, metadata)

            with self.assertRaisesRegex(
                EvidenceError,
                "changed|path|identity",
            ):
                audit_bundle(final, recompute=replace_bundle)

    def test_directory_close_failure_does_not_mask_primary_error(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(Path(temporary).resolve())
            parent_fd = os.open(final.parent, os.O_RDONLY)
            directory_fd = os.open(final, os.O_RDONLY)
            real_close = os.close
            real_open = os.open

            def open_bundle_leaf(
                path: object,
                flags: int,
                mode: int = 0o777,
                *,
                dir_fd: int | None = None,
            ) -> int:
                if path == final.name and dir_fd == parent_fd:
                    return directory_fd
                return real_open(
                    path,
                    flags,
                    mode,
                    dir_fd=dir_fd,
                )

            def close_with_error(descriptor: int) -> None:
                real_close(descriptor)
                if descriptor == directory_fd:
                    raise OSError(errno.EIO, "directory close failed")

            with mock.patch.object(
                evidence_bundle,
                "_open_directory_nofollow",
                return_value=parent_fd,
            ), mock.patch.object(
                evidence_bundle.os,
                "open",
                side_effect=open_bundle_leaf,
            ), mock.patch.object(
                evidence_bundle.os,
                "listdir",
                side_effect=EvidenceError("primary validation failure"),
            ), mock.patch.object(
                evidence_bundle.os,
                "close",
                side_effect=close_with_error,
            ):
                with self.assertRaisesRegex(
                    EvidenceError,
                    "primary validation failure",
                ):
                    audit_bundle(final, recompute=_recompute)

    def test_read_validation_error_is_not_masked_by_close_failure(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(Path(temporary).resolve())
            directory_fd = os.open(final, os.O_RDONLY)
            real_close = os.close

            def close_with_error(descriptor: int) -> None:
                real_close(descriptor)
                if descriptor != directory_fd:
                    raise OSError(errno.EIO, "read handle close failed")

            try:
                with mock.patch.object(
                    evidence_bundle.os,
                    "read",
                    return_value=b"",
                ), mock.patch.object(
                    evidence_bundle.os,
                    "close",
                    side_effect=close_with_error,
                ):
                    with self.assertRaisesRegex(
                        EvidenceError,
                        "changed while being read",
                    ):
                        evidence_bundle._read_regular_file(
                            directory_fd,
                            "raw.csv",
                        )
            finally:
                real_close(directory_fd)

    def test_valid_reject_bundle_audits_without_writes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(Path(temporary).resolve())
            before = _snapshot(final)
            result = audit_bundle(
                final,
                recompute=_recompute,
                required_source_commit=SOURCE_COMMIT,
                required_source_dirty_digest=DIRTY_DIGEST,
            )
            self.assertIsInstance(result, AuditResult)
            self.assertEqual(result.verdict, "REJECT")
            self.assertEqual(result.reasons, ("threshold missed",))
            self.assertEqual(before, _snapshot(final))

    def test_rejects_content_tamper_and_provenance_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            tampered = _build_bundle(root, name="tampered")
            (tampered / "raw.csv").write_bytes(
                b"key,value\nalpha,2\n"
            )
            with self.assertRaises(EvidenceError):
                audit_bundle(tampered, recompute=_recompute)

            valid = _build_bundle(root, name="valid")
            with self.assertRaises(EvidenceError):
                audit_bundle(
                    valid,
                    recompute=_recompute,
                    required_source_commit="f" * 40,
                )
            with self.assertRaises(EvidenceError):
                audit_bundle(
                    valid,
                    recompute=_recompute,
                    required_source_dirty_digest="clean",
                )

    def test_rejects_manifest_grammar_order_duplicates_and_paths(
        self,
    ) -> None:
        mutations = {
            "uppercase": lambda lines: [
                lines[0].upper(),
                *lines[1:],
            ],
            "out-of-order": lambda lines: [
                lines[1],
                lines[0],
                *lines[2:],
            ],
            "duplicate": lambda lines: [lines[0], *lines],
            "unknown": lambda lines: [
                *lines,
                f"{'0' * 64}  unknown.txt\n",
            ],
            "separator": lambda lines: [
                f"{'0' * 64}  ../raw.csv\n",
                *lines[1:],
            ],
            "missing": lambda lines: lines[:-1],
            "bad-spacing": lambda lines: [
                lines[0].replace("  ", " ", 1),
                *lines[1:],
            ],
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name, mutate in mutations.items():
                final = _build_bundle(root, name=name)
                lines = (
                    final / "MANIFEST.sha256"
                ).read_text("ascii").splitlines(keepends=True)
                (final / "MANIFEST.sha256").write_text(
                    "".join(mutate(lines)),
                    encoding="ascii",
                )
                with self.subTest(name=name):
                    with self.assertRaises(EvidenceError):
                        audit_bundle(final, recompute=_recompute)

    def test_rejects_unknown_missing_symlink_and_hardlinked_entries(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            unknown = _build_bundle(root, name="unknown")
            (unknown / "extra").write_text("x")
            with self.assertRaises(EvidenceError):
                audit_bundle(unknown, recompute=_recompute)

            missing = _build_bundle(root, name="missing")
            (missing / "report.md").unlink()
            with self.assertRaises(EvidenceError):
                audit_bundle(missing, recompute=_recompute)

            symlink = _build_bundle(root, name="symlink")
            (symlink / "raw.csv").unlink()
            (symlink / "raw.csv").symlink_to(root / "outside")
            with self.assertRaises(EvidenceError):
                audit_bundle(symlink, recompute=_recompute)

            hardlink = _build_bundle(root, name="hardlink")
            os.link(hardlink / "raw.csv", root / "raw-hardlink")
            with self.assertRaises(EvidenceError):
                audit_bundle(hardlink, recompute=_recompute)

    def test_rejects_symlink_ancestor_and_partial_directories(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            (root / "real").mkdir()
            final = _build_bundle(root / "real")
            link = root / "link"
            link.symlink_to(root / "real", target_is_directory=True)
            with self.assertRaises(EvidenceError):
                audit_bundle(link / "bundle", recompute=_recompute)

            partial = root / "partial"
            partial.mkdir()
            (partial / "raw.csv").write_bytes(RAW)
            with self.assertRaises(EvidenceError):
                audit_bundle(partial, recompute=_recompute)

            writer = EvidenceBundle.create(
                root / "unfinished",
                _metadata(),
            )
            stage = next(root.glob(".unfinished.staging-*"))
            with self.assertRaises(EvidenceError):
                audit_bundle(stage, recompute=_recompute)
            with self.assertRaises(EvidenceError):
                writer.finalize()

    def test_rejects_unknown_json_schemas_fields_and_noncanonical_bytes(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            mutations = (
                (
                    "metadata-schema",
                    "metadata.json",
                    lambda value: {**value, "schema": "unknown"},
                ),
                (
                    "metadata-field",
                    "metadata.json",
                    lambda value: {**value, "extra": True},
                ),
                (
                    "verdict-schema",
                    "verdict.json",
                    lambda value: {**value, "schema": "unknown"},
                ),
                (
                    "verdict-field",
                    "verdict.json",
                    lambda value: {**value, "extra": True},
                ),
            )
            for name, filename, mutate in mutations:
                final = _build_bundle(root, name=name)
                path = final / filename
                value = json.loads(path.read_text("utf-8"))
                path.write_text(
                    json.dumps(mutate(value), sort_keys=True) + "\n",
                    encoding="utf-8",
                )
                _reseal(final)
                with self.subTest(name=name):
                    with self.assertRaises(EvidenceError):
                        audit_bundle(final, recompute=_recompute)

            noncanonical = _build_bundle(root, name="noncanonical")
            value = json.loads(
                (noncanonical / "verdict.json").read_text("utf-8")
            )
            (noncanonical / "verdict.json").write_text(
                json.dumps(value, separators=(",", ":")),
                encoding="utf-8",
            )
            _reseal(noncanonical)
            with self.assertRaises(EvidenceError):
                audit_bundle(noncanonical, recompute=_recompute)

    def test_rejects_blank_duplicate_nonfinite_and_malformed_csv_cells(
        self,
    ) -> None:
        bad_raws = {
            "blank": b"key,value\nalpha,\n",
            "whitespace": b"key,value\nalpha,   \n",
            "duplicate-key": b"key,key\nalpha,1\n",
            "nan": b"key,value\nalpha,NaN\n",
            "inf": b"key,value\nalpha,-Inf\n",
            "missing": b"key,value\nalpha\n",
            "extra": b"key,value\nalpha,1,extra\n",
            "invalid-utf8": b"key,value\nalpha,\xff\n",
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name, raw in bad_raws.items():
                final = _build_bundle(root, name=name)
                (final / "raw.csv").write_bytes(raw)
                _reseal(final)
                with self.subTest(name=name):
                    with self.assertRaises(EvidenceError):
                        audit_bundle(final, recompute=_recompute)

    def test_rejects_oversized_file_before_reading(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            final = _build_bundle(Path(temporary).resolve())
            with (final / "raw.csv").open("r+b") as stream:
                stream.truncate(MAX_FILE_BYTES + 1)
            with self.assertRaises(EvidenceError):
                audit_bundle(final, recompute=_recompute)

    def test_rejects_public_directory_or_file_modes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            public_file = _build_bundle(root, name="public-file")
            (public_file / "raw.csv").chmod(0o644)
            with self.assertRaises(EvidenceError):
                audit_bundle(public_file, recompute=_recompute)

            public_directory = _build_bundle(
                root,
                name="public-directory",
            )
            public_directory.chmod(0o755)
            with self.assertRaises(EvidenceError):
                audit_bundle(
                    public_directory,
                    recompute=_recompute,
                )

    def test_rejects_noncanonical_or_invalid_recompute_result(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            final = _build_bundle(root, name="wrong-bytes")

            def wrong(
                raw_csv: bytes,
                metadata: dict[str, object],
            ) -> RecomputedArtifacts:
                return RecomputedArtifacts(
                    summary_csv=b"key,median\nalpha,2.0\n",
                    verdict_json=VERDICT,
                    report_md=REPORT,
                )

            with self.assertRaises(EvidenceError):
                audit_bundle(final, recompute=wrong)

            invalid_type = _build_bundle(root, name="wrong-type")
            with self.assertRaises(EvidenceError):
                audit_bundle(
                    invalid_type,
                    recompute=lambda raw, metadata: (  # type: ignore[arg-type]
                        SUMMARY,
                        VERDICT,
                        REPORT,
                    ),
                )


class AtomicPrimitiveTests(unittest.TestCase):
    def test_windows_handle_bound_rename_uses_no_replace_and_parent_handle(
        self,
    ) -> None:
        calls: list[tuple[object, ...]] = []

        class Kernel32:
            @staticmethod
            def SetFileInformationByHandle(*args: object) -> int:
                calls.append(args)
                return 1

        api = object.__new__(evidence_bundle._WindowsAPI)
        api._kernel32 = Kernel32()  # type: ignore[attr-defined]
        api.rename_handle_noreplace(0x1234, 0x5678, "final")

        self.assertEqual(len(calls), 1)
        handle, information_class, buffer, size = calls[0]
        self.assertEqual(handle, 0x1234)
        self.assertEqual(
            information_class,
            evidence_bundle._WIN_FILE_RENAME_INFO_EX,
        )
        payload = evidence_bundle.ctypes.string_at(buffer, size)
        self.assertEqual(int.from_bytes(payload[0:4], "little"), 0)
        self.assertEqual(
            int.from_bytes(
                payload[8 : 8 + evidence_bundle.ctypes.sizeof(
                    evidence_bundle.ctypes.c_void_p
                )],
                "little",
            ),
            0x5678,
        )
        self.assertEqual(
            int.from_bytes(payload[16:20], "little"),
            len("final".encode("utf-16-le")),
        )
        self.assertEqual(payload[20:], "final".encode("utf-16-le"))

    def test_windows_handle_rename_uses_only_handle_bound_legacy_fallback(
        self,
    ) -> None:
        classes: list[int] = []

        class Kernel32:
            @staticmethod
            def SetFileInformationByHandle(
                handle: object,
                information_class: int,
                buffer: object,
                size: int,
            ) -> int:
                classes.append(information_class)
                return int(len(classes) == 2)

        api = object.__new__(evidence_bundle._WindowsAPI)
        api._kernel32 = Kernel32()  # type: ignore[attr-defined]
        api._last_error = mock.Mock(  # type: ignore[method-assign]
            return_value=evidence_bundle._WIN_ERROR_INVALID_PARAMETER
        )
        api.rename_handle_noreplace(0x1234, 0x5678, "final")
        self.assertEqual(
            classes,
            [
                evidence_bundle._WIN_FILE_RENAME_INFO_EX,
                evidence_bundle._WIN_FILE_RENAME_INFO,
            ],
        )

    def test_windows_path_based_rename_has_no_unsafe_fallback(self) -> None:
        with mock.patch.object(
            evidence_bundle.sys,
            "platform",
            "win32",
        ):
            with self.assertRaises(UnsupportedPlatformError):
                evidence_bundle._atomic_rename_noreplace(
                    Path("C:\\stage"),
                    Path("C:\\final"),
                    None,
                    None,
                )

    def test_windows_parent_flush_failure_reports_publication_uncertain(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []
                self.opens: list[dict[str, object]] = []
                self.source_identity: tuple[int, int, int] | None = (
                    1,
                    2,
                    3,
                )
                self.final_identity: tuple[int, int, int] | None = None
                self.renamed_with_publication_handle = False

            def create_file(
                self,
                path: Path,
                **kwargs: object,
            ) -> object:
                self.opens.append({"path": path, **kwargs})
                return "publication"

            def require_directory_no_reparse(
                self,
                handle: object,
            ) -> None:
                self.assert_publication(handle)

            def require_private_acl(self, handle: object) -> None:
                self.assert_publication(handle)

            @staticmethod
            def assert_publication(handle: object) -> None:
                if handle != "publication":
                    raise AssertionError("wrong publication handle")

            def info(self, handle: object) -> object:
                identity = (
                    (1, 2, 3)
                    if handle in {"stage", "publication"}
                    else (4, 5, 6)
                )
                return types.SimpleNamespace(identity=identity)

            def directory_identity(self, path: Path) -> object:
                if path.name == ".evidence.staging":
                    return self.source_identity
                if path.name == "evidence":
                    return self.final_identity
                return (4, 5, 6)

            def rename_handle_noreplace(
                self,
                stage_handle: object,
                parent_handle: object,
                final_name: str,
            ) -> None:
                self.renamed_with_publication_handle = (
                    stage_handle == "publication"
                    and "publication" not in self.closed
                    and "stage" not in self.closed
                )
                self.source_identity = None
                self.final_identity = (1, 2, 3)

            def flush_directory(self, handle: object) -> None:
                if handle == "parent":
                    raise OSError(errno.EIO, "parent flush failed")

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        final = Path("/windows/evidence")
        api = FakeWindowsAPI()
        writer = evidence_bundle._WindowsEvidenceBundle(
            final,
            Path("/windows/.evidence.staging"),
            api,  # type: ignore[arg-type]
            ["parent"],
            "stage",
            (4, 5, 6),
            (1, 2, 3),
            _metadata(),
        )
        with mock.patch.object(
            writer,
            "_validate_before_finalize",
        ), mock.patch.object(
            writer,
            "_build_manifest",
            return_value=b"manifest",
        ), mock.patch.object(
            writer,
            "_write_owned",
        ):
            with self.assertRaises(
                PublicationUncertainError
            ) as caught:
                writer.finalize()
        self.assertTrue(api.renamed_with_publication_handle)
        self.assertEqual(len(api.opens), 1)
        publication_open = api.opens[0]
        self.assertEqual(
            publication_open["access"],
            (
                evidence_bundle._WIN_FILE_READ_ATTRIBUTES
                | evidence_bundle._WIN_READ_CONTROL
                | evidence_bundle._WIN_DELETE
            ),
        )
        self.assertEqual(
            publication_open["share"],
            (
                evidence_bundle._WIN_FILE_SHARE_READ
                | evidence_bundle._WIN_FILE_SHARE_WRITE
                | evidence_bundle._WIN_FILE_SHARE_DELETE
            ),
        )
        self.assertIn("publication", api.closed)
        self.assertEqual(caught.exception.final_path, final)
        self.assertIn("audit", str(caught.exception))
        self.assertFalse(writer._active)
        with self.assertRaises(RuntimeError):
            writer.finalize()

    def test_windows_post_rename_state_interrupt_closes_all_handles(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []
                self.source_identity: tuple[int, int, int] | None = (
                    1,
                    2,
                    3,
                )
                self.final_identity: tuple[int, int, int] | None = None

            def create_file(
                self,
                path: Path,
                **kwargs: object,
            ) -> object:
                return "publication"

            def require_directory_no_reparse(
                self,
                handle: object,
            ) -> None:
                pass

            def require_private_acl(self, handle: object) -> None:
                pass

            def info(self, handle: object) -> object:
                identity = (
                    (1, 2, 3)
                    if handle in {"stage", "publication"}
                    else (4, 5, 6)
                )
                return types.SimpleNamespace(identity=identity)

            def directory_identity(self, path: Path) -> object:
                if path.name == ".evidence.staging":
                    return self.source_identity
                if path.name == "evidence":
                    return self.final_identity
                return (4, 5, 6)

            def rename_handle_noreplace(
                self,
                stage_handle: object,
                parent_handle: object,
                final_name: str,
            ) -> None:
                self.source_identity = None
                self.final_identity = (1, 2, 3)

            def flush_directory(self, handle: object) -> None:
                pass

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        final = Path("/windows/evidence")
        api = FakeWindowsAPI()
        writer = evidence_bundle._WindowsEvidenceBundle(
            final,
            Path("/windows/.evidence.staging"),
            api,  # type: ignore[arg-type]
            ["ancestor", "parent"],
            "stage",
            (4, 5, 6),
            (1, 2, 3),
            _metadata(),
        )
        real_state = writer._publication_state

        def interrupt_after_publication() -> str:
            state = real_state()
            if state == evidence_bundle._PUBLISHED:
                raise KeyboardInterrupt(
                    "post-rename classification interrupted"
                )
            return state

        with mock.patch.object(
            writer,
            "_validate_before_finalize",
        ), mock.patch.object(
            writer,
            "_build_manifest",
            return_value=b"manifest",
        ), mock.patch.object(
            writer,
            "_write_owned",
        ), mock.patch.object(
            writer,
            "_publication_state",
            side_effect=interrupt_after_publication,
        ):
            with self.assertRaises(PublicationUncertainError):
                writer.finalize()

        self.assertFalse(writer._active)
        self.assertIsNone(writer._stage_handle)
        self.assertEqual(writer._parent_handles, [])
        self.assertCountEqual(
            api.closed,
            ["publication", "stage", "parent", "ancestor"],
        )
        self.assertEqual(api.final_identity, (1, 2, 3))

    def test_public_create_and_audit_route_to_windows_backend(
        self,
    ) -> None:
        sentinel = object()
        payloads = {
            "raw.csv": RAW,
            "summary.csv": SUMMARY,
            "metadata.json": evidence_bundle.canonical_json_bytes(
                _metadata()
            ),
            "verdict.json": VERDICT,
            "report.md": REPORT,
        }
        payloads["MANIFEST.sha256"] = b"".join(
            (
                hashlib.sha256(payloads[name]).hexdigest()
                + "  "
                + name
                + "\n"
            ).encode("ascii")
            for name in sorted(payloads)
        )

        class Snapshot:
            def __init__(self) -> None:
                self.payloads = payloads
                self.revalidated = False
                self.closed = False

            def revalidate(self) -> None:
                self.revalidated = True

            def close(self, *, suppress: bool) -> None:
                self.closed = True

        snapshot = Snapshot()
        with mock.patch.object(
            evidence_bundle,
            "_platform_kind",
            return_value="windows",
        ), mock.patch.object(
            evidence_bundle._WindowsEvidenceBundle,
            "create",
            return_value=sentinel,
        ) as create, mock.patch.object(
            evidence_bundle,
            "_open_windows_audit_snapshot",
            return_value=snapshot,
        ) as open_snapshot:
            self.assertIs(
                EvidenceBundle.create("C:\\evidence", _metadata()),
                sentinel,
            )
            result = audit_bundle(
                "C:\\evidence",
                recompute=_recompute,
            )
        create.assert_called_once()
        open_snapshot.assert_called_once()
        self.assertTrue(snapshot.revalidated)
        self.assertTrue(snapshot.closed)
        self.assertEqual(result.verdict, "REJECT")

    def test_windows_file_contract_uses_create_new_nofollow_and_flush(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.calls: list[tuple[object, ...]] = []

            def create_file(
                self,
                path: Path,
                *,
                creation_disposition: int,
                flags: int,
                access: int,
                share: int,
                private: bool = False,
            ) -> object:
                self.calls.append(
                    (
                        "create",
                        path,
                        creation_disposition,
                        flags,
                        access,
                        share,
                        private,
                    )
                )
                return "handle"

            def require_regular_single_link(self, handle: object) -> None:
                self.calls.append(("check", handle))

            def require_private_acl(self, handle: object) -> None:
                self.calls.append(("private-acl", handle))

            def write_all(self, handle: object, data: bytes) -> None:
                self.calls.append(("write", handle, data))

            def flush(self, handle: object) -> None:
                self.calls.append(("flush", handle))

            def close(self, handle: object) -> None:
                self.calls.append(("close", handle))

        api = FakeWindowsAPI()
        evidence_bundle._win_create_and_write_file(
            Path("C:\\stage\\raw.csv"),
            RAW,
            api=api,  # type: ignore[arg-type]
        )
        create = api.calls[0]
        self.assertEqual(create[0], "create")
        self.assertEqual(
            create[2],
            evidence_bundle._WIN_CREATE_NEW,
        )
        self.assertTrue(
            create[3] & evidence_bundle._WIN_FILE_FLAG_OPEN_REPARSE_POINT
        )
        self.assertTrue(create[6])
        self.assertIn(("private-acl", "handle"), api.calls)
        self.assertIn(("flush", "handle"), api.calls)
        self.assertEqual(api.calls[-1], ("close", "handle"))

    def test_windows_directory_creation_uses_private_security_attributes(
        self,
    ) -> None:
        calls: list[tuple[object, object]] = []

        class Kernel32:
            @staticmethod
            def CreateDirectoryW(path: object, attributes: object) -> int:
                calls.append((path, attributes))
                return 1

        api = object.__new__(evidence_bundle._WindowsAPI)
        api._kernel32 = Kernel32()  # type: ignore[attr-defined]
        security = mock.MagicMock()
        security.__enter__.return_value = "private-security"
        security.__exit__.return_value = False
        with mock.patch.object(
            api,
            "_private_security_attributes",
            return_value=security,
            create=True,
        ):
            api.create_directory(Path("C:\\stage"))
        self.assertEqual(calls, [("C:\\stage", "private-security")])

    def test_windows_private_acl_validation_rejects_relaxed_dacl(
        self,
    ) -> None:
        api = object.__new__(evidence_bundle._WindowsAPI)
        private_sddl = (
            "D:P(A;;FA;;;SY)(A;;FA;;;S-1-5-21-1000)"
        )
        api._private_dacl_sddl = mock.Mock(  # type: ignore[method-assign]
            return_value=private_sddl
        )
        api.info = mock.Mock(  # type: ignore[method-assign]
            return_value=types.SimpleNamespace(file_attributes=0)
        )
        api._private_dacl_details = mock.Mock(  # type: ignore[method-assign]
            side_effect=(
                (private_sddl, True),
                ("D:P(A;;FA;;;OW)(A;;FR;;;WD)", True),
            )
        )
        api.require_private_acl("private")
        with self.assertRaisesRegex(EvidenceError, "private DACL"):
            api.require_private_acl("relaxed")

    def test_windows_private_acl_validation_requires_protection(
        self,
    ) -> None:
        api = object.__new__(evidence_bundle._WindowsAPI)
        private_sddl = (
            "D:(A;;FA;;;SY)(A;;FA;;;S-1-5-21-1000)"
        )
        api._private_dacl_sddl = mock.Mock(  # type: ignore[method-assign]
            return_value=private_sddl.replace("D:", "D:P", 1)
        )
        api.info = mock.Mock(  # type: ignore[method-assign]
            return_value=types.SimpleNamespace(file_attributes=0)
        )
        api._private_dacl_details = mock.Mock(  # type: ignore[method-assign]
            return_value=(private_sddl, False)
        )
        with self.assertRaisesRegex(EvidenceError, "private DACL"):
            api.require_private_acl("unprotected")

    def test_windows_private_dacl_names_current_token_user_sid(
        self,
    ) -> None:
        api = object.__new__(evidence_bundle._WindowsAPI)
        api._current_user_sid = mock.Mock(  # type: ignore[method-assign]
            return_value="S-1-5-21-1000"
        )
        self.assertEqual(
            api._private_dacl_sddl(),
            (
                "D:P(A;;FA;;;SY)"
                "(A;;FA;;;S-1-5-21-1000)"
            ),
        )
        self.assertEqual(
            api._private_dacl_sddl(directory=True),
            (
                "D:P(A;OICI;FA;;;SY)"
                "(A;OICI;FA;;;S-1-5-21-1000)"
            ),
        )

    def test_windows_audit_rejects_relaxed_bundle_acl_before_hashes(
        self,
    ) -> None:
        with mock.patch.object(
            evidence_bundle,
            "_win_open_directory_chain",
            side_effect=EvidenceError("Windows object lacks private DACL"),
        ) as open_chain, mock.patch.object(
            evidence_bundle,
            "_WindowsAPI",
            return_value=mock.Mock(),
        ):
            with self.assertRaisesRegex(EvidenceError, "private DACL"):
                evidence_bundle._win_read_bundle_payloads(
                    Path("C:\\bundle")
                )
        self.assertTrue(
            open_chain.call_args.kwargs["require_private_leaf"]
        )

    def test_windows_audit_snapshot_holds_read_only_shared_handles(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.opens: list[tuple[Path, int]] = []
                self.closed: list[object] = []

            def create_file(
                self,
                path: Path,
                **kwargs: object,
            ) -> object:
                self.opens.append((path, kwargs["share"]))  # type: ignore[arg-type]
                return f"file:{path.name}:{len(self.opens)}"

            def require_regular_single_link(
                self,
                handle: object,
            ) -> None:
                pass

            def require_directory_no_reparse(
                self,
                handle: object,
            ) -> None:
                pass

            def require_private_acl(self, handle: object) -> None:
                pass

            def info(self, handle: object) -> object:
                if handle == "bundle":
                    identity = (1, 2, 3)
                    size = 0
                else:
                    name = str(handle).split(":")[1]
                    identity = (4, 5, hash(name))
                    size = len(payloads[name])
                return types.SimpleNamespace(
                    identity=identity,
                    size=size,
                    write_time=(7, 8),
                    number_of_links=1,
                    file_attributes=0,
                )

            def read_all(
                self,
                handle: object,
                expected_size: int,
            ) -> bytes:
                name = str(handle).split(":")[1]
                self.assert_size(name, expected_size)
                return payloads[name]

            @staticmethod
            def assert_size(name: str, size: int) -> None:
                if len(payloads[name]) != size:
                    raise AssertionError("wrong size")

            def directory_identity(self, path: Path) -> object:
                return (1, 2, 3)

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        payloads = {
            "raw.csv": RAW,
            "summary.csv": SUMMARY,
            "metadata.json": evidence_bundle.canonical_json_bytes(
                _metadata()
            ),
            "verdict.json": VERDICT,
            "report.md": REPORT,
            "MANIFEST.sha256": b"manifest",
        }
        api = FakeWindowsAPI()
        with mock.patch.object(
            evidence_bundle,
            "_WindowsAPI",
            return_value=api,
        ), mock.patch.object(
            evidence_bundle,
            "_win_open_directory_chain",
            return_value=(Path("/windows/bundle"), ["bundle"]),
        ), mock.patch.object(
            evidence_bundle.os,
            "listdir",
            return_value=list(payloads),
        ):
            snapshot = (
                evidence_bundle._open_windows_audit_snapshot(
                    Path("/windows/bundle")
                )
            )
            self.assertEqual(api.closed, [])
            self.assertTrue(api.opens)
            self.assertEqual(
                {share for _, share in api.opens},
                {evidence_bundle._WIN_FILE_SHARE_READ},
            )
            snapshot.close(suppress=False)
        self.assertIn("bundle", api.closed)

    def test_windows_stage_acl_failure_closes_parent_without_cleanup_guessing(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []
                self.remove_calls: list[Path] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                raise FileNotFoundError(path)

            def info(self, handle: object) -> object:
                return types.SimpleNamespace(identity=(1, 2, 3))

            def directory_identity(self, path: Path) -> object:
                return (1, 2, 3)

            def create_directory(self, path: Path) -> None:
                raise OSError(errno.EACCES, "private DACL creation failed")

            def close(self, handle: object) -> None:
                self.closed.append(handle)

            def remove_directory(self, path: Path) -> None:
                self.remove_calls.append(path)

        api = FakeWindowsAPI()
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
            evidence_bundle,
            "_WindowsAPI",
            return_value=api,
        ), mock.patch.object(
            evidence_bundle,
            "_win_open_directory_chain",
            return_value=(Path(temporary), ["parent"]),
        ):
            with self.assertRaisesRegex(OSError, "private DACL"):
                evidence_bundle._WindowsEvidenceBundle.create(
                    Path(temporary) / "bundle",
                    _metadata(),
                )
        self.assertEqual(api.closed, ["parent"])
        self.assertEqual(api.remove_calls, [])

    def test_windows_stage_identity_failure_preserves_primary_error(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []
                self.remove_calls: list[Path] = []
                self.stage_info_calls = 0

            def info(self, handle: object) -> object:
                if handle == "stage":
                    self.stage_info_calls += 1
                    if self.stage_info_calls == 1:
                        raise OSError(
                            errno.EIO,
                            "stage identity failed",
                        )
                return types.SimpleNamespace(identity=(1, 2, 3))

            def directory_identity(self, path: Path) -> object:
                return (1, 2, 3)

            def create_file(self, path: Path, **kwargs: object) -> object:
                raise FileNotFoundError(path)

            def create_directory(self, path: Path) -> None:
                pass

            def close(self, handle: object) -> None:
                self.closed.append(handle)

            def remove_directory(self, path: Path) -> None:
                self.remove_calls.append(path)

        api = FakeWindowsAPI()
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
            evidence_bundle,
            "_WindowsAPI",
            return_value=api,
        ), mock.patch.object(
            evidence_bundle,
            "_win_open_directory_chain",
            return_value=(Path(temporary), ["parent"]),
        ), mock.patch.object(
            evidence_bundle,
            "_win_open_directory",
            return_value="stage",
        ) as open_directory:
            with self.assertRaisesRegex(
                OSError,
                "stage identity failed",
            ):
                evidence_bundle._WindowsEvidenceBundle.create(
                    Path(temporary) / "bundle",
                    _metadata(),
                )
        self.assertFalse(
            open_directory.call_args.kwargs["rename_source"]
        )
        self.assertEqual(api.closed, ["stage", "parent"])
        self.assertEqual(api.remove_calls, [])

    def test_windows_directory_error_after_side_effect_is_not_claimed(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []
                self.foreign_stage: Path | None = None
                self.remove_calls: list[Path] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                raise FileNotFoundError(path)

            def info(self, handle: object) -> object:
                return types.SimpleNamespace(identity=(1, 2, 3))

            def directory_identity(self, path: Path) -> object:
                return (1, 2, 3)

            def create_directory(self, path: Path) -> None:
                self.foreign_stage = path
                raise OSError(
                    errno.EACCES,
                    "CreateDirectory failed after side effect",
                )

            def close(self, handle: object) -> None:
                self.closed.append(handle)

            def remove_directory(self, path: Path) -> None:
                self.remove_calls.append(path)

        api = FakeWindowsAPI()
        with tempfile.TemporaryDirectory() as temporary, mock.patch.object(
            evidence_bundle,
            "_WindowsAPI",
            return_value=api,
        ), mock.patch.object(
            evidence_bundle,
            "_win_open_directory_chain",
            return_value=(Path(temporary), ["parent"]),
        ):
            with self.assertRaisesRegex(
                OSError,
                "after side effect",
            ):
                evidence_bundle._WindowsEvidenceBundle.create(
                    Path(temporary) / "bundle",
                    _metadata(),
                )
        self.assertIsNotNone(api.foreign_stage)
        self.assertEqual(api.remove_calls, [])
        self.assertEqual(api.closed, ["parent"])

    def test_windows_ancestor_chain_holds_nofollow_handles(self) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.opens: list[tuple[Path, int, int]] = []
                self.checked: list[object] = []

            def create_file(
                self,
                path: Path,
                *,
                creation_disposition: int,
                flags: int,
                access: int,
                share: int,
            ) -> object:
                self.opens.append((path, flags, share))
                return f"handle-{len(self.opens)}"

            def require_directory_no_reparse(self, handle: object) -> None:
                self.checked.append(handle)

            def require_private_acl(self, handle: object) -> None:
                self.checked.append(("private", handle))

            def close(self, handle: object) -> None:
                raise AssertionError("successful chain must retain handles")

        api = FakeWindowsAPI()
        root = Path(tempfile.gettempdir()).resolve()
        _, handles = evidence_bundle._win_open_directory_chain(
            root,
            api=api,  # type: ignore[arg-type]
            require_private_leaf=True,
        )
        self.assertEqual(len(handles), len(api.opens))
        self.assertEqual(
            api.checked[-1],
            ("private", handles[-1]),
        )
        for _, flags, share in api.opens:
            self.assertTrue(
                flags
                & evidence_bundle._WIN_FILE_FLAG_OPEN_REPARSE_POINT
            )
            self.assertEqual(
                share,
                (
                    evidence_bundle._WIN_FILE_SHARE_READ
                    | evidence_bundle._WIN_FILE_SHARE_WRITE
                    | evidence_bundle._WIN_FILE_SHARE_DELETE
                ),
            )

    def test_windows_retained_directory_handle_denies_delete_access(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.open_kwargs: dict[str, object] = {}

            def create_file(
                self,
                path: Path,
                **kwargs: object,
            ) -> object:
                self.open_kwargs = kwargs
                return "stage"

            def require_directory_no_reparse(
                self,
                handle: object,
            ) -> None:
                pass

            def require_private_acl(self, handle: object) -> None:
                pass

            def close(self, handle: object) -> None:
                pass

        api = FakeWindowsAPI()
        handle = evidence_bundle._win_open_directory(
            Path("C:\\stage"),
            api=api,  # type: ignore[arg-type]
            require_private=True,
            writable=True,
            rename_source=False,
        )
        self.assertEqual(handle, "stage")
        access = int(api.open_kwargs["access"])
        self.assertFalse(access & evidence_bundle._WIN_DELETE)
        self.assertEqual(
            api.open_kwargs["share"],
            (
                evidence_bundle._WIN_FILE_SHARE_READ
                | evidence_bundle._WIN_FILE_SHARE_WRITE
                | evidence_bundle._WIN_FILE_SHARE_DELETE
            ),
        )

    def test_windows_audit_directory_chain_denies_only_delete_share(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.shares: list[int] = []

            def create_file(
                self,
                path: Path,
                **kwargs: object,
            ) -> object:
                self.shares.append(int(kwargs["share"]))
                return f"handle-{len(self.shares)}"

            def require_directory_no_reparse(
                self,
                handle: object,
            ) -> None:
                pass

            def require_private_acl(self, handle: object) -> None:
                pass

            def close(self, handle: object) -> None:
                pass

        api = FakeWindowsAPI()
        _, handles = evidence_bundle._win_open_directory_chain(
            Path(tempfile.gettempdir()).resolve(),
            api=api,  # type: ignore[arg-type]
            require_private_leaf=True,
            deny_delete=True,
        )
        self.assertTrue(handles)
        self.assertEqual(
            set(api.shares),
            {
                evidence_bundle._WIN_FILE_SHARE_READ
                | evidence_bundle._WIN_FILE_SHARE_WRITE
            },
        )

    def test_windows_abort_state_interrupt_preserves_write_error(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        api = FakeWindowsAPI()
        writer = evidence_bundle._WindowsEvidenceBundle(
            Path("/windows/evidence"),
            Path("/windows/.evidence.staging"),
            api,  # type: ignore[arg-type]
            ["parent"],
            "stage",
            (4, 5, 6),
            (1, 2, 3),
            _metadata(),
        )
        with mock.patch.object(
            evidence_bundle,
            "_win_create_and_write_file",
            side_effect=OSError(errno.EIO, "primary Windows write error"),
        ), mock.patch.object(
            writer,
            "_publication_state",
            side_effect=KeyboardInterrupt(
                "Windows abort classification interrupted"
            ),
        ):
            with self.assertRaisesRegex(
                OSError,
                "primary Windows write error",
            ):
                writer.write_bytes("raw.csv", RAW)
        self.assertFalse(writer._active)
        self.assertCountEqual(api.closed, ["stage", "parent"])

    def test_windows_reparse_ancestor_rejection_closes_handles(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.opened: list[object] = []
                self.closed: list[object] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                handle = f"handle-{len(self.opened) + 1}"
                self.opened.append(handle)
                return handle

            def require_directory_no_reparse(self, handle: object) -> None:
                if len(self.opened) == 2:
                    raise EvidenceError("junction ancestor")

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        api = FakeWindowsAPI()
        with self.assertRaisesRegex(EvidenceError, "junction"):
            evidence_bundle._win_open_directory_chain(
                Path(tempfile.gettempdir()).resolve(),
                api=api,  # type: ignore[arg-type]
            )
        self.assertCountEqual(api.closed, api.opened)

    def test_windows_hardlink_is_rejected(self) -> None:
        api = object.__new__(evidence_bundle._WindowsAPI)
        api.info = mock.Mock(  # type: ignore[method-assign]
            return_value=types.SimpleNamespace(
                file_attributes=0,
                number_of_links=2,
                size=1,
            )
        )
        with self.assertRaises(EvidenceError):
            api.require_regular_single_link("handle")

    def test_windows_failed_write_closes_new_file_handle(self) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                return "handle"

            def require_regular_single_link(self, handle: object) -> None:
                return None

            def require_private_acl(self, handle: object) -> None:
                return None

            def write_all(self, handle: object, data: bytes) -> None:
                raise OSError(errno.EIO, "short WriteFile")

            def flush(self, handle: object) -> None:
                raise AssertionError("failed write must not flush")

            def close(self, handle: object) -> None:
                self.closed.append(handle)
                raise OSError(errno.EBADF, "CloseHandle failed")

        api = FakeWindowsAPI()
        with self.assertRaisesRegex(OSError, "short WriteFile"):
            evidence_bundle._win_create_and_write_file(
                Path("C:\\stage\\raw.csv"),
                RAW,
                api=api,  # type: ignore[arg-type]
            )
        self.assertEqual(api.closed, ["handle"])

    def test_windows_short_read_is_rejected_and_closed(self) -> None:
        before = types.SimpleNamespace(
            identity=(1, 2, 3),
            size=len(RAW),
            write_time=(4, 5),
        )

        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                return "handle"

            def require_regular_single_link(self, handle: object) -> None:
                return None

            def require_private_acl(self, handle: object) -> None:
                return None

            def info(self, handle: object) -> object:
                return before

            def read_all(self, handle: object, size: int) -> bytes:
                return RAW[:-1]

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        api = FakeWindowsAPI()
        with self.assertRaisesRegex(EvidenceError, "changed"):
            evidence_bundle._win_read_regular_file(
                Path("C:\\bundle\\raw.csv"),
                api=api,  # type: ignore[arg-type]
            )
        self.assertEqual(api.closed, ["handle"])

    def test_windows_failed_read_is_closed(self) -> None:
        before = types.SimpleNamespace(
            identity=(1, 2, 3),
            size=len(RAW),
            write_time=(4, 5),
        )

        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                return "handle"

            def require_regular_single_link(self, handle: object) -> None:
                return None

            def require_private_acl(self, handle: object) -> None:
                return None

            def info(self, handle: object) -> object:
                return before

            def read_all(self, handle: object, size: int) -> bytes:
                raise OSError(errno.EIO, "failed ReadFile")

            def close(self, handle: object) -> None:
                self.closed.append(handle)
                raise OSError(errno.EBADF, "CloseHandle failed")

        api = FakeWindowsAPI()
        with self.assertRaisesRegex(OSError, "failed ReadFile"):
            evidence_bundle._win_read_regular_file(
                Path("C:\\bundle\\raw.csv"),
                api=api,  # type: ignore[arg-type]
            )
        self.assertEqual(api.closed, ["handle"])

    def test_windows_overreported_write_and_read_counts_fail_closed(
        self,
    ) -> None:
        class Kernel32:
            @staticmethod
            def WriteFile(
                handle: object,
                buffer: object,
                requested: int,
                count: object,
                overlapped: object,
            ) -> int:
                ctypes_count = evidence_bundle.ctypes.cast(
                    count,
                    evidence_bundle.ctypes.POINTER(
                        evidence_bundle.ctypes.c_uint32
                    ),
                )
                ctypes_count.contents.value = requested + 1
                return 1

            @staticmethod
            def ReadFile(
                handle: object,
                buffer: object,
                requested: int,
                count: object,
                overlapped: object,
            ) -> int:
                ctypes_count = evidence_bundle.ctypes.cast(
                    count,
                    evidence_bundle.ctypes.POINTER(
                        evidence_bundle.ctypes.c_uint32
                    ),
                )
                ctypes_count.contents.value = requested + 1
                return 1

        api = object.__new__(evidence_bundle._WindowsAPI)
        api._kernel32 = Kernel32()  # type: ignore[attr-defined]
        with self.assertRaisesRegex(OSError, "invalid byte count"):
            api.write_all("handle", b"x")
        with self.assertRaisesRegex(OSError, "invalid byte count"):
            api.read_all("handle", 1)

    def test_windows_directory_flush_does_not_ignore_access_denied(
        self,
    ) -> None:
        api = object.__new__(evidence_bundle._WindowsAPI)
        api.flush = mock.Mock(  # type: ignore[method-assign]
            side_effect=OSError(
                evidence_bundle._WIN_ERROR_ACCESS_DENIED,
                "denied",
            )
        )
        with self.assertRaises(OSError):
            api.flush_directory("directory")

    def test_unsupported_platform_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            source = root / "source"
            source.mkdir()
            with mock.patch.object(
                evidence_bundle.sys,
                "platform",
                "unsupported",
            ):
                with self.assertRaises(UnsupportedPlatformError):
                    evidence_bundle._atomic_rename_noreplace(
                        source,
                        root / "destination",
                        None,
                        None,
                    )
            self.assertTrue(source.is_dir())
            self.assertFalse((root / "destination").exists())


if __name__ == "__main__":
    unittest.main()
