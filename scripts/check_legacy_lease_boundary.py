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
import bisect
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
PAUSE_ACK_ALL_FIELDS = frozenset(
    {
        "merge_pause_ack",
        "merge_pause_ack_sequence",
        "merge_pause_ack_lease_epoch",
        "merge_pause_ack_worker_incarnation",
        "steal_pause_ack",
        "steal_pause_ack_sequence",
        "steal_pause_ack_lease_epoch",
        "steal_pause_ack_worker_incarnation",
    }
)
PAUSE_ACK_MATCHERS = frozenset(
    {
        "llam_shard_merge_pause_ack_matches_locked",
        "llam_shard_steal_pause_ack_matches_locked",
    }
)
PAUSE_ACK_PUBLICATION_HELPERS = frozenset(
    {
        "llam_scheduler_publish_merge_pause_ack_locked",
        "llam_scheduler_publish_steal_pause_ack_locked",
    }
)
PAUSE_ACK_RESET_HELPERS = frozenset(
    {
        "llam_runtime_set_steal_pause",
        "llam_shard_request_merge_pause",
        "llam_shard_release_merge_pause",
    }
)
ATOMIC_LOAD_NAMES = frozenset({"atomic_load", "atomic_load_explicit"})
ATOMIC_STORE_NAMES = frozenset({"atomic_store", "atomic_store_explicit"})
ATOMIC_INIT_NAMES = frozenset({"atomic_init"})
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
LEASE_OWNER_VALUE_NAMES = {
    "llam_legacy_lease_grant_helper_locked": "helper",
    "llam_legacy_lease_complete_handback_locked": "claimant",
    "llam_legacy_lease_reacquire_primary_locked": "worker",
    "llam_legacy_lease_rollback_helper_locked": "primary",
}
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


@dataclass(frozen=True)
class _CallSite:
    name: str
    name_index: int
    open_index: int
    close_index: int
    args: tuple[tuple[int, ...], ...]


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


def _token_in_direct_struct(
    masked: str,
    token_index: int,
    tokens: Sequence[phase0._Token],
    spans: Sequence[_StructSpan],
    name: str,
) -> bool:
    starts = _token_starts(masked)
    if not 0 <= token_index < len(starts):
        return False
    containing = [
        span
        for span in spans
        if span.name == name and span.body_start <= starts[token_index] < span.body_end
    ]
    if not containing:
        return False
    outer = max(containing, key=lambda span: span.body_end - span.body_start)
    prefix = masked[outer.body_start : starts[token_index]]
    return prefix.count("{") == prefix.count("}")


def _token_starts(masked: str) -> list[int]:
    """Return source offsets in the same order as ``phase0._tokens``."""

    return [match.start() for match in phase0._TOKEN_RE.finditer(masked)]


def _token_range_for_span(masked: str, span: _FunctionSpan) -> tuple[int, int]:
    starts = _token_starts(masked)
    return (
        bisect.bisect_left(starts, span.start_offset),
        bisect.bisect_right(starts, span.end_offset),
    )


def _matching_token(
    tokens: Sequence[phase0._Token], opening: int, open_value: str, close_value: str
) -> int | None:
    """Find the balanced closing token for one delimiter."""

    if not 0 <= opening < len(tokens) or tokens[opening].value != open_value:
        return None
    depth = 0
    for index in range(opening, len(tokens)):
        value = tokens[index].value
        if value == open_value:
            depth += 1
        elif value == close_value:
            depth -= 1
            if depth == 0:
                return index
    return None


def _split_call_arguments(
    tokens: Sequence[phase0._Token], opening: int, closing: int
) -> tuple[tuple[int, ...], ...]:
    """Split a call's token range at top-level commas."""

    if closing <= opening + 1:
        return ()
    arguments: list[tuple[int, ...]] = []
    current: list[int] = []
    paren_depth = bracket_depth = brace_depth = 0
    for index in range(opening + 1, closing):
        value = tokens[index].value
        if value == "(":
            paren_depth += 1
        elif value == ")":
            paren_depth -= 1
        elif value == "[":
            bracket_depth += 1
        elif value == "]":
            bracket_depth -= 1
        elif value == "{":
            brace_depth += 1
        elif value == "}":
            brace_depth -= 1
        if (
            value == ","
            and paren_depth == 0
            and bracket_depth == 0
            and brace_depth == 0
        ):
            arguments.append(tuple(current))
            current = []
        else:
            current.append(index)
    if current:
        arguments.append(tuple(current))
    return tuple(arguments)


def _call_sites(
    tokens: Sequence[phase0._Token], names: Iterable[str] | None = None
) -> list[_CallSite]:
    wanted = frozenset(names) if names is not None else None
    calls: list[_CallSite] = []
    for index, token in enumerate(tokens):
        if wanted is not None and token.value not in wanted:
            continue
        if index + 1 >= len(tokens) or tokens[index + 1].value != "(":
            continue
        closing = _matching_token(tokens, index + 1, "(", ")")
        if closing is None:
            continue
        calls.append(
            _CallSite(
                name=token.value,
                name_index=index,
                open_index=index + 1,
                close_index=closing,
                args=_split_call_arguments(tokens, index + 1, closing),
            )
        )
    return calls


