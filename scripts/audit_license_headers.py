#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Feralthedogg
"""Audit license markers on LLAM's tracked code-bearing files."""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys


AUDITED_PREFIXES = (
    ".github/workflows/",
    "cmake/",
    "docker/",
    "examples/",
    "experiments/",
    "include/",
    "scripts/",
    "src/",
    "tests/",
)
CODE_SUFFIXES = {
    ".S",
    ".asm",
    ".c",
    ".cmake",
    ".go",
    ".h",
    ".inc",
    ".ps1",
    ".py",
    ".rs",
    ".s",
    ".sh",
    ".yaml",
    ".yml",
}
SPECIAL_NAMES = {"CMakeLists.txt", "Makefile"}
HEADER_LINE_LIMIT = 45
SPDX_PATTERN = re.compile(
    r"SPDX-License-Identifier:\s*Apache-2\.0(?:\s*\*/)?\s*$"
)
FULL_NOTICE = "Licensed under the Apache License, Version 2.0"


class InventoryError(RuntimeError):
    """Raised when the tracked-file inventory cannot be obtained."""


def is_code_bearing(relative: str) -> bool:
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts:
        return False
    if relative in SPECIAL_NAMES:
        return True
    if not relative.startswith(AUDITED_PREFIXES):
        return False
    if relative.startswith("docker/") and path.name.startswith("Dockerfile"):
        return True
    return path.suffix in CODE_SUFFIXES


def has_license_marker(text: str) -> bool:
    for line in text.splitlines()[:HEADER_LINE_LIMIT]:
        if SPDX_PATTERN.search(line) is not None or FULL_NOTICE in line:
            return True
    return False


def tracked_paths(root: Path) -> list[str]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z", "--cached"],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(
            errors="replace"
        ).strip()
        raise InventoryError(detail or "git ls-files failed")
    return [
        os.fsdecode(raw)
        for raw in result.stdout.split(b"\0")
        if raw
    ]


def audit(root: Path) -> tuple[list[str], int]:
    diagnostics: list[str] = []
    code_bearing_count = 0

    for relative in sorted(tracked_paths(root)):
        if not is_code_bearing(relative):
            continue
        code_bearing_count += 1
        path = root / relative
        try:
            mode = path.lstat().st_mode
        except OSError as exc:
            diagnostics.append(
                f"cannot inspect tracked file: {relative}: "
                f"{exc.__class__.__name__}"
            )
            continue
        if not stat.S_ISREG(mode):
            diagnostics.append(f"non-regular tracked file: {relative}")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            diagnostics.append(
                f"cannot read tracked file: {relative}: "
                f"{exc.__class__.__name__}"
            )
            continue
        if not has_license_marker(text):
            diagnostics.append(f"missing license marker: {relative}")

    return diagnostics, code_bearing_count


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
    )
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--check", action="store_true", required=True)
    args = parser.parse_args(argv)
    root = args.root

    if not root.is_dir():
        print(
            f"cannot inventory tracked files: invalid root: {root}",
            file=sys.stderr,
        )
        return 2
    try:
        diagnostics, count = audit(root)
    except InventoryError as exc:
        print(
            f"cannot inventory tracked files: {exc}",
            file=sys.stderr,
        )
        return 2
    if diagnostics:
        for diagnostic in diagnostics:
            print(diagnostic, file=sys.stderr)
        return 1
    print(f"license header audit passed: {count} code-bearing files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
