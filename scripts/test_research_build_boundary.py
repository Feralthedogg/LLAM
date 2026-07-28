#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Behavioral checks for the private research build boundary."""

from __future__ import annotations

import argparse
import hashlib
import json
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
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
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
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
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

    @classmethod
    def _internal_probe_flags(cls, mode: int) -> list[str]:
        flags = [
            "-std=c11",
            "-Iinclude",
            "-Isrc/internal",
            "-Isrc",
            "-D_GNU_SOURCE",
            f"-DLLAM_BUILD_RESEARCH={mode}",
        ]
        if os.uname().sysname == "Darwin":
            flags.extend(["-D_XOPEN_SOURCE=700", "-D_DARWIN_C_SOURCE"])
        return flags

    @classmethod
    def _runtime_type_size(cls, mode: int) -> int:
        probe = cls.work / f"runtime-layout-{mode}.c"
        binary = cls.work / f"runtime-layout-{mode}"
        probe.write_text(
            '#include "runtime_internal.h"\n'
            "#include <stdio.h>\n"
            "int main(void) {\n"
            '    printf("%zu\\n", sizeof(llam_io_req_t));\n'
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        compile_result = cls._run(
            [
                os.environ.get("CC", "cc"),
                *cls._internal_probe_flags(mode),
                str(probe),
                "-o",
                str(binary),
            ]
        )
        cls.assert_command_succeeded(
            compile_result, f"research mode {mode} layout probe compile"
        )
        run_result = cls._run([str(binary)])
        cls.assert_command_succeeded(
            run_result, f"research mode {mode} layout probe run"
        )
        return int(run_result.stdout.strip())

    @classmethod
    def _preprocessed_runtime_state(cls, mode: int) -> str:
        probe = cls.work / f"runtime-state-{mode}.c"
        probe.write_text(
            '#include "runtime_state.h"\n',
            encoding="utf-8",
        )
        result = cls._run(
            [
                os.environ.get("CC", "cc"),
                *cls._internal_probe_flags(mode),
                "-E",
                "-P",
                str(probe),
            ]
        )
        cls.assert_command_succeeded(
            result, f"research mode {mode} runtime state preprocess"
        )
        return cls._combined_output(result)

    @classmethod
    def _build_cmake_mode(cls, build_dir: Path, mode: str) -> None:
        result = cls._run(
            [
                "cmake",
                "--build",
                str(build_dir),
                "--parallel",
                "4",
            ]
        )
        cls.assert_command_succeeded(result, f"{mode} CMake build")

    @classmethod
    def _find_static_library(cls, build_dir: Path) -> Path:
        matches = [
            path
            for path in build_dir.rglob("libllam_runtime.a")
            if path.is_file() and path.name == "libllam_runtime.a"
        ]
        if len(matches) != 1:
            raise AssertionError(
                f"expected one static runtime in {build_dir}, got {matches}"
            )
        return matches[0]

    @classmethod
    def _find_shared_library(cls, build_dir: Path) -> Path:
        if os.uname().sysname == "Darwin":
            pattern = "*llam_runtime*.dylib"
        else:
            pattern = "*llam_runtime*.so*"
        matches = [
            path
            for path in build_dir.rglob(pattern)
            if path.is_file() and not path.is_symlink()
        ]
        if len(matches) != 1:
            raise AssertionError(
                f"expected one shared runtime in {build_dir}, got {matches}"
            )
        return matches[0]

    @classmethod
    def _defined_dynamic_exports(cls, library: Path) -> set[str]:
        if os.uname().sysname == "Darwin":
            command = ["nm", "-gU", str(library)]
        else:
            command = ["nm", "-D", "--defined-only", str(library)]
        result = cls._run(command)
        cls.assert_command_succeeded(
            result, f"dynamic export inspection for {library}"
        )
        exports = set()
        for line in result.stdout.splitlines():
            fields = line.split()
            if not fields:
                continue
            symbol = fields[-1]
            if os.uname().sysname == "Darwin" and symbol.startswith("_"):
                symbol = symbol[1:]
            if symbol.startswith("llam_"):
                exports.add(symbol)
        return exports

    @classmethod
    def _installed_public_header_hashes(
        cls, build_dir: Path, mode: str
    ) -> dict[str, str]:
        prefix = cls.work / f"install-{mode}"
        result = cls._run(
            [
                "cmake",
                "--install",
                str(build_dir),
                "--prefix",
                str(prefix),
            ]
        )
        cls.assert_command_succeeded(result, f"{mode} CMake install")
        include_dir = prefix / "include" / "llam"
        return {
            str(path.relative_to(include_dir)): hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
            for path in sorted(include_dir.rglob("*"))
            if path.is_file()
        }

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

    def test_native_internals_absent_when_off(self) -> None:
        linux_make_common = [
            "make",
            "-n",
            "-B",
            "HOST_PLATFORM=linux",
            f"OBJDIR={self.work / 'simulated-linux-object'}",
            f"SHARED_OBJDIR={self.work / 'simulated-linux-object-pic'}",
            f"TESTHOOK_OBJDIR={self.work / 'simulated-linux-object-testhooks'}",
            "MAKE=:",
            "libllam_runtime.a",
        ]
        off_make = self._run(
            [*linux_make_common, "LLAM_BUILD_RESEARCH=0"]
        )
        on_make = self._run(
            [*linux_make_common, "LLAM_BUILD_RESEARCH=1"]
        )
        self.assert_command_succeeded(
            off_make, "simulated Linux research-off Make graph"
        )
        self.assert_command_succeeded(
            on_make, "simulated Linux research-on Make graph"
        )
        off_compile_commands = self._combined_output(off_make)
        on_compile_commands = self._combined_output(on_make)
        for source in (
            "linux_segment.c",
            "linux_segment_cancel.c",
            "linux_segment_resources.c",
        ):
            with self.subTest(source=source):
                self.assertNotIn(source, off_compile_commands)
                self.assertIn(source, on_compile_commands)

        off_cmake_commands = json.loads(
            (self.default_cmake_dir / "compile_commands.json").read_text(
                encoding="utf-8"
            )
        )
        on_cmake_commands = json.loads(
            (self.research_cmake_dir / "compile_commands.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertTrue(
            all(
                "linux_segment.c" not in entry["file"].replace("\\", "/")
                for entry in off_cmake_commands
            )
        )
        for mode, commands, suffixes in (
            (
                0,
                off_cmake_commands,
                (
                    "/src/core/base/abi.c",
                    "/tests/test_runtime_core.c",
                    "/tests/test_runtime_shutdown_internal.c",
                    "/examples/stress.c",
                ),
            ),
            (
                1,
                on_cmake_commands,
                (
                    "/src/core/base/abi.c",
                    "/tests/test_runtime_core.c",
                    "/tests/test_runtime_shutdown_internal.c",
                    "/examples/stress.c",
                    "/experiments/leir/leir_engine.c",
                ),
            ),
        ):
            for suffix in suffixes:
                matches = [
                    entry
                    for entry in commands
                    if entry["file"].replace("\\", "/").endswith(suffix)
                ]
                self.assertTrue(matches, f"missing CMake command for {suffix}")
                for entry in matches:
                    with self.subTest(
                        mode=mode,
                        source=suffix,
                        command=entry["command"],
                    ):
                        self.assertIn(
                            f"-DLLAM_BUILD_RESEARCH={mode}",
                            entry["command"],
                        )

        off_runtime_type_size = self._runtime_type_size(0)
        on_runtime_type_size = self._runtime_type_size(1)
        self.assertGreater(on_runtime_type_size, off_runtime_type_size)

        off_preprocessed_state = self._preprocessed_runtime_state(0)
        self.assertNotIn(
            "LLAM_IO_UDATA_LINUX_NATIVE", off_preprocessed_state
        )
        self.assertNotIn(
            "LLAM_IO_UDATA_NATIVE_SEGMENT", off_preprocessed_state
        )
        self.assertNotIn(
            "LLAM_IO_UDATA_NATIVE_CANCEL", off_preprocessed_state
        )

        self._build_cmake_mode(self.default_cmake_dir, "research-off")
        self._build_cmake_mode(self.research_cmake_dir, "research-on")

        off_static = self._find_static_library(self.default_cmake_dir)
        off_static_symbols = self._run(["nm", str(off_static)])
        self.assert_command_succeeded(
            off_static_symbols, "research-off static symbol inspection"
        )
        self.assertNotIn(
            "llam_issue_linux_native_segment",
            self._combined_output(off_static_symbols),
        )

        off_headers = self._installed_public_header_hashes(
            self.default_cmake_dir, "off"
        )
        on_headers = self._installed_public_header_hashes(
            self.research_cmake_dir, "on"
        )
        self.assertTrue(off_headers)
        self.assertEqual(on_headers, off_headers)
        for mode in ("off", "on"):
            install_dir = self.work / f"install-{mode}"
            metadata = "\n".join(
                path.read_text(encoding="utf-8")
                for path in sorted(install_dir.rglob("*"))
                if path.is_file()
                and path.suffix in {".cmake", ".h", ".pc"}
            )
            self.assertNotIn("LLAM_BUILD_RESEARCH", metadata)

        off_exports = self._defined_dynamic_exports(
            self._find_shared_library(self.default_cmake_dir)
        )
        on_exports = self._defined_dynamic_exports(
            self._find_shared_library(self.research_cmake_dir)
        )
        self.assertTrue(off_exports)
        self.assertEqual(on_exports, off_exports)

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
        shared_build = self._run(
            [
                "cmake",
                "--build",
                str(self.research_cmake_dir),
                "--target",
                "llam_runtime_shared",
                "--parallel",
                "4",
                "--verbose",
            ]
        )
        self.assert_command_succeeded(
            shared_build, "research CMake shared runtime build"
        )
        shared_trace = self._combined_output(shared_build)
        self.assertEqual(
            shared_trace.count("cmake/write_build_provenance.cmake"),
            2,
        )

    def test_windows_provenance_requires_shared_import_library(
        self,
    ) -> None:
        package_dir = self.work / "windows-package-provenance"
        package_dir.mkdir(parents=True, exist_ok=True)
        artifacts = [
            package_dir / "llam_runtime.lib",
            package_dir / "llam_runtime.dll",
            package_dir / "llam_runtime_shared.lib",
            package_dir / "bench.exe",
        ]
        for artifact in artifacts:
            artifact.write_bytes(b"artifact")
            Path(f"{artifact}.llam-build-provenance").write_text(
                "LLAM_BUILD_RESEARCH=0\n", encoding="utf-8"
            )
        command = [
            "python3",
            str(self.source / "scripts/check_release_provenance.py"),
            "--windows-artifacts",
            *(str(artifact) for artifact in artifacts),
        ]

        stable = self._run(command)
        self.assert_command_succeeded(
            stable, "stable Windows artifact provenance check"
        )

        import_provenance = Path(
            f"{artifacts[2]}.llam-build-provenance"
        )
        import_provenance.unlink()
        missing = self._run(command)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn(
            f"missing trustworthy build provenance: {import_provenance}",
            missing.stderr,
        )

        import_provenance.write_text(
            "LLAM_BUILD_RESEARCH=1\n", encoding="utf-8"
        )
        research = self._run(command)
        self.assertNotEqual(research.returncode, 0)
        self.assertIn(
            "research-enabled builds cannot be packaged", research.stderr
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
