#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
"""Verify platform hardening markers on a linked LLAM shared library."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def audit_elf(
    program_headers: str,
    dynamic_section: str,
    symbols: str,
) -> list[str]:
    """Return missing ELF hardening properties."""

    missing: list[str] = []
    stack_lines = [
        line for line in program_headers.splitlines() if "GNU_STACK" in line
    ]
    stack_tokens = stack_lines[0].split() if len(stack_lines) == 1 else []
    if len(stack_tokens) < 2 or "E" in stack_tokens[-2]:
        missing.append("non-executable GNU_STACK")
    if "GNU_RELRO" not in program_headers:
        missing.append("GNU_RELRO")
    if "BIND_NOW" not in dynamic_section and "FLAGS" not in dynamic_section:
        missing.append("BIND_NOW")
    elif "BIND_NOW" not in dynamic_section and " NOW" not in dynamic_section:
        missing.append("BIND_NOW")
    if "__stack_chk_fail" not in symbols:
        missing.append("stack-protector reference")
    return missing


def audit_macho(headers: str, symbols: str) -> list[str]:
    """Return missing Mach-O hardening properties."""

    missing: list[str] = []
    if "MH_ALLOW_STACK_EXECUTION" in headers:
        missing.append("non-executable stack")
    if "___stack_chk_fail" not in symbols and "__stack_chk_fail" not in symbols:
        missing.append("stack-protector reference")
    return missing


def audit_pe(headers: str, symbols: str) -> list[str]:
    """Return missing PE/COFF hardening properties."""

    missing: list[str] = []
    normalized_headers = headers.upper()
    normalized_symbols = symbols.upper()
    if (
        "DYNAMIC BASE" not in normalized_headers
        and "DYNAMIC_BASE" not in normalized_headers
    ):
        missing.append("ASLR-enabled image")
    if (
        "NX COMPATIBLE" not in normalized_headers
        and "NX_COMPAT" not in normalized_headers
    ):
        missing.append("NX-compatible image")
    if (
        "GUARD CF" not in normalized_headers
        and "GUARD_CF" not in normalized_headers
    ):
        missing.append("Control Flow Guard")
    if (
        "__SECURITY_CHECK_COOKIE" not in normalized_symbols
        and "__SECURITY_COOKIE" not in normalized_symbols
    ):
        missing.append("stack-protector reference")
    return missing


def run_tool(arguments: list[str]) -> str:
    """Run one inspection tool and return its stdout."""

    result = subprocess.run(
        arguments,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(
            f"{Path(arguments[0]).name} failed with {result.returncode}: "
            f"{detail}"
        )
    return result.stdout


def find_tool(*names: str) -> str | None:
    """Return the first available executable from an ordered tool list."""

    for name in names:
        resolved = shutil.which(name)
        if resolved is not None:
            return resolved
    return None


def audit_artifact(path: Path, profile: str) -> list[str]:
    """Inspect one platform-native linked artifact."""

    if profile == "off":
        return []
    if sys.platform == "darwin":
        otool = find_tool("otool")
        nm = find_tool("nm")
        if otool is None or nm is None:
            raise RuntimeError("otool and nm are required on Darwin")
        return audit_macho(
            run_tool([otool, "-hv", str(path)]),
            run_tool([nm, "-u", str(path)]),
        )
    if sys.platform.startswith(("linux", "freebsd", "netbsd", "openbsd")):
        readelf = find_tool("readelf", "llvm-readelf")
        if readelf is None:
            raise RuntimeError("readelf or llvm-readelf is required on ELF")
        return audit_elf(
            run_tool([readelf, "-W", "-l", str(path)]),
            run_tool([readelf, "-W", "-d", str(path)]),
            run_tool([readelf, "-W", "-s", str(path)]),
        )
    if sys.platform == "win32":
        dumpbin = find_tool("dumpbin")
        if dumpbin is not None:
            return audit_pe(
                run_tool([dumpbin, "/nologo", "/headers", str(path)]),
                run_tool([dumpbin, "/nologo", "/imports", str(path)]),
            )
        readobj = find_tool("llvm-readobj")
        if readobj is not None:
            return audit_pe(
                run_tool(
                    [
                        readobj,
                        "--file-headers",
                        "--coff-load-config",
                        str(path),
                    ]
                ),
                run_tool(
                    [
                        readobj,
                        "--coff-imports",
                        "--symbols",
                        str(path),
                    ]
                ),
            )
        raise RuntimeError("dumpbin or llvm-readobj is required on Windows")
    raise RuntimeError(f"unsupported artifact platform: {sys.platform}")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=Path)
    parser.add_argument(
        "--profile",
        choices=("off", "compatible", "strict"),
        required=True,
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    artifact = args.artifact
    if not artifact.is_file() or artifact.is_symlink():
        print(
            f"hardening audit requires a regular artifact: {artifact}",
            file=sys.stderr,
        )
        return 2
    if args.profile == "off":
        print("hardening artifact audit skipped: profile=off")
        return 0
    try:
        missing = audit_artifact(artifact, args.profile)
    except RuntimeError as exc:
        if args.profile == "compatible":
            print(f"hardening artifact audit unavailable: {exc}", file=sys.stderr)
            return 0
        print(f"hardening artifact audit failed: {exc}", file=sys.stderr)
        return 1
    if missing:
        print(
            "hardening artifact audit failed: " + ", ".join(missing),
            file=sys.stderr,
        )
        return 1
    print(f"hardening artifact audit passed: {artifact}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
