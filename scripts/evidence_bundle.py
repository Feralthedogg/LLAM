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
CLASSIFIER_SCHEMA = "llam.native-classifier.v1"
REPORT_NAME = "report.md"
MANIFEST_NAME = "MANIFEST.sha256"
MAX_FILE_BYTES = 8 * 1024 * 1024
MAX_SOURCE_STATE_BYTES = 8 * 1024 * 1024

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
_VERDICT_FIELDS = frozenset({"schema", "verdict", "reasons"})
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

    def __init__(self, final_path: Path | str, cause: OSError) -> None:
        self.final_path = Path(final_path)
        self.cause = cause
        super().__init__(
            "evidence publication may have succeeded at "
            f"{self.final_path}; do not retry creation or delete it; "
            f"recover with --audit-existing {self.final_path}: {cause}"
        )


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


def canonical_json_bytes(value: object) -> bytes:
    """Return the only accepted deterministic JSON representation."""

    try:
        text = json.dumps(
            value,
            allow_nan=False,
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
    except (TypeError, ValueError) as exc:
        raise EvidenceError(f"value is not deterministic JSON: {exc}") from exc
    return (text + "\n").encode("utf-8")


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
) -> tuple[bytes, bytes]:
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
    for component in relative.parts[:-1]:
        current = current / component
        metadata = current.lstat()
        if not stat.S_ISDIR(metadata.st_mode):
            raise EvidenceError(
                "untracked source has a non-directory ancestor"
            )
    path = root / relative
    before = path.lstat()
    if stat.S_ISLNK(before.st_mode):
        target = os.fsencode(os.readlink(path))
        after = path.lstat()
        if (
            before.st_dev,
            before.st_ino,
            before.st_mode,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
        ) != (
            after.st_dev,
            after.st_ino,
            after.st_mode,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        ):
            raise EvidenceError("untracked symlink changed while hashing")
        if len(path_bytes) + len(target) > remaining:
            raise EvidenceError("untracked source content is oversized")
        return b"symlink", target
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
    try:
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
        return b"regular", data
    finally:
        os.close(descriptor)


def git_source_dirty_digest(
    run_process: Callable[..., object],
    *,
    cwd: Path | None = None,
) -> str:
    """Hash tracked and untracked Git source state, or fail closed.

    Ignored files are excluded by Git.  Untracked paths and contents use
    explicit length framing so distinct inventories cannot collide through
    concatenation.
    """

    try:
        root_result = run_process(
            ["git", "rev-parse", "--show-toplevel"],
            cwd=cwd,
            timeout=5.0,
            max_output_bytes=4096,
        )
        diff_result = run_process(
            ["git", "diff", "--binary", "--no-ext-diff", "HEAD", "--"],
            cwd=cwd,
            timeout=10.0,
            max_output_bytes=MAX_SOURCE_STATE_BYTES,
        )
        untracked_result = run_process(
            [
                "git",
                "ls-files",
                "--others",
                "--exclude-standard",
                "-z",
            ],
            cwd=cwd,
            timeout=10.0,
            max_output_bytes=MAX_SOURCE_STATE_BYTES,
        )
    except (OSError, RuntimeError):
        return "unavailable"
    results = (root_result, diff_result, untracked_result)
    if any(
        getattr(result, "returncode", 1) != 0
        or getattr(result, "stderr", "")
        or getattr(result, "stdout_truncated", True)
        or getattr(result, "stderr_truncated", True)
        for result in results
    ):
        return "unavailable"
    root_text = getattr(root_result, "stdout", "").rstrip("\n")
    tracked_text = getattr(diff_result, "stdout", "")
    inventory_text = getattr(untracked_result, "stdout", "")
    if (
        not root_text
        or "\n" in root_text
        or "\x00" in root_text
        or "\ufffd" in root_text
        or "\ufffd" in tracked_text
        or "\ufffd" in inventory_text
    ):
        return "unavailable"
    if inventory_text:
        if not inventory_text.endswith("\x00"):
            return "unavailable"
        untracked_names = inventory_text[:-1].split("\x00")
    else:
        untracked_names = []
    encoded_names = [name.encode("utf-8") for name in untracked_names]
    if (
        len(set(untracked_names)) != len(untracked_names)
        or encoded_names != sorted(encoded_names)
    ):
        return "unavailable"
    tracked = tracked_text.encode("utf-8")
    inventory = inventory_text.encode("utf-8")
    consumed = len(tracked) + len(inventory)
    if consumed > MAX_SOURCE_STATE_BYTES:
        return "unavailable"
    if not tracked and not untracked_names:
        return "clean"
    try:
        root = Path(root_text).resolve(strict=True)
        digest = hashlib.sha256()
        digest.update(b"llam-source-dirty-v1\x00")
        _source_hash_frame(digest, b"tracked-diff", tracked)
        for name, path_bytes in zip(
            untracked_names,
            encoded_names,
            strict=True,
        ):
            kind, data = _read_untracked_source(
                root,
                name,
                remaining=MAX_SOURCE_STATE_BYTES - consumed,
            )
            consumed += len(path_bytes) + len(data)
            _source_hash_frame(digest, b"path", path_bytes)
            _source_hash_frame(digest, b"kind", kind)
            _source_hash_frame(digest, b"content", data)
    except (EvidenceError, OSError, UnicodeError, ValueError):
        return "unavailable"
    return digest.hexdigest()


