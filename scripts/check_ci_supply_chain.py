#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Fail closed when CI or documentation dependencies lose supply-chain pins."""

from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Iterable


ACTION_RE = re.compile(r"^\s*-\s*uses:\s*([^#\s]+)|^\s*uses:\s*([^#\s]+)")
FULL_COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
HASH_RE = re.compile(r"--hash=sha256:[0-9a-f]{64}\b")
EXACT_REQUIREMENT_RE = re.compile(r"^[A-Za-z0-9_.-]+==[A-Za-z0-9_.+!-]+(?:\s|\\|$)")


def _external_actions(text: str) -> Iterable[tuple[int, str]]:
    for line_number, line in enumerate(text.splitlines(), start=1):
        match = ACTION_RE.match(line)
        if not match:
            continue
        action = match.group(1) or match.group(2)
        if action.startswith(("./", "docker://")):
            continue
        yield line_number, action


def check_action_pins(path: Path, text: str) -> list[str]:
    violations: list[str] = []
    for line_number, action in _external_actions(text):
        if "@" not in action:
            violations.append(f"{path}:{line_number}: external action has no ref: {action}")
            continue
        _repository, ref = action.rsplit("@", 1)
        if not FULL_COMMIT_RE.fullmatch(ref):
            violations.append(
                f"{path}:{line_number}: external action must use a full commit SHA: {action}"
            )
    return violations


def check_workflow_permissions(path: Path, text: str) -> list[str]:
    """Reject write access at workflow scope; jobs may opt in explicitly."""

    violations: list[str] = []
    lines = text.splitlines()
    for index, line in enumerate(lines):
        match = re.fullmatch(r"permissions:\s*(.*)", line)
        if not match:
            continue
        inline = match.group(1).split("#", 1)[0].strip()
        if re.search(r"\bwrite(?:-all)?\b", inline):
            violations.append(f"{path}:{index + 1}: workflow-level write permission is forbidden")
        cursor = index + 1
        while cursor < len(lines):
            candidate = lines[cursor]
            if candidate and not candidate[0].isspace():
                break
            if re.search(r":\s*write\s*(?:#.*)?$", candidate):
                violations.append(
                    f"{path}:{cursor + 1}: workflow-level write permission is forbidden"
                )
            cursor += 1
    return violations


def check_cleartext_workflow_urls(path: Path, text: str) -> list[str]:
    violations: list[str] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        stripped = line.lstrip()
        if stripped.startswith("#"):
            continue
        if "http://" in line:
            violations.append(
                f"{path}:{line_number}: cleartext HTTP is forbidden in workflow inputs"
            )
    return violations


def _job_block(text: str, job_name: str) -> str | None:
    lines = text.splitlines()
    start: int | None = None
    for index, line in enumerate(lines):
        if line == f"  {job_name}:":
            start = index
            break
    if start is None:
        return None
    end = len(lines)
    for index in range(start + 1, len(lines)):
        if re.match(r"^  [A-Za-z0-9_-]+:\s*$", lines[index]):
            end = index
            break
    return "\n".join(lines[start:end])


def _checkout_steps_without_credential_lockdown(text: str) -> list[int]:
    lines = text.splitlines()
    missing: list[int] = []
    for index, line in enumerate(lines):
        if not re.search(r"uses:\s*actions/checkout@[0-9a-f]{40}\b", line):
            continue
        indent = len(line) - len(line.lstrip())
        end = len(lines)
        for cursor in range(index + 1, len(lines)):
            candidate = lines[cursor]
            candidate_indent = len(candidate) - len(candidate.lstrip())
            if candidate.strip().startswith("-") and candidate_indent <= indent:
                end = cursor
                break
        block = "\n".join(lines[index:end])
        if not re.search(r"^\s*persist-credentials:\s*false\s*(?:#.*)?$", block, re.MULTILINE):
            missing.append(index + 1)
    return missing


def _job_write_permissions(block: str) -> set[str]:
    lines = block.splitlines()
    writes: set[str] = set()
    for index, line in enumerate(lines):
        if line != "    permissions:":
            continue
        for candidate in lines[index + 1 :]:
            if candidate and len(candidate) - len(candidate.lstrip()) <= 4:
                break
            match = re.fullmatch(r"\s{6}([A-Za-z0-9_-]+):\s*write\s*(?:#.*)?", candidate)
            if match:
                writes.add(match.group(1))
    return writes


