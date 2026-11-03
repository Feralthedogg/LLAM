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

"""Exercise release staging for optional preemption SDK files."""

from __future__ import annotations

import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def target_name() -> str:
    system = platform.system()
    arch = platform.machine()
    systems = {
        "Darwin": "macos",
        "Linux": "linux",
    }
    architectures = {"x86_64": "x86_64", "amd64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}
    if system not in systems:
        raise SystemExit(f"test_preempt_package: unsupported host {system}")
    return f"{systems[system]}-{architectures.get(arch, arch)}"


def copy_file(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def make_fixture(root: Path, *, with_plugin: bool) -> None:
    for source_name in (
        "LICENSE",
        "README.md",
        "CHANGELOG.md",
        "NOTICE",
        "THIRD_PARTY_NOTICES.md",
        "LICENSES/Apache-2.0.txt",
        "third_party/magic2/LICENSE",
        "scripts/package_release.sh",
        "scripts/generate_sdk_metadata.sh",
        "scripts/install.sh",
        "scripts/install.ps1",
        "scripts/stress_server.py",
        "scripts/stress_server_composite.py",
        "scripts/cli_numbers.py",
        "scripts/process_utils.py",
        "scripts/safe_output.py",
        "tools/llam-clang",
        "tools/preempt/metadata.h",
    ):
        copy_file(ROOT / source_name, root / source_name)

    for tree_name in ("docs", "include/llam", "examples"):
        shutil.copytree(ROOT / tree_name, root / tree_name, copy_function=shutil.copy2)

    for executable in ("demo", "stress", "bench", "server", "server_lossless", "server_flood"):
        path = root / executable
        path.write_bytes(f"fixture:{executable}".encode())
        path.chmod(0o755)

    inspector = root / "llam_preempt_metadata_inspect"
    inspector.write_bytes(b"fixture metadata inspector")
    inspector.chmod(0o755)

    static_library = root / "libllam_runtime.a"
    static_library.write_bytes(b"fixture static library")
    if platform.system() == "Darwin":
        shared_library = root / "libllam_runtime.2.dylib"
        shared_library.write_bytes(b"fixture shared library")
        (root / "libllam_runtime.dylib").symlink_to(shared_library.name)
    else:
        shared_library = root / "libllam_runtime.so.3.0.0"
        shared_library.write_bytes(b"fixture shared library")
        (root / "libllam_runtime.so.2").symlink_to(shared_library.name)
        (root / "libllam_runtime.so").symlink_to("libllam_runtime.so.2")

    if with_plugin:
        plugin = root / "build-preempt-tools/llam_preempt_pass.so"
        plugin.parent.mkdir(parents=True, exist_ok=True)
        plugin.write_bytes(b"fixture plugin")


def package(root: Path, target: str, mode: str, version: str) -> Path:
    environment = os.environ.copy()
    environment.update(
        {
            "LLAM_RELEASE_VERSION": version,
            "LLAM_VERSION": "3.0.0",
            "LLAM_ABI_MAJOR": "2",
            "LLAM_PREEMPT_TOOLING": mode,
            "PREEMPT_TOOLING_BUILD_DIR": "build-preempt-tools",
        }
    )
    result = subprocess.run(
        ["sh", str(root / "scripts/package_release.sh"), target],
        cwd=root,
        env=environment,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"{mode} package failed ({result.returncode}):\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return root / "target/dist" / f"llam-{version}-{target}.tar.xz"


def read_package(archive_path: Path, package_root: str) -> dict[str, bytes]:
    with tarfile.open(archive_path, "r:xz") as archive:
        prefix = f"{package_root}/"
        contents: dict[str, bytes] = {}
        for member in archive.getmembers():
            if not member.isfile() or not member.name.startswith(prefix):
                continue
            contents[member.name[len(prefix) :]] = archive.extractfile(member).read()
        return contents


def main() -> int:
    target = target_name()
    linux_x64 = target == "linux-x86_64"
    with tempfile.TemporaryDirectory(prefix="llam-preempt-package-") as temporary:
        root = Path(temporary) / "off-root"
        root.mkdir()
        make_fixture(root, with_plugin=False)
        off_archive = package(root, target, "off", "ci-off")
        off_contents = read_package(off_archive, f"llam-ci-off-{target}")
        config = off_contents["share/llam/preempt/tooling.conf"].decode()
        if "tooling_mode=off" not in config or "llvm_plugin=unavailable" not in config:
            raise AssertionError(f"off-mode package metadata was wrong:\n{config}")
        if ("compiler_driver=bin/llam-clang" in config) != linux_x64:
            raise AssertionError("off-mode package has an unexpected compiler-driver status")
        if "bin/llam_preempt_metadata_inspect" not in off_contents or \
                "share/llam/preempt/metadata.h" not in off_contents:
            raise AssertionError("release package omitted the portable metadata tools")
        if linux_x64 != ("bin/llam-clang" in off_contents):
            raise AssertionError("release package driver contents do not match its target")

        auto_root = Path(temporary) / "auto-root"
        auto_root.mkdir()
        make_fixture(auto_root, with_plugin=linux_x64)
        auto_archive = package(auto_root, target, "auto", "ci-auto")
        auto_contents = read_package(auto_archive, f"llam-ci-auto-{target}")
        auto_config = auto_contents["share/llam/preempt/tooling.conf"].decode()
        expected_plugin = "available" if linux_x64 else "unavailable"
        if f"tooling_mode=auto" not in auto_config or f"llvm_plugin={expected_plugin}" not in auto_config:
            raise AssertionError(f"auto-mode package metadata was wrong:\n{auto_config}")
        if linux_x64 != ("lib/llam/llam_preempt_pass.so" in auto_contents):
            raise AssertionError("auto-mode package plugin contents do not match its target")

        required_root = Path(temporary) / "required-root"
        required_root.mkdir()
        make_fixture(required_root, with_plugin=linux_x64)
        required_env = os.environ.copy()
        required_env.update(
            {
                "LLAM_RELEASE_VERSION": "ci-required",
                "LLAM_VERSION": "3.0.0",
                "LLAM_ABI_MAJOR": "2",
                "LLAM_PREEMPT_TOOLING": "required",
                "PREEMPT_TOOLING_BUILD_DIR": "build-preempt-tools",
            }
        )
        required_result = subprocess.run(
            ["sh", str(required_root / "scripts/package_release.sh"), target],
            cwd=required_root,
            env=required_env,
            text=True,
            capture_output=True,
            check=False,
        )
        if linux_x64:
            if required_result.returncode != 0:
                raise AssertionError(f"required package failed:\n{required_result.stderr}")
            required_archive = required_root / "target/dist" / f"llam-ci-required-{target}.tar.xz"
            required_contents = read_package(required_archive, f"llam-ci-required-{target}")
            required_config = required_contents["share/llam/preempt/tooling.conf"].decode()
            if "tooling_mode=required" not in required_config or "llvm_plugin=available" not in required_config:
                raise AssertionError(f"required package metadata was wrong:\n{required_config}")
        elif required_result.returncode == 0 or "supports linux-x86_64 only" not in required_result.stderr:
            raise AssertionError("required tooling silently packaged an unsupported platform")

    print("test_preempt_package: off/auto release SDK contents passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, AssertionError, tarfile.TarError) as error:
        print(f"test_preempt_package: {error}", file=sys.stderr)
        raise SystemExit(1)
