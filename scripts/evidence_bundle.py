#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Create-once, self-auditing benchmark evidence bundles.

The recompute callback is deliberately small and typed.  It receives the
exact ``raw.csv`` bytes plus validated metadata and must independently return
the three derived artifacts as :class:`RecomputedArtifacts`.  Audit accepts
the bundle only when those returned bytes exactly match ``summary.csv``,
``verdict.json``, and ``report.md``.
"""

from __future__ import annotations

import csv
import ctypes
import errno
import hashlib
import io
import json
import math
import os
import re
import secrets
import stat
import sys
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Mapping


EVIDENCE_SCHEMA = "llam.performance-evidence.v1"
VERDICT_SCHEMA = "llam.performance-verdict.v1"
SCOPED_VERDICT_SCHEMA = "llam.performance-verdict.v2"
CLASSIFIER_SCHEMA = "llam.native-classifier.v1"
REPORT_NAME = "report.md"
MANIFEST_NAME = "MANIFEST.sha256"
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_SOURCE_STATE_BYTES = 8 * 1024 * 1024
MAX_JSON_DEPTH = 64
MAX_JSON_NODES = 10_000
MAX_JSON_STRING_BYTES = 4 * 1024 * 1024

_CALLER_ARTIFACTS = frozenset(
    {"raw.csv", "summary.csv", "verdict.json", REPORT_NAME}
)
_LIBRARY_ARTIFACTS = frozenset({"metadata.json", MANIFEST_NAME})
_ALL_ARTIFACTS = _CALLER_ARTIFACTS | _LIBRARY_ARTIFACTS
_MANIFEST_MEMBERS = tuple(sorted(_ALL_ARTIFACTS - {MANIFEST_NAME}))
_METADATA_FIELDS = frozenset(
    {
        "schema",
        "source_commit",
        "source_dirty_digest",
        "architecture",
        "kernel",
        "toolchain",
        "commands",
        "cpu_policy",
        "matrix",
        "sample_schedule",
        "classifier",
    }
)
_CLASSIFIER_FIELDS = frozenset({"schema", "thresholds"})
_LEGACY_VERDICT_FIELDS = frozenset(
    {"schema", "verdict", "reasons"}
)
_SCOPED_VERDICT_FIELDS = frozenset(
    {
        "schema",
        "verdict",
        "reasons",
        "portable_verdict",
        "platform_verdict",
        "required_cells",
        "classifier_thresholds",
    }
)
_REQUIRED_CELL_FIELDS = frozenset(
    {"candidate", "batch_width", "concurrency", "payload"}
)
_HEX40_RE = re.compile(r"[0-9a-f]{40}\Z")
_HEX64_RE = re.compile(r"[0-9a-f]{64}\Z")
_ARCHITECTURE_RE = re.compile(r"[a-z0-9][a-z0-9_.-]*\Z")
_MANIFEST_LINE_RE = re.compile(
    rb"([0-9a-f]{64})  ([A-Za-z0-9._-]+)\n\Z"
)
_NONFINITE_CELLS = frozenset(
    {
        "nan",
        "+nan",
        "-nan",
        "inf",
        "+inf",
        "-inf",
        "infinity",
        "+infinity",
        "-infinity",
    }
)


class EvidenceError(ValueError):
    """The bundle or requested bundle operation is invalid."""


class UnsupportedPlatformError(EvidenceError):
    """A required fail-closed filesystem primitive is unavailable."""


class PublicationUncertainError(RuntimeError):
    """The atomic publish occurred but durable directory sync was not proven."""

    def __init__(
        self,
        final_path: Path | str,
        cause: BaseException,
    ) -> None:
        self.final_path = Path(final_path)
        self.cause = cause
        super().__init__(
            "evidence publication may have succeeded at "
            f"{self.final_path}; do not retry creation or delete it; "
            f"recover with --audit-existing {self.final_path}: {cause}"
        )


_NOT_PUBLISHED = "not-published"
_PUBLISHED = "published"
_AMBIGUOUS = "ambiguous"


def _posix_identity(metadata: os.stat_result) -> tuple[int, int]:
    return (metadata.st_dev, metadata.st_ino)


def _named_posix_identity(
    parent_fd: int,
    name: str,
) -> tuple[int, int] | None:
    try:
        metadata = os.stat(
            name,
            dir_fd=parent_fd,
            follow_symlinks=False,
        )
    except FileNotFoundError:
        return None
    if not stat.S_ISDIR(metadata.st_mode):
        return (-1, -1)
    return _posix_identity(metadata)


def _require_posix_rename_authority(
    parent_fd: int,
    *,
    expected_stage_uid: int | None = None,
) -> tuple[int, int]:
    if not hasattr(os, "geteuid"):
        raise UnsupportedPlatformError(
            "effective uid is required for POSIX rename authority"
        )
    effective_uid = os.geteuid()
    metadata = os.fstat(parent_fd)
    mode = stat.S_IMODE(metadata.st_mode)
    trusted_owner = metadata.st_uid in {0, effective_uid}
    trusted_child = expected_stage_uid in {
        None,
        0,
        effective_uid,
    }
    owner_controlled = (
        trusted_owner
        and trusted_child
        and mode & 0o022 == 0
        and mode & 0o300 == 0o300
    )
    trusted_sticky = (
        bool(mode & stat.S_ISVTX)
        and trusted_owner
        and trusted_child
        and bool(mode & 0o022)
        and mode & 0o300 == 0o300
    )
    if not (owner_controlled or trusted_sticky):
        raise EvidenceError(
            "directory binding lacks trusted POSIX rename authority"
        )
    return _posix_identity(metadata)


@dataclass(frozen=True)
class RecomputedArtifacts:
    """Exact derived bytes returned by an audit recompute callback."""

    summary_csv: bytes
    verdict_json: bytes
    report_md: bytes


RecomputeCallback = Callable[
    [bytes, dict[str, object]],
    RecomputedArtifacts,
]


@dataclass(frozen=True)
class AuditResult:
    """Validated semantic result of an evidence-bundle audit."""

    verdict: str
    reasons: tuple[str, ...]
    metadata: Mapping[str, object]
    portable_verdict: str
    platform_verdict: str


def canonical_json_bytes(value: object) -> bytes:
    """Return the only accepted deterministic JSON representation."""

    _check_json_safe(value, where="value")
    try:
        text = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        return (text + "\n").encode("utf-8")
    except (
        TypeError,
        ValueError,
        RecursionError,
        UnicodeError,
    ) as exc:
        raise EvidenceError(f"value is not deterministic JSON: {exc}") from exc


def normalize_architecture(value: str) -> str:
    """Normalize common machine aliases for metadata producers."""

    normalized = value.strip().lower()
    aliases = {
        "aarch64": "arm64",
        "amd64": "x86_64",
        "x64": "x86_64",
    }
    return aliases.get(normalized, normalized)


def _source_hash_frame(
    digest: object,
    label: bytes,
    payload: bytes,
) -> None:
    digest.update(len(label).to_bytes(4, "big"))
    digest.update(label)
    digest.update(len(payload).to_bytes(8, "big"))
    digest.update(payload)


def _read_untracked_source(
    root: Path,
    relative_name: str,
    *,
    remaining: int,
) -> tuple[
    bytes,
    bytes,
    bytes,
    tuple[tuple[int, int, int], ...],
    tuple[int, int, int, int, int, int],
]:
    if (
        not relative_name
        or "\x00" in relative_name
        or "\ufffd" in relative_name
    ):
        raise EvidenceError("unsafe untracked source path encoding")
    relative = Path(relative_name)
    if (
        relative.is_absolute()
        or any(part in {"", ".", ".."} for part in relative.parts)
    ):
        raise EvidenceError("unsafe untracked source path")
    path_bytes = relative_name.encode("utf-8")
    if len(path_bytes) > remaining:
        raise EvidenceError("untracked source inventory is oversized")
    current = root
    ancestor_paths = [root]
    for component in relative.parts[:-1]:
        current = current / component
        ancestor_paths.append(current)
        metadata = current.lstat()
        if not stat.S_ISDIR(metadata.st_mode):
            raise EvidenceError(
                "untracked source has a non-directory ancestor"
            )
    ancestor_identities = tuple(
        (
            metadata.st_dev,
            metadata.st_ino,
            metadata.st_mode,
        )
        for metadata in (ancestor.lstat() for ancestor in ancestor_paths)
    )
    path = root / relative
    before = path.lstat()
    file_identity = (
        before.st_dev,
        before.st_ino,
        before.st_mode,
        before.st_size,
        before.st_mtime_ns,
        before.st_ctime_ns,
    )
    mode = stat.S_IMODE(before.st_mode).to_bytes(4, "big")
    if stat.S_ISLNK(before.st_mode):
        target = os.fsencode(os.readlink(path))
        after = path.lstat()
        if file_identity != (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        ):
            raise EvidenceError("untracked symlink changed while hashing")
        if ancestor_identities != tuple(
            (
                metadata.st_dev,
                metadata.st_ino,
                metadata.st_mode,
            )
            for metadata in (
                ancestor.lstat() for ancestor in ancestor_paths
            )
        ):
            raise EvidenceError(
                "untracked source ancestor changed while hashing"
            )
        if len(path_bytes) + len(target) > remaining:
            raise EvidenceError("untracked source content is oversized")
        return (
            b"symlink",
            mode,
            target,
            ancestor_identities,
            file_identity,
        )
    if not stat.S_ISREG(before.st_mode):
        raise EvidenceError("untracked source is not a regular file")
    if before.st_size > remaining - len(path_bytes):
        raise EvidenceError("untracked source content is oversized")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    no_follow = getattr(os, "O_NOFOLLOW", 0)
    if isinstance(no_follow, int):
        flags |= no_follow
    flags |= getattr(os, "O_BINARY", 0)
    descriptor = os.open(path, flags)
    with _managed_fd(descriptor):
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or (opened.st_dev, opened.st_ino)
            != (before.st_dev, before.st_ino)
        ):
            raise EvidenceError("untracked source identity changed")
        chunks: list[bytes] = []
        retained = 0
        limit = remaining - len(path_bytes)
        while True:
            chunk = os.read(descriptor, min(64 * 1024, limit - retained + 1))
            if not chunk:
                break
            retained += len(chunk)
            if retained > limit:
                raise EvidenceError("untracked source content is oversized")
            chunks.append(chunk)
        after = os.fstat(descriptor)
        if (
            opened.st_dev,
            opened.st_ino,
            opened.st_mode,
            opened.st_size,
            opened.st_mtime_ns,
            opened.st_ctime_ns,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        ):
            raise EvidenceError("untracked source changed while hashing")
        data = b"".join(chunks)
        if len(data) != opened.st_size:
            raise EvidenceError("untracked source read was incomplete")
        if ancestor_identities != tuple(
            (
                metadata.st_dev,
                metadata.st_ino,
                metadata.st_mode,
            )
            for metadata in (
                ancestor.lstat() for ancestor in ancestor_paths
            )
        ):
            raise EvidenceError(
                "untracked source ancestor changed while hashing"
            )
        return (
            b"regular",
            mode,
            data,
            ancestor_identities,
            file_identity,
        )


def _git_result_text(
    result: object,
    *,
    where: str,
) -> str:
    if (
        getattr(result, "returncode", 1) != 0
        or getattr(result, "stderr", "")
        or getattr(result, "stdout_truncated", True)
        or getattr(result, "stderr_truncated", True)
    ):
        raise EvidenceError(f"{where} command failed")
    text = getattr(result, "stdout", None)
    if not isinstance(text, str) or "\ufffd" in text:
        raise EvidenceError(f"{where} output is not strict UTF-8")
    return text


def _capture_git_source_snapshot(
    run_process: Callable[..., object],
    *,
    cwd: Path | None,
) -> tuple[
    str,
    Path,
    tuple[int, int, int],
    bytes,
    bytes,
    bytes,
    tuple[
        tuple[
            bytes,
            bytes,
            bytes,
            bytes,
            tuple[tuple[int, int, int], ...],
            tuple[int, int, int, int, int, int],
        ],
        ...,
    ],
]:
    root_text = _git_result_text(
        run_process(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=cwd,
            timeout=5.0,
            max_output_bytes=4096,
        ),
        where="git root",
    ).rstrip("\n")
    if (
        not root_text
        or "\n" in root_text
        or "\x00" in root_text
        or "\ufffd" in root_text
    ):
        raise EvidenceError("unsafe Git root")
    root = Path(root_text).resolve(strict=True)
    root_before = root.lstat()
    if not stat.S_ISDIR(root_before.st_mode):
        raise EvidenceError("Git root is not a directory")
    root_identity = (
        root_before.st_dev,
        root_before.st_ino,
        root_before.st_mode,
    )
    head_before = _git_result_text(
        run_process(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=root,
            timeout=5.0,
            max_output_bytes=4096,
        ),
        where="Git HEAD",
    ).rstrip("\n")
    if _HEX40_RE.fullmatch(head_before) is None:
        raise EvidenceError("Git HEAD is not one 40-character object ID")
    worktree_text = _git_result_text(
        run_process(
            ["git", "diff", "--binary", "--no-ext-diff", "--"],
            cwd=root,
            timeout=10.0,
            max_output_bytes=MAX_SOURCE_STATE_BYTES,
        ),
        where="Git worktree diff",
    )
    index_text = _git_result_text(
        run_process(
            [
                "git",
                "diff",
                "--binary",
                "--no-ext-diff",
                "--cached",
                "HEAD",
                "--",
            ],
            cwd=root,
            timeout=10.0,
            max_output_bytes=MAX_SOURCE_STATE_BYTES,
        ),
        where="Git index diff",
    )
    inventory_text = _git_result_text(
        run_process(
            [
                "git",
                "ls-files",
                "--others",
                "--exclude-standard",
                "-z",
            ],
            cwd=root,
            timeout=10.0,
            max_output_bytes=MAX_SOURCE_STATE_BYTES,
        ),
        where="Git untracked inventory",
    )
    if inventory_text:
        if not inventory_text.endswith("\x00"):
            raise EvidenceError("invalid untracked source inventory")
        untracked_names = inventory_text[:-1].split("\x00")
    else:
        untracked_names = []
    encoded_names = [name.encode("utf-8") for name in untracked_names]
    if (
        len(set(untracked_names)) != len(untracked_names)
        or encoded_names != sorted(encoded_names)
    ):
        raise EvidenceError("untracked source inventory is not canonical")
    worktree = worktree_text.encode("utf-8")
    index = index_text.encode("utf-8")
    inventory = inventory_text.encode("utf-8")
    consumed = len(worktree) + len(index) + len(inventory)
    if consumed > MAX_SOURCE_STATE_BYTES:
        raise EvidenceError("Git source state is oversized")
    entries = []
    for name, path_bytes in zip(
        untracked_names,
        encoded_names,
        strict=True,
    ):
        kind, mode, data, ancestors, file_identity = (
            _read_untracked_source(
                root,
                name,
                remaining=MAX_SOURCE_STATE_BYTES - consumed,
            )
        )
        consumed += len(path_bytes) + len(mode) + len(data)
        if consumed > MAX_SOURCE_STATE_BYTES:
            raise EvidenceError("Git source state is oversized")
        entries.append(
            (
                path_bytes,
                kind,
                mode,
                data,
                ancestors,
                file_identity,
            )
        )
    root_after = root.lstat()
    if root_identity != (
        root_after.st_dev,
        root_after.st_ino,
        root_after.st_mode,
    ):
        raise EvidenceError("Git root changed during source capture")
    head_after = _git_result_text(
        run_process(
            ["git", "rev-parse", "--verify", "HEAD"],
            cwd=root,
            timeout=5.0,
            max_output_bytes=4096,
        ),
        where="Git HEAD",
    ).rstrip("\n")
    if head_after != head_before:
        raise EvidenceError("Git HEAD changed during source capture")
    return (
        head_before,
        root,
        root_identity,
        worktree,
        index,
        inventory,
        tuple(entries),
    )


def _git_snapshot_dirty_digest(
    snapshot: tuple[
        str,
        Path,
        tuple[int, int, int],
        bytes,
        bytes,
        bytes,
        tuple[
            tuple[
                bytes,
                bytes,
                bytes,
                bytes,
                tuple[tuple[int, int, int], ...],
                tuple[int, int, int, int, int, int],
            ],
            ...,
        ],
    ],
) -> str:
    _, _, _, worktree, index, _, entries = snapshot
    if not worktree and not index and not entries:
        return "clean"
    digest = hashlib.sha256()
    digest.update(b"llam-source-dirty-v2\x00")
    _source_hash_frame(digest, b"worktree-diff", worktree)
    _source_hash_frame(digest, b"index-diff", index)
    for path_bytes, kind, mode, data, _, _ in entries:
        _source_hash_frame(digest, b"path", path_bytes)
        _source_hash_frame(digest, b"kind", kind)
        _source_hash_frame(digest, b"mode", mode)
        _source_hash_frame(digest, b"content", data)
    return digest.hexdigest()


def git_source_provenance(
    run_process: Callable[..., object],
    *,
    cwd: Path | None = None,
) -> tuple[str, str]:
    """Capture one atomic, stable ``(HEAD, dirty digest)`` pair.

    Ignored files are excluded by Git.  Untracked paths and contents use
    explicit length framing so distinct inventories cannot collide through
    concatenation.  HEAD brackets every full snapshot, and two complete
    snapshots must match so a commit from one state cannot be paired with a
    dirty digest from another.
    """

    first = _capture_git_source_snapshot(run_process, cwd=cwd)
    second = _capture_git_source_snapshot(run_process, cwd=cwd)
    if first != second:
        raise EvidenceError("Git source changed between full snapshots")
    return (first[0], _git_snapshot_dirty_digest(first))


def git_source_dirty_digest(
    run_process: Callable[..., object],
    *,
    cwd: Path | None = None,
) -> str:
    """Hash tracked and untracked Git source state, or return unavailable."""

    try:
        _, dirty_digest = git_source_provenance(
            run_process,
            cwd=cwd,
        )
    except (
        EvidenceError,
        OSError,
        RuntimeError,
        UnicodeError,
        ValueError,
    ):
        return "unavailable"
    return dirty_digest


def _require_no_follow() -> int:
    flag = getattr(os, "O_NOFOLLOW", 0)
    if not isinstance(flag, int) or flag == 0:
        raise UnsupportedPlatformError(
            "O_NOFOLLOW is required for evidence bundles"
        )
    return flag


def _check_json_safe(value: object, *, where: str) -> None:
    nodes = 0
    string_bytes = 0
    active_containers: set[int] = set()
    stack: list[tuple[object, int, str, bool]] = [
        (value, 0, where, False)
    ]
    while stack:
        item, depth, location, exiting = stack.pop()
        if exiting:
            active_containers.remove(id(item))
            continue
        nodes += 1
        if nodes > MAX_JSON_NODES:
            raise EvidenceError(
                f"{where} JSON structure is too complex"
            )
        if item is None or isinstance(item, bool):
            continue
        if isinstance(item, int):
            continue
        if isinstance(item, float):
            if not math.isfinite(item):
                raise EvidenceError(
                    f"{location} contains NaN or infinity"
                )
            continue
        if isinstance(item, str):
            if "\x00" in item:
                raise EvidenceError(
                    f"{location} contains a NUL-bearing string"
                )
            try:
                string_bytes += len(item.encode("utf-8"))
            except UnicodeError as exc:
                raise EvidenceError(
                    f"{location} is not valid UTF-8 text"
                ) from exc
            if string_bytes > MAX_JSON_STRING_BYTES:
                raise EvidenceError(
                    f"{where} JSON strings are too large"
                )
            continue
        if not isinstance(item, (list, dict)):
            raise EvidenceError(
                f"{location} contains unsupported JSON type "
                f"{type(item).__name__}"
            )
        if depth >= MAX_JSON_DEPTH:
            raise EvidenceError(
                f"{where} JSON structure is too deep"
            )
        identity = id(item)
        if identity in active_containers:
            raise EvidenceError(f"{where} contains a JSON cycle")
        active_containers.add(identity)
        stack.append((item, depth, location, True))
        if isinstance(item, list):
            for index in range(len(item) - 1, -1, -1):
                stack.append(
                    (
                        item[index],
                        depth + 1,
                        f"{location}[{index}]",
                        False,
                    )
                )
            continue
        for key, child in reversed(tuple(item.items())):
            nodes += 1
            if nodes > MAX_JSON_NODES:
                raise EvidenceError(
                    f"{where} JSON structure is too complex"
                )
            if (
                not isinstance(key, str)
                or not key
                or "\x00" in key
            ):
                raise EvidenceError(
                    f"{location} contains an invalid object key"
                )
            try:
                string_bytes += len(key.encode("utf-8"))
            except UnicodeError as exc:
                raise EvidenceError(
                    f"{location} contains an invalid object key"
                ) from exc
            if string_bytes > MAX_JSON_STRING_BYTES:
                raise EvidenceError(
                    f"{where} JSON strings are too large"
                )
            stack.append(
                (
                    child,
                    depth + 1,
                    f"{location}.{key}",
                    False,
                )
            )


def _require_exact_fields(
    value: Mapping[str, object],
    expected: frozenset[str],
    *,
    where: str,
) -> None:
    actual = frozenset(value)
    if actual != expected:
        raise EvidenceError(
            f"{where} fields mismatch: "
            f"missing={sorted(expected - actual)} "
            f"unknown={sorted(actual - expected)}"
        )


def _validate_metadata(value: object) -> dict[str, object]:
    if not isinstance(value, dict):
        raise EvidenceError("metadata must be a JSON object")
    _require_exact_fields(value, _METADATA_FIELDS, where="metadata")
    if value["schema"] != EVIDENCE_SCHEMA:
        raise EvidenceError(f"metadata schema must be {EVIDENCE_SCHEMA}")
    source_commit = value["source_commit"]
    if (
        not isinstance(source_commit, str)
        or _HEX40_RE.fullmatch(source_commit) is None
    ):
        raise EvidenceError(
            "metadata source_commit must be 40 lowercase hex characters"
        )
    dirty_digest = value["source_dirty_digest"]
    if not (
        dirty_digest == "clean"
        or (
            isinstance(dirty_digest, str)
            and _HEX64_RE.fullmatch(dirty_digest) is not None
        )
    ):
        raise EvidenceError(
            "metadata source_dirty_digest must be clean or 64 lowercase "
            "hex characters"
        )
    architecture = value["architecture"]
    if (
        not isinstance(architecture, str)
        or _ARCHITECTURE_RE.fullmatch(architecture) is None
        or normalize_architecture(architecture) != architecture
    ):
        raise EvidenceError("metadata architecture is not normalized")
    for field in ("kernel", "toolchain"):
        item = value[field]
        if (
            not isinstance(item, str)
            or not item.strip()
            or any(ord(character) < 0x20 for character in item)
        ):
            raise EvidenceError(f"metadata {field} must be nonempty")
    commands = value["commands"]
    if not isinstance(commands, list):
        raise EvidenceError("metadata commands must be a list")
    for index, command in enumerate(commands):
        if (
            not isinstance(command, list)
            or not command
            or not all(
                isinstance(argument, str)
                for argument in command
            )
        ):
            raise EvidenceError(
                f"metadata commands[{index}] must be a nonempty argv list"
            )
    for field in ("cpu_policy", "matrix", "sample_schedule"):
        if not isinstance(value[field], dict):
            raise EvidenceError(f"metadata {field} must be an object")
    classifier = value["classifier"]
    if not isinstance(classifier, dict):
        raise EvidenceError("metadata classifier must be an object")
    _require_exact_fields(
        classifier,
        _CLASSIFIER_FIELDS,
        where="metadata classifier",
    )
    if classifier["schema"] != CLASSIFIER_SCHEMA:
        raise EvidenceError(
            f"classifier schema must be {CLASSIFIER_SCHEMA}"
        )
    if not isinstance(classifier["thresholds"], dict):
        raise EvidenceError("classifier thresholds must be an object")
    _check_json_safe(value, where="metadata")
    canonical_json_bytes(value)
    return value


def _validate_verdict(value: object) -> dict[str, object]:
    if not isinstance(value, dict):
        raise EvidenceError("verdict must be a JSON object")
    fields = frozenset(value)
    if fields not in {
        _LEGACY_VERDICT_FIELDS,
        _SCOPED_VERDICT_FIELDS,
    }:
        expected = (
            _LEGACY_VERDICT_FIELDS
            if not fields.intersection(
                _SCOPED_VERDICT_FIELDS
                - _LEGACY_VERDICT_FIELDS
            )
            else _SCOPED_VERDICT_FIELDS
        )
        _require_exact_fields(value, expected, where="verdict")
    expected_schema = (
        SCOPED_VERDICT_SCHEMA
        if fields == _SCOPED_VERDICT_FIELDS
        else VERDICT_SCHEMA
    )
    if value["schema"] != expected_schema:
        raise EvidenceError(
            f"verdict schema must be {expected_schema}"
        )
    verdict = value["verdict"]
    recognized = {"SPECIALIZED", "REJECT", "INCONCLUSIVE"}
    if verdict not in recognized:
        raise EvidenceError("verdict value is not recognized")
    reasons = value["reasons"]
    if (
        not isinstance(reasons, list)
        or not reasons
        or not all(
            isinstance(reason, str)
            and bool(reason.strip())
            and not any(ord(character) < 0x20 for character in reason)
            for reason in reasons
        )
    ):
        raise EvidenceError("verdict reasons must be nonempty strings")
    if fields == _SCOPED_VERDICT_FIELDS:
        portable_verdict = value["portable_verdict"]
        platform_verdict = value["platform_verdict"]
        if (
            portable_verdict not in recognized
            or platform_verdict not in recognized
        ):
            raise EvidenceError(
                "scoped verdict value is not recognized"
            )
        if verdict != portable_verdict:
            raise EvidenceError(
                "verdict must equal portable_verdict"
            )
        required_cells = value["required_cells"]
        if not isinstance(required_cells, list) or not required_cells:
            raise EvidenceError(
                "verdict required_cells must be a nonempty list"
            )
        observed_cells: set[tuple[object, ...]] = set()
        for cell in required_cells:
            if not isinstance(cell, dict):
                raise EvidenceError(
                    "verdict required cell must be an object"
                )
            _require_exact_fields(
                cell,
                _REQUIRED_CELL_FIELDS,
                where="verdict required cell",
            )
            candidate = cell["candidate"]
            dimensions = (
                cell["batch_width"],
                cell["concurrency"],
                cell["payload"],
            )
            if (
                not isinstance(candidate, str)
                or not candidate
                or any(
                    isinstance(dimension, bool)
                    or not isinstance(dimension, int)
                    or dimension <= 0
                    for dimension in dimensions
                )
            ):
                raise EvidenceError(
                    "verdict required cell is invalid"
                )
            identity = (candidate, *dimensions)
            if identity in observed_cells:
                raise EvidenceError(
                    "verdict required_cells contains a duplicate"
                )
            observed_cells.add(identity)
        thresholds = value["classifier_thresholds"]
        if not isinstance(thresholds, dict) or not thresholds:
            raise EvidenceError(
                "verdict classifier_thresholds must be a nonempty object"
            )
    _check_json_safe(value, where="verdict")
    return value


def _strict_json_object(
    data: bytes,
    *,
    where: str,
) -> dict[str, object]:
    try:
        text = data.decode("utf-8")
    except UnicodeError as exc:
        raise EvidenceError(f"{where} is not strict UTF-8") from exc

    def reject_constant(token: str) -> object:
        raise EvidenceError(f"{where} contains invalid constant {token}")

    try:
        value = json.loads(text, parse_constant=reject_constant)
    except (
        json.JSONDecodeError,
        UnicodeError,
        RecursionError,
    ) as exc:
        raise EvidenceError(f"{where} is invalid JSON: {exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceError(f"{where} must be a JSON object")
    if canonical_json_bytes(value) != data:
        raise EvidenceError(f"{where} is not canonical JSON")
    return value


def _validate_csv(data: bytes, *, where: str) -> None:
    try:
        text = data.decode("utf-8")
    except UnicodeError as exc:
        raise EvidenceError(f"{where} is not strict UTF-8") from exc
    if "\x00" in text:
        raise EvidenceError(f"{where} contains NUL")
    try:
        rows = csv.reader(io.StringIO(text, newline=""), strict=True)
        header = next(rows)
    except (csv.Error, StopIteration) as exc:
        raise EvidenceError(f"{where} lacks a valid CSV header") from exc
    if (
        not header
        or len(set(header)) != len(header)
        or any(
            not field.strip()
            or field != field.strip()
            or any(ord(character) < 0x20 for character in field)
            for field in header
        )
    ):
        raise EvidenceError(f"{where} has invalid or duplicate CSV keys")
    try:
        for line_number, row in enumerate(rows, start=2):
            if len(row) != len(header):
                raise EvidenceError(
                    f"{where} row {line_number} has the wrong cell count"
                )
            for cell in row:
                stripped = cell.strip()
                if not stripped:
                    raise EvidenceError(
                        f"{where} row {line_number} has an empty cell"
                    )
                if stripped.lower() in _NONFINITE_CELLS:
                    raise EvidenceError(
                        f"{where} row {line_number} has a non-finite cell"
                    )
    except csv.Error as exc:
        raise EvidenceError(f"{where} is malformed CSV") from exc


def _validate_report(data: bytes) -> None:
    try:
        text = data.decode("utf-8")
    except UnicodeError as exc:
        raise EvidenceError("report.md is not strict UTF-8") from exc
    if not text.strip() or "\x00" in text:
        raise EvidenceError("report.md must be nonempty UTF-8 text")


def _validate_entry_name(name: object, *, caller_owned: bool) -> str:
    if not isinstance(name, str):
        raise EvidenceError("artifact name must be a string")
    if (
        not name
        or name in {".", ".."}
        or os.path.isabs(name)
        or "/" in name
        or "\\" in name
        or any(ord(character) < 0x20 for character in name)
    ):
        raise EvidenceError(f"unsafe artifact name: {name!r}")
    allowed = _CALLER_ARTIFACTS if caller_owned else _ALL_ARTIFACTS
    if name not in allowed:
        if name in _LIBRARY_ARTIFACTS:
            raise EvidenceError(f"{name} is library-owned")
        raise EvidenceError(f"unknown artifact name: {name}")
    return name


def _validate_bundle_leaf(name: str) -> None:
    if (
        not name
        or name in {".", ".."}
        or "/" in name
        or "\\" in name
        or any(ord(character) < 0x20 for character in name)
    ):
        raise EvidenceError(f"unsafe bundle directory name: {name!r}")


def _safe_absolute(path: Path | str) -> Path:
    raw = os.fspath(path)
    if not raw or "\x00" in raw:
        raise EvidenceError("bundle path is invalid")
    return Path(os.path.abspath(raw))


def _open_directory_nofollow(path: Path) -> int:
    nofollow = _require_no_follow()
    if os.name != "posix":
        raise UnsupportedPlatformError(
            "secure directory traversal requires POSIX dirfd support"
        )
    flags = (
        os.O_RDONLY
        | nofollow
        | getattr(os, "O_DIRECTORY", 0)
        | getattr(os, "O_CLOEXEC", 0)
    )
    absolute = _safe_absolute(path)
    try:
        descriptor = os.open(os.sep, flags)
    except OSError as exc:
        raise EvidenceError(f"cannot open filesystem root: {exc}") from exc
    try:
        for component in absolute.parts[1:]:
            try:
                next_descriptor = os.open(
                    component,
                    flags,
                    dir_fd=descriptor,
                )
            except OSError as exc:
                raise EvidenceError(
                    f"unsafe or unavailable directory ancestor "
                    f"{absolute}: {exc}"
                ) from exc
            os.close(descriptor)
            descriptor = next_descriptor
        metadata = os.fstat(descriptor)
        if not stat.S_ISDIR(metadata.st_mode):
            raise EvidenceError(f"{absolute} is not a directory")
        return descriptor
    except BaseException:
        os.close(descriptor)
        raise


class _PosixDirectoryChain:
    """Retained root-to-directory bindings for pathname authority."""

    def __init__(
        self,
        path: Path,
        descriptors: list[int],
        component_names: tuple[str, ...],
        identities: tuple[tuple[int, int], ...],
        owners: tuple[int, ...],
    ) -> None:
        self.path = path
        self.descriptors = descriptors
        self.component_names = component_names
        self.identities = identities
        self.owners = owners
        self._closed = False

    @property
    def leaf_fd(self) -> int:
        if self._closed or not self.descriptors:
            raise RuntimeError("POSIX directory chain is closed")
        return self.descriptors[-1]

    @property
    def leaf_identity(self) -> tuple[int, int]:
        return self.identities[-1]

    def revalidate(self) -> None:
        if self._closed:
            raise EvidenceError("POSIX directory chain is closed")
        if not hasattr(os, "geteuid"):
            raise UnsupportedPlatformError(
                "effective uid is required for POSIX directory bindings"
            )
        effective_uid = os.geteuid()
        for child_index in range(
            len(self.descriptors) - 1,
            0,
            -1,
        ):
            parent_fd = self.descriptors[child_index - 1]
            child_fd = self.descriptors[child_index]
            parent_metadata = os.fstat(parent_fd)
            child_metadata = os.fstat(child_fd)
            if (
                not stat.S_ISDIR(parent_metadata.st_mode)
                or _posix_identity(parent_metadata)
                != self.identities[child_index - 1]
                or parent_metadata.st_uid
                != self.owners[child_index - 1]
            ):
                raise EvidenceError(
                    "POSIX ancestor identity changed during revalidation"
                )
            if (
                not stat.S_ISDIR(child_metadata.st_mode)
                or _posix_identity(child_metadata)
                != self.identities[child_index]
                or child_metadata.st_uid != self.owners[child_index]
                or child_metadata.st_uid not in {0, effective_uid}
            ):
                raise EvidenceError(
                    "POSIX child binding identity or owner changed"
                )
            _require_posix_rename_authority(
                parent_fd,
                expected_stage_uid=child_metadata.st_uid,
            )
            if (
                _named_posix_identity(
                    parent_fd,
                    self.component_names[child_index - 1],
                )
                != self.identities[child_index]
            ):
                raise EvidenceError(
                    "POSIX ancestor pathname binding changed"
                )

        root_metadata = os.fstat(self.descriptors[0])
        if (
            not stat.S_ISDIR(root_metadata.st_mode)
            or _posix_identity(root_metadata) != self.identities[0]
            or root_metadata.st_uid != self.owners[0]
            or root_metadata.st_uid not in {0, effective_uid}
        ):
            raise EvidenceError(
                "POSIX filesystem root identity or owner changed"
            )
        flags = (
            os.O_RDONLY
            | _require_no_follow()
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_CLOEXEC", 0)
        )
        try:
            reopened_root = os.open(os.sep, flags)
        except OSError as exc:
            raise EvidenceError(
                f"cannot reopen POSIX filesystem root: {exc}"
            ) from exc
        primary_error: BaseException | None = None
        try:
            reopened_metadata = os.fstat(reopened_root)
            if (
                _posix_identity(reopened_metadata)
                != self.identities[0]
                or reopened_metadata.st_uid != self.owners[0]
            ):
                raise EvidenceError(
                    "POSIX filesystem root pathname binding changed"
                )
        except BaseException as exc:
            primary_error = exc
        try:
            os.close(reopened_root)
        except BaseException as exc:
            if primary_error is None:
                primary_error = exc
        if primary_error is not None:
            raise primary_error

    def close(self) -> BaseException | None:
        if self._closed:
            return None
        self._closed = True
        close_error: BaseException | None = None
        while self.descriptors:
            descriptor = self.descriptors.pop()
            try:
                os.close(descriptor)
            except BaseException as exc:
                if close_error is None:
                    close_error = exc
        return close_error


def _open_posix_directory_chain(
    path: Path | str,
) -> _PosixDirectoryChain:
    if os.name != "posix":
        raise UnsupportedPlatformError(
            "secure directory traversal requires POSIX dirfd support"
        )
    if not hasattr(os, "geteuid"):
        raise UnsupportedPlatformError(
            "effective uid is required for POSIX directory bindings"
        )
    absolute = _safe_absolute(path)
    flags = (
        os.O_RDONLY
        | _require_no_follow()
        | getattr(os, "O_DIRECTORY", 0)
        | getattr(os, "O_CLOEXEC", 0)
    )
    descriptors: list[int] = []
    identities: list[tuple[int, int]] = []
    owners: list[int] = []
    try:
        try:
            root_fd = os.open(os.sep, flags)
        except OSError as exc:
            raise EvidenceError(
                f"cannot open POSIX filesystem root: {exc}"
            ) from exc
        descriptors.append(root_fd)
        root_metadata = os.fstat(root_fd)
        effective_uid = os.geteuid()
        if (
            not stat.S_ISDIR(root_metadata.st_mode)
            or root_metadata.st_uid not in {0, effective_uid}
        ):
            raise EvidenceError(
                "POSIX filesystem root has an untrusted owner"
            )
        identities.append(_posix_identity(root_metadata))
        owners.append(root_metadata.st_uid)
        for component in absolute.parts[1:]:
            parent_fd = descriptors[-1]
            try:
                child_fd = os.open(
                    component,
                    flags,
                    dir_fd=parent_fd,
                )
            except OSError as exc:
                raise EvidenceError(
                    "unsafe or unavailable POSIX directory ancestor "
                    f"{absolute}: {exc}"
                ) from exc
            descriptors.append(child_fd)
            child_metadata = os.fstat(child_fd)
            if (
                not stat.S_ISDIR(child_metadata.st_mode)
                or child_metadata.st_uid not in {0, effective_uid}
            ):
                raise EvidenceError(
                    "POSIX directory binding has an untrusted owner"
                )
            child_identity = _posix_identity(child_metadata)
            _require_posix_rename_authority(
                parent_fd,
                expected_stage_uid=child_metadata.st_uid,
            )
            if (
                _named_posix_identity(parent_fd, component)
                != child_identity
            ):
                raise EvidenceError(
                    "POSIX directory name does not match held child"
                )
            identities.append(child_identity)
            owners.append(child_metadata.st_uid)
        return _PosixDirectoryChain(
            absolute,
            descriptors,
            tuple(absolute.parts[1:]),
            tuple(identities),
            tuple(owners),
        )
    except BaseException:
        while descriptors:
            try:
                os.close(descriptors.pop())
            except BaseException:
                pass
        raise


def _fsync_directory(descriptor: int) -> None:
    os.fsync(descriptor)


def _write_all(descriptor: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise OSError(errno.EIO, "artifact write made no progress")
        view = view[written:]


@contextmanager
def _managed_fd(descriptor: int) -> object:
    try:
        yield descriptor
    except BaseException:
        try:
            os.close(descriptor)
        except OSError:
            pass
        raise
    else:
        os.close(descriptor)


_WIN_GENERIC_READ = 0x80000000
_WIN_GENERIC_WRITE = 0x40000000
_WIN_DELETE = 0x00010000
_WIN_READ_CONTROL = 0x00020000
_WIN_FILE_READ_ATTRIBUTES = 0x00000080
_WIN_FILE_SHARE_READ = 0x00000001
_WIN_FILE_SHARE_WRITE = 0x00000002
_WIN_FILE_SHARE_DELETE = 0x00000004
_WIN_CREATE_NEW = 1
_WIN_OPEN_EXISTING = 3
_WIN_FILE_ATTRIBUTE_DIRECTORY = 0x00000010
_WIN_FILE_ATTRIBUTE_NORMAL = 0x00000080
_WIN_FILE_ATTRIBUTE_REPARSE_POINT = 0x00000400
_WIN_FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000
_WIN_FILE_FLAG_BACKUP_SEMANTICS = 0x02000000
_WIN_ERROR_FILE_NOT_FOUND = 2
_WIN_ERROR_PATH_NOT_FOUND = 3
_WIN_ERROR_ACCESS_DENIED = 5
_WIN_ERROR_INVALID_FUNCTION = 1
_WIN_ERROR_NOT_SUPPORTED = 50
_WIN_ERROR_INVALID_PARAMETER = 87
_WIN_ERROR_INSUFFICIENT_BUFFER = 122
_WIN_ERROR_FILE_EXISTS = 80
_WIN_ERROR_ALREADY_EXISTS = 183
_WIN_SE_FILE_OBJECT = 1
_WIN_DACL_SECURITY_INFORMATION = 0x00000004
_WIN_SDDL_REVISION_1 = 1
_WIN_SE_DACL_PROTECTED = 0x1000
_WIN_TOKEN_QUERY = 0x0008
_WIN_TOKEN_USER = 1
_WIN_FILE_ID_INFO = 18
_WIN_FILE_RENAME_INFO = 3
_WIN_FILE_RENAME_INFO_EX = 22


class _WinSecurityAttributes(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_uint32),
        ("security_descriptor", ctypes.c_void_p),
        ("inherit_handle", ctypes.c_int),
    ]


class _WinSidAndAttributes(ctypes.Structure):
    _fields_ = [
        ("sid", ctypes.c_void_p),
        ("attributes", ctypes.c_uint32),
    ]


class _WinTokenUser(ctypes.Structure):
    _fields_ = [("user", _WinSidAndAttributes)]


class _WinFileRenameInfoEx(ctypes.Structure):
    _fields_ = [
        ("flags", ctypes.c_uint32),
        ("root_directory", ctypes.c_void_p),
        ("file_name_length", ctypes.c_uint32),
        ("file_name", ctypes.c_uint16 * 1),
    ]


class _WinFileRenameInfo(ctypes.Structure):
    _fields_ = [
        ("replace_if_exists", ctypes.c_ubyte),
        ("root_directory", ctypes.c_void_p),
        ("file_name_length", ctypes.c_uint32),
        ("file_name", ctypes.c_uint16 * 1),
    ]


class _WinFileTime(ctypes.Structure):
    _fields_ = [
        ("low", ctypes.c_uint32),
        ("high", ctypes.c_uint32),
    ]


class _WinFileId128(ctypes.Structure):
    _fields_ = [("identifier", ctypes.c_ubyte * 16)]


class _WinFileIdInfo(ctypes.Structure):
    _fields_ = [
        ("volume_serial_number", ctypes.c_uint64),
        ("file_id", _WinFileId128),
    ]


class _WinByHandleFileInformation(ctypes.Structure):
    _fields_ = [
        ("file_attributes", ctypes.c_uint32),
        ("creation_time", _WinFileTime),
        ("last_access_time", _WinFileTime),
        ("last_write_time", _WinFileTime),
        ("volume_serial_number", ctypes.c_uint32),
        ("file_size_high", ctypes.c_uint32),
        ("file_size_low", ctypes.c_uint32),
        ("number_of_links", ctypes.c_uint32),
        ("file_index_high", ctypes.c_uint32),
        ("file_index_low", ctypes.c_uint32),
    ]

    @property
    def size(self) -> int:
        return (self.file_size_high << 32) | self.file_size_low

    @property
    def write_time(self) -> tuple[int, int]:
        return (self.last_write_time.high, self.last_write_time.low)


class _WindowsAPI:
    """Narrow ctypes boundary for fail-closed Win32 filesystem access."""

    def __init__(self) -> None:
        win_dll = getattr(ctypes, "WinDLL", None)
        if win_dll is None:
            raise UnsupportedPlatformError("ctypes WinDLL is unavailable")
        self._kernel32 = win_dll("kernel32", use_last_error=True)
        self._advapi32 = win_dll("advapi32", use_last_error=True)
        self._cached_current_user_sid: str | None = None
        self._configure()

    def _configure(self) -> None:
        self._kernel32.CreateFileW.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self._kernel32.CreateFileW.restype = ctypes.c_void_p
        self._kernel32.CreateDirectoryW.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_void_p,
        ]
        self._kernel32.CreateDirectoryW.restype = ctypes.c_int
        self._kernel32.GetFileInformationByHandle.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_WinByHandleFileInformation),
        ]
        self._kernel32.GetFileInformationByHandle.restype = ctypes.c_int
        self._kernel32.GetFileInformationByHandleEx.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
        self._kernel32.GetFileInformationByHandleEx.restype = ctypes.c_int
        self._kernel32.GetVolumeInformationByHandleW.argtypes = [
            ctypes.c_void_p,
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_wchar_p,
            ctypes.c_uint32,
        ]
        self._kernel32.GetVolumeInformationByHandleW.restype = ctypes.c_int
        self._kernel32.WriteFile.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_void_p,
        ]
        self._kernel32.WriteFile.restype = ctypes.c_int
        self._kernel32.ReadFile.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_void_p,
        ]
        self._kernel32.ReadFile.restype = ctypes.c_int
        self._kernel32.FlushFileBuffers.argtypes = [ctypes.c_void_p]
        self._kernel32.FlushFileBuffers.restype = ctypes.c_int
        self._kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        self._kernel32.CloseHandle.restype = ctypes.c_int
        self._kernel32.GetCurrentProcess.argtypes = []
        self._kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        self._kernel32.SetFileInformationByHandle.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_uint32,
        ]
        self._kernel32.SetFileInformationByHandle.restype = ctypes.c_int
        self._kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        self._kernel32.LocalFree.restype = ctypes.c_void_p
        self._advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW.restype = (
            ctypes.c_int
        )
        self._advapi32.GetSecurityInfo.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self._advapi32.GetSecurityInfo.restype = ctypes.c_uint32
        self._advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_wchar_p),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW.restype = (
            ctypes.c_int
        )
        self._advapi32.GetSecurityDescriptorControl.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint16),
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._advapi32.GetSecurityDescriptorControl.restype = ctypes.c_int
        self._advapi32.OpenProcessToken.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self._advapi32.OpenProcessToken.restype = ctypes.c_int
        self._advapi32.GetTokenInformation.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
        ]
        self._advapi32.GetTokenInformation.restype = ctypes.c_int
        self._advapi32.ConvertSidToStringSidW.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_wchar_p),
        ]
        self._advapi32.ConvertSidToStringSidW.restype = ctypes.c_int

    @staticmethod
    def _last_error() -> int:
        getter = getattr(ctypes, "get_last_error", None)
        return int(getter()) if getter is not None else errno.EIO

    @staticmethod
    def _raise_windows_error(
        error_number: int,
        path: Path,
    ) -> None:
        if error_number in {
            _WIN_ERROR_FILE_NOT_FOUND,
            _WIN_ERROR_PATH_NOT_FOUND,
        }:
            raise FileNotFoundError(error_number, "path not found", path)
        if error_number in {
            _WIN_ERROR_FILE_EXISTS,
            _WIN_ERROR_ALREADY_EXISTS,
        }:
            raise FileExistsError(error_number, "path exists", path)
        raise OSError(error_number, "Win32 filesystem operation failed", path)

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
        with self._private_security_attributes(
            enabled=private,
            directory=False,
        ) as security_attributes:
            handle = self._kernel32.CreateFileW(
                os.fspath(path),
                access,
                share,
                security_attributes,
                creation_disposition,
                flags,
                None,
            )
        invalid = ctypes.c_void_p(-1).value
        if handle in {None, invalid}:
            self._raise_windows_error(self._last_error(), path)
        return handle

    @contextmanager
    def _private_security_attributes(
        self,
        *,
        enabled: bool = True,
        directory: bool = False,
    ) -> object:
        if not enabled:
            yield None
            return
        descriptor = ctypes.c_void_p()
        if not self._advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            self._private_dacl_sddl(directory=directory),
            _WIN_SDDL_REVISION_1,
            ctypes.byref(descriptor),
            None,
        ):
            raise OSError(
                self._last_error(),
                "cannot create private Windows security descriptor",
            )
        attributes = _WinSecurityAttributes(
            ctypes.sizeof(_WinSecurityAttributes),
            descriptor,
            0,
        )
        try:
            yield ctypes.byref(attributes)
        finally:
            self._kernel32.LocalFree(descriptor)

    def _current_user_sid(self) -> str:
        cached = getattr(self, "_cached_current_user_sid", None)
        if cached is not None:
            return cached
        token = ctypes.c_void_p()
        if not self._advapi32.OpenProcessToken(
            self._kernel32.GetCurrentProcess(),
            _WIN_TOKEN_QUERY,
            ctypes.byref(token),
        ):
            raise OSError(
                self._last_error(),
                "OpenProcessToken failed",
            )
        primary_error: BaseException | None = None
        try:
            required = ctypes.c_uint32()
            if self._advapi32.GetTokenInformation(
                token,
                _WIN_TOKEN_USER,
                None,
                0,
                ctypes.byref(required),
            ):
                raise OSError(
                    errno.EIO,
                    "GetTokenInformation returned no token buffer",
                )
            error_number = self._last_error()
            if (
                error_number != _WIN_ERROR_INSUFFICIENT_BUFFER
                or required.value < ctypes.sizeof(_WinTokenUser)
            ):
                raise OSError(
                    error_number,
                    "cannot size current Windows token user",
                )
            buffer = ctypes.create_string_buffer(required.value)
            if not self._advapi32.GetTokenInformation(
                token,
                _WIN_TOKEN_USER,
                buffer,
                required.value,
                ctypes.byref(required),
            ):
                raise OSError(
                    self._last_error(),
                    "cannot read current Windows token user",
                )
            token_user = ctypes.cast(
                buffer,
                ctypes.POINTER(_WinTokenUser),
            ).contents
            text = ctypes.c_wchar_p()
            if not self._advapi32.ConvertSidToStringSidW(
                token_user.user.sid,
                ctypes.byref(text),
            ):
                raise OSError(
                    self._last_error(),
                    "cannot stringify current Windows token user SID",
                )
            try:
                sid = text.value
                if not sid:
                    raise OSError(
                        errno.EIO,
                        "current Windows token user SID is empty",
                    )
            finally:
                if text:
                    self._kernel32.LocalFree(
                        ctypes.cast(text, ctypes.c_void_p)
                    )
            self._cached_current_user_sid = sid
            return sid
        except BaseException as exc:
            primary_error = exc
            raise
        finally:
            try:
                self.close(token)
            except OSError:
                if primary_error is None:
                    raise

    def _private_dacl_sddl(self, *, directory: bool = False) -> str:
        ace_flags = "OICI" if directory else ""
        return (
            f"D:P(A;{ace_flags};FA;;;SY)"
            f"(A;{ace_flags};FA;;;{self._current_user_sid()})"
        )

    def create_directory(self, path: Path) -> None:
        with self._private_security_attributes(
            directory=True
        ) as security_attributes:
            if not self._kernel32.CreateDirectoryW(
                os.fspath(path),
                security_attributes,
            ):
                self._raise_windows_error(self._last_error(), path)

    def info(self, handle: object) -> _WinByHandleFileInformation:
        information = _WinByHandleFileInformation()
        if not self._kernel32.GetFileInformationByHandle(
            handle,
            ctypes.byref(information),
        ):
            raise OSError(
                self._last_error(),
                "GetFileInformationByHandle failed",
            )
        return information

    def identity(self, handle: object) -> tuple[int, bytes]:
        information = _WinFileIdInfo()
        if not self._kernel32.GetFileInformationByHandleEx(
            handle,
            _WIN_FILE_ID_INFO,
            ctypes.byref(information),
            ctypes.sizeof(information),
        ):
            raise OSError(
                self._last_error(),
                "GetFileInformationByHandleEx(FileIdInfo) failed",
            )
        return (
            int(information.volume_serial_number),
            bytes(information.file_id.identifier),
        )

    def filesystem_name(self, handle: object) -> str:
        filesystem = ctypes.create_unicode_buffer(64)
        if not self._kernel32.GetVolumeInformationByHandleW(
            handle,
            None,
            0,
            None,
            None,
            None,
            filesystem,
            len(filesystem),
        ):
            raise OSError(
                self._last_error(),
                "GetVolumeInformationByHandleW failed",
            )
        if not filesystem.value:
            raise EvidenceError("Windows filesystem name is empty")
        return filesystem.value

    def require_directory_no_reparse(self, handle: object) -> None:
        information = self.info(handle)
        if (
            not information.file_attributes
            & _WIN_FILE_ATTRIBUTE_DIRECTORY
            or information.file_attributes
            & _WIN_FILE_ATTRIBUTE_REPARSE_POINT
        ):
            raise EvidenceError(
                "Windows directory is not a non-reparse directory"
            )

    def require_regular_single_link(self, handle: object) -> None:
        information = self.info(handle)
        if (
            information.file_attributes
            & (
                _WIN_FILE_ATTRIBUTE_DIRECTORY
                | _WIN_FILE_ATTRIBUTE_REPARSE_POINT
            )
            or information.number_of_links != 1
            or information.size > MAX_FILE_BYTES
        ):
            raise EvidenceError(
                "Windows artifact is not one bounded regular file"
            )

    def _private_dacl_details(
        self,
        handle: object,
    ) -> tuple[str, bool]:
        descriptor = ctypes.c_void_p()
        error_number = int(
            self._advapi32.GetSecurityInfo(
                handle,
                _WIN_SE_FILE_OBJECT,
                _WIN_DACL_SECURITY_INFORMATION,
                None,
                None,
                None,
                None,
                ctypes.byref(descriptor),
            )
        )
        if error_number:
            raise OSError(
                error_number,
                "GetSecurityInfo failed for evidence object",
            )
        text = ctypes.c_wchar_p()
        try:
            control = ctypes.c_uint16()
            revision = ctypes.c_uint32()
            if not self._advapi32.GetSecurityDescriptorControl(
                descriptor,
                ctypes.byref(control),
                ctypes.byref(revision),
            ):
                raise OSError(
                    self._last_error(),
                    "cannot inspect evidence DACL protection",
                )
            if not self._advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW(
                descriptor,
                _WIN_SDDL_REVISION_1,
                _WIN_DACL_SECURITY_INFORMATION,
                ctypes.byref(text),
                None,
            ):
                raise OSError(
                    self._last_error(),
                    "cannot inspect evidence object DACL",
                )
            return (
                text.value or "",
                bool(control.value & _WIN_SE_DACL_PROTECTED),
            )
        finally:
            if text:
                self._kernel32.LocalFree(
                    ctypes.cast(text, ctypes.c_void_p)
                )
            self._kernel32.LocalFree(descriptor)

    def private_dacl_sddl(self, handle: object) -> str:
        return self._private_dacl_details(handle)[0]

    def _canonical_dacl_sddl(self, sddl: str) -> str:
        descriptor = ctypes.c_void_p()
        text = ctypes.c_wchar_p()
        try:
            if not self._advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl,
                _WIN_SDDL_REVISION_1,
                ctypes.byref(descriptor),
                None,
            ):
                raise OSError(
                    self._last_error(),
                    "cannot parse expected private Windows DACL",
                )
            if not self._advapi32.ConvertSecurityDescriptorToStringSecurityDescriptorW(
                descriptor,
                _WIN_SDDL_REVISION_1,
                _WIN_DACL_SECURITY_INFORMATION,
                ctypes.byref(text),
                None,
            ):
                raise OSError(
                    self._last_error(),
                    "cannot canonicalize expected private Windows DACL",
                )
            return text.value or ""
        finally:
            if text:
                self._kernel32.LocalFree(
                    ctypes.cast(text, ctypes.c_void_p)
                )
            if descriptor:
                self._kernel32.LocalFree(descriptor)

    def require_private_acl(self, handle: object) -> None:
        observed, protected = self._private_dacl_details(handle)
        directory = bool(
            self.info(handle).file_attributes
            & _WIN_FILE_ATTRIBUTE_DIRECTORY
        )
        expected = self._private_dacl_sddl(directory=directory)
        canonical_expected = self._canonical_dacl_sddl(expected)
        accepted = {
            expected,
            expected.replace("D:P", "D:", 1),
            canonical_expected,
            canonical_expected.replace("D:P", "D:", 1),
        }
        if not protected or observed not in accepted:
            expected_user_sids = set(
                re.findall(r"S-\d+(?:-\d+)+", expected)
            )
            observed_debug = observed
            expected_debug = expected
            for current_user_sid in sorted(
                expected_user_sids,
                key=len,
                reverse=True,
            ):
                observed_debug = observed_debug.replace(
                    current_user_sid,
                    "<CURRENT_USER>",
                )
                expected_debug = expected_debug.replace(
                    current_user_sid,
                    "<CURRENT_USER>",
                )
            observed_debug = re.sub(
                r"S-\d+(?:-\d+)+",
                "<SID>",
                observed_debug,
            )
            raise EvidenceError(
                "Windows evidence object lacks the exact private DACL: "
                "expected canonical protected SYSTEM and current-user "
                "full-control ACEs; "
                f"observed={observed_debug[:1024]!r}, "
                f"expected={expected_debug!r}, "
                f"protected={protected}"
            )

    @staticmethod
    def _rename_information_buffer(
        header_type: type[ctypes.Structure],
        *,
        parent_handle: object,
        final_name: str,
    ) -> ctypes.Array[ctypes.c_char]:
        encoded_name = final_name.encode("utf-16-le")
        name_offset = header_type.file_name.offset
        buffer = ctypes.create_string_buffer(
            ctypes.sizeof(header_type) + len(encoded_name)
        )
        header = header_type.from_buffer(buffer)
        if isinstance(header, _WinFileRenameInfoEx):
            header.flags = 0
        else:
            header.replace_if_exists = 0
        header.root_directory = parent_handle
        header.file_name_length = len(encoded_name)
        ctypes.memmove(
            ctypes.addressof(buffer) + name_offset,
            encoded_name,
            len(encoded_name),
        )
        return buffer

    def rename_handle_noreplace(
        self,
        stage_handle: object,
        parent_handle: object,
        final_name: str,
    ) -> None:
        _validate_bundle_leaf(final_name)
        attempts = (
            (_WIN_FILE_RENAME_INFO_EX, _WinFileRenameInfoEx),
            (_WIN_FILE_RENAME_INFO, _WinFileRenameInfo),
        )
        for index, (information_class, header_type) in enumerate(
            attempts
        ):
            buffer = self._rename_information_buffer(
                header_type,
                parent_handle=parent_handle,
                final_name=final_name,
            )
            if self._kernel32.SetFileInformationByHandle(
                stage_handle,
                information_class,
                buffer,
                len(buffer),
            ):
                return
            error_number = self._last_error()
            if (
                index == 0
                and error_number
                in {
                    _WIN_ERROR_INVALID_FUNCTION,
                    _WIN_ERROR_NOT_SUPPORTED,
                    _WIN_ERROR_INVALID_PARAMETER,
                }
            ):
                continue
            self._raise_windows_error(
                error_number,
                Path(final_name),
            )
        raise UnsupportedPlatformError(
            "Windows handle-bound no-replace rename is unavailable"
        )

    def directory_identity(
        self,
        path: Path,
    ) -> tuple[int, bytes] | None:
        try:
            handle = self.create_file(
                path,
                creation_disposition=_WIN_OPEN_EXISTING,
                flags=(
                    _WIN_FILE_FLAG_BACKUP_SEMANTICS
                    | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
                ),
                access=_WIN_FILE_READ_ATTRIBUTES,
                share=(
                    _WIN_FILE_SHARE_READ
                    | _WIN_FILE_SHARE_WRITE
                    | _WIN_FILE_SHARE_DELETE
                ),
            )
        except FileNotFoundError:
            return None
        with _win_managed_handle(self, handle):
            self.require_directory_no_reparse(handle)
            return self.identity(handle)

    def write_all(self, handle: object, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            chunk = data[offset : offset + 1024 * 1024]
            buffer = ctypes.create_string_buffer(chunk)
            written = ctypes.c_uint32()
            if not self._kernel32.WriteFile(
                handle,
                buffer,
                len(chunk),
                ctypes.byref(written),
                None,
            ):
                raise OSError(self._last_error(), "WriteFile failed")
            if written.value <= 0 or written.value > len(chunk):
                raise OSError(
                    errno.EIO,
                    "WriteFile returned an invalid byte count",
                )
            offset += written.value

    def read_all(self, handle: object, expected_size: int) -> bytes:
        if expected_size > MAX_FILE_BYTES:
            raise EvidenceError("Windows artifact is oversized")
        chunks: list[bytes] = []
        remaining = expected_size
        while remaining:
            request = min(64 * 1024, remaining)
            buffer = ctypes.create_string_buffer(request)
            read = ctypes.c_uint32()
            if not self._kernel32.ReadFile(
                handle,
                buffer,
                request,
                ctypes.byref(read),
                None,
            ):
                raise OSError(self._last_error(), "ReadFile failed")
            if read.value > request:
                raise OSError(
                    errno.EIO,
                    "ReadFile returned an invalid byte count",
                )
            if read.value == 0:
                break
            chunks.append(buffer.raw[: read.value])
            remaining -= read.value
        return b"".join(chunks)

    def flush(self, handle: object) -> None:
        if not self._kernel32.FlushFileBuffers(handle):
            raise OSError(self._last_error(), "FlushFileBuffers failed")

    def flush_directory(self, handle: object) -> None:
        self.flush(handle)

    def close(self, handle: object) -> None:
        if not self._kernel32.CloseHandle(handle):
            raise OSError(self._last_error(), "CloseHandle failed")


@contextmanager
def _win_managed_handle(
    api: _WindowsAPI,
    handle: object,
) -> object:
    try:
        yield handle
    except BaseException:
        try:
            api.close(handle)
        except OSError:
            pass
        raise
    else:
        api.close(handle)


def _win_create_and_write_file(
    path: Path,
    data: bytes,
    *,
    api: _WindowsAPI,
) -> None:
    handle = api.create_file(
        path,
        creation_disposition=_WIN_CREATE_NEW,
        flags=(
            _WIN_FILE_ATTRIBUTE_NORMAL
            | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
        ),
        access=_WIN_GENERIC_WRITE | _WIN_FILE_READ_ATTRIBUTES,
        share=0,
        private=True,
    )
    with _win_managed_handle(api, handle):
        api.require_regular_single_link(handle)
        api.require_private_acl(handle)
        api.write_all(handle, data)
        api.flush(handle)


def _platform_kind() -> str:
    if sys.platform == "win32":
        return "windows"
    if os.name == "posix":
        return "posix"
    return "unsupported"


def _win_close_handles(
    api: _WindowsAPI,
    handles: list[object],
) -> None:
    while handles:
        handle = handles.pop()
        try:
            api.close(handle)
        except OSError:
            pass


def _win_open_directory_chain(
    path: Path,
    *,
    api: _WindowsAPI,
    require_private_leaf: bool = False,
    writable_leaf: bool = False,
    deny_delete: bool = False,
) -> tuple[Path, list[object]]:
    absolute = _safe_absolute(path)
    parts = absolute.parts
    if not parts or not absolute.anchor:
        raise EvidenceError("Windows bundle path must be absolute")
    current = Path(parts[0])
    handles: list[object] = []
    try:
        for index, component in enumerate(parts):
            if component != parts[0]:
                current = current / component
            is_leaf = index == len(parts) - 1
            handle = api.create_file(
                current,
                creation_disposition=_WIN_OPEN_EXISTING,
                flags=(
                    _WIN_FILE_FLAG_BACKUP_SEMANTICS
                    | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
                ),
                access=(
                    _WIN_FILE_READ_ATTRIBUTES
                    | (
                        _WIN_GENERIC_WRITE
                        if writable_leaf and is_leaf
                        else 0
                    )
                    | (
                        _WIN_READ_CONTROL
                        if require_private_leaf and is_leaf
                        else 0
                    )
                ),
                share=(
                    (
                        _WIN_FILE_SHARE_READ
                        | _WIN_FILE_SHARE_WRITE
                    )
                    if deny_delete
                    else (
                        _WIN_FILE_SHARE_READ
                        | _WIN_FILE_SHARE_WRITE
                        | _WIN_FILE_SHARE_DELETE
                    )
                ),
            )
            try:
                api.require_directory_no_reparse(handle)
                if require_private_leaf and is_leaf:
                    api.require_private_acl(handle)
            except BaseException:
                try:
                    api.close(handle)
                except OSError:
                    pass
                raise
            handles.append(handle)
        return absolute, handles
    except BaseException:
        _win_close_handles(api, handles)
        raise


def _win_open_directory(
    path: Path,
    *,
    api: _WindowsAPI,
    require_private: bool = False,
    writable: bool = False,
    rename_source: bool = False,
) -> object:
    handle = api.create_file(
        path,
        creation_disposition=_WIN_OPEN_EXISTING,
        flags=(
            _WIN_FILE_FLAG_BACKUP_SEMANTICS
            | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
        ),
        access=(
            _WIN_FILE_READ_ATTRIBUTES
            | (_WIN_GENERIC_WRITE if writable else 0)
            | (_WIN_DELETE if rename_source else 0)
            | (_WIN_READ_CONTROL if require_private else 0)
        ),
        share=(
            _WIN_FILE_SHARE_READ
            | _WIN_FILE_SHARE_WRITE
            | _WIN_FILE_SHARE_DELETE
        ),
    )
    try:
        api.require_directory_no_reparse(handle)
        if require_private:
            api.require_private_acl(handle)
    except BaseException:
        try:
            api.close(handle)
        except OSError:
            pass
        raise
    return handle


def _win_read_regular_file(
    path: Path,
    *,
    api: _WindowsAPI,
) -> bytes:
    handle = api.create_file(
        path,
        creation_disposition=_WIN_OPEN_EXISTING,
        flags=(
            _WIN_FILE_ATTRIBUTE_NORMAL
            | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
        ),
        access=(
            _WIN_GENERIC_READ
            | _WIN_FILE_READ_ATTRIBUTES
            | _WIN_READ_CONTROL
        ),
        share=_WIN_FILE_SHARE_READ,
    )
    with _win_managed_handle(api, handle):
        api.require_regular_single_link(handle)
        api.require_private_acl(handle)
        before_identity = api.identity(handle)
        before = api.info(handle)
        data = api.read_all(handle, before.size)
        after = api.info(handle)
        after_identity = api.identity(handle)
        if (
            before_identity != after_identity
            or before.size != after.size
            or before.write_time != after.write_time
            or len(data) != before.size
        ):
            raise EvidenceError(
                f"{path.name} changed while being read"
            )
        return data


def _raise_rename_error(error_number: int, destination: Path) -> None:
    if error_number in {errno.EEXIST, errno.ENOTEMPTY}:
        raise FileExistsError(
            error_number,
            os.strerror(error_number),
            os.fspath(destination),
        )
    raise OSError(
        error_number,
        os.strerror(error_number),
        os.fspath(destination),
    )


def _atomic_rename_noreplace(
    source: Path,
    destination: Path,
    source_parent_fd: int | None,
    destination_parent_fd: int | None,
) -> None:
    """Atomically move a directory while refusing replacement.

    Plain ``os.rename``/``os.replace`` is intentionally forbidden here:
    POSIX rename may replace an existing empty directory.
    """

    if sys.platform.startswith("linux"):
        library = ctypes.CDLL(None, use_errno=True)
        function = getattr(library, "renameat2", None)
        if function is None:
            raise UnsupportedPlatformError(
                "Linux renameat2(RENAME_NOREPLACE) is unavailable"
            )
        function.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        function.restype = ctypes.c_int
        old_fd = (
            source_parent_fd
            if source_parent_fd is not None
            else getattr(os, "AT_FDCWD", -100)
        )
        new_fd = (
            destination_parent_fd
            if destination_parent_fd is not None
            else getattr(os, "AT_FDCWD", -100)
        )
        old_name = (
            source.name if source_parent_fd is not None else os.fspath(source)
        ).encode()
        new_name = (
            destination.name
            if destination_parent_fd is not None
            else os.fspath(destination)
        ).encode()
        result = function(old_fd, old_name, new_fd, new_name, 1)
        if result != 0:
            _raise_rename_error(ctypes.get_errno(), destination)
        return
    if sys.platform == "darwin":
        library = ctypes.CDLL(None, use_errno=True)
        function = getattr(library, "renameatx_np", None)
        if function is None:
            raise UnsupportedPlatformError(
                "Darwin renameatx_np(RENAME_EXCL) is unavailable"
            )
        function.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_uint,
        ]
        function.restype = ctypes.c_int
        at_fdcwd = -2
        old_fd = (
            source_parent_fd
            if source_parent_fd is not None
            else at_fdcwd
        )
        new_fd = (
            destination_parent_fd
            if destination_parent_fd is not None
            else at_fdcwd
        )
        old_name = (
            source.name if source_parent_fd is not None else os.fspath(source)
        ).encode()
        new_name = (
            destination.name
            if destination_parent_fd is not None
            else os.fspath(destination)
        ).encode()
        result = function(old_fd, old_name, new_fd, new_name, 0x00000004)
        if result != 0:
            _raise_rename_error(ctypes.get_errno(), destination)
        return
    if sys.platform == "win32":
        raise UnsupportedPlatformError(
            "Windows publication requires handle-bound rename"
        )
    raise UnsupportedPlatformError(
        f"atomic no-replace directory move unsupported on {sys.platform}"
    )


class EvidenceBundle:
    """Exclusive writer for one immutable evidence bundle."""

    def __init__(
        self,
        final_path: Path,
        stage_path: Path,
        parent_fd: int,
        stage_fd: int,
        parent_identity: tuple[int, int],
        stage_identity: tuple[int, int],
        metadata: dict[str, object],
        parent_chain: _PosixDirectoryChain,
    ) -> None:
        self._final_path = final_path
        self._stage_path = stage_path
        self._parent_fd = parent_fd
        self._stage_fd = stage_fd
        self._parent_identity = parent_identity
        self._stage_identity = stage_identity
        self._parent_chain = parent_chain
        self._metadata = metadata
        self._written: set[str] = set()
        self._active = True
        self._finalize_called = False

    @classmethod
    def create(
        cls,
        final_dir: Path | str,
        metadata: dict[str, object],
    ) -> EvidenceBundle:
        platform_kind = _platform_kind()
        if platform_kind == "windows":
            return _WindowsEvidenceBundle.create(  # type: ignore[return-value]
                final_dir,
                metadata,
            )
        if platform_kind != "posix":
            raise UnsupportedPlatformError(
                f"secure evidence bundles unsupported on {sys.platform}"
            )
        validated_metadata = _validate_metadata(metadata)
        metadata_bytes = canonical_json_bytes(validated_metadata)
        final_path = _safe_absolute(final_dir)
        if final_path.parent == final_path:
            raise EvidenceError("filesystem root cannot be a bundle")
        _validate_bundle_leaf(final_path.name)
        parent_fd = _open_directory_nofollow(final_path.parent)
        parent_chain: _PosixDirectoryChain | None = None
        try:
            parent_identity = _require_posix_rename_authority(parent_fd)
            parent_chain = _open_posix_directory_chain(
                final_path.parent
            )
            if parent_chain.leaf_identity != parent_identity:
                raise EvidenceError(
                    "POSIX parent handles identify different directories"
                )
        except BaseException:
            try:
                os.close(parent_fd)
            except OSError:
                pass
            if parent_chain is not None:
                parent_chain.close()
            raise
        stage_name = (
            f".{final_path.name}.staging-{os.getpid()}-"
            f"{secrets.token_hex(16)}"
        )
        stage_path = final_path.parent / stage_name
        stage_fd: int | None = None
        stage_identity: tuple[int, int] | None = None
        stage_created = False
        bundle_owns_descriptors = False
        try:
            try:
                os.stat(
                    final_path.name,
                    dir_fd=parent_fd,
                    follow_symlinks=False,
                )
            except FileNotFoundError:
                pass
            else:
                raise FileExistsError(
                    errno.EEXIST,
                    os.strerror(errno.EEXIST),
                    os.fspath(final_path),
                )
            os.mkdir(stage_name, mode=0o700, dir_fd=parent_fd)
            stage_created = True
            nofollow = _require_no_follow()
            stage_fd = os.open(
                stage_name,
                os.O_RDONLY
                | nofollow
                | getattr(os, "O_DIRECTORY", 0)
                | getattr(os, "O_CLOEXEC", 0),
                dir_fd=parent_fd,
            )
            stage_metadata = os.fstat(stage_fd)
            if (
                not stat.S_ISDIR(stage_metadata.st_mode)
                or stage_metadata.st_uid != os.geteuid()
            ):
                raise EvidenceError("created staging identity is invalid")
            if stat.S_IMODE(stage_metadata.st_mode) != 0o700:
                os.fchmod(stage_fd, 0o700)
                stage_metadata = os.fstat(stage_fd)
            stage_identity = _posix_identity(stage_metadata)
            if (
                _named_posix_identity(parent_fd, stage_name)
                != stage_identity
            ):
                raise EvidenceError("staging name does not match held stage")
            _require_posix_rename_authority(
                parent_fd,
                expected_stage_uid=stage_metadata.st_uid,
            )
            bundle = cls(
                final_path,
                stage_path,
                parent_fd,
                stage_fd,
                parent_identity,
                stage_identity,
                validated_metadata,
                parent_chain,
            )
            bundle_owns_descriptors = True
            bundle._write_owned(
                "metadata.json",
                metadata_bytes,
            )
            return bundle
        except BaseException:
            if bundle_owns_descriptors:
                raise
            if stage_fd is not None:
                try:
                    held = _posix_identity(os.fstat(stage_fd))
                    source = _named_posix_identity(parent_fd, stage_name)
                    final = _named_posix_identity(
                        parent_fd,
                        final_path.name,
                    )
                    if (
                        stage_created
                        and stage_identity is not None
                        and held == stage_identity
                        and source == stage_identity
                        and final is None
                    ):
                        os.close(stage_fd)
                        stage_fd = None
                        os.rmdir(stage_name, dir_fd=parent_fd)
                except OSError:
                    pass
            if stage_fd is not None:
                try:
                    os.close(stage_fd)
                except OSError:
                    pass
            try:
                os.close(parent_fd)
            except OSError:
                pass
            if parent_chain is not None:
                parent_chain.close()
            raise

    def _ensure_active(self) -> None:
        if not self._active:
            raise RuntimeError("evidence bundle writer is no longer active")

    def _write_owned(self, name: str, data: bytes) -> None:
        self._ensure_active()
        if name in self._written:
            raise EvidenceError(f"duplicate artifact write: {name}")
        if not isinstance(data, bytes):
            raise EvidenceError("artifact data must be bytes")
        if len(data) > MAX_FILE_BYTES:
            raise EvidenceError(
                f"{name} exceeds {MAX_FILE_BYTES} bytes"
            )
        try:
            descriptor = os.open(
                name,
                os.O_WRONLY
                | os.O_CREAT
                | os.O_EXCL
                | _require_no_follow()
                | getattr(os, "O_CLOEXEC", 0),
                0o600,
                dir_fd=self._stage_fd,
            )
            with _managed_fd(descriptor):
                metadata = os.fstat(descriptor)
                if (
                    not stat.S_ISREG(metadata.st_mode)
                    or metadata.st_nlink != 1
                ):
                    raise EvidenceError(
                        f"{name} did not resolve to one regular file"
                    )
                os.fchmod(descriptor, 0o600)
                _write_all(descriptor, data)
                os.fsync(descriptor)
            self._written.add(name)
        except BaseException:
            self._abort()
            raise

    def write_bytes(self, name: str, data: bytes) -> None:
        """Write one caller-owned artifact exactly once."""

        self._ensure_active()
        try:
            validated_name = _validate_entry_name(
                name,
                caller_owned=True,
            )
            self._write_owned(validated_name, data)
        except BaseException:
            if self._active:
                self._abort()
            raise

    def _validate_before_finalize(self) -> None:
        expected = _ALL_ARTIFACTS - {MANIFEST_NAME}
        if self._written != expected:
            raise EvidenceError(
                "bundle artifacts incomplete: "
                f"missing={sorted(expected - self._written)} "
                f"unknown={sorted(self._written - expected)}"
            )
        raw = _read_regular_file(self._stage_fd, "raw.csv")
        summary = _read_regular_file(self._stage_fd, "summary.csv")
        verdict_bytes = _read_regular_file(
            self._stage_fd,
            "verdict.json",
        )
        report = _read_regular_file(self._stage_fd, REPORT_NAME)
        _validate_csv(raw, where="raw.csv")
        _validate_csv(summary, where="summary.csv")
        verdict = _strict_json_object(
            verdict_bytes,
            where="verdict.json",
        )
        _validate_verdict(verdict)
        _validate_report(report)
        observed_metadata = _strict_json_object(
            _read_regular_file(self._stage_fd, "metadata.json"),
            where="metadata.json",
        )
        _validate_metadata(observed_metadata)
        names = os.listdir(self._stage_fd)
        if set(names) != expected or len(names) != len(expected):
            raise EvidenceError("staging directory has unknown entries")

    def _build_manifest(self) -> bytes:
        records: list[bytes] = []
        for name in _MANIFEST_MEMBERS:
            data = _read_regular_file(self._stage_fd, name)
            digest = hashlib.sha256(data).hexdigest()
            records.append(f"{digest}  {name}\n".encode("ascii"))
        return b"".join(records)

    def _publication_state(self) -> str:
        try:
            held_metadata = os.fstat(self._stage_fd)
            if (
                not stat.S_ISDIR(held_metadata.st_mode)
                or _posix_identity(held_metadata) != self._stage_identity
                or _posix_identity(os.fstat(self._parent_fd))
                != self._parent_identity
            ):
                return _AMBIGUOUS
            _require_posix_rename_authority(
                self._parent_fd,
                expected_stage_uid=held_metadata.st_uid,
            )
            path_parent_fd = _open_directory_nofollow(
                self._final_path.parent
            )
            try:
                path_parent_matches = (
                    _posix_identity(os.fstat(path_parent_fd))
                    == self._parent_identity
                )
            except BaseException:
                try:
                    os.close(path_parent_fd)
                except OSError:
                    pass
                raise
            else:
                try:
                    os.close(path_parent_fd)
                except OSError:
                    return _AMBIGUOUS
            if not path_parent_matches:
                return _AMBIGUOUS
            source = _named_posix_identity(
                self._parent_fd,
                self._stage_path.name,
            )
            final = _named_posix_identity(
                self._parent_fd,
                self._final_path.name,
            )
            self._parent_chain.revalidate()
        except (OSError, EvidenceError):
            return _AMBIGUOUS
        if (
            source == self._stage_identity
            and final != self._stage_identity
        ):
            return _NOT_PUBLISHED
        if (
            final == self._stage_identity
            and source != self._stage_identity
        ):
            return _PUBLISHED
        return _AMBIGUOUS

    def _close_without_cleanup(self) -> BaseException | None:
        close_error: BaseException | None = None
        for attribute in ("_stage_fd", "_parent_fd"):
            descriptor = getattr(self, attribute)
            if descriptor >= 0:
                try:
                    os.close(descriptor)
                except BaseException as exc:
                    if close_error is None:
                        close_error = exc
                finally:
                    setattr(self, attribute, -1)
        chain_close_error = self._parent_chain.close()
        if close_error is None:
            close_error = chain_close_error
        self._active = False
        return close_error

    def finalize(self) -> Path:
        """Seal and atomically publish this bundle exactly once."""

        self._ensure_active()
        if self._finalize_called:
            raise RuntimeError("finalize is single-use")
        self._finalize_called = True
        try:
            self._validate_before_finalize()
            _fsync_directory(self._stage_fd)
            self._write_owned(MANIFEST_NAME, self._build_manifest())
            _fsync_directory(self._stage_fd)
            if self._publication_state() != _NOT_PUBLISHED:
                raise PublicationUncertainError(
                    self._final_path,
                    EvidenceError(
                        "staging identity is ambiguous before publication"
                    ),
                )
        except PublicationUncertainError:
            self._close_without_cleanup()
            raise
        except BaseException:
            self._abort()
            raise

        rename_error: BaseException | None = None
        try:
            _atomic_rename_noreplace(
                self._stage_path,
                self._final_path,
                self._parent_fd,
                self._parent_fd,
            )
        except BaseException as exc:
            rename_error = exc
        try:
            publication_state = self._publication_state()
        except BaseException as exc:
            self._close_without_cleanup()
            cause = rename_error or exc
            raise PublicationUncertainError(
                self._final_path,
                cause,
            ) from cause
        if publication_state == _NOT_PUBLISHED:
            self._abort()
            if rename_error is not None:
                raise rename_error
            raise EvidenceError("atomic rename returned without publishing")
        if publication_state != _PUBLISHED:
            self._close_without_cleanup()
            cause = rename_error or EvidenceError(
                "publication identity is ambiguous after rename"
            )
            raise PublicationUncertainError(
                self._final_path,
                cause,
            ) from cause
        publication_error: BaseException | None = rename_error
        try:
            _fsync_directory(self._parent_fd)
        except BaseException as exc:
            if publication_error is None:
                publication_error = exc
        try:
            if self._publication_state() != _PUBLISHED:
                raise EvidenceError(
                    "published final identity changed before success"
                )
        except BaseException as exc:
            if publication_error is None:
                publication_error = exc
        close_error = self._close_without_cleanup()
        if publication_error is None:
            publication_error = close_error
        if publication_error is not None:
            raise PublicationUncertainError(
                self._final_path,
                publication_error,
            ) from publication_error
        return self._final_path

    def _abort(self) -> None:
        if not self._active:
            return
        try:
            try:
                publication_state = self._publication_state()
            except BaseException:
                return
            if publication_state != _NOT_PUBLISHED:
                return
            try:
                names = os.listdir(self._stage_fd)
            except BaseException:
                names = []
            for name in names:
                try:
                    metadata = os.stat(
                        name,
                        dir_fd=self._stage_fd,
                        follow_symlinks=False,
                    )
                    if stat.S_ISDIR(metadata.st_mode):
                        os.rmdir(name, dir_fd=self._stage_fd)
                    else:
                        os.unlink(name, dir_fd=self._stage_fd)
                except BaseException:
                    pass
            try:
                still_not_published = (
                    self._publication_state() == _NOT_PUBLISHED
                )
            except BaseException:
                still_not_published = False
            if still_not_published:
                os.rmdir(
                    self._stage_path.name,
                    dir_fd=self._parent_fd,
                )
        except BaseException:
            pass
        finally:
            self._close_without_cleanup()


class _WindowsEvidenceBundle:
    """Win32 writer using held non-reparse ancestor handles."""

    def __init__(
        self,
        final_path: Path,
        stage_path: Path,
        api: _WindowsAPI,
        parent_handles: list[object],
        stage_handle: object,
        parent_identity: tuple[int, bytes],
        stage_identity: tuple[int, bytes],
        metadata: dict[str, object],
    ) -> None:
        self._final_path = final_path
        self._stage_path = stage_path
        self._api = api
        self._parent_handles = parent_handles
        self._stage_handle: object | None = stage_handle
        self._parent_identity = parent_identity
        self._stage_identity = stage_identity
        self._metadata = metadata
        self._written: set[str] = set()
        self._active = True
        self._finalize_called = False

    @classmethod
    def create(
        cls,
        final_dir: Path | str,
        metadata: dict[str, object],
    ) -> _WindowsEvidenceBundle:
        validated_metadata = _validate_metadata(metadata)
        metadata_bytes = canonical_json_bytes(validated_metadata)
        final_path = _safe_absolute(final_dir)
        if final_path.parent == final_path:
            raise EvidenceError("filesystem root cannot be a bundle")
        _validate_bundle_leaf(final_path.name)
        api = _WindowsAPI()
        _, parent_handles = _win_open_directory_chain(
            final_path.parent,
            api=api,
            writable_leaf=True,
            deny_delete=True,
        )
        stage_handle: object | None = None
        writer_owns_resources = False
        try:
            parent_identity = api.identity(parent_handles[-1])
            if (
                api.directory_identity(final_path.parent)
                != parent_identity
            ):
                raise EvidenceError(
                    "Windows parent path does not match held parent"
                )
            stage_path = final_path.parent / (
                f".{final_path.name}.staging-{os.getpid()}-"
                f"{secrets.token_hex(16)}"
            )
            try:
                existing = api.create_file(
                    final_path,
                    creation_disposition=_WIN_OPEN_EXISTING,
                    flags=(
                        _WIN_FILE_FLAG_BACKUP_SEMANTICS
                        | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
                    ),
                    access=_WIN_FILE_READ_ATTRIBUTES,
                    share=(
                        _WIN_FILE_SHARE_READ
                        | _WIN_FILE_SHARE_WRITE
                        | _WIN_FILE_SHARE_DELETE
                    ),
                )
            except FileNotFoundError:
                pass
            else:
                try:
                    api.close(existing)
                except OSError:
                    pass
                raise FileExistsError(
                    _WIN_ERROR_ALREADY_EXISTS,
                    "destination already exists",
                    final_path,
                )
            api.create_directory(stage_path)
            stage_handle = _win_open_directory(
                stage_path,
                api=api,
                require_private=True,
                writable=True,
                rename_source=False,
            )
            stage_identity = api.identity(stage_handle)
            if api.directory_identity(stage_path) != stage_identity:
                raise EvidenceError(
                    "Windows staging name does not match held stage"
                )
            writer = cls(
                final_path,
                stage_path,
                api,
                parent_handles,
                stage_handle,
                parent_identity,
                stage_identity,
                validated_metadata,
            )
            writer_owns_resources = True
            writer._write_owned("metadata.json", metadata_bytes)
            return writer
        except BaseException:
            if writer_owns_resources:
                raise
            if stage_handle is not None:
                try:
                    api.close(stage_handle)
                except OSError:
                    pass
            _win_close_handles(api, parent_handles)
            raise

    def _ensure_active(self) -> None:
        if not self._active:
            raise RuntimeError("evidence bundle writer is no longer active")

    def _write_owned(self, name: str, data: bytes) -> None:
        self._ensure_active()
        if name in self._written:
            raise EvidenceError(f"duplicate artifact write: {name}")
        if not isinstance(data, bytes):
            raise EvidenceError("artifact data must be bytes")
        if len(data) > MAX_FILE_BYTES:
            raise EvidenceError(
                f"{name} exceeds {MAX_FILE_BYTES} bytes"
            )
        try:
            _win_create_and_write_file(
                self._stage_path / name,
                data,
                api=self._api,
            )
            self._written.add(name)
        except BaseException:
            self._abort()
            raise

    def write_bytes(self, name: str, data: bytes) -> None:
        self._ensure_active()
        try:
            validated_name = _validate_entry_name(
                name,
                caller_owned=True,
            )
            self._write_owned(validated_name, data)
        except BaseException:
            if self._active:
                self._abort()
            raise

    def _validate_before_finalize(self) -> None:
        expected = _ALL_ARTIFACTS - {MANIFEST_NAME}
        if self._written != expected:
            raise EvidenceError(
                "bundle artifacts incomplete: "
                f"missing={sorted(expected - self._written)} "
                f"unknown={sorted(self._written - expected)}"
            )
        names = os.listdir(self._stage_path)
        if set(names) != expected or len(names) != len(expected):
            raise EvidenceError("staging directory has unknown entries")
        raw = _win_read_regular_file(
            self._stage_path / "raw.csv",
            api=self._api,
        )
        summary = _win_read_regular_file(
            self._stage_path / "summary.csv",
            api=self._api,
        )
        verdict_bytes = _win_read_regular_file(
            self._stage_path / "verdict.json",
            api=self._api,
        )
        report = _win_read_regular_file(
            self._stage_path / REPORT_NAME,
            api=self._api,
        )
        _validate_csv(raw, where="raw.csv")
        _validate_csv(summary, where="summary.csv")
        _validate_verdict(
            _strict_json_object(
                verdict_bytes,
                where="verdict.json",
            )
        )
        _validate_report(report)
        _validate_metadata(
            _strict_json_object(
                _win_read_regular_file(
                    self._stage_path / "metadata.json",
                    api=self._api,
                ),
                where="metadata.json",
            )
        )

    def _build_manifest(self) -> bytes:
        records: list[bytes] = []
        for name in _MANIFEST_MEMBERS:
            data = _win_read_regular_file(
                self._stage_path / name,
                api=self._api,
            )
            digest = hashlib.sha256(data).hexdigest()
            records.append(f"{digest}  {name}\n".encode("ascii"))
        return b"".join(records)

    def _publication_state(self) -> str:
        if self._stage_handle is None or not self._parent_handles:
            return _AMBIGUOUS
        try:
            if (
                self._api.identity(self._stage_handle)
                != self._stage_identity
                or self._api.identity(self._parent_handles[-1])
                != self._parent_identity
                or self._api.directory_identity(
                    self._final_path.parent
                )
                != self._parent_identity
            ):
                return _AMBIGUOUS
            source_identity = self._api.directory_identity(
                self._stage_path
            )
            final_identity = self._api.directory_identity(
                self._final_path
            )
        except (OSError, EvidenceError):
            return _AMBIGUOUS
        if (
            source_identity == self._stage_identity
            and final_identity != self._stage_identity
        ):
            return _NOT_PUBLISHED
        if (
            final_identity == self._stage_identity
            and source_identity != self._stage_identity
        ):
            return _PUBLISHED
        return _AMBIGUOUS

    def _close_without_cleanup(self) -> BaseException | None:
        close_error: BaseException | None = None
        if self._stage_handle is not None:
            try:
                self._api.close(self._stage_handle)
            except BaseException as exc:
                close_error = exc
            finally:
                self._stage_handle = None
        while self._parent_handles:
            handle = self._parent_handles.pop()
            try:
                self._api.close(handle)
            except BaseException as exc:
                if close_error is None:
                    close_error = exc
        self._active = False
        return close_error

    def finalize(self) -> Path:
        self._ensure_active()
        if self._finalize_called:
            raise RuntimeError("finalize is single-use")
        self._finalize_called = True
        publication_handle: object | None = None
        try:
            self._validate_before_finalize()
            if self._stage_handle is None:
                raise EvidenceError("Windows staging handle is unavailable")
            if self._publication_state() != _NOT_PUBLISHED:
                raise EvidenceError(
                    "Windows staging publication identity is ambiguous"
                )
            self._api.flush_directory(self._stage_handle)
            self._write_owned(MANIFEST_NAME, self._build_manifest())
            self._api.flush_directory(self._stage_handle)
            if self._publication_state() != _NOT_PUBLISHED:
                raise EvidenceError(
                    "Windows staging publication identity changed"
                )
            publication_handle = _win_open_directory(
                self._stage_path,
                api=self._api,
                require_private=True,
                rename_source=True,
            )
            if (
                self._api.identity(publication_handle)
                != self._stage_identity
            ):
                raise EvidenceError(
                    "Windows publication handle identity changed"
                )
        except BaseException:
            if publication_handle is not None:
                try:
                    self._api.close(publication_handle)
                except BaseException:
                    pass
            self._abort()
            raise

        rename_error: BaseException | None = None
        try:
            self._api.rename_handle_noreplace(
                publication_handle,
                self._parent_handles[-1],
                self._final_path.name,
            )
        except BaseException as exc:
            rename_error = exc
        finally:
            if publication_handle is not None:
                try:
                    self._api.close(publication_handle)
                except BaseException as exc:
                    if rename_error is None:
                        rename_error = exc

        try:
            state = self._publication_state()
        except BaseException as exc:
            self._close_without_cleanup()
            cause = rename_error or exc
            raise PublicationUncertainError(
                self._final_path,
                cause,
            ) from cause
        if state == _NOT_PUBLISHED:
            self._abort()
            if rename_error is not None:
                raise rename_error
            raise EvidenceError("Windows publication did not occur")
        if state == _AMBIGUOUS:
            self._close_without_cleanup()
            cause = rename_error or EvidenceError(
                "Windows publication identity is ambiguous"
            )
            raise PublicationUncertainError(
                self._final_path,
                cause,
            ) from cause

        publication_error: BaseException | None = rename_error
        try:
            self._api.flush_directory(self._parent_handles[-1])
            if self._publication_state() != _PUBLISHED:
                raise EvidenceError(
                    "published Windows bundle identity changed"
                )
        except BaseException as exc:
            if publication_error is None:
                publication_error = exc
        close_error = self._close_without_cleanup()
        if publication_error is None:
            publication_error = close_error
        if publication_error is not None:
            raise PublicationUncertainError(
                self._final_path,
                publication_error,
            ) from publication_error
        return self._final_path

    def _abort(self) -> None:
        if not self._active:
            return
        # Win32 has no portable handle-relative tree deletion primitive.
        # After an error the staging pathname is mutable, so inspecting or
        # deleting through that pathname could target an attacker-controlled
        # replacement.  Close our handles and intentionally leak the private,
        # high-entropy staging directory instead.
        self._close_without_cleanup()


def _win_audit_info_signature(
    information: _WinByHandleFileInformation,
    identity: tuple[int, bytes],
) -> tuple[
    tuple[int, bytes],
    int,
    tuple[int, int],
    int,
    int,
]:
    return (
        identity,
        information.size,
        information.write_time,
        information.number_of_links,
        information.file_attributes,
    )


@dataclass
class _WindowsAuditFile:
    handle: object
    information: _WinByHandleFileInformation
    identity: tuple[int, bytes]
    data: bytes


def _win_open_audit_file(
    path: Path,
    *,
    api: _WindowsAPI,
) -> _WindowsAuditFile:
    handle = api.create_file(
        path,
        creation_disposition=_WIN_OPEN_EXISTING,
        flags=(
            _WIN_FILE_ATTRIBUTE_NORMAL
            | _WIN_FILE_FLAG_OPEN_REPARSE_POINT
        ),
        access=(
            _WIN_GENERIC_READ
            | _WIN_FILE_READ_ATTRIBUTES
            | _WIN_READ_CONTROL
        ),
        # Permit readers only.  This denies write and delete sharing for
        # the complete lifetime of an audit snapshot.
        share=_WIN_FILE_SHARE_READ,
    )
    try:
        api.require_regular_single_link(handle)
        api.require_private_acl(handle)
        before_identity = api.identity(handle)
        before = api.info(handle)
        data = api.read_all(handle, before.size)
        after = api.info(handle)
        after_identity = api.identity(handle)
        if (
            _win_audit_info_signature(before, before_identity)
            != _win_audit_info_signature(after, after_identity)
            or len(data) != before.size
        ):
            raise EvidenceError(
                f"{path.name} changed while being read"
            )
        return _WindowsAuditFile(
            handle,
            before,
            before_identity,
            data,
        )
    except BaseException:
        try:
            api.close(handle)
        except OSError:
            pass
        raise


class _WindowsAuditSnapshot:
    def __init__(
        self,
        path: Path,
        api: _WindowsAPI,
        directory_handles: list[object],
        directory_information: _WinByHandleFileInformation,
        directory_identity: tuple[int, bytes],
        files: dict[str, _WindowsAuditFile],
    ) -> None:
        self.path = path
        self.api = api
        self.directory_handles = directory_handles
        self.directory_information = directory_information
        self.directory_identity = directory_identity
        self.files = files
        self.payloads = {
            name: record.data for name, record in files.items()
        }
        self._closed = False

    def revalidate(self) -> None:
        directory_handle = self.directory_handles[-1]
        self.api.require_directory_no_reparse(directory_handle)
        self.api.require_private_acl(directory_handle)
        if (
            _win_audit_info_signature(
                self.api.info(directory_handle),
                self.api.identity(directory_handle),
            )
            != _win_audit_info_signature(
                self.directory_information,
                self.directory_identity,
            )
            or self.api.directory_identity(self.path)
            != self.directory_identity
        ):
            raise EvidenceError(
                "bundle path identity changed during Windows audit"
            )
        names = os.listdir(self.path)
        if (
            set(names) != _ALL_ARTIFACTS
            or len(names) != len(_ALL_ARTIFACTS)
        ):
            raise EvidenceError(
                "bundle membership changed during Windows audit"
            )
        for name, record in self.files.items():
            self.api.require_regular_single_link(record.handle)
            self.api.require_private_acl(record.handle)
            if (
                _win_audit_info_signature(
                    self.api.info(record.handle),
                    self.api.identity(record.handle),
                )
                != _win_audit_info_signature(
                    record.information,
                    record.identity,
                )
            ):
                raise EvidenceError(
                    f"{name} changed during Windows audit"
                )
            reopened = _win_open_audit_file(
                self.path / name,
                api=self.api,
            )
            try:
                if (
                    _win_audit_info_signature(
                        reopened.information,
                        reopened.identity,
                    )
                    != _win_audit_info_signature(
                        record.information,
                        record.identity,
                    )
                    or reopened.data != record.data
                ):
                    raise EvidenceError(
                        f"{name} identity or bytes changed during "
                        "Windows audit"
                    )
            except BaseException:
                try:
                    self.api.close(reopened.handle)
                except OSError:
                    pass
                raise
            else:
                self.api.close(reopened.handle)

    def close(self, *, suppress: bool) -> None:
        if self._closed:
            return
        self._closed = True
        close_error: BaseException | None = None
        for record in reversed(tuple(self.files.values())):
            try:
                self.api.close(record.handle)
            except OSError as exc:
                if close_error is None:
                    close_error = exc
        while self.directory_handles:
            handle = self.directory_handles.pop()
            try:
                self.api.close(handle)
            except OSError as exc:
                if close_error is None:
                    close_error = exc
        if close_error is not None and not suppress:
            raise close_error


def _open_windows_audit_snapshot(
    path: Path | str,
) -> _WindowsAuditSnapshot:
    api = _WindowsAPI()
    bundle_path, handles = _win_open_directory_chain(
        _safe_absolute(path),
        api=api,
        require_private_leaf=True,
        deny_delete=True,
    )
    files: dict[str, _WindowsAuditFile] = {}
    try:
        directory_information = api.info(handles[-1])
        directory_identity = api.identity(handles[-1])
        if (
            api.directory_identity(bundle_path)
            != directory_identity
        ):
            raise EvidenceError(
                "Windows bundle path does not match held directory"
            )
        names = os.listdir(bundle_path)
        if (
            set(names) != _ALL_ARTIFACTS
            or len(names) != len(_ALL_ARTIFACTS)
        ):
            raise EvidenceError(
                "bundle membership mismatch: "
                f"expected={sorted(_ALL_ARTIFACTS)} "
                f"actual={sorted(names)}"
            )
        for name in sorted(_ALL_ARTIFACTS):
            files[name] = _win_open_audit_file(
                bundle_path / name,
                api=api,
            )
        after = os.listdir(bundle_path)
        if set(after) != set(names) or len(after) != len(names):
            raise EvidenceError(
                "bundle membership changed during Windows audit"
            )
        return _WindowsAuditSnapshot(
            bundle_path,
            api,
            handles,
            directory_information,
            directory_identity,
            files,
        )
    except BaseException:
        for record in reversed(tuple(files.values())):
            try:
                api.close(record.handle)
            except OSError:
                pass
        _win_close_handles(api, handles)
        raise


def _win_read_bundle_payloads(
    path: Path | str,
) -> dict[str, bytes]:
    snapshot = _open_windows_audit_snapshot(path)
    try:
        payloads = dict(snapshot.payloads)
        snapshot.revalidate()
    except BaseException:
        snapshot.close(suppress=True)
        raise
    snapshot.close(suppress=False)
    return payloads


def _read_regular_file(directory_fd: int, name: str) -> bytes:
    _validate_entry_name(name, caller_owned=False)
    try:
        before = os.stat(
            name,
            dir_fd=directory_fd,
            follow_symlinks=False,
        )
    except OSError as exc:
        raise EvidenceError(f"cannot inspect {name}: {exc}") from exc
    if (
        not stat.S_ISREG(before.st_mode)
        or before.st_nlink != 1
        or before.st_size > MAX_FILE_BYTES
        or stat.S_IMODE(before.st_mode) != 0o600
    ):
        raise EvidenceError(
            f"{name} must be one bounded, non-hardlinked regular file"
        )
    try:
        descriptor = os.open(
            name,
            os.O_RDONLY
            | _require_no_follow()
            | getattr(os, "O_CLOEXEC", 0),
            dir_fd=directory_fd,
        )
    except OSError as exc:
        raise EvidenceError(f"cannot safely open {name}: {exc}") from exc
    with _managed_fd(descriptor):
        opened = os.fstat(descriptor)
        if (
            not stat.S_ISREG(opened.st_mode)
            or opened.st_nlink != 1
            or stat.S_IMODE(opened.st_mode) != 0o600
            or opened.st_dev != before.st_dev
            or opened.st_ino != before.st_ino
            or opened.st_size != before.st_size
            or opened.st_size > MAX_FILE_BYTES
        ):
            raise EvidenceError(f"{name} changed during safe open")
        chunks: list[bytes] = []
        remaining = MAX_FILE_BYTES + 1
        while remaining:
            chunk = os.read(descriptor, min(64 * 1024, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        data = b"".join(chunks)
        if len(data) > MAX_FILE_BYTES:
            raise EvidenceError(f"{name} exceeds {MAX_FILE_BYTES} bytes")
        after = os.fstat(descriptor)
        if (
            after.st_size != opened.st_size
            or after.st_mtime_ns != opened.st_mtime_ns
            or after.st_ctime_ns != opened.st_ctime_ns
            or len(data) != opened.st_size
        ):
            raise EvidenceError(f"{name} changed while being read")
        return data


def _posix_audit_stat_signature(
    metadata: os.stat_result,
) -> tuple[int, int, int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        metadata.st_mode,
        metadata.st_nlink,
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )


def _read_posix_audit_fd(
    descriptor: int,
    *,
    expected_size: int,
    name: str,
) -> bytes:
    if expected_size > MAX_FILE_BYTES:
        raise EvidenceError(f"{name} exceeds {MAX_FILE_BYTES} bytes")
    chunks: list[bytes] = []
    remaining = expected_size
    while remaining:
        chunk = os.read(descriptor, min(64 * 1024, remaining))
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    data = b"".join(chunks)
    if len(data) != expected_size:
        raise EvidenceError(f"{name} changed while being read")
    return data


@dataclass
class _PosixAuditFile:
    descriptor: int
    metadata: os.stat_result
    data: bytes


class _PosixAuditSnapshot:
    def __init__(
        self,
        path: Path,
        parent_fd: int,
        parent_identity: tuple[int, int],
        directory_fd: int,
        directory_metadata: os.stat_result,
        files: dict[str, _PosixAuditFile],
        parent_chain: _PosixDirectoryChain,
    ) -> None:
        self.path = path
        self.parent_fd = parent_fd
        self.parent_identity = parent_identity
        self.directory_fd = directory_fd
        self.directory_metadata = directory_metadata
        self.files = files
        self.parent_chain = parent_chain
        self.payloads = {
            name: record.data for name, record in files.items()
        }
        self._closed = False

    def revalidate(self) -> None:
        current_directory = os.fstat(self.directory_fd)
        if (
            _posix_audit_stat_signature(current_directory)
            != _posix_audit_stat_signature(
                self.directory_metadata
            )
        ):
            raise EvidenceError(
                "bundle directory changed during POSIX audit"
            )
        names = os.listdir(self.directory_fd)
        if (
            set(names) != _ALL_ARTIFACTS
            or len(names) != len(_ALL_ARTIFACTS)
        ):
            raise EvidenceError(
                "bundle membership changed during POSIX audit"
            )
        for name, record in self.files.items():
            opened = os.fstat(record.descriptor)
            if (
                _posix_audit_stat_signature(opened)
                != _posix_audit_stat_signature(record.metadata)
            ):
                raise EvidenceError(
                    f"{name} changed during POSIX audit"
                )
            named = os.stat(
                name,
                dir_fd=self.directory_fd,
                follow_symlinks=False,
            )
            if (
                _posix_audit_stat_signature(named)
                != _posix_audit_stat_signature(record.metadata)
            ):
                raise EvidenceError(
                    f"{name} identity changed during POSIX audit"
                )
            os.lseek(record.descriptor, 0, os.SEEK_SET)
            reread = _read_posix_audit_fd(
                record.descriptor,
                expected_size=record.metadata.st_size,
                name=name,
            )
            after = os.fstat(record.descriptor)
            if (
                reread != record.data
                or _posix_audit_stat_signature(after)
                != _posix_audit_stat_signature(record.metadata)
            ):
                raise EvidenceError(
                    f"{name} bytes changed during POSIX audit"
                )

        parent_metadata = os.fstat(self.parent_fd)
        if (
            _posix_identity(parent_metadata)
            != self.parent_identity
        ):
            raise EvidenceError(
                "bundle parent identity changed during POSIX audit"
            )
        _require_posix_rename_authority(
            self.parent_fd,
            expected_stage_uid=self.directory_metadata.st_uid,
        )
        reopened_parent = _open_directory_nofollow(self.path.parent)
        try:
            if (
                _posix_identity(os.fstat(reopened_parent))
                != self.parent_identity
            ):
                raise EvidenceError(
                    "bundle parent path identity changed during POSIX "
                    "audit"
                )
        except BaseException:
            try:
                os.close(reopened_parent)
            except OSError:
                pass
            raise
        else:
            os.close(reopened_parent)
        reopened_directory = os.open(
            self.path.name,
            os.O_RDONLY
            | _require_no_follow()
            | getattr(os, "O_DIRECTORY", 0)
            | getattr(os, "O_CLOEXEC", 0),
            dir_fd=self.parent_fd,
        )
        try:
            if (
                _posix_audit_stat_signature(
                    os.fstat(reopened_directory)
                )
                != _posix_audit_stat_signature(
                    self.directory_metadata
                )
            ):
                raise EvidenceError(
                    "bundle leaf identity changed during POSIX audit"
                )
        except BaseException:
            try:
                os.close(reopened_directory)
            except OSError:
                pass
            raise
        else:
            os.close(reopened_directory)
        self.parent_chain.revalidate()

    def close(self, *, suppress: bool) -> None:
        if self._closed:
            return
        self._closed = True
        close_error: OSError | None = None
        for record in reversed(tuple(self.files.values())):
            try:
                os.close(record.descriptor)
            except OSError as exc:
                if close_error is None:
                    close_error = exc
        try:
            os.close(self.directory_fd)
        except OSError as exc:
            if close_error is None:
                close_error = exc
        try:
            os.close(self.parent_fd)
        except OSError as exc:
            if close_error is None:
                close_error = exc
        chain_close_error = self.parent_chain.close()
        if close_error is None:
            close_error = chain_close_error
        if close_error is not None and not suppress:
            raise close_error


def _open_posix_audit_snapshot(
    path: Path | str,
) -> _PosixAuditSnapshot:
    bundle_path = _safe_absolute(path)
    if bundle_path.parent == bundle_path:
        raise EvidenceError("filesystem root cannot be a bundle")
    _validate_bundle_leaf(bundle_path.name)
    parent_fd = _open_directory_nofollow(bundle_path.parent)
    parent_chain: _PosixDirectoryChain | None = None
    directory_fd = -1
    files: dict[str, _PosixAuditFile] = {}
    try:
        parent_identity = _require_posix_rename_authority(parent_fd)
        parent_chain = _open_posix_directory_chain(
            bundle_path.parent
        )
        if parent_chain.leaf_identity != parent_identity:
            raise EvidenceError(
                "POSIX audit parent handles identify different "
                "directories"
            )
        try:
            directory_fd = os.open(
                bundle_path.name,
                os.O_RDONLY
                | _require_no_follow()
                | getattr(os, "O_DIRECTORY", 0)
                | getattr(os, "O_CLOEXEC", 0),
                dir_fd=parent_fd,
            )
        except OSError as exc:
            raise EvidenceError(
                f"unsafe or unavailable bundle directory "
                f"{bundle_path}: {exc}"
            ) from exc
        directory_metadata = os.fstat(directory_fd)
        _require_posix_rename_authority(
            parent_fd,
            expected_stage_uid=directory_metadata.st_uid,
        )
        if (
            _named_posix_identity(parent_fd, bundle_path.name)
            != _posix_identity(directory_metadata)
        ):
            raise EvidenceError(
                "bundle leaf does not match held directory"
            )
        if stat.S_IMODE(directory_metadata.st_mode) != 0o700:
            raise EvidenceError(
                "bundle directory mode must be exactly 0700"
            )
        names = os.listdir(directory_fd)
        if (
            set(names) != _ALL_ARTIFACTS
            or len(names) != len(_ALL_ARTIFACTS)
        ):
            raise EvidenceError(
                "bundle membership mismatch: "
                f"expected={sorted(_ALL_ARTIFACTS)} "
                f"actual={sorted(names)}"
            )
        for name in sorted(_ALL_ARTIFACTS):
            before = os.stat(
                name,
                dir_fd=directory_fd,
                follow_symlinks=False,
            )
            if (
                not stat.S_ISREG(before.st_mode)
                or before.st_nlink != 1
                or before.st_size > MAX_FILE_BYTES
                or stat.S_IMODE(before.st_mode) != 0o600
            ):
                raise EvidenceError(
                    f"{name} must be one bounded, non-hardlinked "
                    "regular file"
                )
            descriptor = os.open(
                name,
                os.O_RDONLY
                | _require_no_follow()
                | getattr(os, "O_CLOEXEC", 0),
                dir_fd=directory_fd,
            )
            try:
                opened = os.fstat(descriptor)
                if (
                    _posix_audit_stat_signature(opened)
                    != _posix_audit_stat_signature(before)
                ):
                    raise EvidenceError(
                        f"{name} changed during safe open"
                    )
                data = _read_posix_audit_fd(
                    descriptor,
                    expected_size=opened.st_size,
                    name=name,
                )
                after = os.fstat(descriptor)
                if (
                    _posix_audit_stat_signature(after)
                    != _posix_audit_stat_signature(opened)
                ):
                    raise EvidenceError(
                        f"{name} changed while being read"
                    )
            except BaseException:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
                raise
            files[name] = _PosixAuditFile(
                descriptor,
                opened,
                data,
            )
        after_names = os.listdir(directory_fd)
        if (
            set(after_names) != set(names)
            or len(after_names) != len(names)
        ):
            raise EvidenceError(
                "bundle membership changed during POSIX audit"
            )
        return _PosixAuditSnapshot(
            bundle_path,
            parent_fd,
            parent_identity,
            directory_fd,
            directory_metadata,
            files,
            parent_chain,
        )
    except BaseException:
        for record in reversed(tuple(files.values())):
            try:
                os.close(record.descriptor)
            except OSError:
                pass
        if directory_fd >= 0:
            try:
                os.close(directory_fd)
            except OSError:
                pass
        try:
            os.close(parent_fd)
        except OSError:
            pass
        if parent_chain is not None:
            parent_chain.close()
        raise


def _parse_manifest(data: bytes) -> dict[str, str]:
    if not data or len(data) > MAX_FILE_BYTES:
        raise EvidenceError("manifest is empty or oversized")
    lines = data.splitlines(keepends=True)
    records: list[tuple[str, str]] = []
    for line in lines:
        matched = _MANIFEST_LINE_RE.fullmatch(line)
        if matched is None:
            raise EvidenceError("manifest has invalid grammar")
        digest = matched.group(1).decode("ascii")
        name = matched.group(2).decode("ascii")
        _validate_entry_name(name, caller_owned=False)
        if name == MANIFEST_NAME:
            raise EvidenceError("manifest must not hash itself")
        records.append((name, digest))
    names = [name for name, _ in records]
    if names != sorted(names):
        raise EvidenceError("manifest records are not sorted")
    if len(set(names)) != len(names):
        raise EvidenceError("manifest has duplicate records")
    if tuple(names) != _MANIFEST_MEMBERS:
        raise EvidenceError(
            "manifest membership mismatch: "
            f"expected={list(_MANIFEST_MEMBERS)} actual={names}"
        )
    return dict(records)


def _audit_payloads(
    payloads: Mapping[str, bytes],
    recompute: RecomputeCallback,
    *,
    required_source_commit: str | None = None,
    required_source_dirty_digest: str | None = None,
) -> AuditResult:
    manifest = _parse_manifest(payloads[MANIFEST_NAME])
    for name in _MANIFEST_MEMBERS:
        actual = hashlib.sha256(payloads[name]).hexdigest()
        if manifest[name] != actual:
            raise EvidenceError(f"manifest hash mismatch for {name}")

    metadata = _strict_json_object(
        payloads["metadata.json"],
        where="metadata.json",
    )
    _validate_metadata(metadata)
    if (
        required_source_commit is not None
        and metadata["source_commit"] != required_source_commit
    ):
        raise EvidenceError("required source commit does not match metadata")
    if (
        required_source_dirty_digest is not None
        and metadata["source_dirty_digest"]
        != required_source_dirty_digest
    ):
        raise EvidenceError(
            "required source dirty digest does not match metadata"
        )

    _validate_csv(payloads["raw.csv"], where="raw.csv")
    _validate_csv(payloads["summary.csv"], where="summary.csv")
    stored_verdict = _strict_json_object(
        payloads["verdict.json"],
        where="verdict.json",
    )
    _validate_verdict(stored_verdict)
    _validate_report(payloads[REPORT_NAME])

    try:
        derived = recompute(payloads["raw.csv"], dict(metadata))
    except EvidenceError:
        raise
    except Exception as exc:
        raise EvidenceError(f"evidence recomputation failed: {exc}") from exc
    if not isinstance(derived, RecomputedArtifacts):
        raise EvidenceError(
            "recompute callback must return RecomputedArtifacts"
        )
    for field, value in (
        ("summary_csv", derived.summary_csv),
        ("verdict_json", derived.verdict_json),
        ("report_md", derived.report_md),
    ):
        if not isinstance(value, bytes):
            raise EvidenceError(f"recomputed {field} must be bytes")
    _validate_csv(derived.summary_csv, where="recomputed summary.csv")
    recomputed_verdict = _strict_json_object(
        derived.verdict_json,
        where="recomputed verdict.json",
    )
    _validate_verdict(recomputed_verdict)
    _validate_report(derived.report_md)
    comparisons = (
        ("summary.csv", derived.summary_csv),
        ("verdict.json", derived.verdict_json),
        (REPORT_NAME, derived.report_md),
    )
    for name, expected in comparisons:
        if payloads[name] != expected:
            raise EvidenceError(
                f"{name} does not match recomputed evidence"
            )
    return AuditResult(
        verdict=stored_verdict["verdict"],  # type: ignore[arg-type]
        reasons=tuple(stored_verdict["reasons"]),  # type: ignore[arg-type]
        metadata=metadata,
        portable_verdict=stored_verdict.get(
            "portable_verdict",
            stored_verdict["verdict"],
        ),  # type: ignore[arg-type]
        platform_verdict=stored_verdict.get(
            "platform_verdict",
            stored_verdict["verdict"],
        ),  # type: ignore[arg-type]
    )


def audit_bundle(
    path: Path | str,
    recompute: RecomputeCallback,
    *,
    required_source_commit: str | None = None,
    required_source_dirty_digest: str | None = None,
) -> AuditResult:
    """Audit without writes and return a valid semantic verdict.

    Invalid evidence raises :class:`EvidenceError`; a valid ``REJECT`` bundle
    returns an :class:`AuditResult` whose ``verdict`` is ``"REJECT"``.
    """

    platform_kind = _platform_kind()
    if platform_kind == "windows":
        snapshot = _open_windows_audit_snapshot(path)
    elif platform_kind == "posix":
        snapshot = _open_posix_audit_snapshot(path)
    else:
        raise UnsupportedPlatformError(
            f"secure evidence audit unsupported on {sys.platform}"
        )

    try:
        result = _audit_payloads(
            snapshot.payloads,
            recompute,
            required_source_commit=required_source_commit,
            required_source_dirty_digest=(
                required_source_dirty_digest
            ),
        )
        snapshot.revalidate()
    except BaseException:
        snapshot.close(suppress=True)
        raise
    try:
        snapshot.close(suppress=False)
    except OSError as exc:
        raise EvidenceError(
            f"cannot close {platform_kind} audit snapshot: {exc}"
        ) from exc
    return result
