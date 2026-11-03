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
# See LICENSES/Apache-2.0.txt.

"""Install and inspect the standalone CMake preemption-tool component."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--mode", required=True, choices=("off", "auto", "required"))
    parser.add_argument("--expect-plugin", action="store_true")
    parser.add_argument("--expect-driver", action="store_true")
    parser.add_argument("--clang")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="llam-preempt-cmake-install-") as temporary:
        prefix = Path(temporary) / "prefix"
        installed = subprocess.run(
            [
                args.cmake,
                "--install",
                str(args.build_dir),
                "--prefix",
                str(prefix),
                "--component",
                "PreemptTools",
            ],
            text=True,
            capture_output=True,
            check=False,
        )
        if installed.returncode != 0:
            raise SystemExit(
                "CMake PreemptTools component install failed:\n"
                f"stdout:\n{installed.stdout}\nstderr:\n{installed.stderr}"
            )

        inspectors = list(prefix.rglob("llam_preempt_metadata_inspect*"))
        headers = list(prefix.rglob("metadata.h"))
        configs = list(prefix.rglob("tooling.conf"))
        drivers = list(prefix.rglob("llam-clang"))
        plugins = list(prefix.rglob("llam_preempt_pass.so"))
        if len(inspectors) != 1 or not inspectors[0].is_file():
            raise SystemExit("CMake install did not include exactly one metadata inspector")
        if len(headers) != 1 or not headers[0].is_file():
            raise SystemExit("CMake install did not include exactly one metadata header")
        if len(configs) != 1:
            raise SystemExit("CMake install did not include exactly one tooling.conf")

        config = configs[0].read_text(encoding="utf-8")
        expected_plugin = "available" if args.expect_plugin else "unavailable"
        expected_driver = "bin/llam-clang" if args.expect_driver else "unavailable"
        expected_fields = (
            f"tooling_mode={args.mode}",
            f"llvm_plugin={expected_plugin}",
            f"compiler_driver={expected_driver}",
            "language_bindings=excluded",
        )
        if not all(field in config for field in expected_fields):
            raise SystemExit(f"installed tooling.conf mismatch:\n{config}")
        if args.expect_driver != (len(drivers) == 1):
            raise SystemExit("CMake install's driver file disagrees with tooling.conf")
        if args.expect_plugin != (len(plugins) == 1):
            raise SystemExit("CMake install's LLVM plugin disagrees with tooling.conf")
        if args.expect_plugin and args.expect_driver and args.clang:
            installed_driver = drivers[0]
            installed_inspector = inspectors[0]
            smoke = subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).with_name("test_preempt_installed_driver.py")),
                    "--driver",
                    str(installed_driver),
                    "--inspector",
                    str(installed_inspector),
                    "--clang",
                    args.clang,
                ],
                text=True,
                capture_output=True,
                check=False,
            )
            if smoke.returncode != 0:
                raise SystemExit(
                    "CMake installed driver smoke failed:\n"
                    f"stdout:\n{smoke.stdout}\nstderr:\n{smoke.stderr}"
                )

    print("test_preempt_cmake_install: PreemptTools component contents passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
