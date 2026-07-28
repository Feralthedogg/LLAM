#!/usr/bin/env python3
"""Audit LLAM's canonical version and source manifests without evaluation."""

from __future__ import annotations

import argparse
from collections.abc import Iterable
import json
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any


SCHEMA = "llam.build-manifest.v1"
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
TEST_CLASSES = ("public", "internal", "test_hook")
RESEARCH_FAMILIES = ("leir", "lcwe", "lccf", "srem")
TARGET_FIELDS = {
    "name",
    "platforms",
    "sources",
    "link_dependencies",
}
RESEARCH_TARGET_FIELDS = TARGET_FIELDS | {"family"}
MAKE_LINK_VARIABLES = {
    "RUNTIME_OBJS": "llam_runtime",
    "RUNTIME_TESTHOOK_OBJS": "llam_runtime_testhooks",
    "SERVER_FLOOD_LDLIBS": "Threads::Threads",
}


class DuplicateKeyError(ValueError):
    """Raised when JSON repeats an object key."""


class Audit:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.diagnostics: set[str] = set()

    def error(self, message: str) -> None:
        self.diagnostics.add(message)

    def read_text(self, relative: str) -> str | None:
        path = self.root / relative
        try:
            return path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            self.error(f"{relative}: cannot read UTF-8 text: {exc}")
            return None

    def load_json(self, relative: str) -> dict[str, Any] | None:
        text = self.read_text(relative)
        if text is None:
            return None

        def reject_duplicates(
            pairs: list[tuple[str, Any]],
        ) -> dict[str, Any]:
            result: dict[str, Any] = {}
            for key, value in pairs:
                if key in result:
                    raise DuplicateKeyError(f"duplicate JSON key {key}")
                result[key] = value
            return result

        try:
            value = json.loads(text, object_pairs_hook=reject_duplicates)
        except DuplicateKeyError as exc:
            self.error(f"{relative}: {exc}")
            return None
        except json.JSONDecodeError as exc:
            self.error(
                f"{relative}: malformed JSON at line {exc.lineno} "
                f"column {exc.colno}"
            )
            return None
        if not isinstance(value, dict):
            self.error(f"{relative}: top-level value must be an object")
            return None
        return value

    def require_object(
        self,
        value: Any,
        location: str,
        fields: set[str],
    ) -> dict[str, Any]:
        if not isinstance(value, dict):
            self.error(f"{location}: expected object")
            return {}
        for field in sorted(set(value) - fields):
            self.error(f"{location}: unknown field {field}")
        for field in sorted(fields - set(value)):
            self.error(f"{location}: missing field {field}")
        return value

    def require_string(
        self,
        value: Any,
        location: str,
    ) -> str | None:
        if not isinstance(value, str) or not value:
            self.error(f"{location}: expected non-empty string")
            return None
        return value

    def require_string_array(
        self,
        value: Any,
        location: str,
        *,
        paths: bool = False,
    ) -> list[str]:
        if not isinstance(value, list):
            self.error(f"{location}: expected array")
            return []
        result: list[str] = []
        seen: set[str] = set()
        for index, item in enumerate(value):
            string = self.require_string(item, f"{location}[{index}]")
            if string is None:
                continue
            if string in seen:
                self.error(f"{location}: duplicate entry {string}")
                continue
            seen.add(string)
            if paths and not valid_relative_path(string):
                self.error(f"{location}: invalid relative path {string}")
                continue
            result.append(string)
        return result

    def validate_version(self, value: dict[str, Any]) -> tuple[str, int] | None:
        manifest = self.require_object(
            value,
            "llam-version.json",
            {"schema", "version", "abi_major"},
        )
        if manifest.get("schema") != SCHEMA:
            self.error(
                "llam-version.json: schema "
                f"{manifest.get('schema')!r} != {SCHEMA!r}"
            )
        version = self.require_string(
            manifest.get("version"),
            "llam-version.json.version",
        )
        if version is not None and re.fullmatch(r"\d+\.\d+\.\d+", version) is None:
            self.error(
                "llam-version.json.version: expected numeric major.minor.patch"
            )
        abi_major = manifest.get("abi_major")
        if not isinstance(abi_major, int) or isinstance(abi_major, bool):
            self.error("llam-version.json.abi_major: expected integer")
            return None
        if abi_major < 0:
            self.error("llam-version.json.abi_major: expected non-negative integer")
        if version is None:
            return None
        return version, abi_major

    def validate_target(
        self,
        value: Any,
        location: str,
        *,
        research: bool,
    ) -> dict[str, Any]:
        fields = RESEARCH_TARGET_FIELDS if research else TARGET_FIELDS
        target = self.require_object(value, location, fields)
        name = self.require_string(target.get("name"), f"{location}.name")
        if name is not None and re.fullmatch(r"[A-Za-z0-9_]+", name) is None:
            self.error(f"{location}.name: invalid target name {name}")
        platforms = self.require_string_array(
            target.get("platforms"),
            f"{location}.platforms",
        )
        allowed_platforms = {"all", "linux", "darwin", "bsd", "windows"}
        for platform in platforms:
            if platform not in allowed_platforms:
                self.error(f"{location}.platforms: unknown platform {platform}")
        if not platforms:
            self.error(f"{location}.platforms: must not be empty")
        sources = self.require_string_array(
            target.get("sources"),
            f"{location}.sources",
            paths=True,
        )
        if not sources:
            self.error(f"{location}.sources: must not be empty")
        links = self.require_string_array(
            target.get("link_dependencies"),
            f"{location}.link_dependencies",
        )
        if research:
            family = self.require_string(
                target.get("family"),
                f"{location}.family",
            )
            if family is not None and family not in RESEARCH_FAMILIES:
                self.error(f"{location}.family: unknown research family {family}")
        return {
            "name": name or "",
            "platforms": platforms,
            "sources": sources,
            "link_dependencies": links,
            **({"family": target.get("family")} if research else {}),
        }

    def validate_sources(
        self,
        value: dict[str, Any],
    ) -> dict[str, Any]:
        manifest = self.require_object(
            value,
            "llam-sources.json",
            {"schema", "stable", "tests", "research"},
        )
        if manifest.get("schema") != SCHEMA:
            self.error(
                "llam-sources.json: schema "
                f"{manifest.get('schema')!r} != {SCHEMA!r}"
            )

        stable = self.require_object(
            manifest.get("stable"),
            "stable",
            {"common_sources", "platform_sources"},
        )
        common_sources = self.require_string_array(
            stable.get("common_sources"),
            "stable.common_sources",
            paths=True,
        )
        platforms = self.require_object(
            stable.get("platform_sources"),
            "stable.platform_sources",
            set(PLATFORM_GROUPS),
        )
        platform_sources = {
            group: self.require_string_array(
                platforms.get(group),
                f"stable.platform_sources.{group}",
                paths=True,
            )
            for group in PLATFORM_GROUPS
        }

        tests = self.require_object(
            manifest.get("tests"),
            "tests",
            set(TEST_CLASSES),
        )
        test_targets: dict[str, list[dict[str, Any]]] = {}
        target_names: set[str] = set()
        for test_class in TEST_CLASSES:
            values = tests.get(test_class)
            location = f"tests.{test_class}"
            if not isinstance(values, list):
                self.error(f"{location}: expected array")
                values = []
            targets = [
                self.validate_target(
                    item,
                    f"{location}[{index}]",
                    research=False,
                )
                for index, item in enumerate(values)
            ]
            for target in targets:
                name = target["name"]
                if name in target_names:
                    self.error(f"tests: duplicate target {name}")
                target_names.add(name)
            test_targets[test_class] = targets

        research = self.require_object(
            manifest.get("research"),
            "research",
            {"runtime_platform_sources", "private_headers", "targets"},
        )
        runtime_platforms = self.require_object(
            research.get("runtime_platform_sources"),
            "research.runtime_platform_sources",
            {"linux"},
        )
        research_runtime = {
            "linux": self.require_string_array(
                runtime_platforms.get("linux"),
                "research.runtime_platform_sources.linux",
                paths=True,
            )
        }
        private_headers = self.require_string_array(
            research.get("private_headers"),
            "research.private_headers",
            paths=True,
        )
        raw_research_targets = research.get("targets")
        if not isinstance(raw_research_targets, list):
            self.error("research.targets: expected array")
            raw_research_targets = []
        research_targets = [
            self.validate_target(
                item,
                f"research.targets[{index}]",
                research=True,
            )
            for index, item in enumerate(raw_research_targets)
        ]
        for target in research_targets:
            name = target["name"]
            if name in target_names:
                self.error(f"targets: duplicate target {name}")
            target_names.add(name)

        classified: dict[str, str] = {}
        groups: list[tuple[str, Iterable[str]]] = [
            ("common", common_sources),
            *platform_sources.items(),
            ("research-linux", research_runtime["linux"]),
        ]
        for classification, sources in groups:
            for source in sources:
                previous = classified.get(source)
                if previous is not None:
                    self.error(
                        f"{source}: duplicate source classification "
                        f"{previous} and {classification}"
                    )
                classified[source] = classification

        all_paths = set(classified) | set(private_headers)
        for targets in test_targets.values():
            for target in targets:
                all_paths.update(target["sources"])
        for target in research_targets:
            all_paths.update(target["sources"])
        for relative in sorted(all_paths):
            path = self.root / relative
            if not path.is_file() or path.is_symlink():
                self.error(f"{relative}: manifest path is not a regular file")

        return {
            "common_sources": common_sources,
            "platform_sources": platform_sources,
            "test_targets": test_targets,
            "research_runtime": research_runtime,
            "private_headers": private_headers,
            "research_targets": research_targets,
        }

    def check_versions(self, version: str, abi_major: int) -> None:
        components = tuple(int(part) for part in version.split("."))
        checks: list[tuple[str, str, str]] = []

        make = self.read_text("Makefile")
        if make is not None:
            checks.extend(
                [
                    (
                        "Make version",
                        find_required(
                            make,
                            r"(?m)^LLAM_VERSION\s*\?=\s*([^\s#]+)",
                            self,
                            "Make LLAM_VERSION",
                        ),
                        version,
                    ),
                    (
                        "Make ABI major",
                        find_required(
                            make,
                            r"(?m)^LLAM_ABI_MAJOR\s*\?=\s*(\d+)",
                            self,
                            "Make LLAM_ABI_MAJOR",
                        ),
                        str(abi_major),
                    ),
                ]
            )

        cmake = self.read_text("CMakeLists.txt")
        if cmake is not None:
            checks.extend(
                [
                    (
                        "CMake version",
                        find_required(
                            cmake,
                            r"project\s*\(\s*llam\s+VERSION\s+([0-9.]+)",
                            self,
                            "CMake project version",
                        ),
                        version,
                    ),
                    (
                        "CMake ABI major",
                        find_required(
                            cmake,
                            r"set\s*\(\s*LLAM_ABI_VERSION_MAJOR\s+(\d+)\s*\)",
                            self,
                            "CMake ABI major",
                        ),
                        str(abi_major),
                    ),
                ]
            )

        header = self.read_text("include/llam/runtime.h")
        if header is not None:
            for name, wanted in zip(
                ("MAJOR", "MINOR", "PATCH"),
                components,
                strict=True,
            ):
                checks.append(
                    (
                        f"runtime.h version {name.lower()}",
                        find_required(
                            header,
                            rf"(?m)^#define\s+LLAM_VERSION_{name}\s+(\d+)U?\s*$",
                            self,
                            f"runtime.h LLAM_VERSION_{name}",
                        ),
                        str(wanted),
                    )
                )
            checks.append(
                (
                    "runtime.h ABI major",
                    find_required(
                        header,
                        (
                            r"(?m)^#define\s+LLAM_ABI_VERSION_MAJOR\s+"
                            r"(\d+)U?\s*$"
                        ),
                        self,
                        "runtime.h LLAM_ABI_VERSION_MAJOR",
                    ),
                    str(abi_major),
                )
            )

        abi = self.read_text("src/core/base/abi.c")
        if abi is not None:
            checks.append(
                (
                    "ABI version string",
                    find_required(
                        abi,
                        (
                            r'(?m)^#define\s+LLAM_VERSION_STRING_LITERAL\s+'
                            r'"([^"]+)"'
                        ),
                        self,
                        "abi.c LLAM_VERSION_STRING_LITERAL",
                    ),
                    version,
                )
            )

        package = self.read_text("scripts/package_release.sh")
        if package is not None:
            checks.extend(
                [
                    (
                        "package library version",
                        find_required(
                            package,
                            (
                                r'library_version="\$\{LLAM_VERSION:-'
                                r'([^}]+)\}"'
                            ),
                            self,
                            "package LLAM_VERSION default",
                        ),
                        version,
                    ),
                    (
                        "package ABI major",
                        find_required(
                            package,
                            r'abi_major="\$\{LLAM_ABI_MAJOR:-([^}]+)\}"',
                            self,
                            "package LLAM_ABI_MAJOR default",
                        ),
                        str(abi_major),
                    ),
                ]
            )

        workflow = self.read_text(".github/workflows/linux.yml")
        if workflow is not None:
            checks.append(
                (
                    "Linux CI version",
                    find_required(
                        workflow,
                        r'(?m)^\s*LLAM_CI_VERSION:\s*"?([^"\s]+)"?\s*$',
                        self,
                        "Linux CI LLAM_CI_VERSION",
                    ),
                    version,
                )
            )

        for label, actual, expected in checks:
            if actual and actual != expected:
                self.error(f"{label} {actual} != {expected}")

    def check_sources(self, manifest: dict[str, Any]) -> None:
        make_text = self.read_text("Makefile")
        cmake_text = self.read_text("CMakeLists.txt")
        if make_text is None or cmake_text is None:
            return
        make = MakeProjection(self.root, make_text, self)
        cmake = CMakeProjection(cmake_text, self)

        self.compare_source_group(
            "stable common",
            manifest["common_sources"],
            make.source_variable("RUNTIME_COMMON_OBJS"),
            cmake.source_variable("LLAM_RUNTIME_COMMON_SOURCES"),
        )
        expected_classification = {
            source: group
            for group, sources in manifest["platform_sources"].items()
            for source in sources
        }
        make_classification: dict[str, str] = {}
        cmake_classification: dict[str, str] = {}
        for group in PLATFORM_GROUPS:
            make_sources = make.source_variable(
                f"RUNTIME_{group.upper()}_OBJS"
            )
            cmake_sources = cmake.source_variable(
                f"LLAM_RUNTIME_{group.upper()}_SOURCES"
            )
            for source in make_sources:
                make_classification[source] = group
            for source in cmake_sources:
                cmake_classification[source] = group
            self.compare_source_group(
                f"stable platform {group}",
                manifest["platform_sources"][group],
                make_sources,
                cmake_sources,
                report_missing=False,
            )
        self.check_classifications(
            "Make",
            expected_classification,
            make_classification,
        )
        self.check_classifications(
            "CMake",
            expected_classification,
            cmake_classification,
        )

        for platform, expected in manifest["research_runtime"].items():
            make_sources = make.source_variable(
                f"RESEARCH_RUNTIME_{platform.upper()}_OBJS"
            )
            cmake_sources = cmake.source_variable(
                f"LLAM_RESEARCH_RUNTIME_{platform.upper()}_SOURCES"
            )
            self.compare_source_group(
                f"research runtime {platform}",
                expected,
                make_sources,
                cmake_sources,
            )

        expected_headers = manifest["private_headers"]
        actual_headers = make.literal_variable("RESEARCH_PRIVATE_HDRS")
        compare_members(
            expected_headers,
            actual_headers,
            "Make",
            self,
        )
        if "$(RESEARCH_PRIVATE_HDRS)" not in make.raw_tokens("RUNTIME_PRIV_HDRS"):
            self.error(
                "Make RUNTIME_PRIV_HDRS does not include RESEARCH_PRIVATE_HDRS"
            )

        for targets in manifest["test_targets"].values():
            for target in targets:
                self.check_target(target, make, cmake, research=False)
        ctest_platforms = cmake_registered_test_platforms(cmake_text)
        all_platforms = {"linux", "darwin", "bsd", "windows"}
        for targets in manifest["test_targets"].values():
            for target in targets:
                name = target["name"]
                expected = set(target["platforms"])
                if expected == {"all"}:
                    expected = all_platforms
                actual = ctest_platforms.get(name)
                if actual is None:
                    self.error(f"{name}: CTest registration is missing")
                elif actual != expected:
                    actual_label = (
                        ["all"]
                        if actual == all_platforms
                        else sorted(actual)
                    )
                    expected_label = (
                        ["all"]
                        if expected == all_platforms
                        else sorted(expected)
                    )
                    self.error(
                        f"{name}: CTest platforms {actual_label} "
                        f"!= {expected_label}"
                    )
        for target in manifest["research_targets"]:
            self.check_target(target, make, cmake, research=True)

        expected_stable_targets = {
            target["name"]
            for targets in manifest["test_targets"].values()
            for target in targets
        }
        for test_class, targets in manifest["test_targets"].items():
            expected_class_targets = {
                target["name"] for target in targets
            }
            variable_stem = test_class.upper()
            compare_target_sets(
                expected_class_targets,
                set(
                    make.literal_variable(
                        f"{variable_stem}_TEST_TARGETS"
                    )
                ),
                test_class,
                "Make",
                self,
            )
            compare_target_sets(
                expected_class_targets,
                set(
                    cmake.expand_tokens(
                        cmake.variables.get(
                            f"LLAM_{variable_stem}_TEST_TARGETS",
                            [],
                        )
                    )
                ),
                test_class,
                "CMake",
                self,
            )
        expected_research_targets = {
            target["name"] for target in manifest["research_targets"]
        }
        for family in RESEARCH_FAMILIES:
            expected_family_targets = {
                target["name"]
                for target in manifest["research_targets"]
                if target["family"] == family
            }
            compare_target_sets(
                expected_family_targets,
                set(
                    make.literal_variable(
                        f"{family.upper()}_RESEARCH_TARGETS"
                    )
                ),
                family,
                "Make",
                self,
            )
            compare_target_sets(
                expected_family_targets,
                set(
                    cmake.expand_tokens(
                        cmake.variables.get(
                            (
                                f"LLAM_{family.upper()}_"
                                "RESEARCH_TARGETS"
                            ),
                            [],
                        )
                    )
                ),
                family,
                "CMake",
                self,
            )
        make_stable_targets = {
            target
            for target in make.literal_variable("LINK_TARGETS")
            if target.startswith("test_")
        }
        make_research_targets = set(
            make.literal_variable("RESEARCH_LINK_TARGETS")
        )
        cmake_stable_targets = {
            target
            for target in cmake.targets
            if any(
                source.startswith("tests/")
                for source in cmake.target_sources(target)
            )
        }
        cmake_research_targets = {
            target
            for target in cmake.targets
            if any(
                source.startswith("experiments/")
                for source in cmake.target_sources(target)
            )
        }
        compare_target_sets(
            expected_stable_targets,
            make_stable_targets,
            "stable",
            "Make",
            self,
        )
        compare_target_sets(
            expected_stable_targets,
            cmake_stable_targets,
            "stable",
            "CMake",
            self,
        )
        compare_target_sets(
            expected_research_targets,
            make_research_targets,
            "research",
            "Make",
            self,
        )
        compare_target_sets(
            expected_research_targets,
            cmake_research_targets,
            "research",
            "CMake",
            self,
        )

        build_sources = set(make.source_variable("BUILD_OBJS"))
        for targets in manifest["test_targets"].values():
            for target in targets:
                for source in target["sources"]:
                    if source not in build_sources:
                        self.error(f"{source}: missing from Make BUILD_OBJS")
        research_sources = set(make.source_variable("RESEARCH_OBJS"))
        for platform in manifest["research_runtime"]:
            variable = f"RESEARCH_RUNTIME_{platform.upper()}_OBJS"
            if f"$({variable})" not in make_text:
                self.error(
                    f"Make RUNTIME_OBJS does not include {variable}"
                )
        for target in manifest["research_targets"]:
            for source in target["sources"]:
                if source not in research_sources:
                    self.error(f"{source}: missing from Make RESEARCH_OBJS")

        if not make.has_rule_dependency("$(BUILD_OBJS)", "$(BUILD_SIGNATURE)"):
            self.error("Make BUILD_OBJS are not covered by BUILD_SIGNATURE")
        if not make.has_rule_dependency(
            "$(RESEARCH_OBJS)",
            "$(BUILD_SIGNATURE)",
        ):
            self.error("Make RESEARCH_OBJS are not covered by BUILD_SIGNATURE")
        depflags = make.raw_tokens("DEPFLAGS")
        if depflags != ["-MMD", "-MP"]:
            self.error(
                f"Make DEPFLAGS {depflags} != ['-MMD', '-MP']"
            )
        compile_recipes = [
            line.strip()
            for line in make_text.splitlines()
            if line.startswith("\t")
            and "$(CC)" in line
            and "-c -o $@" in line
        ]
        if not compile_recipes:
            self.error("Make compile recipes are missing")
        for recipe in compile_recipes:
            if "$(DEPFLAGS)" not in recipe:
                self.error(
                    f"Make compile recipe omits DEPFLAGS: {recipe}"
                )
        if re.search(
            r"(?m)^-include\s+\$\(ALL_DEPFILES\)\s*$",
            make_text,
        ) is None:
            self.error("Make dependency files are not included")
        if "audit-build-manifests" not in make.rules.get("test", []):
            self.error(
                "Make test target does not depend on audit-build-manifests"
            )
        audit_recipe = "\n".join(
            make.recipes.get("audit-build-manifests", [])
        )
        if (
            "scripts/audit_build_manifests.py" not in audit_recipe
            or "--root . --check" not in audit_recipe
        ):
            self.error("Make audit-build-manifests recipe is missing")

        cmake_build_manifest = False
        for command, body in cmake_commands(cmake_text):
            if command != "add_test":
                continue
            tokens = cmake_tokens(body)
            if (
                "NAME" in tokens
                and "build_manifest" in tokens
                and any(
                    token.endswith("scripts/audit_build_manifests.py")
                    for token in tokens
                )
                and "--check" in tokens
            ):
                cmake_build_manifest = True
        if not cmake_build_manifest:
            self.error("CMake build_manifest test is missing")

    def compare_source_group(
        self,
        label: str,
        expected: list[str],
        make: list[str],
        cmake: list[str],
        *,
        report_missing: bool = True,
    ) -> None:
        del label
        compare_members(
            expected,
            make,
            "Make",
            self,
            report_missing=report_missing,
        )
        compare_members(
            expected,
            cmake,
            "CMake",
            self,
            report_missing=report_missing,
        )

    def check_classifications(
        self,
        build_system: str,
        expected: dict[str, str],
        actual: dict[str, str],
    ) -> None:
        for source, expected_group in sorted(expected.items()):
            actual_group = actual.get(source)
            if actual_group is None:
                self.error(f"{source}: missing from {build_system}")
            elif actual_group != expected_group:
                self.error(
                    f"{source}: {build_system} platform mismatch: "
                    f"{actual_group} != {expected_group}"
                )
        for source in sorted(set(actual) - set(expected)):
            self.error(f"{source}: extra in {build_system}")

    def check_target(
        self,
        target: dict[str, Any],
        make: "MakeProjection",
        cmake: "CMakeProjection",
        *,
        research: bool,
    ) -> None:
        name = target["name"]
        expected_sources = target["sources"]
        compare_members(
            expected_sources,
            make.target_sources(name),
            f"Make target {name}",
            self,
        )
        compare_members(
            expected_sources,
            cmake.target_sources(name),
            f"CMake target {name}",
            self,
        )
        expected_links = sorted(target["link_dependencies"])
        make_links = sorted(make.target_links(name))
        cmake_links = sorted(cmake.target_links(name))
        if make_links != expected_links:
            self.error(
                f"{name}: Make link dependencies {make_links} "
                f"!= {expected_links}"
            )
        if cmake_links != expected_links:
            self.error(
                f"{name}: CMake link dependencies {cmake_links} "
                f"!= {expected_links}"
            )
        del research

    def run(self) -> int:
        version_value = self.load_json("config/llam-version.json")
        sources_value = self.load_json("config/llam-sources.json")
        version = (
            self.validate_version(version_value)
            if version_value is not None
            else None
        )
        sources = (
            self.validate_sources(sources_value)
            if sources_value is not None
            else None
        )
        if version is not None:
            self.check_versions(*version)
        if sources is not None:
            self.check_sources(sources)
        for diagnostic in sorted(self.diagnostics):
            print(f"audit-build-manifests: {diagnostic}", file=sys.stderr)
        return 1 if self.diagnostics else 0


