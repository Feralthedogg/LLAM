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

"""Verify an installed ``llam-clang`` discovers its sibling LLVM plugin."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


SOURCE = """
__attribute__((annotate(\"llam-preempt-async-safe\")))
unsigned long installed_driver_leaf(unsigned long value) { return value + 9UL; }
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True, type=Path)
    parser.add_argument("--inspector", required=True, type=Path)
    parser.add_argument("--clang", default="clang")
    args = parser.parse_args()

    clang = shutil.which(args.clang)
    objcopy = shutil.which("objcopy")
    if clang is None or objcopy is None:
        print("test_preempt_installed_driver: SKIP (clang or objcopy unavailable)", file=sys.stderr)
        return 77
    if not args.driver.is_file() or not args.inspector.is_file():
        raise SystemExit("installed driver or inspector is missing")

    environment = os.environ.copy()
    environment["LLAM_CLANG"] = clang
    environment.pop("LLAM_PREEMPT_PLUGIN", None)
    environment.pop("LLAM_PREEMPT_METADATA_INSPECT", None)
    with tempfile.TemporaryDirectory(prefix="llam-preempt-installed-") as temporary:
        temp = Path(temporary)
        source = temp / "leaf.c"
        object_file = temp / "leaf.o"
        metadata = temp / "metadata.bin"
        source.write_text(SOURCE, encoding="utf-8")
        compile_result = subprocess.run(
            [str(args.driver), "--llam-preempt=auto", "-O1", "-c", str(source), "-o", str(object_file)],
            env=environment,
            text=True,
            capture_output=True,
            check=False,
        )
        if compile_result.returncode != 0:
            raise SystemExit(
                f"installed driver failed to discover its plugin:\n{compile_result.stderr}"
            )
        dump_result = subprocess.run(
            [objcopy, "--dump-section", f".llam.preempt={metadata}", str(object_file)],
            text=True,
            capture_output=True,
            check=False,
        )
        if dump_result.returncode != 0 or not metadata.is_file() or metadata.stat().st_size == 0:
            raise SystemExit("installed driver produced no .llam.preempt section")
        inspected = subprocess.run(
            [str(args.inspector), str(metadata)],
            text=True,
            capture_output=True,
            check=False,
        )
        if inspected.returncode != 0 or "ASYNC_SAFE" not in inspected.stdout:
            raise SystemExit(
                "installed metadata inspector rejected the generated section:\n"
                f"stdout:\n{inspected.stdout}\nstderr:\n{inspected.stderr}"
            )

    print("test_preempt_installed_driver: plugin discovery and metadata emission passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        print(f"test_preempt_installed_driver: {error}", file=sys.stderr)
        raise SystemExit(1)
