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
import functools
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
PAUSE_ACK_SAMPLING_FUNCTIONS = frozenset(
    {
        "llam_runtime_wait_steal_pause_ack",
        "llam_shard_wait_merge_pause_ack",
        "llam_shard_merge_pause_ack_current_locked",
        "llam_merge_lockfree_normq_locked",
        "llam_merge_runnable_queues_locked",
        "llam_runtime_offline_one_shard",
    }
)
PAUSE_ACK_KNOWN_CALLS = frozenset(
    set(PAUSE_ACK_MATCHERS)
    | {"llam_shard_pause_ack_matches_locked"}
    | set(PAUSE_ACK_PUBLICATION_HELPERS)
    | set(PAUSE_ACK_RESET_HELPERS)
    | set(PAUSE_ACK_SAMPLING_FUNCTIONS)
    | {"llam_runtime_steal_pause_active", "llam_shard_merge_pause_ack_current_locked"}
)
PAUSE_ACK_REQUEST_FIELDS = frozenset(
    {
        "merge_pause_requested",
        "merge_pause_request_sequence",
        "steal_pause_request_sequence",
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
LEASE_OWNER_NULL_WRITE_HELPERS = frozenset(
    {
        "llam_legacy_lease_init",
        "llam_legacy_lease_finalize_stop_locked",
        "llam_legacy_lease_enter_redirect_locked",
        "llam_legacy_lease_clear_owner_locked",
        "llam_scheduler_unbind_identity",
    }
)
LEASE_OWNER_PROOF_MARKERS = {
    "llam_legacy_lease_grant_helper_locked": frozenset({"transfer_sequence"}),
    "llam_legacy_lease_complete_handback_locked": frozenset(
        {"return_worker", "claimant"}
    ),
    "llam_legacy_lease_reacquire_primary_locked": frozenset({"return_worker"}),
    "llam_legacy_lease_rollback_helper_locked": frozenset({"return_worker"}),
}
CONTROL_FLOW_NAMES = frozenset({"if", "for", "while", "switch", "catch"})
ASSIGNMENT_OPERATORS = frozenset({"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="})


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


@functools.lru_cache(maxsize=256)
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
            body_start, body_close = _statement_bounds(tokens, body_open, begin, end)
            if body_start <= token_index < body_close:
                return True
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


def _active_g_surface_errors(relative_path: str, masked: str) -> list[tuple[int, str]]:
    """Reject hidden or rebound active-G publication in the three helpers.

    The Phase 0 scanner cannot see a macro-expanded member access.  The Phase
    1A helper allowlist is intentionally source-level and fail-closed: an
    allowlisted helper may contain only control flow and direct atomic calls,
    and its worker parameter must not be reassigned before publication.
    """

    if pathlib.PurePath(relative_path).as_posix() != "src/core/sched/scheduler.c":
        return []
    tokens = phase0._tokens(masked)
    functions = _function_spans(masked)
    errors: list[tuple[int, str]] = []

    def fail(line: int) -> None:
        error = (line, "Phase 0 execution authority member: active_g")
        if error not in errors:
            errors.append(error)

    for function in functions:
        if function.name not in ACTIVE_G_HELPERS:
            continue
        begin, end = _token_range_for_span(masked, function)
        body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            None,
        )
        if body_open is None:
            fail(function.start_line)
            continue

        body_calls = [
            call
            for call in _call_sites(tokens)
            if body_open < call.name_index < end
        ]
        # No arbitrary helper/macro call may hide an active-G access.  The
        # actual helpers use only the C atomic builtins and an if guard.
        for call in body_calls:
            if call.name in CONTROL_FLOW_NAMES or call.name in {
                *ATOMIC_LOAD_NAMES,
                *ATOMIC_STORE_NAMES,
                *ATOMIC_INIT_NAMES,
            }:
                continue
            fail(tokens[call.name_index].line)

        parameters = _function_parameter_names(masked, tokens, function)
        worker = parameters[0] if parameters else None
        if worker is None:
            fail(function.start_line)
            continue

        # A parameter rebinding would make a later syntactically direct
        # ``worker->active_g`` refer to an attacker-controlled object.
        for index in range(body_open + 1, min(end, len(tokens))):
            if tokens[index].value not in ASSIGNMENT_OPERATORS or index == 0:
                continue
            if tokens[index - 1].value == worker:
                fail(tokens[index].line)

        allowed = _active_g_allowed_indices(relative_path, masked, tokens)
        direct_active = [
            index
            for index, token in enumerate(tokens)
            if body_open < index < end
            and token.value == "active_g"
            and index in allowed
        ]
        if not direct_active:
            # A helper with no visible direct active-G operation can only be
            # publishing through a macro/alias, which this checker rejects.
            fail(function.start_line)
            continue
        for index in range(body_open + 1, min(end, len(tokens))):
            if tokens[index].value == "active_g" and index not in allowed:
                fail(tokens[index].line)

    return errors


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

    # A tuple that merely agrees with itself is not a pause acknowledgement.
    # Require the live request bit and generation for merge pauses.  The steal
    # channel's request bit is runtime-wide and is exposed by its audited
    # helper, while the shard-local generation remains a direct atomic load.
    if base == "merge":
        requested_calls = _direct_atomic_field_call(
            tokens,
            function_calls,
            "merge_pause_requested",
            receiver=parameters[0],
            names=ATOMIC_LOAD_NAMES,
        )
        request_generation_calls = _direct_atomic_field_call(
            tokens,
            function_calls,
            "merge_pause_request_sequence",
            receiver=parameters[0],
            names=ATOMIC_LOAD_NAMES,
        )
        if not requested_calls or not request_generation_calls:
            return False
        if not _comparison_is_zero(tokens, requested_calls[0], "!="):
            return False
        if _comparison_identifier(tokens, request_generation_calls[0], "==") != parameters[1]:
            return False
    else:
        request_generation_calls = _direct_atomic_field_call(
            tokens,
            function_calls,
            "steal_pause_request_sequence",
            receiver=parameters[0],
            names=ATOMIC_LOAD_NAMES,
        )
        if not request_generation_calls:
            return False
        runtime_pause_calls = [
            call
            for call in calls
            if begin <= call.name_index < end
            and call.name == "llam_runtime_steal_pause_active"
            and len(call.args) == 1
            and _arg_values(tokens, call.args[0])
            == (parameters[0], "->", "runtime")
            and not _inside_constant_false_block(tokens, call.name_index, begin, end)
        ]
        if not runtime_pause_calls:
            return False

    required_expression_calls = [
        *field_calls.values(),
        *request_generation_calls,
        *(requested_calls if base == "merge" else ()),
        *(runtime_pause_calls if base == "steal" else ()),
    ]
    expression_ranges = {
        _statement_bounds(tokens, call.open_index, begin, end)
        for call in required_expression_calls
    }
    if len(expression_ranges) != 1:
        return False
    expression_start, expression_end = next(iter(expression_ranges))
    if not any(
        tokens[index].value == "&&"
        for index in range(expression_start, expression_end)
    ):
        return False

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
    ) and all(
        not _inside_constant_false_block(tokens, call.name_index, begin, end)
        for call in (
            *request_generation_calls,
            *(requested_calls if base == "merge" else ()),
        )
    )


