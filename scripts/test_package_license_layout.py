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

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parent.parent
NOTICE_RELATIVE = Path("NOTICE")
APACHE_LICENSE_RELATIVE = Path("LICENSE")
THIRD_PARTY_NOTICES_RELATIVE = Path("THIRD_PARTY_NOTICES.md")
THIRD_PARTY_MAGIC2_LICENSE_RELATIVE = Path("third_party/magic2/LICENSE")
NOTICE_BYTES = b"LLAM attribution\n"
APACHE_LICENSE_BYTES = b"Apache license\n"
THIRD_PARTY_NOTICES_BYTES = b"third-party attributions\n"
THIRD_PARTY_MAGIC2_LICENSE_BYTES = b"MIT license\n"


class PackageLicenseLayoutTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        self.repo = self.root / "repo"
        self.target = self._host_target()
        self.package = f"llam-3.0.0-{self.target}"
        self.archive = self.repo / "target" / "dist" / f"{self.package}.tar.xz"
        self._write_fixture()

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def _host_target(self) -> str:
        system_names = {
            "Darwin": "macos",
            "Linux": "linux",
        }
        machine_names = {
            "amd64": "x86_64",
            "x86_64": "x86_64",
            "arm64": "aarch64",
            "aarch64": "aarch64",
        }
        system = platform.system()
        machine = platform.machine().lower()
        if system not in system_names or machine not in machine_names:
            self.skipTest(f"unsupported package test host: {system}/{machine}")
        return f"{system_names[system]}-{machine_names[machine]}"

    def _write(
        self,
        relative: str | Path,
        content: bytes = b"",
        *,
        executable: bool = False,
    ) -> Path:
        path = self.repo / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        if executable:
            path.chmod(0o755)
        return path

    def _copy_script(self, name: str) -> None:
        source = SOURCE_ROOT / "scripts" / name
        destination = self.repo / "scripts" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o755)

    def _write_fixture(self) -> None:
        self._copy_script("package_release.sh")
        self._copy_script("generate_sdk_metadata.sh")
        self._copy_script("install.sh")
        self._copy_script("install.ps1")

        self._write(NOTICE_RELATIVE, NOTICE_BYTES)
        self._write(APACHE_LICENSE_RELATIVE, APACHE_LICENSE_BYTES)
        self._write(THIRD_PARTY_NOTICES_RELATIVE, THIRD_PARTY_NOTICES_BYTES)
        self._write("third_party/magic2/LICENSE", THIRD_PARTY_MAGIC2_LICENSE_BYTES)
        self._write("README.md", b"fixture\n")
        self._write("CHANGELOG.md", b"fixture\n")
        for name in ("stress_server.py", "stress_server_composite.py", "cli_numbers.py",
                     "process_utils.py", "safe_output.py"):
            self._copy_script(name)
        self._write("docs/fixture.md", b"fixture\n")
        self._write("include/llam/runtime.h", b"/* fixture */\n")
        self._write("examples/smoke.c", b"/* fixture */\n")
        self._write("examples/smoke.h", b"/* fixture */\n")

        for name in (
            "demo",
            "stress",
            "bench",
            "server",
            "server_lossless",
            "server_flood",
        ):
            self._write(name, b"fixture\n", executable=True)
        self._write("libllam_runtime.a", b"fixture\n")

        if platform.system() == "Darwin":
            self._write("libllam_runtime.2.dylib", b"fixture\n")
            (self.repo / "libllam_runtime.dylib").symlink_to(
                "libllam_runtime.2.dylib"
            )
        else:
            self._write("libllam_runtime.so.3.0.0", b"fixture\n")
            (self.repo / "libllam_runtime.so.2").symlink_to(
                "libllam_runtime.so.3.0.0"
            )
            (self.repo / "libllam_runtime.so").symlink_to(
                "libllam_runtime.so.2"
            )

    def _package_archive(self, *, expect_failure: bool = False) -> None:
        env = os.environ.copy()
        env.update(
            {
                "LLAM_RELEASE_VERSION": "3.0.0",
                "LLAM_VERSION": "3.0.0",
                "LLAM_ABI_MAJOR": "2",
            }
        )
        result = subprocess.run(
            ["sh", str(self.repo / "scripts/package_release.sh"), self.target],
            cwd=self.repo,
            env=env,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if expect_failure:
            self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
            self.assertFalse(self.archive.exists())
            return
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertTrue(self.archive.is_file())

    def test_missing_notices_fail_before_packaging(self) -> None:
        for relative in (NOTICE_RELATIVE, THIRD_PARTY_NOTICES_RELATIVE, APACHE_LICENSE_RELATIVE):
            with self.subTest(path=relative):
                path = self.repo / relative
                original = path.read_bytes()
                path.unlink()
                self._package_archive(expect_failure=True)
                path.write_bytes(original)

    def test_packaged_stress_help_outside_repository(self) -> None:
        self._package_archive()
        extracted = self.root / "extracted"
        extracted.mkdir()
        subprocess.run(["tar", "-xJf", str(self.archive), "-C", str(extracted)], check=True)
        for name in ("stress_server.py", "stress_server_composite.py"):
            with self.subTest(script=name):
                result = subprocess.run(
                    [sys.executable, "-E", str(extracted / self.package / "scripts" / name), "--help"],
                    cwd=self.root, text=True, capture_output=True, check=False, timeout=10,
                )
                self.assertEqual(0, result.returncode, result.stdout + result.stderr)
                self.assertIn("usage:", result.stdout)

    def test_archive_contains_apache_and_third_party_metadata(self) -> None:
        self._package_archive()

        with tarfile.open(self.archive, mode="r:xz") as archive:
            names = archive.getnames()
            notice_name = (
                f"{self.package}/{NOTICE_RELATIVE.as_posix()}"
            )
            apache_name = (
                f"{self.package}/{APACHE_LICENSE_RELATIVE.as_posix()}"
            )
            attributions_name = (
                f"{self.package}/{THIRD_PARTY_NOTICES_RELATIVE.as_posix()}"
            )
            third_party_name = (
                f"{self.package}/{THIRD_PARTY_MAGIC2_LICENSE_RELATIVE.as_posix()}"
            )
            self.assertIn(notice_name, names)
            self.assertIn(apache_name, names)
            self.assertIn(attributions_name, names)
            self.assertIn(third_party_name, names)
            notice = archive.extractfile(notice_name)
            apache_license = archive.extractfile(apache_name)
            attributions = archive.extractfile(attributions_name)
            third_party_license = archive.extractfile(third_party_name)

            self.assertIsNotNone(notice)
            self.assertIsNotNone(apache_license)
            self.assertIsNotNone(attributions)
            self.assertIsNotNone(third_party_license)
            self.assertEqual(NOTICE_BYTES, notice.read())
            self.assertEqual(APACHE_LICENSE_BYTES, apache_license.read())
            self.assertEqual(THIRD_PARTY_NOTICES_BYTES, attributions.read())
            self.assertEqual(THIRD_PARTY_MAGIC2_LICENSE_BYTES, third_party_license.read())
            self.assertFalse(any("/OLD-LICENSES/" in name for name in names))
            self.assertFalse(any("/LICENSES/" in name or "LicenseRef-LLAM" in name for name in names))

    def test_archive_installer_preserves_active_license_metadata(self) -> None:
        self._package_archive()
        extracted = self.root / "extracted"
        prefix = self.root / "prefix"
        extracted.mkdir()
        subprocess.run(
            ["tar", "-xJf", str(self.archive), "-C", str(extracted)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        package_root = extracted / self.package

        result = subprocess.run(
            [
                "sh",
                str(package_root / "install.sh"),
                "--prefix",
                str(prefix),
                "--force",
            ],
            cwd=package_root,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(
            APACHE_LICENSE_BYTES,
            (prefix / "share/llam/LICENSE").read_bytes(),
        )
        installed_notice = prefix / "share/llam" / NOTICE_RELATIVE
        self.assertTrue(installed_notice.is_file())
        self.assertEqual(
            NOTICE_BYTES,
            installed_notice.read_bytes(),
        )
        installed_apache = prefix / "share/llam" / APACHE_LICENSE_RELATIVE
        installed_attributions = prefix / "share/llam" / THIRD_PARTY_NOTICES_RELATIVE
        self.assertTrue(installed_apache.is_file())
        self.assertTrue(installed_attributions.is_file())
        self.assertEqual(APACHE_LICENSE_BYTES, installed_apache.read_bytes())
        self.assertEqual(THIRD_PARTY_NOTICES_BYTES, installed_attributions.read_bytes())
        self.assertEqual(THIRD_PARTY_MAGIC2_LICENSE_BYTES,
                         (prefix / "share/llam" / THIRD_PARTY_MAGIC2_LICENSE_RELATIVE).read_bytes())
        self.assertFalse((prefix / "share/llam/OLD-LICENSES").exists())
        self.assertFalse((prefix / "share/llam/LICENSES").exists())


if __name__ == "__main__":
    unittest.main()
