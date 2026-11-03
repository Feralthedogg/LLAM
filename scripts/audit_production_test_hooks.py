#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See LICENSE.

"""Audit production artifacts for externally callable test-hook symbols.

The production CMake targets intentionally compile small no-op fallback
functions when test hooks are disabled.  Those functions have local (lowercase
``nm`` type) symbols in unoptimized Debug archives/shared objects.  This audit
therefore uses ``nm -a`` but rejects only externally defined symbols (uppercase
symbol types); local fallback symbols are an explicit, documented exception.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
from typing import Iterable, Sequence


HOOK_RE = re.compile(r"^llam_[A-Za-z0-9_]*_test_[A-Za-z0-9_]*$")
SAFE_SELF_TEST_RE = re.compile(r"^llam_broker_client_self_test_")


def _nm_command() -> str | None:
    return shutil.which("nm") or shutil.which("llvm-nm")


def _run_nm(command: str, artifact: pathlib.Path) -> str:
    result = subprocess.run(
        [command, "-a", str(artifact)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"{command} failed for {artifact}")
    return result.stdout


def _symbols(output: str) -> Iterable[tuple[str, str]]:
    """Yield ``(nm type, normalized symbol)`` pairs from GNU/LLVM/Darwin nm."""

    for raw_line in output.splitlines():
        fields = raw_line.strip().split()
        if not fields:
            continue
        type_index = next(
            (
                index
                for index, field in enumerate(fields)
                if len(field) == 1 and (field.isalpha() or field == "?")
            ),
            None,
        )
        if type_index is None or type_index + 1 >= len(fields):
            continue
        symbol = fields[type_index + 1]
        # Darwin prefixes external C symbols with one underscore; ELF and
        # PE/COFF nm output generally do not.  Strip one prefix only so a
        # genuinely doubled source identifier remains distinguishable.
        if symbol.startswith("_"):
            symbol = symbol[1:]
        yield fields[type_index], symbol


def audit(artifacts: Sequence[pathlib.Path]) -> list[str]:
    command = _nm_command()
    if command is None:
        raise FileNotFoundError("nm/llvm-nm is not available")
    violations: list[str] = []
    for artifact in artifacts:
        if not artifact.is_file():
            violations.append(f"missing production artifact: {artifact}")
            continue
        try:
            output = _run_nm(command, artifact)
        except RuntimeError as error:
            violations.append(str(error))
            continue
        for symbol_type, symbol in _symbols(output):
            if not symbol_type.isupper() or symbol_type == "U":
                # Lowercase local fallback symbols are intentionally allowed;
                # undefined references are not definitions in this artifact.
                continue
            if HOOK_RE.fullmatch(symbol) and not SAFE_SELF_TEST_RE.match(symbol):
                violations.append(
                    f"{artifact}: externally callable test hook symbol "
                    f"{symbol} (nm type {symbol_type})"
                )
    return violations


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifacts", nargs="+", type=pathlib.Path)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        help="accepted for consistency with the other repository audits",
    )
    args = parser.parse_args(argv)
    try:
        violations = audit(args.artifacts)
    except FileNotFoundError as error:
        print(f"audit_production_test_hooks skipped: {error}")
        return 77
    if violations:
        for violation in violations:
            print(f"error: {violation}", file=sys.stderr)
        return 1
    print(
        "audit_production_test_hooks ok: "
        f"{len(args.artifacts)} production artifact(s); local fallback symbols allowed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