def _call_containing(
    calls: Sequence[_CallSite], token_index: int, names: Iterable[str] | None = None
) -> _CallSite | None:
    wanted = frozenset(names) if names is not None else None
    matches = [
        call
        for call in calls
        if (wanted is None or call.name in wanted)
        and call.open_index < token_index < call.close_index
    ]
    if not matches:
        return None
    return min(matches, key=lambda call: call.close_index - call.open_index)


def _arg_values(tokens: Sequence[phase0._Token], argument: Sequence[int]) -> tuple[str, ...]:
    return tuple(tokens[index].value for index in argument)


def _simple_identifier(
    tokens: Sequence[phase0._Token], argument: Sequence[int]
) -> str | None:
    values = _arg_values(tokens, argument)
    if len(values) == 1 and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", values[0]):
        return values[0]
    return None


def _addressed_identifier(
    tokens: Sequence[phase0._Token], argument: Sequence[int]
) -> str | None:
    values = _arg_values(tokens, argument)
    if len(values) == 2 and values[0] == "&" and re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*", values[1]
    ):
        return values[1]
    return None


def _function_indices(
    masked: str, tokens: Sequence[phase0._Token], function: _FunctionSpan
) -> range:
    starts = _token_starts(masked)
    begin = bisect.bisect_left(starts, function.start_offset)
    end = bisect.bisect_right(starts, function.end_offset)
    return range(begin, min(end, len(tokens)))


def _function_for_token(
    masked: str,
    tokens: Sequence[phase0._Token],
    functions: Sequence[_FunctionSpan],
    token_index: int,
) -> _FunctionSpan | None:
    starts = _token_starts(masked)
    if not 0 <= token_index < len(starts):
        return None
    offset = starts[token_index]
    matches = [
        function
        for function in functions
        if function.start_offset <= offset <= function.end_offset
    ]
    if not matches:
        return None
    return min(matches, key=lambda function: function.end_offset - function.start_offset)


def _statement_bounds(
    tokens: Sequence[phase0._Token], token_index: int, begin: int = 0, end: int | None = None
) -> tuple[int, int]:
    """Return a conservative semicolon/braced statement range."""

    if end is None:
        end = len(tokens)
    start = token_index
    while start > begin and tokens[start - 1].value not in {";", "{", "}"}:
        start -= 1
    finish = token_index
    while finish < end and tokens[finish].value not in {";", "}", "{"}:
        finish += 1
    if finish < end and tokens[finish].value == ";":
        finish += 1
    return start, finish


def _inside_constant_false_block(
    tokens: Sequence[phase0._Token], token_index: int, begin: int, end: int
) -> bool:
    """Reject proof tokens nested in an obviously unreachable ``if (false)``."""

    for index in range(begin, min(token_index, end)):
        if tokens[index].value != "if" or index + 1 >= end or tokens[index + 1].value != "(":
            continue
        condition_end = _matching_token(tokens, index + 1, "(", ")")
        if condition_end is None or condition_end >= token_index:
            continue
        condition = [tokens[item].value for item in range(index + 2, condition_end)]
        if condition not in (["false"], ["0"], ["0", "U"]):
            continue
        body_open = condition_end + 1
        if body_open >= end or tokens[body_open].value != "{":
            continue
        body_close = _matching_token(tokens, body_open, "{", "}")
        if body_close is not None and body_open < token_index < body_close:
            return True
    return False


def _member_access_tokens(
    tokens: Sequence[phase0._Token],
) -> Iterable[tuple[int, int, int, str | None, str | None, str]]:
    """Yield member accesses with token indices for precise validation."""

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
        yield index, index + 1, token.line, receiver, kind, member


def _member_accesses(
    tokens: Sequence[phase0._Token],
) -> Iterable[tuple[int, str | None, str | None, str]]:
    """Yield ``(line, receiver-name, receiver-kind, member)`` for C member access."""

    for _operator, _member_index, line, receiver, kind, member in _member_access_tokens(tokens):
        yield line, receiver, kind, member