def _publication_tuple_before(
    masked: str,
    tokens: Sequence[phase0._Token],
    calls: Sequence[_CallSite],
    function: _FunctionSpan,
    base: str,
    before: int,
) -> bool:
    required = (
        f"{base}_pause_ack_sequence",
        f"{base}_pause_ack_lease_epoch",
        f"{base}_pause_ack_worker_incarnation",
    )
    function_lines = (function.start_line, function.end_line)
    begin, end = _token_range_for_span(masked, function)
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
            or _inside_constant_false_block(tokens, call.name_index, begin, end)
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
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    member: str,
    calls: Sequence[_CallSite],
    receiver: str | None,
) -> bool:
    """Permit tuple snapshots only when immediately routed to a matcher."""

    if not member.endswith(("_ack_sequence", "_ack_lease_epoch", "_ack_worker_incarnation")):
        return False
    if function.name not in PAUSE_ACK_SAMPLING_FUNCTIONS:
        return False
    base = "merge" if member.startswith("merge_") else "steal"
    matcher_name = f"llam_shard_{base}_pause_ack_matches_locked"
    # Every audited sampling function passes its shard receiver directly to
    # the corresponding matcher.  A matcher name in a dead branch is not a
    # routing proof.
    function_calls = [
        call
        for call in calls
        if function.start_line <= tokens[call.name_index].line <= function.end_line
        and call.name == matcher_name
        and not _inside_constant_false_block(tokens, call.name_index, 0, len(tokens))
        and len(call.args) >= 1
        and _simple_identifier(tokens, call.args[0]) == receiver
    ]
    if not function_calls:
        return False
    return True


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
            allowed = (
                _valid_rt_shards_field_call(tokens, call, member)
                and _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ())
                and function is not None
                and not _inside_constant_false_block(
                    tokens,
                    call.name_index,
                    *_token_range_for_span(masked, function),
                )
            )
        elif path == "src/core/lifecycle/shutdown.c" and function is not None and call is not None:
            allowed = (
                function.name == "llam_runtime_shutdown_unlocked"
                and call.name in ATOMIC_STORE_NAMES
                and _valid_rt_shards_field_call(tokens, call, member)
                and _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ())
                and not _inside_constant_false_block(
                    tokens,
                    call.name_index,
                    *_token_range_for_span(masked, function),
                )
            )
        elif path == "src/engine/watchdog/watchdog_merge.c" and function is not None and call is not None:
            allowed = (
                function.name in PAUSE_ACK_RESET_HELPERS
                and call.name in ATOMIC_STORE_NAMES
                and direct_receiver
                and _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ())
                and not _inside_constant_false_block(
                    tokens,
                    call.name_index,
                    *_token_range_for_span(masked, function),
                )
            )
            if not allowed and _ack_sampling_allowed(
                masked, tokens, function, member, calls, receiver
            ):
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
                                masked,
                                tokens,
                                [candidate for candidate in calls if candidate.name_index < call.name_index],
                                function,
                                "merge" if member.startswith("merge_") else "steal",
                                call.name_index,
                            )
                        )
                        if allowed and _inside_constant_false_block(
                            tokens,
                            call.name_index,
                            *_token_range_for_span(masked, function),
                        ):
                            allowed = False
                    else:
                        allowed = not _inside_constant_false_block(
                            tokens,
                            call.name_index,
                            *_token_range_for_span(masked, function),
                        )
            elif _ack_sampling_allowed(
                masked, tokens, function, member, calls, receiver
            ):
                allowed = call is not None and call.name in ATOMIC_LOAD_NAMES and direct_receiver
        elif function is not None and _ack_sampling_allowed(
            masked, tokens, function, member, calls, receiver
        ):
            allowed = call is not None and call.name in ATOMIC_LOAD_NAMES and direct_receiver

        if not allowed:
            fail(line)

    # A matcher with no direct, live tuple expression is itself a violation;
    # this catches bare/macro-hidden ACK decisions that contain no member token.
    for function in functions:
        if function.name in PAUSE_ACK_MATCHERS and not matcher_valid.get(function.name, False):
            fail(function.start_line)

    # A preprocessor macro or unknown helper can hide an entire ACK state
    # access, leaving no member token for the direct checks above.  Native ACK
    # code has a closed helper set; reject obvious ACK/PAUSE macros and unknown
    # ACK-named calls instead of treating their body as audited.
    for call in calls:
        if call.name in PAUSE_ACK_KNOWN_CALLS or call.name in CONTROL_FLOW_NAMES:
            continue
        function = _function_for_token(masked, tokens, functions, call.name_index)
        if function is None:
            continue
        lowered = call.name.lower()
        ack_segment = bool(re.search(r"(?:^|_)ack(?:_|$)", lowered))
        if (
            (
                call.name.isupper()
                and ("ACK" in call.name or "PAUSE" in call.name)
                and tokens[call.name_index].line == tokens[call.open_index].line
            )
            or (
                ack_segment
                and pathlib.PurePath(relative_path).as_posix().startswith(
                    ("src/core/sched/", "src/engine/watchdog/")
                )
            )
        ):
            fail(tokens[call.name_index].line)
    return errors


