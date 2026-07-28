#!/usr/bin/env python3
"""Audit LLAM's canonical version and source manifests without evaluation."""

from __future__ import annotations

import argparse
from collections.abc import Iterable
import json
from pathlib import Path, PurePosixPath
import re
import stat
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
    "DL_LIBS": "system_dynamic_loader",
}
RUNTIME_LIBRARY_TARGETS = (
    "llam_runtime",
    "llam_runtime_shared",
    "llam_runtime_testhooks",
)
CMAKE_MUTATING_LIST_OPERATIONS = {
    "REMOVE_ITEM",
    "REMOVE_AT",
    "REMOVE_DUPLICATES",
    "FILTER",
    "INSERT",
    "PREPEND",
    "POP_BACK",
    "POP_FRONT",
    "REVERSE",
    "SORT",
    "TRANSFORM",
}


class DuplicateKeyError(ValueError):
    """Raised when JSON repeats an object key."""


class Audit:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.diagnostics: set[str] = set()
        self.manifest_targets: set[str] = set()
        self.manifest_sources: set[str] = set()
        self.audited_make_variables: set[str] = set()
        self.audited_cmake_variables: set[str] = set()

    def error(self, message: str) -> None:
        self.diagnostics.add(message)

    def read_text(self, relative: str) -> str | None:
        path = self.root / relative
        try:
            return path.read_text(encoding="utf-8")
        except OSError as exc:
            self.error(
                f"{relative}: cannot read UTF-8 text: "
                f"{exc.__class__.__name__}"
            )
            return None
        except UnicodeError as exc:
            self.error(
                f"{relative}: cannot read UTF-8 text: "
                f"{exc.__class__.__name__}"
            )
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
        version_valid = (
            version is not None
            and re.fullmatch(r"\d+\.\d+\.\d+", version) is not None
        )
        if version is not None and not version_valid:
            self.error(
                "llam-version.json.version: expected numeric major.minor.patch"
            )
        abi_major = manifest.get("abi_major")
        if not isinstance(abi_major, int) or isinstance(abi_major, bool):
            self.error("llam-version.json.abi_major: expected integer")
            return None
        if abi_major < 0:
            self.error("llam-version.json.abi_major: expected non-negative integer")
        if not version_valid:
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
        raw_links = target.get("link_dependencies")
        link_platforms = ("linux", "darwin", "bsd", "windows")
        if isinstance(raw_links, dict):
            link_object = self.require_object(
                raw_links,
                f"{location}.link_dependencies",
                set(link_platforms),
            )
            links = {
                platform: self.require_string_array(
                    link_object.get(platform),
                    f"{location}.link_dependencies.{platform}",
                )
                for platform in link_platforms
            }
        else:
            uniform_links = self.require_string_array(
                raw_links,
                f"{location}.link_dependencies",
            )
            links = {
                platform: list(uniform_links)
                for platform in link_platforms
            }
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
            self.check_manifest_path(relative)

        return {
            "common_sources": common_sources,
            "platform_sources": platform_sources,
            "test_targets": test_targets,
            "research_runtime": research_runtime,
            "private_headers": private_headers,
            "research_targets": research_targets,
        }

    def check_manifest_path(self, relative: str) -> None:
        current = self.root
        parts = PurePosixPath(relative).parts
        for index, part in enumerate(parts):
            current /= part
            display = PurePosixPath(*parts[: index + 1]).as_posix()
            try:
                mode = current.lstat().st_mode
            except OSError as exc:
                self.error(
                    f"{relative}: cannot inspect manifest path component "
                    f"{display}: {exc.__class__.__name__}"
                )
                return
            if stat.S_ISLNK(mode):
                self.error(
                    f"{relative}: manifest path traverses symlink at "
                    f"{display}"
                )
                return
            if index < len(parts) - 1 and not stat.S_ISDIR(mode):
                self.error(
                    f"{relative}: manifest path ancestor is not a "
                    f"directory: {display}"
                )
                return
            if index == len(parts) - 1 and not stat.S_ISREG(mode):
                self.error(
                    f"{relative}: manifest path is not a regular file"
                )

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
                    (
                        "package GITHUB_REF_NAME fallback",
                        package_version_fallback(package, self),
                        version,
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
        self.manifest_targets = {
            target["name"]
            for targets in manifest["test_targets"].values()
            for target in targets
        } | {
            target["name"] for target in manifest["research_targets"]
        }
        self.manifest_sources = {
            *manifest["common_sources"],
            *(
                source
                for sources in manifest["platform_sources"].values()
                for source in sources
            ),
            *(
                source
                for sources in manifest["research_runtime"].values()
                for source in sources
            ),
            *(
                source
                for targets in manifest["test_targets"].values()
                for target in targets
                for source in target["sources"]
            ),
            *(
                source
                for target in manifest["research_targets"]
                for source in target["sources"]
            ),
        }
        check_declarative_subset(make_text, cmake_text, self)
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
        self.check_runtime_graphs(
            manifest,
            make_text,
            cmake_text,
            make,
            cmake,
        )
        self.check_enforcement(make_text, cmake_text)

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
        all_platforms = {"linux", "darwin", "bsd", "windows"}
        for targets in manifest["test_targets"].values():
            for target in targets:
                name = target["name"]
                expected = set(target["platforms"])
                if expected == {"all"}:
                    expected = all_platforms
                actual = cmake_target_registration_platforms(
                    cmake_text,
                    name,
                    research=0,
                    audit=self,
                )
                if not actual:
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
            name = target["name"]
            expected = expand_platforms(target["platforms"])
            actual = cmake_target_registration_platforms(
                cmake_text,
                name,
                research=1,
                audit=self,
            )
            if actual != expected:
                self.error(
                    f"{name}: CTest platforms "
                    f"{platform_label(actual)} != "
                    f"{platform_label(expected)}"
                )
        self.check_target_platform_graphs(
            manifest,
            make_text,
            cmake_text,
        )

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
        compile_recipes: list[tuple[str, str]] = []
        for target, recipes in make.recipes.items():
            for recipe in recipes:
                tokens = recipe.split()
                if (
                    "$(CC)" in tokens
                    and "-c" in tokens
                    and "$@" in tokens
                    and "$<" in tokens
                ):
                    category = (
                        "shared"
                        if target.startswith("$(SHARED_OBJDIR)/")
                        else "test-hook"
                        if target.startswith("$(TESTHOOK_OBJDIR)/")
                        else "research"
                        if "experiments/" in target
                        else "ordinary"
                    )
                    compile_recipes.append((category, recipe.strip()))
        if not compile_recipes:
            self.error("Make compile recipes are missing")
        for category, recipe in compile_recipes:
            if "$(DEPFLAGS)" not in recipe.split():
                self.error(
                    f"Make {category} compile recipe omits DEPFLAGS: "
                    f"{recipe}"
                )
        if re.search(
            r"(?m)^-include\s+\$\(ALL_DEPFILES\)\s*$",
            make_text,
        ) is None:
            self.error("Make dependency files are not included")
        expected_depfiles = [
            "$(BUILD_OBJS:.o=.d)",
            "$(RESEARCH_OBJS:.o=.d)",
            "$(SHARED_RUNTIME_OBJS:.o=.d)",
            "$(TESTHOOK_RUNTIME_OVERRIDE_OBJS:.o=.d)",
        ]
        actual_depfiles = make.raw_tokens("ALL_DEPFILES")
        for token in expected_depfiles:
            if token not in actual_depfiles:
                self.error(f"Make ALL_DEPFILES omits {token}")
        for token in actual_depfiles:
            if token not in expected_depfiles:
                self.error(f"Make ALL_DEPFILES has extra token {token}")
        if actual_depfiles != expected_depfiles:
            self.error(
                f"Make ALL_DEPFILES order {actual_depfiles} "
                f"!= {expected_depfiles}"
            )
        for signature in (
            "$(BUILD_SIGNATURE)",
            "$(SHARED_BUILD_SIGNATURE)",
            "$(TESTHOOK_BUILD_SIGNATURE)",
        ):
            signature_recipe = "\n".join(
                make.recipes.get(signature, [])
            )
            active_recipe = uncomment_text(signature_recipe)
            if not signature_recipe_emits_depflags(active_recipe):
                self.error(
                    f"Make {signature} does not record DEPFLAGS"
                )
                self.error(
                    f"Make {signature} does not emit DEPFLAGS"
                )
    def check_enforcement(
        self,
        make_text: str,
        cmake_text: str,
    ) -> None:
        exact_make_recipe = (
            "python3 scripts/audit_build_manifests.py --root . --check"
        )
        for config in runtime_configurations(cmake=False):
            active_text = active_make_text(make_text, config, self)
            projection = MakeProjection(self.root, active_text, self)
            for target in ("test", "check"):
                if not make_target_reaches(
                    projection.rules,
                    target,
                    "audit-build-manifests",
                ):
                    self.error(
                        f"Make {config['label']} "
                        f"research={config['research']} {target} target "
                        "does not reach audit-build-manifests"
                    )
            recipes = projection.recipes.get(
                "audit-build-manifests",
                [],
            )
            active_recipes = [
                recipe.split("#", 1)[0].strip().removeprefix("@")
                for recipe in recipes
                if recipe.split("#", 1)[0].strip()
            ]
            if active_recipes != [exact_make_recipe]:
                self.error(
                    "Make audit-build-manifests recipe is not exact: "
                    f"{active_recipes}"
                )

        exact_cmake_tokens = [
            "NAME",
            "build_manifest",
            "COMMAND",
            "${Python3_EXECUTABLE}",
            (
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/"
                "audit_build_manifests.py"
            ),
            "--root",
            "${CMAKE_CURRENT_SOURCE_DIR}",
            "--check",
        ]
        for research in (0, 1):
            config = {
                "label": "linux-x86_64",
                "platform": "linux",
                "processor": "x86_64",
                "msvc": False,
                "research": research,
            }
            registrations = active_cmake_tests(
                cmake_text,
                config,
                self,
            )
            tokens = registrations.get("build_manifest")
            if tokens is None:
                self.error(
                    "CMake build_manifest test is inactive for "
                    f"research={research}"
                )
            elif tokens != exact_cmake_tokens:
                self.error(
                    "CMake build_manifest test command is not exact: "
                    f"{tokens}"
                )

        workflow = self.read_text(".github/workflows/linux.yml")
        if workflow is not None:
            check_linux_workflow_audit_step(workflow, self)

    def check_runtime_graphs(
        self,
        manifest: dict[str, Any],
        make_text: str,
        cmake_text: str,
        make: "MakeProjection",
        cmake: "CMakeProjection",
    ) -> None:
        for config in runtime_configurations(cmake=False):
            expected = expected_runtime_sources(manifest, config)
            actual = make_runtime_sources(
                make_text,
                make,
                config,
                self,
            )
            compare_ordered(
                expected,
                actual,
                f"Make {config['label']} research={config['research']} "
                "runtime graph",
                self,
            )
            self.check_make_derived_runtime_closures(
                make_text,
                make,
                actual,
                config,
            )

        for config in runtime_configurations(cmake=True):
            expected = expected_runtime_sources(manifest, config)
            actual = cmake_runtime_sources(
                cmake_text,
                cmake,
                config,
                self,
            )
            compare_ordered(
                expected,
                actual,
                f"CMake {config['label']} research={config['research']} "
                "runtime graph",
                self,
            )
            for target in RUNTIME_LIBRARY_TARGETS:
                exists, library_sources = active_cmake_runtime_library(
                    cmake_text,
                    target,
                    config,
                    actual,
                    cmake,
                    self,
                )
                if not exists:
                    self.error(
                        f"CMake {target} is missing for "
                        f"{config['label']} research={config['research']}"
                    )
                compare_ordered(
                    expected,
                    library_sources,
                    f"CMake {target} sources",
                    self,
                )

        static_rules = make.rules.get("libllam_runtime.a", [])
        static_recipe = " ".join(make.recipes.get("libllam_runtime.a", []))
        expected_static = "$(RUNTIME_OBJS)"
        if expected_static not in static_rules:
            self.error(
                "libllam_runtime.a: Make prerequisites omit RUNTIME_OBJS"
            )
        runtime_recipe_inputs = validate_make_link_recipe_inputs(
            static_recipe,
            "libllam_runtime.a",
            "RUNTIME_OBJS",
            self,
        )
        if runtime_recipe_inputs != ["RUNTIME_OBJS"]:
            self.error(
                "libllam_runtime.a: Make link recipe inputs "
                f"{runtime_recipe_inputs} != ['RUNTIME_OBJS']"
            )
        for config in runtime_configurations(cmake=False):
            if config["platform"] == "windows":
                continue
            projection = MakeProjection(
                self.root,
                active_make_text(make_text, config, self),
                self,
            )
            shared_targets = [
                target
                for target, prerequisites in projection.rules.items()
                if "$(SHARED_RUNTIME_OBJS)" in prerequisites
            ]
            if len(shared_targets) != 1:
                self.error(
                    f"Make {config['label']} research={config['research']} "
                    "must have exactly one shared runtime link target"
                )
                continue
            recipe = " ".join(
                projection.recipes.get(shared_targets[0], [])
            )
            inputs = validate_make_link_recipe_inputs(
                recipe,
                shared_targets[0],
                "SHARED_RUNTIME_OBJS",
                self,
            )
            if inputs != ["SHARED_RUNTIME_OBJS"]:
                self.error(
                    "Make shared runtime link recipe inputs "
                    f"{inputs} != ['SHARED_RUNTIME_OBJS']"
                )

    def check_target_platform_graphs(
        self,
        manifest: dict[str, Any],
        make_text: str,
        cmake_text: str,
    ) -> None:
        cmake_variable_cache: dict[
            tuple[str, int],
            dict[str, list[str]],
        ] = {}
        stable_targets = [
            target
            for targets in manifest["test_targets"].values()
            for target in targets
        ]
        for target, research in [
            *((target, False) for target in stable_targets),
            *((target, True) for target in manifest["research_targets"]),
        ]:
            name = target["name"]
            supported = expand_platforms(target["platforms"])
            expected_sources = target["sources"]
            for config in runtime_configurations(cmake=True):
                cache_key = (config["label"], config["research"])
                if cache_key not in cmake_variable_cache:
                    cmake_variable_cache[cache_key] = (
                        configured_cmake_variables(
                            cmake_text,
                            config,
                            self,
                        )
                    )
                cmake_variables = cmake_variable_cache[cache_key]
                expected_active = (
                    config["platform"] in supported
                    and (not research or config["research"] == 1)
                )
                exists, sources, links = active_cmake_target(
                    cmake_text,
                    name,
                    config,
                    cmake_variables,
                    self,
                )
                label = (
                    f"{name}: CMake {config['label']} "
                    f"research={config['research']}"
                )
                if expected_active and not exists:
                    self.error(f"{label} target is missing")
                if expected_active:
                    compare_ordered(
                        expected_sources,
                        sources,
                        f"{label} sources",
                        self,
                    )
                    expected_links = sorted(
                        target["link_dependencies"][config["platform"]]
                    )
                    if sorted(links) != expected_links:
                        self.error(
                            f"{label} link dependencies "
                            f"{sorted(links)} != {expected_links}"
                        )
                        self.error(
                            f"{name}: CMake {config['platform']} "
                            f"link dependencies {sorted(links)} "
                            f"!= {expected_links}"
                        )
                elif research and config["research"] == 0 and exists:
                    self.error(f"{label} target is active")

                registrations = active_cmake_tests(
                    cmake_text,
                    config,
                    self,
                )
                registered = any(
                    ctest_registration_targets(tokens, name)
                    for tokens in registrations.values()
                )
                if expected_active and not registered:
                    self.error(f"{label.replace('CMake', 'CTest')} "
                               "registration is missing")
                elif not expected_active and registered:
                    self.error(f"{label.replace('CMake', 'CTest')} "
                               "registration is active")

            for config in runtime_configurations(cmake=False):
                if (
                    config["platform"] == "windows"
                    or config["platform"] not in supported
                ):
                    continue
                make = MakeProjection(
                    self.root,
                    active_make_text(make_text, config, self),
                    self,
                )
                if name not in make.rules:
                    self.error(
                        f"{name}: Make {config['label']} "
                        f"research={config['research']} target is missing"
                    )
                    continue
                label = (
                    f"{name}: Make {config['label']} "
                    f"research={config['research']}"
                )
                make.validate_target_prerequisites(name)
                compare_ordered(
                    expected_sources,
                    make.target_sources(name),
                    f"{label} prerequisite sources",
                    self,
                )
                compare_ordered(
                    expected_sources,
                    make.target_recipe_sources(name),
                    f"{label} recipe sources",
                    self,
                )
                if not make.recipes.get(name):
                    self.error(f"{label} recipe is missing")
                expected_links = sorted(
                    target["link_dependencies"][config["platform"]]
                )
                actual_links = sorted(make.target_links(name))
                if actual_links != expected_links:
                    self.error(
                        f"{label} link dependencies "
                        f"{actual_links} != {expected_links}"
                    )

    def check_make_derived_runtime_closures(
        self,
        make_text: str,
        make: "MakeProjection",
        runtime_sources: list[str],
        config: dict[str, Any],
    ) -> None:
        shared_values = make_assignment_values(
            make_text,
            "SHARED_RUNTIME_OBJS",
        )
        expected_shared = (
            "$(patsubst $(OBJDIR)/%,$(SHARED_OBJDIR)/%,$(RUNTIME_OBJS))"
        )
        if shared_values != [expected_shared]:
            self.error(
                "Make SHARED_RUNTIME_OBJS must be the canonical "
                "RUNTIME_OBJS projection"
            )

        testhook_values = make_assignment_values(
            make_text,
            "RUNTIME_TESTHOOK_OBJS",
        )
        if len(testhook_values) != 1:
            self.error(
                "Make RUNTIME_TESTHOOK_OBJS must have exactly one projection"
            )
            return
        value = testhook_values[0]
        match = re.fullmatch(
            r"\$\(filter-out\s+(.+),\s*\$\(RUNTIME_OBJS\)\)\s+"
            r"\$\(TESTHOOK_RUNTIME_OVERRIDE_OBJS\)",
            value,
        )
        if match is None:
            self.error(
                "Make RUNTIME_TESTHOOK_OBJS must be the canonical "
                "filter-out/override projection"
            )
            return
        excluded: list[str] = []
        for token in match.group(1).split():
            source = make.object_to_source(token)
            if source is None:
                self.error(
                    "Make RUNTIME_TESTHOOK_OBJS has unsupported exclusion "
                    f"{token}"
                )
                continue
            excluded.append(source)
        overrides = make.source_variable("TESTHOOK_RUNTIME_OVERRIDE_OBJS")
        label = (
            f"Make {config['label']} research={config['research']} "
            "test-hook replacement"
        )
        compare_ordered(excluded, overrides, label, self)
        missing = [source for source in excluded if source not in runtime_sources]
        if missing:
            self.error(
                f"{label}: replaces sources outside runtime graph {missing}"
            )

    def compare_source_group(
        self,
        label: str,
        expected: list[str],
        make: list[str],
        cmake: list[str],
        *,
        report_missing: bool = True,
    ) -> None:
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
        compare_projection_order(expected, make, label, "Make", self)
        compare_projection_order(expected, cmake, label, "CMake", self)

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
        make_sources = make.target_sources(name)
        make_recipe_sources = make.target_recipe_sources(name)
        cmake_sources = cmake.target_sources(name)
        compare_members(
            expected_sources,
            make_sources,
            f"Make target {name}",
            self,
        )
        compare_members(
            expected_sources,
            make_recipe_sources,
            f"Make target {name} recipe",
            self,
        )
        compare_members(
            expected_sources,
            cmake_sources,
            f"CMake target {name}",
            self,
        )
        compare_projection_order(
            expected_sources,
            make_sources,
            f"target {name} sources",
            "Make",
            self,
        )
        compare_projection_order(
            expected_sources,
            make_recipe_sources,
            f"target {name} recipe sources",
            "Make",
            self,
        )
        compare_projection_order(
            expected_sources,
            cmake_sources,
            f"target {name} sources",
            "CMake",
            self,
        )
        expected_links = sorted(
            {
                dependency
                for links in target["link_dependencies"].values()
                for dependency in links
            }
        )
        make_prerequisite_links = sorted(
            make.target_prerequisite_links(name)
        )
        make_recipe_links = sorted(make.target_recipe_links(name))
        make_links = sorted(
            set(make_prerequisite_links) | set(make_recipe_links)
        )
        cmake_links = sorted(cmake.target_links(name))
        if make_links != expected_links:
            self.error(
                f"{name}: Make link dependencies {make_links} "
                f"!= {expected_links}"
            )
        prerequisite_expected = sorted(
            dependency
            for dependency in expected_links
            if dependency in {"llam_runtime", "llam_runtime_testhooks"}
        )
        if make_prerequisite_links != prerequisite_expected:
            self.error(
                f"{name}: Make prerequisite link dependencies "
                f"{make_prerequisite_links} != {prerequisite_expected}"
            )
        if make_recipe_links != expected_links:
            self.error(
                f"{name}: Make recipe link dependencies "
                f"{make_recipe_links} != {expected_links}"
            )
        recipe = "\n".join(make.recipes.get(name, []))
        if prerequisite_expected and "$(LDLIBS)" not in recipe:
            self.error(f"{name}: Make link recipe omits LDLIBS")
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


def is_audited_make_variable(
    name: str,
    audit: Audit | None = None,
) -> bool:
    return (
        name
        in {
            "DEPFLAGS",
            "LLAM_VERSION",
            "LLAM_ABI_MAJOR",
            "RUNTIME_OBJS",
            "RUNTIME_PRIV_HDRS",
            "RESEARCH_PRIVATE_HDRS",
            "BUILD_OBJS",
            "RESEARCH_OBJS",
            "ALL_DEPFILES",
            "BUILD_SIGNATURE",
            "SHARED_BUILD_SIGNATURE",
            "TESTHOOK_BUILD_SIGNATURE",
            "LINK_TARGETS",
            "RESEARCH_LINK_TARGETS",
        }
        or (
            name.startswith("RUNTIME_")
            and name.endswith("_OBJS")
        )
        or (
            name.startswith("RESEARCH_RUNTIME_")
            and name.endswith("_OBJS")
        )
        or name.endswith("_TEST_TARGETS")
        or name.endswith("_RESEARCH_TARGETS")
        or (
            audit is not None
            and name in audit.audited_make_variables
        )
    )


def is_audited_cmake_variable(
    name: str,
    audit: Audit | None = None,
) -> bool:
    return (
        name
        in {
            "LLAM_ABI_VERSION_MAJOR",
            "LLAM_RUNTIME_SOURCES",
        }
        or (
            name.startswith("LLAM_RUNTIME_")
            and name.endswith("_SOURCES")
        )
        or (
            name.startswith("LLAM_RESEARCH_RUNTIME_")
            and name.endswith("_SOURCES")
        )
        or name.endswith("_TEST_TARGETS")
        or name.endswith("_RESEARCH_TARGETS")
        or (
            audit is not None
            and name in audit.audited_cmake_variables
        )
    )


def strip_unquoted_comment(line: str) -> str:
    result: list[str] = []
    quote = ""
    escaped = False
    for char in line:
        if escaped:
            result.append(char)
            escaped = False
            continue
        if char == "\\":
            result.append(char)
            escaped = True
            continue
        if quote:
            result.append(char)
            if char == quote:
                quote = ""
            continue
        if char in {"'", '"'}:
            result.append(char)
            quote = char
            continue
        if char == "#":
            break
        result.append(char)
    return "".join(result)


def uncomment_text(text: str) -> str:
    return "\n".join(
        strip_unquoted_comment(line) for line in text.splitlines()
    )


def make_line_touches_audited(line: str, audit: Audit) -> bool:
    stripped = strip_unquoted_comment(line).strip()
    assignment = re.match(
        (
            r"^(?:(?:override|export|private)\s+)?"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\+=|:=|\?=|=)"
        ),
        stripped,
    )
    if assignment and is_audited_make_variable(assignment.group(1), audit):
        return True
    unexport = re.match(
        r"^(?:export|unexport|define|undefine)\s+"
        r"([A-Za-z_][A-Za-z0-9_]*)",
        stripped,
    )
    if unexport and is_audited_make_variable(unexport.group(1), audit):
        return True
    rule = re.match(r"^([^:=\s][^:]*)\s*:", stripped)
    if rule:
        relevant_targets = (
            audit.manifest_targets
            | {
                "test",
                "check",
                "audit-build-manifests",
                "libllam_runtime.a",
                "static",
                "shared",
            }
        )
        return bool(set(rule.group(1).split()) & relevant_targets)
    return any(
        re.search(rf"\b{re.escape(name)}\b", stripped)
        for name in audit.manifest_targets
    )