def _active_g_allowed_indices(
    relative_path: str, masked: str, tokens: Sequence[phase0._Token]
) -> set[int]:
    """Return only the token positions covered by the Phase 1A allowlist."""

    path = pathlib.PurePath(relative_path).as_posix()
    starts = _token_starts(masked)
    allowed: set[int] = set()

    if path == "src/internal/runtime_types.h":
        spans = _struct_spans(masked, "llam_worker")
        for index, token in enumerate(tokens):
            if token.value != "active_g" or not 0 <= index < len(starts):
                continue
            offset = starts[index]
            containing = [span for span in spans if span.body_start <= offset < span.body_end]
            if not containing:
                continue
            outer = max(containing, key=lambda span: span.body_end - span.body_start)
            if (
                masked[outer.body_start:offset].count("{")
                != masked[outer.body_start:offset].count("}")
            ):
                continue
            statement_start = index
            while statement_start > 0 and tokens[statement_start - 1].value not in {
                ";",
                "{",
                "}",
            }:
                statement_start -= 1
            statement_end = index + 1
            while statement_end < len(tokens) and tokens[statement_end].value != ";":
                statement_end += 1
            if statement_end >= len(tokens):
                continue
            statement = [tokens[item].value for item in range(statement_start, statement_end + 1)]
            if statement == ["_Atomic", "(", "llam_task_t", "*", ")", "active_g", ";"]:
                allowed.add(index)
        return allowed

    if path == "src/core/lifecycle/init.c":
        calls = _call_sites(tokens, ATOMIC_INIT_NAMES)
        for call in calls:
            if len(call.args) != 2 or _arg_values(tokens, call.args[1]) != ("NULL",):
                continue
            values = _arg_values(tokens, call.args[0])
            if len(values) < 11 or values[:5] != ("&", "rt", "->", "shards", "["):
                continue
            close_bracket = None
            depth = 0
            for pos in range(4, len(values)):
                if values[pos] == "[":
                    depth += 1
                elif values[pos] == "]":
                    depth -= 1
                    if depth == 0:
                        close_bracket = pos
                        break
            if close_bracket is None:
                continue
            expected_prefix = values[:close_bracket + 1]
            if len(values) != close_bracket + 5:
                continue
            if (
                not expected_prefix[5:close_bracket]
                or len(values) <= close_bracket + 3
                or values[close_bracket + 1] != "."
                or values[close_bracket + 2] not in ACTIVE_G_INIT_WORKERS
                or values[close_bracket + 3 :]
                != (".", "active_g")
            ):
                continue
            for index in call.args[0]:
                if tokens[index].value == "active_g":
                    allowed.add(index)
        return allowed

    if path == "src/core/sched/scheduler.c":
        functions = _function_spans(masked)
        calls = _call_sites(tokens)
        for index, token in enumerate(tokens):
            if token.value != "active_g" or index < 2:
                continue
            if tokens[index - 2].value != "worker" or tokens[index - 1].value != "->":
                continue
            function = _function_for_token(masked, tokens, functions, index)
            if function is None or function.name not in ACTIVE_G_HELPERS:
                continue
            call = _call_containing(calls, index, ATOMIC_LOAD_NAMES | ATOMIC_STORE_NAMES)
            if call is None or not call.args:
                continue
            if _arg_values(tokens, call.args[0]) != ("&", "worker", "->", "active_g"):
                continue
            allowed.add(index)
    return allowed


def _active_g_allowed(relative_path: str, masked: str, line: int) -> bool:
    """Compatibility helper: every active-G token on ``line`` must be allowed."""

    tokens = phase0._tokens(masked)
    allowed = _active_g_allowed_indices(relative_path, masked, tokens)
    return all(
        token.value != "active_g" or index in allowed
        for index, token in enumerate(tokens)
        if token.line == line
    )


def _phase0_errors(relative_path: str, text: str) -> list[str]:
    errors: list[str] = []
    masked = phase0._mask_comments_and_literals(text)
    tokens = phase0._tokens(masked)
    allowed_active_g = _active_g_allowed_indices(relative_path, masked, tokens)
    invalid_active_g_lines = {
        token.line
        for index, token in enumerate(tokens)
        if token.value == "active_g" and index not in allowed_active_g
    }
    active_error_re = re.compile(
        r":(?P<line>[0-9]+): Phase 0 execution authority member: active_g$"
    )
    for error in phase0.scan_source(relative_path, text):
        match = active_error_re.search(error)
        if match is not None and int(match.group("line")) not in invalid_active_g_lines:
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


