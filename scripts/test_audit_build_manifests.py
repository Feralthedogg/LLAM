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
                }
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
            "TEST_PUBLIC_OBJS = $(OBJDIR)/tests/test_public.o",
            "TEST_INTERNAL_OBJS = $(OBJDIR)/tests/test_internal.o",
            (
                "TEST_WINDOWS_IOCP_IO_OBJS = "
                "$(OBJDIR)/tests/test_windows_iocp_io.o"
            ),
            (
                "RESEARCH_TEST_RESEARCH_OBJS = "
                "$(OBJDIR)/experiments/leir/test_research.o"
            ),
            "PUBLIC_TEST_TARGETS = test_public",
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
            "\t$(TEST_INTERNAL_OBJS) \\",
            "\t$(TEST_WINDOWS_IOCP_IO_OBJS)",
            "RESEARCH_OBJS = \\",
            "\t$(RESEARCH_RUNTIME_LINUX_OBJS) \\",
            "\t$(RESEARCH_TEST_RESEARCH_OBJS)",
            "$(BUILD_OBJS): $(BUILD_SIGNATURE)",
            "$(RESEARCH_OBJS): $(BUILD_SIGNATURE)",
            "ALL_DEPFILES = $(BUILD_OBJS:.o=.d) $(RESEARCH_OBJS:.o=.d)",
            "-include $(ALL_DEPFILES)",
            "$(OBJDIR)/%.o: %.c",
            "\t$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<",
            "test_public: $(RUNTIME_OBJS) $(TEST_PUBLIC_OBJS)",
            "test_internal: $(RUNTIME_OBJS) $(TEST_INTERNAL_OBJS)",
            (
                "test_windows_iocp_io: "
                "$(RUNTIME_TESTHOOK_OBJS) $(TEST_WINDOWS_IOCP_IO_OBJS)"
            ),
            (
                "test_research: "
                "$(RUNTIME_OBJS) $(RESEARCH_TEST_RESEARCH_OBJS)"
            ),
            "audit-build-manifests:",
            (
                "\tpython3 scripts/audit_build_manifests.py "
                "--root . --check"
            ),
            "test: audit-build-manifests",
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
            "set(LLAM_PUBLIC_TEST_TARGETS test_public)",
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
            (
                "add_executable(test_windows_iocp_io "
                "tests/test_windows_iocp_io.c)"
            ),
            (
                "target_link_libraries(test_windows_iocp_io "
                "PRIVATE llam_runtime_testhooks)"
            ),
            (
                "add_executable(test_research "
                "experiments/leir/test_research.c)"
            ),
            "target_link_libraries(test_research PRIVATE llam_runtime)",
            "enable_testing()",
            "add_test(NAME test_public COMMAND test_public)",
            "add_test(NAME test_internal COMMAND test_internal)",
            'if(CMAKE_SYSTEM_NAME STREQUAL "Windows")',
            (
                "add_test(NAME test_windows_iocp_io "
                "COMMAND test_windows_iocp_io)"
            ),
            "endif()",
            (
                "add_test(NAME build_manifest COMMAND python3 "
                "scripts/audit_build_manifests.py --root . --check)"
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
            ),
        }
        for source in (
            "src/common.c",
            "src/linux.c",
            "src/linux_research.c",
            "src/runtime_io_segment_linux_internal.h",
            "tests/test_public.c",
            "tests/test_internal.c",
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

    def test_rejects_target_platform_mismatch(self) -> None:
        self.fixture.sources["tests"]["test_hook"][0]["platforms"] = ["all"]
        self.fixture.write()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "test_windows_iocp_io: CTest platforms ['windows'] != ['all']",
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
            "test_research: Make link dependencies [] != ['llam_runtime']",
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

    def test_rejects_missing_build_test_wiring(self) -> None:
        self.fixture.files["Makefile"] = self.fixture.files["Makefile"].replace(
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
            "Make test target does not depend on audit-build-manifests",
            result.stderr,
        )
        self.assertIn("CMake build_manifest test is missing", result.stderr)

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