def check_make_declarative_subset(text: str, audit: Audit) -> None:
    audit.audited_make_variables = {
        variable
        for line in logical_make_lines(text)
        for rule in [re.match(r"^([^:=\s][^:]*)\s*:\s*(.*)$", line)]
        if rule is not None
        and bool(set(rule.group(1).split()) & audit.manifest_targets)
        for variable in re.findall(
            r"\$\(([A-Za-z_][A-Za-z0-9_]*_OBJS)\)",
            rule.group(2),
        )
    }
    define_name: str | None = None
    for line in logical_make_lines(text):
        clean = strip_unquoted_comment(line).strip()
        if not clean:
            continue
        define = re.match(
            r"^(?:override\s+)?define\s+"
            r"([A-Za-z_][A-Za-z0-9_]*)",
            clean,
        )
        if define:
            define_name = define.group(1)
            if is_audited_make_variable(define_name, audit):
                audit.error(
                    f"Make audited variable {define_name} uses "
                    "unsupported define"
                )
            continue
        if clean == "endef":
            define_name = None
            continue
        if define_name is not None:
            continue
        modifier = re.match(
            (
                r"^(override|export|private)\s+"
                r"([A-Za-z_][A-Za-z0-9_]*)\s*"
                r"(?:\+=|:=|\?=|=)"
            ),
            clean,
        )
        if modifier and is_audited_make_variable(modifier.group(2), audit):
            audit.error(
                f"Make audited variable {modifier.group(2)} uses "
                f"unsupported {modifier.group(1)} assignment"
            )
            continue
        unexport = re.match(
            r"^(export|unexport|undefine)\s+"
            r"([A-Za-z_][A-Za-z0-9_]*)\s*$",
            clean,
        )
        if unexport and is_audited_make_variable(unexport.group(2), audit):
            audit.error(
                f"Make audited variable {unexport.group(2)} uses "
                f"unsupported {unexport.group(1)}"
            )
            continue
        assignment = re.match(
            (
                r"^([A-Za-z_][A-Za-z0-9_]*)\s*"
                r"(\+=|:=|\?=|=)\s*(.*)$"
            ),
            clean,
        )
        if assignment and is_audited_make_variable(
            assignment.group(1),
            audit,
        ):
            name, _, value = assignment.groups()
            allowed_function: str | None = None
            if name == "SHARED_RUNTIME_OBJS":
                allowed_function = "patsubst"
            elif name == "RUNTIME_TESTHOOK_OBJS":
                allowed_function = "filter-out"
            functions = {
                function
                for function in re.findall(
                    r"\$\(([A-Za-z_][A-Za-z0-9_-]*)(?=[\s,])",
                    value,
                )
                if function != allowed_function
            }
            for function in sorted(functions):
                audit.error(
                    f"Make audited variable {name} uses unsupported "
                    f"$({function} ...)"
                )
        if "$(eval " in clean:
            body = clean.partition("$(eval ")[2]
            identifiers = re.findall(
                r"[A-Za-z_][A-Za-z0-9_]*",
                body,
            )
            if (
                any(
                    is_audited_make_variable(name, audit)
                    for name in identifiers
                )
                or bool(set(identifiers) & audit.manifest_targets)
            ):
                audit.error(
                    "Make audited state uses unsupported $(eval ...)"
                )