def _function_parameter_names(
    masked: str, tokens: Sequence[phase0._Token], function: _FunctionSpan
) -> list[str]:
    """Extract simple parameter names from one function definition."""

    indices = list(_function_indices(masked, tokens, function))
    body_open = next(
        (index for index in indices if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return []
    function_name = next(
        (
            index
            for index in indices
            if tokens[index].value == function.name
            and index + 1 < body_open
            and tokens[index + 1].value == "("
        ),
        None,
    )
    if function_name is None:
        return []
    closing = _matching_token(tokens, function_name + 1, "(", ")")
    if closing is None or closing >= body_open:
        return []
    arguments = _split_call_arguments(tokens, function_name + 1, closing)
    names: list[str] = []
    for argument in arguments:
        identifiers = [
            tokens[index].value
            for index in argument
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", tokens[index].value)
        ]
        if identifiers and identifiers != ["void"]:
            names.append(identifiers[-1])
    return names


def _direct_atomic_field_call(
    tokens: Sequence[phase0._Token],
    calls: Sequence[_CallSite],
    field: str,
    receiver: str | None = None,
    names: Iterable[str] | None = None,
) -> list[_CallSite]:
    wanted = frozenset(names) if names is not None else ATOMIC_LOAD_NAMES | ATOMIC_STORE_NAMES | ATOMIC_INIT_NAMES
    matches: list[_CallSite] = []
    for call in calls:
        if call.name not in wanted or not call.args:
            continue
        values = _arg_values(tokens, call.args[0])
        expected = ("&", receiver, "->", field) if receiver is not None else None
        if expected is not None and values != expected:
            continue
        if receiver is None and field not in values:
            continue
        if any(tokens[index].value == field for index in call.args[0]):
            matches.append(call)
    return matches


def _scalar_value(tokens: Sequence[phase0._Token], argument: Sequence[int]) -> str | None:
    values = list(_arg_values(tokens, argument))
    while len(values) >= 2 and values[0] == "(" and values[-1] == ")":
        values = values[1:-1]
    if len(values) == 1:
        return values[0]
    if values == ["0", "U"]:
        return "0U"
    if values == ["1", "U"]:
        return "1U"
    return None


def _is_zero_value(tokens: Sequence[phase0._Token], argument: Sequence[int]) -> bool:
    return _scalar_value(tokens, argument) in {"0", "0U", "false", "NULL"}


def _is_one_value(tokens: Sequence[phase0._Token], argument: Sequence[int]) -> bool:
    return _scalar_value(tokens, argument) in {"1", "1U", "true"}


def _call_field_and_value(
    tokens: Sequence[phase0._Token], call: _CallSite, field: str
) -> tuple[bool, bool, bool]:
    """Return ``(direct_field, has_value, is_zero)`` for an atomic call."""

    if not call.args:
        return False, False, False
    direct = any(tokens[index].value == field for index in call.args[0])
    value_argument = call.args[1] if len(call.args) > 1 else ()
    return direct, bool(value_argument), _is_zero_value(tokens, value_argument)


def _valid_rt_shards_field_call(
    tokens: Sequence[phase0._Token], call: _CallSite, field: str
) -> bool:
    if len(call.args) < 2:
        return False
    values = _arg_values(tokens, call.args[0])
    if len(values) < 8 or values[:5] != ("&", "rt", "->", "shards", "["):
        return False
    depth = 0
    closing = None
    for index in range(4, len(values)):
        if values[index] == "[":
            depth += 1
        elif values[index] == "]":
            depth -= 1
            if depth == 0:
                closing = index
                break
    if closing is None or not values[5:closing]:
        return False
    return values[closing + 1 :] == (".", field)


def _comparison_identifier(
    tokens: Sequence[phase0._Token], call: _CallSite, operator: str
) -> str | None:
    after = call.close_index + 1
    if after + 1 < len(tokens) and tokens[after].value == operator:
        return _simple_identifier(tokens, (after + 1,))
    before = call.open_index - 2
    if before >= 0 and tokens[before + 1].value == operator:
        return _simple_identifier(tokens, (before,))
    return None


def _comparison_is_zero(
    tokens: Sequence[phase0._Token], call: _CallSite, operator: str
) -> bool:
    after = call.close_index + 1
    if after + 1 < len(tokens) and tokens[after].value == operator:
        value = _scalar_value(tokens, (after + 1,))
        if value is not None:
            return value in {"0", "0U", "false"}
        if after + 2 < len(tokens) and tokens[after + 2].value == "U":
            return tokens[after + 1].value == "0"
    return False


def _matcher_is_valid(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    calls: Sequence[_CallSite],
) -> bool:
    if function.name not in PAUSE_ACK_MATCHERS:
        return False
    base = "merge" if "merge" in function.name else "steal"
    fields = (
        f"{base}_pause_ack",
        f"{base}_pause_ack_sequence",
        f"{base}_pause_ack_lease_epoch",
        f"{base}_pause_ack_worker_incarnation",
    )
    parameters = _function_parameter_names(masked, tokens, function)
    if len(parameters) < 4:
        return False
    begin, end = _token_range_for_span(masked, function)
    function_calls = [
        call
        for call in calls
        if begin <= call.name_index < end
        and call.name in ATOMIC_LOAD_NAMES
    ]
    field_calls: dict[str, _CallSite] = {}
    for field in fields:
        direct = _direct_atomic_field_call(
            tokens,
            function_calls,
            field,
            receiver=parameters[0],
            names=ATOMIC_LOAD_NAMES,
        )
        if not direct:
            return False
        field_calls[field] = direct[0]

    bool_call = field_calls[fields[0]]
    if not _comparison_is_zero(tokens, bool_call, "!="):
        return False
    expected_parameters = {
        fields[1]: parameters[1],
        fields[2]: parameters[2],
        fields[3]: parameters[3],
    }
    for field, parameter in expected_parameters.items():
        if _comparison_identifier(tokens, field_calls[field], "==") != parameter:
            return False

    ranges = {
        _statement_bounds(tokens, call.open_index, begin, end)
        for call in field_calls.values()
    }
    if len(ranges) != 1:
        return False
    statement_start, statement_end = next(iter(ranges))
    if not any(
        tokens[index].value == "&&" for index in range(statement_start, statement_end)
    ):
        return False
    return all(
        not _inside_constant_false_block(tokens, call.name_index, begin, end)
        for call in field_calls.values()
    )


def _publication_tuple_before(
    tokens: Sequence[phase0._Token], calls: Sequence[_CallSite], function: _FunctionSpan, base: str, before: int
) -> bool:
    required = (
        f"{base}_pause_ack_sequence",
        f"{base}_pause_ack_lease_epoch",
        f"{base}_pause_ack_worker_incarnation",
    )
    function_lines = (function.start_line, function.end_line)
    seen: set[str] = set()
    required_values = {
        f"{base}_pause_ack_sequence": lambda values: any(
            value in {"request_sequence", "request"} for value in values
        ),
        f"{base}_pause_ack_lease_epoch": lambda values: any(
            value in {"epoch", "epoch_value"} for value in values
        ),
        f"{base}_pause_ack_worker_incarnation": lambda values: any(
            value in {"incarnation", "worker_incarnation"} for value in values
        ),
    }
    for call in calls:
        if (
            call.name not in ATOMIC_STORE_NAMES
            or call.name_index >= before
            or not call.args
            or not function_lines[0] <= tokens[call.name_index].line <= function_lines[1]
        ):
            continue
        values = _arg_values(tokens, call.args[0])
        value_values = _arg_values(tokens, call.args[1]) if len(call.args) > 1 else ()
        for field in required:
            if (
                values == ("&", "shard", "->", field)
                and not _is_zero_value(tokens, call.args[1])
                and required_values[field](value_values)
            ):
                seen.add(field)
    return len(seen) == len(required)


def _ack_sampling_allowed(
    tokens: Sequence[phase0._Token], function: _FunctionSpan, member: str
) -> bool:
    """Permit tuple snapshots only when immediately routed to a matcher."""

    if not member.endswith(("_ack_sequence", "_ack_lease_epoch", "_ack_worker_incarnation")):
        return False
    base = "merge" if member.startswith("merge_") else "steal"
    body = [
        token.value
        for token in tokens
        if function.start_line <= token.line <= function.end_line
    ]
    return f"llam_shard_{base}_pause_ack_matches_locked" in body


def _ack_field_errors(relative_path: str, masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    path = pathlib.PurePath(relative_path).as_posix()
    tokens = phase0._tokens(masked)
    calls = _call_sites(tokens)
    functions = _function_spans(masked)
    struct_spans = _struct_spans(masked, "llam_shard")
    records = list(_member_access_tokens(tokens))
    matcher_valid: dict[str, bool] = {
        function.name: _matcher_is_valid(masked, tokens, function, calls)
        for function in functions
        if function.name in PAUSE_ACK_MATCHERS
    }

    def fail(line: int) -> None:
        errors.append((line, "Phase 1A unversioned pause acknowledgement decision"))

    for _operator, member_index, line, receiver, _kind, member in records:
        if member not in PAUSE_ACK_ALL_FIELDS:
            continue
        if path == "src/internal/runtime_types.h" and _token_in_direct_struct(
            masked, member_index, tokens, struct_spans, "llam_shard"
        ):
            continue

        function = _function_for_token(masked, tokens, functions, member_index)
        call = _call_containing(calls, member_index, ATOMIC_LOAD_NAMES | ATOMIC_STORE_NAMES | ATOMIC_INIT_NAMES)
        direct_receiver = receiver in {"shard", "source", "target", "first", "second"}
        if function is not None and function.name in PAUSE_ACK_MATCHERS:
            parameters = _function_parameter_names(masked, tokens, function)
            direct_receiver = bool(parameters) and receiver == parameters[0]
        allowed = False
        if path == "src/core/lifecycle/init.c" and call is not None:
            allowed = _valid_rt_shards_field_call(tokens, call, member) and _is_zero_value(
                tokens, call.args[1] if len(call.args) > 1 else ()
            )
        elif path == "src/core/lifecycle/shutdown.c" and function is not None and call is not None:
            allowed = (
                function.name == "llam_runtime_shutdown_unlocked"
                and call.name in ATOMIC_STORE_NAMES
                and _valid_rt_shards_field_call(tokens, call, member)
                and _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ())
            )
        elif path == "src/engine/watchdog/watchdog_merge.c" and function is not None and call is not None:
            allowed = (
                function.name in PAUSE_ACK_RESET_HELPERS
                and call.name in ATOMIC_STORE_NAMES
                and direct_receiver
                and _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ())
            )
            if not allowed and _ack_sampling_allowed(tokens, function, member):
                allowed = call.name in ATOMIC_LOAD_NAMES and direct_receiver
        elif path == "src/core/sched/scheduler.c" and function is not None:
            if function.name in PAUSE_ACK_MATCHERS:
                allowed = matcher_valid.get(function.name, False) and call is not None and call.name in ATOMIC_LOAD_NAMES
            elif function.name in PAUSE_ACK_PUBLICATION_HELPERS and call is not None:
                if call.name in ATOMIC_STORE_NAMES and direct_receiver:
                    if member in PAUSE_ACK_FIELDS:
                        allowed = _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ()) or (
                            _is_one_value(tokens, call.args[1] if len(call.args) > 1 else ())
                            and _publication_tuple_before(
                                tokens,
                                [candidate for candidate in calls if candidate.name_index < call.name_index],
                                function,
                                "merge" if member.startswith("merge_") else "steal",
                                call.name_index,
                            )
                        )
                    else:
                        allowed = True
            elif _ack_sampling_allowed(tokens, function, member):
                allowed = call is not None and call.name in ATOMIC_LOAD_NAMES and direct_receiver
        elif function is not None and _ack_sampling_allowed(tokens, function, member):
            allowed = call is not None and call.name in ATOMIC_LOAD_NAMES and direct_receiver

        if not allowed:
            fail(line)

    # A matcher with no direct, live tuple expression is itself a violation;
    # this catches bare/macro-hidden ACK decisions that contain no member token.
    for function in functions:
        if function.name in PAUSE_ACK_MATCHERS and not matcher_valid.get(function.name, False):
            fail(function.start_line)
    return errors