def check_release_workflow(path: Path, text: str) -> list[str]:
    if path.name != "release.yml":
        return []
    violations: list[str] = []
    if not re.search(
        r"^permissions:\s*$\n(?:^  [^\n]*\n)*?^  contents:\s*read\s*$",
        text,
        re.MULTILINE,
    ):
        violations.append(f"{path}: release workflow must default to contents: read")
    for builder_name in ("build-artifacts", "build-bsd-artifacts"):
        builder = _job_block(text, builder_name)
        if builder is not None and _job_write_permissions(builder):
            violations.append(
                f"{path}: {builder_name} must not receive a write permission"
            )
    publish = _job_block(text, "publish-release")
    if publish is None:
        violations.append(f"{path}: publish-release job is missing")
    elif not re.search(
        r"^\s{4}permissions:\s*$\n(?:^\s{6}[^\n]*\n)*?^\s{6}contents:\s*write\s*$",
        publish,
        re.MULTILINE,
    ):
        violations.append(
            f"{path}: publish-release must scope contents: write to the publishing job"
        )
    elif _job_write_permissions(publish) != {"contents"}:
        violations.append(
            f"{path}: publish-release may receive only contents: write"
        )
    for line_number in _checkout_steps_without_credential_lockdown(text):
        violations.append(
            f"{path}:{line_number}: release checkout must set persist-credentials: false"
        )
    return violations


def _logical_requirements(text: str) -> list[tuple[int, str]]:
    logical: list[tuple[int, str]] = []
    current: list[str] = []
    start = 0
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if current:
            current.append(line)
            if not line.split("#", 1)[0].rstrip().endswith("\\"):
                logical.append((start, " ".join(current)))
                current = []
            continue
        if line.startswith("--"):
            continue
        current = [line]
        start = line_number
        if not line.split("#", 1)[0].rstrip().endswith("\\"):
            logical.append((start, line))
            current = []
    if current:
        logical.append((start, " ".join(current)))
    return logical


def check_docs_requirements(path: Path, text: str) -> list[str]:
    violations: list[str] = []
    if "--only-binary=:all:" not in text:
        violations.append(f"{path}: documentation lock must require binary distributions")
    requirements = _logical_requirements(text)
    if not requirements:
        violations.append(f"{path}: documentation lock has no requirements")
    for line_number, requirement in requirements:
        if not EXACT_REQUIREMENT_RE.match(requirement):
            violations.append(
                f"{path}:{line_number}: documentation dependency must use an exact == pin"
            )
        if not HASH_RE.search(requirement):
            violations.append(
                f"{path}:{line_number}: documentation dependency must include a SHA-256 hash"
            )
    return violations


def check_docs_workflow(path: Path, text: str) -> list[str]:
    if path.name != "docs.yml":
        return []
    if not re.search(
        r"python\s+-m\s+pip\s+install\s+--require-hashes\s+-r\s+docs/requirements\.txt",
        text,
    ):
        return [f"{path}: docs dependencies must be installed with --require-hashes"]
    return []


def find_violations(root: Path) -> list[str]:
    violations: list[str] = []
    workflows = root / ".github" / "workflows"
    for path in sorted((*workflows.glob("*.yml"), *workflows.glob("*.yaml"))):
        text = path.read_text(encoding="utf-8")
        violations.extend(check_action_pins(path.relative_to(root), text))
        violations.extend(check_workflow_permissions(path.relative_to(root), text))
        violations.extend(check_cleartext_workflow_urls(path.relative_to(root), text))
        violations.extend(check_release_workflow(path.relative_to(root), text))
        violations.extend(check_docs_workflow(path.relative_to(root), text))
    requirements = root / "docs" / "requirements.txt"
    violations.extend(
        check_docs_requirements(
            requirements.relative_to(root),
            requirements.read_text(encoding="utf-8"),
        )
    )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (default: inferred from this script)",
    )
    args = parser.parse_args()
    violations = find_violations(args.root.resolve())
    if violations:
        print("CI supply-chain policy violations:")
        for violation in violations:
            print(f"  - {violation}")
        return 1
    print("CI supply-chain policy: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