def check_cmake_declarative_subset(text: str, audit: Audit) -> None:
    relevant_targets = audit.manifest_targets | set(RUNTIME_LIBRARY_TARGETS)
    commands = cmake_commands(uncomment_text(text))
    audit.audited_cmake_variables = {
        variable
        for command, body in commands
        if command in {"add_executable", "target_sources"}
        for tokens in [cmake_tokens(body)]
        if tokens and tokens[0] in audit.manifest_targets
        for token in tokens[1:]
        for match in [
            re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", token)
        ]
        if match is not None
        for variable in [match.group(1)]
    }
    for command, body in commands:
        tokens = cmake_tokens(body)
        if not tokens:
            continue
        if (
            command == "list"
            and len(tokens) >= 2
            and tokens[0] in CMAKE_MUTATING_LIST_OPERATIONS
            and is_audited_cmake_variable(tokens[1], audit)
        ):
            audit.error(
                f"CMake audited variable {tokens[1]} uses unsupported "
                f"list({tokens[0]})"
            )
        if (
            command == "unset"
            and is_audited_cmake_variable(tokens[0], audit)
        ):
            audit.error(
                f"CMake audited variable {tokens[0]} uses unsupported "
                "unset()"
            )
        if command in {"set_property", "set_target_properties"}:
            targets = set(tokens) & relevant_targets
            closure_properties = {
                "SOURCES",
                "LINK_LIBRARIES",
                "INTERFACE_LINK_LIBRARIES",
                "EXCLUDE_FROM_ALL",
            }
            if targets and set(tokens) & closure_properties:
                audit.error(
                    "CMake audited target property mutation is unsupported: "
                    f"{command}({body.strip()})"
                )
        if (
            command == "set_property"
            and tokens[0] == "SOURCE"
            and bool(set(tokens) & audit.manifest_sources)
        ):
            audit.error(
                "CMake source property mutation in an audited closure "
                f"is unsupported: {command}({body.strip()})"
            )
        if command == "set_source_files_properties":
            sources = {
                token for token in tokens if source_suffix(token)
            }
            if sources & audit.manifest_sources:
                audit.error(
                    "CMake source property mutation in an audited closure "
                    f"is unsupported: {command}({body.strip()})"
                )
        relevant_command = (
            command in {
                "set",
                "list",
                "add_executable",
                "add_library",
                "target_sources",
                "target_link_libraries",
            }
            and (
                (
                    tokens
                    and is_audited_cmake_variable(tokens[0], audit)
                )
                or bool(set(tokens) & relevant_targets)
                or (
                    command == "list"
                    and len(tokens) >= 2
                    and is_audited_cmake_variable(tokens[1], audit)
                )
            )
        )
        if relevant_command and any("$<" in token for token in tokens):
            audit.error(
                "CMake generator-expression indirection in an audited "
                f"closure is unsupported: {command}({body.strip()})"
            )
        if (
            command == "add_library"
            and tokens
            and tokens[0] in relevant_targets
            and "OBJECT" in tokens
        ):
            audit.error(
                f"CMake audited target {tokens[0]} uses unsupported "
                "OBJECT library indirection"
            )


