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

"""Verify that retired platforms fail before building, downloading or installing."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BSD = ("FreeBSD", "OpenBSD", "NetBSD", "DragonFly")
MESSAGE = "BSD support ended in LLAM 3.0.0"


def retired_distribution_commands(system: str, work: Path) -> list[list[str]]:
    target = ("dragonflybsd" if system == "DragonFly" else system.lower()) + "-x86_64"
    commands = [
        ["make", "-s", "-n", f"UNAME_S={system}", "all"],
        ["sh", "scripts/install.sh", "--dry-run", "--prefix", str(work / "prefix")],
        ["sh", "scripts/package_release.sh", target],
        ["sh", "scripts/generate_sdk_metadata.sh", str(work / "sdk"), target],
    ]
    powershell = shutil.which("pwsh")
    if powershell:
        commands.extend([
            [powershell, "-NoProfile", "-File", "scripts/install.ps1", "-Target", target,
             "-Prefix", str(work / "prefix"), "-DryRun"],
            [powershell, "-NoProfile", "-File", "scripts/package_release_windows.ps1",
             "-Target", target, "-BuildDir", str(work / "sdk")],
        ])
    if shutil.which("cmake"):
        commands.append(["cmake", "-S", str(ROOT), "-B", str(work / system),
                         f"-DCMAKE_SYSTEM_NAME={system}"])
    return commands


class PlatformSupportTests(unittest.TestCase):
    def assert_retired_distribution(self, system: str, work: Path, fake_bin: Path) -> None:
        uname = fake_bin / "uname"
        uname.write_text(f'#!/bin/sh\ncase "$1" in -s) echo {system} ;; *) echo x86_64 ;; esac\n')
        uname.chmod(0o700)
        env = dict(os.environ, PATH=str(fake_bin) + os.pathsep + os.environ["PATH"],
                   PYTHONDONTWRITEBYTECODE="1")
        for command in retired_distribution_commands(system, work):
            result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
            self.assertNotEqual(result.returncode, 0, command)
            self.assertIn(MESSAGE, result.stdout, (command, result.stdout))
        self.assertFalse((work / "prefix").exists())
        self.assertFalse((work / "sdk").exists())

    def test_retired_build_and_distribution_paths(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".llam-retired-platform-", dir=ROOT) as directory:
            work = Path(directory)
            fake_bin = work / "bin"
            fake_bin.mkdir()
            for system in BSD:
                with self.subTest(system=system):
                    self.assert_retired_distribution(system, work, fake_bin)

    def test_retired_public_headers(self) -> None:
        compiler = shutil.which("clang") or shutil.which("cc")
        if compiler is None:
            self.skipTest("C preprocessor unavailable")
        for macro in ("__FreeBSD__", "__OpenBSD__", "__NetBSD__", "__DragonFly__"):
            with self.subTest(macro=macro):
                result = subprocess.run([compiler, "-E", "-x", "c", "-Iinclude", f"-D{macro}=1", "-"],
                                        input='#include "llam/platform.h"\n', cwd=ROOT, text=True,
                                        capture_output=True, timeout=15)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(MESSAGE, result.stderr)


if __name__ == "__main__":
    unittest.main()
