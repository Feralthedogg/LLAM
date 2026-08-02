#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Feralthedogg
"""Require every C-level fiber switch to pass through the common gateway."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


SWITCH_PATTERN = re.compile(r"\bllam_ctx_switch\s*\(")
GATEWAY = "src/core/base/errno.c"
DECLARATION = "src/internal/llam_internal.h"
CONTEXT_PREFIX = "src/core/context/"


def is_allowed_declaration(relative: str, line: str) -> bool:
    stripped = line.lstrip()
    if not stripped.startswith("void llam_ctx_switch"):
        return False
    return relative == DECLARATION or relative.startswith(CONTEXT_PREFIX)


def source_paths(root: Path) -> list[Path]:
    source_root = root / "src"
    if not source_root.is_dir():
        return []
    return sorted(
        path
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix in {".c", ".h"}
    )


def audit(root: Path) -> tuple[list[str], int]:
    diagnostics: list[str] = []
    gateway_occurrences = 0

    for path in source_paths(root):
        relative = path.relative_to(root).as_posix()
        try:
            lines = path.read_text(encoding="utf-8").splitlines()
        except (OSError, UnicodeError) as exc:
            diagnostics.append(
                f"cannot read switch source: {relative}: "
                f"{exc.__class__.__name__}"
            )
            continue
        for line_number, line in enumerate(lines, start=1):
            if SWITCH_PATTERN.search(line) is None:
                continue
            if relative == GATEWAY:
                gateway_occurrences += 1
                continue
            if is_allowed_declaration(relative, line):
                continue
            diagnostics.append(
                f"direct context switch outside gateway: "
                f"{relative}:{line_number}"
            )

    if gateway_occurrences == 0:
        diagnostics.append(f"switch gateway has no context switch: {GATEWAY}")
    return diagnostics, gateway_occurrences


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--check", action="store_true", required=True)
    args = parser.parse_args(argv)

    if not args.root.is_dir():
        print(f"invalid repository root: {args.root}", file=sys.stderr)
        return 2
    diagnostics, gateway_occurrences = audit(args.root)
    if diagnostics:
        for diagnostic in diagnostics:
            print(diagnostic, file=sys.stderr)
        return 1
    print(
        "context switch gateway audit passed: "
        f"{gateway_occurrences} gateway calls"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
