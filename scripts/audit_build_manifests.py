#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Feralthedogg

"""Audit LLAM's canonical version and source manifests without evaluation."""

from __future__ import annotations

import argparse
from collections import Counter
from collections.abc import Iterable
from dataclasses import dataclass
import hashlib
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
    "SHARED_LOAD_LDLIBS": "Threads::Threads",
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
ALLOWED_CMAKE_COMMANDS = {
    "add_compile_options",
    "add_custom_command",
    "add_dependencies",
    "add_executable",
    "add_library",
    "add_link_options",
    "add_test",
    "check_c_compiler_flag",
    "check_linker_flag",
    "check_symbol_exists",
    "cmake_minimum_required",
    "configure_file",
    "configure_package_config_file",
    "else",
    "elseif",
    "enable_language",
    "enable_testing",
    "endforeach",
    "endfunction",
    "endif",
    "find_package",
    "foreach",
    "function",
    "if",
    "include",
    "install",
    "list",
    "llam_attach_build_provenance",
    "llam_attach_linker_build_provenance",
    "message",
    "option",
    "project",
    "set",
    "set_target_properties",
    "set_tests_properties",
    "target_compile_definitions",
    "target_include_directories",
    "target_link_libraries",
    "unset",
    "write_basic_package_version_file",
}
ALLOWED_CMAKE_BLOCK_HASHES = {
    "22264e4a8d124c967b3d97b9432e12881be5e3c8429a5096ea657cbcfe67a57c",
    "02b0159a7ee10e82cbc2e0d9d620591caa10f4c75bb6cde2b8da3222dde43110",
    "63a95de07095916a0f2023f8775f86e2976b4bcd0ab0607539645b62c7f3ff08",
    "7e93d487270c9f91a8bb1b72e712979bae26f382c4c3ac76903ac0c57620e68e",
    "227103ba7b4aacdb766bab4b1ae5e2dd7c68287d81bab3a8c08067dfd1d9fa03",
    "ac38a919288ba04738ea3bd17421987f9b24bcc4e5b4c890e808f6ee76817c77",
    "a6c8e9dbfd5ed8143138df908699c1adccb9f2749ab9d67fb20759eafc6f1f7a",
    "edaec5fca5613245de63d07a08d44ca638a3c7de260d53ac815f90370dfb58c3",
    "ac815719cfd63cb68fe11733fd0063a75a0ddd4204917f6ef0c45fbffdec3fa2",
}
ALLOWED_SIGNATURE_RECIPE_HASHES = {
    "925e5a08ff79131ba5785a55aa5d4a22e1e51c6cc01e60bc046e01b97acbd920",
    "c990f42521137951b7fe731a1ba3b76b36837ccb5002fd6b85a5f01db34b5795",
    "6516438d7c244e4e8533220d15fa9ffac35e194bc7d82feeb0b01794f021fd8e",
    "819d34e495e4b31aa4884003c614cbe83d24c329a0c9c3bdaf3104849b6268e5",
    "f0a6256ac0f225058f9bef72bcc29af9a28289c20acef1a18b624139aac66377",
    "3e039acabe122a955e80ddf8e72828a429c71942efbbfe37d16569909613feb0",
    "a229dd1c9c7d454941cc91c34d9407a12d98c76cfb185988b9db786f29251a5a",
}
ALLOWED_MAKE_RECIPE_COUNTER_HASHES = {
    # Canonical repository Makefile, including sanitizer positive controls.
    "c727e131116acd03ab90a4f932170f96dc2b9be4c26bc137e6af659fecd3b074",
    # Canonical repository Makefile with the context-switch gateway audit.
    "2d8a66fc6f127c14128cab654d5cc2022249fd2f6fa9ee5015cbd8de108ebaa3",
    # Canonical repository Makefile with LEIR AOT research targets.
    "e7a4de19a165d1ade2d2df9725e6bd3173e7efabaf25fb9a49449ba1ea3db804",
    # Canonical LEIR AOT graph after splitting portable and ownership tests.
    "205dbdd06393328239f4640231970a8772ff7311f9f3780e2f2004609779ea92",
    # Canonical LEIR AOT graph with the strict ring-profile contract test.
    "22157f3d7bb1ca38f695a8fd14fa7b686b284e2ca72464bdcf9b4f2412e15584",
    # Independently derived mini-repository fixture.
    "0cbd49d8804c20bbca8e0cff012aff2977800bd67b3172643ec7dca4f6ac097d",
}
ALLOWED_PACKAGE_BLOCK_HASHES = {
    "PACKAGE ARCHIVE METADATA VALIDATOR": (
        "5c8e03d46938a8a4645c5a33df671b9b6bd40fe659814b91f34a850efc70fd48"
    ),
    "PACKAGE FINALIZATION": (
        "124d27b2af5d16090722834621ec5ebdbde4c7a2abf782fb3aa25fe86a7486af"
    ),
    "PACKAGE CHECKSUM OUTPUT": (
        "b2ff442e5af59fb5b75a7893c9802cbbdd4ba28da8f38f2f589659b4eb98de76"
    ),
}


class DuplicateKeyError(ValueError):
    """Raised when JSON repeats an object key."""


@dataclass(frozen=True)
class MakeLogicalIR:
    text: str
    line: int
    start: int
    end: int
    recipe: bool


@dataclass(frozen=True)
class MakeRuleIR:
    targets: tuple[str, ...]
    prerequisites: str
    header: MakeLogicalIR
    recipes: tuple[MakeLogicalIR, ...]
    inline_recipe: str | None


@dataclass(frozen=True)
class MakeIR:
    """A single, source-located parse of the canonical Makefile."""

    text: str
    logical_lines: tuple[MakeLogicalIR, ...]
    rules: tuple[MakeRuleIR, ...]
    recipe_rule_counts: tuple[tuple[str, int], ...]
    diagnostics: tuple[str, ...]
    consumed: int


@dataclass(frozen=True)
class CMakeCommandIR:
    """One source-located CMake command."""

    command: str
    body: str
    tokens: tuple[str, ...]
    line: int
    start: int
    end: int


@dataclass(frozen=True)
class CMakeIR:
    """A single parse of the canonical CMake input."""

    text: str
    commands: tuple[CMakeCommandIR, ...]
    diagnostics: tuple[str, ...]
    consumed: int


@dataclass(frozen=True)
class YamlMappingIR:
    line: int
    indent: int
    list_item: bool
    key: str
    value: str


