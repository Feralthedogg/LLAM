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

"""Check the Phase 1A legacy lease and execution-publication boundaries.

The Phase 0 worker-identity scanner remains the source of truth for its own
rules.  This checker delegates to it and filters only the three explicitly
owned Phase 1A ``active_g`` surfaces: the worker field declaration, embedded
worker initialization, and the three worker publication helpers.  Additional
checks below reject independent shard/processor authority, unversioned pause
acknowledgements, unsafe watchdog target use, ownerless grants, and reverse
lease/shard lock acquisition.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import pathlib
import re
import sys
from typing import Iterable, Sequence

try:
    import check_worker_identity_boundary as phase0
except ModuleNotFoundError:  # Imported as ``scripts.check_legacy_lease_boundary``.
    from scripts import check_worker_identity_boundary as phase0


ROOT = pathlib.Path(__file__).resolve().parents[1]

ACTIVE_G_HELPERS = frozenset(
    {
        "llam_worker_publish_active_g",
        "llam_worker_load_active_g",
        "llam_worker_clear_active_g",
    }
)
ACTIVE_G_INIT_WORKERS = frozenset({"primary_worker", "opaque_helper_worker"})
PROCESSOR_ALLOWED_MEMBERS = frozenset({"runtime", "compat_shard", "id"})
PAUSE_ACK_FIELDS = frozenset({"merge_pause_ack", "steal_pause_ack"})
PAUSE_ACK_TUPLE_SUFFIXES = (
    "_ack_sequence",
    "_ack_lease_epoch",
    "_ack_worker_incarnation",
)
WATCHDOG_TARGET_MEMBERS = frozenset(
    {"native_thread", "primary_thread", "opaque_helper_thread", "preempt_thread", "thread"}
)
LEASE_OWNER_RECEIVERS = frozenset({"lease", "legacy_lease", "coordinator", "execution_lease"})
LEASE_OWNER_WRITE_HELPERS = frozenset(
    {
        "llam_legacy_lease_grant_helper_locked",
        "llam_legacy_lease_complete_handback_locked",
        "llam_legacy_lease_reacquire_primary_locked",
        "llam_legacy_lease_rollback_helper_locked",
    }
)
CONTROL_FLOW_NAMES = frozenset({"if", "for", "while", "switch", "catch"})


@dataclass(frozen=True)
class _FunctionSpan:
    name: str
    start_offset: int
    end_offset: int
    start_line: int
    end_line: int


@dataclass(frozen=True)
class _StructSpan:
    name: str
    body_start: int
    body_end: int
    start_line: int
    end_line: int


def _line_at(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def _matching_brace(text: str, opening: int) -> int | None:
    depth = 0
    for index in range(opening, len(text)):
        value = text[index]
        if value == "{":
            depth += 1
        elif value == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def _function_spans(masked: str) -> list[_FunctionSpan]:
    """Find C function definition bodies in masked source text."""

    spans: list[_FunctionSpan] = []
    definition_re = re.compile(
        r"\b(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\{",
        re.DOTALL,
    )
    for match in definition_re.finditer(masked):
        name = match.group("name")
        if name in CONTROL_FLOW_NAMES:
            continue
        opening = match.end() - 1
        closing = _matching_brace(masked, opening)
        if closing is None:
            continue
        spans.append(
            _FunctionSpan(
                name=name,
                start_offset=match.start(),
                end_offset=closing + 1,
                start_line=_line_at(masked, match.start()),
                end_line=_line_at(masked, closing),
            )
        )
    return spans


def _struct_spans(masked: str, name: str) -> list[_StructSpan]:
    spans: list[_StructSpan] = []
    struct_re = re.compile(r"\bstruct\s+" + re.escape(name) + r"\s*\{")
    for match in struct_re.finditer(masked):
        opening = match.end() - 1
        closing = _matching_brace(masked, opening)
        if closing is None:
            continue
        spans.append(
            _StructSpan(
                name=name,
                body_start=opening + 1,
                body_end=closing,
                start_line=_line_at(masked, match.start()),
                end_line=_line_at(masked, closing),
            )
        )
    return spans


def _function_for_line(spans: Sequence[_FunctionSpan], line: int) -> _FunctionSpan | None:
    matches = [span for span in spans if span.start_line <= line <= span.end_line]
    if not matches:
        return None
    return min(matches, key=lambda span: span.end_line - span.start_line)


def _struct_contains_line(spans: Sequence[_StructSpan], line: int, name: str) -> bool:
    return any(
        span.name == name and span.start_line <= line <= span.end_line for span in spans
    )


def _member_accesses(
    tokens: Sequence[phase0._Token],
) -> Iterable[tuple[int, str | None, str | None, str]]:
    """Yield ``(line, receiver-name, receiver-kind, member)`` for C member access."""

    def receiver_name(operator_index: int) -> str | None:
        previous = operator_index - 1
        if previous < 0:
            return None
        if tokens[previous].value != "]":
            value = tokens[previous].value
            return value if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) else None

        depth = 0
        for index in range(previous, -1, -1):
            value = tokens[index].value
            if value == "]":
                depth += 1
            elif value == "[":
                depth -= 1
                if depth == 0 and index > 0:
                    candidate = tokens[index - 1].value
                    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", candidate):
                        return candidate
                    return None
        return None

    for index, token in enumerate(tokens):
        if token.value not in {"->", "."} or index + 1 >= len(tokens):
            continue
        member = tokens[index + 1].value
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", member):
            continue
        receiver = receiver_name(index)
        if receiver in {"shard", "shards"}:
            kind = "shard"
        elif receiver in {"processor", "processors"}:
            kind = "processor"
        elif receiver in {"worker", "workers"}:
            kind = "worker"
        else:
            kind = None
        yield token.line, receiver, kind, member


def _active_g_allowed(relative_path: str, masked: str, line: int) -> bool:
    path = pathlib.PurePath(relative_path).as_posix()
    lines = masked.splitlines()
    if line < 1 or line > len(lines):
        return False
    line_text = lines[line - 1]

    if path == "src/internal/runtime_types.h":
        if not _struct_contains_line(_struct_spans(masked, "llam_worker"), line, "llam_worker"):
            return False
        return bool(re.search(r"\bactive_g\b\s*(?:\[[^]]*\])?\s*;", line_text)) and not bool(
            re.search(r"(?:->|\.)\s*active_g\b", line_text)
        )

    if path == "src/core/lifecycle/init.c":
        return bool(
            re.search(
                r"\batomic_init\s*\([^;]*\b(?:primary_worker|opaque_helper_worker)\s*\.\s*active_g\b",
                line_text,
            )
        )

    if path == "src/core/sched/scheduler.c":
        spans = _function_spans(masked)
        function = _function_for_line(spans, line)
        return function is not None and function.name in ACTIVE_G_HELPERS and bool(
            re.search(r"\bworker\s*->\s*active_g\b", line_text)
        )

    return False


def _phase0_errors(relative_path: str, text: str) -> list[str]:
    errors: list[str] = []
    active_error_re = re.compile(
        r":(?P<line>[0-9]+): Phase 0 execution authority member: active_g$"
    )
    for error in phase0.scan_source(relative_path, text):
        match = active_error_re.search(error)
        if match is not None and _active_g_allowed(relative_path, phase0._mask_comments_and_literals(text), int(match.group("line"))):
            continue
        errors.append(error)
    return errors


def _processor_struct_errors(relative_path: str, masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    spans = _struct_spans(masked, "llam_processor")
    identifier_re = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    for span in spans:
        body = masked[span.body_start : span.body_end]
        for statement in re.finditer(r"[^;]+;", body, re.DOTALL):
            statement_text = statement.group(0)
            if "(" in statement_text:
                continue
            identifiers = list(identifier_re.finditer(statement_text))
            if not identifiers:
                continue
            field = identifiers[-1].group(0)
            if field in PROCESSOR_ALLOWED_MEMBERS:
                continue
            field_offset = span.body_start + statement.start() + identifiers[-1].start()
            errors.append(
                (_line_at(masked, field_offset), "Phase 1A copied P resource in processor")
            )
    return errors


def _shard_struct_errors(masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    identifier_re = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
    for span in _struct_spans(masked, "llam_shard"):
        body = masked[span.body_start : span.body_end]
        for statement in re.finditer(r"[^;]+;", body, re.DOTALL):
            statement_text = statement.group(0)
            if "(" in statement_text:
                continue
            identifiers = list(identifier_re.finditer(statement_text))
            if not identifiers:
                continue
            field_match = identifiers[-1]
            field = field_match.group(0)
            if field not in {"current", "preempt_thread"}:
                continue
            field_offset = span.body_start + statement.start() + field_match.start()
            message = (
                "Phase 1A independent shard current authority"
                if field == "current"
                else "Phase 1A independent preempt target"
            )
            errors.append((_line_at(masked, field_offset), message))
    return errors


def _atomic_call_kind(tokens: Sequence[phase0._Token], member_index: int) -> str | None:
    depth = 0
    for index in range(member_index - 1, -1, -1):
        value = tokens[index].value
        if value == ")":
            depth += 1
        elif value == "(":
            if depth == 0:
                if index == 0:
                    return None
                candidate = tokens[index - 1].value
                return candidate if candidate.startswith("atomic_") else None
            depth -= 1
        elif depth == 0 and value in {";", "{", "}"}:
            break
    return None


def _ack_field_errors(relative_path: str, masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    function_spans = _function_spans(masked)
    struct_spans = _struct_spans(masked, "llam_shard")
    tuple_suffixes = PAUSE_ACK_TUPLE_SUFFIXES

    for line, _receiver, _kind, member in _member_accesses(tokens):
        if member not in PAUSE_ACK_FIELDS:
            continue
        if pathlib.PurePath(relative_path).as_posix() == "src/internal/runtime_types.h" and _struct_contains_line(
            struct_spans, line, "llam_shard"
        ):
            continue

        function = _function_for_line(function_spans, line)
        body = masked[function.start_offset : function.end_offset] if function else ""
        required = [member[: member.index("_ack")] + suffix for suffix in tuple_suffixes]
        has_tuple = all(name in body for name in required)
        call_kind = None
        for index, token in enumerate(tokens):
            if token.line == line and token.value == member:
                call_kind = _atomic_call_kind(tokens, index)
                break
        matcher = function is not None and function.name.endswith("pause_ack_matches_locked")
        publication = call_kind in {"atomic_store", "atomic_store_explicit", "atomic_init"}
        if has_tuple and (matcher or publication):
            continue
        errors.append((line, "Phase 1A unversioned pause acknowledgement decision"))
    return errors


def _owner_assignment_errors(masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    function_spans = _function_spans(masked)
    access_by_line: dict[int, list[tuple[int, str | None]]] = {}

    def receiver_name(operator_index: int) -> str | None:
        previous = operator_index - 1
        if previous < 0:
            return None
        value = tokens[previous].value
        if value != "]":
            return value if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) else None
        depth = 0
        for index in range(previous, -1, -1):
            value = tokens[index].value
            if value == "]":
                depth += 1
            elif value == "[":
                depth -= 1
                if depth == 0 and index > 0:
                    return tokens[index - 1].value
        return None

    for index, token in enumerate(tokens):
        if token.value not in {"->", "."} or index + 1 >= len(tokens):
            continue
        if tokens[index + 1].value != "owner":
            continue
        receiver = receiver_name(index)
        if receiver not in LEASE_OWNER_RECEIVERS:
            continue
        access_by_line.setdefault(token.line, []).append((index + 1, receiver))

    for line, accesses in access_by_line.items():
        function = _function_for_line(function_spans, line)
        for member_index, _receiver in accesses:
            if member_index + 1 >= len(tokens):
                continue
            operator = tokens[member_index + 1].value
            if operator in {"=", "+=", "-=", "*=", "/=", "%="}:
                value = tokens[member_index + 2].value if member_index + 2 < len(tokens) else ""
            else:
                call_kind = _atomic_call_kind(tokens, member_index)
                if not (call_kind or "").startswith("atomic_store"):
                    continue
                value = ""
                for next_index in range(member_index + 1, min(len(tokens), member_index + 12)):
                    if tokens[next_index].value == "," and next_index + 1 < len(tokens):
                        value = tokens[next_index + 1].value
                        break
            if value == "NULL":
                continue

            body = masked[function.start_offset : function.end_offset] if function else ""
            guard_lines = [
                _line_at(masked, function.start_offset + match.start())
                for match in re.finditer(
                    r"\b(?:lease|legacy_lease|coordinator|execution_lease)\s*(?:->|\.)\s*owner\s*!=\s*NULL",
                    body,
                )
            ]
            # Keep the proof visibly ahead of the write.  A same-line guard is
            # intentionally rejected because this lightweight source audit
            # cannot prove statement ordering within a line.
            guard_before_assignment = any(guard_line < line for guard_line in guard_lines)
            safe = (
                function is not None
                and function.name in LEASE_OWNER_WRITE_HELPERS
                and guard_before_assignment
                and "return_worker" in body
                and "llam_worker_binding_live" in body
            )
            if not safe:
                errors.append((line, "Phase 1A ownerless lease grant"))
    return errors


def _lock_order_errors(masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    lock_re = re.compile(
        r"pthread_mutex_(?P<action>lock|unlock)\s*\(\s*&\s*shard\s*->\s*(?P<which>opaque_lock|lock)"
    )
    shard_depth = 0
    for match in lock_re.finditer(masked):
        action = match.group("action")
        which = match.group("which")
        if action == "lock":
            if which == "opaque_lock":
                if shard_depth > 0:
                    errors.append(
                        (_line_at(masked, match.start()), "Phase 1A opaque_lock must precede shard->lock")
                    )
            else:
                shard_depth += 1
        elif which == "lock" and shard_depth > 0:
            shard_depth -= 1
    return errors


def _watchdog_errors(relative_path: str, masked: str) -> list[tuple[int, str]]:
    path = pathlib.PurePath(relative_path).as_posix()
    if not path.startswith("src/engine/watchdog/"):
        return []
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    for line, _receiver, _kind, member in _member_accesses(tokens):
        if member in WATCHDOG_TARGET_MEMBERS:
            errors.append((line, "Phase 1A watchdog target outside lease lifetime"))

    function_spans = _function_spans(masked)
    for match in re.finditer(r"\bpthread_kill\s*\(", masked):
        line = _line_at(masked, match.start())
        function = _function_for_line(function_spans, line)
        body = masked[function.start_offset : function.end_offset] if function else ""
        required = (
            "llam_legacy_lease_snapshot_watchdog_locked" in body
            and "llam_worker_binding_live" in body
            and "pthread_equal" in body
            and "opaque_lock" in body
        )
        if not required:
            errors.append((line, "Phase 1A watchdog target outside lease lifetime"))
    return errors


def _extra_authority_errors(masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    for token in tokens:
        if token.value == "owns_p":
            errors.append((token.line, "Phase 1A execution authority member: owns_p"))
    for line, receiver, kind, member in _member_accesses(tokens):
        if kind == "shard" and member == "current":
            errors.append((line, "Phase 1A independent shard current authority"))
        elif kind == "shard" and member == "preempt_thread":
            errors.append((line, "Phase 1A independent preempt target"))
        elif kind == "processor" and member not in PROCESSOR_ALLOWED_MEMBERS:
            if member not in {"executor", "current_g", "lease_epoch"}:
                errors.append((line, "Phase 1A copied P resource in processor"))
    return errors


def scan_source(relative_path: str, text: str) -> list[str]:
    """Return Phase 1A boundary violations found in one C source/header."""

    masked = phase0._mask_comments_and_literals(text)
    errors: list[str] = _phase0_errors(relative_path, text)
    extra: list[tuple[int, str]] = []
    extra.extend(_extra_authority_errors(masked))
    extra.extend(_processor_struct_errors(relative_path, masked))
    extra.extend(_shard_struct_errors(masked))
    extra.extend(_ack_field_errors(relative_path, masked))
    extra.extend(_owner_assignment_errors(masked))
    extra.extend(_lock_order_errors(masked))
    extra.extend(_watchdog_errors(relative_path, masked))

    seen = set(errors)
    for line, message in extra:
        error = f"{relative_path}:{line}: {message}"
        if error not in seen:
            errors.append(error)
            seen.add(error)

    def sort_key(error: str) -> tuple[int, str]:
        match = re.search(r":([0-9]+):", error)
        return (int(match.group(1)) if match else 0, error)

    errors.sort(key=sort_key)
    return errors


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
        print(f"check_legacy_lease_boundary: missing repository root: {root}", file=sys.stderr)
        return 2

    errors = scan_repository(root)
    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        print(
            f"check_legacy_lease_boundary failed: {len(errors)} violation(s)",
            file=sys.stderr,
        )
        return 1

    count = sum(1 for _ in iter_source_files(root))
    print(f"check_legacy_lease_boundary ok: {count} source/header file(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
