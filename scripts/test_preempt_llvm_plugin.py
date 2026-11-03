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
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSE.

"""Smoke-test LLAM's opt-in LLVM preemption metadata pass."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


SAFE_FIXTURE = r"""
__attribute__((annotate("llam-preempt-async-safe")))
unsigned long safe_leaf(unsigned long value) {
    return value * 3UL + 1UL;
}
"""

COOPERATIVE_FIXTURE = r"""
extern unsigned long safe_leaf(unsigned long value);

__attribute__((annotate("llam-preempt-cooperative")))
unsigned long cooperative_loop(unsigned long limit) {
    unsigned long total = 0UL;
    for (volatile unsigned long index = 0UL; index < limit; ++index) {
        total += index;
    }
    return total;
}

void llam_task_safepoint(void) {}

int main(void) {
    return (int)(safe_leaf(1UL) + cooperative_loop(3UL) == 7UL ? 0 : 1);
}
"""

NO_ASYNC_FIXTURE = r"""
int main(void) { return 0; }
"""

UNSAFE_FIXTURE = r"""
extern int foreign_call(void);
__attribute__((annotate("llam-preempt-async-safe")))
int not_safe(void) {
    return foreign_call();
}
void llam_task_safepoint(void) {}
int main(void) { return not_safe(); }
"""


def run(
    command: list[str],
    *,
    expect_success: bool,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(command, text=True, capture_output=True, check=False, env=env)
    wanted = 0 if expect_success else None
    if expect_success and result.returncode != wanted:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    if not expect_success and result.returncode == 0:
        raise RuntimeError(
            f"command unexpectedly succeeded: {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--inspector", required=True, type=Path)
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--clang", default=os.environ.get("LLAM_TEST_CLANG", "clang"))
    parser.add_argument("--llvm-major", type=int)
    args = parser.parse_args()

    clang = shutil.which(args.clang)
    objcopy = shutil.which("objcopy")
    if clang is None or objcopy is None:
        print("test_preempt_llvm_plugin: SKIP (clang or objcopy unavailable)", file=sys.stderr)
        return 77
    if args.llvm_major is not None:
        version = subprocess.run(
            [clang, "-dumpversion"], text=True, capture_output=True, check=False
        )
        match = re.match(r"\s*(\d+)", version.stdout)
        if version.returncode != 0 or match is None or int(match.group(1)) != args.llvm_major:
            print(
                "test_preempt_llvm_plugin: SKIP (clang major does not match plugin LLVM)",
                file=sys.stderr,
            )
            return 77
    if not args.plugin.is_file() or not args.inspector.is_file() or not args.driver.is_file():
        print("test_preempt_llvm_plugin: plugin, inspector, or driver is missing", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="llam-preempt-plugin-") as temp_name:
        temp = Path(temp_name)
        safe_source = temp / "safe.c"
        cooperative_source = temp / "cooperative.c"
        executable = temp / "fixture"
        metadata = temp / "metadata.bin"

        safe_source.write_text(SAFE_FIXTURE, encoding="utf-8")
        cooperative_source.write_text(COOPERATIVE_FIXTURE, encoding="utf-8")
        driver_env = os.environ.copy()
        driver_env["LLAM_CLANG"] = clang
        driver_env["LLAM_PREEMPT_PLUGIN"] = str(args.plugin)
        driver_env["LLAM_PREEMPT_METADATA_INSPECT"] = str(args.inspector)
        compile_command = [
            str(args.driver),
            "--llam-preempt=required",
            "-O1",
            str(safe_source),
            str(cooperative_source),
            "-o",
            str(executable),
        ]
        run(compile_command, expect_success=True, env=driver_env)
        run([str(executable)], expect_success=True)
        run(
            [objcopy, "--dump-section", f".llam.preempt={metadata}", str(executable)],
            expect_success=True,
        )
        inspected = run([str(args.inspector), "--required", str(metadata)], expect_success=True)
        if "ranges=1" not in inspected.stdout or "ASYNC_SAFE" not in inspected.stdout:
            raise RuntimeError(f"valid async metadata was not reported:\n{inspected.stdout}")
        if "COOPERATIVE_POLL" not in inspected.stdout:
            raise RuntimeError(f"cooperative poll annotation was not reported:\n{inspected.stdout}")
        if inspected.stdout.count("record ") != 2:
            raise RuntimeError(f"metadata records from separate translation units were lost:\n{inspected.stdout}")

        win_ir = temp / "win-audit.ll"
        win_env = os.environ.copy()
        win_env["LLAM_PREEMPT_REQUIRED"] = "1"
        win_env["LLAM_PREEMPT_WIN64_AUDIT"] = "1"
        run(
            [
                clang,
                "--target=x86_64-w64-windows-gnu",
                "-O1",
                f"-fpass-plugin={args.plugin}",
                "-S",
                "-emit-llvm",
                str(safe_source),
                "-o",
                str(win_ir),
            ],
            expect_success=True,
            env=win_env,
        )
        win_text = win_ir.read_text(encoding="utf-8")
        if ('"llam.preempt.win64_ir_accepted"' not in win_text or
                ".llam.preempt" in win_text or ".llampm" in win_text or
                "__start_" in win_text):
            raise RuntimeError("Win64 IR audit emitted no tag or leaked a certificate")

        win_disabled_env = os.environ.copy()
        win_disabled_env["LLAM_PREEMPT_REQUIRED"] = "1"
        win_disabled_env.pop("LLAM_PREEMPT_WIN64_AUDIT", None)
        win_disabled = run(
            [
                clang,
                "--target=x86_64-w64-windows-gnu",
                "-O1",
                f"-fpass-plugin={args.plugin}",
                "-S",
                "-emit-llvm",
                str(safe_source),
                "-o",
                str(temp / "win-disabled.ll"),
            ],
            expect_success=False,
            env=win_disabled_env,
        )
        if "unsupported target" not in win_disabled.stderr:
            raise RuntimeError("Win64 audit-off mode did not fail closed")

        source = temp / "missing.c"
        source.write_text(NO_ASYNC_FIXTURE, encoding="utf-8")
        missing = run(
            [
                str(args.driver),
                "--llam-preempt=required",
                "-O1",
                str(source),
                "-o",
                str(temp / "missing"),
            ],
            expect_success=False,
            env=driver_env,
        )
        if "no readable .llam.preempt section" not in missing.stderr:
            raise RuntimeError(f"required mode failed for an unexpected reason:\n{missing.stderr}")

        source.write_text(UNSAFE_FIXTURE, encoding="utf-8")
        rejected = run(
            [
                str(args.driver),
                "--llam-preempt=required",
                "-O1",
                str(source),
                "-o",
                str(temp / "unsafe"),
            ],
            expect_success=False,
            env=driver_env,
        )
        if "contains a call or inline assembly" not in rejected.stderr:
            raise RuntimeError(f"unsafe function failed for an unexpected reason:\n{rejected.stderr}")

    print("test_preempt_llvm_plugin: verified metadata emission and required-mode rejection")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"test_preempt_llvm_plugin: {error}", file=sys.stderr)
        raise SystemExit(1)
