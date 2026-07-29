#!/usr/bin/env python3
"""Fixture tests for LLAM's scope-aware C structure audit."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


AUDIT = Path(__file__).resolve().with_name("audit_c_structure.py")
SCHEMA = "llam.c-structure-baseline.v1"


class StructureAuditFixture(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.root = Path(self._temp.name)
        (self.root / "include" / "llam").mkdir(parents=True)
        (self.root / "src" / "internal").mkdir(parents=True)
        self.baseline = self.root / "config" / "c-structure-baseline.json"
        self.write_baseline([])

    def tearDown(self) -> None:
        self._temp.cleanup()

    def write_lines(self, relative: str, count: int) -> None:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("/* fixture line */\n" * count, encoding="utf-8")

    def write_baseline(self, entries: list[dict[str, object]]) -> None:
        self.baseline.parent.mkdir(parents=True, exist_ok=True)
        self.baseline.write_text(
            json.dumps(
                {"schema": SCHEMA, "entries": entries},
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )

    def create_symlink(
        self,
        link: Path,
        target: Path | str,
        *,
        target_is_directory: bool = False,
    ) -> None:
        try:
            link.symlink_to(
                target,
                target_is_directory=target_is_directory,
            )
        except (NotImplementedError, OSError) as exc:
            self.skipTest(
                f"symlink creation is unavailable: {exc.__class__.__name__}"
            )

    def audit(
        self,
        *arguments: str,
        root: Path | None = None,
        baseline: Path | None = None,
    ) -> subprocess.CompletedProcess[str]:
        command = [
            sys.executable,
            str(AUDIT),
            "--root",
            str(self.root if root is None else root),
            *arguments,
        ]
        if baseline is not None:
            command.extend(["--baseline", str(baseline)])
        return subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
        )

    @staticmethod
    def findings(
        result: subprocess.CompletedProcess[str],
    ) -> list[dict[str, object]]:
        return [
            json.loads(line)
            for line in result.stdout.splitlines()
            if line.startswith("{")
        ]

    def test_report_returns_warnings_without_failing(self) -> None:
        # Break caught: report mode accidentally becomes a promotion gate.
        self.write_lines("src/core/debug/debug.c", 751)
        self.write_lines("src/new_component.c", 800)

        result = self.audit("--mode", "report")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.findings(result),
            [
                {
                    "category": "size_budget",
                    "limit": 750,
                    "line_count": 751,
                    "path": "src/core/debug/debug.c",
                    "severity": "warning",
                },
                {
                    "category": "split_candidate",
                    "limit": 800,
                    "line_count": 800,
                    "path": "src/new_component.c",
                    "severity": "warning",
                },
            ],
        )

    def test_strict_promotes_explicit_budget(self) -> None:
        # Break caught: strict checks message text and misses explicit budgets.
        self.write_lines("src/core/debug/debug.c", 751)

        result = self.audit("--mode", "strict")

        self.assertEqual(result.returncode, 1)
        self.assertIn('"category": "size_budget"', result.stdout)
        self.assertIn('"severity": "error"', result.stdout)

    def test_strict_promotes_generic_split_candidate(self) -> None:
        # Break caught: strict only promotes hard-coded file budgets.
        self.write_lines("src/new_component.c", 800)

        result = self.audit("--mode", "strict")

        self.assertEqual(result.returncode, 1)
        self.assertIn('"category": "split_candidate"', result.stdout)
        self.assertIn('"severity": "error"', result.stdout)

    def test_ratchet_scans_tests_and_experiments(self) -> None:
        # Break caught: tests and research experiments disappear from the scan.
        self.write_lines("tests/large_test.c", 800)
        self.write_lines("experiments/model.c", 800)

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 1)
        self.assertIn("tests/large_test.c", result.stdout)
        self.assertIn("experiments/model.c", result.stdout)

    def test_ratchet_rejects_new_oversized_file(self) -> None:
        # Break caught: unrecorded structural debt enters the repository.
        self.write_lines("src/new_component.c", 800)

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            self.findings(result),
            [
                {
                    "category": "split_candidate",
                    "limit": 800,
                    "line_count": 800,
                    "path": "src/new_component.c",
                    "severity": "error",
                }
            ],
        )

    def test_ratchet_allows_unchanged_grandfathered_file(self) -> None:
        # Break caught: ratchet acts like strict and blocks unchanged debt.
        self.write_lines("tests/legacy_test.c", 801)
        self.write_baseline(
            [
                {
                    "path": "tests/legacy_test.c",
                    "line_count": 801,
                    "limit": 800,
                }
            ]
        )

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.findings(result)[0]["severity"],
            "warning",
        )

    def test_ratchet_rejects_one_line_growth(self) -> None:
        # Break caught: existing debt can grow beyond its exact baseline count.
        self.write_lines("tests/legacy_test.c", 802)
        self.write_baseline(
            [
                {
                    "path": "tests/legacy_test.c",
                    "line_count": 801,
                    "limit": 800,
                }
            ]
        )

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(
            self.findings(result),
            [
                {
                    "category": "growth",
                    "limit": 801,
                    "line_count": 802,
                    "path": "tests/legacy_test.c",
                    "severity": "error",
                }
            ],
        )

    def test_ratchet_allows_reduced_grandfathered_file(self) -> None:
        # Break caught: paying down debt is rejected until the file is split.
        self.write_lines("tests/legacy_test.c", 800)
        self.write_baseline(
            [
                {
                    "path": "tests/legacy_test.c",
                    "line_count": 801,
                    "limit": 800,
                }
            ]
        )

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            self.findings(result)[0]["severity"],
            "warning",
        )

    def test_malformed_and_unsafe_baselines_fail_closed(self) -> None:
        # Break caught: corrupt or path-traversing policy is silently ignored.
        cases = (
            ("not JSON\n", "valid JSON"),
            (
                (
                    '{"schema":"llam.c-structure-baseline.v1",'
                    '"schema":"llam.c-structure-baseline.v1",'
                    '"entries":[]}'
                ),
                "duplicate JSON key",
            ),
            (
                (
                    '{"schema":"llam.c-structure-baseline.v1",'
                    '"entries":[{"path":"tests/legacy_test.c",'
                    '"line_count":NaN,"limit":800}]}'
                ),
                "non-finite JSON constant",
            ),
            (
                json.dumps(
                    {
                        "schema": "wrong",
                        "entries": [],
                    }
                ),
                "schema",
            ),
            (
                json.dumps(
                    {
                        "schema": SCHEMA,
                        "entries": [
                            {
                                "path": "../outside.c",
                                "line_count": 900,
                                "limit": 800,
                            }
                        ],
                    }
                ),
                "canonical relative path",
            ),
        )
        for contents, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                self.baseline.write_text(contents, encoding="utf-8")
                result = self.audit(
                    "--mode",
                    "ratchet",
                    baseline=self.baseline,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(diagnostic, result.stderr)

    def test_baseline_is_rejected_outside_ratchet_mode(self) -> None:
        # Break caught: a supplied policy file is silently ignored.
        for mode in ("report", "strict"):
            with self.subTest(mode=mode):
                result = self.audit(
                    "--mode",
                    mode,
                    baseline=self.baseline,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(
                    "--baseline is only valid in ratchet mode",
                    result.stderr,
                )

    def test_symlinked_baseline_is_rejected(self) -> None:
        # Break caught: policy identity is redirected through a symlink.
        real_baseline = self.baseline.with_name("real-baseline.json")
        self.baseline.replace(real_baseline)
        self.create_symlink(self.baseline, real_baseline.name)

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("baseline", result.stderr)
        self.assertIn("symlink", result.stderr)

    def test_baseline_cannot_raise_generic_limit(self) -> None:
        # Break caught: a baseline hides debt by redefining the global limit.
        self.write_lines("tests/legacy_test.c", 801)
        self.write_baseline(
            [
                {
                    "path": "tests/legacy_test.c",
                    "line_count": 801,
                    "limit": 801,
                }
            ]
        )

        result = self.audit(
            "--mode",
            "ratchet",
            baseline=self.baseline,
        )

        self.assertEqual(result.returncode, 2)
        self.assertIn("configured limit 800", result.stderr)

    def test_symlinked_source_is_rejected(self) -> None:
        # Break caught: an audited path escapes the selected root via symlink.
        outside = self.root.parent / f"{self.root.name}-outside.c"
        outside.write_text("/* outside */\n" * 800, encoding="utf-8")
        link = self.root / "src" / "linked.c"
        try:
            self.create_symlink(link, outside)
            result = self.audit("--mode", "report")
        finally:
            outside.unlink(missing_ok=True)

        self.assertEqual(result.returncode, 2)
        self.assertIn("symlink", result.stderr)

    def test_symlinked_audited_directory_is_rejected(self) -> None:
        # Break caught: a complete scan scope escapes through a directory link.
        target = self.root / "research-scope"
        target.mkdir()
        self.create_symlink(
            self.root / "experiments",
            target,
            target_is_directory=True,
        )

        result = self.audit("--mode", "report")

        self.assertEqual(result.returncode, 2)
        self.assertIn("experiments", result.stderr)
        self.assertIn("symlink", result.stderr)

    def test_existing_structure_boundaries_still_fail(self) -> None:
        # Break caught: size refactoring drops the pre-existing boundary gates.
        self.write_lines("include/private.h", 1)
        include_violation = self.root / "src" / "include_violation.c"
        include_violation.write_text(
            '#include "../../include/internal/private.h"\n',
            encoding="utf-8",
        )
        self.write_lines("src/helper.c", 1)

        result = self.audit("--mode", "report")

        self.assertEqual(result.returncode, 1)
        self.assertIn(
            "public header outside include/llam: include/private.h",
            result.stderr,
        )
        self.assertIn("forbidden include path", result.stderr)
        self.assertIn("src/helper.c: forbidden broad filename stem", result.stderr)

    def test_findings_have_deterministic_path_order(self) -> None:
        # Break caught: filesystem enumeration makes evidence nondeterministic.
        for relative in (
            "tests/zeta.c",
            "src/zeta.c",
            "experiments/alpha.c",
            "examples/zeta.c",
            "include/llam/zeta.h",
        ):
            self.write_lines(relative, 800)

        first = self.audit("--mode", "report")
        second = self.audit("--mode", "report")

        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(
            [finding["path"] for finding in self.findings(first)],
            [
                "examples/zeta.c",
                "experiments/alpha.c",
                "include/llam/zeta.h",
                "src/zeta.c",
                "tests/zeta.c",
            ],
        )

    def test_invalid_root_and_missing_ratchet_baseline_return_two(self) -> None:
        # Break caught: invalid invocations are confused with policy failures.
        missing_root = self.root / "missing"
        invalid_root = self.audit("--mode", "report", root=missing_root)
        missing_baseline = self.audit("--mode", "ratchet")

        self.assertEqual(invalid_root.returncode, 2)
        self.assertIn("root", invalid_root.stderr)
        self.assertEqual(missing_baseline.returncode, 2)
        self.assertIn("--baseline", missing_baseline.stderr)


if __name__ == "__main__":
    unittest.main()
