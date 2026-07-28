#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

from pathlib import Path
import unittest

from scripts import check_ci_supply_chain as policy


class ActionPinTests(unittest.TestCase):
    def test_rejects_mutable_action_ref(self) -> None:
        violations = policy.check_action_pins(
            Path("ci.yml"), "steps:\n  - uses: actions/checkout@v6\n"
        )
        self.assertEqual(len(violations), 1)
        self.assertIn("full commit SHA", violations[0])

    def test_accepts_full_action_commit(self) -> None:
        violations = policy.check_action_pins(
            Path("ci.yml"),
            "steps:\n"
            "  - uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803 # v6\n",
        )
        self.assertEqual(violations, [])


class PermissionTests(unittest.TestCase):
    def test_rejects_workflow_level_write(self) -> None:
        violations = policy.check_workflow_permissions(
            Path("release.yml"), "permissions:\n  contents: write\njobs:\n"
        )
        self.assertEqual(len(violations), 1)

    def test_accepts_job_scoped_write(self) -> None:
        text = (
            "permissions:\n"
            "  contents: read\n"
            "jobs:\n"
            "  publish:\n"
            "    permissions:\n"
            "      contents: write\n"
        )
        self.assertEqual(policy.check_workflow_permissions(Path("release.yml"), text), [])

    def test_rejects_inline_workflow_write_all(self) -> None:
        violations = policy.check_workflow_permissions(
            Path("release.yml"), "permissions: write-all # unsafe\njobs:\n"
        )
        self.assertEqual(len(violations), 1)


class TransportTests(unittest.TestCase):
    def test_rejects_cleartext_workflow_url(self) -> None:
        violations = policy.check_cleartext_workflow_urls(
            Path("bsd.yml"), 'run: pkg add "http://mirror.invalid/repo"\n'
        )
        self.assertEqual(len(violations), 1)

    def test_accepts_https_workflow_url(self) -> None:
        violations = policy.check_cleartext_workflow_urls(
            Path("bsd.yml"), 'run: pkg add "https://mirror.invalid/repo"\n'
        )
        self.assertEqual(violations, [])


class DocumentationLockTests(unittest.TestCase):
    def test_rejects_ranged_unhashed_dependency(self) -> None:
        violations = policy.check_docs_requirements(
            Path("requirements.txt"), "mkdocs>=1.6,<2\n"
        )
        self.assertEqual(len(violations), 3)

    def test_accepts_exact_hashed_dependency(self) -> None:
        text = (
            "--only-binary=:all:\n"
            "mkdocs==1.6.1 \\\n"
            "    --hash=sha256:"
            "db91759624d1647f3f34aa0c3f327dd2601beae39a366d6e064c03468d35c20e\n"
        )
        self.assertEqual(
            policy.check_docs_requirements(Path("requirements.txt"), text), []
        )


class ReleaseTests(unittest.TestCase):
    def test_requires_non_persistent_checkout_credentials(self) -> None:
        text = (
            "permissions:\n"
            "  contents: read\n"
            "jobs:\n"
            "  publish-release:\n"
            "    permissions:\n"
            "      contents: write\n"
            "    steps:\n"
            "      - uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803\n"
        )
        violations = policy.check_release_workflow(Path("release.yml"), text)
        self.assertEqual(len(violations), 1)
        self.assertIn("persist-credentials", violations[0])

    def test_accepts_minimal_release_permissions(self) -> None:
        text = (
            "permissions:\n"
            "  contents: read\n"
            "jobs:\n"
            "  publish-release:\n"
            "    permissions:\n"
            "      contents: write\n"
            "    steps:\n"
            "      - uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803\n"
            "        with:\n"
            "          persist-credentials: false\n"
        )
        self.assertEqual(policy.check_release_workflow(Path("release.yml"), text), [])

    def test_rejects_builder_write_permission(self) -> None:
        text = (
            "permissions:\n"
            "  contents: read\n"
            "jobs:\n"
            "  build-artifacts:\n"
            "    permissions:\n"
            "      contents: write\n"
            "  publish-release:\n"
            "    permissions:\n"
            "      contents: write\n"
            "    steps: []\n"
        )
        violations = policy.check_release_workflow(Path("release.yml"), text)
        self.assertEqual(len(violations), 1)
        self.assertIn("build-artifacts", violations[0])


if __name__ == "__main__":
    unittest.main()
