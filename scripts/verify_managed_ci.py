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

"""Build and require the managed compiler-to-installed-SDK CTest chain."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

REQUIRED_TESTS = (
    "test_managed_compiler",
    "test_managed_runtime_contracts",
    "test_managed_image_internal",
    "test_managed_sdk",
    "test_preempt_regions_native",
    "test_local_executor_native",
)
BUILD_TARGETS = (
    "llam_managed_pass", "llam_runtime", "llam_runtime_shared",
    "llam_preempt_metadata_inspect", "test_managed_image_internal",
    "test_execution_controls",
    "demo", "stress", "bench", "server", "server_lossless", "server_flood",
)


def require_registered_tests(inventory: dict) -> None:
    counts = Counter(item["name"] for item in inventory.get("tests", []))
    invalid = [name for name in REQUIRED_TESTS if counts[name] != 1]
    if invalid:
        raise ValueError("Required managed CTests missing or duplicated: " + ", ".join(invalid))


def require_completed_tests(report: Path) -> None:
    cases = list(ET.parse(report).getroot().iter("testcase"))
    counts = Counter(item.get("name") for item in cases)
    invalid = [name for name in REQUIRED_TESTS if counts[name] != 1]
    for case in cases:
        if case.get("name") not in REQUIRED_TESTS:
            continue
        if (case.find("skipped") is not None or case.find("failure") is not None or
                case.find("error") is not None or case.get("status") in {"notrun", "disabled"}):
            invalid.append(case.get("name", "unnamed"))
    if invalid:
        raise ValueError("Required managed CTests did not pass: " + ", ".join(sorted(set(invalid))))


def run_logged(command: list[str], output: Path) -> int:
    print("Running:", " ".join(command), flush=True)
    with output.open("w", encoding="utf-8") as log:
        process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=False)
    if process.returncode:
        print(output.read_text(encoding="utf-8"), file=sys.stderr)
    return process.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--report-dir", required=True, type=Path)
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("parallel must be positive")
    build = args.build_dir.resolve(strict=True)
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    if "LLAM_MANAGED_TOOLING:STRING=required" not in cache.splitlines():
        parser.error("build must be configured with -DLLAM_MANAGED_TOOLING=required")
    args.report_dir.mkdir(parents=True, exist_ok=True)
    inventory_run = subprocess.run([args.ctest, "--test-dir", str(build), "--show-only=json-v1"],
                                   capture_output=True, text=True, check=True)
    inventory = json.loads(inventory_run.stdout)
    (args.report_dir / "ctest-inventory.json").write_text(inventory_run.stdout, encoding="utf-8")
    require_registered_tests(inventory)
    build_command = [args.cmake, "--build", str(build), "--parallel", str(args.parallel),
                     "--target", *BUILD_TARGETS]
    if run_logged(build_command, args.report_dir / "build.log"):
        return 1
    junit = args.report_dir.resolve() / "managed-ctest.xml"
    command = [args.ctest, "--test-dir", str(build), "--output-on-failure", "--no-tests=error",
               "--output-junit", str(junit), "-R", "^(" + "|".join(REQUIRED_TESTS) + ")$"]
    result = run_logged(command, args.report_dir / "ctest.log")
    require_completed_tests(junit)
    if result:
        return result
    print("Managed CI: compiler, runtime contracts, native execution controls and relocated SDK passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