def _owner_write_call(
    tokens: Sequence[phase0._Token],
    calls: Sequence[_CallSite],
    member_index: int,
) -> tuple[_CallSite | None, str | None]:
    """Resolve a direct owner assignment or an atomic owner store."""

    if member_index + 1 < len(tokens) and tokens[member_index + 1].value in {
        "=",
        "+=",
        "-=",
        "*=",
        "/=",
        "%=",
    }:
        value = tokens[member_index + 2].value if member_index + 2 < len(tokens) else None
        return None, value
    call = _call_containing(calls, member_index, ATOMIC_STORE_NAMES)
    if call is None or len(call.args) < 2:
        return None, None
    if member_index not in call.args[0]:
        return None, None
    return call, _scalar_value(tokens, call.args[1]) or (
        tokens[call.args[1][0]].value if call.args[1] else None
    )


def _owner_proof_before(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    receiver: str,
    member_index: int,
    value: str | None,
) -> bool:
    """Require the known helper's lease origin, guard, claimant, and binding."""

    # The audited helpers all resolve one local ``lease`` from the shard
    # coordinator helper.  Other receiver spellings are deliberately rejected
    # instead of being treated as equivalent aliases.
    if function.name not in LEASE_OWNER_WRITE_HELPERS or receiver != "lease":
        return False
    if value != LEASE_OWNER_VALUE_NAMES.get(function.name):
        return False
    begin, _end = _token_range_for_span(masked, function)
    before = list(range(begin, member_index))
    values = [tokens[index].value for index in before]
    if "llam_shard_legacy_lease_locked" not in values:
        return False
    # Every production owner grant uses a non-NULL guard before publication.
    guard = any(
        values[index : index + 5]
        == [receiver, "->" if index + 1 < len(values) and values[index + 1] == "->" else ".", "owner", "!=", "NULL"]
        for index in range(max(0, len(values) - 24))
    )
    if not guard:
        return False
    if "llam_worker_binding_live" not in values:
        return False
    # A grant is sequenced by transfer_sequence; a handback/reacquire/rollback
    # is sequenced by the return claimant.  Requiring one of these markers
    # before the write rejects owner-slot writes that merely add a guard.
    if "return_worker" not in values and "transfer_sequence" not in values:
        return False
    return True