class Audit:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.diagnostics: set[str] = set()
        self.manifest_targets: set[str] = set()
        self.manifest_sources: set[str] = set()
        self.audited_make_variables: set[str] = set()
        self.audited_cmake_variables: set[str] = set()
        self.expected_version: str | None = None
        self.expected_abi_major: int | None = None
        self.parse_counts = {"make": 0, "cmake": 0}
        self.make_ir: MakeIR | None = None
        self.cmake_ir: CMakeIR | None = None
        self._configured_cmake_cache: dict[
            tuple[Any, ...],
            list[tuple[str, str, list[str], bool | None]],
        ] = {}
        self._active_make_text_cache: dict[tuple[Any, ...], str] = {}
        self._active_make_entries_cache: dict[
            tuple[Any, ...],
            tuple[MakeLogicalIR, ...],
        ] = {}
        self._make_projection_cache: dict[
            tuple[Any, ...],
            MakeProjection,
        ] = {}

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

    def parse_build_inputs(self, make_text: str, cmake_text: str) -> None:
        if self.make_ir is None:
            self.make_ir = parse_make_ir(make_text)
            self.parse_counts["make"] += 1
            for diagnostic in self.make_ir.diagnostics:
                self.error(diagnostic)
        if self.cmake_ir is None:
            self.cmake_ir = parse_cmake_ir(cmake_text)
            self.parse_counts["cmake"] += 1
            for diagnostic in self.cmake_ir.diagnostics:
                self.error(diagnostic)

    @staticmethod
    def configuration_key(
        source_identity: int,
        config: dict[str, Any],
    ) -> tuple[Any, ...]:
        return (
            source_identity,
            str(config["label"]),
            str(config["platform"]),
            str(config["processor"]),
            bool(config["msvc"]),
            int(config["research"]),
        )

    def active_make_text(
        self,
        text: str,
        config: dict[str, Any],
    ) -> str:
        key = self.configuration_key(
            id(self.make_ir) if self.make_ir is not None else hash(text),
            config,
        )
        if key not in self._active_make_text_cache:
            self._active_make_text_cache[key] = (
                "\n".join(active_make_lines(text, config, self)) + "\n"
            )
        return self._active_make_text_cache[key]

    def active_make_entries(
        self,
        config: dict[str, Any],
    ) -> tuple[MakeLogicalIR, ...]:
        if self.make_ir is None:
            return ()
        key = self.configuration_key(id(self.make_ir), config)
        if key not in self._active_make_entries_cache:
            self._active_make_entries_cache[key] = tuple(
                active_make_ir_entries(self.make_ir, config, self)
            )
        return self._active_make_entries_cache[key]

    def make_projection(
        self,
        text: str,
        config: dict[str, Any],
    ) -> "MakeProjection":
        key = self.configuration_key(
            id(self.make_ir) if self.make_ir is not None else hash(text),
            config,
        )
        if key not in self._make_projection_cache:
            self._make_projection_cache[key] = MakeProjection(
                self.root,
                self.active_make_entries(config),
                self.make_ir.rules if self.make_ir is not None else (),
                self,
            )
        return self._make_projection_cache[key]

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
            logical_make = [
                strip_unquoted_comment(line).strip()
                for line in logical_make_lines(make)
            ]
            version_assignments = [
                line
                for line in logical_make
                if re.match(
                    (
                        r"^(?:(?:override|export|private)\s+)?"
                        r"LLAM_VERSION\s*(?:\+=|:=|\?=|!=|=)"
                    ),
                    line,
                )
            ]
            if version_assignments != [f"LLAM_VERSION ?= {version}"]:
                self.error(
                    "Make LLAM_VERSION must have exactly one canonical "
                    "assignment"
                )
            abi_assignments = [
                line
                for line in logical_make
                if re.match(
                    (
                        r"^(?:(?:override|export|private)\s+)?"
                        r"LLAM_ABI_MAJOR\s*(?:\+=|:=|\?=|!=|=)"
                    ),
                    line,
                )
            ]
            if abi_assignments != [f"LLAM_ABI_MAJOR ?= {abi_major}"]:
                self.error(
                    "Make LLAM_ABI_MAJOR must have exactly one canonical "
                    "assignment"
                )
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
            check_runtime_header_version(
                header,
                components,
                abi_major,
                self,
            )
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
            check_package_version_state(package, self)
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
        self.parse_build_inputs(make_text, cmake_text)
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
        make = MakeProjection(
            self.root,
            self.make_ir.logical_lines if self.make_ir is not None else (),
            self.make_ir.rules if self.make_ir is not None else (),
            self,
        )
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
            shell_segments = split_shell_commands(recipe)
            compiler_commands = [
                segment
                for segment in shell_segments
                if "$(CC)" in segment.split()
                and "-c" in segment.split()
                and "$@" in segment.split()
                and "$<" in segment.split()
            ]
            if (
                len(shell_segments) != 1
                or len(compiler_commands) != 1
                or "$(DEPFLAGS)" not in compiler_commands[0].split()
            ):
                self.error(
                    f"Make {category} compile recipe omits DEPFLAGS: "
                    f"{recipe}"
                )
                self.error(
                    f"Make {category} actual compiler command omits "
                    "DEPFLAGS: "
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
            normalized_signature = "\n".join(
                line.rstrip() for line in signature_recipe.splitlines()
            ).strip()
            signature_hash = hashlib.sha256(
                normalized_signature.encode("utf-8")
            ).hexdigest()
            if signature_hash not in ALLOWED_SIGNATURE_RECIPE_HASHES:
                self.error(
                    f"Make {signature} signature recipe is not exact"
                )
            active_recipe = uncomment_text(signature_recipe)
            if not signature_recipe_emits_depflags(active_recipe):
                self.error(
                    f"Make {signature} does not record DEPFLAGS"
                )
                self.error(
                    f"Make {signature} does not emit DEPFLAGS"
                )
            if signature_recipe_has_later_writer(active_recipe):
                self.error(
                    f"Make {signature} has a later signature writer"
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
            projection = self.make_projection(make_text, config)
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
        if static_rules != [expected_static]:
            self.error(
                "libllam_runtime.a: Make prerequisites are not exact for "
                "RUNTIME_OBJS"
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
            projection = self.make_projection(make_text, config)
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
            allowed_prerequisites = {
                "$(SHARED_RUNTIME_OBJS)",
                f"{shared_targets[0]}.link-signature",
            }
            prerequisites = projection.rules.get(shared_targets[0], [])
            if (
                "$(SHARED_RUNTIME_OBJS)" not in prerequisites
                or any(
                    prerequisite not in allowed_prerequisites
                    for prerequisite in prerequisites
                )
            ):
                self.error(
                    "Make shared runtime link prerequisites are not exact"
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
                make = self.make_projection(make_text, config)
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
            self.expected_version, self.expected_abi_major = version
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


def check_make_global_closure(text: str, audit: Audit) -> None:
    """Reject Make evaluation surfaces before deciding manifest relevance."""

    hardening_declared = bool(
        re.search(r"(?m)^LLAM_HARDENING\s*\?=", text)
    )
    allowed_overrides = {
        (
            "override LLAM_INTERNAL_CPPFLAGS := "
            "-DLLAM_BUILD_RESEARCH=$(LLAM_BUILD_RESEARCH)"
        ),
        "override CPPFLAGS := $(CPPFLAGS) $(LLAM_INTERNAL_CPPFLAGS)",
        (
            "override SHARED_CPPFLAGS := "
            "$(SHARED_CPPFLAGS) $(LLAM_INTERNAL_CPPFLAGS)"
        ),
    }
    if hardening_declared:
        allowed_overrides.update(
            {
                (
                    "override CFLAGS := "
                    "$(CFLAGS) $(LLAM_HARDENING_CFLAGS)"
                ),
                (
                    "override LDLIBS := "
                    "$(LLAM_HARDENING_LINK_FLAGS) $(LDLIBS)"
                ),
                (
                    "override SERVER_FLOOD_LDLIBS := "
                    "$(LLAM_HARDENING_LINK_FLAGS) "
                    "$(SERVER_FLOOD_LDLIBS)"
                ),
                (
                    "override SHARED_LOAD_LDLIBS := "
                    "$(LLAM_HARDENING_LINK_FLAGS) "
                    "$(SHARED_LOAD_LDLIBS)"
                ),
            }
        )
    expected_provenance_body = (
        "define WRITE_BUILD_PROVENANCE",
        '@tmp="$@.llam-build-provenance.$$$$.tmp"; \\',
        (
            "printf 'LLAM_BUILD_RESEARCH=%s\\n' "
            "'$(LLAM_BUILD_RESEARCH)' > \"$$tmp\"; \\"
        ),
        'mv "$$tmp" "$@.llam-build-provenance"',
        "endef",
    )
    logical_entries = (
        audit.make_ir.logical_lines
        if audit.make_ir is not None
        else parse_make_ir(text).logical_lines
    )
    make_ir = audit.make_ir if audit.make_ir is not None else parse_make_ir(text)
    recipe_counter_projection = json.dumps(
        make_ir.recipe_rule_counts,
        separators=(",", ":"),
    )
    recipe_counter_hash = hashlib.sha256(
        recipe_counter_projection.encode("utf-8")
    ).hexdigest()
    if recipe_counter_hash not in ALLOWED_MAKE_RECIPE_COUNTER_HASHES:
        audit.error("Make recipe-bearing rule Counter is not exact")
    for entry in logical_entries:
        clean = strip_unquoted_comment(entry.text).strip()
        if not clean or entry.recipe:
            continue
        if re.match(
            (
                r"^(?:(?:override|export|private)\s+)?"
                r"[A-Za-z_.][A-Za-z0-9_.-]*\s*!="
            ),
            clean,
        ):
            audit.error(
                f"Makefile:{entry.line}: shell assignment is unsupported"
            )
        define = re.match(
            (
                r"^(?:override\s+)?define\s+"
                r"([A-Za-z_][A-Za-z0-9_]*)"
                r"(?:\s*(?:\+=|:=|\?=|=))?\s*$"
            ),
            clean,
        )
        if define and clean != "define WRITE_BUILD_PROVENANCE":
            audit.error(
                f"Makefile:{entry.line}: unsupported global Make "
                "construct: define"
            )
        modifier = re.match(
            r"^(override|export|private)\s+(.+)$",
            clean,
        )
        if modifier and (
            modifier.group(1) != "override"
            or clean not in allowed_overrides
        ):
            audit.error(
                f"Makefile:{entry.line}: unsupported global Make "
                f"construct: {modifier.group(1)}"
            )
        if re.match(
            (
                r"^.+?:\s*(?:(?:private|override|export)\s+)?"
                r"[A-Za-z_][A-Za-z0-9_]*\s*(?:\+=|:=|\?=|!=|=)"
            ),
            clean,
        ):
            audit.error(
                f"Makefile:{entry.line}: target-specific assignment "
                "is unsupported"
            )
        if clean.startswith(".SECONDEXPANSION"):
            audit.error(
                f"Makefile:{entry.line}: unsupported global Make "
                "construct: .SECONDEXPANSION"
            )
        if re.search(r"\$(?:\(value\s+|\{value\s+)", clean):
            audit.error(
                f"Makefile:{entry.line}: unsupported global Make "
                "function value"
            )
    lines = text.splitlines()
    include_count = 0
    override_counts: dict[str, int] = {}
    provenance_definitions = 0
    provenance_invocations = 0
    provenance_locations: list[str] = []
    for rule in make_ir.rules:
        if (
            rule.inline_recipe is not None
            and "WRITE_BUILD_PROVENANCE" in rule.inline_recipe
        ):
            audit.error(
                f"Makefile:{rule.header.line}: unsupported global Make "
                "construct: provenance invocation"
            )
        for recipe in rule.recipes:
            clean_recipe = strip_unquoted_comment(recipe.text).strip()
            if "WRITE_BUILD_PROVENANCE" not in clean_recipe:
                continue
            if recipe.text != "\t$(WRITE_BUILD_PROVENANCE)":
                audit.error(
                    f"Makefile:{recipe.line}: unsupported global Make "
                    "construct: provenance invocation"
                )
                continue
            provenance_invocations += 1
            provenance_locations.extend(rule.targets)
    allowed_computed_rule_variables = {
        "WINDOWS_CMAKE_TARGETS",
        "BUILD_SIGNATURE",
        "SHARED_BUILD_SIGNATURE",
        "TESTHOOK_BUILD_SIGNATURE",
        "BUILD_OBJS",
        "RESEARCH_OBJS",
        "SHARED_RUNTIME_OBJS",
        "TESTHOOK_RUNTIME_OVERRIDE_OBJS",
        "LINK_TARGETS",
        "RESEARCH_LINK_TARGETS",
        "SHLIB_REAL",
        "SHLIB_LINK",
        "SHLIB_SONAME",
        "OBJDIR",
        "SHARED_OBJDIR",
        "TESTHOOK_OBJDIR",
    }
    index = 0
    while index < len(lines):
        physical = lines[index]
        clean = strip_unquoted_comment(physical).strip()
        if not clean:
            index += 1
            continue
        define = re.fullmatch(
            r"(?:override\s+)?define\s+([A-Za-z_][A-Za-z0-9_]*)",
            clean,
        )
        if define:
            provenance_definitions += int(
                define.group(1) == "WRITE_BUILD_PROVENANCE"
            )
            block = [clean]
            cursor = index + 1
            while cursor < len(lines):
                block.append(lines[cursor].strip())
                if lines[cursor].strip() == "endef":
                    break
                cursor += 1
            if tuple(block) != expected_provenance_body:
                audit.error(
                    f"Makefile:{index + 1}: unsupported global Make "
                    "construct: define"
                )
            index = cursor + 1
            continue
        if re.search(
            (
                r"\$(?:\((?:eval|call|foreach)(?:[\s,)])"
                r"|\{(?:eval|call|foreach)(?:[\s,}]))"
            ),
            clean,
        ) or "$($(" in clean or "${${" in clean:
            audit.error(
                f"Makefile:{index + 1}: unsupported global Make construct"
            )
        if physical.startswith("\t"):
            index += 1
            continue
        if re.match(r"^(?:-?include|sinclude)(?:\s|$)", clean):
            if clean != "-include $(ALL_DEPFILES)":
                audit.error(
                    f"Makefile:{index + 1}: unsupported global Make "
                    "construct: include"
                )
            else:
                include_count += 1
        if clean.startswith("override ") and clean not in allowed_overrides:
            audit.error(
                f"Makefile:{index + 1}: unsupported global Make "
                "construct: override"
            )
        elif clean.startswith("override "):
            override_counts[clean] = override_counts.get(clean, 0) + 1
        if re.match(
            (
                r"^[^:=]+:\s*"
                r"[A-Za-z_][A-Za-z0-9_]*\s*(?:\+=|:=|\?=|=)"
            ),
            clean,
        ):
            audit.error(
                f"Makefile:{index + 1}: target-specific assignment "
                "is unsupported"
            )
        assignment = re.match(
            r"^([^:=\s]+)\s*(?:\+=|:=|\?=|=)",
            clean,
        )
        if assignment and (
            "$(" in assignment.group(1) or "${" in assignment.group(1)
        ):
            audit.error(
                f"Makefile:{index + 1}: unsupported global Make construct: "
                "computed assignment"
            )
        rule = (
            re.match(r"^([^:=\s][^:]*)\s*:", clean)
            if assignment is None
            else None
        )
        if rule and "$" in rule.group(1):
            names = set(make_expansion_names(rule.group(1)))
            if (
                "${" in rule.group(1)
                or not names
                or not names <= allowed_computed_rule_variables
            ):
                audit.error(
                    f"Makefile:{index + 1}: unsupported global Make "
                    "construct: computed rule"
                )
        if re.match(r"^(?:export|unexport|undefine|private)\b", clean):
            audit.error(
                f"Makefile:{index + 1}: unsupported global Make construct"
            )
        index += 1
    if include_count != 1:
        audit.error(
            "Make dependency include must be exactly "
            "-include $(ALL_DEPFILES)"
        )
    if (
        set(override_counts) != allowed_overrides
        or any(count != 1 for count in override_counts.values())
    ):
        audit.error("Make override allowlist is not exact")
    expected_locations = [
        "libllam_runtime.a",
        "$(SHLIB_REAL)",
        "$(SHLIB_REAL)",
        "demo",
        "stress",
        "bench",
        "server",
        "server_lossless",
        "server_flood",
    ]
    if (
        provenance_definitions != 1
        or provenance_invocations != 9
        or sorted(provenance_locations) != sorted(expected_locations)
    ):
        audit.error(
            "Make WRITE_BUILD_PROVENANCE definition/invocations are "
            "not exact"
        )
    origin_lines = [
        strip_unquoted_comment(entry.text).strip()
        for entry in logical_entries
        if "origin SHARED_CPPFLAGS" in entry.text
    ]
    if origin_lines != ["ifeq ($(origin SHARED_CPPFLAGS),undefined)"]:
        audit.error("Make origin allowlist is not exact")


def cmake_block_hash(block: Iterable[CMakeCommandIR]) -> str:
    projection = json.dumps(
        [
            (entry.command, list(entry.tokens))
            for entry in block
        ],
        separators=(",", ":"),
    )
    return hashlib.sha256(projection.encode("utf-8")).hexdigest()


def check_cmake_global_closure(text: str, audit: Audit) -> None:
    """Validate every CMake command against the closed structural grammar."""

    commands = list(
        audit.cmake_ir.commands
        if audit.cmake_ir is not None
        else parse_cmake_ir(uncomment_text(text)).commands
    )
    allowed_includes = {
        "GNUInstallDirs",
        "CMakePackageConfigHelpers",
        "CheckCCompilerFlag",
        "CheckLinkerFlag",
        "CheckSymbolExists",
    }
    hardening_declared = any(
        entry.command == "set"
        and entry.tokens
        and entry.tokens[0] == "LLAM_HARDENING"
        for entry in commands
    )
    block_stack: list[tuple[str, int]] = []
    includes: list[str] = []
    function_hashes: list[str] = []
    foreach_hashes: list[str] = []
    provenance_calls: list[tuple[str, str]] = []
    hardening_c_checks: list[tuple[str, ...]] = []
    hardening_link_checks: list[tuple[str, ...]] = []
    hardening_link_options: list[tuple[str, ...]] = []
    for index, entry in enumerate(commands):
        command = entry.command
        tokens = list(entry.tokens)
        if command not in ALLOWED_CMAKE_COMMANDS:
            audit.error(
                f"CMakeLists.txt:{entry.line}: unsupported global CMake "
                f"command {command}()"
            )
            continue
        if command == "include" and (
            len(tokens) != 1 or tokens[0] not in allowed_includes
        ):
            audit.error(
                f"CMakeLists.txt:{entry.line}: unsupported global CMake "
                "command include()"
            )
        elif command == "include":
            includes.append(tokens[0])
        if command == "check_c_compiler_flag":
            hardening_c_checks.append(tuple(tokens))
        elif command == "check_linker_flag":
            hardening_link_checks.append(tuple(tokens))
        elif command == "add_link_options":
            hardening_link_options.append(tuple(tokens))
        if command in {"function", "foreach"}:
            block_stack.append((command, index))
        elif command in {"endfunction", "endforeach"}:
            wanted = "function" if command == "endfunction" else "foreach"
            if not block_stack or block_stack[-1][0] != wanted:
                audit.error(
                    f"CMakeLists.txt:{entry.line}: unsupported global CMake "
                    f"command {command}()"
                )
            else:
                _, start = block_stack.pop()
                block_hash = cmake_block_hash(commands[start : index + 1])
                if wanted == "function":
                    function_hashes.append(block_hash)
                elif wanted == "foreach":
                    foreach_hashes.append(block_hash)
                if block_hash not in ALLOWED_CMAKE_BLOCK_HASHES:
                    audit.error(
                        f"CMakeLists.txt:{commands[start].line}: unsupported "
                        f"global CMake command {wanted}()"
                    )
        if command == "set" and tokens and re.search(r"[$<>{}]", tokens[0]):
            audit.error(
                f"CMakeLists.txt:{entry.line}: unsupported computed CMake "
                "set() name"
            )
        if command == "list" and len(tokens) >= 2 and re.search(
            r"[$<>{}]",
            tokens[1],
        ):
            audit.error(
                f"CMakeLists.txt:{entry.line}: unsupported computed CMake "
                "list() name"
            )
        if command == "unset" and tokens and re.search(
            r"[$<>{}]",
            tokens[0],
        ):
            audit.error(
                f"CMakeLists.txt:{entry.line}: unsupported computed CMake "
                "unset() name"
            )
        if command in {
            "add_dependencies",
            "add_executable",
            "add_library",
            "set_target_properties",
            "target_compile_definitions",
            "target_include_directories",
            "target_link_libraries",
        } and tokens:
            target = tokens[0]
            in_approved_block = bool(block_stack)
            if re.search(r"[$<>{}]", target) and not in_approved_block:
                audit.error(
                    f"CMakeLists.txt:{entry.line}: unsupported indirect "
                    f"target in {command}()"
                )
        if command in {"set_target_properties", "set_tests_properties"}:
            if "PROPERTIES" not in tokens:
                audit.error(
                    f"CMakeLists.txt:{entry.line}: unsupported {command}() "
                    "shape"
                )
            else:
                property_index = tokens.index("PROPERTIES")
                names = tokens[:property_index]
                properties = tokens[property_index + 1 :]
                if (
                    not names
                    or any(re.search(r"[$<>{}]", name) for name in names)
                    or len(properties) % 2
                ):
                    audit.error(
                        f"CMakeLists.txt:{entry.line}: unsupported "
                        f"{command}() projection"
                    )
        if command in {
            "llam_attach_build_provenance",
            "llam_attach_linker_build_provenance",
        }:
            allowed_calls = {
                ("llam_attach_build_provenance", "llam_runtime"),
                ("llam_attach_build_provenance", "llam_runtime_shared"),
                ("llam_attach_build_provenance", "bench"),
                (
                    "llam_attach_linker_build_provenance",
                    "llam_runtime_shared",
                ),
            }
            if len(tokens) != 1 or (command, tokens[0]) not in allowed_calls:
                audit.error(
                    f"CMakeLists.txt:{entry.line}: unsupported global CMake "
                    "provenance invocation"
                )
            elif tokens:
                provenance_calls.append((command, tokens[0]))
    if block_stack:
        audit.error("CMakeLists.txt: unterminated global CMake block")
    expected_includes = [
        "GNUInstallDirs",
        "CMakePackageConfigHelpers",
    ]
    if hardening_declared:
        expected_includes.extend(
            ["CheckCCompilerFlag", "CheckLinkerFlag"]
        )
    expected_includes.append("CheckSymbolExists")
    if includes != expected_includes:
        audit.error("CMake global include projection is not exact")
    expected_c_checks = [
        ("/GS", "LLAM_HARDENING_STACK_PROTECTOR"),
        ("/guard:cf", "LLAM_HARDENING_CONTROL_FLOW_GUARD"),
        (
            "-fstack-protector-strong",
            "LLAM_HARDENING_STACK_PROTECTOR",
        ),
        ("-D_FORTIFY_SOURCE=2", "LLAM_HARDENING_FORTIFY"),
        ("-fstack-clash-protection", "LLAM_HARDENING_STACK_CLASH"),
    ]
    expected_link_checks = [
        ("C", "/guard:cf", "LLAM_HARDENING_CONTROL_FLOW_GUARD_LINK"),
        ("C", "-Wl,-z,relro", "LLAM_HARDENING_RELRO"),
        ("C", "-Wl,-z,now", "LLAM_HARDENING_NOW"),
        ("C", "-Wl,-z,noexecstack", "LLAM_HARDENING_NOEXECSTACK"),
    ]
    expected_link_options = [
        ("/guard:cf",),
        ("-Wl,-z,relro",),
        ("-Wl,-z,now",),
        ("-Wl,-z,noexecstack",),
    ]
    if hardening_declared:
        if hardening_c_checks != expected_c_checks:
            audit.error("CMake hardening compiler checks are not exact")
        if hardening_link_checks != expected_link_checks:
            audit.error("CMake hardening linker checks are not exact")
        if hardening_link_options != expected_link_options:
            audit.error("CMake hardening link options are not exact")
    elif (
        hardening_c_checks
        or hardening_link_checks
        or hardening_link_options
    ):
        audit.error("CMake hardening projection lacks its profile declaration")
    expected_function_hashes = [
        "22264e4a8d124c967b3d97b9432e12881be5e3c8429a5096ea657cbcfe67a57c",
        "02b0159a7ee10e82cbc2e0d9d620591caa10f4c75bb6cde2b8da3222dde43110",
    ]
    if function_hashes != expected_function_hashes:
        audit.error("CMake provenance function definitions are not exact")
    expected_calls = [
        ("llam_attach_build_provenance", "llam_runtime"),
        ("llam_attach_build_provenance", "llam_runtime_shared"),
        ("llam_attach_linker_build_provenance", "llam_runtime_shared"),
        ("llam_attach_build_provenance", "bench"),
    ]
    if provenance_calls != expected_calls:
        audit.error("CMake provenance function invocations are not exact")
    expected_foreach_hashes = [
        "63a95de07095916a0f2023f8775f86e2976b4bcd0ab0607539645b62c7f3ff08",
        "7e93d487270c9f91a8bb1b72e712979bae26f382c4c3ac76903ac0c57620e68e",
        "227103ba7b4aacdb766bab4b1ae5e2dd7c68287d81bab3a8c08067dfd1d9fa03",
        "ac38a919288ba04738ea3bd17421987f9b24bcc4e5b4c890e808f6ee76817c77",
        "a6c8e9dbfd5ed8143138df908699c1adccb9f2749ab9d67fb20759eafc6f1f7a",
        "edaec5fca5613245de63d07a08d44ca638a3c7de260d53ac815f90370dfb58c3",
        "ac815719cfd63cb68fe11733fd0063a75a0ddd4204917f6ef0c45fbffdec3fa2",
    ]
    if foreach_hashes != expected_foreach_hashes:
        audit.error("CMake foreach block multiset is not exact")

    expected_version = audit.expected_version
    expected_abi = audit.expected_abi_major
    projects = [
        list(entry.tokens)
        for entry in commands
        if entry.command == "project"
    ]
    project_valid = (
        len(projects) == 1
        and len(projects[0]) >= 3
        and projects[0][0] == "llam"
        and "VERSION" in projects[0]
        and projects[0][projects[0].index("VERSION") + 1]
        == expected_version
    )
    if not project_valid:
        audit.error("CMake project version projection is not exact")

    def set_writers(name: str) -> list[list[str]]:
        return [
            list(entry.tokens)
            for entry in commands
            if entry.command == "set"
            and entry.tokens
            and entry.tokens[0] == name
        ]

    expected_sets = {
        "LLAM_ABI_VERSION_MAJOR": [str(expected_abi)],
        "LLAM_ABI_VERSION_MINOR": ["0"],
        "LLAM_LIBRARY_VERSION": ["${PROJECT_VERSION}"],
    }
    for name, expected_value in expected_sets.items():
        writers = set_writers(name)
        if len(writers) != 1 or writers[0][1:] != expected_value:
            audit.error(f"CMake {name} projection is not exact")

    version_properties: list[tuple[str, str]] = []
    for entry in commands:
        if (
            entry.command != "set_target_properties"
            or not entry.tokens
            or entry.tokens[0] != "llam_runtime_shared"
            or "PROPERTIES" not in entry.tokens
        ):
            continue
        properties = list(entry.tokens)[
            list(entry.tokens).index("PROPERTIES") + 1 :
        ]
        version_properties.extend(
            (properties[index], properties[index + 1])
            for index in range(0, len(properties) - 1, 2)
            if properties[index] in {"VERSION", "SOVERSION"}
        )
    if version_properties != [
        ("VERSION", "${LLAM_LIBRARY_VERSION}"),
        ("SOVERSION", "${LLAM_ABI_VERSION_MAJOR}"),
    ]:
        audit.error(
            "CMake runtime shared VERSION/SOVERSION projection is not exact"
        )


def check_make_declarative_subset(text: str, audit: Audit) -> None:
    check_make_global_closure(text, audit)
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
    check_cmake_global_closure(text, audit)
    relevant_targets = audit.manifest_targets | set(RUNTIME_LIBRARY_TARGETS)
    commands = [
        (entry.command, entry.body)
        for entry in (
            audit.cmake_ir.commands
            if audit.cmake_ir is not None
            else parse_cmake_ir(uncomment_text(text)).commands
        )
    ]
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
    def __init__(
        self,
        root: Path,
        entries: Iterable[MakeLogicalIR],
        rule_irs: Iterable[MakeRuleIR],
        audit: Audit,
    ) -> None:
        self.root = root
        self.audit = audit
        self.variables: dict[str, list[str]] = {}
        self.rules: dict[str, list[str]] = {}
        self.recipes: dict[str, list[str]] = {}
        active_entries = tuple(entries)
        active_starts = {entry.start for entry in active_entries}
        for entry in active_entries:
            if entry.recipe:
                continue
            line = strip_unquoted_comment(entry.text).strip()
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
        for rule in rule_irs:
            if rule.header.start not in active_starts:
                continue
            prerequisites = rule.prerequisites.split()
            recipes = [
                entry.text[1:]
                for entry in rule.recipes
                if entry.text.startswith("\t")
            ]
            for target in rule.targets:
                self.rules.setdefault(target, []).extend(prerequisites)
                self.recipes.setdefault(target, []).extend(recipes)

    def raw_tokens(self, variable: str) -> list[str]:
        return list(self.variables.get(variable, []))

    def expand_tokens(
        self,
        tokens: Iterable[str],
        seen: frozenset[str] = frozenset(),
    ) -> list[str]:
        expanded: list[str] = []
        for token in tokens:
            name = full_make_variable(token)
            if name and name in self.variables:
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
            name = full_make_variable(token)
            if name and name in MAKE_LINK_VARIABLES:
                continue
            for expanded in self.expand_tokens([token]):
                source = self.object_to_source(expanded)
                if source is not None:
                    result.append(source)
        return result

    def validate_target_prerequisites(self, target: str) -> None:
        for token in self.rules.get(target, []):
            name = full_make_variable(token)
            if name:
                if name not in self.variables and name not in MAKE_LINK_VARIABLES:
                    self.audit.error(
                        f"{target}: Make prerequisites have unknown "
                        f"expansion $({name})"
                    )
                continue
            if self.object_to_source(token) is not None:
                continue
            if build_input_suffix(token):
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
            name = full_make_variable(token)
            if name:
                dependency = MAKE_LINK_VARIABLES.get(name)
                if dependency is not None and dependency not in links:
                    links.append(dependency)
        return links

    def target_recipe_links(self, target: str) -> list[str]:
        links: list[str] = []
        recipe = "\n".join(self.recipes.get(target, []))
        for variable, dependency in MAKE_LINK_VARIABLES.items():
            if (
                f"$({variable})" in recipe or f"${{{variable}}}" in recipe
            ) and dependency not in links:
                links.append(dependency)
        known_library_variables = {
            "LDLIBS",
            "SERVER_FLOOD_LDLIBS",
            "SHARED_LOAD_LDLIBS",
            "DL_LIBS",
        }
        for variable in make_expansion_names(recipe):
            if not variable.endswith(("LDLIBS", "LIBS")):
                continue
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
        object_variables = [
            name
            for name in dict.fromkeys(make_expansion_names(recipe))
            if name.endswith("_OBJS")
        ]
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
        for variable in make_expansion_names(recipe):
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
        commands = (
            audit.cmake_ir.commands
            if audit.cmake_ir is not None
            else parse_cmake_ir(uncomment_text(text)).commands
        )
        for entry in commands:
            command = entry.command
            tokens = list(entry.tokens)
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


def build_input_suffix(value: str) -> bool:
    cleaned = value.strip("'\";,()")
    return re.search(
        r"\.(?:o|obj|a|so(?:\.\d+)*|dylib|dll|lib)$",
        cleaned,
    ) is not None


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


def check_runtime_header_version(
    text: str,
    components: tuple[int, ...],
    abi_major: int,
    audit: Audit,
) -> None:
    without_blocks = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    logical: list[str] = []
    current = ""
    for physical in without_blocks.splitlines():
        line = re.sub(r"//.*$", "", physical).rstrip()
        if line.endswith("\\"):
            current += line[:-1] + " "
            continue
        logical.append((current + line).strip())
        current = ""
    if current:
        logical.append(current.strip())
    expected = {
        "LLAM_VERSION_MAJOR": f"#define LLAM_VERSION_MAJOR {components[0]}U",
        "LLAM_VERSION_MINOR": f"#define LLAM_VERSION_MINOR {components[1]}U",
        "LLAM_VERSION_PATCH": f"#define LLAM_VERSION_PATCH {components[2]}U",
        "LLAM_ABI_VERSION_MAJOR": (
            f"#define LLAM_ABI_VERSION_MAJOR {abi_major}U"
        ),
        "LLAM_ABI_VERSION_MINOR": "#define LLAM_ABI_VERSION_MINOR 0U",
        "LLAM_ABI_VERSION": (
            "#define LLAM_ABI_VERSION "
            "((LLAM_ABI_VERSION_MAJOR << 16U) | LLAM_ABI_VERSION_MINOR)"
        ),
    }
    for name, canonical in expected.items():
        directives = [
            line
            for line in logical
            if re.match(
                rf"^#\s*(?:define|undef)\s+{re.escape(name)}\b",
                line,
            )
        ]
        if directives != [canonical]:
            audit.error(
                f"runtime.h {name}: expected exactly one canonical "
                "definition"
            )


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


def fold_shell_logical_lines(text: str) -> list[str]:
    logical: list[str] = []
    current = ""
    for physical in text.splitlines():
        clean = strip_unquoted_comment(physical).strip()
        if clean.endswith("\\"):
            current += clean[:-1]
            continue
        logical.append(current + clean)
        current = ""
    if current:
        logical.append(current)
    return logical


def package_marked_block(
    text: str,
    label: str,
    audit: Audit,
) -> tuple[int, int] | None:
    physical = text.splitlines()
    begin_marker = f"# AUDIT:BEGIN {label}"
    end_marker = f"# AUDIT:END {label}"
    begins = [
        index for index, line in enumerate(physical)
        if line == begin_marker
    ]
    ends = [
        index for index, line in enumerate(physical)
        if line == end_marker
    ]
    if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
        audit.error(f"package {label.lower()} markers are not exact")
        return None
    begin = begins[0]
    end = ends[0]
    normalized = tuple(
        line
        for line in fold_shell_logical_lines(
            "\n".join(physical[begin + 1 : end])
        )
        if line
    )
    projection = json.dumps(normalized, separators=(",", ":"))
    block_hash = hashlib.sha256(projection.encode("utf-8")).hexdigest()
    if block_hash != ALLOWED_PACKAGE_BLOCK_HASHES[label]:
        audit.error(f"package {label.lower()} block is not exact")
    return begin, end


def check_package_version_state(text: str, audit: Audit) -> None:
    expected_preamble = (
        'version="${LLAM_RELEASE_VERSION:-${GITHUB_REF_NAME:-v2.2.0}}"',
        'version="${version#v}"',
        'abi_major="${LLAM_ABI_MAJOR:-2}"',
        'library_version="${LLAM_VERSION:-2.2.0}"',
    )
    lines = [
        line for line in fold_shell_logical_lines(text) if line
    ]
    marked_blocks = {
        label: package_marked_block(text, label, audit)
        for label in ALLOWED_PACKAGE_BLOCK_HASHES
    }
    finalization = marked_blocks["PACKAGE FINALIZATION"]
    checksum_output = marked_blocks["PACKAGE CHECKSUM OUTPUT"]
    physical = text.splitlines()
    if (
        finalization is not None
        and checksum_output is not None
        and finalization[1] + 1 != checksum_output[0]
    ):
        audit.error(
            "package finalization and checksum/output tail are not adjacent"
        )
    if (
        checksum_output is not None
        and any(line.strip() for line in physical[checksum_output[1] + 1 :])
    ):
        audit.error("package commands follow the sealed checksum/output tail")
    validator_definition = "validate_packaged_archive_metadata() ("
    validator_call = (
        'validate_packaged_archive_metadata "$archive" "$package_name"'
    )
    if lines.count(validator_definition) != 1:
        audit.error(
            "package archive metadata validator definition is not exact"
        )
    if lines.count(validator_call) != 1:
        audit.error("package archive metadata validator call is not exact")
    starts = [
        index
        for index in range(
            max(0, len(lines) - len(expected_preamble) + 1)
        )
        if tuple(lines[index : index + len(expected_preamble)])
        == expected_preamble
    ]
    if len(starts) != 1:
        audit.error("package version preamble is not exact")
    start = starts[0] if len(starts) == 1 else -1
    protected = {"version", "abi_major", "library_version"}
    assignments: list[str] = []
    for index, line in enumerate(lines):
        if not line:
            continue
        assignment_matches = list(
            re.finditer(
                (
                    r"(?:^|[;{]\s*)"
                    r"(version|abi_major|library_version)\s*="
                ),
                line,
            )
        )
        if assignment_matches:
            assignments.append(line)
        command = re.match(r"^(?:unset|export|read|eval)\b(.*)$", line)
        in_preamble = (
            start >= 0
            and start <= index < start + len(expected_preamble)
            and line == expected_preamble[index - start]
        )
        if (assignment_matches and not in_preamble) or (
            command and line.startswith("eval")
        ) or (
            command
            and any(
                re.search(rf"\b{re.escape(name)}\b", command.group(1))
                for name in protected
            )
        ):
            audit.error(
                "package version state uses unsupported mutation "
                f"at line {index + 1}"
            )
    if assignments != list(expected_preamble):
        audit.error("package version assignment multiset is not exact")
    readonly = "readonly version abi_major library_version"
    readonly_indices = [
        index for index, line in enumerate(lines) if line == readonly
    ]
    if readonly_indices != (
        [start + len(expected_preamble)] if start >= 0 else []
    ):
        audit.error("package version readonly projection is not exact")
    expected_writers = {
        "VERSION": 'printf \'%s\\n\' "$version" > "$stage/VERSION"',
        "ABI_MAJOR": (
            'printf \'%s\\n\' "$abi_major" > "$stage/ABI_MAJOR"'
        ),
        "LIBRARY_VERSION": (
            'printf \'%s\\n\' "$library_version" '
            '> "$stage/LIBRARY_VERSION"'
        ),
    }
    for filename, canonical in expected_writers.items():
        writers = [
            line
            for line in lines
            if re.search(
                rf">\s*['\"]?\$stage/{filename}['\"]?(?:\s|$)",
                line,
            )
        ]
        if writers != [canonical]:
            audit.error(
                f"package {filename} writer projection is not exact"
            )
    expected_readback = (
        'if [ "$(cat "$stage/VERSION")" != "$version" ] || '
        '[ "$(cat "$stage/ABI_MAJOR")" != "$abi_major" ] || '
        '[ "$(cat "$stage/LIBRARY_VERSION")" != '
        '"$library_version" ]; then'
    )
    allowed_metadata_references = {
        *expected_writers.values(),
        expected_readback,
    }
    metadata_path = re.compile(
        (
            r"\$(?:stage\b|\{stage\})\"?/"
            r"(?:VERSION|ABI_MAJOR|LIBRARY_VERSION)\b"
        )
    )
    for index, line in enumerate(lines):
        if (
            metadata_path.search(line)
            and line not in allowed_metadata_references
        ):
            audit.error(
                "package version metadata has unsupported executable "
                f"reference at line {index + 1}"
            )
    protected_pattern = r"(?:version|abi_major|library_version)"
    for index, line in enumerate(lines):
        if re.search(
            rf"(?:^|[;]\s*)for\s+{protected_pattern}\s+in\b",
            line,
        ) or re.search(
            rf"(?:^|[;&|]\s*)read(?:\s+\S+)*\s+{protected_pattern}\b",
            line,
        ):
            audit.error(
                "package version state uses unsupported loop/read "
                f"mutation at line {index + 1}"
            )
    expected_fallback_archive = (
        'tar -C "$out_dir" -cf - "$package_name" | '
        'xz -z -c > "$archive"'
    )
    expected_archive = (
        'if ! tar -C "$out_dir" -cJf "$archive" '
        '"$package_name" 2>/dev/null; then'
    )
    archive_writers = [
        line
        for line in lines
        if (
            re.search(r"\$(?:\{archive\}|archive\b)", line)
            and (
                re.match(r"^(?:if ! )?tar\b", line)
                or re.search(r">\s*\"\$archive\"(?:\s|$)", line)
            )
        )
    ]
    if archive_writers not in (
        [expected_archive],
        [expected_archive, expected_fallback_archive],
    ):
        audit.error("package archive writer inventory is not exact")


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


def parse_make_rule_header(
    entry: MakeLogicalIR,
) -> tuple[tuple[str, ...], str, str | None] | None:
    clean = strip_unquoted_comment(entry.text).strip()
    match = re.match(r"^([^:=\s][^:]*?)\s*:(?!=)\s*(.*)$", clean)
    if match is None:
        return None
    targets_text, remainder = match.groups()
    inline_recipe: str | None = None
    if ";" in remainder:
        remainder, inline_recipe = remainder.split(";", 1)
        inline_recipe = inline_recipe.strip()
    targets = tuple(targets_text.split())
    if not targets:
        return None
    return targets, remainder.strip(), inline_recipe


def parse_make_ir(text: str) -> MakeIR:
    logical: list[MakeLogicalIR] = []
    diagnostics: list[str] = []
    physical_lines = text.splitlines(keepends=True)
    offset = 0
    index = 0
    while index < len(physical_lines):
        first = physical_lines[index]
        start = offset
        line_number = index + 1
        recipe = first.startswith("\t")
        pieces: list[str] = []
        while True:
            physical = physical_lines[index]
            content = physical.rstrip("\r\n")
            continued = content.endswith("\\")
            pieces.append(content[:-1] if continued else content)
            offset += len(physical)
            index += 1
            if continued and index >= len(physical_lines):
                diagnostics.append(
                    f"Makefile:{line_number}: unterminated continuation"
                )
                break
            if not continued:
                break
            pieces.append(" ")
        folded = "".join(pieces)
        logical.append(
            MakeLogicalIR(
                text=folded,
                line=line_number,
                start=start,
                end=offset,
                recipe=recipe,
            )
        )

    condition_stack: list[tuple[str, int, bool]] = []
    rule_builders: list[dict[str, Any]] = []
    current_rule: dict[str, Any] | None = None
    define_line: int | None = None
    for entry in logical:
        clean = strip_unquoted_comment(entry.text).strip()
        if define_line is not None:
            if re.fullmatch(r"endef", clean):
                define_line = None
            continue
        if entry.recipe:
            if current_rule is None:
                diagnostics.append(
                    f"Makefile:{entry.line}: recipe has no owning rule"
                )
            else:
                current_rule["recipes"].append(entry)
            continue
        if not clean:
            continue
        if re.match(r"^(?:override\s+)?define(?:\s|$)", clean):
            define_line = entry.line
            current_rule = None
            continue
        if clean == "endef":
            diagnostics.append(
                f"Makefile:{entry.line}: orphan endef"
            )
            current_rule = None
            continue
        conditional = re.match(
            r"^(ifeq|ifneq|ifdef|ifndef)(?:\s|$)",
            clean,
        )
        if conditional:
            condition_stack.append(
                (conditional.group(1), entry.line, False)
            )
            current_rule = None
            continue
        if re.match(r"^else(?:\s|$)", clean):
            if not condition_stack:
                diagnostics.append(
                    f"Makefile:{entry.line}: orphan else"
                )
            else:
                name, line, seen_else = condition_stack[-1]
                bare_else = clean == "else"
                if seen_else and bare_else:
                    diagnostics.append(
                        f"Makefile:{entry.line}: duplicate else"
                    )
                condition_stack[-1] = (
                    name,
                    line,
                    seen_else or bare_else,
                )
            current_rule = None
            continue
        if clean == "endif":
            if not condition_stack:
                diagnostics.append(
                    f"Makefile:{entry.line}: orphan endif"
                )
            else:
                condition_stack.pop()
            current_rule = None
            continue
        assignment = re.match(
            (
                r"^(?:(?:override|export|private)\s+)?"
                r"[A-Za-z_.][A-Za-z0-9_.-]*\s*"
                r"(?:\+=|:=|\?=|!=|=)"
            ),
            clean,
        )
        parsed_rule = parse_make_rule_header(entry)
        directive = re.match(
            r"^(?:-?include|sinclude|export|unexport|undefine)(?:\s|$)",
            clean,
        )
        standalone_function = re.fullmatch(
            r"\$\((?:error|warning|info)\b.*\)",
            clean,
        )
        if parsed_rule is not None and assignment is None:
            targets, prerequisites, inline_recipe = parsed_rule
            current_rule = {
                "targets": targets,
                "prerequisites": prerequisites,
                "header": entry,
                "recipes": [],
                "inline_recipe": inline_recipe,
            }
            rule_builders.append(current_rule)
            if inline_recipe is not None:
                diagnostics.append(
                    f"Makefile:{entry.line}: inline recipes are unsupported"
                )
        else:
            current_rule = None
            if not assignment and not directive and not standalone_function:
                diagnostics.append(
                    f"Makefile:{entry.line}: unknown structural syntax: "
                    f"{clean}"
                )
    if define_line is not None:
        diagnostics.append(
            f"Makefile:{define_line}: unterminated define"
        )
    for name, line, _ in condition_stack:
        diagnostics.append(
            f"Makefile:{line}: unterminated {name}"
        )
    rules = tuple(
        MakeRuleIR(
            targets=builder["targets"],
            prerequisites=builder["prerequisites"],
            header=builder["header"],
            recipes=tuple(builder["recipes"]),
            inline_recipe=builder["inline_recipe"],
        )
        for builder in rule_builders
    )
    recipe_counts = Counter(
        target
        for rule in rules
        if rule.recipes or rule.inline_recipe is not None
        for target in rule.targets
    )
    return MakeIR(
        text=text,
        logical_lines=tuple(logical),
        rules=rules,
        recipe_rule_counts=tuple(sorted(recipe_counts.items())),
        diagnostics=tuple(diagnostics),
        consumed=len(text),
    )


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


def active_make_ir_entries(
    make_ir: MakeIR,
    config: dict[str, Any],
    audit: Audit,
) -> list[MakeLogicalIR]:
    env = make_environment(config)
    frames: list[dict[str, bool | None]] = []
    active: bool | None = True
    output: list[MakeLogicalIR] = []
    unknown_reported = False
    define_depth = 0
    for entry in make_ir.logical_lines:
        line = strip_unquoted_comment(entry.text).strip()
        if define_depth:
            if line == "endef":
                define_depth -= 1
            if active is True:
                output.append(entry)
            continue
        if re.match(r"^(?:override\s+)?define(?:\s|$)", line):
            define_depth += 1
            if active is True:
                output.append(entry)
            continue
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
            output.append(entry)
        elif (
            active is None
            and not unknown_reported
            and make_line_touches_audited(line, audit)
        ):
            audit.error(
                f"Make {config['label']} research={config['research']} "
                "audit enforcement is guarded by an unsupported condition"
            )
            unknown_reported = True
    return output


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
    active_text = audit.active_make_text(text, config)
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
    return audit.active_make_text(text, config)


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


def strip_yaml_comment(line: str) -> str:
    quote = ""
    escaped = False
    for index, char in enumerate(line):
        if escaped:
            escaped = False
            continue
        if char == "\\" and quote == '"':
            escaped = True
            continue
        if quote:
            if char == quote:
                quote = ""
            continue
        if char in {"'", '"'}:
            quote = char
            continue
        if char == "#":
            return line[:index]
    return line


def normalize_yaml_scalar(value: str) -> str | None:
    value = value.strip()
    if not value:
        return ""
    if value.startswith('"'):
        try:
            parsed = json.loads(value)
        except (json.JSONDecodeError, TypeError):
            return None
        return parsed if isinstance(parsed, str) else None
    if value.startswith("'"):
        if len(value) < 2 or not value.endswith("'"):
            return None
        return value[1:-1].replace("''", "'")
    return value


def parse_yaml_mapping_ir(text: str) -> tuple[list[str], list[YamlMappingIR]]:
    lines = [strip_yaml_comment(line).rstrip() for line in text.splitlines()]
    entries: list[YamlMappingIR] = []
    key_pattern = (
        r'(?:"(?:\\.|[^"])*"|\'(?:\'\'|[^\'])*\'|'
        r"[A-Za-z0-9_-]+|<<)"
    )
    for line_number, line in enumerate(lines, 1):
        match = re.match(
            (
                rf"^(?P<indent> *)(?P<list>-\s+)?"
                rf"(?P<key>{key_pattern})\s*:\s*"
                r"(?P<value>.*)$"
            ),
            line,
        )
        if match is None:
            continue
        key = normalize_yaml_scalar(match.group("key"))
        value = normalize_yaml_scalar(match.group("value"))
        if key is None or value is None:
            continue
        entries.append(
            YamlMappingIR(
                line=line_number,
                indent=len(match.group("indent")),
                list_item=match.group("list") is not None,
                key=key,
                value=value,
            )
        )
    return lines, entries


def yaml_mapping_range_end(
    lines: list[str],
    start: int,
    indent: int,
) -> int:
    for index in range(start + 1, len(lines)):
        line = lines[index]
        if not line.strip():
            continue
        line_indent = len(line) - len(line.lstrip())
        if line_indent <= indent:
            return index
    return len(lines)


def check_linux_workflow_audit_step(text: str, audit: Audit) -> None:
    lines, entries = parse_yaml_mapping_ir(text)
    entries_by_line = {entry.line - 1: entry for entry in entries}
    expected_run = (
        "python3 scripts/audit_build_manifests.py --root . --check"
    )
    candidates = [
        entry
        for entry in entries
        if entry.list_item
        and entry.key == "name"
        and entry.value == "Audit build manifests"
    ]
    valid_steps = 0
    for candidate in candidates:
        step_start = candidate.line - 1
        step_end = yaml_mapping_range_end(
            lines,
            step_start,
            candidate.indent,
        )
        job = next(
            (
                entry
                for entry in reversed(entries)
                if entry.line < candidate.line
                and not entry.list_item
                and entry.indent == 2
                and entry.value == ""
            ),
            None,
        )
        if job is None:
            audit.error(
                "Linux CI build-manifest audit step has no literal job"
            )
            continue
        job_start = job.line - 1
        job_end = yaml_mapping_range_end(lines, job_start, job.indent)
        job_fields = [
            entry
            for entry in entries
            if job_start < entry.line - 1 < job_end
            and not entry.list_item
            and entry.indent == job.indent + 2
        ]
        step_fields = [
            candidate,
            *(
                entry
                for entry in entries
                if step_start < entry.line - 1 < step_end
                and not entry.list_item
                and entry.indent == candidate.indent + 2
            ),
        ]
        valid = True
        for label, fields in (
            ("job", job_fields),
            ("audit step", step_fields),
        ):
            counts = Counter(field.key for field in fields)
            duplicates = sorted(
                key for key, count in counts.items() if count != 1
            )
            if duplicates:
                valid = False
                audit.error(
                    "Linux CI build-manifest "
                    f"{label} has duplicate YAML keys: "
                    + ", ".join(duplicates)
                )
            for field in fields:
                if (
                    field.key == "<<"
                    or field.value.startswith(("*", "&"))
                    or field.value in {"|", "|-", "|+", ">", ">-", ">+"}
                ):
                    valid = False
                    audit.error(
                        "Linux CI build-manifest "
                        f"{label} uses unsupported YAML indirection"
                    )
        job_keys = Counter(field.key for field in job_fields)
        step_keys = Counter(field.key for field in step_fields)
        if job_keys["if"]:
            valid = False
            audit.error(
                "Linux CI build-manifest job is disabled or uses "
                "an unsupported condition"
            )
        if step_keys["if"]:
            valid = False
            audit.error(
                "Linux CI build-manifest audit step has an "
                "unsupported condition"
            )
        if step_keys["continue-on-error"]:
            valid = False
            audit.error(
                "Linux CI build-manifest audit step uses "
                "continue-on-error"
            )
        names = [
            field.value for field in step_fields if field.key == "name"
        ]
        runs = [
            field.value for field in step_fields if field.key == "run"
        ]
        if names != ["Audit build manifests"] or runs != [expected_run]:
            valid = False
        for line_index in range(job_start + 1, job_end):
            line = lines[line_index]
            indentation = len(line) - len(line.lstrip())
            if (
                line.strip()
                and indentation == job.indent + 2
                and line_index not in entries_by_line
            ):
                valid = False
                audit.error(
                    "Linux CI build-manifest job uses unsupported YAML"
                )
        for line_index in range(step_start + 1, step_end):
            line = lines[line_index]
            indentation = len(line) - len(line.lstrip())
            if (
                line.strip()
                and indentation == candidate.indent + 2
                and line_index not in entries_by_line
            ):
                valid = False
                audit.error(
                    "Linux CI build-manifest audit step uses "
                    "unsupported YAML"
                )
        if valid:
            valid_steps += 1
    if len(candidates) != 1 or valid_steps != 1:
        audit.error("Linux CI build-manifest audit step is disabled")
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
    audit: Audit,
) -> list[tuple[str, str, list[str], bool | None]]:
    cache_key = audit.configuration_key(
        id(audit.cmake_ir) if audit.cmake_ir is not None else hash(text),
        config,
    )
    cached = audit._configured_cmake_cache.get(cache_key)
    if cached is not None:
        return cached
    env = cmake_environment(config)
    frames: list[dict[str, bool | None]] = []
    active: bool | None = True
    result: list[tuple[str, str, list[str], bool | None]] = []
    commands = (
        audit.cmake_ir.commands
        if audit.cmake_ir is not None
        else parse_cmake_ir(uncomment_text(text)).commands
    )
    for entry in commands:
        command = entry.command
        body = entry.body
        tokens = list(entry.tokens)
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
    audit._configured_cmake_cache[cache_key] = result
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
        audit,
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
        audit,
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
        audit,
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
        audit,
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
        audit,
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


def make_expansion_names(text: str) -> list[str]:
    return [
        parenthesized or braced
        for parenthesized, braced in re.findall(
            (
                r"\$\(([A-Za-z_][A-Za-z0-9_]*)\)"
                r"|\$\{([A-Za-z_][A-Za-z0-9_]*)\}"
            ),
            text,
        )
    ]


def full_make_variable(token: str) -> str | None:
    match = re.fullmatch(
        (
            r"(?:\$\(([A-Za-z_][A-Za-z0-9_]*)\)"
            r"|\$\{([A-Za-z_][A-Za-z0-9_]*)\})"
        ),
        token,
    )
    if match is None:
        return None
    return match.group(1) or match.group(2)


def make_recipe_object_variables(recipe: str) -> list[str]:
    return [
        name
        for name in make_expansion_names(recipe)
        if name.endswith("_OBJS")
    ]


def split_shell_commands(recipe: str) -> list[str]:
    commands: list[str] = []
    start = 0
    index = 0
    quote = ""
    escaped = False
    while index < len(recipe):
        char = recipe[index]
        if escaped:
            escaped = False
            index += 1
            continue
        if char == "\\" and quote != "'":
            escaped = True
            index += 1
            continue
        if quote:
            if char == quote:
                quote = ""
            index += 1
            continue
        if char in {"'", '"'}:
            quote = char
            index += 1
            continue
        boundary = 0
        if recipe.startswith("&&", index) or recipe.startswith("||", index):
            boundary = 2
        elif char in {";", "|"}:
            boundary = 1
        if boundary:
            command = recipe[start:index].strip()
            if command:
                commands.append(command)
            index += boundary
            start = index
            continue
        index += 1
    command = recipe[start:].strip()
    if command:
        commands.append(command)
    return commands


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
    for variable in make_expansion_names(recipe):
        if variable not in allowed_expansions:
            audit.error(
                f"{target}: Make link recipe has unknown expansion "
                f"$({variable})"
            )
    for automatic in re.findall(r"\$[\^+?*<|%]", recipe):
        audit.error(
            f"{target}: Make link recipe has noncanonical automatic "
            f"input {automatic}"
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
        cleaned = token.strip("'\";,()")
        if (
            build_input_suffix(cleaned)
            and "$(" not in cleaned
            and "${" not in cleaned
            and cleaned not in literal_objects
        ):
            audit.error(
                f"{target}: Make link recipe has additional input {cleaned}"
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


def signature_recipe_has_later_writer(recipe: str) -> bool:
    direct = re.search(
        (
            r"""printf\s+['"]DEPFLAGS=%s\\n['"].*?"""
            r""">\s*['"]?\$@['"]?"""
        ),
        recipe,
    )
    installed = list(
        re.finditer(
            r"""mv\s+['"]\$\$tmp['"]\s+['"]\$@['"]""",
            recipe,
        )
    )
    if installed:
        writer_end = installed[-1].end()
    elif direct:
        writer_end = direct.end()
    else:
        return False
    suffix = recipe[writer_end:]
    return re.search(
        (
            r"""(?:>|>>)\s*['"]?\$@['"]?"""
            r"""|mv\s+\S+\s+['"]?\$@['"]?"""
        ),
        suffix,
    ) is not None


def parse_cmake_ir(text: str) -> CMakeIR:
    commands: list[CMakeCommandIR] = []
    diagnostics: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        if text[index].isspace():
            index += 1
            continue
        if text[index] == "#":
            bracket = cmake_bracket_delimiter(text, index + 1)
            if bracket is not None:
                opener_end, closer = bracket
                close = text.find(closer, opener_end)
                if close < 0:
                    diagnostics.append(
                        f"CMakeLists.txt:{text.count(chr(10), 0, index) + 1}: "
                        "unterminated bracket comment"
                    )
                    index = length
                else:
                    index = close + len(closer)
                continue
            newline = text.find("\n", index)
            index = length if newline < 0 else newline + 1
            continue
        start = index
        identifier = re.match(
            r"[A-Za-z_][A-Za-z0-9_]*",
            text[index:],
        )
        if identifier is None:
            line = text.count("\n", 0, index) + 1
            diagnostics.append(
                f"CMakeLists.txt:{line}: unconsumed source byte "
                f"{text[index]!r}"
            )
            index += 1
            continue
        name = identifier.group(0)
        index += len(name)
        while index < length and text[index].isspace():
            index += 1
        if index >= length or text[index] != "(":
            line = text.count("\n", 0, start) + 1
            diagnostics.append(
                f"CMakeLists.txt:{line}: command {name} is missing '('"
            )
            newline = text.find("\n", index)
            index = length if newline < 0 else newline + 1
            continue
        body_start = index + 1
        depth = 1
        cursor = body_start
        quoted = False
        escaped = False
        argument_boundary = True
        bracket_argument_ended = False
        while cursor < length and depth:
            char = text[cursor]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
                cursor += 1
                continue
            if char == '"':
                if bracket_argument_ended:
                    diagnostics.append(
                        f"CMakeLists.txt:"
                        f"{text.count(chr(10), 0, cursor) + 1}: "
                        "bracket argument has an unseparated trailing token"
                    )
                    bracket_argument_ended = False
                quoted = True
                argument_boundary = False
                cursor += 1
                continue
            if char.isspace():
                argument_boundary = True
                bracket_argument_ended = False
                cursor += 1
                continue
            if char == "#":
                bracket = cmake_bracket_delimiter(text, cursor + 1)
                if bracket is not None:
                    opener_end, closer = bracket
                    close = text.find(closer, opener_end)
                    if close < 0:
                        cursor = length
                        break
                    cursor = close + len(closer)
                    argument_boundary = True
                    bracket_argument_ended = False
                    continue
                newline = text.find("\n", cursor)
                cursor = length if newline < 0 else newline + 1
                argument_boundary = True
                bracket_argument_ended = False
                continue
            bracket = cmake_bracket_delimiter(text, cursor)
            if bracket is not None:
                if not argument_boundary:
                    diagnostics.append(
                        f"CMakeLists.txt:"
                        f"{text.count(chr(10), 0, cursor) + 1}: "
                        "bracket argument opener is not at an argument "
                        "boundary"
                    )
                opener_end, closer = bracket
                close = text.find(closer, opener_end)
                if close < 0:
                    cursor = length
                    break
                cursor = close + len(closer)
                argument_boundary = False
                bracket_argument_ended = True
                continue
            if bracket_argument_ended and char != ")":
                diagnostics.append(
                    f"CMakeLists.txt:"
                    f"{text.count(chr(10), 0, cursor) + 1}: "
                    "bracket argument has an unseparated trailing token"
                )
                bracket_argument_ended = False
            if char == "(":
                depth += 1
                argument_boundary = True
            elif char == ")":
                depth -= 1
                argument_boundary = False
            else:
                argument_boundary = False
            cursor += 1
        if depth:
            line = text.count("\n", 0, start) + 1
            diagnostics.append(
                f"CMakeLists.txt:{line}: unterminated command {name}()"
            )
            index = length
            continue
        body = text[body_start : cursor - 1]
        commands.append(
            CMakeCommandIR(
                command=name.lower(),
                body=body,
                tokens=tuple(cmake_tokens(body)),
                line=text.count("\n", 0, start) + 1,
                start=start,
                end=cursor,
            )
        )
        index = cursor

    stack: list[tuple[str, int, bool]] = []
    openers = {"if", "foreach", "function", "macro"}
    closers = {
        "endif": "if",
        "endforeach": "foreach",
        "endfunction": "function",
        "endmacro": "macro",
    }
    for entry in commands:
        if entry.command in openers:
            stack.append((entry.command, entry.line, False))
            continue
        if entry.command in {"else", "elseif"}:
            if not stack or stack[-1][0] != "if":
                diagnostics.append(
                    f"CMakeLists.txt:{entry.line}: orphan {entry.command}()"
                )
            elif entry.command == "else":
                name, line, seen_else = stack[-1]
                if seen_else:
                    diagnostics.append(
                        f"CMakeLists.txt:{entry.line}: duplicate else()"
                    )
                stack[-1] = (name, line, True)
            elif stack[-1][2]:
                diagnostics.append(
                    f"CMakeLists.txt:{entry.line}: elseif() after else()"
                )
            continue
        wanted = closers.get(entry.command)
        if wanted is not None:
            if not stack or stack[-1][0] != wanted:
                diagnostics.append(
                    f"CMakeLists.txt:{entry.line}: orphan {entry.command}()"
                )
            else:
                stack.pop()
    for command, line, _ in stack:
        diagnostics.append(
            f"CMakeLists.txt:{line}: unterminated {command}()"
        )
    return CMakeIR(
        text=text,
        commands=tuple(commands),
        diagnostics=tuple(diagnostics),
        consumed=len(text),
    )


def cmake_bracket_delimiter(
    text: str,
    index: int,
) -> tuple[int, str] | None:
    if index >= len(text) or text[index] != "[":
        return None
    cursor = index + 1
    while cursor < len(text) and text[cursor] == "=":
        cursor += 1
    if cursor >= len(text) or text[cursor] != "[":
        return None
    equals = text[index + 1 : cursor]
    return cursor + 1, f"]{equals}]"


def cmake_commands(text: str) -> list[tuple[str, str]]:
    return [
        (command.command, command.body)
        for command in parse_cmake_ir(text).commands
    ]


def cmake_tokens(body: str) -> list[str]:
    tokens: list[str] = []
    index = 0
    while index < len(body):
        if body[index].isspace():
            index += 1
            continue
        if body[index] == "#":
            bracket = cmake_bracket_delimiter(body, index + 1)
            if bracket is not None:
                opener_end, closer = bracket
                close = body.find(closer, opener_end)
                index = len(body) if close < 0 else close + len(closer)
                continue
            newline = body.find("\n", index)
            index = len(body) if newline < 0 else newline + 1
            continue
        bracket = cmake_bracket_delimiter(body, index)
        if bracket is not None:
            opener_end, closer = bracket
            close = body.find(closer, opener_end)
            if close < 0:
                tokens.append(body[opener_end:])
                break
            tokens.append(body[opener_end:close])
            index = close + len(closer)
            continue
        if body[index] == '"':
            cursor = index + 1
            escaped = False
            while cursor < len(body):
                char = body[cursor]
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    break
                cursor += 1
            tokens.append(body[index + 1 : cursor])
            index = min(cursor + 1, len(body))
            continue
        cursor = index
        while cursor < len(body) and not body[cursor].isspace():
            cursor += 1
        tokens.append(body[index:cursor])
        index = cursor
    return tokens


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