def _owner_statement_end(
    tokens: Sequence[phase0._Token], start: int, end: int
) -> int:
    """Return the first semicolon/closing brace after a source statement."""

    index = start
    while index < end and tokens[index].value not in {";", "}"}:
        index += 1
    return index


def _direct_lease_origin(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    variable: str,
    before: int,
) -> bool:
    """Resolve one local lease only from the audited helper call.

    The source checker deliberately does not chase arbitrary pointer aliases.
    It accepts a declaration initializer as well as a split assignment, but the
    most recent assignment to ``variable`` before the owner write must be the
    exact ``llam_shard_legacy_lease_locked(shard)`` call.
    """

    begin, end = _token_range_for_span(masked, function)
    events: list[tuple[int, bool]] = []
    for index in range(max(begin + 1, 1), min(before, end)):
        if tokens[index].value not in ASSIGNMENT_OPERATORS or index == 0:
            continue
        if tokens[index - 1].value != variable:
            continue
        statement_end = _owner_statement_end(tokens, index + 1, min(before, end))
        rhs = [tokens[item].value for item in range(index + 1, statement_end)]
        exact = rhs == ["llam_shard_legacy_lease_locked", "(", "shard", ")"]
        if function.name == "llam_legacy_lease_init":
            exact = exact or rhs == ["&", "shard", "->", "legacy_lease"]
        events.append(
            (
                index,
                exact
                and not _inside_constant_false_block(tokens, index, begin, end),
            )
        )
    if not events:
        return False
    return events[-1][1]