def check_declarative_subset(
    make_text: str,
    cmake_text: str,
    audit: Audit,
) -> None:
    check_make_declarative_subset(make_text, audit)
    check_cmake_declarative_subset(cmake_text, audit)


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

    def validate_target_prerequisites(self, target: str) -> None:
        for token in self.rules.get(target, []):
            variable = re.fullmatch(
                r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
                token,
            )
            if variable:
                name = variable.group(1)
                if name not in self.variables and name not in MAKE_LINK_VARIABLES:
                    self.audit.error(
                        f"{target}: Make prerequisites have unknown "
                        f"expansion $({name})"
                    )
                continue
            if self.object_to_source(token) is not None:
                continue
            if token.endswith(".o"):
                self.audit.error(
                    f"{target}: Make prerequisites have additional "
                    f"input {token}"
                )

    def target_links(self, target: str) -> list[str]:
        return sorted(
            set(self.target_prerequisite_links(target))
            | set(self.target_recipe_links(target))
        )

    def target_prerequisite_links(self, target: str) -> list[str]:
        links: list[str] = []
        for token in self.rules.get(target, []):
            match = re.fullmatch(r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)", token)
            if match:
                dependency = MAKE_LINK_VARIABLES.get(match.group(1))
                if dependency is not None and dependency not in links:
                    links.append(dependency)
        return links

    def target_recipe_links(self, target: str) -> list[str]:
        links: list[str] = []
        recipe = "\n".join(self.recipes.get(target, []))
        for variable, dependency in MAKE_LINK_VARIABLES.items():
            if f"$({variable})" in recipe and dependency not in links:
                links.append(dependency)
        known_library_variables = {
            "LDLIBS",
            "SERVER_FLOOD_LDLIBS",
            "DL_LIBS",
        }
        for variable in re.findall(
            r"\$\(([A-Za-z_][A-Za-z0-9_]*(?:LDLIBS|LIBS))\)",
            recipe,
        ):
            if variable not in known_library_variables:
                self.audit.error(
                    f"{target}: Make recipe has unknown library "
                    f"expansion $({variable})"
                )
        return links

    def target_recipe_sources(self, target: str) -> list[str]:
        recipe = "\n".join(self.recipes.get(target, []))
        result: list[str] = []
        link_variables = set(MAKE_LINK_VARIABLES)
        object_variables = list(
            dict.fromkeys(
                re.findall(
                r"\$\(([A-Za-z_][A-Za-z0-9_]*_OBJS)\)",
                recipe,
                )
            )
        )
        for variable in object_variables:
            if variable in link_variables:
                continue
            if variable not in self.variables:
                self.audit.error(
                    f"{target}: Make recipe has unknown object "
                    f"expansion $({variable})"
                )
                continue
            result.extend(self.source_variable(variable))
        for token in re.findall(
            r"\$\((?:OBJDIR|TESTHOOK_OBJDIR|SHARED_OBJDIR)\)/"
            r"[A-Za-z0-9_./-]+\.o",
            recipe,
        ):
            source = self.object_to_source(token)
            if source is not None:
                result.append(source)
        for token in recipe.split():
            if token.endswith(".o") and "$(" not in token:
                self.audit.error(
                    f"{target}: Make recipe has additional object "
                    f"input {token}"
                )
        allowed_expansions = {
            "CC",
            "CFLAGS",
            "CPPFLAGS",
            "LDFLAGS",
            "LDLIBS",
            "OBJDIR",
            "TESTHOOK_OBJDIR",
            "SHARED_OBJDIR",
            *link_variables,
            *object_variables,
        }
        for variable in re.findall(
            r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
            recipe,
        ):
            if variable not in allowed_expansions:
                self.audit.error(
                    f"{target}: Make recipe has unknown expansion "
                    f"$({variable})"
                )
        return result

    def has_rule_dependency(self, target: str, dependency: str) -> bool:
        return dependency in self.rules.get(target, [])


