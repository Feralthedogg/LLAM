#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import argparse
import hashlib
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
ACTIVE_LICENSE_RELATIVE = Path(
    "LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
)
HISTORICAL_LICENSE_RELATIVE = Path("OLD-LICENSES/Apache-2.0.txt")
HISTORICAL_NOTICE_RELATIVE = Path("OLD-LICENSES/README.md")
HISTORICAL_APACHE_SHA256 = (
    "7d16370e642185e2eecad74eaf1e15179b27e2690f82644e7d247b395b600430"
)
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
    layout_required = (
        (ACTIVE_LICENSE_RELATIVE, "active LicenseRef text is missing"),
        (HISTORICAL_LICENSE_RELATIVE, "historical Apache text is missing"),
        (
            HISTORICAL_NOTICE_RELATIVE,
            "historical license scope notice is missing",
        ),
    )
    missing_layout = [
        f"{relative}: {message}"
        for relative, message in layout_required
        if not (root / relative).is_file()
    ]
    if missing_layout:
        print("\n".join(missing_layout), file=sys.stderr)
        return 1

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
    if (root / ACTIVE_LICENSE_RELATIVE).read_bytes() != (root / "LICENSE").read_bytes():
        errors.append(
            f"{ACTIVE_LICENSE_RELATIVE}: must be byte-identical to LICENSE"
        )

    historical_digest = hashlib.sha256(
        (root / HISTORICAL_LICENSE_RELATIVE).read_bytes()
    ).hexdigest()
    if historical_digest != HISTORICAL_APACHE_SHA256:
        errors.append(
            f"{HISTORICAL_LICENSE_RELATIVE}: historical Apache text does not match v2.2.1"
        )

    historical_notice = (root / HISTORICAL_NOTICE_RELATIVE).read_text(
        encoding="utf-8"
    )
    historical_notice_markers = (
        "v2.2.1",
        "not an alternative license",
        "exact tag or commit",
    )
    if not all(marker in historical_notice for marker in historical_notice_markers):
        errors.append(
            f"{HISTORICAL_NOTICE_RELATIVE}: historical license scope is incomplete"
        )

    tracked = tracked_files(root)
    for path in tracked:
        relative = path.relative_to(root)
        if (
            relative.parts
            and relative.parts[0] == "LICENSES"
            and relative != ACTIVE_LICENSE_RELATIVE
        ):
            errors.append(f"{relative}: inactive license text in LICENSES")

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

    for path in tracked:
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative == HISTORICAL_LICENSE_RELATIVE or (
            relative.parts and relative.parts[0] == "LICENSES"
        ):
            continue
        data = path.read_bytes()
        if b"\0" in data:
            continue
        text = data.decode("utf-8", errors="replace")
        if has_stale_apache_notice(text):
            errors.append(f"{relative}: stale Apache license notice")
        elif COPYRIGHT_NOTICE in text and LICENSE_REF not in text:
            errors.append(f"{relative}: current LicenseRef is missing")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print("license policy ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
