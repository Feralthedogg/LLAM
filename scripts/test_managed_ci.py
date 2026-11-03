#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSES/Apache-2.0.txt.

"""Keep mandatory compiler/SDK coverage fail-closed on missing or skipped tests."""
from __future__ import annotations

from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from verify_managed_ci import REQUIRED_TESTS, require_completed_tests, require_registered_tests


class ManagedCIGateTests(unittest.TestCase):
    def test_inventory_requires_every_stage(self) -> None:
        entries = [{"name": name} for name in REQUIRED_TESTS]
        require_registered_tests({"tests": entries})
        for omitted in REQUIRED_TESTS:
            with self.subTest(omitted=omitted), self.assertRaises(ValueError):
                require_registered_tests({"tests": [row for row in entries if row["name"] != omitted]})
        with self.assertRaises(ValueError):
            require_registered_tests({"tests": entries + [entries[0]]})

    def test_results_require_execution_and_success(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".llam-managed-ci-", dir=Path(__file__).resolve().parents[1]) as directory:
            report = Path(directory) / "ctest.xml"
            for marker in (None, "skipped", "failure", "error", "missing", "duplicate", "disabled"):
                with self.subTest(marker=marker):
                    suite = ET.Element("testsuite")
                    for name in REQUIRED_TESTS:
                        if marker == "missing" and name == REQUIRED_TESTS[-1]:
                            continue
                        item = ET.SubElement(suite, "testcase", name=name, status="run")
                        if name == REQUIRED_TESTS[-1]:
                            if marker in {"skipped", "failure", "error"}:
                                ET.SubElement(item, marker)
                            elif marker == "disabled":
                                item.set("status", "disabled")
                    if marker == "duplicate":
                        ET.SubElement(suite, "testcase", name=REQUIRED_TESTS[-1])
                    ET.ElementTree(suite).write(report)
                    if marker is None:
                        require_completed_tests(report)
                    else:
                        with self.assertRaises(ValueError):
                            require_completed_tests(report)


if __name__ == "__main__":
    unittest.main()
