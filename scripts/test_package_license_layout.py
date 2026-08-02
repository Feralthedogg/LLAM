#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Licensed under the LLAM Commercial Reciprocity License 1.0.
# See the LICENSE file distributed with this Software.

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parent.parent
ACTIVE_LICENSE_RELATIVE = Path(
    "LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
)
APACHE_LICENSE_RELATIVE = Path("LICENSES/OLD-LICENSE/Apache-2.0.txt")
APACHE_MANIFEST_RELATIVE = Path(
    "LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt"
)
CURRENT_LICENSE_BYTES = b"current license\n"
APACHE_LICENSE_BYTES = b"Apache license\n"
APACHE_MANIFEST_BYTES = b"include/llam/runtime.h\n"


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
            "FreeBSD": "freebsd",
            "OpenBSD": "openbsd",
            "NetBSD": "netbsd",
            "DragonFly": "dragonflybsd",
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

        self._write("LICENSE", CURRENT_LICENSE_BYTES)
        self._write(ACTIVE_LICENSE_RELATIVE, CURRENT_LICENSE_BYTES)
        self._write(APACHE_LICENSE_RELATIVE, APACHE_LICENSE_BYTES)
        self._write(APACHE_MANIFEST_RELATIVE, APACHE_MANIFEST_BYTES)
        self._write("README.md", b"fixture\n")
        self._write("CHANGELOG.md", b"fixture\n")
        self._write("scripts/stress_server.py", b"# fixture\n")
        self._write("scripts/stress_server_composite.py", b"# fixture\n")
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

    def _package_archive(self) -> None:
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
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertTrue(self.archive.is_file())

    def test_archive_contains_mixed_license_metadata(self) -> None:
        self._package_archive()

        with tarfile.open(self.archive, mode="r:xz") as archive:
            names = archive.getnames()
            active_name = (
                f"{self.package}/{ACTIVE_LICENSE_RELATIVE.as_posix()}"
            )
            apache_name = (
                f"{self.package}/{APACHE_LICENSE_RELATIVE.as_posix()}"
            )
            manifest_name = (
                f"{self.package}/{APACHE_MANIFEST_RELATIVE.as_posix()}"
            )
            self.assertIn(active_name, names)
            self.assertIn(apache_name, names)
            self.assertIn(manifest_name, names)
            root_license = archive.extractfile(f"{self.package}/LICENSE")
            active_license = archive.extractfile(active_name)
            apache_license = archive.extractfile(apache_name)
            apache_manifest = archive.extractfile(manifest_name)

            self.assertIsNotNone(root_license)
            self.assertIsNotNone(active_license)
            self.assertIsNotNone(apache_license)
            self.assertIsNotNone(apache_manifest)
            self.assertEqual(CURRENT_LICENSE_BYTES, root_license.read())
            self.assertEqual(CURRENT_LICENSE_BYTES, active_license.read())
            self.assertEqual(APACHE_LICENSE_BYTES, apache_license.read())
            self.assertEqual(APACHE_MANIFEST_BYTES, apache_manifest.read())
            self.assertFalse(any("/OLD-LICENSES/" in name for name in names))

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
            CURRENT_LICENSE_BYTES,
            (prefix / "share/llam/LICENSE").read_bytes(),
        )
        installed_active = prefix / "share/llam" / ACTIVE_LICENSE_RELATIVE
        self.assertTrue(installed_active.is_file())
        self.assertEqual(
            CURRENT_LICENSE_BYTES,
            installed_active.read_bytes(),
        )
        installed_apache = prefix / "share/llam" / APACHE_LICENSE_RELATIVE
        installed_manifest = prefix / "share/llam" / APACHE_MANIFEST_RELATIVE
        self.assertTrue(installed_apache.is_file())
        self.assertTrue(installed_manifest.is_file())
        self.assertEqual(APACHE_LICENSE_BYTES, installed_apache.read_bytes())
        self.assertEqual(APACHE_MANIFEST_BYTES, installed_manifest.read_bytes())
        self.assertFalse((prefix / "share/llam/OLD-LICENSES").exists())


if __name__ == "__main__":
    unittest.main()
