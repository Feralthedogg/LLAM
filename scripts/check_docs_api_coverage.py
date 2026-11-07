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
"""Check that every public function has one row in the API reference.

With --site-dir, also check that each one rendered as a card with an anchor and
reached the symbol search index of a built site.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

HEADERS = ("runtime.h", "scope.h", "executor.h", "io.h", "managed.h", "platform.h")
RESERVED = frozenset({"llam_managed_native_enter", "llam_managed_native_leave"})
RESERVED_SECTION = "Reserved compiler ABI"

COMMENT = re.compile(r"/\*.*?\*/", re.S)
DOC_COMMENT = re.compile(r"/\*\*(?:(?!\*/).)*?\*/", re.S)
DECLARATION = re.compile(r"LLAM_API\s+[^;{}()]*?\b(llam_\w+)\s*\(([^;{}]*)\)\s*;", re.S)
SIZE_MACRO = re.compile(r"^#\s*define\s+(LLAM_\w+_CURRENT_SIZE)\b", re.M)
PARAM_TAG = re.compile(r"@param(?:\[[\w, ]+\])?\s+(\w+)")


def parameter_names(arguments: str) -> list[str]:
    names = []
    for argument in arguments.split(","):
        match = re.search(r"(\w+)\s*(?:\[[^\]]*\])?\s*$", argument)
        if match and match.group(1) != "void":
            names.append(match.group(1))
    return names


def read_headers(root: Path) -> list[str]:
    return [(root / "include" / "llam" / name).read_text(encoding="utf-8") for name in HEADERS]


def public_functions(root: Path) -> set[str]:
    names: set[str] = set()
    for text in read_headers(root):
        names.update(match.group(1) for match in DECLARATION.finditer(COMMENT.sub(" ", text)))
    return names


def parameter_tag_errors(root: Path) -> list[str]:
    """A @param must name a parameter of a declaration its comment documents."""
    errors = []
    for name, text in zip(HEADERS, read_headers(root)):
        for comment in DOC_COMMENT.finditer(text):
            tags = PARAM_TAG.findall(comment.group(0))
            if not tags:
                continue
            documented: set[str] = set()
            position = comment.end()
            # Declarations directly under a comment share it
            while True:
                match = DECLARATION.match(text, re.compile(r"\s*").match(text, position).end())
                if match is None:
                    break
                documented.update(parameter_names(match.group(2)))
                position = match.end()
            if position == comment.end():
                continue
            errors.extend(f"{name}: @param {tag} names no parameter" for tag in tags if tag not in documented)
    return errors


def api_rows(root: Path) -> dict[str, list[str]]:
    """Map each llam_* name in an API table to the page#section of its rows."""
    rows: dict[str, list[str]] = {}
    for page in sorted((root / "docs" / "reference" / "api").glob("*.md")):
        section = ""
        in_table = False
        for line in page.read_text(encoding="utf-8").splitlines():
            if line.startswith("#"):
                section = line.lstrip("#").strip()
            if not line.startswith("|"):
                in_table = False
                continue
            first = line.split("|")[1].strip()
            if first == "API":
                in_table = True
                continue
            if in_table:
                for name in re.findall(r"`([^`]+)`", first):
                    if name.startswith("llam_"):
                        rows.setdefault(name, []).append(f"{page.name}#{section}")
    return rows


def source_errors(root: Path) -> list[str]:
    errors = parameter_tag_errors(root)
    public = public_functions(root)
    rows = api_rows(root)
    for name in sorted(public):
        places = rows.get(name, [])
        if not places:
            errors.append(f"{name}: no row in an API table")
        elif len(places) > 1:
            errors.append(f"{name}: more than one row: {', '.join(places)}")
        elif (name in RESERVED) != places[0].endswith("#" + RESERVED_SECTION):
            errors.append(f"{name}: belongs {'in' if name in RESERVED else 'outside'} {RESERVED_SECTION}")
    errors.extend(f"{name}: row names no public function" for name in sorted(set(rows) - public))
    reference = "\n".join(
        path.read_text(encoding="utf-8") for path in (root / "docs" / "reference").rglob("*.md")
    )
    for macro in sorted({m for text in read_headers(root) for m in SIZE_MACRO.findall(text)}):
        if macro not in reference:
            errors.append(f"{macro}: not in the reference pages")
    return errors


def site_errors(root: Path, site: Path) -> list[str]:
    index = site / "search" / "site.json"
    if not index.is_file():
        return [f"{index}: missing"]
    symbols = {symbol["n"]: symbol["u"] for symbol in json.loads(index.read_text(encoding="utf-8"))["symbols"]}
    errors = []
    pages: dict[str, str] = {}
    for name in sorted(public_functions(root)):
        url = symbols.get(name)
        if url is None:
            errors.append(f"{name}: not in the symbol search index")
            continue
        path = url.split("#")[0]
        if path not in pages:
            page = site / path / "index.html"
            pages[path] = page.read_text(encoding="utf-8") if page.is_file() else ""
        if f'<details class="sym" id="{name}">' not in pages[path]:
            errors.append(f"{name}: no card on {path}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--site-dir", type=Path, help="built site to check as well")
    args = parser.parse_args()
    errors = source_errors(args.root)
    if args.site_dir is not None:
        errors.extend(site_errors(args.root, args.site_dir))
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print(f"API reference covers {len(public_functions(args.root))} public functions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
