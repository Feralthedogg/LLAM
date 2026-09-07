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
# See LICENSES/OLD-LICENSE/Apache-2.0.txt.

"""Check that Phase 0 worker identity code stays descriptive and shard-owned."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import pathlib
import re
import sys
from typing import Iterable, Sequence


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEBUG_EQUIVALENCE_PATH = "src/internal/runtime_state.h"

FUTURE_EXECUTION_AUTHORITY_NAMES = frozenset(
    {
        "worker_is_executor",
        "worker_owns_processor",
        "can_execute_processor",
        "processor_current_worker",
    }
)
FUTURE_EXECUTION_AUTHORITY_MEMBERS = frozenset(
    {
        "active_g",
        "owns_processor",
        "preempt_target",
        "executor",
        "current_g",
        "lease_epoch",
    }
)
OBSOLETE_SIGNAL_STACK_MEMBERS = frozenset(
    {
        "signal_stack",
        "signal_stack_size",
        "previous_sigaltstack",
        "sigaltstack_installed",
    }
)
CONTEXT_MEMBERS = frozenset({"scheduler_ctx", "opaque_scheduler_ctx"})


@dataclass(frozen=True)
class _Token:
    value: str
    line: int


@dataclass
class _ConditionalFrame:
    debug_guard: bool
    branch: int = 0
    guard_name: str = ""


def _mask_comments_and_literals(text: str) -> str:
    """Replace comments and literals with spaces while preserving newlines."""

    chars = list(text)
    index = 0
    state = "normal"
    while index < len(chars):
        current = chars[index]
        following = chars[index + 1] if index + 1 < len(chars) else ""

        if state == "normal":
            if current == "/" and following == "/":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "line-comment"
                continue
            if current == "/" and following == "*":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "block-comment"
                continue
            if current == '"':
                chars[index] = " "
                index += 1
                state = "string"
                continue
            if current == "'":
                chars[index] = " "
                index += 1
                state = "character"
                continue
            index += 1
            continue

        if state == "line-comment":
            if current in "\r\n":
                state = "normal"
            else:
                chars[index] = " "
            index += 1
            continue

        if state == "block-comment":
            if current == "*" and following == "/":
                chars[index] = chars[index + 1] = " "
                index += 2
                state = "normal"
                continue
            if current not in "\r\n":
                chars[index] = " "
            index += 1
            continue

        if state in {"string", "character"}:
            if current == "\\":
                chars[index] = " "
                if index + 1 < len(chars):
                    if chars[index + 1] not in "\r\n":
                        chars[index + 1] = " "
                    index += 2
                else:
                    index += 1
                continue
            if (state == "string" and current == '"') or (
                state == "character" and current == "'"
            ):
                chars[index] = " "
                index += 1
                state = "normal"
                continue
            if current not in "\r\n":
                chars[index] = " "
            index += 1
            continue

    return "".join(chars)


_TOKEN_RE = re.compile(
    r"(?P<identifier>[A-Za-z_][A-Za-z0-9_]*)"
    r"|(?P<operator>==|!=|->|&&|\|\||<=|>=|\+\+|--|<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=)"
    r"|(?P<number>(?:0[xX][0-9A-Fa-f]+|[0-9]+))"
    r"|(?P<punct>[^\s])"
)


def _tokens(masked: str) -> list[_Token]:
    tokens: list[_Token] = []
    for match in _TOKEN_RE.finditer(masked):
        tokens.append(_Token(match.group(0), masked.count("\n", 0, match.start()) + 1))
    return tokens


def _debug_equivalence_lines(relative_path: str, text: str) -> set[int]:
    """Return lines inside NDEBUG-only blocks in the one legacy helper header."""

    path = pathlib.PurePath(relative_path).as_posix()
    if path != DEBUG_EQUIVALENCE_PATH:
        return set()

    lines: set[int] = set()
    block_stack: list[_ConditionalFrame] = []
    directive_re = re.compile(r"^\s*#\s*(ifn?def|if|elif|else|endif)\b(.*)$")
    for number, raw_line in enumerate(_mask_comments_and_literals(text).splitlines(), start=1):
        match = directive_re.match(raw_line)
        if match is not None:
            directive, expression = match.groups()
            expression = expression.strip()
            if directive == "endif":
                if block_stack:
                    block_stack.pop()
            elif directive in {"elif", "else"}:
                if block_stack:
                    block_stack[-1].branch += 1
            elif directive == "ifndef":
                block_stack.append(
                    _ConditionalFrame(
                        debug_guard=expression == "NDEBUG", guard_name=expression
                    )
                )
            elif directive == "if":
                block_stack.append(
                    _ConditionalFrame(
                        debug_guard=bool(
                            re.fullmatch(
                                r"!\s*defined\s*\(\s*NDEBUG\s*\)", expression
                            )
                        )
                    )
                )

        debug_guard_index = next(
            (
                index
                for index, frame in enumerate(block_stack)
                if frame.debug_guard and frame.branch == 0
            ),
            None,
        )
        enclosing = block_stack[:debug_guard_index] if debug_guard_index is not None else []
        intended_outer_guard = not enclosing or (
            len(enclosing) == 1
            and enclosing[0].guard_name == "LLAM_RUNTIME_STATE_H"
            and enclosing[0].branch == 0
        )
        if (
            debug_guard_index is not None
            and debug_guard_index == len(block_stack) - 1
            and intended_outer_guard
        ):
            lines.add(number)
    return lines


def _context_operand(tokens: Sequence[_Token], start: int, step: int) -> bool:
    """Check one side of a comparison for a direct scheduler context operand."""

    delimiters = {";", "{", "}", ",", "&&", "||", "==", "!=", "="}
    paren_depth = 0
    bracket_depth = 0
    brace_depth = 0
    saw_context = False
    saw_nested_conditional = False
    index = start
    while 0 <= index < len(tokens):
        if index < 0 or index >= len(tokens):
            break
        value = tokens[index].value
        at_top_level = paren_depth == 0 and bracket_depth == 0 and brace_depth == 0
        if at_top_level and (value in delimiters or value in {"?", ":"}):
            break
        if value == "NULL" and not saw_context and at_top_level and index == start:
            return False
        if value in CONTEXT_MEMBERS:
            saw_context = True
        if value in {"?", ":"}:
            saw_nested_conditional = True

        if step > 0:
            if value == "(":
                paren_depth += 1
            elif value == ")" and paren_depth:
                paren_depth -= 1
            elif value == "[":
                bracket_depth += 1
            elif value == "]" and bracket_depth:
                bracket_depth -= 1
            elif value == "{":
                brace_depth += 1
            elif value == "}" and brace_depth:
                brace_depth -= 1
        else:
            if value == ")":
                paren_depth += 1
            elif value == "(" and paren_depth:
                paren_depth -= 1
            elif value == "]":
                bracket_depth += 1
            elif value == "[" and bracket_depth:
                bracket_depth -= 1
            elif value == "}":
                brace_depth += 1
            elif value == "{" and brace_depth:
                brace_depth -= 1
        index += step
    return saw_context and not saw_nested_conditional


def _raw_context_role_checks(
    tokens: Sequence[_Token], allowed_lines: set[int]
) -> list[tuple[int, str]]:
    def has_tls_operand(start: int, step: int) -> bool:
        if not 0 <= start < len(tokens):
            return False
        if tokens[start].value == "g_llam_tls_scheduler_ctx":
            return True

        if step < 0 and tokens[start].value == ")":
            depth = 1
            opening = start - 1
            while opening >= 0 and depth:
                if tokens[opening].value == ")":
                    depth += 1
                elif tokens[opening].value == "(":
                    depth -= 1
                opening -= 1
            opening += 1
            closing = start
        elif step > 0 and tokens[start].value == "(":
            depth = 1
            closing = start + 1
            while closing < len(tokens) and depth:
                if tokens[closing].value == "(":
                    depth += 1
                elif tokens[closing].value == ")":
                    depth -= 1
                closing += 1
            closing -= 1
            opening = start
        else:
            return False

        inner = list(tokens[opening + 1 : closing])
        while len(inner) >= 2 and inner[0].value == "(" and inner[-1].value == ")":
            depth = 0
            wraps_all = True
            for inner_index, inner_token in enumerate(inner):
                if inner_token.value == "(":
                    depth += 1
                elif inner_token.value == ")":
                    depth -= 1
                    if depth == 0 and inner_index != len(inner) - 1:
                        wraps_all = False
                        break
            if not wraps_all or depth != 0:
                break
            inner = inner[1:-1]
        return len(inner) == 1 and inner[0].value == "g_llam_tls_scheduler_ctx"

    errors: list[tuple[int, str]] = []
    for index, token in enumerate(tokens):
        if token.value not in {"==", "!="}:
            continue
        left_tls = has_tls_operand(index - 1, -1)
        right_tls = has_tls_operand(index + 1, 1)
        matches = (left_tls and _context_operand(tokens, index + 1, 1)) or (
            right_tls and _context_operand(tokens, index - 1, -1)
        )
        if matches and token.line not in allowed_lines:
            errors.append((token.line, "raw scheduler-context role check"))
    return errors


def _receiver_is_shard(tokens: Sequence[_Token], member_operator: int) -> bool:
    """Recognize a direct shard or shard-array receiver without matching workers."""

    if member_operator == 0:
        return False
    previous = tokens[member_operator - 1].value
    if previous in {"shard", "shards"}:
        return True
    if previous != "]":
        return False

    depth = 0
    for index in range(member_operator - 1, max(-1, member_operator - 32), -1):
        value = tokens[index].value
        if value == "]":
            depth += 1
        elif value == "[":
            depth -= 1
            if depth == 0 and index > 0:
                return tokens[index - 1].value in {"shard", "shards"}
    return False


def scan_source(relative_path: str, text: str) -> list[str]:
    """Return Phase 0 boundary violations found in one C source/header."""

    masked = _mask_comments_and_literals(text)
    tokens = _tokens(masked)
    errors: list[str] = []
    authority_reported: set[tuple[int, str]] = set()
    signal_reported: set[tuple[int, str]] = set()

    for index, token in enumerate(tokens):
        if token.value in FUTURE_EXECUTION_AUTHORITY_NAMES:
            key = (token.line, token.value)
            if key not in authority_reported:
                errors.append(f"{token.line}: Phase 0 execution authority identifier: {token.value}")
                authority_reported.add(key)
            continue
        if token.value in FUTURE_EXECUTION_AUTHORITY_MEMBERS:
            key = (token.line, token.value)
            if key not in authority_reported:
                errors.append(f"{token.line}: Phase 0 execution authority member: {token.value}")
                authority_reported.add(key)
            continue
        if token.value in {"->", "."} and index + 1 < len(tokens):
            member = tokens[index + 1]
            if member.value in OBSOLETE_SIGNAL_STACK_MEMBERS and _receiver_is_shard(tokens, index):
                key = (member.line, member.value)
                if key not in signal_reported:
                    errors.append(
                        f"{member.line}: obsolete shard signal-stack access: {member.value}"
                    )
                    signal_reported.add(key)

    errors.extend(
        f"{line}: {message}"
        for line, message in _raw_context_role_checks(
            tokens, _debug_equivalence_lines(relative_path, text)
        )
    )
    errors.sort(key=lambda item: (int(item.split(":", 1)[0]), item))
    return [f"{relative_path}:{error}" for error in errors]


def iter_source_files(root: pathlib.Path) -> Iterable[pathlib.Path]:
    source_root = root / "src"
    if not source_root.is_dir():
        return ()
    return sorted(
        path
        for path in source_root.rglob("*")
        if path.is_file() and path.suffix in {".c", ".h"}
    )


def scan_repository(root: pathlib.Path) -> list[str]:
    errors: list[str] = []
    for path in iter_source_files(root):
        relative = path.relative_to(root).as_posix()
        errors.extend(scan_source(relative, path.read_text(encoding="utf-8", errors="replace")))
    return errors


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT, help="repository root")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    if not root.is_dir():
        print(f"check_worker_identity_boundary: missing repository root: {root}", file=sys.stderr)
        return 2

    errors = scan_repository(root)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(
            f"check_worker_identity_boundary failed: {len(errors)} violation(s)",
            file=sys.stderr,
        )
        return 1

    count = sum(1 for _ in iter_source_files(root))
    print(f"check_worker_identity_boundary ok: {count} source/header file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