class CMakeProjection:
    def __init__(self, text: str, audit: Audit) -> None:
        self.audit = audit
        self.variables: dict[str, list[str]] = {}
        self.targets: dict[str, list[str]] = {}
        self.links: dict[str, list[str]] = {}
        uncommented = uncomment_text(text)
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
            elif command == "target_sources" and tokens[0] in self.targets:
                self.targets[tokens[0]].extend(
                    token
                    for token in tokens[1:]
                    if token not in {"PRIVATE", "PUBLIC", "INTERFACE"}
                )
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


def package_version_fallback(text: str, audit: Audit) -> str:
    assignment_pattern = re.compile(
        (
            r"""^\s*version\s*=\s*['"]"""
            r"(?:\$\{LLAM_RELEASE_VERSION:-)?"
            r"\$\{GITHUB_REF_NAME:-v([^}]+)\}"
            r"\}?"
            r"""['"]\s*$"""
        )
    )
    normalization_pattern = re.compile(
        r"""^\s*version\s*=\s*['"]\$\{version#v\}['"]\s*$"""
    )
    fallback: str | None = None
    fallback_index = -1
    for index, physical in enumerate(text.splitlines()):
        line = strip_unquoted_comment(physical).strip()
        if not line:
            continue
        assignment = assignment_pattern.fullmatch(line)
        if assignment:
            if fallback is not None:
                audit.error(
                    "package version fallback assignment is duplicated"
                )
            else:
                fallback = assignment.group(1)
                fallback_index = index
            continue
        if re.match(r"^version\s*=", line):
            if normalization_pattern.fullmatch(line):
                if fallback is None:
                    audit.error(
                        "package version normalization precedes fallback"
                    )
                continue
            audit.error(
                "package version assignment does not use the canonical "
                "fallback"
            )
        if (
            fallback is None
            and index != fallback_index
            and re.search(r"\$(?:\{version\}|version\b)", line)
        ):
            audit.error("package version is used before canonical fallback")
    if fallback is None:
        audit.error(
            "package GITHUB_REF_NAME fallback: expected exactly one "
            "effective projection, found 0"
        )
        return ""
    return fallback


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


def runtime_configurations(*, cmake: bool) -> list[dict[str, Any]]:
    if cmake:
        platforms = [
            ("linux-x86_64", "linux", "x86_64", False),
            ("linux-amd64", "linux", "amd64", False),
            ("linux-aarch64", "linux", "aarch64", False),
            ("linux-arm64", "linux", "arm64", False),
            ("darwin-x86_64", "darwin", "x86_64", False),
            ("darwin-amd64", "darwin", "amd64", False),
            ("darwin-arm64", "darwin", "arm64", False),
            ("darwin-aarch64", "darwin", "aarch64", False),
            ("bsd-x86_64", "bsd", "x86_64", False),
            ("bsd-amd64", "bsd", "amd64", False),
            ("bsd-aarch64", "bsd", "aarch64", False),
            ("bsd-arm64", "bsd", "arm64", False),
            ("windows-gnu-x86_64", "windows", "x86_64", False),
            ("windows-gnu-amd64", "windows", "amd64", False),
            ("windows-msvc-x86_64", "windows", "x86_64", True),
            ("windows-msvc-amd64", "windows", "amd64", True),
        ]
    else:
        platforms = [
            ("linux-x86_64", "linux", "x86_64", False),
            ("linux-aarch64", "linux", "aarch64", False),
            ("darwin-x86_64", "darwin", "x86_64", False),
            ("darwin-arm64", "darwin", "arm64", False),
            ("bsd-x86_64", "bsd", "x86_64", False),
            ("bsd-amd64", "bsd", "amd64", False),
            ("bsd-aarch64", "bsd", "aarch64", False),
            ("bsd-arm64", "bsd", "arm64", False),
            ("windows-gnu-x86_64", "windows", "x86_64", False),
            ("windows-gnu-AMD64", "windows", "AMD64", False),
        ]
    return [
        {
            "label": label,
            "platform": platform,
            "processor": processor,
            "msvc": msvc,
            "research": research,
        }
        for label, platform, processor, msvc in platforms
        for research in (0, 1)
    ]


def expected_runtime_sources(
    manifest: dict[str, Any],
    config: dict[str, Any],
) -> list[str]:
    platform = config["platform"]
    processor = config["processor"]
    groups: list[str] = []
    if platform == "linux":
        groups.append("linux")
    elif platform in {"darwin", "bsd"}:
        groups.append("kqueue")
    elif platform == "windows":
        groups.append("windows")
    result = list(manifest["common_sources"])
    for group in groups:
        result.extend(manifest["platform_sources"][group])
    if platform == "linux" and config["research"]:
        result.extend(manifest["research_runtime"]["linux"])
    if platform == "linux" and processor in {"x86_64", "amd64"}:
        result.extend(manifest["platform_sources"]["linux_x86_64"])
        result.extend(
            manifest["platform_sources"]["linux_x86_64_wake"]
        )
    elif platform in {"linux", "bsd"} and processor in {
        "aarch64",
        "arm64",
    }:
        result.extend(manifest["platform_sources"]["context_arm64"])
        result.extend(manifest["platform_sources"]["linux_arm64"])
    elif platform == "bsd" and processor in {"x86_64", "amd64"}:
        result.extend(manifest["platform_sources"]["linux_x86_64"])
    elif platform == "darwin" and processor in {"x86_64", "amd64"}:
        result.extend(manifest["platform_sources"]["darwin_x86_64"])
    elif platform == "darwin" and processor in {"aarch64", "arm64"}:
        result.extend(manifest["platform_sources"]["context_arm64"])
        result.extend(manifest["platform_sources"]["darwin_arm64"])
    elif platform == "windows":
        group = (
            "windows_msvc_x86_64"
            if config["msvc"]
            else "windows_gnu_x86_64"
        )
        result.extend(manifest["platform_sources"][group])
    return result


def tri_and(left: bool | None, right: bool | None) -> bool | None:
    if left is False or right is False:
        return False
    if left is None or right is None:
        return None
    return True


def tri_or(left: bool | None, right: bool | None) -> bool | None:
    if left is True or right is True:
        return True
    if left is None or right is None:
        return None
    return False


def tri_not(value: bool | None) -> bool | None:
    return None if value is None else not value


def make_condition(
    directive: str,
    env: dict[str, str],
) -> bool | None:
    match = re.fullmatch(r"(ifeq|ifneq)\s*\((.*?),(.*?)\)", directive)
    if match is None:
        return None
    operator, left, right = match.groups()

    def value(token: str) -> str | None:
        token = token.strip().strip("\"'")
        variable = re.fullmatch(
            r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
            token,
        )
        if variable:
            return env.get(variable.group(1))
        if "$(" in token:
            return None
        return token

    left_value = value(left)
    right_value = value(right)
    if left_value is None or right_value is None:
        return None
    equal = left_value == right_value
    return equal if operator == "ifeq" else not equal