def _owner_proof_before(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    member_index: int,
    value: str,
) -> bool:
    """Require a live, direct transition proof before a non-NULL owner write."""

    if function.name not in LEASE_OWNER_WRITE_HELPERS or value != LEASE_OWNER_VALUE_NAMES.get(function.name):
        return False
    begin, end = _token_range_for_span(masked, function)
    if not _direct_lease_origin(masked, tokens, function, "lease", member_index):
        return False

    def live_before(predicate) -> bool:
        return any(
            predicate(index)
            and not _inside_constant_false_block(tokens, index, begin, end)
            for index in range(begin, member_index)
        )

    # Every direct owner transition has an explicit non-NULL guard.  Requiring
    # the exact member expression prevents a proof marker in an unrelated field
    # from authorizing the write.
    if not live_before(
        lambda index: tokens[index].value == "lease"
        and index + 4 < member_index
        and tokens[index + 1].value == "->"
        and tokens[index + 2].value == "owner"
        and tokens[index + 3].value == "!="
        and tokens[index + 4].value == "NULL"
    ):
        return False
    if not live_before(lambda index: tokens[index].value == "llam_worker_binding_live"):
        return False
    markers = LEASE_OWNER_PROOF_MARKERS.get(function.name, frozenset())
    if not any(
        tokens[index].value in markers
        and not _inside_constant_false_block(tokens, index, begin, end)
        for index in range(begin, member_index)
    ):
        return False
    return True


