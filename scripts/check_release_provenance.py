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
    parser.add_argument("provenance", type=Path, nargs="+")
    args = parser.parse_args()

    try:
        for provenance in args.provenance:
            check_release_provenance(provenance)
    except ValueError as error:
        print(error, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
