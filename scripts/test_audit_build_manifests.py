#!/usr/bin/env python3
"""Fixture tests for the non-evaluating LLAM build-manifest audit."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


AUDIT = Path(__file__).resolve().with_name("audit_build_manifests.py")
PLATFORM_GROUPS = (
    "linux",
    "kqueue",
    "windows",
    "context_arm64",
    "linux_x86_64",
    "linux_x86_64_wake",
    "linux_arm64",
    "darwin_x86_64",
    "darwin_arm64",
    "windows_gnu_x86_64",
    "windows_msvc_x86_64",
)


def valid_sources_manifest() -> dict[str, object]:
    platform_sources = {name: [] for name in PLATFORM_GROUPS}
    platform_sources["linux"] = ["src/linux.c"]
    return {
        "schema": "llam.build-manifest.v1",
        "stable": {
            "common_sources": ["src/common.c"],
            "platform_sources": platform_sources,
        },
        "tests": {
            "public": [
                {
                    "name": "test_public",
                    "platforms": ["all"],
                    "sources": ["tests/test_public.c"],
                    "link_dependencies": ["llam_runtime"],
                },
                {
                    "name": "test_shared_load",
                    "platforms": ["linux", "darwin", "bsd"],
                    "sources": ["tests/test_shared_load.c"],
                    "link_dependencies": ["system_dynamic_loader"],
                },
            ],
            "internal": [
                {
                    "name": "test_internal",
                    "platforms": ["all"],
                    "sources": ["tests/test_internal.c"],
                    "link_dependencies": ["llam_runtime"],
                }
            ],
            "test_hook": [
                {
                    "name": "test_windows_iocp_io",
                    "platforms": ["windows"],
                    "sources": ["tests/test_windows_iocp_io.c"],
                    "link_dependencies": ["llam_runtime_testhooks"],
                }
            ],
        },
        "research": {
            "runtime_platform_sources": {
                "linux": ["src/linux_research.c"],
            },
            "private_headers": [
                "src/runtime_io_segment_linux_internal.h",
            ],
            "targets": [
                {
                    "family": "leir",
                    "name": "test_research",
                    "platforms": ["all"],
                    "sources": ["experiments/leir/test_research.c"],
                    "link_dependencies": ["llam_runtime"],
                }
            ],
        },
    }


def valid_makefile() -> str:
    platform_lines = []
    for group in PLATFORM_GROUPS:
        value = "$(OBJDIR)/src/linux.o" if group == "linux" else ""
        platform_lines.append(
            f"RUNTIME_{group.upper()}_OBJS = {value}".replace("-", "_")
        )
    return "\n".join(
        [
            "DEPFLAGS ?= -MMD -MP",
            "LLAM_VERSION ?= 2.2.0",
            "LLAM_ABI_MAJOR ?= 2",
            "RUNTIME_COMMON_OBJS = $(OBJDIR)/src/common.o",
            *platform_lines,
            "RESEARCH_RUNTIME_LINUX_OBJS = $(OBJDIR)/src/linux_research.o",
            (
                "RESEARCH_PRIVATE_HDRS = "
                "src/runtime_io_segment_linux_internal.h"
            ),
            "RUNTIME_PRIV_HDRS += $(RESEARCH_PRIVATE_HDRS)",
            "ifeq ($(HOST_PLATFORM),linux)",
            "RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)",
            "RUNTIME_OBJS += $(RUNTIME_LINUX_OBJS)",
            "ifeq ($(LLAM_BUILD_RESEARCH),1)",
            "RUNTIME_OBJS += $(RESEARCH_RUNTIME_LINUX_OBJS)",
            "endif",
            "else ifeq ($(HOST_PLATFORM),darwin)",
            "RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)",
            "else ifeq ($(HOST_PLATFORM),windows)",
            "RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)",
            "else",
            "RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)",
            "endif",
            (
                "SHARED_RUNTIME_OBJS = "
                "$(patsubst $(OBJDIR)/%,$(SHARED_OBJDIR)/%,$(RUNTIME_OBJS))"
            ),
            (
                "TESTHOOK_RUNTIME_OVERRIDE_OBJS = "
                "$(TESTHOOK_OBJDIR)/src/common.o"
            ),
            (
                "RUNTIME_TESTHOOK_OBJS = "
                "$(filter-out $(OBJDIR)/src/common.o,$(RUNTIME_OBJS)) "
                "$(TESTHOOK_RUNTIME_OVERRIDE_OBJS)"
            ),
            "TEST_PUBLIC_OBJS = $(OBJDIR)/tests/test_public.o",
            "TEST_SHARED_LOAD_OBJS = $(OBJDIR)/tests/test_shared_load.o",
            "TEST_INTERNAL_OBJS = $(OBJDIR)/tests/test_internal.o",
            (
                "TEST_WINDOWS_IOCP_IO_OBJS = "
                "$(OBJDIR)/tests/test_windows_iocp_io.o"
            ),
            (
                "RESEARCH_TEST_RESEARCH_OBJS = "
                "$(OBJDIR)/experiments/leir/test_research.o"
            ),
            "PUBLIC_TEST_TARGETS = test_public test_shared_load",
            "INTERNAL_TEST_TARGETS = test_internal",
            "TEST_HOOK_TEST_TARGETS = test_windows_iocp_io",
            (
                "LINK_TARGETS = $(PUBLIC_TEST_TARGETS) "
                "$(INTERNAL_TEST_TARGETS) $(TEST_HOOK_TEST_TARGETS)"
            ),
            "LEIR_RESEARCH_TARGETS = test_research",
            "LCWE_RESEARCH_TARGETS =",
            "LCCF_RESEARCH_TARGETS =",
            "SREM_RESEARCH_TARGETS =",
            (
                "RESEARCH_LINK_TARGETS = $(LEIR_RESEARCH_TARGETS) "
                "$(LCWE_RESEARCH_TARGETS) $(LCCF_RESEARCH_TARGETS) "
                "$(SREM_RESEARCH_TARGETS)"
            ),
            "BUILD_OBJS = \\",
            "\t$(RUNTIME_COMMON_OBJS) \\",
            "\t$(RUNTIME_LINUX_OBJS) \\",
            "\t$(TEST_PUBLIC_OBJS) \\",
            "\t$(TEST_SHARED_LOAD_OBJS) \\",
            "\t$(TEST_INTERNAL_OBJS) \\",
            "\t$(TEST_WINDOWS_IOCP_IO_OBJS)",
            "RESEARCH_OBJS = \\",
            "\t$(RESEARCH_RUNTIME_LINUX_OBJS) \\",
            "\t$(RESEARCH_TEST_RESEARCH_OBJS)",
            "$(BUILD_OBJS): $(BUILD_SIGNATURE)",
            "$(RESEARCH_OBJS): $(BUILD_SIGNATURE)",
            "$(SHARED_RUNTIME_OBJS): $(SHARED_BUILD_SIGNATURE)",
            (
                "$(TESTHOOK_RUNTIME_OVERRIDE_OBJS): "
                "$(TESTHOOK_BUILD_SIGNATURE)"
            ),
            (
                "ALL_DEPFILES = $(BUILD_OBJS:.o=.d) "
                "$(RESEARCH_OBJS:.o=.d) "
                "$(SHARED_RUNTIME_OBJS:.o=.d) "
                "$(TESTHOOK_RUNTIME_OVERRIDE_OBJS:.o=.d)"
            ),
            "-include $(ALL_DEPFILES)",
            "$(BUILD_SIGNATURE):",
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > $@",
            "$(SHARED_BUILD_SIGNATURE):",
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > $@",
            "$(TESTHOOK_BUILD_SIGNATURE):",
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > $@",
            "$(OBJDIR)/%.o: %.c",
            "\t$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<",
            "$(SHARED_OBJDIR)/%.o: %.c",
            (
                "\t$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) "
                "-fPIC -c -o $@ $<"
            ),
            "$(TESTHOOK_OBJDIR)/%.o: %.c",
            (
                "\t$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) "
                "-DLLAM_ENABLE_TEST_HOOKS=1 -c -o $@ $<"
            ),
            "test_public: $(RUNTIME_OBJS) $(TEST_PUBLIC_OBJS)",
            (
                "\t$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) "
                "$(TEST_PUBLIC_OBJS) $(LDLIBS)"
            ),
            "test_internal: $(RUNTIME_OBJS) $(TEST_INTERNAL_OBJS)",
            (
                "\t$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) "
                "$(TEST_INTERNAL_OBJS) $(LDLIBS)"
            ),
            "test_shared_load: $(TEST_SHARED_LOAD_OBJS)",
            (
                "\t$(CC) $(CFLAGS) -o $@ "
                "$(TEST_SHARED_LOAD_OBJS) $(DL_LIBS)"
            ),
            (
                "test_windows_iocp_io: "
                "$(RUNTIME_TESTHOOK_OBJS) $(TEST_WINDOWS_IOCP_IO_OBJS)"
            ),
            (
                "\t$(CC) $(CFLAGS) -o $@ $(RUNTIME_TESTHOOK_OBJS) "
                "$(TEST_WINDOWS_IOCP_IO_OBJS) $(LDLIBS)"
            ),
            (
                "test_research: "
                "$(RUNTIME_OBJS) $(RESEARCH_TEST_RESEARCH_OBJS)"
            ),
            (
                "\t$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) "
                "$(RESEARCH_TEST_RESEARCH_OBJS) $(LDLIBS)"
            ),
            "libllam_runtime.a: $(RUNTIME_OBJS)",
            "\t$(AR) rcs $@ $(RUNTIME_OBJS)",
            "libllam_runtime.so: $(SHARED_RUNTIME_OBJS)",
            (
                "\t$(CC) -shared -o $@ $(SHARED_RUNTIME_OBJS) "
                "$(LDLIBS)"
            ),
            "audit-build-manifests:",
            (
                "\tpython3 scripts/audit_build_manifests.py "
                "--root . --check"
            ),
            "ifeq ($(HOST_PLATFORM),windows)",
            "test check: audit-build-manifests",
            "else",
            "test: audit-build-manifests",
            "check: test",
            "endif",
            "",
        ]
    )


def valid_cmakelists() -> str:
    platform_lines = []
    for group in PLATFORM_GROUPS:
        value = "src/linux.c" if group == "linux" else ""
        variable = f"LLAM_RUNTIME_{group.upper()}_SOURCES".replace("-", "_")
        platform_lines.append(f"set({variable} {value})")
    return "\n".join(
        [
            "cmake_minimum_required(VERSION 3.20)",
            "project(llam VERSION 2.2.0 LANGUAGES C)",
            "set(LLAM_ABI_VERSION_MAJOR 2)",
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c)",
            *platform_lines,
            (
                "set(LLAM_RESEARCH_RUNTIME_LINUX_SOURCES "
                "src/linux_research.c)"
            ),
            "set(LLAM_RUNTIME_SOURCES ${LLAM_RUNTIME_COMMON_SOURCES})",
            'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")',
            (
                "list(APPEND LLAM_RUNTIME_SOURCES "
                "${LLAM_RUNTIME_LINUX_SOURCES})"
            ),
            "if(LLAM_BUILD_RESEARCH)",
            (
                "list(APPEND LLAM_RUNTIME_SOURCES "
                "${LLAM_RESEARCH_RUNTIME_LINUX_SOURCES})"
            ),
            "endif()",
            "endif()",
            "add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})",
            (
                "add_library(llam_runtime_shared SHARED "
                "${LLAM_RUNTIME_SOURCES})"
            ),
            (
                "add_library(llam_runtime_testhooks STATIC "
                "${LLAM_RUNTIME_SOURCES})"
            ),
            (
                "set(LLAM_PUBLIC_TEST_TARGETS "
                "test_public test_shared_load)"
            ),
            "set(LLAM_INTERNAL_TEST_TARGETS test_internal)",
            (
                "set(LLAM_TEST_HOOK_TEST_TARGETS "
                "test_windows_iocp_io)"
            ),
            "set(LLAM_LEIR_RESEARCH_TARGETS test_research)",
            "set(LLAM_LCWE_RESEARCH_TARGETS)",
            "set(LLAM_LCCF_RESEARCH_TARGETS)",
            "set(LLAM_SREM_RESEARCH_TARGETS)",
            "add_executable(test_public tests/test_public.c)",
            "target_link_libraries(test_public PRIVATE llam_runtime)",
            "add_executable(test_internal tests/test_internal.c)",
            "target_link_libraries(test_internal PRIVATE llam_runtime)",
            "add_executable(test_shared_load tests/test_shared_load.c)",
            (
                "target_link_libraries(test_shared_load "
                "PRIVATE ${CMAKE_DL_LIBS})"
            ),
            (
                "add_executable(test_windows_iocp_io "
                "tests/test_windows_iocp_io.c)"
            ),
            (
                "target_link_libraries(test_windows_iocp_io "
                "PRIVATE llam_runtime_testhooks)"
            ),
            "if(LLAM_BUILD_RESEARCH)",
            (
                "add_executable(test_research "
                "experiments/leir/test_research.c)"
            ),
            "target_link_libraries(test_research PRIVATE llam_runtime)",
            "endif()",
            "enable_testing()",
            "add_test(NAME test_public COMMAND test_public)",
            "add_test(NAME test_internal COMMAND test_internal)",
            'if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")',
            (
                "add_test(NAME test_shared_load "
                "COMMAND test_shared_load)"
            ),
            "endif()",
            'if(CMAKE_SYSTEM_NAME STREQUAL "Windows")',
            (
                "add_test(NAME test_windows_iocp_io "
                "COMMAND test_windows_iocp_io)"
            ),
            "endif()",
            "if(LLAM_BUILD_RESEARCH)",
            "add_test(NAME test_research COMMAND test_research)",
            "endif()",
            (
                "add_test(NAME build_manifest COMMAND ${Python3_EXECUTABLE} "
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_build_manifests.py "
                "--root ${CMAKE_CURRENT_SOURCE_DIR} --check)"
            ),
            "",
        ]
    )


def valid_runtime_header() -> str:
    return "\n".join(
        [
            "#define LLAM_VERSION_MAJOR 2U",
            "#define LLAM_VERSION_MINOR 2U",
            "#define LLAM_VERSION_PATCH 0U",
            "#define LLAM_ABI_VERSION_MAJOR 2U",
            "",
        ]
    )


def valid_package_script() -> str:
    return "\n".join(
        [
            'abi_major="${LLAM_ABI_MAJOR:-2}"',
            'library_version="${LLAM_VERSION:-2.2.0}"',
            'version="${GITHUB_REF_NAME:-v2.2.0}"',
            "",
        ]
    )


class Fixture:
    """A test-owned mini repository with independently derived literals."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self.sources = valid_sources_manifest()
        self.version = {
            "schema": "llam.build-manifest.v1",
            "version": "2.2.0",
            "abi_major": 2,
        }
        self.files = {
            "Makefile": valid_makefile(),
            "CMakeLists.txt": valid_cmakelists(),
            "include/llam/runtime.h": valid_runtime_header(),
            "src/core/base/abi.c": (
                '#define LLAM_VERSION_STRING_LITERAL "2.2.0"\n'
            ),
            "scripts/package_release.sh": valid_package_script(),
            ".github/workflows/linux.yml": (
                'env:\n  LLAM_BUILD_RESEARCH: "0"\n'
                '  LLAM_CI_VERSION: "2.2.0"\n'
                "jobs:\n  audit:\n    steps:\n"
                "      - name: Audit build manifests\n"
                "        run: python3 scripts/audit_build_manifests.py "
                "--root . --check\n"
            ),
        }
        for source in (
            "src/common.c",
            "src/linux.c",
            "src/linux_research.c",
            "src/runtime_io_segment_linux_internal.h",
            "tests/test_public.c",
            "tests/test_internal.c",
            "tests/test_shared_load.c",
            "tests/test_windows_iocp_io.c",
            "experiments/leir/test_research.c",
        ):
            self.files[source] = ""

    def write(self) -> Path:
        for relative, contents in self.files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents, encoding="utf-8")
        config = self.root / "config"
        config.mkdir(parents=True, exist_ok=True)
        (config / "llam-version.json").write_text(
            json.dumps(self.version, indent=2) + "\n",
            encoding="utf-8",
        )
        (config / "llam-sources.json").write_text(
            json.dumps(self.sources, indent=2) + "\n",
            encoding="utf-8",
        )
        return self.root


