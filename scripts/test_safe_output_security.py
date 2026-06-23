#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Safe-output path traversal regression tests."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import safe_output
from safe_output import open_binary_for_write, write_text_safely


SCRIPT_DIR = Path(__file__).resolve().parent


def fail(message: str, output: str = "") -> None:
    print(f"[test_safe_output_security] {message}", file=sys.stderr)
    if output:
        print(output, end="" if output.endswith("\n") else "\n", file=sys.stderr)
    raise SystemExit(1)


def expect_runtime_error(name: str, expected: str, action) -> None:
    try:
        action()
    except RuntimeError as exc:
        if expected not in str(exc):
            fail(f"{name}: unexpected diagnostic: {exc}")
    else:
        fail(f"{name}: unsafe output path was accepted")


def expect_command_rejects(name: str, args: list[str], expected: str, env: dict[str, str] | None = None) -> None:
    child_env = os.environ.copy()
    if env:
        child_env.update(env)
    proc = subprocess.run(
        args,
        env=child_env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if proc.returncode == 0:
        fail(f"{name}: command unexpectedly succeeded", proc.stdout)
    if expected not in proc.stdout:
        fail(f"{name}: diagnostic did not contain {expected!r}", proc.stdout)


def test_safe_output_paths() -> None:
    with tempfile.TemporaryDirectory(prefix="llam-python-safe-output-") as tmp:
        tmp_path = Path(tmp)
        outside_json = tmp_path / "outside-json"
        result_link = tmp_path / "result-link.json"
        outside_hardlink = tmp_path / "outside-hardlink.json"
        result_hardlink = tmp_path / "result-hardlink.json"
        outside_dir = tmp_path / "outside-dir"
        link_dir = tmp_path / "link-dir"

        outside_json.touch()
        result_link.symlink_to(outside_json)
        outside_hardlink.write_text("outside\n", encoding="utf-8")
        os.link(outside_hardlink, result_hardlink)
        outside_dir.mkdir()
        link_dir.symlink_to(outside_dir, target_is_directory=True)

        expect_runtime_error(
            "symlink leaf",
            "refusing symlink output path",
            lambda: write_text_safely(result_link, "x"),
        )
        if outside_json.stat().st_size != 0:
            fail("safe_output modified a symlink target")

        expect_runtime_error(
            "hardlink leaf",
            "refusing hard-linked output path",
            lambda: write_text_safely(result_hardlink, "x"),
        )
        if outside_hardlink.read_text(encoding="utf-8") != "outside\n":
            fail("safe_output modified a hard-linked target")

        expect_runtime_error(
            "symlink parent",
            "refusing symlink output directory",
            lambda: write_text_safely(link_dir / "result.json", "x"),
        )
        if (outside_dir / "result.json").exists():
            fail("safe_output created a file through a symlink parent")

        expect_runtime_error(
            "deep symlink parent",
            "refusing symlink output directory",
            lambda: write_text_safely(link_dir / "sub" / "result.json", "x"),
        )
        if (outside_dir / "sub" / "result.json").exists():
            fail("safe_output created a deep file through a symlink parent")

        cwd = Path.cwd()
        try:
            os.chdir(tmp_path)
            expect_runtime_error(
                "relative symlink parent",
                "refusing symlink output directory",
                lambda: write_text_safely(Path("link-dir/relative.json"), "x"),
            )
        finally:
            os.chdir(cwd)
        if (outside_dir / "relative.json").exists():
            fail("safe_output created a relative file through a symlink parent")

        expect_runtime_error(
            "binary symlink parent",
            "refusing symlink output directory",
            lambda: open_binary_for_write(link_dir / "graph" / "runtime.png").close(),
        )
        if (outside_dir / "graph" / "runtime.png").exists():
            fail("safe_output binary writer created a file through a symlink parent")

        old_can_use_dir_fd = safe_output._CAN_USE_DIR_FD
        try:
            safe_output._CAN_USE_DIR_FD = False
            expect_runtime_error(
                "fallback symlink parent",
                "refusing symlink output directory",
                lambda: write_text_safely(link_dir / "fallback.json", "x"),
            )
            expect_runtime_error(
                "fallback symlink leaf",
                "refusing symlink output path",
                lambda: write_text_safely(result_link, "x"),
            )
        finally:
            safe_output._CAN_USE_DIR_FD = old_can_use_dir_fd
        if (outside_dir / "fallback.json").exists() or outside_json.stat().st_size != 0:
            fail("safe_output fallback followed an unsafe output path")

        expect_command_rejects(
            "safe_output CLI symlink parent",
            [sys.executable, str(SCRIPT_DIR / "safe_output.py"), "--prepare-dir", str(link_dir / "artifacts")],
            "refusing symlink output directory",
        )
        if (outside_dir / "artifacts").exists():
            fail("safe_output CLI created a directory through a symlink parent")

        expect_command_rejects(
            "run_with_timeout dump symlink parent",
            [
                sys.executable,
                str(SCRIPT_DIR / "run_with_timeout.py"),
                "--timeout",
                "1",
                "--dump-on-timeout",
                str(link_dir / "dump" / "runtime.txt"),
                "--log",
                str(tmp_path / "timeout.log"),
                "--",
                sys.executable,
                "-c",
                "print('ok')",
            ],
            "refusing symlink output directory",
        )
        if (outside_dir / "dump" / "runtime.txt").exists():
            fail("run_with_timeout created a dump through a symlink parent")

        expect_command_rejects(
            "stress_server_composite dump symlink parent",
            [
                sys.executable,
                "-c",
                (
                    "from pathlib import Path; "
                    "import stress_server_composite as s; "
                    "s.start_server(Path('/definitely/missing/llam-server'), '127.0.0.1', 0.01)"
                ),
            ],
            "refusing symlink output directory",
            {
                "PYTHONPATH": str(SCRIPT_DIR),
                "LLAM_SERVER_COMPOSITE_DUMP_DIR": str(link_dir / "composite"),
            },
        )
        if (outside_dir / "composite").exists():
            fail("stress_server_composite created a dump directory through a symlink parent")


def main() -> int:
    test_safe_output_paths()
    print("[test_safe_output_security] ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
