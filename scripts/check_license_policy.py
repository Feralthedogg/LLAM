#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Licensed under the LLAM Commercial Reciprocity License 1.0.
# See the LICENSE file distributed with this Software.

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from pathlib import Path


SOFTWARE_DEFINITION = """1.4. "Software" means source code, object code, documentation, tests,
examples, build materials, configuration, and other materials to which this
License is expressly applied by a LICENSE file, package notice, file header,
or other accompanying notice, excluding materials expressly identified as
governed by another license."""
CONTRIBUTION_TERMS = """By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the LLAM Commercial Reciprocity License
1.0, unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies."""
APPLICATION_SCOPE = """This License is expressly applied to the LLAM repository snapshot,
distribution, or copy that contains this LICENSE file, except for materials
conspicuously identified as governed by another license.

LLAM-authored source files may use the following notice in the appropriate
comment syntax:"""
COPYRIGHT_NOTICE = "Copyright 2026 Feralthedogg"
LICENSE_REF = "SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0"
LICENSE_NOTICE = "Licensed under the LLAM Commercial Reciprocity License 1.0."
LICENSE_FILE_NOTICE = "See the LICENSE file distributed with this Software."
APACHE_REF = "SPDX-License-Identifier: Apache-2.0"
APACHE_FILE_NOTICE = "See LICENSES/OLD-LICENSE/Apache-2.0.txt."
ACTIVE_LICENSE_RELATIVE = Path(
    "LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
)
APACHE_LICENSE_RELATIVE = Path("LICENSES/OLD-LICENSE/Apache-2.0.txt")
APACHE_MANIFEST_RELATIVE = Path(
    "LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt"
)
APACHE_LICENSE_SHA256 = (
    "7d16370e642185e2eecad74eaf1e15179b27e2690f82644e7d247b395b600430"
)
LICENSE_METADATA_FILES = frozenset(
    {
        Path("LICENSE"),
        ACTIVE_LICENSE_RELATIVE,
        APACHE_LICENSE_RELATIVE,
        APACHE_MANIFEST_RELATIVE,
    }
)
HEADER_REQUIRED_PREFIXES = (
    Path("src"),
    Path("include"),
    Path("tests"),
    Path("examples"),
    Path("scripts"),
    Path("cmake"),
    Path(".github"),
)
HEADER_REQUIRED_FILES = frozenset({Path("Makefile"), Path("CMakeLists.txt")})
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


def contains_policy_text(text: str, required: str) -> bool:
    return " ".join(required.split()) in " ".join(text.split())


def requires_current_license(relative: Path) -> bool:
    if relative.name.endswith(".license"):
        return False
    if relative in HEADER_REQUIRED_FILES:
        return True
    return any(prefix in relative.parents for prefix in HEADER_REQUIRED_PREFIXES)


def license_status(
    root: Path,
    path: Path,
    relative: Path,
    text: str,
    tracked_relatives: set[Path],
    spdx_ref: str,
    required_notices: tuple[str, ...],
    *,
    allow_sidecar: bool,
    notice_window: int,
) -> str:
    if spdx_ref in text:
        lines = text.splitlines()
        license_line_index = next(
            index for index, line in enumerate(lines) if spdx_ref in line
        )
        notice_block = "\n".join(
            lines[license_line_index + 1 : license_line_index + 1 + notice_window]
        )
        if all(notice in notice_block for notice in required_notices):
            return "complete"
        return "incomplete"
    if not allow_sidecar:
        return "missing"
    sidecar = path.with_name(path.name + ".license")
    sidecar_relative = relative.with_name(relative.name + ".license")
    if sidecar_relative not in tracked_relatives or not sidecar.is_file():
        return "missing"
    sidecar_text = sidecar.read_text(encoding="utf-8", errors="replace")
    return "sidecar" if spdx_ref in sidecar_text else "missing"