class BuildManifestAuditTests(unittest.TestCase):
    maxDiff = None

    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        self.fixture = Fixture(self.root)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def run_audit(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(AUDIT),
                "--root",
                str(self.root),
                "--check",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

    def test_accepts_clean_fixture(self) -> None:
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def test_rejects_missing_and_extra_members(self) -> None:
        self.fixture.sources["stable"]["common_sources"].append("src/other.c")
        self.fixture.files["src/other.c"] = ""
        self.fixture.files["src/extra.c"] = ""
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c)",
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c src/other.c src/extra.c)",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn("src/other.c: missing from Make", result.stderr)
        self.assertIn("src/extra.c: extra in CMake", result.stderr)

    def test_rejects_version_drift(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "LLAM_VERSION ?= 2.2.0",
            "LLAM_VERSION ?= 2.2.1",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn("Make version 2.2.1 != 2.2.0", result.stderr)

    def test_rejects_invalid_version_without_traceback(self) -> None:
        self.fixture.version["version"] = "2.bad.0"
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "llam-version.json.version: expected numeric major.minor.patch",
            result.stderr,
        )
        self.assertNotIn("Traceback", result.stderr)

    def test_rejects_package_github_ref_fallback_drift(self) -> None:
        self.fixture.files["scripts/package_release.sh"] = self.fixture.files[
            "scripts/package_release.sh"
        ].replace(
            "GITHUB_REF_NAME:-v2.2.0",
            "GITHUB_REF_NAME:-v2.2.9",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "package GITHUB_REF_NAME fallback 2.2.9 != 2.2.0",
            result.stderr,
        )

    def test_rejects_dead_package_fallback_before_effective_drift(self) -> None:
        self.fixture.files["scripts/package_release.sh"] = (
            'abi_major="${LLAM_ABI_MAJOR:-2}"\n'
            'library_version="${LLAM_VERSION:-2.2.0}"\n'
            ': "${GITHUB_REF_NAME:-v2.2.0}"\n'
            'version="v9.9.9"\n'
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "package version assignment does not use the canonical fallback",
            result.stderr,
        )

    def test_rejects_duplicate_array_members(self) -> None:
        self.fixture.sources["stable"]["common_sources"].append("src/common.c")
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "stable.common_sources: duplicate entry src/common.c",
            result.stderr,
        )

    def test_rejects_duplicate_json_keys(self) -> None:
        self.fixture.write()
        (self.root / "config/llam-version.json").write_text(
            (
                '{"schema":"llam.build-manifest.v1",'
                '"version":"2.2.0","version":"2.2.1","abi_major":2}\n'
            ),
            encoding="utf-8",
        )

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn("duplicate JSON key version", result.stderr)

    def test_rejects_duplicate_build_projection(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c)",
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c src/common.c)",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "src/common.c: duplicate in CMake",
            result.stderr,
        )

    def test_rejects_source_order_drift_positionally(self) -> None:
        self.fixture.sources["stable"]["common_sources"].append("src/other.c")
        self.fixture.files["src/other.c"] = ""
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "RUNTIME_COMMON_OBJS = $(OBJDIR)/src/common.o",
            (
                "RUNTIME_COMMON_OBJS = $(OBJDIR)/src/common.o "
                "$(OBJDIR)/src/other.o"
            ),
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c)",
            (
                "set(LLAM_RUNTIME_COMMON_SOURCES "
                "src/other.c src/common.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "stable common: CMake order drift at index 0: "
                "src/other.c != src/common.c"
            ),
            result.stderr,
        )

    def test_rejects_target_source_order_drift_positionally(self) -> None:
        self.fixture.sources["tests"]["public"][0]["sources"].append(
            "tests/public_helper.c"
        )
        self.fixture.files["tests/public_helper.c"] = ""
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "TEST_PUBLIC_OBJS = $(OBJDIR)/tests/test_public.o",
            (
                "TEST_PUBLIC_OBJS = $(OBJDIR)/tests/test_public.o "
                "$(OBJDIR)/tests/public_helper.o"
            ),
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_executable(test_public tests/test_public.c)",
            (
                "add_executable(test_public "
                "tests/public_helper.c tests/test_public.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "target test_public sources: CMake order drift at index 0: "
                "tests/public_helper.c != tests/test_public.c"
            ),
            result.stderr,
        )

    def test_rejects_manifest_leaf_symlink(self) -> None:
        self.fixture.write()
        common = self.root / "src/common.c"
        common.rename(self.root / "src/common-real.c")
        common.symlink_to("common-real.c")

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "src/common.c: manifest path traverses symlink at src/common.c",
            result.stderr,
        )

    def test_rejects_manifest_ancestor_symlink(self) -> None:
        self.fixture.write()
        source_dir = self.root / "src"
        source_dir.rename(self.root / "real-src")
        source_dir.symlink_to("real-src", target_is_directory=True)

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "src/common.c: manifest path traverses symlink at src",
            result.stderr,
        )

    def test_rejects_platform_mismatch(self) -> None:
        makefile = self.fixture.files["Makefile"]
        makefile = makefile.replace(
            "RUNTIME_LINUX_OBJS = $(OBJDIR)/src/linux.o",
            "RUNTIME_LINUX_OBJS =",
        )
        makefile = makefile.replace(
            "RUNTIME_WINDOWS_OBJS =",
            "RUNTIME_WINDOWS_OBJS = $(OBJDIR)/src/linux.o",
        )
        self.fixture.files["Makefile"] = makefile
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "src/linux.c: Make platform mismatch: windows != linux",
            result.stderr,
        )

    def test_rejects_direct_object_in_active_make_runtime_graph(self) -> None:
        self.fixture.files["src/direct.c"] = ""
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "RUNTIME_OBJS += $(RUNTIME_LINUX_OBJS)",
            (
                "RUNTIME_OBJS += $(RUNTIME_LINUX_OBJS)\n"
                "RUNTIME_OBJS += $(OBJDIR)/src/direct.o"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make linux-x86_64 research=0 runtime graph",
            result.stderr,
        )

    def test_rejects_direct_source_in_cmake_runtime_library(self) -> None:
        self.fixture.files["src/direct.c"] = ""
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})",
            (
                "add_library(llam_runtime STATIC "
                "${LLAM_RUNTIME_SOURCES} src/direct.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake llam_runtime sources",
            result.stderr,
        )

    def test_rejects_cmake_target_sources_runtime_injection(self) -> None:
        self.fixture.files["src/direct.c"] = ""
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})",
            (
                "add_library(llam_runtime STATIC "
                "${LLAM_RUNTIME_SOURCES})\n"
                "target_sources(llam_runtime PRIVATE src/direct.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake llam_runtime sources",
            result.stderr,
        )

    def test_rejects_cmake_target_sources_test_injection(self) -> None:
        self.fixture.files["tests/audit_extra.c"] = ""
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_executable(test_public tests/test_public.c)",
            (
                "add_executable(test_public tests/test_public.c)\n"
                "target_sources(test_public PRIVATE tests/audit_extra.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: CMake linux-x86_64 research=0 sources",
            result.stderr,
        )

    def test_rejects_omitted_make_common_group_consumption(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)",
            "RUNTIME_OBJS =",
            1,
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make linux-x86_64 research=0 runtime graph",
            result.stderr,
        )

    def test_rejects_omitted_make_platform_group_consumption(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "RUNTIME_OBJS += $(RUNTIME_LINUX_OBJS)",
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make linux-x86_64 research=0 runtime graph",
            result.stderr,
        )

    def test_rejects_omitted_cmake_research_group_consumption(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            (
                "list(APPEND LLAM_RUNTIME_SOURCES "
                "${LLAM_RESEARCH_RUNTIME_LINUX_SOURCES})"
            ),
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake linux-x86_64 research=1 runtime graph",
            result.stderr,
        )

    def test_rejects_conditional_make_runtime_reassignment(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "SHARED_RUNTIME_OBJS =",
            (
                "ifeq ($(LLAM_BUILD_RESEARCH),1)\n"
                "RUNTIME_OBJS = $(RUNTIME_COMMON_OBJS)\n"
                "endif\n"
                "SHARED_RUNTIME_OBJS ="
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make linux-x86_64 research=1 runtime graph",
            result.stderr,
        )

    def test_rejects_override_mutation_of_audited_make_state(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "RUNTIME_COMMON_OBJS = $(OBJDIR)/src/common.o",
            (
                "RUNTIME_COMMON_OBJS = $(OBJDIR)/src/common.o\n"
                "override RUNTIME_COMMON_OBJS := "
                "$(filter-out $(OBJDIR)/src/common.o,"
                "$(RUNTIME_COMMON_OBJS))"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make audited variable RUNTIME_COMMON_OBJS uses unsupported",
            result.stderr,
        )

    def test_rejects_unsupported_condition_on_audited_make_assignment(
        self,
    ) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "SHARED_RUNTIME_OBJS =",
            (
                "ifdef OPTIONAL_RUNTIME_MUTATION\n"
                "RUNTIME_OBJS += $(RUNTIME_COMMON_OBJS)\n"
                "endif\n"
                "SHARED_RUNTIME_OBJS ="
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make audited build state is guarded by an unsupported condition",
            result.stderr,
        )

    def test_rejects_make_metaprogramming_of_audited_state(self) -> None:
        mutations = (
            "export RUNTIME_COMMON_OBJS := $(OBJDIR)/src/common.o",
            "unexport RUNTIME_COMMON_OBJS",
            "define RUNTIME_COMMON_OBJS\n$(OBJDIR)/src/common.o\nendef",
            (
                "$(eval RUNTIME_COMMON_OBJS := "
                "$(OBJDIR)/src/common.o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(call identity,"
                "$(OBJDIR)/src/common.o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(foreach item,common,"
                "$(OBJDIR)/src/$(item).o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(addprefix $(OBJDIR)/src/,"
                "common.o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(addsuffix .o,"
                "$(OBJDIR)/src/common)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(filter %.o,"
                "$(OBJDIR)/src/common.o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(filter-out other.o,"
                "$(OBJDIR)/src/common.o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(subst other,common,"
                "$(OBJDIR)/src/other.o)"
            ),
            (
                "RUNTIME_COMMON_OBJS := $(patsubst %.c,"
                "$(OBJDIR)/%.o,src/common.c)"
            ),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.fixture = Fixture(self.root)
                self.fixture.files["Makefile"] += mutation + "\n"
                self.fixture.write()

                result = self.run_audit()

                self.assertEqual(result.returncode, 1, mutation)
                self.assertIn("unsupported", result.stderr)

    def test_rejects_cmake_list_removal_from_audited_state(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c)",
            (
                "set(LLAM_RUNTIME_COMMON_SOURCES src/common.c)\n"
                "list(REMOVE_ITEM LLAM_RUNTIME_COMMON_SOURCES src/common.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake audited variable LLAM_RUNTIME_COMMON_SOURCES uses "
            "unsupported list(REMOVE_ITEM)",
            result.stderr,
        )

    def test_rejects_other_cmake_list_mutators_of_audited_state(self) -> None:
        mutations = (
            "unset(LLAM_RUNTIME_COMMON_SOURCES)",
            "list(REMOVE_AT LLAM_RUNTIME_COMMON_SOURCES 0)",
            (
                "list(FILTER LLAM_RUNTIME_COMMON_SOURCES "
                "EXCLUDE REGEX common)"
            ),
            (
                "list(INSERT LLAM_RUNTIME_COMMON_SOURCES 0 "
                "src/common.c)"
            ),
            (
                "list(PREPEND LLAM_RUNTIME_COMMON_SOURCES "
                "src/common.c)"
            ),
            (
                "list(TRANSFORM LLAM_RUNTIME_COMMON_SOURCES "
                "REPLACE common other)"
            ),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.fixture = Fixture(self.root)
                self.fixture.files["CMakeLists.txt"] += mutation + "\n"
                self.fixture.write()

                result = self.run_audit()

                self.assertEqual(result.returncode, 1)
                self.assertIn("uses unsupported", result.stderr)

    def test_rejects_cmake_property_and_object_indirection(self) -> None:
        mutations = (
            (
                "set_property(TARGET test_public APPEND "
                "PROPERTY SOURCES tests/audit_extra.c)"
            ),
            (
                "set_target_properties(test_public PROPERTIES "
                "SOURCES tests/audit_extra.c)"
            ),
            (
                "set_source_files_properties(tests/test_public.c "
                "PROPERTIES GENERATED TRUE)"
            ),
            (
                "add_library(audit_objects OBJECT tests/audit_extra.c)\n"
                "target_sources(test_public PRIVATE "
                "$<TARGET_OBJECTS:audit_objects>)"
            ),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.fixture = Fixture(self.root)
                self.fixture.files["tests/audit_extra.c"] = ""
                self.fixture.files["CMakeLists.txt"] += mutation + "\n"
                self.fixture.write()

                result = self.run_audit()

                self.assertEqual(result.returncode, 1)
                self.assertIn("unsupported", result.stderr)

    def test_accepts_unrelated_and_quoted_unsupported_syntax(self) -> None:
        self.fixture.files["Makefile"] += (
            "# override RUNTIME_COMMON_OBJS := hidden\n"
            "DEMO_OBJS := $(foreach item,demo,$(OBJDIR)/$(item).o)\n"
        )
        self.fixture.files["CMakeLists.txt"] += (
            "# list(REMOVE_ITEM LLAM_RUNTIME_COMMON_SOURCES src/common.c)\n"
            'message("target_sources(test_public PRIVATE hidden.c)")\n'
            "set(UNRELATED_SOURCES tools/other.c)\n"
            "list(REMOVE_ITEM UNRELATED_SOURCES tools/other.c)\n"
            "set_source_files_properties(tools/other.c "
            "PROPERTIES GENERATED TRUE)\n"
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_make_runtime_drift_in_supported_arch_alias(self) -> None:
        self.fixture.files["src/direct.c"] = ""
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "SHARED_RUNTIME_OBJS =",
            (
                "ifeq ($(HOST_PLATFORM),bsd)\n"
                "ifeq ($(UNAME_M),amd64)\n"
                "RUNTIME_OBJS += $(OBJDIR)/src/direct.o\n"
                "endif\n"
                "endif\n"
                "SHARED_RUNTIME_OBJS ="
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make bsd-amd64 research=0 runtime graph",
            result.stderr,
        )

    def test_rejects_cmake_runtime_drift_in_supported_arch_alias(self) -> None:
        self.fixture.files["src/direct.c"] = ""
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})",
            (
                'if(LLAM_TARGET_PROCESSOR STREQUAL "amd64")\n'
                "list(APPEND LLAM_RUNTIME_SOURCES src/direct.c)\n"
                "endif()\n"
                "add_library(llam_runtime STATIC ${LLAM_RUNTIME_SOURCES})"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake linux-amd64 research=0 runtime graph",
            result.stderr,
        )

    def test_rejects_make_static_recipe_runtime_divergence(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(AR) rcs $@ $(RUNTIME_OBJS)",
            "$(AR) rcs $@ $(RUNTIME_COMMON_OBJS)",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "libllam_runtime.a: Make link recipe inputs",
            result.stderr,
        )

    def test_rejects_literal_object_in_make_static_recipe(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(AR) rcs $@ $(RUNTIME_OBJS)",
            (
                "$(AR) rcs $@ $(RUNTIME_OBJS) "
                "$(OBJDIR)/src/common.o"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "libllam_runtime.a: Make link recipe has additional input",
            result.stderr,
        )

    def test_rejects_unknown_input_in_make_static_recipe(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(AR) rcs $@ $(RUNTIME_OBJS)",
            "$(AR) rcs $@ $(RUNTIME_OBJS) $(MYSTERY_INPUT)",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "libllam_runtime.a: Make link recipe has unknown expansion "
            "$(MYSTERY_INPUT)",
            result.stderr,
        )

    def test_rejects_cmake_shared_runtime_divergence(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            (
                "add_library(llam_runtime_shared SHARED "
                "${LLAM_RUNTIME_SOURCES})"
            ),
            (
                "add_library(llam_runtime_shared SHARED "
                "${LLAM_RUNTIME_COMMON_SOURCES})"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake llam_runtime_shared sources",
            result.stderr,
        )

    def test_rejects_cmake_testhook_runtime_divergence(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            (
                "add_library(llam_runtime_testhooks STATIC "
                "${LLAM_RUNTIME_SOURCES})"
            ),
            (
                "add_library(llam_runtime_testhooks STATIC "
                "${LLAM_RUNTIME_COMMON_SOURCES})"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake llam_runtime_testhooks sources",
            result.stderr,
        )

    def test_rejects_inactive_cmake_runtime_library(self) -> None:
        declaration = (
            "add_library(llam_runtime_shared SHARED "
            "${LLAM_RUNTIME_SOURCES})"
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            declaration,
            f"if(FALSE)\n{declaration}\nendif()",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake llam_runtime_shared is missing for linux-x86_64",
            result.stderr,
        )

    def test_rejects_make_shared_recipe_runtime_divergence(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            (
                "\t$(CC) -shared -o $@ $(SHARED_RUNTIME_OBJS) "
                "$(LDLIBS)"
            ),
            "\t$(CC) -shared -o $@ $(RUNTIME_OBJS) $(LDLIBS)",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make shared runtime link recipe inputs",
            result.stderr,
        )

    def test_rejects_target_platform_mismatch(self) -> None:
        self.fixture.sources["tests"]["test_hook"][0]["platforms"] = ["all"]
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_windows_iocp_io: CTest platforms ['windows'] != ['all']",
            result.stderr,
        )

    def test_rejects_known_cmake_test_condition_platform_drift(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_test(NAME test_public COMMAND test_public)",
            (
                'if(CMAKE_SYSTEM_NAME STREQUAL "Linux")\n'
                "add_test(NAME test_public COMMAND test_public)\n"
                "endif()"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: CTest platforms ['linux'] != ['all']",
            result.stderr,
        )

    def test_rejects_research_target_platform_narrowing(self) -> None:
        self.fixture.sources["research"]["targets"][0]["platforms"] = [
            "windows"
        ]
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_research: CTest platforms ['all'] != ['windows']",
            result.stderr,
        )

    def test_rejects_stable_cmake_target_narrowed_to_x86_aliases(self) -> None:
        declaration = (
            "add_executable(test_public tests/test_public.c)\n"
            "target_link_libraries(test_public PRIVATE llam_runtime)"
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            declaration,
            (
                'if(LLAM_TARGET_PROCESSOR MATCHES "^(x86_64|amd64)$")\n'
                f"{declaration}\n"
                "endif()"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: CMake linux-aarch64 research=0 target is missing",
            result.stderr,
        )

    def test_rejects_configured_cmake_target_source_variable_drift(
        self,
    ) -> None:
        self.fixture.files["tests/audit_extra.c"] = ""
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_executable(test_public tests/test_public.c)",
            (
                "set(PUBLIC_SOURCES tests/test_public.c)\n"
                'if(LLAM_TARGET_PROCESSOR MATCHES "^(aarch64|arm64)$")\n'
                "set(PUBLIC_SOURCES tests/audit_extra.c)\n"
                "endif()\n"
                "add_executable(test_public ${PUBLIC_SOURCES})"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: CMake linux-aarch64 research=0 sources",
            result.stderr,
        )

    def test_rejects_unsupported_condition_on_cmake_target_source_variable(
        self,
    ) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_executable(test_public tests/test_public.c)",
            (
                "set(PUBLIC_SOURCES tests/test_public.c)\n"
                "if(UNMODELED_SOURCE_TOGGLE)\n"
                "set(PUBLIC_SOURCES)\n"
                "endif()\n"
                "add_executable(test_public ${PUBLIC_SOURCES})"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake audited variable mutation is guarded by an unsupported "
            "condition",
            result.stderr,
        )

    def test_rejects_stable_make_rule_missing_only_in_research_mode(self) -> None:
        rule = (
            "test_public: $(RUNTIME_OBJS) $(TEST_PUBLIC_OBJS)\n"
            "\t$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) "
            "$(TEST_PUBLIC_OBJS) $(LDLIBS)"
        )
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            rule,
            f"ifeq ($(LLAM_BUILD_RESEARCH),0)\n{rule}\nendif",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: Make linux-x86_64 research=1 target is missing",
            result.stderr,
        )

    def test_rejects_stable_ctest_missing_only_in_research_mode(self) -> None:
        registration = "add_test(NAME test_public COMMAND test_public)"
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            registration,
            (
                "if(NOT LLAM_BUILD_RESEARCH)\n"
                f"{registration}\n"
                "endif()"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: CTest linux-x86_64 research=1 registration "
            "is missing",
            result.stderr,
        )

    def test_rejects_unsupported_cmake_test_condition(self) -> None:
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_test(NAME test_public COMMAND test_public)",
            (
                "if(UNMODELED_PLATFORM_TOGGLE)\n"
                "add_test(NAME test_public COMMAND test_public)\n"
                "endif()"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake add_test is guarded by an unsupported condition",
            result.stderr,
        )

    def test_accepts_platform_specific_link_dependencies(self) -> None:
        self.fixture.sources["research"]["targets"][0][
            "link_dependencies"
        ] = {
            "linux": ["llam_runtime"],
            "darwin": ["llam_runtime"],
            "bsd": ["llam_runtime"],
            "windows": [],
        }
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "target_link_libraries(test_research PRIVATE llam_runtime)",
            (
                'if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")\n'
                "target_link_libraries(test_research PRIVATE llam_runtime)\n"
                "endif()"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_per_platform_cmake_link_drift(self) -> None:
        self.fixture.sources["research"]["targets"][0][
            "link_dependencies"
        ] = {
            "linux": ["llam_runtime"],
            "darwin": ["llam_runtime"],
            "bsd": ["llam_runtime"],
            "windows": [],
        }
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "test_research: CMake windows link dependencies "
                "['llam_runtime'] != []"
            ),
            result.stderr,
        )

    def test_rejects_test_classification_mismatch(self) -> None:
        target = self.fixture.sources["tests"]["internal"].pop()
        self.fixture.sources["tests"]["public"].append(target)
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_internal: missing public target from Make",
            result.stderr,
        )
        self.assertIn(
            "test_internal: extra internal target in CMake",
            result.stderr,
        )

    def test_rejects_research_family_mismatch(self) -> None:
        self.fixture.sources["research"]["targets"][0]["family"] = "lcwe"
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_research: extra leir target in Make",
            result.stderr,
        )
        self.assertIn(
            "test_research: missing lcwe target from CMake",
            result.stderr,
        )

    def test_rejects_wrong_link_dependency(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "test_research: $(RUNTIME_OBJS) $(RESEARCH_TEST_RESEARCH_OBJS)",
            "test_research: $(RESEARCH_TEST_RESEARCH_OBJS)",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "test_research: Make prerequisite link dependencies "
                "[] != ['llam_runtime']"
            ),
            result.stderr,
        )

    def test_rejects_make_recipe_omitting_runtime_objects(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            (
                "\t$(CC) $(CFLAGS) -o $@ $(RUNTIME_OBJS) "
                "$(TEST_PUBLIC_OBJS) $(LDLIBS)"
            ),
            (
                "\t$(CC) $(CFLAGS) -o $@ "
                "$(TEST_PUBLIC_OBJS) $(LDLIBS)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: Make recipe link dependencies",
            result.stderr,
        )

    def test_rejects_make_recipe_only_object(self) -> None:
        self.fixture.files["tests/recipe_only.c"] = ""
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(TEST_PUBLIC_OBJS) $(LDLIBS)",
            "$(TEST_PUBLIC_OBJS) $(OBJDIR)/tests/recipe_only.o $(LDLIBS)",
            1,
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "tests/recipe_only.c: extra in Make target test_public recipe",
            result.stderr,
        )

    def test_rejects_plain_object_only_in_make_target_inputs(self) -> None:
        for location in ("prerequisite", "recipe"):
            with self.subTest(location=location):
                self.fixture = Fixture(self.root)
                if location == "prerequisite":
                    self.fixture.files["Makefile"] = self.fixture.files[
                        "Makefile"
                    ].replace(
                        "test_public: $(RUNTIME_OBJS) $(TEST_PUBLIC_OBJS)",
                        (
                            "test_public: $(RUNTIME_OBJS) "
                            "$(TEST_PUBLIC_OBJS) rogue.o"
                        ),
                    )
                else:
                    self.fixture.files["Makefile"] = self.fixture.files[
                        "Makefile"
                    ].replace(
                        "$(TEST_PUBLIC_OBJS) $(LDLIBS)",
                        "$(TEST_PUBLIC_OBJS) rogue.o $(LDLIBS)",
                        1,
                    )
                self.fixture.write()

                result = self.run_audit()

                self.assertEqual(result.returncode, 1)
                self.assertIn("additional", result.stderr)

    def test_rejects_make_recipe_only_library_expansion(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(TEST_PUBLIC_OBJS) $(LDLIBS)",
            "$(TEST_PUBLIC_OBJS) $(LDLIBS) $(SERVER_FLOOD_LDLIBS)",
            1,
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "test_public: Make recipe link dependencies "
                "['Threads::Threads', 'llam_runtime'] "
                "!= ['llam_runtime']"
            ),
            result.stderr,
        )

    def test_rejects_unknown_make_recipe_expansion(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(TEST_PUBLIC_OBJS) $(LDLIBS)",
            "$(TEST_PUBLIC_OBJS) $(MYSTERY_INPUT) $(LDLIBS)",
            1,
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_public: Make recipe has unknown expansion $(MYSTERY_INPUT)",
            result.stderr,
        )

    def test_rejects_unmanifested_targets(self) -> None:
        self.fixture.files["tests/test_untracked.c"] = ""
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "TEST_HOOK_TEST_TARGETS = test_windows_iocp_io",
            "TEST_HOOK_TEST_TARGETS = test_windows_iocp_io test_untracked",
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            "add_executable(test_public tests/test_public.c)",
            (
                "add_executable(test_public tests/test_public.c)\n"
                "add_executable(test_untracked tests/test_untracked.c)"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_untracked: extra stable target in Make",
            result.stderr,
        )
        self.assertIn(
            "test_untracked: extra stable target in CMake",
            result.stderr,
        )

    def test_rejects_malformed_json(self) -> None:
        self.fixture.write()
        (self.root / "config/llam-sources.json").write_text(
            '{"schema": ',
            encoding="utf-8",
        )

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn("llam-sources.json: malformed JSON", result.stderr)

    def test_filesystem_errors_are_deterministic(self) -> None:
        self.fixture.write()
        version_path = self.root / "config/llam-version.json"
        version_path.unlink()
        version_path.mkdir()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "config/llam-version.json: cannot read UTF-8 text: "
                "IsADirectoryError"
            ),
            result.stderr,
        )
        self.assertNotIn(str(self.root), result.stderr)

    def test_invalid_roots_fail_without_traceback_or_path_leak(self) -> None:
        loop = self.root / "loop"
        loop.symlink_to("loop")
        non_directory = self.root / "not-a-directory"
        non_directory.write_text("", encoding="utf-8")
        for invalid_root in (loop, non_directory):
            with self.subTest(invalid_root=invalid_root.name):
                result = subprocess.run(
                    [
                        sys.executable,
                        str(AUDIT),
                        "--root",
                        str(invalid_root),
                        "--check",
                    ],
                    check=False,
                    text=True,
                    capture_output=True,
                )

                self.assertEqual(result.returncode, 1)
                self.assertNotIn("Traceback", result.stderr)
                self.assertNotIn(str(self.root), result.stderr)
                self.assertEqual(
                    result.stderr.splitlines(),
                    sorted(result.stderr.splitlines()),
                )

    def test_inaccessible_root_fails_with_sorted_diagnostics(self) -> None:
        inaccessible = self.root / "inaccessible"
        inaccessible.mkdir()
        inaccessible.chmod(0)
        try:
            result = subprocess.run(
                [
                    sys.executable,
                    str(AUDIT),
                    "--root",
                    str(inaccessible),
                    "--check",
                ],
                check=False,
                text=True,
                capture_output=True,
            )
        finally:
            inaccessible.chmod(0o700)

        self.assertEqual(result.returncode, 1)
        self.assertNotIn("Traceback", result.stderr)
        self.assertNotIn(str(self.root), result.stderr)
        self.assertEqual(
            result.stderr.splitlines(),
            sorted(result.stderr.splitlines()),
        )

    def test_rejects_unknown_fields(self) -> None:
        self.fixture.sources["stable"]["mystery_sources"] = []
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn("stable: unknown field mystery_sources", result.stderr)

    def test_rejects_test_object_missing_from_make_build_objects(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "\t$(TEST_WINDOWS_IOCP_IO_OBJS)",
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "tests/test_windows_iocp_io.c: missing from Make BUILD_OBJS",
            result.stderr,
        )

    def test_rejects_unsigned_make_build_objects(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "$(BUILD_OBJS): $(BUILD_SIGNATURE)",
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make BUILD_OBJS are not covered by BUILD_SIGNATURE",
            result.stderr,
        )

    def test_rejects_missing_research_private_header_provenance(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "RUNTIME_PRIV_HDRS += $(RESEARCH_PRIVATE_HDRS)",
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make RUNTIME_PRIV_HDRS does not include RESEARCH_PRIVATE_HDRS",
            result.stderr,
        )

    def test_rejects_missing_make_dependency_wiring(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "DEPFLAGS ?= -MMD -MP",
            "DEPFLAGS ?= -MMD",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make DEPFLAGS ['-MMD'] != ['-MMD', '-MP']",
            result.stderr,
        )

    def test_rejects_reordered_compile_recipe_without_depflags(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            (
                "\t$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) "
                "-c -o $@ $<"
            ),
            "\t$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ -c $<",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make ordinary compile recipe omits DEPFLAGS",
            result.stderr,
        )

    def assert_depfile_category_rejected(self, token: str) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            f" {token}",
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            f"Make ALL_DEPFILES omits {token}",
            result.stderr,
        )

    def test_rejects_missing_ordinary_depfile_category(self) -> None:
        self.assert_depfile_category_rejected("$(BUILD_OBJS:.o=.d)")

    def test_rejects_missing_research_depfile_category(self) -> None:
        self.assert_depfile_category_rejected("$(RESEARCH_OBJS:.o=.d)")

    def test_rejects_missing_shared_depfile_category(self) -> None:
        self.assert_depfile_category_rejected("$(SHARED_RUNTIME_OBJS:.o=.d)")

    def test_rejects_missing_testhook_depfile_category(self) -> None:
        self.assert_depfile_category_rejected(
            "$(TESTHOOK_RUNTIME_OVERRIDE_OBJS:.o=.d)"
        )

    def assert_signature_depflags_rejected(self, signature: str) -> None:
        marker = (
            f"{signature}:\n"
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > $@"
        )
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            marker,
            f"{signature}:\n\t@true",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            f"Make {signature} does not record DEPFLAGS",
            result.stderr,
        )

    def test_rejects_build_signature_without_depflags(self) -> None:
        self.assert_signature_depflags_rejected("$(BUILD_SIGNATURE)")

    def test_rejects_shared_signature_without_depflags(self) -> None:
        self.assert_signature_depflags_rejected("$(SHARED_BUILD_SIGNATURE)")

    def test_rejects_testhook_signature_without_depflags(self) -> None:
        self.assert_signature_depflags_rejected("$(TESTHOOK_BUILD_SIGNATURE)")

    def test_rejects_decorative_depflags_in_signature_recipe(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > $@",
            "\t: '$(DEPFLAGS)'",
            1,
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make $(BUILD_SIGNATURE) does not emit DEPFLAGS",
            result.stderr,
        )

    def test_rejects_depflags_printf_redirected_away_from_signature(
        self,
    ) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > $@",
            "\tprintf 'DEPFLAGS=%s\\n' '$(DEPFLAGS)' > /dev/null",
            1,
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make $(BUILD_SIGNATURE) does not emit DEPFLAGS",
            result.stderr,
        )

    def test_rejects_missing_build_test_wiring(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "test check: audit-build-manifests",
            "test check:",
        ).replace(
            "test: audit-build-manifests",
            "test:",
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace("NAME build_manifest", "NAME other_test")
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test target does not reach audit-build-manifests",
            result.stderr,
        )
        self.assertIn("CMake build_manifest test is inactive", result.stderr)

    def test_rejects_comment_only_make_audit_recipe(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            (
                "\tpython3 scripts/audit_build_manifests.py "
                "--root . --check"
            ),
            (
                "\t@true # python3 scripts/audit_build_manifests.py "
                "--root . --check"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make audit-build-manifests recipe is not exact",
            result.stderr,
        )

    def test_rejects_disabled_cmake_manifest_test(self) -> None:
        registration = (
            "add_test(NAME build_manifest COMMAND ${Python3_EXECUTABLE} "
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/audit_build_manifests.py "
            "--root ${CMAKE_CURRENT_SOURCE_DIR} --check)"
        )
        self.fixture.files["CMakeLists.txt"] = self.fixture.files[
            "CMakeLists.txt"
        ].replace(
            registration,
            f"if(FALSE)\n{registration}\nendif()",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "CMake build_manifest test is inactive for research=0",
            result.stderr,
        )
        self.assertIn(
            "CMake build_manifest test is inactive for research=1",
            result.stderr,
        )

    def test_rejects_unsupported_make_condition_hiding_audit_rule(self) -> None:
        rule = (
            "audit-build-manifests:\n"
            "\tpython3 scripts/audit_build_manifests.py --root . --check"
        )
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            rule,
            f"ifdef NEVER\n{rule}\nendif",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Make audit enforcement is guarded by an unsupported condition",
            result.stderr,
        )

    def test_rejects_manifest_test_disabled_by_cmake_property(self) -> None:
        mutations = (
            (
                "set_tests_properties(build_manifest "
                "PROPERTIES DISABLED TRUE)"
            ),
            (
                "set_property(TEST build_manifest "
                "PROPERTY DISABLED TRUE)"
            ),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                self.fixture = Fixture(self.root)
                self.fixture.files["CMakeLists.txt"] += mutation + "\n"
                self.fixture.write()

                result = self.run_audit()

                self.assertEqual(result.returncode, 1)
                self.assertIn(
                    "CMake build_manifest test is disabled",
                    result.stderr,
                )

    def test_rejects_false_condition_on_linux_workflow_audit_step(self) -> None:
        self.fixture.files[".github/workflows/linux.yml"] = self.fixture.files[
            ".github/workflows/linux.yml"
        ].replace(
            "      - name: Audit build manifests\n",
            (
                "      - name: Audit build manifests\n"
                "        if: ${{ false }}\n"
            ),
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "Linux CI build-manifest audit step is disabled",
            result.stderr,
        )

    def test_rejects_missing_linux_workflow_audit_step(self) -> None:
        self.fixture.files[".github/workflows/linux.yml"] = (
            'env:\n  LLAM_BUILD_RESEARCH: "0"\n'
            '  LLAM_CI_VERSION: "2.2.0"\n'
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn("Linux CI build-manifest audit step is missing", result.stderr)

    def test_rejects_one_make_platform_branch_missing_audit(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "test check: audit-build-manifests",
            "test check:",
        )
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            (
                "Make windows-gnu-x86_64 research=0 test target "
                "does not reach audit-build-manifests"
            ),
            result.stderr,
        )

    def test_diagnostics_are_sorted(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
            "LLAM_VERSION ?= 2.2.0",
            "LLAM_VERSION ?= 9.9.9",
        ).replace(
            "RUNTIME_LINUX_OBJS = $(OBJDIR)/src/linux.o",
            "",
        )
        self.fixture.write()

        result = self.run_audit()

        diagnostics = [
            line.removeprefix("audit-build-manifests: ")
            for line in result.stderr.splitlines()
            if line.startswith("audit-build-manifests: ")
        ]
        self.assertGreaterEqual(len(diagnostics), 2, result.stderr)
        self.assertEqual(diagnostics, sorted(diagnostics))


if __name__ == "__main__":
    unittest.main()
