#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import errno
import hashlib
import json
import os
import stat
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


class CreationAndFinalizationTests(unittest.TestCase):
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
    def test_windows_parent_flush_failure_reports_publication_uncertain(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []

            def flush_directory(self, handle: object) -> None:
                if handle == "parent":
                    raise OSError(errno.EIO, "parent flush failed")

            def close(self, handle: object) -> None:
                self.closed.append(handle)

        final = Path("C:\\evidence")
        api = FakeWindowsAPI()
        writer = evidence_bundle._WindowsEvidenceBundle(
            final,
            Path("C:\\.evidence.staging"),
            api,  # type: ignore[arg-type]
            ["parent"],
            "stage",
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
        ), mock.patch.object(
            evidence_bundle,
            "_atomic_rename_noreplace",
        ) as rename:
            with self.assertRaises(
                PublicationUncertainError
            ) as caught:
                writer.finalize()
        rename.assert_called_once()
        self.assertEqual(caught.exception.final_path, final)
        self.assertIn("audit", str(caught.exception))
        self.assertFalse(writer._active)
        with self.assertRaises(RuntimeError):
            writer.finalize()
        rename.assert_called_once()

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
            "_win_read_bundle_payloads",
            return_value=payloads,
        ) as read:
            self.assertIs(
                EvidenceBundle.create("C:\\evidence", _metadata()),
                sentinel,
            )
            result = audit_bundle(
                "C:\\evidence",
                recompute=_recompute,
            )
        create.assert_called_once()
        read.assert_called_once()
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
        api.private_dacl_sddl = mock.Mock(  # type: ignore[method-assign]
            side_effect=(
                evidence_bundle._WIN_PRIVATE_DACL_SDDL,
                "D:P(A;;FA;;;OW)(A;;FR;;;WD)",
            )
        )
        api.require_private_acl("private")
        with self.assertRaisesRegex(EvidenceError, "private DACL"):
            api.require_private_acl("relaxed")

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

    def test_windows_stage_acl_failure_closes_parent_without_cleanup_guessing(
        self,
    ) -> None:
        class FakeWindowsAPI:
            def __init__(self) -> None:
                self.closed: list[object] = []
                self.remove_calls: list[Path] = []

            def create_file(self, path: Path, **kwargs: object) -> object:
                raise FileNotFoundError(path)

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
                ),
            )

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

    def test_windows_atomic_move_is_nonreplacing(self) -> None:
        class FakeFunction:
            def __init__(self) -> None:
                self.calls: list[tuple[object, ...]] = []

            def __call__(self, *args: object) -> int:
                self.calls.append(args)
                return 1

        move = FakeFunction()
        kernel32 = type("Kernel32", (), {"MoveFileExW": move})()
        with mock.patch.object(
            evidence_bundle.sys,
            "platform",
            "win32",
        ), mock.patch.object(
            evidence_bundle.ctypes,
            "WinDLL",
            return_value=kernel32,
            create=True,
        ):
            evidence_bundle._atomic_rename_noreplace(
                Path("C:\\stage"),
                Path("C:\\final"),
                None,
                None,
            )
        self.assertEqual(len(move.calls), 1)
        self.assertEqual(move.calls[0][2], 0)

    def test_windows_atomic_move_maps_existing_destination(self) -> None:
        class FakeFunction:
            def __call__(self, *args: object) -> int:
                return 0

        kernel32 = type(
            "Kernel32",
            (),
            {"MoveFileExW": FakeFunction()},
        )()
        with mock.patch.object(
            evidence_bundle.sys,
            "platform",
            "win32",
        ), mock.patch.object(
            evidence_bundle.ctypes,
            "WinDLL",
            return_value=kernel32,
            create=True,
        ), mock.patch.object(
            evidence_bundle.ctypes,
            "get_last_error",
            return_value=evidence_bundle._WIN_ERROR_ALREADY_EXISTS,
            create=True,
        ):
            with self.assertRaises(FileExistsError):
                evidence_bundle._atomic_rename_noreplace(
                    Path("C:\\stage"),
                    Path("C:\\final"),
                    None,
                    None,
                )

    def test_windows_existing_destinations_are_left_untouched(
        self,
    ) -> None:
        class FakeFunction:
            def __call__(self, *args: object) -> int:
                return 0

        kernel32 = type(
            "Kernel32",
            (),
            {"MoveFileExW": FakeFunction()},
        )()
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            for name, nonempty in (("empty", False), ("full", True)):
                destination = root / name
                destination.mkdir()
                if nonempty:
                    (destination / "sentinel").write_text("keep")
                with mock.patch.object(
                    evidence_bundle.sys,
                    "platform",
                    "win32",
                ), mock.patch.object(
                    evidence_bundle.ctypes,
                    "WinDLL",
                    return_value=kernel32,
                    create=True,
                ), mock.patch.object(
                    evidence_bundle.ctypes,
                    "get_last_error",
                    return_value=(
                        evidence_bundle._WIN_ERROR_ALREADY_EXISTS
                    ),
                    create=True,
                ):
                    with self.assertRaises(FileExistsError):
                        evidence_bundle._atomic_rename_noreplace(
                            root / "stage",
                            destination,
                            None,
                            None,
                        )
                self.assertTrue(destination.is_dir())
                if nonempty:
                    self.assertEqual(
                        (destination / "sentinel").read_text(),
                        "keep",
                    )

    def test_windows_atomic_move_unknown_error_fails_closed(self) -> None:
        class FakeFunction:
            def __call__(self, *args: object) -> int:
                return 0

        kernel32 = type(
            "Kernel32",
            (),
            {"MoveFileExW": FakeFunction()},
        )()
        with mock.patch.object(
            evidence_bundle.sys,
            "platform",
            "win32",
        ), mock.patch.object(
            evidence_bundle.ctypes,
            "WinDLL",
            return_value=kernel32,
            create=True,
        ), mock.patch.object(
            evidence_bundle.ctypes,
            "get_last_error",
            return_value=1234,
            create=True,
        ):
            with self.assertRaises(OSError) as raised:
                evidence_bundle._atomic_rename_noreplace(
                    Path("C:\\stage"),
                    Path("C:\\final"),
                    None,
                    None,
                )
        self.assertEqual(raised.exception.errno, 1234)

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