def _owner_assignment_errors(masked: str) -> list[tuple[int, str]]:
    """Audit direct lease-owner writes and reject aliases/indirection."""

    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    calls = _call_sites(tokens)
    function_spans = _function_spans(masked)
    owner_slot_names: set[str] = set()

    def fail(line: int) -> None:
        error = (line, "Phase 1A ownerless lease grant")
        if error not in errors:
            errors.append(error)

    # First collect every pointer that was formed from an owner address.  Such
    # pointers are intentionally unsupported, including zero/clear stores.
    for operator, member_index, line, receiver, _kind, member in _member_access_tokens(tokens):
        if member != "owner":
            continue
        function = _function_for_token(masked, tokens, function_spans, member_index)
        if function is None:
            continue
        if not (
            receiver in LEASE_OWNER_RECEIVERS
            or (receiver is not None and "lease" in receiver)
            or function.name in LEASE_OWNER_WRITE_HELPERS
            or function.name in LEASE_OWNER_NULL_WRITE_HELPERS
        ):
            continue
        begin, end = _token_range_for_span(masked, function)
        statement_start = operator
        while statement_start > begin and tokens[statement_start - 1].value not in {";", "{", "}"}:
            statement_start -= 1
        if any(tokens[index].value == "&" for index in range(statement_start, operator)):
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

    for operator, member_index, line, receiver, _kind, member in _member_access_tokens(tokens):
        if member != "owner":
            continue
        function = _function_for_token(masked, tokens, function_spans, member_index)
        if function is None:
            continue
        begin, end = _token_range_for_span(masked, function)
        receiver_is_local_lease = receiver == "lease"
        receiver_is_direct_shard_lease = receiver == "legacy_lease"
        transition = function.name in LEASE_OWNER_WRITE_HELPERS
        null_transition = function.name in LEASE_OWNER_NULL_WRITE_HELPERS
        lease_shaped = receiver_is_local_lease or receiver_is_direct_shard_lease or (
            receiver is not None and "lease" in receiver
        )
        if not lease_shaped and not (transition or null_transition):
            continue

        next_value = tokens[member_index + 1].value if member_index + 1 < end else None
        direct_assignment = next_value in ASSIGNMENT_OPERATORS
        unary_mutation = next_value in {"++", "--"} or (
            member_index >= 2 and tokens[member_index - 2].value in {"++", "--"}
        )
        statement_end = _owner_statement_end(tokens, member_index + 1, end)
        statement_start = member_index
        while statement_start > begin and tokens[statement_start - 1].value not in {";", "{", "}"}:
            statement_start -= 1
        unary_mutation = unary_mutation or any(
            tokens[index].value in {"++", "--"}
            for index in range(statement_start, statement_end)
        )

        if direct_assignment:
            operator_value = next_value
            rhs = [tokens[index].value for index in range(member_index + 2, statement_end)]
            exact_value = rhs == [LEASE_OWNER_VALUE_NAMES.get(function.name)] or rhs in (
                ["NULL"],
                ["0"],
                ["0", "U"],
                ["false"],
            )
            if operator_value != "=" or not exact_value:
                fail(line)
                continue
            value = rhs[0] if len(rhs) == 1 else "0U" if rhs == ["0", "U"] else rhs[0]
            if receiver_is_local_lease and transition and value not in {"NULL", "0", "0U", "false"}:
                if not _owner_proof_before(masked, tokens, function, member_index, value):
                    fail(line)
                continue
            if receiver_is_local_lease and null_transition and value in {"NULL", "0", "0U", "false"}:
                if not _direct_lease_origin(masked, tokens, function, "lease", member_index):
                    fail(line)
                continue
            if (
                receiver_is_direct_shard_lease
                and function.name == "llam_scheduler_unbind_identity"
                and value in {"NULL", "0", "0U", "false"}
            ):
                continue
            fail(line)
            continue

        if unary_mutation:
            fail(line)
            continue

        # A member passed to a non-control call is an indirect/macro-hidden
        # access.  No such owner path is part of the audited runtime.
        containing = _call_containing(calls, member_index)
        if (
            containing is not None
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", containing.name)
            and containing.name not in CONTROL_FLOW_NAMES
        ):
            fail(line)

    # A dereference or atomic store through an owner slot is never an audited
    # direct assignment.  Keep this check independent of the member-access
    # walk so ``*slot =`` and ``atomic_store(slot, ...)`` cannot hide writes.
    for index, token in enumerate(tokens):
        if token.value == "=" and index >= 2 and tokens[index - 2].value == "*":
            function = _function_for_token(masked, tokens, function_spans, index)
            declaration = index >= 3 and re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*", tokens[index - 3].value
            )
            if tokens[index - 1].value in owner_slot_names or (
                function is not None
                and function.name in LEASE_OWNER_WRITE_HELPERS
                and not declaration
            ):
                fail(token.line)
    for call in calls:
        if call.name in ATOMIC_STORE_NAMES and call.args:
            alias = _simple_identifier(tokens, call.args[0])
            function = _function_for_token(masked, tokens, function_spans, call.name_index)
            if alias in owner_slot_names or (
                function is not None
                and function.name in LEASE_OWNER_WRITE_HELPERS
                and alias is not None
                and len(call.args) > 1
                and not _is_zero_value(tokens, call.args[1])
            ):
                fail(tokens[call.name_index].line)
    return errors