def _owner_assignment_errors(masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    calls = _call_sites(tokens)
    function_spans = _function_spans(masked)
    owner_slot_names: set[str] = set()
    lease_alias_names: set[str] = set(LEASE_OWNER_RECEIVERS)

    # Resolve aliases whose value is the shard's legacy-lease record before
    # looking at owner writes.  Unknown aliases remain fail-closed when they
    # appear in one of the audited transition helpers below.
    for index, token in enumerate(tokens):
        if token.value != "=" or index == 0 or index + 1 >= len(tokens):
            continue
        lhs = tokens[index - 1].value
        statement_end = index + 1
        while statement_end < len(tokens) and tokens[statement_end].value not in {";", "}"}:
            statement_end += 1
        statement = [tokens[item].value for item in range(index + 1, statement_end)]
        if (
            re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs)
            and statement
            and statement[0] == "&"
            and "legacy_lease" in statement
            and any(value in {"shard", "shards"} for value in statement)
        ):
            lease_alias_names.add(lhs)

    def fail(line: int) -> None:
        error = (line, "Phase 1A ownerless lease grant")
        if error not in errors:
            errors.append(error)

    for operator, member_index, line, receiver, _kind, member in _member_access_tokens(tokens):
        if member != "owner":
            continue
        # ``owner`` is a common field name in broker/synchronization records.
        # Only lease-shaped receivers (or the explicitly audited transition
        # helpers) belong to this boundary; unrelated owner fields are not
        # execution-lease authority.
        function = _function_for_token(masked, tokens, function_spans, member_index)
        lease_shaped = receiver in lease_alias_names or (
            receiver is not None and "lease" in receiver
        )
        if not lease_shaped and not (
            function is not None and function.name in LEASE_OWNER_WRITE_HELPERS
        ):
            continue
        call, value = _owner_write_call(tokens, calls, member_index)
        if call is None and value is None:
            # An address taken from the owner field creates an indirect write
            # surface.  The current runtime has no such path, so reject it
            # unless it is the first argument of a direct audited atomic call.
            statement_start = operator
            while statement_start > 0 and tokens[statement_start - 1].value not in {
                ";",
                "{",
                "}",
            }:
                statement_start -= 1
            address_taken = any(
                tokens[index].value == "&"
                for index in range(statement_start, operator)
            )
            if address_taken:
                equals = next(
                    (
                        index
                        for index in range(statement_start, operator)
                        if tokens[index].value == "="
                    ),
                    None,
                )
                if equals is not None and equals > statement_start:
                    lhs = tokens[equals - 1].value
                    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs):
                        owner_slot_names.add(lhs)
                fail(line)
            continue
        if value in {None, "NULL", "0", "0U", "false"}:
            if value is not None and receiver not in LEASE_OWNER_RECEIVERS:
                fail(line)
            continue
        if function is None or receiver not in LEASE_OWNER_RECEIVERS:
            fail(line)
            continue
        if not _owner_proof_before(
            masked, tokens, function, receiver or "", member_index, value
        ):
            fail(line)

    # Resolve owner-slot aliases conservatively.  Any dereference assignment
    # or atomic store through a name near an owner address is rejected.
    for index, token in enumerate(tokens):
        if token.value != "=" or index == 0 or index + 1 >= len(tokens):
            continue
        lhs = tokens[index - 1].value
        if tokens[index - 2].value != "*" if index >= 2 else True:
            continue
        if tokens[index + 1].value in {"NULL", "0", "0U", "false"}:
            continue
        function = _function_for_token(masked, tokens, function_spans, index)
        if lhs in owner_slot_names or (
            function is not None and function.name in LEASE_OWNER_WRITE_HELPERS
        ):
            fail(tokens[index].line)
    for call in calls:
        if call.name not in ATOMIC_STORE_NAMES or len(call.args) < 2:
            continue
        alias = _simple_identifier(tokens, call.args[0])
        function = _function_for_token(masked, tokens, function_spans, call.name_index)
        if (
            alias in owner_slot_names
            or function is not None
            and function.name in LEASE_OWNER_WRITE_HELPERS
            and alias is not None
        ) and not _is_zero_value(tokens, call.args[1]):
            fail(tokens[call.name_index].line)
    return errors


