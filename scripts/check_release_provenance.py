#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Reject release packaging unless build provenance records stable mode."""

from __future__ import annotations

import argparse
from pathlib import Path
import sys


def check_release_provenance(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"missing trustworthy build provenance: {path}")

    try:
        provenance = path.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise ValueError(
            f"cannot read trustworthy build provenance: {path}"
        ) from error

    if provenance == "LLAM_BUILD_RESEARCH=0\n":
        return
    if provenance == "LLAM_BUILD_RESEARCH=1\n":
        raise ValueError("research-enabled builds cannot be packaged")
    raise ValueError(f"invalid build provenance: {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provenance", type=Path, nargs="*")
    parser.add_argument(
        "--windows-artifacts",
        type=Path,
        nargs=4,
        metavar=("STATIC_LIB", "SHARED_DLL", "SHARED_IMPORT_LIB", "BENCH_EXE"),
    )
    args = parser.parse_args()
    provenance_paths = args.provenance
    if args.windows_artifacts is not None:
        if provenance_paths:
            parser.error(
                "provenance paths and --windows-artifacts are mutually exclusive"
            )
        provenance_paths = [
            Path(f"{artifact}.llam-build-provenance")
            for artifact in args.windows_artifacts
        ]
    elif not provenance_paths:
        parser.error("at least one provenance path is required")

    try:
        for provenance in provenance_paths:
            check_release_provenance(provenance)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
