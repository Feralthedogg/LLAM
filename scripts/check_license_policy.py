#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0.
# See LICENSES/Apache-2.0.txt.

"""Check the current Apache-2.0 policy without relabeling third-party material."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

APACHE_LICENSE = Path("LICENSES/Apache-2.0.txt")
APACHE_SHA256 = "7d16370e642185e2eecad74eaf1e15179b27e2690f82644e7d247b395b600430"
APACHE_REF = "SPDX-License-Identifier: Apache-2.0"
APACHE_NOTICE = "Licensed under the Apache License, Version 2.0"
APACHE_FILE_NOTICE = "See LICENSES/Apache-2.0.txt."
CONTRIBUTION_TERMS = """By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the Apache License, Version 2.0,
unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies."""
RETIRED_ID = "LicenseRef-LLAM-Commercial-Reciprocity-1.0"
RETIRED_PATHS = (
    Path("LICENSES") / (RETIRED_ID + ".txt"),
    Path("LICENSES/OLD-LICENSE/Apache-2.0.txt"),
    Path("LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt"),
)
RETIRED_MARKERS = (
    RETIRED_ID,
    "LLAM Commercial Reciprocity License",
    "LICENSES/OLD-LICENSE/",
    "LICENSES\\OLD-LICENSE\\",
    "OLD-LICENSES/",
)
HISTORICAL_POLICY_FILES = frozenset({
    Path("docs/research/plans/2026-08-01-license-transition.md"),
    Path("docs/research/plans/2026-08-01-license-file-layout.md"),
    Path("docs/research/specs/2026-08-01-license-transition-design.md"),
    Path("docs/superpowers/specs/2026-08-03-mixed-license-lineage-design.md"),
})
NEGATIVE_TEST_FILES = frozenset({
    Path("scripts/check_license_policy.py"),
    Path("scripts/test_license_policy.py"),
    Path("scripts/test_package_license_layout.py"),
    Path("scripts/test_package_license_layout_windows.ps1"),
})
REQUIRED_FILES = (
    Path("LICENSE"), APACHE_LICENSE, Path("NOTICE"), Path("THIRD_PARTY_NOTICES.md"),
    Path("README.md"), Path("CONTRIBUTING.md"), Path("docs/licensing.md"),
    Path(".github/SECURITY.md"), Path(".github/ISSUE_TEMPLATE/config.yml"),
    Path(".github/ISSUE_TEMPLATE/defect-report.yml"),
    Path("scripts/package_release.sh"), Path("scripts/package_release_windows.ps1"),
    Path("scripts/install.sh"), Path("scripts/install.ps1"),
    Path("third_party/magic2/LICENSE"),
)
HEADER_PREFIXES = {"src", "include", "tests", "examples", "scripts", "cmake", ".github", "tools"}
HEADER_FILES = {"Makefile", "CMakeLists.txt", "meson.build", "meson_options.txt", "AGENTS.md"}
METADATA_FILES = {Path("LICENSE"), APACHE_LICENSE, Path("NOTICE")}


def project_files(root: Path, include_untracked: bool) -> list[Path]:
    command = ["git", "-C", str(root), "ls-files", "-z", "--cached"]
    if include_untracked:
        command += ["--others", "--exclude-standard"]
    result = subprocess.run(command, check=True, stdout=subprocess.PIPE)
    return sorted({
        Path(item.decode("utf-8", errors="surrogateescape"))
        for item in result.stdout.split(b"\0") if item
    })


def excluded(relative: Path) -> bool:
    # Do not relabel dependencies or old local measurement/build artifacts.
    if relative == Path("third_party/magic2/UPSTREAM.md"):
        return False  # LLAM-authored provenance, not part of the vendored snapshot.
    if relative.name == ".DS_Store":
        return True
    return relative.parts[0] in {"third_party", "vendor", "artifacts", ".artifacts"}


def has_notice(text: str) -> bool:
    # Restrict to a header, not a string literal deep in an implementation/test.
    lines = text.splitlines()[:96]
    for index, line in enumerate(lines):
        if re.fullmatch(r"[ \t#*/;]*" + re.escape(APACHE_REF) + r"\s*", line):
            notice = "\n".join(lines[index + 1:index + 25])
            return APACHE_NOTICE in notice and APACHE_FILE_NOTICE in notice
    return False


def current_policy_text(relative: Path, text: str) -> str:
    """Keep deleted paths in recorded Git inventories from asserting policy."""
    if relative.parts[0] != "research" or relative.name != "verification.json":
        return text
    try:
        record = json.loads(text)
    except (ValueError, TypeError, RecursionError):
        return text
    if not isinstance(record, dict) or not isinstance(record.get("commands"), list):
        return text
    snapshot = record.get("git_status_before")
    if not isinstance(snapshot, str):
        return text
    lines = snapshot.splitlines()
    if not all(re.fullmatch(r"[ MADRCUT?!]{2} .+", line) for line in lines):
        return text
    # Only a structured porcelain deletion is historical inventory. Active
    # paths, other fields and quoted current-policy assertions stay audited.
    record["git_status_before"] = "\n".join(line for line in lines if line[:2] not in {" D", "D "})
    values = [record]
    strings = []
    while values:
        value = values.pop()
        if isinstance(value, str):
            strings.append(value)
        elif isinstance(value, dict):
            values.extend(value.keys())
            values.extend(value.values())
        elif isinstance(value, list):
            values.extend(value)
    return "\n".join(strings)


def check(root: Path, include_untracked: bool = False) -> list[str]:
    errors = []
    for relative in REQUIRED_FILES:
        path = root / relative
        if not path.is_file() or path.is_symlink():
            errors.append(f"{relative}: required policy file is missing or is a symlink")
    if errors:
        return errors
    for relative in RETIRED_PATHS:
        if (root / relative).exists():
            errors.append(f"{relative}: retired license metadata must not be active")

    canonical = (root / APACHE_LICENSE).read_bytes()
    if hashlib.sha256(canonical).hexdigest() != APACHE_SHA256:
        errors.append(f"{APACHE_LICENSE}: canonical Apache text differs from the published grant")
    if (root / "LICENSE").read_bytes() != canonical:
        errors.append("LICENSE: must be byte-identical to LICENSES/Apache-2.0.txt")

    def read(relative: str) -> str:
        return (root / relative).read_text(encoding="utf-8")

    if "Apache License" not in read("README.md"):
        errors.append("README.md: Apache license disclosure is missing")
    if CONTRIBUTION_TERMS not in read("CONTRIBUTING.md"):
        errors.append("CONTRIBUTING.md: Apache contribution terms are missing")
    if "Signed-off-by" not in read("CONTRIBUTING.md"):
        errors.append("CONTRIBUTING.md: DCO instructions are missing")
    security = read(".github/SECURITY.md")
    if "security/advisories/new" not in security or "voluntary" not in security.lower():
        errors.append(".github/SECURITY.md: voluntary private reporting guidance is missing")
    if re.search(r"\b(?:7|45) calendar days\b", security):
        errors.append(".github/SECURITY.md: retired mandatory reporting window")
    if "security/advisories/new" not in read(".github/ISSUE_TEMPLATE/config.yml"):
        errors.append(".github/ISSUE_TEMPLATE/config.yml: private security route is missing")
    if "Apache License" not in read("NOTICE") or "Copyright" not in read("NOTICE"):
        errors.append("NOTICE: Apache attribution is missing")
    licensing = read("docs/licensing.md")
    if not all(marker in licensing for marker in ("Apache", "first-party", "Third-party", "Historical")):
        errors.append("docs/licensing.md: current scope or historical boundary is missing")
    for relative in ("scripts/package_release.sh", "scripts/package_release_windows.ps1"):
        text = read(relative).replace("\\", "/")
        for marker in ("LICENSES/Apache-2.0.txt", "NOTICE", "THIRD_PARTY_NOTICES.md", "third_party/magic2/LICENSE"):
            if marker not in text:
                errors.append(f"{relative}: missing packaging metadata: {marker}")
    for relative in ("scripts/install.sh", "scripts/install.ps1"):
        text = read(relative)
        if "NOTICE" not in text or "THIRD_PARTY_NOTICES.md" not in text:
            errors.append(f"{relative}: attribution installation is missing")

    files = project_files(root, include_untracked)
    file_set = set(files)
    for relative in files:
        path = root / relative
        if excluded(relative) or not path.is_file() or path.is_symlink():
            continue
        if relative == Path("src/internal/magic2.h"):
            upstream = root / "third_party/magic2/magic2.h"
            if not upstream.is_file() or path.read_bytes() != upstream.read_bytes():
                errors.append(f"{relative}: MIT-licensed mirror differs from the pinned upstream")
            continue
        data = path.read_bytes()
        text = "" if b"\0" in data else data.decode("utf-8", errors="replace")
        if relative in HISTORICAL_POLICY_FILES:
            if "Superseded on 2026-09-23" not in text[:2500]:
                errors.append(f"{relative}: historical license plan lacks superseded notice")
            # The document's own header is Apache; historical excerpts remain history.
            continue
        if relative not in NEGATIVE_TEST_FILES and relative != Path("CHANGELOG.md"):
            policy_text = current_policy_text(relative, text)
            for marker in RETIRED_MARKERS:
                if marker in policy_text:
                    errors.append(f"{relative}: stale current-policy reference: {marker}")
        if relative in METADATA_FILES:
            continue
        if relative.name.endswith(".license"):
            if APACHE_REF not in text or "SPDX-FileCopyrightText:" not in text:
                errors.append(f"{relative}: Apache sidecar notice is incomplete")
            continue
        required = (
            relative.parts[0] in HEADER_PREFIXES or relative.as_posix() in HEADER_FILES
            or "Copyright 2026 Feralthedogg" in text
        )
        if not required:
            continue
        if has_notice(text):
            continue
        sidecar = relative.with_name(relative.name + ".license")
        if sidecar in file_set and (root / sidecar).is_file():
            sidecar_text = (root / sidecar).read_text(encoding="utf-8")
            if APACHE_REF in sidecar_text and "SPDX-FileCopyrightText:" in sidecar_text:
                continue
        errors.append(f"{relative}: Apache-2.0 header or sidecar is missing/incomplete")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--include-untracked", action="store_true")
    args = parser.parse_args()
    errors = check(args.root.resolve(), args.include_untracked)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Apache-2.0 license policy ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