class MakeProjection:
    def __init__(self, root: Path, text: str, audit: Audit) -> None:
        self.root = root
        self.audit = audit
        self.variables: dict[str, list[str]] = {}
        self.rules: dict[str, list[str]] = {}
        self.recipes: dict[str, list[str]] = {}
        for line in logical_make_lines(text):
            assignment = re.match(
                r"^([A-Za-z_][A-Za-z0-9_]*)\s*(\+=|:=|\?=|=)\s*(.*)$",
                line,
            )
            if assignment:
                name, operator, value = assignment.groups()
                tokens = value.split()
                if operator == "+=":
                    self.variables.setdefault(name, []).extend(tokens)
                elif operator == "?=" and name in self.variables:
                    continue
                else:
                    self.variables[name] = tokens
                continue
            rule = re.match(r"^([^:=\s][^:]*)\s*:\s*(.*)$", line)
            if rule:
                targets, prerequisites = rule.groups()
                for target in targets.split():
                    self.rules.setdefault(target, []).extend(
                        prerequisites.split()
                    )
        active_targets: list[str] = []
        for physical in text.splitlines():
            if physical.startswith("\t"):
                for target in active_targets:
                    self.recipes.setdefault(target, []).append(physical[1:])
                continue
            active_targets = []
            stripped = physical.strip()
            rule = re.match(r"^([^:=\s][^:]*)\s*:\s*(.*)$", stripped)
            if rule:
                active_targets = rule.group(1).split()

    def raw_tokens(self, variable: str) -> list[str]:
        return list(self.variables.get(variable, []))

    def expand_tokens(
        self,
        tokens: Iterable[str],
        seen: frozenset[str] = frozenset(),
    ) -> list[str]:
        expanded: list[str] = []
        for token in tokens:
            match = re.fullmatch(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", token)
            if match and match.group(1) in self.variables:
                name = match.group(1)
                if name in seen:
                    self.audit.error(f"Make variable expansion cycle at {name}")
                    continue
                expanded.extend(
                    self.expand_tokens(
                        self.variables[name],
                        seen | {name},
                    )
                )
            else:
                expanded.append(token)
        return expanded

    def literal_variable(self, variable: str) -> list[str]:
        return [
            token
            for token in self.expand_tokens(self.variables.get(variable, []))
            if not token.startswith("$(")
        ]

    def source_variable(self, variable: str) -> list[str]:
        result: list[str] = []
        for token in self.expand_tokens(self.variables.get(variable, [])):
            source = self.object_to_source(token)
            if source is not None:
                result.append(source)
        return result

    def object_to_source(self, token: str) -> str | None:
        for prefix in ("$(OBJDIR)/", "$(TESTHOOK_OBJDIR)/", "$(SHARED_OBJDIR)/"):
            if token.startswith(prefix):
                token = token[len(prefix) :]
                break
        else:
            return None
        if not token.endswith(".o"):
            self.audit.error(f"Make object token has unknown suffix: {token}")
            return None
        stem = token[:-2]
        aliases = {
            "examples/server_lossless": "examples/server.c",
        }
        alias = aliases.get(stem)
        if alias is not None:
            return alias
        candidates = [
            f"{stem}{suffix}"
            for suffix in (".c", ".S", ".asm")
            if (self.root / f"{stem}{suffix}").is_file()
        ]
        if len(candidates) != 1:
            self.audit.error(
                f"Make object {token}: expected exactly one source, "
                f"found {candidates}"
            )
            return None
        return candidates[0]

    def target_sources(self, target: str) -> list[str]:
        result: list[str] = []
        for token in self.rules.get(target, []):
            match = re.fullmatch(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", token)
            if match and match.group(1) in MAKE_LINK_VARIABLES:
                continue
            for expanded in self.expand_tokens([token]):
                source = self.object_to_source(expanded)
                if source is not None:
                    result.append(source)
        return result

    def target_links(self, target: str) -> list[str]:
        links: list[str] = []
        for token in self.rules.get(target, []):
            match = re.fullmatch(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", token)
            if match:
                dependency = MAKE_LINK_VARIABLES.get(match.group(1))
                if dependency is not None and dependency not in links:
                    links.append(dependency)
        if links:
            return links
        recipe = "\n".join(self.recipes.get(target, []))
        if "$(SERVER_FLOOD_LDLIBS)" in recipe:
            links.append("Threads::Threads")
        if "$(DL_LIBS)" in recipe:
            links.append("system_dynamic_loader")
        return links

    def has_rule_dependency(self, target: str, dependency: str) -> bool:
        return dependency in self.rules.get(target, [])


class CMakeProjection:
    def __init__(self, text: str, audit: Audit) -> None:
        self.audit = audit
        self.variables: dict[str, list[str]] = {}
        self.targets: dict[str, list[str]] = {}
        self.links: dict[str, list[str]] = {}
        uncommented = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
        for command, body in cmake_commands(uncommented):
            tokens = cmake_tokens(body)
            if not tokens:
                continue
            if command == "set":
                self.variables[tokens[0]] = tokens[1:]
            elif command == "list" and len(tokens) >= 2 and tokens[0] == "APPEND":
                self.variables.setdefault(tokens[1], []).extend(tokens[2:])
            elif command == "add_executable":
                self.targets[tokens[0]] = tokens[1:]
            elif command == "target_link_libraries":
                target = tokens[0]
                values = [
                    token
                    for token in tokens[1:]
                    if token not in {"PRIVATE", "PUBLIC", "INTERFACE"}
                ]
                self.links.setdefault(target, []).extend(values)

    def expand_tokens(
        self,
        tokens: Iterable[str],
        seen: frozenset[str] = frozenset(),
    ) -> list[str]:
        result: list[str] = []
        for token in tokens:
            match = re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", token)
            if match and match.group(1) in self.variables:
                name = match.group(1)
                if name in seen:
                    self.audit.error(f"CMake variable expansion cycle at {name}")
                    continue
                result.extend(
                    self.expand_tokens(
                        self.variables[name],
                        seen | {name},
                    )
                )
            else:
                result.append(token)
        return result

    def source_variable(self, variable: str) -> list[str]:
        return [
            token
            for token in self.expand_tokens(self.variables.get(variable, []))
            if source_suffix(token)
        ]

    def target_sources(self, target: str) -> list[str]:
        return [
            token
            for token in self.expand_tokens(self.targets.get(target, []))
            if source_suffix(token)
        ]

    def target_links(self, target: str) -> list[str]:
        result: list[str] = []
        for token in self.expand_tokens(self.links.get(target, [])):
            if token.startswith("$<"):
                continue
            if token == "${CMAKE_DL_LIBS}":
                token = "system_dynamic_loader"
            result.append(token)
        return result


def valid_relative_path(value: str) -> bool:
    path = PurePosixPath(value)
    return (
        not path.is_absolute()
        and "\\" not in value
        and ".." not in path.parts
        and "." not in path.parts
        and str(path) == value
    )


def source_suffix(value: str) -> bool:
    return value.endswith((".c", ".S", ".asm"))


def find_required(
    text: str,
    pattern: str,
    audit: Audit,
    label: str,
) -> str:
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        audit.error(f"{label}: expected exactly one projection, found {len(matches)}")
        return ""
    return matches[0]


def logical_make_lines(text: str) -> list[str]:
    result: list[str] = []
    current = ""
    for physical in text.splitlines():
        stripped = physical.strip()
        if current:
            if stripped.endswith("\\"):
                current += stripped[:-1] + " "
            else:
                result.append((current + stripped).strip())
                current = ""
            continue
        if not stripped or stripped.startswith("#") or physical.startswith("\t"):
            continue
        if stripped.endswith("\\"):
            current += stripped[:-1] + " "
        else:
            result.append((current + stripped).strip())
            current = ""
    if current:
        result.append(current.strip())
    return result


def cmake_commands(text: str) -> list[tuple[str, str]]:
    commands: list[tuple[str, str]] = []
    index = 0
    pattern = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")
    while True:
        match = pattern.search(text, index)
        if match is None:
            break
        depth = 1
        cursor = match.end()
        quoted = False
        escaped = False
        while cursor < len(text) and depth:
            char = text[cursor]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            cursor += 1
        if depth:
            break
        commands.append(
            (
                match.group(1).lower(),
                text[match.end() : cursor - 1],
            )
        )
        index = cursor
    return commands


def cmake_tokens(body: str) -> list[str]:
    return [
        token[1:-1] if len(token) >= 2 and token[0] == token[-1] == '"' else token
        for token in re.findall(r'"(?:\\.|[^"])*"|[^\s]+', body)
    ]


def cmake_registered_test_platforms(text: str) -> dict[str, set[str]]:
    all_platforms = {"linux", "darwin", "bsd", "windows"}
    posix_platforms = all_platforms - {"windows"}
    allowed_stack = [set(all_platforms)]
    frames: list[tuple[set[str], set[str], set[str]]] = []
    registrations: dict[str, set[str]] = {}
    command_lines: list[str] = []
    command_allowed: set[str] = set()
    depth = 0

    for raw_line in text.splitlines():
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        if command_lines:
            command_lines.append(line)
            depth += line.count("(") - line.count(")")
            if depth > 0:
                continue
            command = "\n".join(command_lines)
            parsed = cmake_commands(command)
            if parsed and parsed[0][0] == "add_test":
                tokens = cmake_tokens(parsed[0][1])
                if "NAME" in tokens:
                    index = tokens.index("NAME")
                    if index + 1 < len(tokens):
                        registrations.setdefault(tokens[index + 1], set()).update(
                            command_allowed
                        )
            command_lines = []
            continue

        if line.startswith("if("):
            parent = allowed_stack[-1]
            if re.fullmatch(
                r'if\(CMAKE_SYSTEM_NAME\s+STREQUAL\s+"Windows"\)',
                line,
            ):
                true_allowed = {"windows"}
                false_allowed = posix_platforms
            elif re.fullmatch(
                r'if\(NOT\s+CMAKE_SYSTEM_NAME\s+STREQUAL\s+"Windows"\)',
                line,
            ):
                true_allowed = posix_platforms
                false_allowed = {"windows"}
            else:
                true_allowed = all_platforms
                false_allowed = all_platforms
            frames.append((set(parent), true_allowed, false_allowed))
            allowed_stack.append(parent & true_allowed)
            continue
        if line.startswith("else("):
            if frames:
                parent, _, false_allowed = frames[-1]
                allowed_stack[-1] = parent & false_allowed
            continue
        if line.startswith("elseif("):
            if frames:
                parent, _, _ = frames[-1]
                allowed_stack[-1] = set(parent)
            continue
        if line.startswith("endif("):
            if frames:
                frames.pop()
                allowed_stack.pop()
            continue
        if re.match(r"add_test\s*\(", line):
            command_lines = [line]
            command_allowed = set(allowed_stack[-1])
            depth = line.count("(") - line.count(")")
            if depth <= 0:
                parsed = cmake_commands(line)
                if parsed:
                    tokens = cmake_tokens(parsed[0][1])
                    if "NAME" in tokens:
                        index = tokens.index("NAME")
                        if index + 1 < len(tokens):
                            registrations.setdefault(
                                tokens[index + 1],
                                set(),
                            ).update(command_allowed)
                command_lines = []
    return registrations


def compare_members(
    expected: Iterable[str],
    actual: Iterable[str],
    build_system: str,
    audit: Audit,
    *,
    report_missing: bool = True,
) -> None:
    expected_values = list(expected)
    actual_values = list(actual)
    expected_set = set(expected_values)
    actual_set = set(actual_values)
    seen: set[str] = set()
    duplicates: set[str] = set()
    for item in actual_values:
        if item in seen:
            duplicates.add(item)
        seen.add(item)
    for item in sorted(duplicates):
        audit.error(f"{item}: duplicate in {build_system}")
    if report_missing:
        for item in sorted(expected_set - actual_set):
            audit.error(f"{item}: missing from {build_system}")
    for item in sorted(actual_set - expected_set):
        audit.error(f"{item}: extra in {build_system}")


def compare_target_sets(
    expected: set[str],
    actual: set[str],
    classification: str,
    build_system: str,
    audit: Audit,
) -> None:
    for target in sorted(expected - actual):
        audit.error(
            f"{target}: missing {classification} target from {build_system}"
        )
    for target in sorted(actual - expected):
        audit.error(
            f"{target}: extra {classification} target in {build_system}"
        )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Audit canonical LLAM build manifests",
    )
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--check",
        action="store_true",
        help="check projections without modifying files (the default behavior)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    root = args.root.resolve()
    if not root.is_dir():
        print(
            f"audit-build-manifests: root is not a directory: {root}",
            file=sys.stderr,
        )
        return 1
    return Audit(root).run()


if __name__ == "__main__":
    raise SystemExit(main())