def load_apache_manifest(
    root: Path,
    tracked_relatives: set[Path],
) -> tuple[set[Path], list[str]]:
    raw_lines = (root / APACHE_MANIFEST_RELATIVE).read_text(
        encoding="utf-8"
    ).splitlines()
    errors: list[str] = []
    if raw_lines != sorted(raw_lines, key=lambda value: value.encode("utf-8")):
        errors.append("Apache path manifest must be bytewise sorted")
    if len(raw_lines) != len(set(raw_lines)):
        errors.append("Apache path manifest contains duplicate paths")

    paths: set[Path] = set()
    for value in raw_lines:
        candidate = Path(value)
        if not value or candidate.is_absolute() or ".." in candidate.parts:
            errors.append(f"{value}: invalid Apache manifest path")
            continue
        paths.add(candidate)
        if candidate not in tracked_relatives:
            errors.append(f"{value}: Apache manifest path is not tracked")
    return paths, errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Check LLAM repository license policy.")
    parser.add_argument("--root", type=Path, default=Path.cwd())
    args = parser.parse_args()

    root = args.root.resolve()
    layout_required = (
        (ACTIVE_LICENSE_RELATIVE, "active LicenseRef text is missing"),
        (APACHE_LICENSE_RELATIVE, "canonical Apache text is missing"),
        (APACHE_MANIFEST_RELATIVE, "Apache path manifest is missing"),
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
    if not contains_policy_text(license_text, SOFTWARE_DEFINITION):
        print("LICENSE: approved Section 1.4 is missing", file=sys.stderr)
        return 1
    if not contains_policy_text(license_text, APPLICATION_SCOPE):
        print("LICENSE: application scope notice is missing", file=sys.stderr)
        return 1

    errors: list[str] = []
    if (root / ACTIVE_LICENSE_RELATIVE).read_bytes() != (root / "LICENSE").read_bytes():
        errors.append(
            f"{ACTIVE_LICENSE_RELATIVE}: must be byte-identical to LICENSE"
        )

    apache_digest = hashlib.sha256(
        (root / APACHE_LICENSE_RELATIVE).read_bytes()
    ).hexdigest()
    if apache_digest != APACHE_LICENSE_SHA256:
        errors.append(
            f"{APACHE_LICENSE_RELATIVE}: canonical Apache text does not match "
            "the published Apache grant"
        )

    tracked = tracked_files(root)
    tracked_relatives = {path.relative_to(root) for path in tracked}
    apache_paths, manifest_errors = load_apache_manifest(root, tracked_relatives)
    errors.extend(manifest_errors)
    for path in tracked:
        relative = path.relative_to(root)
        if (
            relative.parts
            and relative.parts[0] == "LICENSES"
            and relative not in LICENSE_METADATA_FILES
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
    security_markers = (
        "security/advisories/new",
        "7 calendar days",
        "45 calendar days",
    )
    if not all(marker in security_text for marker in security_markers):
        errors.append(".github/SECURITY.md: reporting requirements are incomplete")

    packaging_markers = {
        "scripts/package_release.sh": (
            '$root_dir/LICENSE',
            "LICENSES/OLD-LICENSE/Apache-2.0.txt",
            "LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt",
        ),
        "scripts/package_release_windows.ps1": (
            'Join-Path $Root "LICENSE"',
            "LICENSES\\OLD-LICENSE\\Apache-2.0.txt",
            "LICENSES\\OLD-LICENSE\\APACHE-2.0-FILES.txt",
        ),
    }
    for relative, markers in packaging_markers.items():
        text = (root / relative).read_text(encoding="utf-8")
        if not all(marker in text for marker in markers):
            errors.append(f"{relative}: LICENSE packaging is missing")

    issue_config = (root / ".github/ISSUE_TEMPLATE/config.yml").read_text(
        encoding="utf-8"
    )
    if "security/advisories/new" not in issue_config:
        errors.append(
            ".github/ISSUE_TEMPLATE/config.yml: private security route is missing"
        )

    licensing_text = (root / "docs/licensing.md").read_text(encoding="utf-8")
    licensing_markers = (
        "mixed-license",
        "LICENSES/OLD-LICENSE/APACHE-2.0-FILES.txt",
        "Untagged",
    )
    if not all(marker in licensing_text for marker in licensing_markers):
        errors.append("docs/licensing.md: mixed-license policy is incomplete")

    obsolete_reference_exclusions = {
        Path("scripts/check_license_policy.py"),
        Path("scripts/test_license_policy.py"),
        Path("scripts/test_package_license_layout.py"),
        Path("scripts/test_package_license_layout_windows.ps1"),
    }
    for path in tracked:
        relative = path.relative_to(root)
        if relative in obsolete_reference_exclusions or not path.is_file():
            continue
        data = path.read_bytes()
        if b"\0" not in data and "OLD-LICENSES/" in data.decode(
            "utf-8", errors="replace"
        ):
            errors.append(f"{relative}: obsolete OLD-LICENSES reference")

    for path in tracked:
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if relative in LICENSE_METADATA_FILES:
            continue
        data = path.read_bytes()
        text = "" if b"\0" in data else data.decode("utf-8", errors="replace")
        file_notice_required = requires_current_license(relative) or (
            COPYRIGHT_NOTICE in text
        )
        if not file_notice_required:
            continue

        if relative in apache_paths:
            apache_status = license_status(
                root,
                path,
                relative,
                text,
                tracked_relatives,
                APACHE_REF,
                (APACHE_FILE_NOTICE,),
                allow_sidecar=True,
                notice_window=17,
            )
            if apache_status == "missing":
                errors.append(f"{relative}: Apache-2.0 notice is missing")
            elif apache_status == "incomplete":
                errors.append(
                    f"{relative}: Apache application notice is incomplete"
                )
            continue

        current_status = license_status(
            root,
            path,
            relative,
            text,
            tracked_relatives,
            LICENSE_REF,
            (LICENSE_NOTICE, LICENSE_FILE_NOTICE),
            allow_sidecar=True,
            notice_window=3,
        )
        if current_status == "missing":
            errors.append(f"{relative}: current LicenseRef is missing")
        elif current_status == "incomplete":
            errors.append(f"{relative}: current application notice is incomplete")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1

    print("license policy ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
