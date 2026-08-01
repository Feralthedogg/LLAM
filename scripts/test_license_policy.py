#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0

from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


CHECKER = Path(__file__).with_name("check_license_policy.py")
LICENSE_REF = "LicenseRef-LLAM-Commercial-Reciprocity-1.0"
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

    def _write_valid_repository(self) -> None:
        self._write(
            "LICENSE",
            "LLAM COMMERCIAL RECIPROCITY LICENSE 1.0\n\n"
            + SOFTWARE_DEFINITION
            + "\n",
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
            "Report privately at /security/advisories/new within 7 calendar days.\n"
            "Report non-security defects within 30 calendar days.\n",
        )
        self._write(".github/ISSUE_TEMPLATE/defect-report.yml", "name: Defect report\n")
        self._write(
            ".github/ISSUE_TEMPLATE/config.yml",
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
            'cp "$root_dir/LICENSE" "$stage/"\n',
        )
        self._write(
            "scripts/package_release_windows.ps1",
            'Copy-Item -LiteralPath (Join-Path $Root "LICENSE") -Destination $Stage\n',
        )
        self._write(
            "src/example.c",
            "/*\n"
            " * Copyright 2026 Feralthedogg\n"
            f" * SPDX-License-Identifier: {LICENSE_REF}\n"
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

    def test_rejects_changed_software_definition(self) -> None:
        self._write(
            "LICENSE",
            "LLAM COMMERCIAL RECIPROCITY LICENSE 1.0\n\n"
            '1.4. "Software" means only files with a header.\n',
        )

        result = self._run_checker()

        self.assertNotEqual(0, result.returncode)
        self.assertIn("LICENSE: approved Section 1.4 is missing", result.stderr)

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
