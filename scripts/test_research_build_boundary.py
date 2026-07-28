#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Behavioral checks for the private research build boundary."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ResearchBoundaryTests(unittest.TestCase):
    source = Path(__file__).resolve().parents[1]
    requested_work: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        for command in ("make", "cmake"):
            if shutil.which(command) is None:
                raise unittest.SkipTest(f"{command} is not available")

        cls._temporary_work: tempfile.TemporaryDirectory[str] | None = None
        if cls.requested_work is None:
            cls._temporary_work = tempfile.TemporaryDirectory(
                prefix="llam-research-boundary-"
            )
            cls.work = Path(cls._temporary_work.name)
        else:
            cls.work = cls.requested_work.resolve()
            cls.work.mkdir(parents=True, exist_ok=True)

        make_common = [
            "make",
            "-n",
            "-B",
            f"OBJDIR={cls.work / 'make-object'}",
            f"SHARED_OBJDIR={cls.work / 'make-object-pic'}",
            f"TESTHOOK_OBJDIR={cls.work / 'make-object-testhooks'}",
            "MAKE=:",
        ]
        default_make = cls._run([*make_common, "all", "test"])
        cls.assert_command_succeeded(default_make, "default Make trace")
        cls.default_make_trace = cls._combined_output(default_make)

        research_make = cls._run(
            [*make_common, "LLAM_BUILD_RESEARCH=1", "research"]
        )
        cls.research_make_trace = cls._combined_output(research_make)
        cls.raw_research_off = cls._run(
            [*make_common, "LLAM_BUILD_RESEARCH=0", "test_leir_native_plan"]
        )
        cls.make_package = cls._run(
            [*make_common, "LLAM_BUILD_RESEARCH=1", "package"]
        )

        cls.default_cmake_dir = cls.work / "cmake-off"
        default_configure = cls._run(
            [
                "cmake",
                "-S",
                str(cls.source),
                "-B",
                str(cls.default_cmake_dir),
                "-DLLAM_BUILD_RESEARCH=OFF",
            ]
        )
        cls.assert_command_succeeded(default_configure, "default CMake configure")
        default_targets = cls._run(
            [
                "cmake",
                "--build",
                str(cls.default_cmake_dir),
                "--target",
                "help",
            ]
        )
        cls.assert_command_succeeded(default_targets, "default CMake target query")
        cls.default_cmake_targets = cls._combined_output(default_targets)

        cls.research_cmake_dir = cls.work / "cmake-on"
        research_configure = cls._run(
            [
                "cmake",
                "-S",
                str(cls.source),
                "-B",
                str(cls.research_cmake_dir),
                "-DLLAM_BUILD_RESEARCH=ON",
            ]
        )
        cls.assert_command_succeeded(research_configure, "research CMake configure")
        research_targets = cls._run(
            [
                "cmake",
                "--build",
                str(cls.research_cmake_dir),
                "--target",
                "help",
            ]
        )
        cls.assert_command_succeeded(research_targets, "research CMake target query")
        cls.research_cmake_targets = cls._combined_output(research_targets)

        package_source = cls.work / "package-source"
        package_script = package_source / "scripts" / "package_release.sh"
        package_script.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(cls.source / "scripts" / "package_release.sh", package_script)
        package_env = os.environ.copy()
        package_env["LLAM_BUILD_RESEARCH"] = "1"
        cls.package = cls._run(
            ["/bin/sh", str(package_script)], env=package_env
        )
        dist_dir = package_source / "target" / "dist"
        cls.created_archives = list(dist_dir.glob("*")) if dist_dir.exists() else []

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._temporary_work is not None:
            cls._temporary_work.cleanup()

    @classmethod
    def _run(
        cls, command: list[str], *, env: dict[str, str] | None = None
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            command,
            cwd=cls.source,
            check=False,
            text=True,
            capture_output=True,
            env=env,
        )

    @staticmethod
    def _combined_output(result: subprocess.CompletedProcess[str]) -> str:
        return (result.stdout + result.stderr).replace("\\", "/")

    @staticmethod
    def assert_command_succeeded(
        result: subprocess.CompletedProcess[str], label: str
    ) -> None:
        if result.returncode != 0:
            raise AssertionError(
                f"{label} failed with {result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )

    def test_default_graph_has_no_experiment_objects(self) -> None:
        self.assertNotIn("/experiments/", self.default_make_trace)
        self.assertNotIn("test_leir_", self.default_cmake_targets)

    def test_explicit_graph_has_research_targets(self) -> None:
        self.assertIn("test_leir_native_plan", self.research_cmake_targets)
        self.assertIn("/experiments/leir/", self.research_make_trace)

    def test_package_rejects_research_mode(self) -> None:
        self.assertNotEqual(self.make_package.returncode, 0)
        self.assertIn(
            "research-enabled builds cannot be packaged",
            self.make_package.stderr,
        )
        self.assertNotEqual(self.package.returncode, 0)
        self.assertIn(
            "research-enabled builds cannot be packaged", self.package.stderr
        )
        self.assertEqual(self.created_archives, [])

    def test_raw_research_target_rejected_when_off(self) -> None:
        self.assertNotEqual(self.raw_research_off.returncode, 0)
        self.assertIn(
            "research targets require LLAM_BUILD_RESEARCH=1",
            self.raw_research_off.stderr,
        )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path, default=Path(__file__).resolve().parents[1]
    )
    parser.add_argument("--work", type=Path)
    parser.add_argument("-v", "--verbose", action="store_true")
    return parser.parse_args()


if __name__ == "__main__":
    args = _parse_args()
    ResearchBoundaryTests.source = args.source.resolve()
    ResearchBoundaryTests.requested_work = args.work
    unittest.main(
        argv=[__file__, "-v"] if args.verbose else [__file__],
        exit=True,
    )