def _lock_order_errors(masked: str) -> list[tuple[int, str]]:
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    functions = _function_spans(masked)
    calls = _call_sites(
        tokens,
        {
            "pthread_mutex_lock",
            "pthread_mutex_trylock",
            "pthread_mutex_timedlock",
            "pthread_mutex_unlock",
        },
    )

    def receiver_for_lock(call: _CallSite) -> tuple[str | None, str | None]:
        if not call.args:
            return None, None
        values = list(_arg_values(tokens, call.args[0]))
        if values and values[0] == "&":
            values = values[1:]
        if len(values) < 3 or values[-1] not in {"opaque_lock", "lock"}:
            return None, None
        if values[-2] not in {"->", "."}:
            return None, None
        receiver = values[-3]
        if receiver == "]":
            depth = 0
            for index in range(len(values) - 3, -1, -1):
                if values[index] == "]":
                    depth += 1
                elif values[index] == "[":
                    depth -= 1
                    if depth == 0 and index > 0:
                        receiver = values[index - 1]
                        break
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver):
            return None, None
        return receiver, values[-1]

    def aliases_for(function: _FunctionSpan) -> dict[str, str]:
        begin, end = _token_range_for_span(masked, function)
        aliases: dict[str, str] = {}
        for index in range(begin + 1, end - 1):
            if tokens[index].value != "=" or index < begin + 1:
                continue
            lhs = tokens[index - 1].value
            rhs = tokens[index + 1].value if index + 1 < end else ""
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs):
                continue
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", rhs):
                continue
            aliases[lhs] = rhs
        for _ in range(len(aliases)):
            for alias, target in list(aliases.items()):
                aliases[alias] = aliases.get(target, target)
        return aliases

    def fail(call: _CallSite) -> None:
        error = (tokens[call.name_index].line, "Phase 1A opaque_lock must precede shard->lock")
        if error not in errors:
            errors.append(error)

    for function in functions:
        begin, end = _token_range_for_span(masked, function)
        aliases = aliases_for(function)
        held: dict[str, dict[str, bool]] = {}
        two_shard_lock_count = 0
        function_calls = sorted(
            (
                call
                for call in calls
                if begin <= call.name_index < end
            ),
            key=lambda call: call.name_index,
        )
        for call in function_calls:
            receiver, which = receiver_for_lock(call)
            if receiver is None or which is None:
                continue
            symbol = aliases.get(receiver, receiver)
            state = held.setdefault(symbol, {"opaque_lock": False, "lock": False})
            if call.name == "pthread_mutex_unlock":
                state[which] = False
                if receiver in {"first", "second"} and which == "lock" and two_shard_lock_count:
                    two_shard_lock_count -= 1
                continue
            if which == "opaque_lock" and state["lock"]:
                fail(call)
            if receiver in {"first", "second"}:
                if which == "lock":
                    two_shard_lock_count += 1
                elif two_shard_lock_count:
                    # The merge path acquires both first/second opaque locks
                    # before either shard lock.  A later opaque acquisition
                    # would invert that cross-shard order.
                    fail(call)
            state[which] = True
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
    calls = _call_sites(tokens)
    mutex_calls = _call_sites(
        tokens,
        {
            "pthread_mutex_lock",
            "pthread_mutex_trylock",
            "pthread_mutex_timedlock",
            "pthread_mutex_unlock",
        },
    )

    def fail(line: int) -> None:
        error = (line, "Phase 1A watchdog target outside lease lifetime")
        if error not in errors:
            errors.append(error)

    def direct_identifier_argument(call: _CallSite, argument: int) -> str | None:
        if argument >= len(call.args):
            return None
        return _simple_identifier(tokens, call.args[argument])

    for call in calls:
        if call.name != "pthread_kill" or len(call.args) < 2:
            continue
        function = _function_for_token(masked, tokens, function_spans, call.name_index)
        if function is None:
            fail(tokens[call.name_index].line)
            continue
        begin, end = _token_range_for_span(masked, function)
        signal_target = direct_identifier_argument(call, 0)
        signal_name = _scalar_value(tokens, call.args[1])
        if signal_target is None or signal_name != "LLAM_PREEMPT_SIGNAL":
            fail(tokens[call.name_index].line)
            continue

        snapshot_calls = [
            candidate
            for candidate in calls
            if candidate.name == "llam_legacy_lease_snapshot_watchdog_locked"
            and begin <= candidate.name_index < call.name_index
            and len(candidate.args) >= 5
        ]
        valid_snapshot = None
        for snapshot in reversed(snapshot_calls):
            native_target = _addressed_identifier(tokens, snapshot.args[4])
            owner = _addressed_identifier(tokens, snapshot.args[1])
            incarnation = _addressed_identifier(tokens, snapshot.args[2])
            if (
                native_target == signal_target
                and owner is not None
                and incarnation is not None
                and not _inside_constant_false_block(tokens, snapshot.name_index, begin, end)
            ):
                valid_snapshot = (snapshot, native_target, owner, incarnation)
                break
        if valid_snapshot is None:
            fail(tokens[call.name_index].line)
            continue
        snapshot, native_target, owner, incarnation = valid_snapshot
        snapshot_index = snapshot.name_index

        function_body_open = next(
            (
                index
                for index in range(begin, end)
                if tokens[index].value == "{"
            ),
            begin,
        )

        def brace_depth(token_index: int) -> int:
            depth = 0
            for index in range(function_body_open, min(token_index, end)):
                if tokens[index].value == "{":
                    depth += 1
                elif tokens[index].value == "}":
                    depth -= 1
            return depth

        # Proof calls in a nested conditional body are not dominance proofs:
        # the signal may execute on a path that skips that body.  The actual
        # watchdog keeps snapshot/binding/equality in the function-level guard
        # expression, so depth one is the conservative accepted shape.
        if any(
            brace_depth(candidate.name_index) != 1
            for candidate in calls
            if candidate.name in {
                "llam_legacy_lease_snapshot_watchdog_locked",
                "llam_worker_binding_live",
                "pthread_equal",
            }
            and snapshot_index <= candidate.name_index < call.name_index
            and (
                candidate.name == "llam_legacy_lease_snapshot_watchdog_locked"
                or candidate.name in {"llam_worker_binding_live", "pthread_equal"}
            )
        ):
            fail(tokens[call.name_index].line)
            continue

        binding_ok = any(
            candidate.name == "llam_worker_binding_live"
            and snapshot_index < candidate.name_index < call.name_index
            and len(candidate.args) >= 2
            and _simple_identifier(tokens, candidate.args[0]) == owner
            and _simple_identifier(tokens, candidate.args[1]) == incarnation
            and not _inside_constant_false_block(tokens, candidate.name_index, begin, end)
            for candidate in calls
        )
        equality_ok = False
        for candidate in calls:
            if candidate.name != "pthread_equal" or not (
                snapshot_index < candidate.name_index < call.name_index
            ):
                continue
            if len(candidate.args) < 2:
                continue
            left = _simple_identifier(tokens, candidate.args[0])
            right = _simple_identifier(tokens, candidate.args[1])
            if (
                left == native_target
                and right is not None
                and right != native_target
                or right == native_target
                and left is not None
                and left != native_target
            ) and not _inside_constant_false_block(tokens, candidate.name_index, begin, end):
                equality_ok = True
                break
        if not binding_ok or not equality_ok:
            fail(tokens[call.name_index].line)
            continue

        # The lease snapshot must be followed by both scheduler locks, and
        # neither lock may be released before the signal.  This keeps the
        # lifetime proof attached to the same source-level critical section.
        held_opaque = held_lock = False
        snapshot_had_opaque = snapshot_had_lock = False
        crossed_snapshot = False

        def lock_receiver(mutex: _CallSite) -> str | None:
            if not mutex.args:
                return None
            values = list(_arg_values(tokens, mutex.args[0]))
            if values and values[0] == "&":
                values = values[1:]
            if len(values) < 3 or values[-2] not in {"->", "."}:
                return None
            receiver = values[-3]
            return receiver if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver) else None

        snapshot_shard = _simple_identifier(tokens, snapshot.args[0])
        for mutex in sorted(
            (
                candidate
                for candidate in mutex_calls
                if begin <= candidate.name_index < call.name_index
            ),
            key=lambda candidate: candidate.name_index,
        ):
            if not mutex.args:
                continue
            if lock_receiver(mutex) != snapshot_shard:
                continue
            values = list(_arg_values(tokens, mutex.args[0]))
            if values and values[0] == "&":
                values = values[1:]
            which = values[-1] if len(values) >= 3 and values[-1] in {"opaque_lock", "lock"} else None
            if which is None:
                continue
            if not crossed_snapshot and mutex.name_index >= snapshot_index:
                snapshot_had_opaque = held_opaque
                snapshot_had_lock = held_lock
                crossed_snapshot = True
            if mutex.name == "pthread_mutex_unlock":
                # The production revalidation guard releases both locks only
                # on its failing return path.  That branch cannot reach the
                # later signal, so do not treat its cleanup as an intervening
                # lifetime break.  A real unlock-before-kill has no such
                # terminating return and remains visible to this check.
                lookahead = [
                    tokens[index].value
                    for index in range(
                        mutex.close_index + 1,
                        min(call.name_index, mutex.close_index + 18),
                    )
                ]
                if "return" in lookahead:
                    continue
                if which == "opaque_lock":
                    held_opaque = False
                else:
                    held_lock = False
            elif mutex.name in {
                "pthread_mutex_lock",
                "pthread_mutex_trylock",
                "pthread_mutex_timedlock",
            }:
                if which == "opaque_lock":
                    held_opaque = True
                else:
                    held_lock = True
        if not crossed_snapshot:
            snapshot_had_opaque = held_opaque
            snapshot_had_lock = held_lock
        if not (snapshot_had_opaque and snapshot_had_lock and held_opaque and held_lock):
            fail(tokens[call.name_index].line)
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
