#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Regression tests for llam_broker command-line parsing."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def fail(message: str, proc: subprocess.CompletedProcess[str] | None = None) -> None:
    print(f"[test_broker_cli_parsing] {message}", file=sys.stderr)
    if proc is not None:
        if proc.stdout:
            print(proc.stdout, file=sys.stderr, end="")
        if proc.stderr:
            print(proc.stderr, file=sys.stderr, end="")
    raise SystemExit(1)


def assert_invalid_count(value: str) -> None:
    path = Path(tempfile.gettempdir()) / f"llam-broker-invalid-{os.getpid()}.sock"
    try:
        proc = subprocess.run(
            [str(ROOT / "llam_broker"), "--serve-n", str(path), value],
            cwd=ROOT,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=1.0,
            check=False,
        )
    except subprocess.TimeoutExpired:
        fail(f"accepted invalid --serve-n count {value!r} and kept serving")
    finally:
        try:
            path.unlink()
        except FileNotFoundError:
            pass
    if proc.returncode != 2:
        fail(f"invalid --serve-n count {value!r} returned {proc.returncode}, want 2", proc)


def main() -> int:
    for value in ("-1", "+1", " -1", " +1", "0", "bad"):
        assert_invalid_count(value)
    print("[test_broker_cli_parsing] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
