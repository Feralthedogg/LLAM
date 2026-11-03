#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0.
# See LICENSE.

from __future__ import annotations

import shutil
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

import check_license_policy as policy

SOURCE_ROOT = Path(__file__).resolve().parent.parent
HEADER = """/* Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0.
 * See LICENSE.
 */
"""
SIDECAR = """SPDX-FileCopyrightText: 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
"""


class LicensePolicyTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="llam-apache-policy-")
        self.root = Path(self.temp.name)
        subprocess.run(["git", "init", "--quiet", str(self.root)], check=True)
        for relative in policy.REQUIRED_FILES:
            destination = self.root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE_ROOT / relative, destination)
        self.write("src/example.c", HEADER + "int example;\n")
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def write(self, name: str | Path, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def errors(self) -> str:
        return "\n".join(policy.check(self.root, include_untracked=True))

    def test_current_apache_repository(self) -> None:
        self.assertEqual("", self.errors())

    def test_cli(self) -> None:
        result = subprocess.run(
            ["python3", str(SOURCE_ROOT / "scripts/check_license_policy.py"),
             "--root", str(self.root), "--include-untracked"],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("Apache-2.0 license policy ok", result.stdout)

    def test_missing_required_metadata(self) -> None:
        for relative in ("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md"):
            with self.subTest(path=relative):
                path = self.root / relative
                saved = path.read_bytes()
                path.unlink()
                self.assertIn(relative + ": required policy file", self.errors())
                path.write_bytes(saved)

    def test_reject_symlink_license(self) -> None:
        (self.root / "LICENSE").unlink()
        (self.root / "LICENSE").symlink_to("NOTICE")
        self.assertIn("LICENSE: required policy file", self.errors())

    def test_root_license_must_remain_canonical(self) -> None:
        self.write("LICENSE", "modified\n")
        self.assertIn("published grant", self.errors())

    def test_retired_duplicate_license_directory_is_rejected(self) -> None:
        self.write("LICENSES/Apache-2.0.txt", (self.root / "LICENSE").read_text())
        self.assertIn("retired license metadata", self.errors())

    def test_retired_license_text_and_manifest_rejected(self) -> None:
        for relative in policy.RETIRED_PATHS:
            with self.subTest(path=relative):
                self.write(relative, "retired\n")
                self.assertIn("retired license metadata", self.errors())
                (self.root / relative).unlink()

    def test_new_source_requires_apache_notice(self) -> None:
        self.write("src/new.c", "int new_file;\n")
        self.assertIn("src/new.c: Apache-2.0 header", self.errors())

    def test_recorded_deleted_git_paths_are_preserved_history(self) -> None:
        path = "research/measurement/verification.json"
        self.write(path, json.dumps({"commands": [], "git_status_before":
                                    " D LICENSES/" + policy.RETIRED_ID + ".txt\n"}))
        self.write(path + ".license", SIDECAR)
        self.assertEqual("", self.errors())
        for changes in (
            {"git_status_before": " M LICENSES/" + policy.RETIRED_ID + ".txt\n"},
            {"git_status_before": policy.RETIRED_ID},
            {"current_policy": policy.RETIRED_ID},
        ):
            with self.subTest(changes=changes):
                record = {"commands": [], "git_status_before":
                          " D LICENSES/" + policy.RETIRED_ID + ".txt\n"}
                record.update(changes)
                self.write(path, json.dumps(record))
                self.assertIn("stale current-policy", self.errors())

    def test_git_inventory_exception_cannot_apply_to_current_source(self) -> None:
        self.write("src/verification.json", json.dumps({"commands": [], "git_status_before":
                   " D LICENSES/" + policy.RETIRED_ID + ".txt\n"}))
        self.assertIn("stale current-policy", self.errors())

    def test_custom_spdx_is_not_current(self) -> None:
        self.write("src/new.c", HEADER.replace("Apache-2.0", policy.RETIRED_ID))
        self.assertIn("stale current-policy", self.errors())
        self.assertIn("src/new.c: Apache-2.0 header", self.errors())

    def test_new_script_requires_apache_notice(self) -> None:
        self.write("scripts/new.py", "print('hi')\n")
        self.assertIn("scripts/new.py: Apache-2.0 header", self.errors())

    def test_no_manifest_needed_for_new_apache_file(self) -> None:
        self.write("src/new.c", HEADER)
        self.assertEqual("", self.errors())

    def test_spdx_alone_is_not_complete(self) -> None:
        self.write("src/new.c", "/*\n * SPDX-License-Identifier: Apache-2.0\n */\n")
        self.assertIn("src/new.c: Apache-2.0 header", self.errors())

    def test_license_string_literal_is_not_a_header(self) -> None:
        self.write("src/new.c", 'const char *s = "SPDX-License-Identifier: Apache-2.0";\n')
        self.assertIn("src/new.c: Apache-2.0 header", self.errors())

    def test_c_cpp_hash_and_assembly_comments(self) -> None:
        for prefix in ("// ", "# ", "; ", " * "):
            with self.subTest(prefix=prefix):
                self.write("src/new.c", "\n".join(prefix + line for line in (
                    policy.APACHE_REF, policy.APACHE_NOTICE + ".", policy.APACHE_FILE_NOTICE,
                )))
                self.assertEqual("", self.errors())

    def test_adjacent_sidecar(self) -> None:
        self.write("scripts/generated.lock", "generated\n")
        self.write("scripts/generated.lock.license", SIDECAR)
        self.assertEqual("", self.errors())

    def test_wrong_license_sidecar(self) -> None:
        self.write("scripts/generated.lock", "generated\n")
        self.write("scripts/generated.lock.license", SIDECAR.replace("Apache-2.0", "MIT"))
        self.assertIn("generated.lock: Apache-2.0", self.errors())
        self.assertIn("sidecar notice is incomplete", self.errors())

    def test_sidecar_needs_attribution(self) -> None:
        self.write("scripts/generated.lock", "generated\n")
        self.write("scripts/generated.lock.license", policy.APACHE_REF)
        self.assertIn("sidecar notice is incomplete", self.errors())

    def test_untracked_source_is_optional_but_explicit_check_catches_it(self) -> None:
        self.write("src/new.c", "no notice\n")
        self.assertEqual([], policy.check(self.root))
        self.assertIn("src/new.c", self.errors())

    def test_third_party_terms_untouched(self) -> None:
        self.write("third_party/fixture/a.c", "/* SPDX-License-Identifier: MIT */\n")
        self.assertEqual("", self.errors())

    def test_internal_magic2_mirror_remains_upstream(self) -> None:
        self.write("third_party/magic2/magic2.h", "upstream\n")
        self.write("src/internal/magic2.h", "upstream\n")
        self.assertEqual("", self.errors())
        self.write("src/internal/magic2.h", "changed\n")
        self.assertIn("MIT-licensed mirror differs", self.errors())

    def test_provenance_is_first_party(self) -> None:
        self.write("third_party/magic2/UPSTREAM.md", HEADER)
        self.assertEqual("", self.errors())
        self.write("third_party/magic2/UPSTREAM.md", policy.RETIRED_ID)
        self.assertIn("stale current-policy", self.errors())

    def test_stale_current_document_rejected(self) -> None:
        self.write("docs/current.md", "See LICENSES/OLD-LICENSE/Apache-2.0.txt\n")
        self.assertIn("stale current-policy", self.errors())

    def test_historical_policy_requires_explicit_supersession(self) -> None:
        path = next(iter(policy.HISTORICAL_POLICY_FILES))
        self.write(path, policy.RETIRED_ID)
        self.assertIn("lacks superseded notice", self.errors())
        self.write(path, "Superseded on 2026-09-23\n" + policy.RETIRED_ID)
        self.assertEqual("", self.errors())

    def test_voluntary_private_security_route_is_required(self) -> None:
        self.write(".github/SECURITY.md", HEADER + "No guidance\n")
        self.assertIn("voluntary private reporting", self.errors())

    def test_mandatory_reporting_windows_are_removed(self) -> None:
        with (self.root / ".github/SECURITY.md").open("a") as output:
            output.write("\nReport within 7 calendar days.\n")
        self.assertIn("retired mandatory reporting window", self.errors())

    def test_contribution_terms_and_dco(self) -> None:
        self.write("CONTRIBUTING.md", HEADER)
        self.assertIn("Apache contribution terms", self.errors())
        self.assertIn("DCO instructions", self.errors())

    def test_notices_must_be_packaged_and_installed(self) -> None:
        self.write("scripts/package_release.sh", HEADER)
        self.write("scripts/install.sh", HEADER)
        self.assertIn("missing packaging metadata", self.errors())
        self.assertIn("attribution installation is missing", self.errors())


if __name__ == "__main__":
    unittest.main()
