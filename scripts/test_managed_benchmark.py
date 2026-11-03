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
# See LICENSE.

"""Reject benchmark claims whose complete production source inputs drifted."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import os
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("managed_benchmark", ROOT / "experiments/async_compare/run_managed.py")
assert SPEC is not None and SPEC.loader is not None
BENCHMARK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCHMARK)


class BenchmarkInputTests(unittest.TestCase):
    def test_changes_additions_and_removals_invalidate_snapshot(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".llam-benchmark-inputs-", dir=ROOT) as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text("project(test)\n")
            (root / "Makefile").write_text("all:\n")
            source = root / "src/core/task.c"
            source.parent.mkdir(parents=True)
            source.write_text("void task(void) {}\n")
            before = BENCHMARK.source_snapshot(root)
            self.assertIn("src/core/task.c", before)
            self.assertEqual(BENCHMARK.snapshot_changes(before, BENCHMARK.source_snapshot(root)), [])
            # A new production file must invalidate even when no known old path changed.
            added = root / "src/core/added.c"
            added.write_text("void added(void) {}\n")
            self.assertEqual(BENCHMARK.snapshot_changes(before, BENCHMARK.source_snapshot(root)), ["src/core/added.c"])
            added.unlink()
            source.write_text("void task(void) { return; }\n")
            self.assertEqual(BENCHMARK.snapshot_changes(before, BENCHMARK.source_snapshot(root)), ["src/core/task.c"])
            source.unlink()
            self.assertEqual(BENCHMARK.snapshot_changes(before, BENCHMARK.source_snapshot(root)), ["src/core/task.c"])

    def test_build_and_compiler_inputs_are_covered(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".llam-benchmark-inputs-", dir=ROOT) as directory:
            root = Path(directory)
            for name in ("CMakeLists.txt", "Makefile", "include/llam/runtime.h",
                         "src/asm/context_layout.inc",
                         "tools/preempt/managed_runtime_contracts.def",
                         "tools/preempt/managed_driver.py", "experiments/async_compare/managed_workload.c"):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("original\n")
            before = BENCHMARK.source_snapshot(root)
            for name in before:
                path = root / name
                path.write_text("changed\n")
                self.assertEqual(BENCHMARK.snapshot_changes(before, BENCHMARK.source_snapshot(root)), [name])
                path.write_text("original\n")

    @unittest.skipIf(os.name == "nt", "native benchmark input scopes are POSIX")
    def test_linked_source_directories_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix=".llam-benchmark-inputs-", dir=ROOT) as directory:
            root = Path(directory)
            (root / "CMakeLists.txt").write_text("project(test)\n")
            (root / "Makefile").write_text("all:\n")
            (root / "src").mkdir()
            (root / "outside").mkdir()
            (root / "src/alias").symlink_to(root / "outside", target_is_directory=True)
            with self.assertRaises(ValueError):
                BENCHMARK.source_snapshot(root)


if __name__ == "__main__":
    unittest.main()