def make_environment(config: dict[str, Any]) -> dict[str, str]:
    system_names = {
        "linux": "Linux",
        "darwin": "Darwin",
        "bsd": "FreeBSD",
        "windows": "Windows_NT",
    }
    platform = config["platform"]
    return {
        "HOST_PLATFORM": platform,
        "UNAME_S": system_names[platform],
        "UNAME_M": config["processor"],
        "LLAM_BUILD_RESEARCH": str(config["research"]),
        "OS": "Windows_NT" if platform == "windows" else "",
    }


def active_make_lines(
    text: str,
    config: dict[str, Any],
    audit: Audit,
) -> list[str]:
    env = make_environment(config)
    frames: list[dict[str, bool | None]] = []
    active: bool | None = True
    output: list[str] = []
    unknown_reported = False
    for physical in text.splitlines():
        line = strip_unquoted_comment(physical).strip()
        conditional = re.match(
            r"^(ifeq|ifneq|ifdef|ifndef)(?:\s+|$)",
            line,
        )
        if conditional:
            condition = (
                make_condition(line, env)
                if conditional.group(1) in {"ifeq", "ifneq"}
                else None
            )
            frames.append({"parent": active, "prior": condition})
            active = tri_and(active, condition)
            continue
        else_conditional = re.match(
            r"^else\s+(ifeq|ifneq|ifdef|ifndef)(?:\s+|$)",
            line,
        )
        if else_conditional:
            if frames:
                frame = frames[-1]
                condition = (
                    make_condition(line.removeprefix("else "), env)
                    if else_conditional.group(1) in {"ifeq", "ifneq"}
                    else None
                )
                active = tri_and(
                    frame["parent"],
                    tri_and(tri_not(frame["prior"]), condition),
                )
                frame["prior"] = tri_or(frame["prior"], condition)
            continue
        if line == "else":
            if frames:
                frame = frames[-1]
                active = tri_and(
                    frame["parent"],
                    tri_not(frame["prior"]),
                )
                frame["prior"] = True
            continue
        if line == "endif":
            if frames:
                frame = frames.pop()
                active = frame["parent"]
            continue
        if active is True:
            output.append(physical)
        elif (
            active is None
            and not unknown_reported
            and make_line_touches_audited(line, audit)
        ):
            label = (
                "Make audit enforcement"
                if re.match(
                    r"^(?:test|check|audit-build-manifests)(?:\s|:)",
                    line,
                )
                else "Make audited build state"
            )
            audit.error(
                f"{label} is guarded by an unsupported condition "
                f"near: {line}"
            )
            unknown_reported = True
    return output


def make_runtime_sources(
    text: str,
    make: MakeProjection,
    config: dict[str, Any],
    audit: Audit,
) -> list[str]:
    runtime_tokens: list[str] = []
    active_text = "\n".join(active_make_lines(text, config, audit))
    for line in logical_make_lines(active_text):
        assignment = re.match(
            r"^RUNTIME_OBJS\s*(\+=|:=|\?=|=)\s*(.*)$",
            line,
        )
        if assignment is None:
            continue
        operator, value = assignment.groups()
        tokens = value.split()
        if operator == "+=":
            runtime_tokens.extend(tokens)
        elif operator in {"=", ":="}:
            runtime_tokens = tokens
        else:
            audit.error(
                f"Make RUNTIME_OBJS uses unsupported assignment {operator}"
            )

    result: list[str] = []
    allowed_variables = {
        "RUNTIME_COMMON_OBJS",
        *(f"RUNTIME_{group.upper()}_OBJS" for group in PLATFORM_GROUPS),
        "RESEARCH_RUNTIME_LINUX_OBJS",
    }
    for token in runtime_tokens:
        variable = re.fullmatch(
            r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
            token,
        )
        if variable and variable.group(1) in allowed_variables:
            result.extend(make.source_variable(variable.group(1)))
            continue
        source = make.object_to_source(token)
        if source is not None:
            result.append(source)
        audit.error(
            f"Make {config['label']} research={config['research']} "
            f"runtime graph has unsupported token {token}"
        )
    return result


def active_make_text(
    text: str,
    config: dict[str, Any],
    audit: Audit,
) -> str:
    return "\n".join(active_make_lines(text, config, audit)) + "\n"


def make_target_reaches(
    rules: dict[str, list[str]],
    start: str,
    wanted: str,
) -> bool:
    pending = [start]
    visited: set[str] = set()
    while pending:
        target = pending.pop()
        if target == wanted:
            return True
        if target in visited:
            continue
        visited.add(target)
        pending.extend(
            prerequisite
            for prerequisite in rules.get(target, [])
            if not prerequisite.startswith("$(")
        )
    return False


def check_linux_workflow_audit_step(text: str, audit: Audit) -> None:
    lines = text.splitlines()
    matching_step = False
    exact_run = False
    disabled = False
    index = 0
    while index < len(lines):
        match = re.match(
            r"^(\s*)-\s+name:\s+Audit build manifests\s*$",
            lines[index],
        )
        if match is None:
            index += 1
            continue
        matching_step = True
        base_indent = len(match.group(1))
        index += 1
        while index < len(lines):
            line = lines[index]
            next_step = re.match(r"^(\s*)-\s+name:", line)
            if next_step and len(next_step.group(1)) == base_indent:
                break
            field = re.match(r"^\s+(run|if):\s*(.*?)\s*$", line)
            if field:
                name, value = field.groups()
                value = value.strip().strip("\"'")
                if name == "run" and value == (
                    "python3 scripts/audit_build_manifests.py "
                    "--root . --check"
                ):
                    exact_run = True
                elif name == "if":
                    normalized = re.sub(r"\s+", "", value).lower()
                    if normalized in {
                        "false",
                        "0",
                        "no",
                        "off",
                        "${{false}}",
                        "${{0}}",
                    }:
                        disabled = True
                    elif normalized not in {
                        "true",
                        "1",
                        "yes",
                        "on",
                        "${{true}}",
                        "${{1}}",
                    }:
                        audit.error(
                            "Linux CI build-manifest audit step has an "
                            "unsupported condition"
                        )
            index += 1
    if disabled:
        audit.error("Linux CI build-manifest audit step is disabled")
    if not matching_step or not exact_run:
        audit.error("Linux CI build-manifest audit step is missing")


def cmake_condition(
    tokens: list[str],
    env: dict[str, Any],
) -> bool | None:
    if not tokens:
        return None
    if "OR" in tokens:
        index = tokens.index("OR")
        return tri_or(
            cmake_condition(tokens[:index], env),
            cmake_condition(tokens[index + 1 :], env),
        )
    if "AND" in tokens:
        index = tokens.index("AND")
        return tri_and(
            cmake_condition(tokens[:index], env),
            cmake_condition(tokens[index + 1 :], env),
        )
    if tokens[0] == "NOT":
        return tri_not(cmake_condition(tokens[1:], env))
    if len(tokens) == 1:
        token = tokens[0]
        if token in {"TRUE", "ON", "1"}:
            return True
        if token in {"FALSE", "OFF", "0"}:
            return False
        value = env.get(token)
        return bool(value) if value is not None else None
    if len(tokens) == 3 and tokens[1] in {"STREQUAL", "MATCHES"}:
        left = env.get(tokens[0])
        if left is None:
            return None
        right = tokens[2]
        if tokens[1] == "STREQUAL":
            return str(left) == right
        try:
            return re.search(right, str(left)) is not None
        except re.error:
            return None
    return None


def cmake_environment(config: dict[str, Any]) -> dict[str, Any]:
    system_names = {
        "linux": "Linux",
        "darwin": "Darwin",
        "bsd": "FreeBSD",
        "windows": "Windows",
    }
    return {
        "CMAKE_SYSTEM_NAME": system_names[config["platform"]],
        "LLAM_SYSTEM_IS_BSD": config["platform"] == "bsd",
        "LLAM_SYSTEM_IS_KQUEUE": config["platform"] in {"darwin", "bsd"},
        "LLAM_BUILD_RESEARCH": bool(config["research"]),
        "LLAM_TARGET_PROCESSOR": config["processor"],
        "MSVC": bool(config["msvc"]),
        "Python3_Interpreter_FOUND": True,
        "Python3_FOUND": True,
    }


