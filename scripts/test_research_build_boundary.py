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
import re
import shutil
import shlex
import subprocess
import tempfile
import time
import unittest
from unittest import mock


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
            name_pattern = None
        else:
            pattern = "libllam_runtime.so*"
            name_pattern = re.compile(
                r"libllam_runtime\.so(?:\.\d+)*"
            )
        matches = [
            path
            for path in build_dir.rglob(pattern)
            if path.is_file() and not path.is_symlink()
            and (
                name_pattern is None
                or name_pattern.fullmatch(path.name) is not None
            )
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

    @staticmethod
    def _sha256_tree(root: Path) -> dict[str, str]:
        if not root.is_dir():
            raise AssertionError(f"expected directory: {root}")
        return {
            str(path.relative_to(root)): hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
            for path in sorted(root.rglob("*"))
            if path.is_file()
        }

    @staticmethod
    def _normalized_prefix_text(text: str, prefix: Path) -> str:
        return text.replace(str(prefix), "${prefix}").replace("\\", "/")

    @classmethod
    def _build_and_install_contract_mode(cls, mode: str) -> Path:
        prefix = cls.work / f"install-contract-{mode}"
        build_dir = cls.work / f"cmake-contract-{mode}"
        configure_result = cls._run(
            [
                "cmake",
                "-S",
                str(cls.source),
                "-B",
                str(build_dir),
                f"-DLLAM_BUILD_RESEARCH={'ON' if mode == 'on' else 'OFF'}",
                f"-DCMAKE_INSTALL_PREFIX={prefix}",
            ]
        )
        cls.assert_command_succeeded(
            configure_result, f"{mode} CMake contract configure"
        )
        cls._build_cmake_mode(build_dir, f"research-{mode}")
        prefix = cls.work / f"install-contract-{mode}"
        result = cls._run(
            [
                "cmake",
                "--install",
                str(build_dir),
            ]
        )
        cls.assert_command_succeeded(result, f"{mode} CMake contract install")
        return prefix

    @classmethod
    def _installed_library_dir(cls, prefix: Path) -> Path:
        return cls._find_shared_library(prefix).parent

    @staticmethod
    def _parse_abi_probe_record(output: str) -> dict[str, str]:
        record = {}
        for line in output.splitlines():
            key, separator, value = line.partition("=")
            if not separator or not key or key in record:
                raise AssertionError(f"invalid installed ABI probe line: {line}")
            record[key] = value
        return record

    @staticmethod
    def _validate_abi_probe_record(
        record: dict[str, str], prefix: Path
    ) -> dict[str, str]:
        required = {
            "abi_version",
            "abi_major",
            "abi_minor",
            "version_major",
            "version_minor",
            "version_patch",
            "reserved0",
            "struct_size",
            "runtime_opts_size",
            "spawn_opts_size",
            "runtime_stats_size",
            "runtime_name",
            "version_string",
            "platform_name",
        }
        expected = required | ({"loaded_image"} if os.name != "nt" else set())
        if set(record) != expected:
            raise AssertionError(f"unexpected installed ABI record: {record}")
        for field in required - {"runtime_name", "version_string", "platform_name"}:
            if not record[field].isdigit():
                raise AssertionError(f"non-numeric ABI probe field {field}")
        expected_values = {
            "abi_version": str(2 << 16),
            "abi_major": "2",
            "abi_minor": "0",
            "version_major": "2",
            "version_minor": "2",
            "version_patch": "0",
            "reserved0": "0",
            "runtime_name": "LLAM",
            "version_string": "2.2.0",
        }
        for field, value in expected_values.items():
            if record[field] != value:
                raise AssertionError(
                    f"unexpected installed ABI field {field}: {record[field]}"
                )
        for field in (
            "struct_size",
            "runtime_opts_size",
            "spawn_opts_size",
            "runtime_stats_size",
        ):
            if int(record[field]) <= 0:
                raise AssertionError(f"non-positive installed ABI field {field}")
        normalized = record.copy()
        if "loaded_image" in record:
            loaded_image = Path(record["loaded_image"]).resolve()
            if not loaded_image.is_relative_to(prefix.resolve()):
                raise AssertionError(
                    f"ABI probe loaded outside installed prefix: {loaded_image}"
                )
            normalized["loaded_image"] = "${prefix}/" + str(
                loaded_image.relative_to(prefix.resolve())
            )
        return normalized

    @classmethod
    def _abi_probe(cls, prefix: Path, mode: str) -> dict[str, str]:
        probe = cls.work / f"installed-abi-probe-{mode}.c"
        binary = cls.work / f"installed-abi-probe-{mode}"
        probe.write_text(
            '#include <llam/runtime.h>\n'
            "#include <dlfcn.h>\n"
            "#include <stdio.h>\n"
            "int main(void) {\n"
            "    llam_abi_info_t info = {0};\n"
            "    if (llam_abi_get_info(&info, sizeof(info)) != 0) {\n"
            "        return 1;\n"
            "    }\n"
            '    printf("abi_version=%u\\n", llam_abi_version());\n'
            '    printf("abi_major=%u\\n", info.abi_major);\n'
            '    printf("abi_minor=%u\\n", info.abi_minor);\n'
            '    printf("version_major=%u\\n", info.version_major);\n'
            '    printf("version_minor=%u\\n", info.version_minor);\n'
            '    printf("version_patch=%u\\n", info.version_patch);\n'
            '    printf("reserved0=%u\\n", info.reserved0);\n'
            '    printf("struct_size=%zu\\n", info.struct_size);\n'
            '    printf("runtime_opts_size=%zu\\n", info.runtime_opts_size);\n'
            '    printf("spawn_opts_size=%zu\\n", info.spawn_opts_size);\n'
            '    printf("runtime_stats_size=%zu\\n", info.runtime_stats_size);\n'
            '    printf("runtime_name=%s\\n", info.runtime_name);\n'
            '    printf("version_string=%s\\n", info.version_string);\n'
            '    printf("platform_name=%s\\n", info.platform_name);\n'
            "#ifndef _WIN32\n"
            "    Dl_info loaded = {0};\n"
            "    if (dladdr((const void *)&llam_abi_version, &loaded) == 0 "
            "|| loaded.dli_fname == NULL) {\n"
            "        return 2;\n"
            "    }\n"
            '    printf("loaded_image=%s\\n", loaded.dli_fname);\n'
            "#endif\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        compile_result = cls._run(
            [
                os.environ.get("CC", "cc"),
                "-std=c11",
                "-D_GNU_SOURCE",
                str(probe),
                "-I",
                str(prefix / "include"),
                "-L",
                str(cls._installed_library_dir(prefix)),
                "-lllam_runtime",
                "-o",
                str(binary),
                *([] if os.uname().sysname == "Darwin" else ["-ldl"]),
            ]
        )
        cls.assert_command_succeeded(
            compile_result, f"{mode} installed ABI probe compile"
        )
        environment = os.environ.copy()
        library_dir = str(cls._installed_library_dir(prefix))
        if os.uname().sysname == "Darwin":
            environment["DYLD_LIBRARY_PATH"] = library_dir
        else:
            environment["LD_LIBRARY_PATH"] = library_dir
        run_result = cls._run([str(binary)], env=environment)
        cls.assert_command_succeeded(
            run_result, f"{mode} installed ABI probe run"
        )
        return cls._validate_abi_probe_record(
            cls._parse_abi_probe_record(run_result.stdout), prefix
        )

    @classmethod
    def _pkg_config_contract(cls, prefix: Path) -> dict[str, str]:
        metadata = [
            path
            for path in prefix.rglob("llam.pc")
            if path.is_file() and path.parent.name == "pkgconfig"
        ]
        if len(metadata) != 1:
            raise AssertionError(
                f"expected one installed pkg-config file, got: {metadata}"
            )
        tool = shutil.which("pkg-config") or shutil.which("pkgconf")
        if tool is None:
            raise AssertionError("pkg-config or pkgconf is required")
        environment = os.environ.copy()
        environment["PKG_CONFIG_PATH"] = str(metadata[0].parent)
        environment["PKG_CONFIG_LIBDIR"] = str(metadata[0].parent)
        environment.pop("PKG_CONFIG_SYSROOT_DIR", None)

        def query(*arguments: str) -> str:
            result = cls._run([tool, *arguments, "llam"], env=environment)
            cls.assert_command_succeeded(result, f"pkg-config {' '.join(arguments)}")
            return result.stdout.strip()

        resolved_path = Path(query("--path")).resolve()
        if resolved_path != metadata[0].resolve():
            raise AssertionError(
                f"pkg-config resolved unexpected metadata: {resolved_path}"
            )
        version = query("--modversion")
        cflags = query("--cflags")
        libs = query("--libs")
        library_dir = cls._installed_library_dir(prefix)
        expected_cflag = f"-I{prefix / 'include'}"
        expected_libflag = f"-L{library_dir}"
        if expected_cflag not in shlex.split(cflags):
            raise AssertionError(f"pkg-config cflags escaped prefix: {cflags}")
        if expected_libflag not in shlex.split(libs):
            raise AssertionError(f"pkg-config libs escaped prefix: {libs}")
        if "-lllam_runtime" not in shlex.split(libs) or version != "2.2.0":
            raise AssertionError(f"unexpected pkg-config contract: {version} {libs}")

        source = cls.work / f"pkg-config-consumer-{prefix.name}.c"
        binary = cls.work / f"pkg-config-consumer-{prefix.name}"
        source.write_text(
            "#include <llam/runtime.h>\n"
            "int main(void) {\n"
            "    return llam_abi_version() == ((2U << 16) | 0U) ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )
        compile_result = cls._run(
            [
                os.environ.get("CC", "cc"),
                str(source),
                *shlex.split(cflags),
                *shlex.split(libs),
                "-o",
                str(binary),
            ]
        )
        cls.assert_command_succeeded(
            compile_result, "pkg-config installed consumer compile"
        )
        run_environment = os.environ.copy()
        library_dir_text = str(library_dir)
        if os.uname().sysname == "Darwin":
            run_environment["DYLD_LIBRARY_PATH"] = library_dir_text
        else:
            run_environment["LD_LIBRARY_PATH"] = library_dir_text
        run_result = cls._run([str(binary)], env=run_environment)
        cls.assert_command_succeeded(
            run_result, "pkg-config installed consumer run"
        )
        return {
            "path": cls._normalized_prefix_text(str(resolved_path), prefix),
            "version": version,
            "cflags": cls._normalized_prefix_text(cflags, prefix),
            "libs": cls._normalized_prefix_text(libs, prefix),
        }

    @classmethod
    def _cmake_package_contract(
        cls,
        prefix: Path,
        mode: str,
        *,
        exact_config_dir: Path | None = None,
        fallback_prefix: Path | None = None,
    ) -> str:
        config_matches = list(prefix.rglob("llam-config.cmake"))
        if len(config_matches) != 1:
            raise AssertionError(
                f"expected one installed CMake config, got: {config_matches}"
            )
        source_dir = cls.work / f"cmake-consumer-{mode}"
        build_dir = cls.work / f"cmake-consumer-build-{mode}"
        config_dir = exact_config_dir or config_matches[0].parent
        source_dir.mkdir(parents=True, exist_ok=True)
        (source_dir / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(llam_installed_consumer C)\n"
            "find_package(llam 2.2 CONFIG REQUIRED PATHS "
            "\"${LLAM_EXACT_CONFIG_DIR}\" NO_DEFAULT_PATH)\n"
            "file(REAL_PATH \"${llam_DIR}\" resolved_llam_dir)\n"
            "file(REAL_PATH \"${LLAM_EXACT_PREFIX}\" expected_prefix)\n"
            "string(FIND \"${resolved_llam_dir}/\" \"${expected_prefix}/\" "
            "prefix_index)\n"
            "if(NOT prefix_index EQUAL 0)\n"
            "  message(FATAL_ERROR \"llam_DIR escaped prefix: ${resolved_llam_dir}\")\n"
            "endif()\n"
            "file(WRITE \"${CMAKE_BINARY_DIR}/contract.txt\" "
            "\"llam_DIR=${resolved_llam_dir}\\n\")\n"
            "function(record_property target property required)\n"
            "  get_property(is_set TARGET ${target} PROPERTY ${property} SET)\n"
            "  if(NOT is_set)\n"
            "    if(required)\n"
            "      message(FATAL_ERROR \"missing ${target} ${property}\")\n"
            "    endif()\n"
            "    return()\n"
            "  endif()\n"
            "  get_target_property(value ${target} ${property})\n"
            "  if(\"${value}\" MATCHES \"-NOTFOUND$\")\n"
            "    message(FATAL_ERROR \"not found ${target} ${property}\")\n"
            "  endif()\n"
            "  file(APPEND \"${CMAKE_BINARY_DIR}/contract.txt\" "
            "\"${target}.${property}=${value}\\n\")\n"
            "endfunction()\n"
            "function(assert_artifact_prefix artifact)\n"
            "  if(IS_ABSOLUTE \"${artifact}\")\n"
            "    file(REAL_PATH \"${artifact}\" resolved_artifact)\n"
            "    string(FIND \"${resolved_artifact}\" \"${expected_prefix}/\" "
            "artifact_index)\n"
            "    if(NOT artifact_index EQUAL 0)\n"
            "      message(FATAL_ERROR \"artifact escaped prefix: ${resolved_artifact}\")\n"
            "    endif()\n"
            "  endif()\n"
            "endfunction()\n"
            "foreach(target IN ITEMS llam::runtime llam::runtime_shared)\n"
            "  if(NOT TARGET ${target})\n"
            "    message(FATAL_ERROR \"missing exported target ${target}\")\n"
            "  endif()\n"
            "  record_property(${target} IMPORTED_CONFIGURATIONS TRUE)\n"
            "  get_target_property(configurations ${target} IMPORTED_CONFIGURATIONS)\n"
            "  foreach(configuration IN LISTS configurations)\n"
            "    foreach(property IN ITEMS IMPORTED_LOCATION IMPORTED_IMPLIB "
            "IMPORTED_SONAME IMPORTED_NO_SONAME IMPORTED_LINK_INTERFACE_LIBRARIES "
            "IMPORTED_LINK_DEPENDENT_LIBRARIES "
            "IMPORTED_LINK_INTERFACE_LANGUAGES)\n"
            "      set(full_property ${property}_${configuration})\n"
            "      if(property STREQUAL IMPORTED_LOCATION OR "
            "property STREQUAL IMPORTED_IMPLIB)\n"
            "        if(property STREQUAL IMPORTED_LOCATION)\n"
            "          record_property(${target} ${full_property} TRUE)\n"
            "        else()\n"
            "          record_property(${target} ${full_property} FALSE)\n"
            "        endif()\n"
            "        get_property(artifact_is_set TARGET ${target} "
            "PROPERTY ${full_property} SET)\n"
            "        if(artifact_is_set)\n"
            "          get_target_property(artifact ${target} ${full_property})\n"
            "          assert_artifact_prefix(\"${artifact}\")\n"
            "        endif()\n"
            "      else()\n"
            "        record_property(${target} ${full_property} FALSE)\n"
            "      endif()\n"
            "    endforeach()\n"
            "  endforeach()\n"
            "  foreach(property IN ITEMS INTERFACE_INCLUDE_DIRECTORIES "
            "INTERFACE_LINK_LIBRARIES INTERFACE_COMPILE_DEFINITIONS "
            "INTERFACE_COMPILE_OPTIONS INTERFACE_LINK_OPTIONS "
            "INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)\n"
            "    record_property(${target} ${property} FALSE)\n"
            "  endforeach()\n"
            "  get_target_property(include_dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)\n"
            "  if(\"${include_dirs}\" MATCHES \"-NOTFOUND$\")\n"
            "    message(FATAL_ERROR \"missing public include directories for ${target}\")\n"
            "  endif()\n"
            "  foreach(include_dir IN LISTS include_dirs)\n"
            "    assert_artifact_prefix(\"${include_dir}\")\n"
            "  endforeach()\n"
            "endforeach()\n"
            "add_executable(llam_installed_consumer_runtime main.c)\n"
            "target_link_libraries(llam_installed_consumer_runtime PRIVATE "
            "llam::runtime)\n"
            "add_executable(llam_installed_consumer_shared main.c)\n"
            "target_link_libraries(llam_installed_consumer_shared PRIVATE "
            "llam::runtime_shared)\n",
            encoding="utf-8",
        )
        (source_dir / "main.c").write_text(
            "#include <llam/runtime.h>\n"
            "int main(void) {\n"
            "    return llam_abi_version() == ((2U << 16) | 0U) ? 0 : 1;\n"
            "}\n",
            encoding="utf-8",
        )
        configure_command = [
            "cmake",
            "-S",
            str(source_dir),
            "-B",
            str(build_dir),
            f"-DLLAM_EXACT_PREFIX={prefix}",
            f"-DLLAM_EXACT_CONFIG_DIR={config_dir}",
        ]
        if fallback_prefix is not None:
            configure_command.append(f"-DCMAKE_PREFIX_PATH={fallback_prefix}")
        configure_result = cls._run(configure_command)
        cls.assert_command_succeeded(
            configure_result, f"{mode} installed CMake consumer configure"
        )
        build_result = cls._run(["cmake", "--build", str(build_dir)])
        cls.assert_command_succeeded(
            build_result, f"{mode} installed CMake consumer build"
        )
        environment = os.environ.copy()
        library_dir = str(cls._installed_library_dir(prefix))
        if os.uname().sysname == "Darwin":
            environment["DYLD_LIBRARY_PATH"] = library_dir
        else:
            environment["LD_LIBRARY_PATH"] = library_dir
        for consumer in (
            "llam_installed_consumer_runtime",
            "llam_installed_consumer_shared",
        ):
            executable = build_dir / consumer
            if os.name == "nt":
                executable = executable.with_suffix(".exe")
            run_result = cls._run([str(executable)], env=environment)
            cls.assert_command_succeeded(
                run_result, f"{mode} installed CMake {consumer} run"
            )
        return cls._normalized_prefix_text(
            (build_dir / "contract.txt").read_text(encoding="utf-8"), prefix
        )

    @classmethod
    def _soname(cls, library: Path) -> str:
        if os.uname().sysname == "Darwin":
            result = cls._run(["otool", "-D", str(library)])
            cls.assert_command_succeeded(result, f"SONAME inspection for {library}")
            lines = [line.strip() for line in result.stdout.splitlines()]
            if len(lines) != 2:
                raise AssertionError(f"unexpected Darwin install name output: {lines}")
            return lines[1]
        if shutil.which("readelf"):
            command = ["readelf", "-d", str(library)]
        elif shutil.which("objdump"):
            command = ["objdump", "-p", str(library)]
        else:
            raise AssertionError("SONAME inspection requires readelf or objdump")
        result = cls._run(command)
        cls.assert_command_succeeded(result, f"SONAME inspection for {library}")
        match = re.search(r"SONAME.*?\[(.+)\]", result.stdout)
        if match is None:
            raise AssertionError(f"no SONAME in {library}: {result.stdout}")
        return match.group(1)

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

    def test_linux_verifier_separates_stable_and_research_builds(
        self,
    ) -> None:
        verifier = (
            self.source / "scripts" / "verify_linux.sh"
        ).read_text(encoding="utf-8")
        stable_build = (
            'make -j"$JOBS" LLAM_BUILD_RESEARCH=0 all test'
        )
        stable_probe = 'run(["./demo"], timeout=30)'
        research_build = (
            'make -j"$JOBS" LLAM_BUILD_RESEARCH=1 \\\n'
            "    test_leir_native_linux "
            "bench_leir_native_segment bench_leir_native_pipeline"
        )
        stable_restore = (
            'make -j"$JOBS" LLAM_BUILD_RESEARCH=0 all'
        )
        self.assertIn(stable_build, verifier)
        self.assertIn(research_build, verifier)
        self.assertEqual(
            verifier.splitlines().count(stable_restore),
            1,
        )
        self.assertLess(verifier.index(stable_build), verifier.index(stable_probe))
        self.assertLess(
            verifier.index(stable_probe),
            verifier.index(research_build),
        )
        self.assertLess(
            verifier.index(research_build),
            verifier.rindex(stable_restore),
        )

    def test_native_research_workflow_keeps_runtime_regression_stable(
        self,
    ) -> None:
        workflow = (
            self.source / ".github/workflows/leir-native-research.yml"
        ).read_text(encoding="utf-8")
        regression = workflow.split(
            "      - name: Full runtime regression and export gates\n",
            1,
        )[1].split("      - name:", 1)[0]
        self.assertIn(
            "env LLAM_BUILD_RESEARCH=0 make -j2 test CC=gcc",
            regression,
        )
        self.assertIn(
            "env LLAM_BUILD_RESEARCH=0 make -j2 shared "
            "audit-shared-exports",
            regression,
        )

    def test_bsd_packaging_workflows_provision_python3(self) -> None:
        for relative in (
            ".github/workflows/bsd.yml",
            ".github/workflows/release.yml",
        ):
            with self.subTest(workflow=relative):
                workflow = (self.source / relative).read_text(
                    encoding="utf-8"
                )
                self.assertIn("python3", workflow)
                self.assertIn("python%3.12", workflow)
                self.assertIn("python312", workflow)
                self.assertIn("command -v python3", workflow)

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

    def test_installed_contract_parity(self) -> None:
        off = self._build_and_install_contract_mode("off")
        on = self._build_and_install_contract_mode("on")
        off_library = self._find_shared_library(off)
        on_library = self._find_shared_library(on)

        self.assertEqual(
            self._sha256_tree(off / "include"),
            self._sha256_tree(on / "include"),
        )
        self.assertEqual(
            self._defined_dynamic_exports(off_library),
            self._defined_dynamic_exports(on_library),
        )
        self.assertEqual(
            self._abi_probe(off, "off"), self._abi_probe(on, "on")
        )
        self.assertEqual(
            self._pkg_config_contract(off), self._pkg_config_contract(on)
        )
        self.assertEqual(
            self._cmake_package_contract(off, "off"),
            self._cmake_package_contract(on, "on"),
        )
        self.assertEqual(self._soname(off_library), self._soname(on_library))

        experiment_files = [
            path.relative_to(off)
            for path in off.rglob("*")
            if path.is_file() and "experiments" in path.relative_to(off).parts
        ]
        self.assertEqual(experiment_files, [])

        package_source = self.work / "installed-contract-package"
        package_script = package_source / "scripts" / "package_release.sh"
        package_script.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(
            self.source / "scripts" / "package_release.sh", package_script
        )
        package_result = self._run(
            ["/bin/sh", str(package_script)],
            env={**os.environ, "LLAM_BUILD_RESEARCH": "1"},
        )
        self.assertNotEqual(package_result.returncode, 0)
        self.assertIn(
            "research-enabled builds cannot be packaged", package_result.stderr
        )
        self.assertFalse((package_source / "target" / "dist").exists())

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


class SharedLibraryFinderTests(unittest.TestCase):
    def test_linux_finder_selects_library_not_provenance_receipts(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(
            prefix="linux-shared-library-finder-"
        ) as directory:
            finder_dir = Path(directory)
            actual_library = finder_dir / "libllam_runtime.so.2.2.0"
            actual_library.write_bytes(b"\x7fELFfixture")
            for symlink_name in (
                "libllam_runtime.so",
                "libllam_runtime.so.2",
            ):
                (finder_dir / symlink_name).symlink_to(actual_library.name)
            for non_library_name in (
                "libllam_runtime.so.llam-build-provenance",
                "libllam_runtime.so.2.2.0.llam-build-provenance",
                "libllam_runtime.so.backup",
            ):
                (finder_dir / non_library_name).write_bytes(b"not a library")

            linux_uname = os.uname_result(
                ("Linux", "fixture", "6.0", "fixture", "x86_64")
            )
            with mock.patch.object(
                os, "uname", return_value=linux_uname
            ):
                self.assertEqual(
                    ResearchBoundaryTests._find_shared_library(finder_dir),
                    actual_library,
                )


class InstalledContractReceiptMutationTests(unittest.TestCase):
    def test_pkg_config_contract_rejects_stale_install_prefix(self) -> None:
        with tempfile.TemporaryDirectory(prefix="stale-llam-pc-") as directory:
            prefix = Path(directory)
            metadata_dir = prefix / "lib" / "pkgconfig"
            metadata_dir.mkdir(parents=True)
            (metadata_dir / "llam.pc").write_text(
                "prefix=/usr/local\n"
                "libdir=${prefix}/lib\n"
                "includedir=${prefix}/include\n"
                "Name: LLAM\n"
                "Description: stale fixture\n"
                "Version: 2.2.0\n"
                "Libs: -L${libdir} -lllam_runtime\n"
                "Cflags: -I${includedir}\n",
                encoding="utf-8",
            )
            with self.assertRaises(AssertionError):
                ResearchBoundaryTests._pkg_config_contract(prefix)

    @staticmethod
    def _write_fake_cmake_package(
        prefix: Path,
        *,
        link_languages: str,
        implib: Path | None = None,
    ) -> Path:
        config_dir = prefix / "lib" / "cmake" / "llam"
        config_dir.mkdir(parents=True, exist_ok=True)
        include_dir = prefix / "include"
        include_dir.mkdir(parents=True, exist_ok=True)
        header_dir = include_dir / "llam"
        header_dir.mkdir(exist_ok=True)
        header_dir.joinpath("runtime.h").write_text(
            "unsigned llam_abi_version(void);\n", encoding="utf-8"
        )
        source = prefix / "fake_runtime.c"
        object_file = prefix / "fake_runtime.o"
        artifact = prefix / "lib" / "libfake_runtime.a"
        artifact.parent.mkdir(exist_ok=True)
        source.write_text(
            "unsigned llam_abi_version(void) { return 2U << 16; }\n",
            encoding="utf-8",
        )
        compile_result = subprocess.run(
            [os.environ.get("CC", "cc"), "-c", str(source), "-o", str(object_file)],
            check=False,
            text=True,
            capture_output=True,
        )
        if compile_result.returncode != 0:
            raise AssertionError(compile_result.stderr)
        archive_result = subprocess.run(
            ["ar", "rcs", str(artifact), str(object_file)],
            check=False,
            text=True,
            capture_output=True,
        )
        if archive_result.returncode != 0:
            raise AssertionError(archive_result.stderr)
        if os.uname().sysname == "Darwin":
            shared_artifact = prefix / "lib" / "libllam_runtime.2.dylib"
            shared_command = [
                os.environ.get("CC", "cc"),
                "-dynamiclib",
                str(source),
                "-o",
                str(shared_artifact),
            ]
        else:
            shared_artifact = prefix / "lib" / "libllam_runtime.so.2.2.0"
            shared_command = [
                os.environ.get("CC", "cc"),
                "-shared",
                "-fPIC",
                str(source),
                "-o",
                str(shared_artifact),
            ]
        shared_result = subprocess.run(
            shared_command, check=False, text=True, capture_output=True
        )
        if shared_result.returncode != 0:
            raise AssertionError(shared_result.stderr)
        implib_property = ""
        if implib is not None:
            implib_property = f"    IMPORTED_IMPLIB_RELEASE \"{implib}\"\n"
        config_dir.joinpath("llam-config-version.cmake").write_text(
            "set(PACKAGE_VERSION \"2.2.0\")\n"
            "set(PACKAGE_VERSION_COMPATIBLE TRUE)\n",
            encoding="utf-8",
        )
        config_dir.joinpath("llam-config.cmake").write_text(
            "foreach(target IN ITEMS runtime runtime_shared)\n"
            "  add_library(llam::${target} STATIC IMPORTED)\n"
            "  set_target_properties(llam::${target} PROPERTIES\n"
            "    IMPORTED_CONFIGURATIONS RELEASE\n"
            f"    IMPORTED_LOCATION_RELEASE \"{artifact}\"\n"
            f"    IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE \"{link_languages}\"\n"
            f"{implib_property}"
            f"    INTERFACE_INCLUDE_DIRECTORIES \"{include_dir}\")\n"
            "endforeach()\n",
            encoding="utf-8",
        )
        return config_dir

    def test_cmake_consumer_rejects_fallback_notfound_and_escaped_artifacts(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory(prefix="cmake-contract-mutation-") as directory:
            root = Path(directory)
            expected = root / "expected"
            fallback = root / "fallback"
            outside_implib = root / "outside.lib"
            outside_implib.write_bytes(b"outside")
            self._write_fake_cmake_package(expected, link_languages="C")
            self._write_fake_cmake_package(fallback, link_languages="C")
            previous_work = getattr(ResearchBoundaryTests, "work", None)
            ResearchBoundaryTests.work = root / "work"
            try:
                with self.assertRaisesRegex(
                    AssertionError, "Could not find a package configuration file"
                ):
                    ResearchBoundaryTests._cmake_package_contract(
                        expected,
                        "fallback",
                        exact_config_dir=root / "missing",
                        fallback_prefix=fallback,
                    )
                with self.assertRaisesRegex(
                    AssertionError, "artifact escaped prefix"
                ):
                    self._write_fake_cmake_package(
                        expected, link_languages="C", implib=outside_implib
                    )
                    ResearchBoundaryTests._cmake_package_contract(expected, "escape")
                notfound = root / "notfound"
                self._write_fake_cmake_package(notfound, link_languages="C")
                config = notfound / "lib" / "cmake" / "llam" / "llam-config.cmake"
                config.write_text(
                    config.read_text(encoding="utf-8").replace(
                        str(notfound / "lib" / "libfake_runtime.a"),
                        "missing-NOTFOUND",
                    ),
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(AssertionError, "not found"):
                    ResearchBoundaryTests._cmake_package_contract(notfound, "notfound")
                c_only = root / "c-only"
                asm_and_c = root / "asm-and-c"
                self._write_fake_cmake_package(c_only, link_languages="C")
                self._write_fake_cmake_package(asm_and_c, link_languages="ASM;C")
                c_contract = ResearchBoundaryTests._cmake_package_contract(c_only, "c")
                asm_contract = ResearchBoundaryTests._cmake_package_contract(
                    asm_and_c, "asm"
                )
                self.assertNotEqual(c_contract, asm_contract)
            finally:
                if previous_work is None:
                    del ResearchBoundaryTests.work
                else:
                    ResearchBoundaryTests.work = previous_work

    def test_abi_probe_record_rejects_missing_wrong_and_escaped_fields(self) -> None:
        with tempfile.TemporaryDirectory(prefix="abi-probe-mutation-") as directory:
            prefix = Path(directory)
            library = prefix / "lib" / "libllam_runtime.fixture"
            library.parent.mkdir(parents=True)
            library.write_bytes(b"fixture")
            record = {
                "abi_version": str(2 << 16),
                "abi_major": "2",
                "abi_minor": "0",
                "version_major": "2",
                "version_minor": "2",
                "version_patch": "0",
                "reserved0": "0",
                "struct_size": "104",
                "runtime_opts_size": "64",
                "spawn_opts_size": "48",
                "runtime_stats_size": "128",
                "runtime_name": "LLAM",
                "version_string": "2.2.0",
                "platform_name": "fixture",
            }
            if os.name != "nt":
                record["loaded_image"] = str(library)
            self.assertEqual(
                ResearchBoundaryTests._validate_abi_probe_record(record, prefix)[
                    "version_string"
                ],
                "2.2.0",
            )
            missing = record.copy()
            missing.pop("runtime_stats_size")
            with self.assertRaises(AssertionError):
                ResearchBoundaryTests._validate_abi_probe_record(missing, prefix)
            wrong = record.copy()
            wrong["version_minor"] = "3"
            with self.assertRaises(AssertionError):
                ResearchBoundaryTests._validate_abi_probe_record(wrong, prefix)
            if os.name != "nt":
                escaped = record.copy()
                escaped["loaded_image"] = str(
                    Path(directory).parent / "outside.dylib"
                )
                with self.assertRaises(AssertionError):
                    ResearchBoundaryTests._validate_abi_probe_record(escaped, prefix)


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
