#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Verify that a LLAM shared library exports exactly the public C ABI."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
PUBLIC_HEADERS = ("runtime.h", "io.h", "platform.h")
PUBLIC_FN_RE = re.compile(r"\bLLAM_API\b[\s\S]*?\b(llam_[A-Za-z0-9_]+)\s*\(")
LLAM_SYMBOL_RE = re.compile(r"^_?(llam_[A-Za-z0-9_]+)$")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*[\s\S]*?\*/", "", text)
    text = re.sub(r"//.*", "", text)
    return text


def declared_public_symbols(root: pathlib.Path) -> set[str]:
    symbols: set[str] = set()
    include_dir = root / "include" / "llam"

    for name in PUBLIC_HEADERS:
        path = include_dir / name
        if not path.exists():
            raise FileNotFoundError(f"missing public header: {path}")
        symbols.update(PUBLIC_FN_RE.findall(strip_comments(path.read_text(encoding="utf-8"))))
    return symbols


def run_nm(library: pathlib.Path) -> str:
    commands = (("nm", "-g", str(library)), ("llvm-nm", "-g", str(library)))
    failures: list[str] = []

    for command in commands:
        try:
            proc = subprocess.run(command,
                                  check=False,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE,
                                  text=True)
        except FileNotFoundError as exc:
            failures.append(f"{command[0]}: {exc}")
            continue
        if proc.returncode == 0:
            return proc.stdout
        failures.append(f"{' '.join(command)}: {proc.stderr.strip() or proc.returncode}")
    raise RuntimeError("unable to inspect shared library exports: " + "; ".join(failures))


def exported_llam_symbols(library: pathlib.Path) -> set[str]:
    symbols: set[str] = set()

    for line in run_nm(library).splitlines():
        parts = line.split()
        if not parts:
            continue
        symbol = parts[-1]
        symbol_type = parts[-2] if len(parts) >= 2 and len(parts[-2]) == 1 else None
        if symbol_type == "U":
            continue
        match = LLAM_SYMBOL_RE.match(symbol)
        if match is not None:
            symbols.add(match.group(1))
    return symbols


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=pathlib.Path, help="shared library to inspect")
    parser.add_argument("--root", type=pathlib.Path, default=ROOT, help="repository root")
    parser.add_argument("--dump", action="store_true", help="print matching symbol sets on success")
    args = parser.parse_args(argv)

    library = args.library
    root = args.root.resolve()
    if not library.exists():
        print(f"audit_shared_exports: missing shared library: {library}", file=sys.stderr)
        return 2

    public = declared_public_symbols(root)
    exported = exported_llam_symbols(library)
    extra = sorted(exported - public)
    missing = sorted(public - exported)

    if extra or missing:
        print("audit_shared_exports: public ABI/export mismatch", file=sys.stderr)
        if extra:
            print("extra exports:", ", ".join(extra), file=sys.stderr)
        if missing:
            print("missing exports:", ", ".join(missing), file=sys.stderr)
        return 1

    if args.dump:
        print("public/exported symbols:")
        for symbol in sorted(public):
            print(symbol)
    print(f"audit_shared_exports ok: {len(public)} public symbol(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
