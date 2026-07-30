#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
"""Audit C structure boundaries and ratchet visible size debt."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
from typing import Any


AUDITED_DIRECTORIES = ("include", "src", "examples", "tests", "experiments")
FORBIDDEN_STEMS = {
    "common",
    "misc",
    "helper",
    "helpers",
    "utils",
    "manager",
    "handler",
    "processor",
    "data",
    "logic",
    "module",
    "function",
}
SOURCE_EXTS = {".c", ".h", ".inc", ".S"}
SPLIT_LINES = 800
BASELINE_SCHEMA = "llam.c-structure-baseline.v1"
SIZE_BUDGETS = {
    # Public ABI and intentionally dense runtime internals. These files are
    # still audited: ratchet mode rejects growth beyond recorded debt.
    "examples/bench_support.c": 1100,
    "examples/demo_entry.c": 700,
    "examples/demo_tasks.c": 700,
    "examples/server.c": 1500,
    "examples/server_flood.c": 1100,
    "examples/stress_core_cases.c": 700,
    "examples/stress_dynamic_cases.c": 850,
    "examples/stress_suite.c": 800,
    "examples/stress_support.c": 850,
    "examples/stress_tasks.c": 700,
    "examples/stress_timeout_cases.c": 750,
    "include/llam/runtime.h": 1400,
    "src/core/memory/alloc.c": 800,
    "src/core/api/blocking_api.c": 700,
    "src/core/sync/channel.c": 1200,
    "src/core/sync/channel_select.c": 700,
    "src/core/debug/debug.c": 750,
    "src/core/lifecycle/init.c": 1250,
    "src/core/platform/platform.c": 650,
    "src/core/sched/core_queue.c": 800,
    "src/core/sched/scheduler.c": 650,
    "src/core/task/task_stack.c": 950,
    "src/core/time/timer.c": 750,
    "src/core/sched/wake.c": 1100,
    "src/core/task/yield_join_sleep.c": 1050,
    "src/engine/scheduler/scheduler_engine.c": 650,
    "src/engine/watchdog/watchdog_rehome.c": 900,
    "src/internal/runtime_types.h": 1200,
    "src/internal/runtime_windows_compat.h": 900,
    "src/io/darwin/watch/darwin_migration_live.c": 750,
    "src/io/darwin/watch/darwin_migration_rehome.c": 900,
    "src/io/darwin/watch/darwin_state.c": 800,
    "src/io/linux/watch/cqe.c": 650,
    "src/io/linux/watch/linux_migration_live.c": 750,
    "src/io/linux/watch/linux_migration_rehome.c": 900,
    "src/io/linux/watch/linux_state.c": 650,
    "src/io/api/blocking_ops.c": 800,
    "src/io/api/direct.c": 850,
    "src/io/api/direct_tuning.c": 850,
    "src/io/api/public.c": 1200,
    "src/io/engine/io_engine.c": 1000,
}


class AuditInputError(Exception):
    """The audit could not safely interpret its selected input."""


class DuplicateKeyError(ValueError):
    """A JSON object repeated a key."""


class NonFiniteJSONError(ValueError):
    """JSON used a non-standard NaN or infinity constant."""


@dataclass(frozen=True)
class Finding:
    path: str
    line_count: int
    limit: int
    category: str
    severity: str


@dataclass(frozen=True)
class BaselineEntry:
    path: str
    line_count: int
    limit: int


def valid_relative_path(value: str) -> bool:
    path = PurePosixPath(value)
    return (
        bool(value)
        and not path.is_absolute()
        and "\\" not in value
        and ".." not in path.parts
        and "." not in path.parts
        and path.parts[0] in AUDITED_DIRECTORIES
        and str(path) == value
    )


def relative_path(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def reject_symlink_components(
    root: Path,
    relative: str,
    label: str,
) -> None:
    current = root
    parts = PurePosixPath(relative).parts
    for index, part in enumerate(parts):
        current /= part
        display = PurePosixPath(*parts[: index + 1]).as_posix()
        try:
            mode = current.lstat().st_mode
        except OSError as exc:
            raise AuditInputError(
                f"{label} path cannot be inspected at {display}: "
                f"{exc.__class__.__name__}"
            ) from exc
        if stat.S_ISLNK(mode):
            raise AuditInputError(
                f"{label} path traverses symlink at {display}"
            )
        if index < len(parts) - 1 and not stat.S_ISDIR(mode):
            raise AuditInputError(
                f"{label} path ancestor is not a directory: {display}"
            )


def read_regular_bytes(root: Path, relative: str, label: str) -> bytes:
    reject_symlink_components(root, relative, label)
    parts = PurePosixPath(relative).parts
    base_flags = (
        os.O_RDONLY
        | getattr(os, "O_CLOEXEC", 0)
        | getattr(os, "O_BINARY", 0)
    )
    secure_openat = (
        os.name != "nt"
        and os.open in os.supports_dir_fd
        and hasattr(os, "O_DIRECTORY")
        and hasattr(os, "O_NOFOLLOW")
    )
    leaf_fd: int | None = None
    if secure_openat:
        root_fd: int | None = None
        directory_fd: int | None = None
        try:
            directory_flags = (
                base_flags | os.O_DIRECTORY | os.O_NOFOLLOW
            )
            root_fd = os.open(root, directory_flags)
            directory_fd = root_fd
            for part in parts[:-1]:
                next_fd = os.open(
                    part,
                    directory_flags,
                    dir_fd=directory_fd,
                )
                if directory_fd != root_fd:
                    os.close(directory_fd)
                directory_fd = next_fd
            leaf_fd = os.open(
                parts[-1],
                base_flags | os.O_NOFOLLOW,
                dir_fd=directory_fd,
            )
            opened = os.fstat(leaf_fd)
            if not stat.S_ISREG(opened.st_mode):
                raise AuditInputError(
                    f"{label} path is not a regular file"
                )
            with os.fdopen(leaf_fd, "rb") as stream:
                leaf_fd = None
                return stream.read()
        except AuditInputError:
            raise
        except OSError as exc:
            raise AuditInputError(
                f"{label} path cannot be opened without following "
                f"symlinks: {exc.__class__.__name__}"
            ) from exc
        finally:
            if leaf_fd is not None:
                os.close(leaf_fd)
            if directory_fd is not None and directory_fd != root_fd:
                os.close(directory_fd)
            if root_fd is not None:
                os.close(root_fd)

    path = root / relative
    try:
        leaf_fd = os.open(path, base_flags)
        opened = os.fstat(leaf_fd)
        if not stat.S_ISREG(opened.st_mode):
            raise AuditInputError(f"{label} path is not a regular file")
        with os.fdopen(leaf_fd, "rb") as stream:
            leaf_fd = None
            contents = stream.read()
        reject_symlink_components(root, relative, label)
        inspected = path.lstat()
        if (
            inspected.st_dev,
            inspected.st_ino,
            inspected.st_mode,
        ) != (
            opened.st_dev,
            opened.st_ino,
            opened.st_mode,
        ):
            raise AuditInputError(
                f"{label} path identity changed while reading"
            )
        path.resolve(strict=True).relative_to(root)
        return contents
    except AuditInputError:
        raise
    except ValueError as exc:
        raise AuditInputError(f"{label} path escapes root") from exc
    except OSError as exc:
        raise AuditInputError(
            f"{label} path cannot be safely read: "
            f"{exc.__class__.__name__}"
        ) from exc
    finally:
        if leaf_fd is not None:
            os.close(leaf_fd)


def resolve_root(path: Path) -> Path:
    try:
        root = path.resolve(strict=True)
        mode = root.stat().st_mode
    except (OSError, RuntimeError) as exc:
        raise AuditInputError(
            f"root cannot be resolved: {exc.__class__.__name__}"
        ) from exc
    if not stat.S_ISDIR(mode):
        raise AuditInputError("root is not a directory")
    return root


def iter_source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for name in AUDITED_DIRECTORIES:
        directory = root / name
        try:
            mode = directory.lstat().st_mode
        except FileNotFoundError:
            continue
        except OSError as exc:
            raise AuditInputError(
                f"{name}: cannot inspect audit scope: "
                f"{exc.__class__.__name__}"
            ) from exc
        if stat.S_ISLNK(mode):
            raise AuditInputError(f"{name}: audit scope is a symlink")
        if not stat.S_ISDIR(mode):
            raise AuditInputError(f"{name}: audit scope is not a directory")

        try:
            walk = os.walk(directory, topdown=True, followlinks=False)
            for current, directory_names, file_names in walk:
                directory_names.sort()
                file_names.sort()
                current_path = Path(current)
                for child_name in directory_names:
                    child = current_path / child_name
                    child_mode = child.lstat().st_mode
                    if stat.S_ISLNK(child_mode):
                        raise AuditInputError(
                            f"{relative_path(root, child)}: "
                            "audit path traverses symlink"
                        )
                for child_name in file_names:
                    child = current_path / child_name
                    child_mode = child.lstat().st_mode
                    if stat.S_ISLNK(child_mode):
                        raise AuditInputError(
                            f"{relative_path(root, child)}: "
                            "audit path is a symlink"
                        )
                    if child.suffix not in SOURCE_EXTS:
                        continue
                    if not stat.S_ISREG(child_mode):
                        raise AuditInputError(
                            f"{relative_path(root, child)}: "
                            "source path is not a regular file"
                        )
                    files.append(child)
        except AuditInputError:
            raise
        except OSError as exc:
            raise AuditInputError(
                f"{name}: cannot enumerate audit scope: "
                f"{exc.__class__.__name__}"
            ) from exc
    return sorted(files, key=lambda path: relative_path(root, path))


def read_text(root: Path, path: Path) -> str:
    try:
        relative = relative_path(root, path)
        return read_regular_bytes(
            root,
            relative,
            f"source {relative}",
        ).decode(encoding="utf-8", errors="replace")
    except AuditInputError:
        raise
    except (OSError, UnicodeError) as exc:
        raise AuditInputError(
            f"{relative_path(root, path)}: cannot read source: "
            f"{exc.__class__.__name__}"
        ) from exc


def line_count(root: Path, path: Path) -> int:
    return len(read_text(root, path).splitlines())


def public_header_boundary_errors(
    root: Path,
    files: list[Path],
) -> list[str]:
    errors: list[str] = []
    public_include_dir = root / "include" / "llam"
    internal_include_dir = root / "src" / "internal"
    if (root / "include" / "internal").exists():
        errors.append(
            "include/internal must not exist; "
            "internal headers live under src/internal"
        )
    for path in files:
        relative = relative_path(root, path)
        if (
            relative.startswith("include/")
            and path.suffix == ".h"
            and not relative.startswith("include/llam/")
        ):
            errors.append(f"public header outside include/llam: {relative}")
    if not public_include_dir.is_dir():
        errors.append("missing include/llam public API directory")
    if not internal_include_dir.is_dir():
        errors.append("missing src/internal private header directory")
    return errors


def include_boundary_errors(root: Path, files: list[Path]) -> list[str]:
    errors: list[str] = []
    bad_patterns = (
        re.compile(r'#\s*include\s+"(?:\.\./)*include/internal/'),
        re.compile(r'#\s*include\s+"(?:\.\./)*include/nm_'),
    )
    for path in files:
        for index, line in enumerate(
            read_text(root, path).splitlines(),
            start=1,
        ):
            for pattern in bad_patterns:
                if pattern.search(line):
                    errors.append(
                        f"{relative_path(root, path)}:{index}: "
                        f"forbidden include path: {line.strip()}"
                    )
    return errors


def naming_errors(root: Path, files: list[Path]) -> list[str]:
    errors: list[str] = []
    for path in files:
        if path.suffix == ".c" and path.stem in FORBIDDEN_STEMS:
            errors.append(
                f"{relative_path(root, path)}: "
                "forbidden broad filename stem"
            )
    return errors


def configured_limit(path: str) -> int:
    return SIZE_BUDGETS.get(path, SPLIT_LINES)


def size_finding(root: Path, path: Path) -> Finding | None:
    relative = relative_path(root, path)
    count = line_count(root, path)
    budget = SIZE_BUDGETS.get(relative)
    if budget is not None:
        if count > budget:
            return Finding(
                path=relative,
                line_count=count,
                limit=budget,
                category="size_budget",
                severity="warning",
            )
        return None
    if count >= SPLIT_LINES:
        return Finding(
            path=relative,
            line_count=count,
            limit=SPLIT_LINES,
            category="split_candidate",
            severity="warning",
        )
    return None


def structured_size_findings(
    root: Path,
    files: list[Path],
) -> list[Finding]:
    findings: list[Finding] = []
    for path in files:
        if path.suffix not in {".c", ".h", ".inc"}:
            continue
        finding = size_finding(root, path)
        if finding is not None:
            findings.append(finding)
    return sorted(
        findings,
        key=lambda finding: (
            finding.path,
            finding.category,
            finding.limit,
        ),
    )


def require_exact_object(
    value: Any,
    location: str,
    fields: set[str],
) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise AuditInputError(f"{location}: expected object")
    actual = set(value)
    if actual != fields:
        missing = sorted(fields - actual)
        unknown = sorted(actual - fields)
        details: list[str] = []
        if missing:
            details.append(f"missing {', '.join(missing)}")
        if unknown:
            details.append(f"unknown {', '.join(unknown)}")
        raise AuditInputError(f"{location}: {'; '.join(details)}")
    return value


def duplicate_rejecting_object(
    pairs: list[tuple[str, Any]],
) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateKeyError(key)
        result[key] = value
    return result


def reject_non_finite_constant(value: str) -> None:
    raise NonFiniteJSONError(value)


def resolve_baseline(
    root: Path,
    logical_root: Path,
    selected: Path,
) -> str:
    candidate = (
        Path(os.path.abspath(selected))
        if selected.is_absolute()
        else logical_root / selected
    )
    try:
        relative = candidate.relative_to(logical_root).as_posix()
    except ValueError as exc:
        raise AuditInputError("baseline must stay within root") from exc
    path = PurePosixPath(relative)
    if (
        not relative
        or path.is_absolute()
        or "\\" in relative
        or ".." in path.parts
        or "." in path.parts
        or str(path) != relative
    ):
        raise AuditInputError(
            "baseline path must be canonical and relative to root"
        )
    reject_symlink_components(root, relative, "baseline")
    try:
        mode = (root / relative).lstat().st_mode
    except OSError as exc:
        raise AuditInputError(
            f"baseline cannot be inspected: {exc.__class__.__name__}"
        ) from exc
    if not stat.S_ISREG(mode):
        raise AuditInputError("baseline is not a regular file")
    return relative


def load_baseline(
    root: Path,
    logical_root: Path,
    selected: Path,
) -> dict[str, BaselineEntry]:
    relative = resolve_baseline(root, logical_root, selected)
    try:
        value = json.loads(
            read_regular_bytes(root, relative, "baseline").decode("utf-8"),
            object_pairs_hook=duplicate_rejecting_object,
            parse_constant=reject_non_finite_constant,
        )
    except DuplicateKeyError as exc:
        raise AuditInputError(
            f"baseline contains duplicate JSON key {exc.args[0]!r}"
        ) from exc
    except NonFiniteJSONError as exc:
        raise AuditInputError(
            f"baseline contains non-finite JSON constant {exc.args[0]!r}"
        ) from exc
    except (RecursionError, OverflowError) as exc:
        raise AuditInputError(
            "baseline JSON exceeds the supported nesting depth"
        ) from exc
    except json.JSONDecodeError as exc:
        raise AuditInputError("baseline must contain valid JSON") from exc
    except UnicodeError as exc:
        raise AuditInputError(
            f"baseline cannot be read: {exc.__class__.__name__}"
        ) from exc

    document = require_exact_object(
        value,
        "baseline",
        {"schema", "entries"},
    )
    if document["schema"] != BASELINE_SCHEMA:
        raise AuditInputError(
            "baseline schema must be "
            f"{BASELINE_SCHEMA!r}"
        )
    raw_entries = document["entries"]
    if not isinstance(raw_entries, list):
        raise AuditInputError("baseline.entries: expected array")

    entries: dict[str, BaselineEntry] = {}
    order: list[str] = []
    for index, raw_entry in enumerate(raw_entries):
        location = f"baseline.entries[{index}]"
        entry = require_exact_object(
            raw_entry,
            location,
            {"path", "line_count", "limit"},
        )
        relative = entry["path"]
        if not isinstance(relative, str) or not valid_relative_path(relative):
            raise AuditInputError(
                f"{location}.path: expected canonical relative path"
            )
        if PurePosixPath(relative).suffix not in {".c", ".h", ".inc"}:
            raise AuditInputError(
                f"{location}.path: unsupported structure-audit suffix"
            )
        if relative in entries:
            raise AuditInputError(
                f"{location}.path: duplicate entry {relative}"
            )
        count = entry["line_count"]
        limit = entry["limit"]
        if (
            isinstance(count, bool)
            or not isinstance(count, int)
            or count < 0
        ):
            raise AuditInputError(
                f"{location}.line_count: expected non-negative integer"
            )
        if (
            isinstance(limit, bool)
            or not isinstance(limit, int)
            or limit <= 0
        ):
            raise AuditInputError(
                f"{location}.limit: expected positive integer"
            )
        expected_limit = configured_limit(relative)
        if limit != expected_limit:
            raise AuditInputError(
                f"{location}.limit: expected configured limit "
                f"{expected_limit}, got {limit}"
            )
        if relative in SIZE_BUDGETS:
            visible_debt = count > limit
        else:
            visible_debt = count >= limit
        if not visible_debt:
            raise AuditInputError(
                f"{location}.line_count: entry does not record visible debt"
            )
        entries[relative] = BaselineEntry(
            path=relative,
            line_count=count,
            limit=limit,
        )
        order.append(relative)
    if order != sorted(order):
        raise AuditInputError("baseline.entries: paths must be sorted")
    return entries


def apply_mode(
    findings: list[Finding],
    mode: str,
    baseline: dict[str, BaselineEntry] | None,
) -> list[Finding]:
    if mode == "report":
        return findings
    if mode == "strict":
        return [
            Finding(
                path=finding.path,
                line_count=finding.line_count,
                limit=finding.limit,
                category=finding.category,
                severity="error",
            )
            for finding in findings
        ]
    if baseline is None:
        raise AssertionError("ratchet mode requires a parsed baseline")

    result: list[Finding] = []
    for finding in findings:
        recorded = baseline.get(finding.path)
        if recorded is None:
            result.append(
                Finding(
                    path=finding.path,
                    line_count=finding.line_count,
                    limit=finding.limit,
                    category=finding.category,
                    severity="error",
                )
            )
        elif finding.line_count > recorded.line_count:
            result.append(
                Finding(
                    path=finding.path,
                    line_count=finding.line_count,
                    limit=recorded.line_count,
                    category="growth",
                    severity="error",
                )
            )
        else:
            result.append(finding)
    return result


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit C project structure and module boundary rules.",
    )
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--mode",
        choices=("report", "ratchet", "strict"),
        default="report",
    )
    parser.add_argument("--baseline", type=Path)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.mode == "ratchet" and args.baseline is None:
        print(
            "audit-c-structure: --baseline is required in ratchet mode",
            file=sys.stderr,
        )
        return 2
    if args.mode != "ratchet" and args.baseline is not None:
        print(
            "audit-c-structure: --baseline is only valid in ratchet mode",
            file=sys.stderr,
        )
        return 2

    try:
        logical_root = Path(os.path.abspath(args.root))
        root = resolve_root(args.root)
        baseline = (
            load_baseline(root, logical_root, args.baseline)
            if args.mode == "ratchet"
            else None
        )
        files = iter_source_files(root)
        errors = sorted(
            public_header_boundary_errors(root, files)
            + include_boundary_errors(root, files)
            + naming_errors(root, files)
        )
        findings = apply_mode(
            structured_size_findings(root, files),
            args.mode,
            baseline,
        )
    except AuditInputError as exc:
        print(f"audit-c-structure: invalid input: {exc}", file=sys.stderr)
        return 2

    for finding in findings:
        print(json.dumps(asdict(finding), sort_keys=True))
    for error in errors:
        print(f"error: {error}", file=sys.stderr)

    policy_errors = sum(
        finding.severity == "error" for finding in findings
    )
    if errors or policy_errors:
        print(
            "audit failed: "
            f"{len(errors)} structure error(s), "
            f"{policy_errors} size policy error(s)",
            file=sys.stderr,
        )
        return 1
    print(
        "audit ok: "
        f"{len(files)} source/header file(s), "
        f"{len(findings)} visible size finding(s)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