def _require_no_follow() -> int:
    flag = getattr(os, "O_NOFOLLOW", 0)
    if not isinstance(flag, int) or flag == 0:
        raise UnsupportedPlatformError(
            "O_NOFOLLOW is required for evidence bundles"
        )
    return flag


def _check_json_safe(value: object, *, where: str) -> None:
    if value is None or isinstance(value, bool):
        return
    if isinstance(value, int):
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise EvidenceError(f"{where} contains NaN or infinity")
        return
    if isinstance(value, str):
        if "\x00" in value:
            raise EvidenceError(
                f"{where} contains a NUL-bearing string"
            )
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _check_json_safe(item, where=f"{where}[{index}]")
        return
    if isinstance(value, dict):
        for key, item in value.items():
            if (
                not isinstance(key, str)
                or not key
                or "\x00" in key
            ):
                raise EvidenceError(
                    f"{where} contains an invalid object key"
                )
            _check_json_safe(item, where=f"{where}.{key}")
        return
    raise EvidenceError(
        f"{where} contains unsupported JSON type {type(value).__name__}"
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
    _require_exact_fields(value, _VERDICT_FIELDS, where="verdict")
    if value["schema"] != VERDICT_SCHEMA:
        raise EvidenceError(f"verdict schema must be {VERDICT_SCHEMA}")
    verdict = value["verdict"]
    if verdict not in {"SPECIALIZED", "REJECT", "INCONCLUSIVE"}:
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
    except (json.JSONDecodeError, UnicodeError) as exc:
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
_WIN_READ_CONTROL = 0x00020000
_WIN_FILE_READ_ATTRIBUTES = 0x00000080
_WIN_FILE_SHARE_READ = 0x00000001
_WIN_FILE_SHARE_WRITE = 0x00000002
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
_WIN_ERROR_FILE_EXISTS = 80
_WIN_ERROR_ALREADY_EXISTS = 183
_WIN_SE_FILE_OBJECT = 1
_WIN_DACL_SECURITY_INFORMATION = 0x00000004
_WIN_SDDL_REVISION_1 = 1
_WIN_PRIVATE_DACL_SDDL = "D:P(A;;FA;;;OW)"


class _WinSecurityAttributes(ctypes.Structure):
    _fields_ = [
        ("length", ctypes.c_uint32),
        ("security_descriptor", ctypes.c_void_p),
        ("inherit_handle", ctypes.c_int),
    ]


class _WinFileTime(ctypes.Structure):
    _fields_ = [
        ("low", ctypes.c_uint32),
        ("high", ctypes.c_uint32),
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
    def identity(self) -> tuple[int, int, int]:
        return (
            self.volume_serial_number,
            self.file_index_high,
            self.file_index_low,
        )

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
        self._kernel32.RemoveDirectoryW.argtypes = [ctypes.c_wchar_p]
        self._kernel32.RemoveDirectoryW.restype = ctypes.c_int
        self._kernel32.DeleteFileW.argtypes = [ctypes.c_wchar_p]
        self._kernel32.DeleteFileW.restype = ctypes.c_int
        self._kernel32.GetFileAttributesW.argtypes = [ctypes.c_wchar_p]
        self._kernel32.GetFileAttributesW.restype = ctypes.c_uint32
        self._kernel32.GetFileInformationByHandle.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(_WinByHandleFileInformation),
        ]
        self._kernel32.GetFileInformationByHandle.restype = ctypes.c_int
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
            enabled=private
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
    ) -> object:
        if not enabled:
            yield None
            return
        descriptor = ctypes.c_void_p()
        if not self._advapi32.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            _WIN_PRIVATE_DACL_SDDL,
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

    def create_directory(self, path: Path) -> None:
        with self._private_security_attributes() as security_attributes:
            if not self._kernel32.CreateDirectoryW(
                os.fspath(path),
                security_attributes,
            ):
                self._raise_windows_error(self._last_error(), path)

    def remove_directory(self, path: Path) -> None:
        if not self._kernel32.RemoveDirectoryW(os.fspath(path)):
            self._raise_windows_error(self._last_error(), path)

    def delete_file(self, path: Path) -> None:
        if not self._kernel32.DeleteFileW(os.fspath(path)):
            self._raise_windows_error(self._last_error(), path)

    def path_is_directory(self, path: Path) -> bool:
        attributes = int(
            self._kernel32.GetFileAttributesW(os.fspath(path))
        )
        if attributes == 0xFFFFFFFF:
            self._raise_windows_error(self._last_error(), path)
        return bool(attributes & _WIN_FILE_ATTRIBUTE_DIRECTORY)

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

    def private_dacl_sddl(self, handle: object) -> str:
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
            return text.value or ""
        finally:
            if text:
                self._kernel32.LocalFree(
                    ctypes.cast(text, ctypes.c_void_p)
                )
            self._kernel32.LocalFree(descriptor)

    def require_private_acl(self, handle: object) -> None:
        if self.private_dacl_sddl(handle) != _WIN_PRIVATE_DACL_SDDL:
            raise EvidenceError(
                "Windows evidence object lacks the exact private DACL"
            )

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
                share=_WIN_FILE_SHARE_READ | _WIN_FILE_SHARE_WRITE,
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
            | (_WIN_READ_CONTROL if require_private else 0)
        ),
        share=_WIN_FILE_SHARE_READ | _WIN_FILE_SHARE_WRITE,
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
        before = api.info(handle)
        data = api.read_all(handle, before.size)
        after = api.info(handle)
        if (
            before.identity != after.identity
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
        if source_parent_fd is not None or destination_parent_fd is not None:
            raise UnsupportedPlatformError(
                "Windows non-replacing move requires absolute paths"
            )
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        move_file = kernel32.MoveFileExW
        move_file.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_wchar_p,
            ctypes.c_uint,
        ]
        move_file.restype = ctypes.c_int
        if not move_file(os.fspath(source), os.fspath(destination), 0):
            error_number = ctypes.get_last_error()
            if error_number in {80, 183}:
                raise FileExistsError(
                    error_number,
                    "destination already exists",
                    os.fspath(destination),
                )
            raise OSError(
                error_number,
                "MoveFileExW failed",
                os.fspath(destination),
            )
        return
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
        metadata: dict[str, object],
    ) -> None:
        self._final_path = final_path
        self._stage_path = stage_path
        self._parent_fd = parent_fd
        self._stage_fd = stage_fd
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
        stage_name = (
            f".{final_path.name}.staging-{os.getpid()}-"
            f"{secrets.token_hex(16)}"
        )
        stage_path = final_path.parent / stage_name
        stage_fd: int | None = None
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
            nofollow = _require_no_follow()
            stage_fd = os.open(
                stage_name,
                os.O_RDONLY
                | nofollow
                | getattr(os, "O_DIRECTORY", 0)
                | getattr(os, "O_CLOEXEC", 0),
                dir_fd=parent_fd,
            )
            if stat.S_IMODE(os.fstat(stage_fd).st_mode) != 0o700:
                os.fchmod(stage_fd, 0o700)
            bundle = cls(
                final_path,
                stage_path,
                parent_fd,
                stage_fd,
                validated_metadata,
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
                os.close(stage_fd)
            try:
                os.rmdir(stage_name, dir_fd=parent_fd)
            except OSError:
                pass
            os.close(parent_fd)
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
            _atomic_rename_noreplace(
                self._stage_path,
                self._final_path,
                self._parent_fd,
                self._parent_fd,
            )
        except BaseException:
            self._abort()
            raise
        self._active = False
        publication_error: OSError | None = None
        try:
            os.close(self._stage_fd)
        except OSError as exc:
            publication_error = exc
        self._stage_fd = -1
        try:
            _fsync_directory(self._parent_fd)
        except OSError as exc:
            if publication_error is None:
                publication_error = exc
        finally:
            try:
                os.close(self._parent_fd)
            except OSError as exc:
                if publication_error is None:
                    publication_error = exc
            self._parent_fd = -1
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
                names = os.listdir(self._stage_fd)
            except OSError:
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
                except OSError:
                    pass
            try:
                os.close(self._stage_fd)
            except OSError:
                pass
            self._stage_fd = -1
            try:
                os.rmdir(
                    self._stage_path.name,
                    dir_fd=self._parent_fd,
                )
            except OSError:
                pass
        finally:
            try:
                os.close(self._parent_fd)
            except OSError:
                pass
            self._parent_fd = -1
            self._active = False


class _WindowsEvidenceBundle:
    """Win32 writer using held non-reparse ancestor handles."""

    def __init__(
        self,
        final_path: Path,
        stage_path: Path,
        api: _WindowsAPI,
        parent_handles: list[object],
        stage_handle: object,
        metadata: dict[str, object],
    ) -> None:
        self._final_path = final_path
        self._stage_path = stage_path
        self._api = api
        self._parent_handles = parent_handles
        self._stage_handle: object | None = stage_handle
        self._metadata = metadata
        self._written: set[str] = set()
        self._active = True
        self._finalize_called = False
        self._stage_exists = True

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
        )
        stage_path = final_path.parent / (
            f".{final_path.name}.staging-{os.getpid()}-"
            f"{secrets.token_hex(16)}"
        )
        stage_handle: object | None = None
        stage_created = False
        writer_owns_resources = False
        try:
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
            stage_created = True
            stage_handle = _win_open_directory(
                stage_path,
                api=api,
                require_private=True,
                writable=True,
            )
            writer = cls(
                final_path,
                stage_path,
                api,
                parent_handles,
                stage_handle,
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
            if stage_created:
                try:
                    api.remove_directory(stage_path)
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

    def finalize(self) -> Path:
        self._ensure_active()
        if self._finalize_called:
            raise RuntimeError("finalize is single-use")
        self._finalize_called = True
        try:
            self._validate_before_finalize()
            if self._stage_handle is None:
                raise EvidenceError("Windows staging handle is unavailable")
            self._api.flush_directory(self._stage_handle)
            self._write_owned(MANIFEST_NAME, self._build_manifest())
            self._api.flush_directory(self._stage_handle)
            self._api.close(self._stage_handle)
            self._stage_handle = None
            _atomic_rename_noreplace(
                self._stage_path,
                self._final_path,
                None,
                None,
            )
            self._stage_exists = False
        except BaseException:
            self._abort()
            raise
        self._active = False
        publication_error: OSError | None = None
        try:
            self._api.flush_directory(self._parent_handles[-1])
        except OSError as exc:
            publication_error = exc
        while self._parent_handles:
            handle = self._parent_handles.pop()
            try:
                self._api.close(handle)
            except OSError as exc:
                if publication_error is None:
                    publication_error = exc
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
            if self._stage_handle is not None:
                try:
                    self._api.close(self._stage_handle)
                except OSError:
                    pass
                self._stage_handle = None
            if self._stage_exists:
                try:
                    names = os.listdir(self._stage_path)
                except OSError:
                    names = []
                for name in names:
                    entry = self._stage_path / name
                    try:
                        if self._api.path_is_directory(entry):
                            self._api.remove_directory(entry)
                        else:
                            self._api.delete_file(entry)
                    except OSError:
                        pass
                try:
                    self._api.remove_directory(self._stage_path)
                    self._stage_exists = False
                except OSError:
                    pass
        finally:
            _win_close_handles(self._api, self._parent_handles)
            self._active = False


def _win_read_bundle_payloads(
    path: Path | str,
) -> dict[str, bytes]:
    api = _WindowsAPI()
    bundle_path, handles = _win_open_directory_chain(
        _safe_absolute(path),
        api=api,
        require_private_leaf=True,
    )
    try:
        names = os.listdir(bundle_path)
        if set(names) != _ALL_ARTIFACTS or len(names) != len(
            _ALL_ARTIFACTS
        ):
            raise EvidenceError(
                "bundle membership mismatch: "
                f"expected={sorted(_ALL_ARTIFACTS)} "
                f"actual={sorted(names)}"
            )
        payloads = {
            name: _win_read_regular_file(
                bundle_path / name,
                api=api,
            )
            for name in sorted(_ALL_ARTIFACTS)
        }
        after = os.listdir(bundle_path)
        if set(after) != set(names) or len(after) != len(names):
            raise EvidenceError(
                "bundle membership changed during Windows audit"
            )
        return payloads
    finally:
        _win_close_handles(api, handles)


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
        payloads = _win_read_bundle_payloads(path)
    elif platform_kind == "posix":
        bundle_path = _safe_absolute(path)
        directory_fd = _open_directory_nofollow(bundle_path)
        try:
            if stat.S_IMODE(os.fstat(directory_fd).st_mode) != 0o700:
                raise EvidenceError(
                    "bundle directory mode must be exactly 0700"
                )
            names = os.listdir(directory_fd)
            if set(names) != _ALL_ARTIFACTS or len(names) != len(
                _ALL_ARTIFACTS
            ):
                raise EvidenceError(
                    "bundle membership mismatch: "
                    f"expected={sorted(_ALL_ARTIFACTS)} "
                    f"actual={sorted(names)}"
                )
            payloads = {
                name: _read_regular_file(directory_fd, name)
                for name in sorted(_ALL_ARTIFACTS)
            }
            after = os.listdir(directory_fd)
            if set(after) != set(names) or len(after) != len(names):
                raise EvidenceError(
                    "bundle membership changed during POSIX audit"
                )
        finally:
            os.close(directory_fd)
    else:
        raise UnsupportedPlatformError(
            f"secure evidence audit unsupported on {sys.platform}"
        )

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
    )
