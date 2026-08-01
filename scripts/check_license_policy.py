#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


SOFTWARE_DEFINITION = """1.4. "Software" means source code, object code, documentation, tests,
examples, build materials, configuration, and other materials included in a
release, branch, commit, package, repository snapshot, or copy to which this
License is expressly applied by a LICENSE file, package metadata, file header,
or other accompanying notice, excluding materials expressly identified as
being governed by another license."""
CONTRIBUTION_TERMS = """By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the LLAM Commercial Reciprocity License
1.0, unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies."""
STALE_APACHE_MARKERS = (
    "SPDX-License-Identifier: Apache-2.0",
    'Licensed under the Apache License, Version 2.0',
)
COPYRIGHT_NOTICE = "Copyright 2026 Feralthedogg"
LICENSE_REF = "SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0"
REQUIRED_POLICY_FILES = (
    "LICENSE",
    "README.md",
    "CONTRIBUTING.md",
    ".github/SECURITY.md",
    ".github/ISSUE_TEMPLATE/defect-report.yml",
    ".github/ISSUE_TEMPLATE/config.yml",
    "docs/licensing.md",
    "scripts/package_release.sh",
    "scripts/package_release_windows.ps1",
)


def tracked_files(root: Path) -> list[Path]:
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=True,
        stdout=subprocess.PIPE,
    )
    return [
        root / item.decode("utf-8", errors="surrogateescape")
        for item in result.stdout.split(b"\0")
        if item
    ]


def has_stale_apache_notice(text: str) -> bool:
    comment_prefixes = ("/*", "//", "#", ";", "*")
    for line in text.splitlines():
        stripped = line.lstrip()
        for prefix in comment_prefixes:
            if not stripped.startswith(prefix):
                continue
            comment = stripped[len(prefix) :].lstrip()
            if any(comment.startswith(marker) for marker in STALE_APACHE_MARKERS):
                return True
            break
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="Check LLAM repository license policy.")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    args = parser.parse_args()

    root = args.root.resolve()
    missing = [
        f"{relative}: required policy file is missing"
        for relative in REQUIRED_POLICY_FILES
        if not (root / relative).is_file()
    ]
    if missing:
        print("\n".join(missing), file=sys.stderr)
        return 1

    license_text = (root / "LICENSE").read_text(encoding="utf-8")
    if SOFTWARE_DEFINITION not in license_text:
        print("LICENSE: approved Section 1.4 is missing", file=sys.stderr)
        return 1

    errors: list[str] = []
    readme_text = (root / "README.md").read_text(encoding="utf-8")
    readme_lower = readme_text.lower()
    if "source-available" not in readme_lower or "not osi-approved open source" not in readme_lower:
        errors.append("README.md: source-available disclosure is missing")

    contribution_text = (root / "CONTRIBUTING.md").read_text(encoding="utf-8")
    if CONTRIBUTION_TERMS not in contribution_text:
        errors.append("CONTRIBUTING.md: approved contribution terms are missing")
    if "Signed-off-by" not in contribution_text:
        errors.append("CONTRIBUTING.md: DCO sign-off instructions are missing")

    security_text = (root / ".github/SECURITY.md").read_text(encoding="utf-8")
    security_markers = ("security/advisories/new", "7 calendar days", "30 calendar days")
    if not all(marker in security_text for marker in security_markers):
        errors.append(".github/SECURITY.md: reporting requirements are incomplete")

    packaging_markers = {
        "scripts/package_release.sh": '$root_dir/LICENSE',
        "scripts/package_release_windows.ps1": 'Join-Path $Root "LICENSE"',
    }
    for relative, marker in packaging_markers.items():
        text = (root / relative).read_text(encoding="utf-8")
        if marker not in text:
            errors.append(f"{relative}: LICENSE packaging is missing")

    issue_config = (root / ".github/ISSUE_TEMPLATE/config.yml").read_text(
        encoding="utf-8"
    )
    if "security/advisories/new" not in issue_config:
        errors.append(
            ".github/ISSUE_TEMPLATE/config.yml: private security route is missing"
        )

    licensing_text = (root / "docs/licensing.md").read_text(encoding="utf-8")
    licensing_markers = ("v2.2.1", "Apache License 2.0", "v3.0.0")
    if not all(marker in licensing_text for marker in licensing_markers):
        errors.append("docs/licensing.md: release boundary is incomplete")

    for path in tracked_files(root):
        if not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue
        text = data.decode("utf-8", errors="replace")
        if has_stale_apache_notice(text):
            errors.append(f"{path.relative_to(root)}: stale Apache license notice")
        elif COPYRIGHT_NOTICE in text and LICENSE_REF not in text:
            errors.append(f"{path.relative_to(root)}: current LicenseRef is missing")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print("license policy ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