def configured_cmake_commands(
    text: str,
    config: dict[str, Any],
) -> list[tuple[str, str, list[str], bool | None]]:
    env = cmake_environment(config)
    frames: list[dict[str, bool | None]] = []
    active: bool | None = True
    result: list[tuple[str, str, list[str], bool | None]] = []
    for command, body in cmake_commands(uncomment_text(text)):
        tokens = cmake_tokens(body)
        if command == "if":
            condition = cmake_condition(tokens, env)
            frames.append({"parent": active, "prior": condition})
            active = tri_and(active, condition)
            continue
        if command == "elseif":
            if frames:
                frame = frames[-1]
                condition = cmake_condition(tokens, env)
                active = tri_and(
                    frame["parent"],
                    tri_and(tri_not(frame["prior"]), condition),
                )
                frame["prior"] = tri_or(frame["prior"], condition)
            continue
        if command == "else":
            if frames:
                frame = frames[-1]
                active = tri_and(
                    frame["parent"],
                    tri_not(frame["prior"]),
                )
                frame["prior"] = True
            continue
        if command == "endif":
            if frames:
                frame = frames.pop()
                active = frame["parent"]
            continue
        result.append((command, body, tokens, active))
    return result


def configured_cmake_variables(
    text: str,
    config: dict[str, Any],
    audit: Audit,
) -> dict[str, list[str]]:
    variables: dict[str, list[str]] = {}
    for command, body, tokens, active in configured_cmake_commands(
        text,
        config,
    ):
        relevant = (
            command == "set"
            and tokens
            and is_audited_cmake_variable(tokens[0], audit)
        ) or (
            command == "list"
            and len(tokens) >= 2
            and is_audited_cmake_variable(tokens[1], audit)
        )
        if active is None and relevant:
            audit.error(
                "CMake audited variable mutation is guarded by an "
                f"unsupported condition: {command}({body.strip()})"
            )
            continue
        if not active or not tokens:
            continue
        if command == "set":
            variables[tokens[0]] = tokens[1:]
        elif (
            command == "list"
            and len(tokens) >= 2
            and tokens[0] == "APPEND"
        ):
            variables.setdefault(tokens[1], []).extend(tokens[2:])
    return variables


def expand_cmake_values(
    tokens: Iterable[str],
    variables: dict[str, list[str]],
    audit: Audit,
    seen: frozenset[str] = frozenset(),
) -> list[str]:
    result: list[str] = []
    for token in tokens:
        match = re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", token)
        if match and match.group(1) in variables:
            name = match.group(1)
            if name in seen:
                audit.error(f"CMake variable expansion cycle at {name}")
                continue
            result.extend(
                expand_cmake_values(
                    variables[name],
                    variables,
                    audit,
                    seen | {name},
                )
            )
        else:
            result.append(token)
    return result


def cmake_runtime_sources(
    text: str,
    cmake: CMakeProjection,
    config: dict[str, Any],
    audit: Audit,
) -> list[str]:
    runtime_tokens: list[str] = []
    for command, body, tokens, active in configured_cmake_commands(
        text,
        config,
    ):
        touches_runtime = (
            command == "set"
            and tokens
            and tokens[0] == "LLAM_RUNTIME_SOURCES"
        ) or (
            command == "list"
            and len(tokens) >= 2
            and tokens[:2] == ["APPEND", "LLAM_RUNTIME_SOURCES"]
        )
        if not touches_runtime:
            continue
        if active is None:
            audit.error(
                "CMake LLAM_RUNTIME_SOURCES construction is guarded by "
                f"an unsupported condition: {command}({body.strip()})"
            )
            continue
        if not active:
            continue
        if command == "set":
            runtime_tokens = tokens[1:]
        else:
            runtime_tokens.extend(tokens[2:])

    result: list[str] = []
    for token in runtime_tokens:
        variable = re.fullmatch(
            r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}",
            token,
        )
        if variable and variable.group(1) != "LLAM_RUNTIME_SOURCES":
            result.extend(cmake.source_variable(variable.group(1)))
        elif source_suffix(token):
            result.append(token)
            audit.error(
                f"CMake {config['label']} research={config['research']} "
                f"runtime graph has unsupported direct source {token}"
            )
        else:
            audit.error(
                f"CMake {config['label']} research={config['research']} "
                f"runtime graph has unsupported token {token}"
            )
    return result


def active_cmake_tests(
    text: str,
    config: dict[str, Any],
    audit: Audit,
) -> dict[str, list[str]]:
    registrations: dict[str, list[str]] = {}
    disabled: set[str] = set()
    for command, body, tokens, active in configured_cmake_commands(
        text,
        config,
    ):
        relevant = command == "add_test" or (
            command == "set_tests_properties"
            and "build_manifest" in tokens
        ) or (
            command == "set_property"
            and len(tokens) >= 2
            and tokens[0] == "TEST"
            and "build_manifest" in tokens[1:]
        )
        if not relevant:
            continue
        if active is None:
            if command == "add_test":
                audit.error(
                    "CMake add_test is guarded by an unsupported "
                    f"condition: add_test({body.strip()})"
                )
            else:
                audit.error(
                    "CMake test property is guarded by an unsupported "
                    f"condition: {command}({body.strip()})"
                )
            continue
        if not active:
            continue
        if command == "add_test":
            if "NAME" not in tokens:
                continue
            index = tokens.index("NAME")
            if index + 1 >= len(tokens):
                continue
            name = tokens[index + 1]
            if name in registrations:
                audit.error(
                    f"CMake duplicate active CTest registration {name}"
                )
            registrations[name] = tokens
            continue
        property_index = (
            tokens.index("PROPERTIES") + 1
            if "PROPERTIES" in tokens
            else tokens.index("PROPERTY") + 1
            if "PROPERTY" in tokens
            else -1
        )
        if property_index < 1 or property_index + 1 >= len(tokens):
            audit.error(
                "CMake build_manifest test property mutation is unsupported: "
                f"{command}({body.strip()})"
            )
            continue
        property_name = tokens[property_index]
        property_value = tokens[property_index + 1].upper()
        if property_name == "DISABLED" and property_value in {
            "TRUE",
            "ON",
            "1",
            "YES",
        }:
            disabled.add("build_manifest")
            audit.error(
                "CMake build_manifest test is disabled"
            )
        elif (
            command == "set_tests_properties"
            and property_name == "WORKING_DIRECTORY"
            and tokens[property_index + 1]
            == "${CMAKE_CURRENT_SOURCE_DIR}"
            and property_index + 2 == len(tokens)
        ):
            continue
        else:
            audit.error(
                "CMake build_manifest test property mutation is unsupported: "
                f"{command}({body.strip()})"
            )
    for name in disabled:
        registrations.pop(name, None)
    return registrations


def active_cmake_target(
    text: str,
    target: str,
    config: dict[str, Any],
    variables: dict[str, list[str]],
    audit: Audit,
) -> tuple[bool, list[str], list[str]]:
    exists = False
    sources: list[str] = []
    links: list[str] = []
    for command, body, tokens, active in configured_cmake_commands(
        text,
        config,
    ):
        relevant = (
            command == "add_executable"
            and tokens
            and tokens[0] == target
        ) or (
            command == "target_sources"
            and tokens
            and tokens[0] == target
        ) or (
            command == "target_link_libraries"
            and tokens
            and tokens[0] == target
        )
        if not relevant:
            continue
        if active is None:
            audit.error(
                f"CMake {target} is guarded by an unsupported condition: "
                f"{command}({body.strip()})"
            )
            continue
        if not active:
            continue
        if command == "add_executable":
            if exists:
                audit.error(
                    f"CMake duplicate active target definition {target}"
                )
            exists = True
            source_tokens = tokens[1:]
            for token in expand_cmake_values(
                source_tokens,
                variables,
                audit,
            ):
                if source_suffix(token):
                    sources.append(token)
                elif token in {"EXCLUDE_FROM_ALL", "WIN32", "MACOSX_BUNDLE"}:
                    continue
                elif token.startswith("$<"):
                    audit.error(
                        f"CMake {target} has unsupported source token {token}"
                    )
                elif token.startswith("${"):
                    audit.error(
                        f"CMake {target} has unsupported source "
                        f"indirection {token}"
                    )
        elif command == "target_sources":
            for token in expand_cmake_values(
                tokens[1:],
                variables,
                audit,
            ):
                if token in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                    continue
                if source_suffix(token):
                    sources.append(token)
                else:
                    audit.error(
                        f"CMake {target} has unsupported source token {token}"
                    )
        else:
            for token in tokens[1:]:
                if token in {"PRIVATE", "PUBLIC", "INTERFACE"}:
                    continue
                if token.startswith("$<"):
                    audit.error(
                        f"CMake {target} has unsupported link token {token}"
                    )
                    continue
                if token == "${CMAKE_DL_LIBS}":
                    token = "system_dynamic_loader"
                links.append(token)
    return exists, sources, links


