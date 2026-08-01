#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Licensed under the LLAM Commercial Reciprocity License 1.0.
# See the LICENSE file distributed with this Software.

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


CHECKER = Path(__file__).with_name("check_license_policy.py")
SOURCE_ROOT = CHECKER.parent.parent
LICENSE_REF = "LicenseRef-LLAM-Commercial-Reciprocity-1.0"
ACTIVE_LICENSE_RELATIVE = (
    "LICENSES/LicenseRef-LLAM-Commercial-Reciprocity-1.0.txt"
)
HISTORICAL_LICENSE_RELATIVE = "OLD-LICENSES/Apache-2.0.txt"
HISTORICAL_NOTICE_RELATIVE = "OLD-LICENSES/README.md"
CURRENT_LICENSE_TEXT = (SOURCE_ROOT / "LICENSE").read_text(encoding="utf-8")
HISTORICAL_APACHE_TEXT = (
    SOURCE_ROOT / HISTORICAL_LICENSE_RELATIVE
).read_text(encoding="utf-8")
CONTRIBUTION_TERMS = """By intentionally submitting a contribution for inclusion in LLAM, you agree
to license that contribution under the LLAM Commercial Reciprocity License
1.0, unless the submission is conspicuously marked "Not a Contribution" or a
separate written agreement applies."""
LICENSE_NOTICE = "Licensed under the LLAM Commercial Reciprocity License 1.0."
LICENSE_FILE_NOTICE = "See the LICENSE file distributed with this Software."
INDENTED_LICENSE_TEXT = """LLAM COMMERCIAL RECIPROCITY LICENSE 1.0

   1.4. "Software" means source code, object code, documentation, tests,
        examples, build materials, configuration, and other materials to
        which this License is expressly applied by a LICENSE file, package
        notice, file header, or other accompanying notice, excluding
        materials expressly identified as governed by another license.

APPLICATION NOTICE (NOT PART OF THE TERMS)

   This License is expressly applied to the LLAM repository snapshot,
   distribution, or copy that contains this LICENSE file, except for materials
   conspicuously identified as governed by another license.

   LLAM-authored source files may use the following notice in the appropriate
   comment syntax:

   Copyright 2026 Feralthedogg
   SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
   Licensed under the LLAM Commercial Reciprocity License 1.0.
   See the LICENSE file distributed with this Software.
"""


class LicensePolicyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        subprocess.run(
            ["git", "init", "--quiet", str(self.root)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self._write_valid_repository()

    def tearDown(self) -> None:
        self.temp_dir.cleanup()

    def _write(self, relative: str, content: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def _track(self, *relative_paths: str) -> None:
        subprocess.run(
            ["git", "-C", str(self.root), "add", "--", *relative_paths],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def _write_valid_repository(self) -> None:
        self._write("LICENSE", CURRENT_LICENSE_TEXT)
        self._write(ACTIVE_LICENSE_RELATIVE, CURRENT_LICENSE_TEXT)
        self._write(HISTORICAL_LICENSE_RELATIVE, HISTORICAL_APACHE_TEXT)
        self._write(
            HISTORICAL_NOTICE_RELATIVE,
            "v2.2.1 and earlier only. This is not an alternative license.\n"
            "Use the LICENSE stored at the exact tag or commit.\n",
        )
        self._write(
            "README.md",
            "LLAM is source-available and is not OSI-approved open source.\n",
        )
        self._write(
            "CONTRIBUTING.md",
            CONTRIBUTION_TERMS + "\n\nUse a Signed-off-by trailer.\n",
        )
        self._write(
            ".github/SECURITY.md",
            f"SPDX-License-Identifier: {LICENSE_REF}\n"
            f"{LICENSE_NOTICE}\n"
            f"{LICENSE_FILE_NOTICE}\n"
            "Report privately at /security/advisories/new within 7 calendar days.\n"
            "Report non-security defects within 45 calendar days.\n",
        )
        self._write(
            ".github/ISSUE_TEMPLATE/defect-report.yml",
            f"# SPDX-License-Identifier: {LICENSE_REF}\n"
            f"# {LICENSE_NOTICE}\n"
            f"# {LICENSE_FILE_NOTICE}\n"
            "name: Defect report\n",
        )
        self._write(
            ".github/ISSUE_TEMPLATE/config.yml",
            f"# SPDX-License-Identifier: {LICENSE_REF}\n"
            f"# {LICENSE_NOTICE}\n"
            f"# {LICENSE_FILE_NOTICE}\n"
            "blank_issues_enabled: false\n"
            "contact_links:\n"
            "  - name: Private security report\n"
            "    url: https://github.com/Feralthedogg/LLAM/security/advisories/new\n",
        )
        self._write(
            "docs/licensing.md",
            "v2.2.1 and earlier use Apache License 2.0; v3.0.0 uses the current LICENSE.\n",
        )
        self._write(
            "scripts/package_release.sh",
            f"# SPDX-License-Identifier: {LICENSE_REF}\n"
            f"# {LICENSE_NOTICE}\n"
            f"# {LICENSE_FILE_NOTICE}\n"
            'cp "$root_dir/LICENSE" "$stage/"\n',
        )
        self._write(
            "scripts/package_release_windows.ps1",
            f"# SPDX-License-Identifier: {LICENSE_REF}\n"
            f"# {LICENSE_NOTICE}\n"
            f"# {LICENSE_FILE_NOTICE}\n"
            'Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination $Stage\n',
        )
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            f" * SPDX-License-Identifier: {LICENSE_REF}\n"
            f" * {LICENSE_NOTICE}\n"
            f" * {LICENSE_FILE_NOTICE}\n"
            " */\n",
        )
        subprocess.run(
            ["git", "-C", str(self.root), "add", "--all"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def _run_checker(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["python3", str(CHECKER), "--root", str(self.root)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_accepts_complete_repository_policy(self) -> None:
        result = self._run_checker()

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_rejects_missing_active_license_text(self) -> None:
        (self.root / ACTIVE_LICENSE_RELATIVE).unlink()

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("active LicenseRef text is missing", result.stderr)

    def test_rejects_active_license_text_that_differs_from_root(self) -> None:
        self._write(ACTIVE_LICENSE_RELATIVE, "different license text\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("must be byte-identical to LICENSE", result.stderr)

    def test_accepts_indented_license_policy_text(self) -> None:
        self._write("LICENSE", INDENTED_LICENSE_TEXT)
        self._write(ACTIVE_LICENSE_RELATIVE, INDENTED_LICENSE_TEXT)

        result = self._run_checker()

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_rejects_unexpected_active_license_file(self) -> None:
        self._write("LICENSES/Apache-2.0.txt", HISTORICAL_APACHE_TEXT)
        self._track("LICENSES/Apache-2.0.txt")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(
            "LICENSES/Apache-2.0.txt: inactive license text", result.stderr
        )

    def test_rejects_new_llam_script_without_current_license(self) -> None:
        self._write("scripts/new_tool.py", "print('hello')\n")
        self._track("scripts/new_tool.py")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("scripts/new_tool.py: current LicenseRef is missing", result.stderr)

    def test_accepts_adjacent_license_file_for_generated_content(self) -> None:
        self._write("scripts/generated.lock", "generated = true\n")
        self._write(
            "scripts/generated.lock.license",
            "SPDX-FileCopyrightText: 2026 Feralthedogg\n"
            f"SPDX-License-Identifier: {LICENSE_REF}\n",
        )
        self._track("scripts/generated.lock", "scripts/generated.lock.license")

        result = self._run_checker()

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_rejects_adjacent_license_file_with_wrong_license(self) -> None:
        self._write("scripts/generated.lock", "generated = true\n")
        self._write(
            "scripts/generated.lock.license",
            "SPDX-FileCopyrightText: 2026 Feralthedogg\n"
            "SPDX-License-Identifier: MIT\n",
        )
        self._track("scripts/generated.lock", "scripts/generated.lock.license")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("scripts/generated.lock: current LicenseRef is missing", result.stderr)

    def test_rejects_modified_historical_apache_text(self) -> None:
        self._write(
            HISTORICAL_LICENSE_RELATIVE,
            HISTORICAL_APACHE_TEXT + "modified\n",
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(
            "historical Apache text does not match v2.2.1", result.stderr
        )

    def test_rejects_missing_historical_scope_notice(self) -> None:
        (self.root / HISTORICAL_NOTICE_RELATIVE).unlink()

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("historical license scope notice is missing", result.stderr)

    def test_rejects_changed_software_definition(self) -> None:
        self._write(
            "LICENSE",
            "LLAM COMMERCIAL RECIPROCITY LICENSE 1.0\n\n"
            '1.4. "Software" means only files with a header.\n',
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("LICENSE: approved Section 1.4 is missing", result.stderr)

    def test_rejects_license_without_explicit_application_scope(self) -> None:
        license_text = (
            "\n\n".join(
                paragraph
                for paragraph in CURRENT_LICENSE_TEXT.rstrip().split("\n\n")
                if "This License is expressly applied" not in paragraph
            )
            + "\n"
        )
        self._write("LICENSE", license_text)
        self._write(ACTIVE_LICENSE_RELATIVE, license_text)

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("LICENSE: application scope notice is missing", result.stderr)

    def test_rejects_spdx_header_without_application_notice(self) -> None:
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            f" * SPDX-License-Identifier: {LICENSE_REF}\n"
            " */\n",
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("src/example.c: current application notice is incomplete", result.stderr)

    def test_rejects_partial_application_notice(self) -> None:
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            f" * SPDX-License-Identifier: {LICENSE_REF}\n"
            f" * {LICENSE_NOTICE}\n"
            " */\n",
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("src/example.c: current application notice is incomplete", result.stderr)

    def test_rejects_application_notice_away_from_spdx_header(self) -> None:
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            f" * SPDX-License-Identifier: {LICENSE_REF}\n"
            " */\n\n"
            f'const char *spdx = "SPDX-License-Identifier: {LICENSE_REF}";\n'
            f'const char *license_notice = "{LICENSE_NOTICE}";\n'
            f'const char *license_file_notice = "{LICENSE_FILE_NOTICE}";\n',
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("src/example.c: current application notice is incomplete", result.stderr)

    def test_rejects_stale_apache_notice_in_tracked_source(self) -> None:
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            " * SPDX-License-Identifier: Apache-2.0\n"
            " */\n",
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("src/example.c: stale Apache license notice", result.stderr)

    def test_accepts_stale_marker_used_as_policy_test_data(self) -> None:
        self._write(
            "scripts/policy_data.py",
            f'# Copyright 2026 Feralthedogg\n'
            f'# SPDX-License-Identifier: {LICENSE_REF}\n'
            f'# {LICENSE_NOTICE}\n'
            f'# {LICENSE_FILE_NOTICE}\n'
            'STALE_SPDX = "SPDX-License-Identifier: Apache-2.0"\n'
            'STALE_BOILERPLATE = "Licensed under the Apache License, Version 2.0"\n',
        )
        subprocess.run(
            ["git", "-C", str(self.root), "add", "scripts/policy_data.py"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        result = self._run_checker()

        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_rejects_llam_notice_without_current_license_ref(self) -> None:
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            " */\n",
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("src/example.c: current LicenseRef is missing", result.stderr)

    def test_rejects_missing_operational_policy_file(self) -> None:
        (self.root / ".github/SECURITY.md").unlink()

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(".github/SECURITY.md: required policy file is missing", result.stderr)

    def test_rejects_readme_without_source_available_disclosure(self) -> None:
        self._write("README.md", "LLAM concurrency runtime.\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("README.md: source-available disclosure is missing", result.stderr)

    def test_rejects_changed_contribution_license_terms(self) -> None:
        self._write("CONTRIBUTING.md", "Contributions are welcome.\nSigned-off-by\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("CONTRIBUTING.md: approved contribution terms are missing", result.stderr)

    def test_rejects_contribution_guide_without_dco_signoff(self) -> None:
        self._write("CONTRIBUTING.md", CONTRIBUTION_TERMS + "\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("CONTRIBUTING.md: DCO sign-off instructions are missing", result.stderr)

    def test_rejects_security_policy_without_private_route_and_deadlines(self) -> None:
        self._write(".github/SECURITY.md", "Please report security problems.\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(".github/SECURITY.md: reporting requirements are incomplete", result.stderr)

    def test_rejects_obsolete_nonsecurity_reporting_deadline(self) -> None:
        self._write(
            ".github/SECURITY.md",
            f"SPDX-License-Identifier: {LICENSE_REF}\n"
            f"{LICENSE_NOTICE}\n"
            f"{LICENSE_FILE_NOTICE}\n"
            "Report privately at /security/advisories/new within 7 calendar days.\n"
            "Report non-security defects within 30 calendar days.\n",
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(
            ".github/SECURITY.md: reporting requirements are incomplete",
            result.stderr,
        )

    def test_rejects_release_packager_that_omits_license(self) -> None:
        self._write("scripts/package_release.sh", 'cp "$root_dir/README.md" "$stage/"\n')

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("scripts/package_release.sh: LICENSE packaging is missing", result.stderr)

    def test_rejects_issue_configuration_without_private_security_route(self) -> None:
        self._write(".github/ISSUE_TEMPLATE/config.yml", "blank_issues_enabled: false\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn(
            ".github/ISSUE_TEMPLATE/config.yml: private security route is missing",
            result.stderr,
        )

    def test_rejects_missing_historical_license_boundary(self) -> None:
        self._write("docs/licensing.md", "The current release uses the current LICENSE.\n")

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("docs/licensing.md: release boundary is incomplete", result.stderr)


if __name__ == "__main__":
    unittest.main()
