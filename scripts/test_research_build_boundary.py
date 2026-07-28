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
import time
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
        cls.assert_command_succeeded(research_make, "research Make trace")
        cls.research_make_trace = cls._combined_output(research_make)
        cls.raw_research_off = cls._run(
            [*make_common, "LLAM_BUILD_RESEARCH=0", "test_leir_native_plan"]
        )
        cls.raw_research_object_off = cls._run(
            [
                *make_common,
                "LLAM_BUILD_RESEARCH=0",
                str(cls.work / "make-object/experiments/leir/leir_program.o"),
            ]
        )
        cls.make_package = cls._run(
            [*make_common, "LLAM_BUILD_RESEARCH=1", "package"]
        )
        cls.override_make = cls._run(
            [
                *make_common,
                "LLAM_BUILD_RESEARCH=1",
                "CPPFLAGS=-DUSER_CPPFLAGS",
                "SHARED_CPPFLAGS=-DUSER_SHARED_CPPFLAGS",
                str(cls.work / "make-object/src/core/base/abi.o"),
                str(cls.work / "make-object-pic/src/core/base/abi.o"),
                str(cls.work / "make-object-testhooks/src/core/base/abi.o"),
                str(cls.work / "make-object/experiments/leir/leir_program.o"),
            ]
        )
        cls.assert_command_succeeded(cls.override_make, "Make flag override trace")
        cls.override_make_trace = cls._combined_output(cls.override_make)

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
        shutil.copy2(
            cls.source / "scripts" / "check_release_provenance.py",
            package_script.parent / "check_release_provenance.py",
        )
        package_env = os.environ.copy()
        package_env.pop("LLAM_BUILD_RESEARCH", None)
        artifact_names = [
            "demo",
            "stress",
            "bench",
            "server",
            "server_lossless",
            "server_flood",
            "libllam_runtime.a",
        ]
        if os.uname().sysname == "Darwin":
            artifact_names.append("libllam_runtime.2.dylib")
        else:
            artifact_names.append("libllam_runtime.so.2.2.0")
        for artifact_name in artifact_names:
            mode = "1" if artifact_name == "libllam_runtime.a" else "0"
            (package_source / f"{artifact_name}.llam-build-provenance").write_text(
                f"LLAM_BUILD_RESEARCH={mode}\n", encoding="utf-8"
            )
        cls.package = cls._run(
            ["/bin/sh", str(package_script)], env=package_env
        )
        dist_dir = package_source / "target" / "dist"
        cls.created_archives = list(dist_dir.glob("*")) if dist_dir.exists() else []

        stable_provenance = cls.work / "stable-build-provenance.txt"
        stable_provenance.write_text(
            "LLAM_BUILD_RESEARCH=0\n", encoding="utf-8"
        )
        cls.stable_provenance = cls._run(
            [
                "python3",
                str(cls.source / "scripts/check_release_provenance.py"),
                str(stable_provenance),
            ]
        )

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
        package_artifacts = [
            "demo",
            "stress",
            "bench",
            "server",
            "server_lossless",
            "server_flood",
            "libllam_runtime.a",
        ]
        if os.uname().sysname == "Darwin":
            package_artifacts.append("libllam_runtime.2.dylib")
        else:
            package_artifacts.append("libllam_runtime.so.2.2.0")
        for artifact in package_artifacts:
            with self.subTest(artifact=artifact):
                self.assertIn(
                    f"{artifact}.llam-build-provenance",
                    self.default_make_trace,
                )

    def test_explicit_graph_has_research_targets(self) -> None:
        self.assertIn("test_leir_native_plan", self.research_cmake_targets)
        for family in ("leir", "lcwe", "lccf", "srem"):
            with self.subTest(family=family):
                self.assertIn(
                    f"/experiments/{family}/", self.research_make_trace
                )

    def test_make_flag_overrides_preserve_internal_research_define(self) -> None:
        compile_lines = [
            line
            for line in self.override_make_trace.splitlines()
            if " -c " in line
        ]
        self.assertEqual(len(compile_lines), 4)
        for line in compile_lines:
            with self.subTest(command=line):
                self.assertIn("-DLLAM_BUILD_RESEARCH=1", line)
        self.assertTrue(
            any("-DUSER_CPPFLAGS" in line for line in compile_lines)
        )
        self.assertTrue(
            any("-DUSER_SHARED_CPPFLAGS" in line for line in compile_lines)
        )

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
        self.assert_command_succeeded(
            self.stable_provenance, "stable build provenance check"
        )

    def test_raw_research_target_rejected_when_off(self) -> None:
        self.assertNotEqual(self.raw_research_off.returncode, 0)
        self.assertIn(
            "research targets require LLAM_BUILD_RESEARCH=1",
            self.raw_research_off.stderr,
        )
        self.assertNotEqual(self.raw_research_object_off.returncode, 0)
        self.assertIn(
            "research targets require LLAM_BUILD_RESEARCH=1",
            self.raw_research_object_off.stderr,
        )

    def test_windows_test_objects_rebuild_across_in_place_mode_toggles(
        self,
    ) -> None:
        objdir = self.work / "make-toggle-object"
        targets = [
            str(objdir / "tests/test_windows_iocp_io.o"),
            str(objdir / "tests/test_windows_handle_io.o"),
        ]
        common = [
            "make",
            f"OBJDIR={objdir}",
            f"SHARED_OBJDIR={self.work / 'make-toggle-object-pic'}",
            f"TESTHOOK_OBJDIR={self.work / 'make-toggle-object-testhooks'}",
            *targets,
        ]
        for index, mode in enumerate(("0", "1", "0")):
            if index:
                time.sleep(1.1)
            result = self._run(
                [*common, f"LLAM_BUILD_RESEARCH={mode}"]
            )
            self.assert_command_succeeded(
                result, f"Make in-place mode {mode} compile"
            )
            trace = self._combined_output(result)
            for target in targets:
                with self.subTest(mode=mode, target=target):
                    self.assertIn(f" -c -o {target} ", trace)

    def test_cmake_research_build_provenance_blocks_windows_packaging(
        self,
    ) -> None:
        runtime_build = self._run(
            [
                "cmake",
                "--build",
                str(self.research_cmake_dir),
                "--target",
                "llam_runtime",
                "--parallel",
                "4",
            ]
        )
        self.assert_command_succeeded(
            runtime_build, "research CMake runtime build"
        )
        provenance_matches = list(
            self.research_cmake_dir.rglob(
                "libllam_runtime.a.llam-build-provenance"
            )
        )
        self.assertEqual(len(provenance_matches), 1)
        provenance = provenance_matches[0]
        result = self._run(
            [
                "python3",
                str(self.source / "scripts/check_release_provenance.py"),
                str(provenance),
            ]
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "research-enabled builds cannot be packaged", result.stderr
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