def active_cmake_runtime_library(
    text: str,
    target: str,
    config: dict[str, Any],
    runtime_sources: list[str],
    cmake: CMakeProjection,
    audit: Audit,
) -> tuple[bool, list[str]]:
    exists = False
    source_tokens: list[str] = []
    for command, body, tokens, active in configured_cmake_commands(
        text,
        config,
    ):
        relevant = (
            command == "add_library"
            and tokens
            and tokens[0] == target
        ) or (
            command == "target_sources"
            and tokens
            and tokens[0] == target
        )
        if not relevant:
            continue
        if active is None:
            audit.error(
                f"CMake {target} is guarded by an unsupported condition: "
                f"{command}({body.strip()})"
            )
            continue
        if not active:
            continue
        if command == "add_library":
            if exists:
                audit.error(
                    f"CMake duplicate active library definition {target}"
                )
            exists = True
            source_tokens.extend(
                token
                for token in tokens[1:]
                if token
                not in {
                    "STATIC",
                    "SHARED",
                    "MODULE",
                    "OBJECT",
                    "INTERFACE",
                    "EXCLUDE_FROM_ALL",
                    "IMPORTED",
                    "GLOBAL",
                }
            )
        else:
            source_tokens.extend(
                token
                for token in tokens[1:]
                if token not in {"PRIVATE", "PUBLIC", "INTERFACE"}
            )

    result: list[str] = []
    for token in source_tokens:
        if token == "${LLAM_RUNTIME_SOURCES}":
            result.extend(runtime_sources)
            continue
        variable = re.fullmatch(
            r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}",
            token,
        )
        if variable:
            result.extend(cmake.source_variable(variable.group(1)))
        elif source_suffix(token):
            result.append(token)
        else:
            audit.error(
                f"CMake {target} has unsupported source token {token}"
            )
    return exists, result


def expand_platforms(platforms: Iterable[str]) -> set[str]:
    values = set(platforms)
    if values == {"all"}:
        return {"linux", "darwin", "bsd", "windows"}
    return values


def platform_label(platforms: set[str]) -> list[str]:
    if platforms == {"linux", "darwin", "bsd", "windows"}:
        return ["all"]
    return sorted(platforms)


def cmake_target_registration_platforms(
    text: str,
    target: str,
    *,
    research: int,
    audit: Audit,
) -> set[str]:
    result: set[str] = set()
    for platform in ("linux", "darwin", "bsd", "windows"):
        config = {
            "label": f"{platform}-x86_64",
            "platform": platform,
            "processor": "x86_64",
            "msvc": platform == "windows",
            "research": research,
        }
        registrations = active_cmake_tests(text, config, audit)
        if any(
            ctest_registration_targets(tokens, target)
            for tokens in registrations.values()
        ):
            result.add(platform)
    return result


def ctest_registration_targets(tokens: list[str], target: str) -> bool:
    if "NAME" in tokens:
        index = tokens.index("NAME")
        if index + 1 < len(tokens) and tokens[index + 1] == target:
            return True
    target_file = f"$<TARGET_FILE:{target}>"
    return target in tokens or any(target_file in token for token in tokens)


def make_assignment_values(text: str, variable: str) -> list[str]:
    values: list[str] = []
    for line in logical_make_lines(text):
        match = re.match(
            rf"^{re.escape(variable)}\s*(?:\+=|:=|\?=|=)\s*(.*)$",
            line,
        )
        if match:
            values.append(match.group(1).strip())
    return values


def make_recipe_object_variables(recipe: str) -> list[str]:
    return [
        name
        for name in re.findall(
            r"\$\(([A-Za-z_][A-Za-z0-9_]*_OBJS)\)",
            recipe,
        )
    ]


def validate_make_link_recipe_inputs(
    recipe: str,
    target: str,
    expected_variable: str,
    audit: Audit,
) -> list[str]:
    object_variables = make_recipe_object_variables(recipe)
    allowed_expansions = {
        expected_variable,
        "AR",
        "CC",
        "CFLAGS",
        "CPPFLAGS",
        "SHARED_CPPFLAGS",
        "LDFLAGS",
        "SHLIB_LDFLAGS",
        "LDLIBS",
        "WRITE_BUILD_PROVENANCE",
        "OBJDIR",
        "SHARED_OBJDIR",
        "TESTHOOK_OBJDIR",
    }
    for variable in re.findall(
        r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)",
        recipe,
    ):
        if variable not in allowed_expansions:
            audit.error(
                f"{target}: Make link recipe has unknown expansion "
                f"$({variable})"
            )
    literal_objects = re.findall(
        (
            r"\$\((?:OBJDIR|SHARED_OBJDIR|TESTHOOK_OBJDIR)\)/"
            r"[A-Za-z0-9_./%-]+\.o"
        ),
        recipe,
    )
    for literal in literal_objects:
        audit.error(
            f"{target}: Make link recipe has additional input {literal}"
        )
    for token in recipe.split():
        if (
            token.endswith(".o")
            and "$(" not in token
            and token not in literal_objects
        ):
            audit.error(
                f"{target}: Make link recipe has additional input {token}"
            )
    return object_variables


def signature_recipe_emits_depflags(recipe: str) -> bool:
    printf_pattern = re.compile(
        (
            r"""printf\s+['"]DEPFLAGS=%s\\n['"]\s+"""
            r"""['"]\$\(DEPFLAGS\)['"]"""
        )
    )
    for match in printf_pattern.finditer(recipe):
        line_end = recipe.find("\n", match.end())
        if line_end < 0:
            line_end = len(recipe)
        if re.search(
            r""">\s*['"]?\$@['"]?""",
            recipe[match.end() : line_end],
        ):
            return True
        prefix = recipe[: match.start()]
        suffix = recipe[match.end() :]
        inside_group = prefix.rfind("{") > prefix.rfind("}")
        group_redirect = re.search(
            r"""}\s*>\s*['"]\$\$tmp['"]""",
            suffix,
        )
        installs_signature = re.search(
            r"""mv\s+['"]\$\$tmp['"]\s+['"]\$@['"]""",
            suffix,
        )
        if inside_group and group_redirect and installs_signature:
            return True
    return False


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


def compare_ordered(
    expected: Iterable[str],
    actual: Iterable[str],
    label: str,
    audit: Audit,
) -> None:
    expected_values = list(expected)
    actual_values = list(actual)
    if actual_values != expected_values:
        mismatch = next(
            (
                index
                for index, (actual_item, expected_item) in enumerate(
                    zip(actual_values, expected_values, strict=False)
                )
                if actual_item != expected_item
            ),
            min(len(actual_values), len(expected_values)),
        )
        actual_item = (
            actual_values[mismatch]
            if mismatch < len(actual_values)
            else "<end>"
        )
        expected_item = (
            expected_values[mismatch]
            if mismatch < len(expected_values)
            else "<end>"
        )
        audit.error(
            f"{label}: source drift at index {mismatch}: "
            f"{actual_item} != {expected_item} "
            f"(actual count {len(actual_values)}, "
            f"expected count {len(expected_values)})"
        )


def compare_projection_order(
    expected: Iterable[str],
    actual: Iterable[str],
    label: str,
    build_system: str,
    audit: Audit,
) -> None:
    expected_values = list(expected)
    actual_values = list(actual)
    if (
        actual_values == expected_values
        or sorted(actual_values) != sorted(expected_values)
    ):
        return
    mismatch = next(
        index
        for index, (actual_item, expected_item) in enumerate(
            zip(actual_values, expected_values, strict=True)
        )
        if actual_item != expected_item
    )
    audit.error(
        f"{label}: {build_system} order drift at index {mismatch}: "
        f"{actual_values[mismatch]} != {expected_values[mismatch]}"
    )


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
    try:
        root = args.root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        print(
            "audit-build-manifests: root cannot be resolved: "
            f"{exc.__class__.__name__}",
            file=sys.stderr,
        )
        return 1
    try:
        is_directory = root.is_dir()
    except OSError as exc:
        print(
            "audit-build-manifests: root cannot be inspected: "
            f"{exc.__class__.__name__}",
            file=sys.stderr,
        )
        return 1
    if not is_directory:
        print(
            "audit-build-manifests: root is not a directory",
            file=sys.stderr,
        )
        return 1
    return Audit(root).run()


if __name__ == "__main__":
    raise SystemExit(main())
