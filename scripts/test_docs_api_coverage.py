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
"""Verify the public API reference coverage check."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import check_docs_api_coverage as coverage  # noqa: E402

HEADER = """
/** @brief Open it. @param path Where. @param[out] out Result. */
LLAM_API int llam_open(const char *path, int *out);
LLAM_API int llam_open_at(int dir, const char *path, int *out);

/* Not documented. */
LLAM_API void llam_close(void);
LLAM_API void llam_managed_native_enter(void);
LLAM_API void llam_managed_native_leave(void);
#define LLAM_THING_CURRENT_SIZE 4
"""

PAGE = """# Page

## Files

| API | Purpose |
| --- | --- |
| `llam_open` / `llam_open_at` | Open. |
| `llam_close` | Close. |

## Reserved compiler ABI

| API | Purpose |
| --- | --- |
| `llam_managed_native_enter` / `llam_managed_native_leave` | Compiler only. |

| Field | Meaning |
| --- | --- |
| `llam_not_an_api_row` | Other tables do not count. |

`LLAM_THING_CURRENT_SIZE` is a size token.
"""


class CoverageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "include" / "llam").mkdir(parents=True)
        (self.root / "docs" / "reference" / "api").mkdir(parents=True)
        for name in coverage.HEADERS:
            (self.root / "include" / "llam" / name).write_text("", encoding="utf-8")
        self.write_header(HEADER)
        self.write_page(PAGE)

    def write_header(self, text: str) -> None:
        (self.root / "include" / "llam" / "runtime.h").write_text(text, encoding="utf-8")

    def write_page(self, text: str) -> None:
        (self.root / "docs" / "reference" / "api" / "files.md").write_text(text, encoding="utf-8")

    def test_covered_reference_passes(self) -> None:
        self.assertEqual(coverage.source_errors(self.root), [])

    def test_prose_mention_is_not_a_row(self) -> None:
        self.write_page(PAGE.replace("| `llam_close` | Close. |\n", "") + "\nCall `llam_close` last.\n")
        self.assertEqual(coverage.source_errors(self.root), ["llam_close: no row in an API table"])

    def test_substring_of_a_longer_name_is_not_a_row(self) -> None:
        self.write_page(PAGE.replace("| `llam_open` / `llam_open_at` |", "| `llam_open_at` |"))
        self.assertEqual(coverage.source_errors(self.root), ["llam_open: no row in an API table"])

    def test_every_header_is_read(self) -> None:
        (self.root / "include" / "llam" / "managed.h").write_text(
            "LLAM_API int llam_extra(void);\n", encoding="utf-8"
        )
        self.assertEqual(coverage.source_errors(self.root), ["llam_extra: no row in an API table"])

    def test_commented_out_declaration_is_not_public(self) -> None:
        self.write_header(HEADER + "/* LLAM_API int llam_old(void); */\n")
        self.assertEqual(coverage.source_errors(self.root), [])

    def test_duplicate_row_fails(self) -> None:
        self.write_page(PAGE + "\n| API | Purpose |\n| --- | --- |\n| `llam_close` | Again. |\n")
        errors = coverage.source_errors(self.root)
        self.assertEqual(len(errors), 1)
        self.assertIn("more than one row", errors[0])

    def test_row_for_a_missing_function_fails(self) -> None:
        self.write_page(PAGE.replace("| `llam_close` | Close. |", "| `llam_close` | Close. |\n| `llam_gone` | Gone. |"))
        self.assertEqual(coverage.source_errors(self.root), ["llam_gone: row names no public function"])

    def test_reserved_functions_stay_in_their_section(self) -> None:
        self.write_page(PAGE.replace("## Reserved compiler ABI", "## Other"))
        errors = coverage.source_errors(self.root)
        self.assertEqual(len(errors), 2)
        self.assertIn("belongs in Reserved compiler ABI", errors[0])
        self.write_page(PAGE.replace("## Files", "## Reserved compiler ABI"))
        self.assertIn("belongs outside Reserved compiler ABI", coverage.source_errors(self.root)[0])

    def test_parameter_tag_must_match_a_declaration_in_its_group(self) -> None:
        self.write_header(HEADER.replace("@param path Where.", "@param pth Where."))
        self.assertEqual(coverage.source_errors(self.root), ["runtime.h: @param pth names no parameter"])

    def test_parameter_tag_can_name_a_later_declaration_in_its_group(self) -> None:
        self.write_header(HEADER.replace("@param path Where.", "@param dir Where."))
        self.assertEqual(coverage.source_errors(self.root), [])

    def test_undocumented_size_macro_fails(self) -> None:
        self.write_page(PAGE.replace("`LLAM_THING_CURRENT_SIZE` is a size token.\n", ""))
        self.assertEqual(coverage.source_errors(self.root), ["LLAM_THING_CURRENT_SIZE: not in the reference pages"])

    def build_site(self, skip_card: str = "", skip_search: str = "") -> Path:
        site = self.root / "site"
        page = site / "reference" / "api" / "files"
        page.mkdir(parents=True)
        (site / "search").mkdir()
        names = sorted(coverage.public_functions(self.root))
        (page / "index.html").write_text(
            "".join(f'<details class="sym" id="{name}">' for name in names if name != skip_card),
            encoding="utf-8",
        )
        (site / "search" / "site.json").write_text(
            json.dumps({"symbols": [{"n": name, "u": f"reference/api/files/#{name}"} for name in names if name != skip_search]}),
            encoding="utf-8",
        )
        return site

    def test_built_site_passes(self) -> None:
        self.assertEqual(coverage.site_errors(self.root, self.build_site()), [])

    def test_missing_card_fails(self) -> None:
        site = self.build_site(skip_card="llam_close")
        self.assertEqual(coverage.site_errors(self.root, site), ["llam_close: no card on reference/api/files/"])

    def test_missing_search_entry_fails(self) -> None:
        site = self.build_site(skip_search="llam_close")
        self.assertEqual(coverage.site_errors(self.root, site), ["llam_close: not in the symbol search index"])

    def test_missing_index_fails(self) -> None:
        errors = coverage.site_errors(self.root, self.root / "nowhere")
        self.assertEqual(len(errors), 1)
        self.assertIn("missing", errors[0])

    def test_repository_reference_is_covered(self) -> None:
        self.assertEqual(coverage.source_errors(ROOT), [])


if __name__ == "__main__":
    unittest.main()
