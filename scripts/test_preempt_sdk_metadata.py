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

"""Check generated SDK metadata for optional and required LLVM tooling."""

from __future__ import annotations

import os
import shutil
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / "scripts" / "generate_sdk_metadata.sh"


def generate(stage: Path, target: str, mode: str, *, success: bool) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["LLAM_PREEMPT_TOOLING"] = mode
    env["LLAM_VERSION"] = "3.0.0"
    env["LLAM_ABI_MAJOR"] = "2"
    result = subprocess.run(
        ["sh", str(GENERATOR), str(stage), target],
        text=True,
        capture_output=True,
        env=env,
        check=False,
    )
    if success and result.returncode != 0:
        raise AssertionError(f"SDK metadata generation failed: {result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError("invalid or unmet required tooling mode unexpectedly succeeded")
    return result


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="llam-preempt-sdk-") as temporary:
        root = Path(temporary)
        off_stage = root / "off"
        generate(off_stage, "linux-x86_64", "off", success=True)
        off_config = (off_stage / "share/llam/preempt/tooling.conf").read_text(encoding="utf-8")
        for expected in (
            "format_version=1",
            "section=.llam.preempt",
            "tooling_mode=off",
            "llvm_plugin=unavailable",
            "compiler_driver=unavailable",
            "language_bindings=excluded",
        ):
            if expected not in off_config:
                raise AssertionError(f"off-mode SDK metadata is missing {expected!r}")

        auto_stage = root / "auto"
        plugin = auto_stage / "lib/llam/llam_preempt_pass.so"
        plugin.parent.mkdir(parents=True)
        plugin.write_bytes(b"test plugin placeholder")
        generate(auto_stage, "linux-x86_64", "auto", success=True)
        auto_config = (auto_stage / "share/llam/preempt/tooling.conf").read_text(encoding="utf-8")
        if ("tooling_mode=auto" not in auto_config or
                "llvm_plugin=available" not in auto_config or
                "compiler_driver=unavailable" not in auto_config):
            raise AssertionError("auto-mode SDK metadata did not report the staged plugin")

        required_missing_stage = root / "required-missing"
        missing = generate(required_missing_stage, "linux-x86_64", "required", success=False)
        if "plugin is absent" not in missing.stderr:
            raise AssertionError(f"required mode failed for an unexpected reason: {missing.stderr}")

        required_stage = root / "required"
        required_plugin = required_stage / "lib/llam/llam_preempt_pass.so"
        required_plugin.parent.mkdir(parents=True)
        required_plugin.write_bytes(b"test plugin placeholder")
        required_bin = required_stage / "bin"
        required_bin.mkdir()
        for tool_name in ("llam-clang", "llam_preempt_metadata_inspect"):
            tool = required_bin / tool_name
            tool.write_bytes(b"#!/bin/sh\nexit 0\n")
            tool.chmod(0o755)
        generate(required_stage, "linux-x86_64", "required", success=True)
        required_config = (required_stage / "share/llam/preempt/tooling.conf").read_text(encoding="utf-8")
        if "compiler_driver=bin/llam-clang" not in required_config:
            raise AssertionError("required-mode SDK metadata did not expose its driver")

        unsupported = generate(root / "unsupported", "macos-aarch64", "required", success=False)
        if "supports linux-x86_64 only" not in unsupported.stderr:
            raise AssertionError(f"required mode accepted an unsupported SDK target: {unsupported.stderr}")

        invalid = generate(root / "invalid", "linux-x86_64", "sometimes", success=False)
        if "must be off, auto, or required" not in invalid.stderr:
            raise AssertionError(f"invalid tooling mode failed for an unexpected reason: {invalid.stderr}")

        pkg_config = (off_stage / "lib/pkgconfig/llam.pc").read_text(encoding="utf-8")
        cmake_config = (off_stage / "share/llam/cmake/llam-config.cmake").read_text(encoding="utf-8")
        if "-luring" not in pkg_config or "llam-targets.cmake" not in cmake_config:
            raise AssertionError("existing C and CMake SDK metadata was not retained")

        cmake = shutil.which("cmake")
        if cmake:
            (off_stage / "include").mkdir()
            (off_stage / "lib/libllam_runtime.a").touch()
            consumer = root / "consumer"
            consumer.mkdir()
            for index, (request, accepted) in enumerate((
                ("3", True), ("3.0.0 EXACT", True), ("2", False),
                ("4", False), ("3.0.1", False),
            )):
                (consumer / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.20)\nproject(sdk_consumer C)\n"
                    f'find_package(llam {request} CONFIG REQUIRED PATHS "{off_stage.as_posix()}" NO_DEFAULT_PATH)\n',
                    encoding="utf-8",
                )
                result = subprocess.run(
                    [cmake, "-S", str(consumer), "-B", str(root / f"consumer-build-{index}"),
                     f"-DCMAKE_PREFIX_PATH={off_stage}"],
                    text=True, capture_output=True, check=False, timeout=60,
                )
                if (result.returncode == 0) != accepted:
                    raise AssertionError(f"SDK version request {request!r}: {result.stdout}\n{result.stderr}")
        else:
            print("SDK CMake version checks skipped: cmake unavailable")

    print("test_preempt_sdk_metadata: off/auto/required package metadata passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
