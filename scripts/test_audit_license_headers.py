#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Feralthedogg
"""Fixture tests for LLAM's tracked-file license-header audit."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest


AUDIT = Path(__file__).resolve().with_name("audit_license_headers.py")


def load_audit_module() -> types.ModuleType:
    if not AUDIT.is_file():
        raise AssertionError(f"missing production audit: {AUDIT}")
    spec = importlib.util.spec_from_file_location(
        "llam_audit_license_headers",
        AUDIT,
    )
    if spec is None or spec.loader is None:
        raise AssertionError("cannot load license-header audit module")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class LicenseHeaderAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        subprocess.run(
            ["git", "init", "-q", str(self.root)],
            check=True,
            capture_output=True,
            text=True,
        )

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write(self, relative: str, text: str) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def track_all(self) -> None:
        subprocess.run(
            ["git", "-C", str(self.root), "add", "--all"],
            check=True,
            capture_output=True,
            text=True,
        )

    def run_audit(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(AUDIT),
                "--root",
                str(self.root),
                "--check",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_code_bearing_policy_excludes_data_and_unscoped_text(self) -> None:
        # Break caught: a path rule silently drops build/workflow code or
        # starts requiring license comments in strict data files.
        module = load_audit_module()

        self.assertTrue(module.is_code_bearing("src/core.c"))
        self.assertTrue(module.is_code_bearing("include/llam/runtime.h"))
        self.assertTrue(
            module.is_code_bearing(".github/workflows/linux.yml")
        )
        self.assertTrue(
            module.is_code_bearing("docker/linux/Dockerfile.ubuntu24")
        )
        self.assertTrue(module.is_code_bearing("CMakeLists.txt"))
        self.assertTrue(module.is_code_bearing("Makefile"))
        self.assertFalse(
            module.is_code_bearing("config/llam-version.json")
        )
        self.assertFalse(module.is_code_bearing("docs/design.md"))
        self.assertFalse(module.is_code_bearing("random.py"))

    def test_marker_must_be_valid_and_near_the_file_start(self) -> None:
        # Break caught: substring matching accepts an invalid identifier or
        # scans arbitrary body text instead of the leading license block.
        module = load_audit_module()
        late_marker = "\n" * 45 + (
            "# SPDX-License-Identifier: Apache-2.0\n"
        )

        self.assertTrue(
            module.has_license_marker(
                "#!/usr/bin/env python3\n"
                "# SPDX-License-Identifier: Apache-2.0\n"
            )
        )
        self.assertTrue(
            module.has_license_marker(
                "/* Licensed under the Apache License, "
                'Version 2.0 (the "License"); */\n'
            )
        )
        self.assertFalse(
            module.has_license_marker(
                "# SPDX-License-Identifier: Apache 2\n"
            )
        )
        self.assertFalse(module.has_license_marker(late_marker))

    def test_cli_reports_only_unmarked_eligible_tracked_files(self) -> None:
        # Break caught: the CLI audits untracked/data files, misses Docker or
        # workflow files, or emits diagnostics in filesystem order.
        self.write(
            "src/marked.c",
            "// SPDX-License-Identifier: Apache-2.0\nint marked;\n",
        )
        self.write("src/z_missing.c", "int missing;\n")
        self.write(
            "scripts/tool.py",
            "#!/usr/bin/env python3\n"
            "# SPDX-License-Identifier: Apache-2.0\n",
        )
        self.write("config/llam-version.json", "{}\n")
        self.write("notes.py", "print('outside audited roots')\n")
        self.write(
            ".github/workflows/a_missing.yml",
            "name: missing\n",
        )
        self.write("docker/linux/Dockerfile.test", "FROM scratch\n")
        self.write("src/untracked.c", "int ignored;\n")
        self.track_all()
        subprocess.run(
            [
                "git",
                "-C",
                str(self.root),
                "reset",
                "-q",
                "--",
                "src/untracked.c",
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        result = self.run_audit()

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertEqual(
            result.stderr.splitlines(),
            [
                (
                    "missing license marker: "
                    ".github/workflows/a_missing.yml"
                ),
                (
                    "missing license marker: "
                    "docker/linux/Dockerfile.test"
                ),
                "missing license marker: src/z_missing.c",
            ],
        )

    def test_cli_accepts_spdx_full_notice_and_shebang_markers(self) -> None:
        # Break caught: accepted repository header forms regress or the
        # success receipt counts excluded data.
        self.write(
            "src/core.c",
            "/* Licensed under the Apache License, "
            'Version 2.0 (the "License"); */\n',
        )
        self.write(
            "scripts/tool.py",
            "#!/usr/bin/env python3\n"
            "# SPDX-License-Identifier: Apache-2.0\n",
        )
        self.write(
            ".github/workflows/check.yml",
            "# SPDX-License-Identifier: Apache-2.0\nname: check\n",
        )
        self.write("config/data.json", "{}\n")
        self.track_all()

        result = self.run_audit()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout,
            "license header audit passed: 3 code-bearing files\n",
        )
        self.assertEqual(result.stderr, "")

    def test_cli_rejects_tracked_symlink_without_following_it(self) -> None:
        # Break caught: audit follows a tracked link outside the repository and
        # mistakes the target's marker for the repository file's license.
        outside = self.root.parent / f"{self.root.name}-outside.c"
        outside.write_text(
            "// SPDX-License-Identifier: Apache-2.0\n",
            encoding="utf-8",
        )
        link = self.root / "src" / "linked.c"
        link.parent.mkdir(parents=True)
        try:
            link.symlink_to(outside)
        except (NotImplementedError, OSError) as exc:
            outside.unlink(missing_ok=True)
            self.skipTest(
                f"symlink creation is unavailable: {exc.__class__.__name__}"
            )
        self.addCleanup(outside.unlink, missing_ok=True)
        self.track_all()

        result = self.run_audit()

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(
            result.stderr,
            "non-regular tracked file: src/linked.c\n",
        )

    def test_cli_returns_two_for_non_repository_root(self) -> None:
        # Break caught: an unusable Git inventory is mistaken for an empty,
        # successfully audited source tree.
        with tempfile.TemporaryDirectory() as empty:
            result = subprocess.run(
                [
                    sys.executable,
                    str(AUDIT),
                    "--root",
                    empty,
                    "--check",
                ],
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("cannot inventory tracked files:", result.stderr)


if __name__ == "__main__":
    unittest.main()