def _lock_order_errors(
    relative_path: str, masked: str
) -> list[tuple[int, str]]:
    """Audit direct scheduler lock calls and fail closed on opaque wrappers."""

    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    functions = _function_spans(masked)
    lock_names = {
        "pthread_mutex_lock",
        "pthread_mutex_trylock",
        "pthread_mutex_timedlock",
        "pthread_mutex_unlock",
    }
    calls = _call_sites(tokens, lock_names)
    all_calls = _call_sites(tokens)

    def fail(line: int) -> None:
        error = (line, "Phase 1A opaque_lock must precede shard->lock")
        if error not in errors:
            errors.append(error)

    def direct_receiver_for_lock(
        call: _CallSite,
    ) -> tuple[str | None, str | None, bool]:
        """Return (canonical receiver, field, field-looking argument)."""

        if not call.args:
            return None, None, False
        values = list(_arg_values(tokens, call.args[0]))
        if values and values[0] == "&":
            values = values[1:]
        field = next(
            (value for value in ("opaque_lock", "lock") if value in values),
            None,
        )
        if field is None:
            return None, None, False
        if len(values) < 3 or values[-1] != field or values[-2] not in {"->", "."}:
            # Parenthesized/cast/unknown forms are deliberately not inferred.
            return None, field, True
        receiver_values = values[:-2]
        if not receiver_values:
            return None, field, True
        receiver = "".join(receiver_values)
        return receiver, field, True

    def aliases_for(function: _FunctionSpan) -> dict[str, str]:
        begin, end = _token_range_for_span(masked, function)
        aliases: dict[str, str] = {}
        for index in range(begin + 1, end - 1):
            if tokens[index].value != "=" or index == 0:
                continue
            lhs = tokens[index - 1].value
            rhs = tokens[index + 1].value if index + 1 < end else ""
            if lhs in {"first", "second"}:
                # The production merge computes these role variables with a
                # conditional expression.  Their names still define the
                # documented lock-ID order and must not be collapsed to shard.
                continue
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs) and re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*", rhs
            ):
                aliases[lhs] = rhs
        for _ in range(len(aliases) + 1):
            for alias, target in list(aliases.items()):
                aliases[alias] = aliases.get(target, target)
        return aliases

    def role_aliases_for(function: _FunctionSpan) -> dict[str, str]:
        begin, end = _token_range_for_span(masked, function)
        roles = {"first": "first", "second": "second"}
        for index in range(begin + 1, end - 1):
            if tokens[index].value != "=" or index == 0:
                continue
            lhs = tokens[index - 1].value
            rhs = tokens[index + 1].value
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs) and rhs in roles:
                roles[lhs] = roles[rhs]
        return roles

    for function in functions:
        begin, end = _token_range_for_span(masked, function)
        aliases = aliases_for(function)
        roles = role_aliases_for(function)
        held: dict[str, dict[str, bool]] = {}
        held_opaque_roles: set[str] = set()
        held_lock_roles: set[str] = set()
        function_calls = sorted(
            (call for call in calls if begin <= call.name_index < end),
            key=lambda call: call.name_index,
        )
        for call in function_calls:
            receiver, which, field_mentioned = direct_receiver_for_lock(call)
            if receiver is None or which is None:
                if field_mentioned:
                    fail(tokens[call.name_index].line)
                continue
            symbol = aliases.get(receiver, receiver)
            state = held.setdefault(symbol, {"opaque_lock": False, "lock": False})
            role = roles.get(receiver, roles.get(symbol))
            if call.name == "pthread_mutex_unlock":
                state[which] = False
                if which == "opaque_lock" and role is not None:
                    held_opaque_roles.discard(role)
                if which == "lock" and role is not None:
                    held_lock_roles.discard(role)
                continue
            if which == "opaque_lock" and state["lock"]:
                fail(tokens[call.name_index].line)
            if which == "opaque_lock" and role in {"first", "second"}:
                if role == "first" and "second" in held_opaque_roles:
                    fail(tokens[call.name_index].line)
                if held_lock_roles:
                    # A two-shard merge must acquire both opaque locks before
                    # either shard lock, not just before its matching shard.
                    fail(tokens[call.name_index].line)
            state[which] = True
            if which == "opaque_lock" and role in {"first", "second"}:
                held_opaque_roles.add(role)
            if which == "lock" and role in {"first", "second"}:
                held_lock_roles.add(role)

        # Obvious lock wrappers/macros cannot be proven by lexical receiver
        # tracking.  Reject them in scheduler/watchdog source rather than
        # silently accepting a hidden acquisition.
        if pathlib.PurePath(relative_path).as_posix().startswith(
            ("src/core/sched/", "src/engine/watchdog/")
        ):
            for call in all_calls:
                if not begin <= call.name_index < end or call.name in lock_names:
                    continue
                if call.name in CONTROL_FLOW_NAMES:
                    continue
                lowered = call.name.lower()
                if (
                    lowered.startswith(("lock_shard", "unlock_shard", "trylock_shard"))
                    or (
                        lowered.startswith(("acquire_", "release_"))
                        and ("lock" in lowered or "opaque" in lowered)
                    )
                    or (
                        call.name.isupper()
                        and ("LOCK" in call.name or "OPAQUE" in call.name)
                        and tokens[call.name_index].line == tokens[call.open_index].line
                    )
                ):
                    fail(tokens[call.name_index].line)
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

    def enclosing_if(token_index: int, begin: int, end: int):
        """Return the nearest if-condition/body containing one token."""

        candidates = []
        for index in range(begin, min(token_index, end)):
            if tokens[index].value != "if" or index + 1 >= end:
                continue
            if tokens[index + 1].value != "(":
                continue
            condition_end = _matching_token(tokens, index + 1, "(", ")")
            if condition_end is None or not index + 1 < token_index < condition_end:
                continue
            body_open = condition_end + 1
            if body_open >= end:
                continue
            if tokens[body_open].value == "{":
                body_close = _matching_token(tokens, body_open, "{", "}")
                if body_close is None:
                    continue
            else:
                _body_start, body_close = _statement_bounds(tokens, body_open, begin, end)
            candidates.append((index, condition_end, body_open, body_close))
        return max(candidates, key=lambda item: item[0], default=None)

    def proof_is_negative(proof: _CallSite) -> bool:
        before = proof.name_index - 1
        if before >= 0 and tokens[before].value == "!":
            return True
        after = proof.close_index + 2
        return (
            proof.close_index + 1 < len(tokens)
            and tokens[proof.close_index + 1].value in {"==", "!="}
            and after < len(tokens)
            and _scalar_value(tokens, (after,)) in {"0", "0U", "false"}
        )

    def proof_dominates(binding: _CallSite, equality: _CallSite, kill: _CallSite) -> bool:
        binding_if = enclosing_if(binding.name_index, begin, end)
        equality_if = enclosing_if(equality.name_index, begin, end)
        if binding_if is None or equality_if is None or binding_if != equality_if:
            return False
        _if_index, _condition_end, body_open, body_close = binding_if
        if body_open < kill.name_index < body_close:
            return True
        # A guard whose body returns on failure dominates later code only when
        # each proof is expressed as a failure predicate (the production
        # ``!binding`` and ``pthread_equal(...) == 0`` form).
        return (
            kill.name_index > body_close
            and any(
                tokens[index].value == "return"
                for index in range(body_open + 1, min(body_close, end))
            )
            and proof_is_negative(binding)
            and proof_is_negative(equality)
        )

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

        # The revalidation equality must compare the latest target snapshot to
        # the original handle captured by an earlier snapshot in this same
        # function.  Any arbitrary second operand is a non-proof.
        original_target = None
        for prior in reversed(snapshot_calls):
            if prior.name_index >= snapshot_index:
                continue
            prior_target = _addressed_identifier(tokens, prior.args[4])
            if (
                prior_target is not None
                and prior_target != native_target
                and not _inside_constant_false_block(tokens, prior.name_index, begin, end)
            ):
                original_target = prior_target
                break
        if original_target is None:
            fail(tokens[call.name_index].line)
            continue

        # A snapshot output is immutable until the signal is sent.  Direct
        # rebinding (including compound/unary updates) invalidates the proof;
        # unknown macro writes are rejected by the absence of a direct proof
        # shape below.
        snapshot_outputs = {owner, incarnation, native_target}
        rebound = False
        for index in range(snapshot.close_index + 1, call.name_index):
            if tokens[index].value not in snapshot_outputs:
                continue
            if (
                index + 1 < len(tokens)
                and tokens[index + 1].value in ASSIGNMENT_OPERATORS
            ) or (
                index > 0 and tokens[index - 1].value in {"++", "--"}
            ) or (
                index + 1 < len(tokens) and tokens[index + 1].value in {"++", "--"}
            ):
                rebound = True
                break
        if rebound:
            fail(tokens[call.name_index].line)
            continue

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

        binding_candidates = [
            candidate
            for candidate in calls
            if candidate.name == "llam_worker_binding_live"
            and snapshot_index < candidate.name_index < call.name_index
            and len(candidate.args) >= 2
            and _simple_identifier(tokens, candidate.args[0]) == owner
            and _simple_identifier(tokens, candidate.args[1]) == incarnation
            and not _inside_constant_false_block(tokens, candidate.name_index, begin, end)
        ]
        equality_candidates = [
            candidate
            for candidate in calls
            if candidate.name == "pthread_equal"
            and snapshot_index < candidate.name_index < call.name_index
            and len(candidate.args) >= 2
            and {
                _simple_identifier(tokens, candidate.args[0]),
                _simple_identifier(tokens, candidate.args[1]),
            }
            == {native_target, original_target}
            and not _inside_constant_false_block(tokens, candidate.name_index, begin, end)
        ]
        binding_ok = equality_ok = False
        for binding in binding_candidates:
            for equality in equality_candidates:
                if proof_dominates(binding, equality, call):
                    binding_ok = equality_ok = True
                    break
            if binding_ok:
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
        invalidated_by_unlock = False
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
                # A cleanup unlock in the failing revalidation branch is safe
                # because it returns before the signal.  Any unlock whose path
                # can reach pthread_kill invalidates the lifetime proof, even
                # when the same lock is reacquired later.
                enclosing_blocks = []
                for opening in range(function_body_open, mutex.name_index):
                    if tokens[opening].value != "{":
                        continue
                    closing = _matching_token(tokens, opening, "{", "}")
                    if closing is not None and mutex.name_index < closing:
                        enclosing_blocks.append(closing)
                block_end = min(enclosing_blocks, default=call.name_index)
                if not any(
                    tokens[index].value == "return"
                    for index in range(mutex.close_index + 1, min(block_end, call.name_index))
                ):
                    invalidated_by_unlock = True
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
        if invalidated_by_unlock or not (
            snapshot_had_opaque and snapshot_had_lock and held_opaque and held_lock
        ):
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
    extra.extend(_active_g_surface_errors(relative_path, masked))
    extra.extend(_extra_authority_errors(masked))
    extra.extend(_processor_struct_errors(relative_path, masked))
    extra.extend(_shard_struct_errors(masked))
    extra.extend(_ack_field_errors(relative_path, masked))
    extra.extend(_owner_assignment_errors(masked))
    extra.extend(_lock_order_errors(relative_path, masked))
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
