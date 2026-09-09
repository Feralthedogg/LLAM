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
PAUSE_ACK_SAMPLING_ALLOWED_CALLS = frozenset(
    {
        # Direct matcher/tuple operations.
        *ATOMIC_LOAD_NAMES,
        *ATOMIC_STORE_NAMES,
        "atomic_fetch_sub_explicit",
        "llam_shard_merge_pause_ack_matches_locked",
        "llam_shard_steal_pause_ack_matches_locked",
        # Wait-loop and timing helpers.
        "llam_now_ns",
        "llam_watchdog_pause_briefly",
        "llam_runtime_steal_pause_active",
        "llam_shard_is_online",
        # Merge/offline sampling helpers.
        "llam_lockfree_normq_enabled",
        "llam_merge_task_to_target_locked",
        "llam_merge_shard_timers_locked",
        "llam_merge_lockfree_normq_locked",
        "llam_merge_runnable_queues_locked",
        "llam_norm_queue_depth",
        "llam_norm_queue_pop_owner_locked",
        "llam_norm_queue_push_owner_locked",
        "llam_norm_queue_note_dequeue",
        "llam_queue_can_merge_runnable",
        "llam_queue_pop_head",
        "llam_shard_merge_pause_requested",
        "llam_task_can_merge_runnable",
        "llam_runtime_active_io_waiters",
        "llam_runtime_live_tasks",
        "llam_runtime_online_shards",
        "llam_runtime_note_online_shards",
        "llam_runtime_pick_merge_target",
        "llam_runtime_set_steal_pause",
        "llam_runtime_wait_steal_pause_ack",
        "llam_shard_accepts_new_work",
        "llam_shard_can_offline_locked",
        "llam_shard_can_start_merge_locked",
        "llam_shard_inflight_io_waiters",
        "llam_shard_load_lease_current",
        "llam_shard_release_merge_pause",
        "llam_shard_request_merge_pause",
        "llam_shard_wait_merge_pause_ack",
        "llam_rehome_inflight_io_waiters",
        "llam_rehome_node_submit_waiters",
        "llam_rehome_parked_waiters",
        "llam_rehome_runtime_watch_waiters",
        "llam_evacuate_rehomed_submit_waiters",
        "llam_quiesce_cross_io_watch_state",
        "llam_runtime_nudge_marked_watch_migrations",
        "llam_dynamic_trace_enabled",
        "llam_kick_node",
        "llam_kick_shard",
        "fprintf",
        "pthread_mutex_lock",
        "pthread_mutex_unlock",
    }
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
LEASE_OWNER_VALUE_NAMES = {
    "llam_legacy_lease_grant_helper_locked": "helper",
    "llam_legacy_lease_complete_handback_locked": "claimant",
    "llam_legacy_lease_reacquire_primary_locked": "worker",
    "llam_legacy_lease_rollback_helper_locked": "primary",
}
LEASE_OWNER_INCARNATION_VALUE_NAMES = {
    "llam_legacy_lease_grant_helper_locked": "incarnation",
    "llam_legacy_lease_complete_handback_locked": "claimant_incarnation",
    "llam_legacy_lease_reacquire_primary_locked": "incarnation",
    "llam_legacy_lease_rollback_helper_locked": "incarnation",
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
LEASE_OWNER_ALLOWED_CALLS = frozenset(
    {
        "llam_shard_legacy_lease_locked",
        "llam_legacy_lease_runtime_stopping",
        "llam_legacy_lease_merge_paused",
        "llam_legacy_lease_matches_worker",
        "llam_worker_binding_live",
        "llam_legacy_lease_next_counter",
        "llam_legacy_lease_assert_consistent_locked",
        "atomic_load",
        "atomic_load_explicit",
        "atomic_store",
        "atomic_store_explicit",
    }
)
LEASE_OWNER_NULL_ALLOWED_CALLS = frozenset(
    {
        *LEASE_OWNER_ALLOWED_CALLS,
        "llam_worker_clear_active_g",
        "memset",
        "pthread_equal",
        "pthread_mutex_lock",
        "pthread_mutex_unlock",
        "pthread_self",
        "sizeof",
    }
)
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


def _exact_atomic_field_argument(
    tokens: Sequence[phase0._Token],
    calls: Sequence[_CallSite],
    argument: Sequence[int],
    receiver: str,
    field: str,
) -> _CallSite | None:
    """Return one unmodified atomic field load occupying ``argument``.

    ACK sampling is intentionally an allowlist rather than a C expression
    evaluator.  A field load hidden inside arithmetic, a cast, a conditional,
    or another call is not a value-derived tuple argument.  Requiring the
    call's token span to cover the complete argument keeps those forms
    fail-closed while preserving the inline loads used by the runtime.
    """

    if not argument:
        return None
    first = argument[0]
    last = argument[-1]
    expected = ("&", receiver, "->", field)
    for call in calls:
        if call.name not in ATOMIC_LOAD_NAMES:
            continue
        if call.name_index != first or call.close_index != last:
            continue
        if call.open_index != first + 1 or not call.args:
            continue
        if _arg_values(tokens, call.args[0]) != expected:
            continue
        if call.name == "atomic_load" and len(call.args) != 1:
            continue
        if call.name == "atomic_load_explicit" and (
            len(call.args) != 2
            or _arg_values(tokens, call.args[1]) != ("memory_order_acquire",)
        ):
            continue
        return call
    return None


def _canonical_atomic_load(
    tokens: Sequence[phase0._Token], call: _CallSite
) -> bool:
    """Return whether one atomic load has the audited argument shape."""

    if call.name == "atomic_load":
        return len(call.args) == 1
    return call.name == "atomic_load_explicit" and len(call.args) == 2 and _arg_values(
        tokens, call.args[1]
    ) == ("memory_order_acquire",)


def _addressed_identifier(
    tokens: Sequence[phase0._Token], argument: Sequence[int]
) -> str | None:
    values = _arg_values(tokens, argument)
    if len(values) == 2 and values[0] == "&" and re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*", values[1]
    ):
        return values[1]
    return None


def _addressed_identifier_at(
    tokens: Sequence[phase0._Token], index: int, end: int
) -> tuple[str, int] | None:
    """Recognize ``&identifier`` with balanced redundant parentheses.

    This helper is used only for rejecting address aliases.  Canonical call
    argument validators continue to require the unparenthesized two-token
    spelling through ``_addressed_identifier``.
    """

    if not 0 <= index < end or tokens[index].value != "&":
        return None
    cursor = index + 1
    opening = 0
    while cursor < end and tokens[cursor].value == "(":
        opening += 1
        cursor += 1
    if cursor >= end or not re.fullmatch(
        r"[A-Za-z_][A-Za-z0-9_]*", tokens[cursor].value
    ):
        return None
    name = tokens[cursor].value
    cursor += 1
    closing = 0
    while cursor < end and closing < opening and tokens[cursor].value == ")":
        closing += 1
        cursor += 1
    if closing != opening:
        return None
    return name, cursor


def _identifier_has_expression_write_or_move(
    tokens: Sequence[phase0._Token],
    name: str,
    start: int,
    end: int,
    *,
    pointer: bool = False,
    include_assignment: bool = True,
    include_address: bool = True,
) -> bool:
    """Reject an identifier used as a mutable expression in one interval.

    The Phase 1A audits intentionally accept only direct identifier/member
    spellings.  Looking at the token immediately adjacent to an identifier is
    insufficient for ordinary C expressions such as ``(shard) = other`` or
    ``((worker))++``: the assignment/unary operator is separated by one or
    more closing parentheses (and a cast can add another pair).  This helper
    keeps the audit lexical and conservative.  It treats an assignment
    operator later in the same statement, an address escape, or a movement
    operator after redundant parentheses as a mutation.  A direct member
    receiver (``worker->field``) remains a read of the pointer itself and is
    therefore excluded.

    ``pointer`` additionally rejects parenthesized pointer arithmetic.  Scalar
    tuple values are checked for assignment/unary movement but their normal
    arithmetic in surrounding transition expressions is not rejected here.
    """

    if not name or start >= end:
        return False
    for index in range(max(0, start), min(end, len(tokens))):
        if tokens[index].value != name:
            continue
        following = tokens[index + 1].value if index + 1 < end else None
        previous = tokens[index - 1].value if index > start else None

        # ``name->member``/``name.member`` is a direct projection, not a
        # mutation of the receiver.  Addressed member projections are also
        # canonical (``&name->field``), while bare ``&name`` is not.
        if following in {"->", "."} or previous in {"->", "."}:
            continue
        # An identifier between an indexing pair is the selector of an
        # object, not the object/index variable being assigned.  For example,
        # ``rt->shards[i].metrics += 1`` must not be interpreted as a write to
        # ``i`` merely because the assignment appears later in the statement.
        # A grouped movement ``(i)++`` or grouped assignment ``(i) = j`` does
        # not have this immediate ``[name]`` shape and remains rejected.
        if previous == "[" and following == "]":
            continue

        if include_address:
            addressed = _addressed_identifier_at(tokens, index - 1, end) if index > 0 else None
            if addressed is not None and addressed[0] == name:
                addressed_end = addressed[1]
                if addressed_end >= end or tokens[addressed_end].value not in {"->", "."}:
                    return True

        statement_start, statement_end = _statement_bounds(tokens, index, start, end)
        # An assignment later in this statement is an expression lvalue even
        # when parentheses/casts/comma operators separate it from the name.
        # Assignments before the name (for example ``out = name``) are not.
        if include_assignment and any(
            tokens[item].value in ASSIGNMENT_OPERATORS
            for item in range(index + 1, statement_end)
        ):
            # A loop-local pointer declaration (``type *name = origin``)
            # commonly appears after an earlier lock in the same function.
            # It establishes a fresh lexical object, whereas a grouped or
            # cast lvalue has a closing delimiter before its assignment.  The
            # lock audit already treats this declaration as an allowed source
            # origin; preserve that exception here so later receiver uses are
            # still checked.
            if (
                following == "="
                and previous == "*"
                and any(
                    value.endswith("_t")
                    or value in {
                        "const",
                        "volatile",
                        "restrict",
                        "struct",
                        "union",
                        "unsigned",
                        "signed",
                        "void",
                    }
                    for value in (
                        tokens[item].value for item in range(statement_start, index - 1)
                    )
                )
            ):
                continue
            return True

        # Prefix/postfix movement may be hidden behind redundant grouping or
        # a cast.  Skip only delimiters that can close such a grouping; a
        # member projection was handled above and is not skipped.
        cursor = index + 1
        while cursor < statement_end and tokens[cursor].value == ")":
            cursor += 1
        if cursor < statement_end and tokens[cursor].value in {"++", "--"}:
            return True
        cursor = index - 1
        while cursor >= statement_start and tokens[cursor].value == "(":
            cursor -= 1
        if cursor >= statement_start and tokens[cursor].value in {"++", "--"}:
            return True

        if pointer:
            # ``(name) + offset`` and the equivalent cast/grouped forms move
            # a pointer receiver to an unproven object.  Keep this scoped to
            # the same statement so scalar comparisons elsewhere cannot make
            # a direct member use fail spuriously.
            cursor = index + 1
            while cursor < statement_end and tokens[cursor].value in {")", "]"
            }:
                cursor += 1
            if cursor < statement_end and tokens[cursor].value in {"+", "-"}:
                return True
            cursor = index - 1
            while cursor >= statement_start and tokens[cursor].value in {"(", "["}:
                cursor -= 1
            if cursor >= statement_start and tokens[cursor].value in {"+", "-"}:
                return True
    return False


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


def _control_flow_records(
    tokens: Sequence[phase0._Token], begin: int, end: int
) -> list[tuple[int, int, int, int, int, bool]]:
    """Return visible ``if``/loop bodies and conditions in one function.

    The boundary check is intentionally lexical, but it must not confuse an
    unbraced one-line statement with an unconditional statement.  Keeping the
    body range for both braced and unbraced controls gives the callers a small,
    conservative CFG approximation: a value captured in any returned body is
    conditional, and a return in a nested body is not an unconditional guard
    exit.  The tuple is ``(keyword, condition_start, condition_end,
    body_start, body_end, braced)`` where ``keyword`` is the token index.
    """

    records: list[tuple[int, int, int, int, int, bool]] = []
    for index in range(begin, end):
        if tokens[index].value not in {"if", "for", "while", "switch"}:
            continue
        if index + 1 >= end or tokens[index + 1].value != "(":
            continue
        condition_end = _matching_token(tokens, index + 1, "(", ")")
        if condition_end is None or condition_end + 1 >= end:
            continue
        body_start = condition_end + 1
        if tokens[body_start].value == "{":
            body_end = _matching_token(tokens, body_start, "{", "}")
            if body_end is None:
                continue
            records.append((index, index + 2, condition_end, body_start, body_end, True))
            continue
        body_start, body_end = _statement_bounds(tokens, body_start, begin, end)
        if body_start >= body_end:
            continue
        records.append((index, index + 2, condition_end, body_start, body_end, False))
    return records


def _control_contains(
    record: tuple[int, int, int, int, int, bool], token_index: int
) -> bool:
    """Return whether a token is in a control condition or body."""

    _keyword, condition_start, condition_end, body_start, body_end, _braced = record
    return condition_start <= token_index < condition_end or body_start <= token_index < body_end


def _token_in_any_control_body(
    records: Sequence[tuple[int, int, int, int, int, bool]], token_index: int
) -> bool:
    return any(record[3] <= token_index < record[4] for record in records)


def _has_unconditional_return(
    tokens: Sequence[phase0._Token],
    records: Sequence[tuple[int, int, int, int, int, bool]],
    body_start: int,
    body_end: int,
) -> bool:
    """Return whether a guard body has a direct return on every path shape.

    This deliberately accepts only a direct return in the guard body.  A bare
    ``if (flag) return`` is represented as a nested unbraced record and is not
    counted, preventing the later signal from falling through when ``flag``
    is false.  Any ``else`` or jump in the body is likewise unsupported by the
    lexical allowlist and therefore cannot manufacture a proof.
    """

    if body_start >= body_end:
        return False
    if tokens[body_start].value == "return":
        return True
    for index in range(body_start + 1, body_end):
        if tokens[index].value != "return":
            continue
        nested = any(
            body_start < record[3] <= index < record[4] <= body_end
            for record in records
        )
        if nested:
            continue
        if any(tokens[item].value in {"goto", "else"} for item in range(body_start + 1, body_end)):
            return False
        return True
    return False


def _has_unconditional_false_return(
    tokens: Sequence[phase0._Token],
    records: Sequence[tuple[int, int, int, int, int, bool]],
    body_start: int,
    body_end: int,
) -> bool:
    """Return whether a control body directly returns the failure value."""

    if body_start >= body_end:
        return False
    candidates: list[int] = []
    if tokens[body_start].value == "return":
        candidates.append(body_start)
    else:
        for index in range(body_start + 1, body_end):
            if tokens[index].value != "return":
                continue
            if any(
                body_start < record[3] <= index < record[4] <= body_end
                for record in records
            ):
                continue
            candidates.append(index)
    for index in candidates:
        finish = index + 1
        while finish < body_end and tokens[finish].value not in {";", "}"}:
            finish += 1
        values = tuple(tokens[item].value for item in range(index, finish))
        if values in {
            ("return", "false"),
            ("return", "0"),
            ("return", "0", "U"),
        }:
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
        for index in range(body_open + 1, end - 4):
            if (
                tokens[index].value == "("
                and tokens[index + 1].value == "*"
                and re.fullmatch(
                    r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 2].value
                )
                and tokens[index + 3].value == ")"
                and tokens[index + 4].value == "("
            ):
                fail(tokens[index + 2].line)

        parameters = _function_parameter_names(masked, tokens, function)
        worker = parameters[0] if parameters else None
        if worker is None:
            fail(function.start_line)
            continue

        if _parameter_has_escape_or_rebind(tokens, parameters, body_open, end):
            # The helper's direct parameters are the only worker/task
            # provenance accepted by this operation-closed surface.  A slot
            # alias or parameter assignment cannot be reconciled with a
            # later-looking ``worker->active_g`` access.
            fail(function.start_line)

        # The helper must retain the exact worker object supplied by its
        # caller.  Taking the parameter's address and writing through a slot
        # is an alias/rebinding escape even when the eventual atomic spelling
        # still says ``worker->active_g``.
        for index in range(body_open + 1, min(end, len(tokens) - 1)):
            if tokens[index].value != "&":
                continue
            cursor = index + 1
            opening_count = 0
            while cursor < end and tokens[cursor].value == "(":
                opening_count += 1
                cursor += 1
            if cursor >= end or tokens[cursor].value != worker:
                continue
            cursor += 1
            closing_count = 0
            while cursor < end and tokens[cursor].value == ")" and closing_count < opening_count:
                closing_count += 1
                cursor += 1
            # Parenthesized worker addresses are aliases just like the direct
            # ``&worker`` spelling.  Only a member-address expression is
            # allowed in the helper's atomic operation.
            if (
                closing_count != opening_count
                or cursor >= end
                or tokens[cursor].value not in {"->", "."}
            ):
                fail(tokens[index].line)
                break
        for index in range(body_open + 1, min(end, len(tokens) - 1)):
            if tokens[index].value != "*" or not re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 1].value
            ):
                continue
            operator_index = index + 2
            if operator_index < end and tokens[operator_index].value == ")":
                operator_index += 1
            if operator_index < end and tokens[operator_index].value in {
                "++",
                "--",
                "=",
                "+=",
                "-=",
                "*=",
                "/=",
                "%=",
                "&=",
                "|=",
                "^=",
            }:
                fail(tokens[operator_index].line)
        if any(
            tokens[index].value == "worker"
            and index + 1 < end
            and tokens[index + 1].value in {"+", "-"}
            or tokens[index].value == "worker"
            and index > body_open + 1
            and tokens[index - 1].value in {"+", "-"}
            for index in range(body_open + 1, end)
        ):
            fail(function.start_line)

        # A parameter rebinding would make a later syntactically direct
        # ``worker->active_g`` refer to an attacker-controlled object.
        for index in range(body_open + 1, min(end, len(tokens))):
            if tokens[index].value not in ASSIGNMENT_OPERATORS or index == 0:
                if tokens[index].value not in {"++", "--"}:
                    continue
                if (
                    (index > body_open + 1 and tokens[index - 1].value == worker)
                    or (index + 1 < end and tokens[index + 1].value == worker)
                ):
                    fail(tokens[index].line)
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

        # Each active-G helper is a distinct one-operation API.  Accepting a
        # direct atomic spelling is insufficient: an extra store/load, wrong
        # order, wrong value, or relaxed order can publish state that callers
        # did not audit.  Keep the null guard used by the native helpers, but
        # require the exact atomic operation sequence below.
        def call_values(call: _CallSite, argument: int) -> tuple[str, ...]:
            return (
                _arg_values(tokens, call.args[argument])
                if argument < len(call.args)
                else ()
            )

        # The only control-flow operation in the native helpers is the
        # fail-fast NULL guard.  Conditional publication/load/clear would make
        # cardinality and ordering path-dependent, so reject every other
        # visible control form.
        control_records = _control_flow_records(tokens, body_open, end)
        for record in control_records:
            _keyword, condition_start, condition_end, body_start, body_end, _braced = record
            if tuple(tokens[index].value for index in range(condition_start, condition_end)) != (
                worker,
                "==",
                "NULL",
            ):
                fail(tokens[_keyword].line)
                continue
            body_values = tuple(tokens[index].value for index in range(body_start, body_end))
            allowed_guard_bodies = (
                {
                    ("return", "NULL", ";"),
                    ("{", "return", "NULL", ";"),
                }
                if function.name == "llam_worker_load_active_g"
                else {
                    ("return", ";"),
                    ("{", "return", ";"),
                }
            )
            if body_values not in allowed_guard_bodies:
                fail(tokens[_keyword].line)

        atomic_body_calls = [
            call
            for call in body_calls
            if call.name in ATOMIC_LOAD_NAMES | ATOMIC_STORE_NAMES | ATOMIC_INIT_NAMES
        ]
        for call in atomic_body_calls:
            if call.name == "atomic_init":
                fail(tokens[call.name_index].line)

        expected_active_target = ("&", worker, "->", "active_g")
        if function.name == "llam_worker_publish_active_g":
            valid = len(atomic_body_calls) == 1
            if valid:
                call = atomic_body_calls[0]
                valid = (
                    call.name == "atomic_store_explicit"
                    and len(call.args) == 3
                    and call_values(call, 0) == expected_active_target
                    and call_values(call, 1) == (parameters[1],)
                    and call_values(call, 2) == ("memory_order_release",)
                )
            if not valid:
                fail(function.start_line)
        elif function.name == "llam_worker_load_active_g":
            valid = len(atomic_body_calls) == 1
            load_call: _CallSite | None = atomic_body_calls[0] if valid else None
            if valid and load_call is not None:
                valid = (
                    load_call.name == "atomic_load_explicit"
                    and len(load_call.args) == 2
                    and call_values(load_call, 0) == expected_active_target
                    and call_values(load_call, 1) == ("memory_order_acquire",)
                )
            # The load operation is returned directly.  A temporary/boolean
            # route could combine the publication with an attacker value while
            # retaining the member token.
            if valid and load_call is not None:
                return_statements = [
                    _statement_bounds(tokens, index, body_open + 1, end)
                    for index in range(body_open + 1, end)
                    if tokens[index].value == "return"
                ]
                call_values_in_return = tuple(
                    tokens[index].value
                    for index in range(load_call.name_index, load_call.close_index + 1)
                )
                valid = any(
                    tuple(
                        tokens[index].value
                        for index in range(start + 1, finish - 1)
                    )
                    == call_values_in_return
                    for start, finish in return_statements
                )
            if not valid:
                fail(function.start_line)
        else:  # llam_worker_clear_active_g
            valid = len(atomic_body_calls) == 2
            if valid:
                active_call, timestamp_call = atomic_body_calls
                valid = (
                    active_call.name == "atomic_store_explicit"
                    and len(active_call.args) == 3
                    and call_values(active_call, 0) == expected_active_target
                    and _is_zero_value(tokens, active_call.args[1])
                    and call_values(active_call, 2) == ("memory_order_release",)
                    and timestamp_call.name == "atomic_store_explicit"
                    and len(timestamp_call.args) == 3
                    and call_values(timestamp_call, 0)
                    == ("&", worker, "->", "last_run_started_ns")
                    and _is_zero_value(tokens, timestamp_call.args[1])
                    and call_values(timestamp_call, 2) == ("memory_order_release",)
                )
            if not valid:
                fail(function.start_line)

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


def _parameter_has_escape_or_rebind(
    tokens: Sequence[phase0._Token],
    parameters: Sequence[str],
    body_open: int,
    finish: int,
    pointer_parameters: Sequence[str] | None = None,
) -> bool:
    """Reject writes, movement, or direct address escapes of parameters.

    The Phase 1A source contract uses the names supplied at a boundary as
    stable provenance.  A lexical checker cannot follow a pointer or prove
    that a later assignment is harmless, so the operation-closed policy is to
    reject every parameter rebind and every bare ``&parameter``.  Taking the
    address of a member (``&shard->field``) remains a direct operation and is
    therefore allowed.
    """

    names = set(parameters)
    if not names:
        return True

    # A parameter may not be used as a bare address.  ``_addressed_identifier_at``
    # also recognizes redundant parentheses, which must not create an alias
    # that the direct atomic/member checks cannot see.
    for index in range(body_open + 1, finish):
        addressed = _addressed_identifier_at(tokens, index, finish)
        if addressed is None or addressed[0] not in names:
            continue
        if addressed[1] < finish and tokens[addressed[1]].value in {"->", "."}:
            continue
        return True

    # Inspect every assignment operator's left side.  Looking only at the
    # immediately preceding token misses ``(worker) =`` and ``*slot =``;
    # scanning the statement prefix catches both while never treating a RHS
    # claimant/value as a parameter rebind.
    for index in range(body_open + 1, finish):
        if tokens[index].value not in ASSIGNMENT_OPERATORS:
            continue
        statement_start, _statement_end = _statement_bounds(
            tokens, index, body_open + 1, finish
        )
        lhs = [tokens[item].value for item in range(statement_start, index)]
        if not lhs:
            continue
        for position, value in enumerate(lhs):
            if value not in names:
                continue
            # A name appearing after ``->``/``.`` is a member, not the
            # parameter object itself (e.g. helper->role).
            if (
                (position > 0 and lhs[position - 1] in {"->", "."})
                or (position + 1 < len(lhs) and lhs[position + 1] in {"->", "."})
            ):
                continue
            # A parameter on the left is always unsupported, including a
            # parenthesized/cast/comma lvalue.  There is no need to recover the
            # exact C lvalue because accepting any such form is unsafe.
            return True

    # Unary movement can alter a receiver without an assignment token.  Binary
    # arithmetic on scalar sequence/epoch parameters is part of the existing
    # rollback transition (``sequence - 1U``), so it is intentionally not
    # treated as a rebind.  Pointer parameters use an explicit/inferred set and
    # cannot participate in even a standalone arithmetic expression.
    scalar_names = {
        "request",
        "request_sequence",
        "sequence",
        "epoch",
        "epoch_value",
        "incarnation",
        "worker_incarnation",
        "deadline",
        "deadline_ns",
        "now_ns",
        "value",
        "count",
        "index",
        "i",
        "j",
    }
    pointer_names = set(pointer_parameters or ()) or {
        name for name in names if name not in scalar_names
    }
    # The older adjacent-token checks below are retained for simple source
    # forms, but every parameter also needs an expression-aware walk.  In
    # particular ``(worker)++`` and ``((task)) = other`` put the mutation
    # operator past a closing parenthesis and otherwise look like reads.
    for name in names:
        if _identifier_has_expression_write_or_move(
            tokens,
            name,
            body_open + 1,
            finish,
            pointer=name in pointer_names,
        ):
            return True
    for index in range(body_open + 1, finish):
        if tokens[index].value not in {"++", "--", "+", "-"}:
            continue
        if index > body_open + 1 and tokens[index - 1].value in pointer_names:
            return True
        if index + 1 < finish and tokens[index + 1].value in pointer_names:
            return True
    # Indexed parameter expressions are not direct identity provenance.  They
    # are rejected even when no assignment follows (e.g. ``worker[0]``).
    for index in range(body_open + 1, finish):
        if tokens[index].value not in names:
            continue
        if (index + 1 < finish and tokens[index + 1].value == "[") or (
            index > body_open + 1 and tokens[index - 1].value == "]"
        ):
            return True
    return False


def _receiver_is_stable(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    receiver: str | None,
    before: int,
) -> bool:
    """Require a sampler receiver to retain one direct object provenance."""

    if receiver is None or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver):
        return False
    begin, end = _token_range_for_span(masked, function)
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return False
    parameters = _function_parameter_names(masked, tokens, function)
    if receiver in parameters:
        # The direct function receiver (including ``rt`` in the runtime-wide
        # wait) must not move or escape anywhere in the audited body.
        return not _parameter_has_escape_or_rebind(
            tokens, (receiver,), body_open, min(before + 1, end)
        )

    # Runtime-wide steal sampling creates one local shard from the direct
    # runtime/index source.  Allow only that declaration/assignment shape;
    # aliases such as ``shard = attacker`` or a pointer slot are unsupported.
    origin: int | None = None
    runtime = parameters[0] if parameters else None
    for index in range(body_open + 1, min(before, end)):
        if tokens[index].value != receiver or index + 1 >= end:
            continue
        if tokens[index + 1].value not in ASSIGNMENT_OPERATORS:
            continue
        statement_end = _owner_statement_end(tokens, index + 2, min(before, end))
        rhs = tuple(tokens[item].value for item in range(index + 2, statement_end))
        valid_origin = (
            tokens[index + 1].value == "="
            and runtime is not None
            and len(rhs) >= 6
            and rhs[:5] == ("&", runtime, "->", "shards", "[")
            and rhs[-1] == "]"
            and len(rhs[5:-1]) == 1
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", rhs[5]) is not None
        )
        if valid_origin and origin is None:
            origin = index
        else:
            return False
    if origin is None:
        return False

    # Freeze the local receiver after the one direct runtime-shards origin.
    if _identifier_has_expression_write_or_move(
        tokens,
        receiver,
        origin + 1,
        min(before + 1, end),
        pointer=True,
    ):
        return False
    for index in range(body_open + 1, min(before + 1, end)):
        addressed = _addressed_identifier_at(tokens, index, min(before + 1, end))
        if addressed is not None and addressed[0] == receiver:
            if addressed[1] >= before + 1 or tokens[addressed[1]].value not in {"->", "."}:
                return False
        if tokens[index].value != receiver:
            continue
        previous = tokens[index - 1].value if index > body_open + 1 else None
        following = tokens[index + 1].value if index + 1 < end else None
        if following in ASSIGNMENT_OPERATORS and index != origin:
            return False
        if previous in {"++", "--"} or following in {"++", "--"}:
            return False
    return True


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

    # Matchers are deliberately a tiny, canonical expression.  A lexical
    # checker cannot prove arbitrary boolean data flow, so accepting only one
    # return statement made from direct atomic loads and audited predicates is
    # the fail-closed policy for this boundary.
    statements = [
        _statement_bounds(tokens, index, begin, end)
        for index in range(begin, end)
        if tokens[index].value == "return"
    ]
    if len(statements) != 1:
        return False
    statement_start, statement_end = statements[0]
    if statement_end >= len(tokens) or tokens[statement_end - 1].value != ";":
        return False
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None or statement_start != body_open + 1:
        # Matchers are expression-only helpers.  Declarations, guards, local
        # calls, and dead branches before the return are not part of the
        # canonical proof and can mutate/rebind a parameter invisibly.
        return False
    body_close = _matching_token(tokens, body_open, "{", "}")
    if body_close is None or any(
        tokens[index].value != "}"
        for index in range(statement_end, min(body_close + 1, end))
    ):
        return False
    if _parameter_has_escape_or_rebind(tokens, parameters, body_open, statement_start):
        return False
    expression_start = statement_start + 1
    if expression_start >= statement_end or tokens[expression_start].value == "return":
        expression_start += 1
    if expression_start >= statement_end:
        return False
    expression_values = [
        tokens[index].value for index in range(expression_start, statement_end - 1)
    ]
    if any(value in {"||", "!", "?", ":"} for value in expression_values):
        return False

    def direct_field_calls(field: str) -> list[_CallSite]:
        expected = ("&", parameters[0], "->", field)
        return [
            call
            for call in calls
            if begin <= call.name_index < end
            and call.name in ATOMIC_LOAD_NAMES
            and _canonical_atomic_load(tokens, call)
            and len(call.args) >= 1
            and _arg_values(tokens, call.args[0]) == expected
            and statement_start < call.name_index < statement_end
            and not _inside_constant_false_block(tokens, call.name_index, begin, end)
        ]

    field_calls: dict[str, _CallSite] = {}
    for field in fields:
        matches = direct_field_calls(field)
        if len(matches) != 1:
            return False
        field_calls[field] = matches[0]

    def direct_call(name: str, argument_values: tuple[str, ...]) -> _CallSite | None:
        matches = [
            call
            for call in calls
            if begin <= call.name_index < end
            and call.name == name
            and len(call.args) == 1
            and _arg_values(tokens, call.args[0]) == argument_values
            and statement_start < call.name_index < statement_end
        ]
        return matches[0] if len(matches) == 1 else None

    def direct_atomic_nested(name: str, argument_values: tuple[str, ...]) -> _CallSite | None:
        matches = [
            call
            for call in calls
            if begin <= call.name_index < end
            and call.name == name
            and (name not in ATOMIC_LOAD_NAMES or _canonical_atomic_load(tokens, call))
            and call.args
            and _arg_values(tokens, call.args[0]) == argument_values
            and statement_start < call.name_index < statement_end
        ]
        return matches[0] if len(matches) == 1 else None

    def direct_request_field(field: str) -> _CallSite | None:
        matches = direct_field_calls(field)
        return matches[0] if len(matches) == 1 else None

    request_calls: list[_CallSite] = []
    if base == "merge":
        requested = direct_request_field("merge_pause_requested")
        request_generation = direct_request_field("merge_pause_request_sequence")
        if requested is None or request_generation is None:
            return False
        request_calls.extend((requested, request_generation))
    else:
        request_generation = direct_request_field("steal_pause_request_sequence")
        runtime_generation = direct_atomic_nested(
            "atomic_load",
            ("&", parameters[0], "->", "runtime", "->", "steal_pause_request_sequence"),
        )
        runtime_generation_explicit = direct_atomic_nested(
            "atomic_load_explicit",
            ("&", parameters[0], "->", "runtime", "->", "steal_pause_request_sequence"),
        )
        runtime_pause = direct_call(
            "llam_runtime_steal_pause_active",
            (parameters[0], "->", "runtime"),
        )
        runtime_generation = runtime_generation or runtime_generation_explicit
        if request_generation is None or runtime_generation is None or runtime_pause is None:
            return False
        request_calls.extend((request_generation, runtime_generation, runtime_pause))

    all_required = [*field_calls.values(), *request_calls]
    if len({
        _statement_bounds(tokens, call.open_index, begin, end)
        for call in all_required
    }) != 1:
        return False

    def comparison(call: _CallSite) -> tuple[str, tuple[str, ...]] | None:
        after = call.close_index + 1
        if after < statement_end and tokens[after].value in {"==", "!="}:
            stop = after + 2
            while stop < statement_end and tokens[stop].value not in {"&&", "||", ")", ";"}:
                stop += 1
            values = tuple(tokens[index].value for index in range(after + 1, stop))
            return tokens[after].value, values
        before = call.open_index - 2
        if before >= expression_start and tokens[before + 1].value in {"==", "!="}:
            start = before
            while start > expression_start and tokens[start - 1].value not in {"&&", "||", "("}:
                start -= 1
            values = tuple(tokens[index].value for index in range(start, before + 1))
            return tokens[before + 1].value, values
        return None

    def has_comparison(call: _CallSite, operator: str, operand: tuple[str, ...]) -> bool:
        result = comparison(call)
        return result == (operator, operand)

    if not has_comparison(field_calls[fields[0]], "!=", ("0", "U")):
        return False
    if not has_comparison(field_calls[fields[1]], "==", (parameters[1],)):
        return False
    if not has_comparison(field_calls[fields[2]], "==", (parameters[2],)):
        return False
    if not has_comparison(field_calls[fields[3]], "==", (parameters[3],)):
        return False
    if base == "merge":
        if not has_comparison(request_calls[1], "==", (parameters[1],)):
            return False
        if not has_comparison(request_calls[0], "!=", ("0", "U")):
            return False
    else:
        if not has_comparison(request_calls[0], "==", (parameters[1],)):
            return False
        if not has_comparison(request_calls[1], "==", (parameters[1],)):
            return False

    # Only conjunctions of the direct fields, the live request predicate, and
    # the audited lease-owner helper are accepted.  This rejects attacker/OR,
    # hidden aliases, and a macro that happens to mention the right fields.
    allowed_calls = set(ATOMIC_LOAD_NAMES)
    allowed_calls.update({"llam_runtime_steal_pause_active", "llam_shard_pause_owner_matches_locked"})
    for call in calls:
        if not statement_start < call.name_index < statement_end:
            continue
        if call.name not in allowed_calls:
            return False
    owner_calls = [
        call
        for call in calls
        if statement_start < call.name_index < statement_end
        and call.name == "llam_shard_pause_owner_matches_locked"
    ]

    def top_level_parts(start: int, finish: int, operator: str) -> list[tuple[int, int]]:
        parts: list[tuple[int, int]] = []
        part_start = start
        paren_depth = bracket_depth = brace_depth = 0
        for index in range(start, finish):
            value = tokens[index].value
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
            elif (
                value == operator
                and paren_depth == 0
                and bracket_depth == 0
                and brace_depth == 0
            ):
                parts.append((part_start, index))
                part_start = index + 1
        parts.append((part_start, finish))
        return parts
    allowed_parts: set[tuple[str, ...]] = {
        (parameters[0], "!=", "NULL"),
        (parameters[1], "!=", "0", "U"),
    }

    def part_for_call(call: _CallSite) -> tuple[str, ...] | None:
        for part in top_level_parts(expression_start, statement_end - 1, "&&"):
            if part[0] <= call.name_index < part[1]:
                return tuple(tokens[index].value for index in range(part[0], part[1]))
        return None

    expected_call_parts = (
        (field_calls[fields[0]], "!=", ("0", "U")),
        (field_calls[fields[1]], "==", (parameters[1],)),
        (field_calls[fields[2]], "==", (parameters[2],)),
        (field_calls[fields[3]], "==", (parameters[3],)),
    )
    if base == "merge":
        expected_call_parts += (
            (request_calls[0], "!=", ("0", "U")),
            (request_calls[1], "==", (parameters[1],)),
        )
    else:
        expected_call_parts += (
            (request_calls[0], "==", (parameters[1],)),
            (request_calls[1], "==", (parameters[1],)),
            (request_calls[2], None, ()),
        )
    for required_call, operator, operand in expected_call_parts:
        if operator is None:
            part = part_for_call(required_call)
            if part is None:
                return False
            allowed_parts.add(part)
            continue
        call_prefix = tuple(
            tokens[index].value
            for index in range(required_call.name_index, required_call.close_index + 1)
        )
        allowed_parts.add(call_prefix + (operator,) + operand)

    for call in owner_calls:
        part = part_for_call(call)
        if part is None:
            return False
        allowed_parts.add(part)
    if any(
        tuple(tokens[index].value for index in range(part[0], part[1]))
        not in allowed_parts
        for part in top_level_parts(expression_start, statement_end - 1, "&&")
    ):
        return False
    for index in range(expression_start, statement_end - 1):
        if tokens[index].value == "=" and (index + 1 >= len(tokens) or tokens[index + 1].value != "="):
            return False
    if "&&" not in expression_values:
        return False
    if owner_calls:
        if len(owner_calls) != 1 or len(owner_calls[0].args) != 3:
            return False
        if tuple(_simple_identifier(tokens, argument) for argument in owner_calls[0].args) != (
            parameters[0], parameters[2], parameters[3]
        ):
            return False
        owner_part = part_for_call(owner_calls[0])
        owner_values = tuple(
            tokens[index].value
            for index in range(owner_calls[0].name_index, owner_calls[0].close_index + 1)
        )
        if owner_part != owner_values:
            return False
    else:
        # A tuple of ACK fields without a live lease-owner identity proof is
        # stale-state admitting, even when every atomic field comparison is
        # otherwise canonical.
        return False
    return True


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
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return False

    def body_depth(token_index: int) -> int:
        depth = 0
        for index in range(body_open, token_index):
            if tokens[index].value == "{":
                depth += 1
            elif tokens[index].value == "}":
                depth -= 1
        return depth

    control_records = _control_flow_records(tokens, begin, end)

    def in_conditional_body(token_index: int) -> bool:
        return _token_in_any_control_body(control_records, token_index)

    def direct_assignment_for_atomic_field(
        variable: str, field: str, before_index: int
    ) -> int | None:
        """Find the latest direct ``variable = atomic_load(&...field)``."""

        candidates = [
            call
            for call in calls
            if begin <= call.name_index < before_index
            and call.name in ATOMIC_LOAD_NAMES
            and call.args
            and _arg_values(tokens, call.args[0])
            == ("&", "shard", "->", field)
        ]
        latest: int | None = None
        for call in candidates:
            statement_start, _statement_end = _statement_bounds(
                tokens, call.name_index, begin, end
            )
            if not any(
                tokens[index].value == variable
                and index + 1 < call.name_index
                and tokens[index + 1].value == "="
                for index in range(statement_start, call.name_index)
            ):
                continue
            if in_conditional_body(call.name_index):
                continue
            latest = call.name_index
        return latest

    def direct_assignment_for_worker_incarnation(before_index: int) -> int | None:
        latest: int | None = None
        for call in calls:
            if not begin <= call.name_index < before_index:
                continue
            if call.name not in ATOMIC_LOAD_NAMES or not call.args:
                continue
            if _arg_values(tokens, call.args[0]) != (
                "&",
                "worker",
                "->",
                "binding_incarnation",
            ):
                continue
            statement_start, _statement_end = _statement_bounds(
                tokens, call.name_index, begin, end
            )
            if any(
                tokens[index].value == "incarnation"
                and index + 1 < call.name_index
                and tokens[index + 1].value == "="
                for index in range(statement_start, call.name_index)
            ):
                if in_conditional_body(call.name_index):
                    continue
                latest = call.name_index
        return latest

    request_origin = direct_assignment_for_atomic_field(
        "request_sequence",
        f"{base}_pause_request_sequence",
        before,
    )
    incarnation_origin = direct_assignment_for_worker_incarnation(before)
    if request_origin is None or incarnation_origin is None:
        return False

    # A tuple field has one canonical publication on the path that can reach
    # the nonzero ACK.  Reset stores in the early failure branch are allowed,
    # but a second unconditional store (or a conditional overwrite after the
    # canonical store) would let the boolean ACK advertise a different tuple
    # than the one this audit observed.
    unconditional_stores: dict[str, list[_CallSite]] = {
        field: [] for field in required
    }
    all_tuple_stores: list[tuple[str, _CallSite]] = []
    for candidate in calls:
        if (
            candidate.name not in ATOMIC_STORE_NAMES
            or candidate.name_index >= before
            or not candidate.args
            or len(candidate.args) < 2
            or _inside_constant_false_block(tokens, candidate.name_index, begin, end)
        ):
            continue
        target = _arg_values(tokens, candidate.args[0])
        field = next(
            (
                item
                for item in required
                if target == ("&", "shard", "->", item)
            ),
            None,
        )
        if field is None:
            continue
        if (
            candidate.name != "atomic_store_explicit"
            or len(candidate.args) != 3
            or _arg_values(tokens, candidate.args[2]) != ("memory_order_release",)
        ):
            # Publication stores are the release edge for the ACK tuple.  A
            # relaxed/implicit atomic or a wrong arity is not the canonical
            # operation even when its first two arguments look right.
            return False
        all_tuple_stores.append((field, candidate))
        if not in_conditional_body(candidate.name_index):
            unconditional_stores[field].append(candidate)
    if any(len(stores) > 1 for stores in unconditional_stores.values()):
        return False
    for field, candidate in all_tuple_stores:
        if in_conditional_body(candidate.name_index) and any(
            prior.name_index < candidate.name_index
            for prior in unconditional_stores[field]
        ):
            return False

    def variable_rebound(variable: str, start: int, finish: int) -> bool:
        for index in range(start, min(finish, end)):
            if tokens[index].value == "&" and index + 1 < finish:
                if tokens[index + 1].value == variable:
                    return True
            if tokens[index].value != variable:
                continue
            if index + 1 < finish and tokens[index + 1].value in ASSIGNMENT_OPERATORS:
                return True
            if index > start and tokens[index - 1].value in {"++", "--"}:
                return True
            if index + 1 < finish and tokens[index + 1].value in {"++", "--"}:
                return True
        # Parenthesized/comma/cast lvalues do not put the assignment operator
        # immediately after the identifier.  Treat any assignment statement
        # containing the routed temporary on its left side as a rebind.
        for index in range(start, min(finish, end)):
            if tokens[index].value != variable:
                continue
            statement_start, statement_end = _statement_bounds(
                tokens, index, begin, min(finish, end)
            )
            if any(
                tokens[item].value in ASSIGNMENT_OPERATORS
                and item > index
                for item in range(statement_start, statement_end)
            ):
                return True
        return False

    def address_escaped_before(variable: str, start: int) -> bool:
        # An address alias made before the canonical field load can overwrite
        # the temporary after that load.  Inspect the whole function prefix,
        # not just the post-origin interval.
        for index in range(body_open + 1, min(start, end)):
            addressed = _addressed_identifier_at(tokens, index, min(start, end))
            if addressed is not None and addressed[0] == variable:
                return True
        return False

    if address_escaped_before("request_sequence", request_origin) or variable_rebound(
        "request_sequence", request_origin + 1, before
    ):
        return False
    if address_escaped_before("incarnation", incarnation_origin) or variable_rebound(
        "incarnation", incarnation_origin + 1, before
    ):
        return False

    seen: set[str] = set()
    required_values = {
        f"{base}_pause_ack_sequence": lambda values: values == ("request_sequence",),
        f"{base}_pause_ack_lease_epoch": lambda values: values
        == ("shard", "->", "legacy_lease", ".", "epoch"),
        f"{base}_pause_ack_worker_incarnation": lambda values: values == ("incarnation",),
    }
    for call in calls:
        if (
            call.name not in ATOMIC_STORE_NAMES
            or call.name_index >= before
            or not call.args
            or not function_lines[0] <= tokens[call.name_index].line <= function_lines[1]
            or _inside_constant_false_block(tokens, call.name_index, begin, end)
            or body_depth(call.name_index) != 1
            or in_conditional_body(call.name_index)
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
    member_index: int,
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
    begin, end = _token_range_for_span(masked, function)
    parameters = _function_parameter_names(masked, tokens, function)
    if not _receiver_is_stable(masked, tokens, function, receiver, end):
        return False
    # When the receiver is a local shard derived from ``rt->shards[index]``,
    # freeze the runtime parameter as well.  Otherwise a later ``rt`` rebind
    # can change the meaning of the indexed receiver while its spelling stays
    # unchanged.
    if parameters and parameters[0] != receiver:
        body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            None,
        )
        if body_open is None or _parameter_has_escape_or_rebind(
            tokens, (parameters[0],), body_open, end
        ):
            return False

    request_names = {
        "request_sequence",
        "merge_pause_sequence",
        "steal_pause_sequence",
    }
    epoch_names = {
        "epoch_value",
        "merge_pause_epoch",
        "steal_pause_epoch",
    }
    worker_names = {
        "worker_incarnation",
        "merge_pause_incarnation",
        "steal_pause_incarnation",
    }

    # The runtime-wide steal wait samples its request generation from ``rt``
    # before it visits each ``shard``.  That is the one canonical request
    # origin whose receiver is not the matcher receiver itself; keep it
    # explicit instead of accepting arbitrary field-shaped aliases.
    request_receivers = {receiver}
    if base == "steal" and function.name == "llam_runtime_wait_steal_pause_ack":
        request_receivers = {"rt"}

    def argument_has_direct_field(argument: Sequence[int], field: str) -> bool:
        return _exact_atomic_field_argument(
            tokens,
            calls,
            argument,
            receiver or "",
            field,
        ) is not None

    def variable_origin_matches(
        argument: Sequence[int],
        field: str,
        before_index: int,
        expected_receivers: set[str] | None = None,
    ) -> bool:
        """Require an identifier tuple argument to come from that field."""

        variable = _simple_identifier(tokens, argument)
        if variable is None:
            return False
        latest_assignment: int | None = None
        for index in range(begin, before_index):
            if (
                tokens[index].value == variable
                and index + 1 < before_index
                and tokens[index + 1].value in ASSIGNMENT_OPERATORS
            ):
                latest_assignment = index
        if latest_assignment is None:
            return False
        statement_end = _owner_statement_end(tokens, latest_assignment + 1, before_index)
        origin_call: _CallSite | None = None
        receivers = expected_receivers or {receiver}
        for candidate in calls:
            if not latest_assignment < candidate.name_index < before_index:
                continue
            if candidate.name not in ATOMIC_LOAD_NAMES or not candidate.args:
                continue
            values = _arg_values(tokens, candidate.args[0])
            if len(values) != 4 or values[0] != "&" or values[2] != "->":
                continue
            if values[1] not in receivers or values[3] != field:
                continue
            if candidate.name_index >= statement_end:
                continue
            if candidate.name_index != latest_assignment + 2:
                continue
            if candidate.close_index + 1 != statement_end:
                continue
            if candidate.name == "atomic_load" and len(candidate.args) != 1:
                continue
            if candidate.name == "atomic_load_explicit" and (
                len(candidate.args) != 2
                or _arg_values(tokens, candidate.args[1])
                != ("memory_order_acquire",)
            ):
                continue
            origin_call = candidate
            break
        if origin_call is None:
            return False
        return True

    def matcher_arguments_are_canonical(candidate: _CallSite) -> bool:
        if len(candidate.args) != 4:
            return False
        if _simple_identifier(tokens, candidate.args[0]) != receiver:
            return False
        request_field = f"{base}_pause_request_sequence"
        request_ok = argument_has_direct_field(candidate.args[1], request_field) or (
            _simple_identifier(tokens, candidate.args[1]) in request_names
            and variable_origin_matches(
                candidate.args[1],
                request_field,
                candidate.name_index,
                request_receivers,
            )
        )
        epoch_ok = (
            _simple_identifier(tokens, candidate.args[2]) in epoch_names
            and variable_origin_matches(
                candidate.args[2],
                f"{base}_pause_ack_lease_epoch",
                candidate.name_index,
            )
            or argument_has_direct_field(candidate.args[2], f"{base}_pause_ack_lease_epoch")
        )
        worker_ok = (
            _simple_identifier(tokens, candidate.args[3]) in worker_names
            and variable_origin_matches(
                candidate.args[3],
                f"{base}_pause_ack_worker_incarnation",
                candidate.name_index,
            )
            or argument_has_direct_field(
                candidate.args[3], f"{base}_pause_ack_worker_incarnation"
            )
        )
        return request_ok and epoch_ok and worker_ok

    matcher_calls = [
        call
        for call in calls
        if begin <= call.name_index < end
        and call.name == matcher_name
        and not _inside_constant_false_block(tokens, call.name_index, begin, end)
        and len(call.args) >= 1
        and _simple_identifier(tokens, call.args[0]) == receiver
        and matcher_arguments_are_canonical(call)
    ]
    if not matcher_calls:
        return False

    def direct_field_load(call: _CallSite) -> bool:
        if call.name not in ATOMIC_LOAD_NAMES or not call.args:
            return False
        return _arg_values(tokens, call.args[0]) == ("&", receiver, "->", member)

    def matcher_result_routes(call: _CallSite) -> bool:
        """Require a matcher result to control a return/branch.

        A standalone call (especially one under ``if (flag)``) is not a
        validation route.  The native code either embeds the matcher as the
        complete ``if``/``return`` condition or assigns it to ``matched`` and
        tests that value immediately afterwards.  Do not treat a matcher
        result that is ORed with an attacker-controlled value as a proof: the
        decision expression must be exactly this call (optionally negated for
        a failure branch).
        """

        call_values = tuple(
            tokens[index].value for index in range(call.name_index, call.close_index + 1)
        )
        statement_start, statement_end = _statement_bounds(
            tokens, call.open_index, begin, end
        )
        # A direct return may contain only this matcher call (or its direct
        # negation).  Parenthesized/combined expressions are unsupported.
        if statement_start < statement_end and tokens[statement_start].value == "return":
            expression = tuple(
                tokens[index].value
                for index in range(statement_start + 1, statement_end - 1)
            )
            return expression in {call_values, ("!", *call_values)}

        # Calls used directly as the condition of ``if (...)`` are nested in
        # the condition's parentheses.  The full condition must be exactly
        # this call or ``!`` followed by it; no attacker-controlled operand or
        # recombination is inferred.
        for index in range(begin, call.name_index):
            if tokens[index].value != "if" or index + 1 >= end:
                continue
            if tokens[index + 1].value != "(":
                continue
            condition_end = _matching_token(tokens, index + 1, "(", ")")
            if condition_end is None or not index < call.name_index < condition_end:
                continue
            condition = tuple(
                tokens[item].value for item in range(index + 2, condition_end)
            )
            if condition in {call_values, ("!", *call_values)}:
                return True
            return False

        # Assignment to a boolean is accepted only when the assignment RHS is
        # exactly this matcher call and the first later use is a direct
        # ``if (matched)``/``if (!matched)`` or ``return matched`` decision.
        equals = next(
            (
                index
                for index in range(statement_start, call.name_index)
                if tokens[index].value == "="
            ),
            None,
        )
        if equals is None or equals == statement_start:
            return False
        lhs = tokens[equals - 1].value
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs):
            return False
        assignment = tuple(
            tokens[index].value for index in range(equals + 1, statement_end - 1)
        )
        if assignment != call_values:
            return False

        for index in range(call.close_index + 1, end):
            if tokens[index].value != lhs:
                continue
            # A rebind before the decision invalidates the routed result.
            if index + 1 < end and tokens[index + 1].value in ASSIGNMENT_OPERATORS:
                return False
            for if_index in range(begin, index):
                if tokens[if_index].value != "if" or if_index + 1 >= end:
                    continue
                if tokens[if_index + 1].value != "(":
                    continue
                condition_end = _matching_token(tokens, if_index + 1, "(", ")")
                if condition_end is None or not if_index < index < condition_end:
                    continue
                condition = tuple(
                    tokens[item].value for item in range(if_index + 2, condition_end)
                )
                return condition in {(lhs,), ("!", lhs)}
            statement_start2, statement_end2 = _statement_bounds(
                tokens, index, begin, end
            )
            if (
                statement_start2 < statement_end2
                and tokens[statement_start2].value == "return"
                and tuple(
                    tokens[item].value
                    for item in range(statement_start2 + 1, statement_end2 - 1)
                )
                in {(lhs,), ("!", lhs)}
            ):
                return True
            return False
        return False

    def variable_has_alias_or_rebind(
        variable: str, start: int, finish: int
    ) -> bool:
        """Reject address escapes and writes to a routed tuple temporary."""

        if _identifier_has_expression_write_or_move(
            tokens, variable, max(start, begin), min(finish, end)
        ):
            return True
        for index in range(max(start, begin), min(finish, end)):
            addressed = _addressed_identifier_at(tokens, index, min(finish, end))
            if addressed is not None and addressed[0] == variable:
                return True
            if tokens[index].value != variable:
                continue
            if index + 1 < finish and tokens[index + 1].value in ASSIGNMENT_OPERATORS:
                return True
            if index > start and tokens[index - 1].value in {"++", "--"}:
                return True
            if index + 1 < finish and tokens[index + 1].value in {"++", "--"}:
                return True
        return False

    def routed_variables_are_stable(candidate: _CallSite) -> bool:
        if len(candidate.args) != 4:
            return False
        # Inline direct loads have no temporary to alias.  Identifier
        # arguments must remain untouched from their latest visible
        # assignment through this exact matcher call.
        for argument_index in (1, 2, 3):
            variable = _simple_identifier(tokens, candidate.args[argument_index])
            if variable is None:
                continue
            latest_assignment = None
            for index in range(begin, candidate.name_index):
                if (
                    tokens[index].value == variable
                    and index + 1 < candidate.name_index
                    and tokens[index + 1].value in ASSIGNMENT_OPERATORS
                ):
                    latest_assignment = index
            if latest_assignment is None:
                # A function parameter or an uninitialized temporary cannot
                # be treated as a tuple provenance proof.
                return False
            # An address alias formed before the field load can overwrite the
            # freshly loaded temporary through a slot.  The prior stability
            # walk started at the latest assignment and therefore missed that
            # alias; inspect the whole function prefix for address escapes,
            # then inspect the post-origin interval for ordinary rebinds.
            if any(
                (
                    addressed := _addressed_identifier_at(
                        tokens, index, latest_assignment
                    )
                )
                is not None
                and addressed[0] == variable
                for index in range(begin, latest_assignment)
            ) or variable_has_alias_or_rebind(
                variable,
                latest_assignment + 1,
                candidate.name_index,
            ):
                return False
            # A redundant-parenthesis lvalue does not look like
            # ``variable =`` to the simple token walk above.  Treat any
            # assignment statement containing this temporary as a write,
            # while leaving the original exact field-load statement intact.
            for variable_index in range(
                latest_assignment + 1, candidate.name_index
            ):
                if tokens[variable_index].value != variable:
                    continue
                statement_start, statement_end = _statement_bounds(
                    tokens, variable_index, begin, candidate.name_index
                )
                if any(
                    tokens[item].value in ASSIGNMENT_OPERATORS
                    for item in range(statement_start, statement_end)
                ):
                    return False
        return True

    for matcher in matcher_calls:
        if not routed_variables_are_stable(matcher):
            continue
        # A tuple load directly inside the matcher arguments is tied to this
        # exact read.  This covers the native inline atomic snapshots.
        if matcher.open_index < member_index < matcher.close_index:
            expected_argument = (
                1
                if member.endswith("_ack_sequence")
                else 2
                if member.endswith("_ack_lease_epoch")
                else 3
            )
            if member_index in matcher.args[expected_argument]:
                load = _call_containing(calls, member_index, ATOMIC_LOAD_NAMES)
                if load is not None and direct_field_load(load):
                    return matcher_result_routes(matcher)
        # Otherwise require the read to initialize the corresponding matcher
        # argument before the call.  This covers the native epoch/incarnation
        # temporaries used by the pause wait loops.
        if matcher.name_index <= member_index or len(matcher.args) < 4:
            continue
        expected_argument = (
            1
            if member.endswith("_ack_sequence")
            else 2
            if member.endswith("_ack_lease_epoch")
            else 3
        )
        variable = _simple_identifier(tokens, matcher.args[expected_argument])
        if variable is None:
            continue
        load = _call_containing(calls, member_index, ATOMIC_LOAD_NAMES)
        if load is None or not direct_field_load(load):
            continue
        statement_start, _statement_end = _statement_bounds(
            tokens, load.open_index, begin, end
        )
        if not any(
            tokens[index].value == variable
            and index + 1 < load.name_index
            and tokens[index + 1].value == "="
            for index in range(statement_start, load.name_index)
        ):
            continue
        if variable_has_alias_or_rebind(
            variable,
            load.close_index + 1,
            matcher.name_index,
        ):
            continue
        if matcher_result_routes(matcher):
            return True
    return False


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

    # The audited matcher/publication/sampling files use direct calls only.
    # Rejecting every preprocessor definition in these files prevents an
    # object-like macro from erasing an ACK field, tuple store, or routed
    # decision from the lexical token stream.
    if path in {
        "src/core/sched/scheduler.c",
        "src/engine/watchdog/watchdog_merge.c",
        "src/engine/watchdog/watchdog_scale.c",
    }:
        for line, raw in enumerate(masked.splitlines(), start=1):
            if re.match(r"^\s*#\s*define\b", raw):
                fail(line)

    # Publication helpers are a closed source-level API.  A generic helper or
    # macro can otherwise hide a tuple write (or an ACK store) from the member
    # walk below.  Keep only the operations present in the native helpers.
    publication_calls = {
        *ATOMIC_LOAD_NAMES,
        *ATOMIC_STORE_NAMES,
        "llam_runtime_steal_pause_active",
        "llam_worker_binding_live",
    }
    for function in functions:
        if function.name not in PAUSE_ACK_PUBLICATION_HELPERS:
            continue
        begin, end = _token_range_for_span(masked, function)
        body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            end,
        )
        parameters = _function_parameter_names(masked, tokens, function)
        if body_open >= end or _parameter_has_escape_or_rebind(
            tokens, parameters, body_open, end
        ):
            # Publication is tied to the exact shard supplied by the caller;
            # a receiver rebind/address escape can turn a lock-protected ACK
            # store into a write to another shard.
            fail(function.start_line)
        definition_indices = {
            index
            for index in range(begin, body_open)
            if tokens[index].value == function.name
            and index + 1 < body_open
            and tokens[index + 1].value == "("
        }
        for call in calls:
            if not begin <= call.name_index < end:
                continue
            if call.name_index in definition_indices:
                continue
            if call.name in publication_calls or call.name in CONTROL_FLOW_NAMES:
                continue
            fail(tokens[call.name_index].line)

        # The publication helper obtains the worker only from the scheduler's
        # TLS binding.  A later-looking direct ``worker->...`` store is not a
        # provenance proof if the local was rebound or its address escaped.
        worker_name = "worker"
        worker_origin: int | None = None
        for index in range(body_open + 1, end):
            if tokens[index].value != worker_name or index + 1 >= end:
                continue
            if tokens[index + 1].value not in ASSIGNMENT_OPERATORS:
                continue
            statement_end = _owner_statement_end(tokens, index + 2, end)
            rhs = tuple(tokens[item].value for item in range(index + 2, statement_end))
            if rhs == ("g_llam_tls_worker",):
                if worker_origin is not None:
                    fail(tokens[index].line)
                worker_origin = index
            else:
                # ``worker = attacker`` and compound assignments are never a
                # canonical TLS-origin declaration.
                fail(tokens[index].line)
        if worker_origin is None:
            fail(function.start_line)
        else:
            for index in range(body_open + 1, end):
                addressed = _addressed_identifier_at(tokens, index, end)
                if addressed is not None and addressed[0] == worker_name:
                    if addressed[1] >= end or tokens[addressed[1]].value not in {"->", "."}:
                        fail(tokens[index].line)
                        break
                if tokens[index].value != worker_name:
                    continue
                previous = tokens[index - 1].value if index > body_open + 1 else None
                following = tokens[index + 1].value if index + 1 < end else None
                if following in ASSIGNMENT_OPERATORS and index != worker_origin:
                    fail(tokens[index].line)
                    break
                if index == worker_origin:
                    continue
                if previous in {"++", "--", "+", "-"} or following in {
                    "++",
                    "--",
                    "+",
                    "-",
                }:
                    fail(tokens[index].line)
                    break
                statement_start, statement_end = _statement_bounds(
                    tokens, index, body_open + 1, end
                )
                if any(
                    tokens[item].value in ASSIGNMENT_OPERATORS and item > index
                    for item in range(index + 1, statement_end)
                ):
                    fail(tokens[index].line)
                    break

        base = "merge" if function.name.endswith("merge_pause_ack_locked") else "steal"
        publication_fields = {
            f"{base}_pause_ack",
            f"{base}_pause_ack_sequence",
            f"{base}_pause_ack_lease_epoch",
            f"{base}_pause_ack_worker_incarnation",
        }
        # A publication helper may store only the four direct fields of its
        # own channel.  This closes the no-member-token path where an alias or
        # pointer slot receives a tuple overwrite that the member walk cannot
        # observe.
        for candidate in calls:
            if not body_open < candidate.name_index < end:
                continue
            if candidate.name not in ATOMIC_STORE_NAMES or not candidate.args:
                continue
            target = _arg_values(tokens, candidate.args[0])
            if len(target) != 4 or target[0] != "&" or target[1] != "shard" or target[2] != "->" or target[3] not in publication_fields:
                fail(tokens[candidate.name_index].line)

        # The release ACK bit is the single publication edge.  Once its sole
        # nonzero store executes, no tuple field or ACK field may be written
        # again—even with a relaxed order, a zero reset, or from a conditional
        # branch.  Looking only at tuple stores before the candidate ACK lets
        # a later overwrite advertise a different tuple to an acquire-side
        # sampler, so validate the complete helper through its closing brace.
        publication_control_records = _control_flow_records(tokens, begin, end)
        publication_stores: list[tuple[str, _CallSite]] = []
        for candidate in calls:
            if (
                not body_open < candidate.name_index < end
                or candidate.name not in ATOMIC_STORE_NAMES
                or len(candidate.args) < 2
            ):
                continue
            target = _arg_values(tokens, candidate.args[0])
            field = next(
                (
                    item
                    for item in publication_fields
                    if target == ("&", "shard", "->", item)
                ),
                None,
            )
            if field is None:
                continue
            publication_stores.append((field, candidate))

            # A tuple publication is one unconditional sequence.  The early
            # NULL/request guard is the only conditional control shape in
            # this helper; treating any tuple store under another ``if`` as
            # canonical would leave a path with a partially published tuple.
            if _token_in_any_control_body(
                publication_control_records, candidate.name_index
            ) and not _inside_constant_false_block(
                tokens, candidate.name_index, begin, end
            ) and not _is_zero_value(tokens, candidate.args[1]):
                fail(tokens[candidate.name_index].line)

        nonzero_ack = [
            candidate
            for field, candidate in publication_stores
            if field == f"{base}_pause_ack"
            and _is_one_value(tokens, candidate.args[1])
        ]
        if len(nonzero_ack) != 1:
            # The member walk reports malformed/missing ACK publications, but
            # recording the helper line here also catches an ACK hidden behind
            # a noncanonical value that otherwise leaves no positive edge.
            fail(function.start_line)
        else:
            ack_index = nonzero_ack[0].name_index
            for _field, candidate in publication_stores:
                if candidate.name_index > ack_index:
                    fail(tokens[candidate.name_index].line)

    # Sampling functions are an explicit, closed list of matcher callers.  A
    # helper or macro with no visible ACK field could otherwise hide the
    # complete decision and leave no member token for the routing checks below.
    for function in functions:
        if function.name not in PAUSE_ACK_SAMPLING_FUNCTIONS:
            continue
        begin, end = _token_range_for_span(masked, function)
        body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            end,
        )
        for call in calls:
            if not body_open < call.name_index < end:
                continue
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", call.name):
                continue
            if call.name in PAUSE_ACK_SAMPLING_ALLOWED_CALLS or call.name in CONTROL_FLOW_NAMES:
                continue
            fail(tokens[call.name_index].line)

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
                masked, tokens, function, member, calls, receiver, member_index
            ):
                allowed = call.name in ATOMIC_LOAD_NAMES and direct_receiver
        elif path == "src/core/sched/scheduler.c" and function is not None:
            if function.name in PAUSE_ACK_MATCHERS:
                allowed = matcher_valid.get(function.name, False) and call is not None and call.name in ATOMIC_LOAD_NAMES
            elif function.name in PAUSE_ACK_PUBLICATION_HELPERS and call is not None:
                if call.name in ATOMIC_STORE_NAMES and direct_receiver:
                    if member in PAUSE_ACK_FIELDS:
                        canonical_store = (
                            call.name == "atomic_store_explicit"
                            and len(call.args) == 3
                            and _arg_values(tokens, call.args[2])
                            == ("memory_order_release",)
                        )
                        allowed = canonical_store and (
                            _is_zero_value(tokens, call.args[1] if len(call.args) > 1 else ())
                            or (
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
                masked, tokens, function, member, calls, receiver, member_index
            ):
                allowed = call is not None and call.name in ATOMIC_LOAD_NAMES and direct_receiver
        elif function is not None and _ack_sampling_allowed(
            masked, tokens, function, member, calls, receiver, member_index
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
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return False

    def body_depth(token_index: int) -> int:
        depth = 0
        for index in range(body_open, token_index):
            if tokens[index].value == "{":
                depth += 1
            elif tokens[index].value == "}":
                depth -= 1
        return depth

    events: list[tuple[int, bool]] = []
    for index in range(max(begin + 1, 1), min(before, end)):
        if tokens[index].value not in ASSIGNMENT_OPERATORS or index == 0:
            continue
        if tokens[index - 1].value != variable:
            continue
        statement_end = _owner_statement_end(tokens, index + 1, min(before, end))
        rhs = [tokens[item].value for item in range(index + 1, statement_end)]
        exact = (
            body_depth(index) == 1
            and rhs == ["llam_shard_legacy_lease_locked", "(", "shard", ")"]
        )
        if function.name == "llam_legacy_lease_init":
            exact = exact or (
                body_depth(index) == 1
                and rhs == ["&", "shard", "->", "legacy_lease"]
            )
        events.append(
            (
                index,
                exact
                and not _inside_constant_false_block(tokens, index, begin, end),
            )
        )
    if not events:
        return False
    # Every visible rebind must be the one direct coordinator origin.  Merely
    # ignoring assignments nested under ``if``/loops lets an attacker move the
    # pointer after the canonical origin and retain a lease-looking receiver.
    return all(exact for _index, exact in events)


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
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return False

    def body_depth(token_index: int) -> int:
        depth = 0
        for index in range(body_open, token_index):
            if tokens[index].value == "{":
                depth += 1
            elif tokens[index].value == "}":
                depth -= 1
        return depth

    guard_records = _control_flow_records(tokens, begin, end)

    def in_returning_guard(token_index: int) -> bool:
        return any(
            record[1] <= token_index < record[2]
            and _has_unconditional_false_return(
                tokens,
                guard_records,
                record[3],
                record[4],
            )
            for record in guard_records
        )

    def top_level_parts(start: int, finish: int) -> list[tuple[int, int]]:
        """Split one failure condition at top-level ``||`` operators."""

        parts: list[tuple[int, int]] = []
        part_start = start
        paren_depth = bracket_depth = brace_depth = 0
        for index in range(start, finish):
            value = tokens[index].value
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
            elif (
                value == "||"
                and paren_depth == 0
                and bracket_depth == 0
                and brace_depth == 0
            ):
                parts.append((part_start, index))
                part_start = index + 1
        parts.append((part_start, finish))
        return parts

    def strip_outer_parentheses(values: tuple[str, ...]) -> tuple[str, ...]:
        while len(values) >= 2 and values[0] == "(" and values[-1] == ")":
            depth = 0
            wraps_all = True
            for offset, value in enumerate(values):
                if value == "(":
                    depth += 1
                elif value == ")":
                    depth -= 1
                    if depth == 0 and offset != len(values) - 1:
                        wraps_all = False
                        break
            if not wraps_all or depth != 0:
                break
            values = values[1:-1]
        return values

    def strict_failure_guard(
        token_index: int, expected: tuple[str, ...]
    ) -> bool:
        """Require one expected predicate as a complete ``||`` arm.

        The old proof accepted a predicate nested in ``flag && (...)``.  A
        lexical audit cannot establish that the extra condition is always
        true, so non-canonical conjunctions are rejected.  The completed
        handback transition has one explicitly audited parenthesized
        conjunction; retain that production form while keeping every other
        transition arm exact.
        """

        for record in guard_records:
            if not record[1] <= token_index < record[2]:
                continue
            if record[4] > member_index or not _has_unconditional_false_return(
                tokens, guard_records, record[3], record[4]
            ):
                continue
            condition_parts = top_level_parts(record[1], record[2])
            condition_values = tuple(
                tokens[index].value for index in range(record[1], record[2])
            )
            allowed_identifiers = {
                "lease",
                "owner",
                "owner_incarnation",
                "state",
                "transfer_sequence",
                "sequence",
                "helper",
                "worker",
                "primary",
                "claimant",
                "claimant_incarnation",
                "return_worker",
                "return_incarnation",
                "incarnation",
                "shard",
                "opaque_helper_worker",
                "primary_worker",
                "role",
                "compat_shard",
                "NULL",
                "U",
                "llam_worker_binding_live",
                "llam_legacy_lease_matches_worker",
                "llam_legacy_lease_merge_paused",
                "llam_legacy_lease_runtime_stopping",
            }
            if any(
                re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value)
                and value not in allowed_identifiers
                and not value.startswith("LLAM_")
                for value in condition_values
            ):
                continue
            for part_start, part_end in condition_parts:
                part = strip_outer_parentheses(
                    tuple(tokens[index].value for index in range(part_start, part_end))
                )
                if part == expected:
                    return True
                if function.name == "llam_legacy_lease_complete_handback_locked":
                    # Only the canonical state/owner pair conjunctions may
                    # contain an expected predicate beneath parentheses.
                    if "flag" in part or "attacker" in part:
                        continue
                    allowed = {
                        "lease",
                        "owner",
                        "owner_incarnation",
                        "state",
                        "transfer_sequence",
                        "sequence",
                        "worker",
                        "claimant",
                        "claimant_incarnation",
                        "NULL",
                        "LLAM_LEASE_TRANSFER_TO_PRIMARY",
                        "LLAM_LEASE_STOPPING",
                        "llam_legacy_lease_matches_worker",
                        "&&",
                        "!=",
                        "==",
                        "(",
                        ")",
                        ",",
                    }
                    if expected == ("lease", "->", "owner", "!=", "NULL"):
                        if part == (
                            "lease",
                            "->",
                            "owner",
                            "!=",
                            "NULL",
                            "&&",
                            "lease",
                            "->",
                            "owner",
                            "!=",
                            "worker",
                        ):
                            return True
                    if all(value in allowed for value in part) and expected in tuple(
                        tuple(tokens[index].value for index in range(start, end))
                        for start, end in top_level_parts(part_start, part_end)
                    ):
                        return True
                if (
                    function.name == "llam_legacy_lease_rollback_helper_locked"
                    and expected == ("lease", "->", "owner", "!=", "NULL")
                    and part
                    == (
                        "lease",
                        "->",
                        "owner",
                        "!=",
                        "NULL",
                        "&&",
                        "(",
                        "lease",
                        "->",
                        "owner",
                        "!=",
                        "primary",
                        "||",
                        "lease",
                        "->",
                        "owner_incarnation",
                        "!=",
                        "incarnation",
                        ")",
                    )
                ):
                    return True
        return False

    if not _direct_lease_origin(masked, tokens, function, "lease", member_index):
        return False

    def latest_assignment(
        variable: str,
    ) -> tuple[int, tuple[str, ...]] | None:
        latest: tuple[int, tuple[str, ...]] | None = None
        for index in range(body_open + 1, member_index):
            if (
                tokens[index].value != variable
                or index + 1 >= member_index
                or tokens[index + 1].value not in ASSIGNMENT_OPERATORS
            ):
                continue
            statement_end = _owner_statement_end(tokens, index + 2, member_index)
            rhs = tuple(tokens[item].value for item in range(index + 2, statement_end))
            latest = (index, rhs)
        return latest

    if function.name == "llam_legacy_lease_complete_handback_locked":
        claimant_assignment = latest_assignment("claimant")
        if claimant_assignment is None or claimant_assignment[1] != (
            "lease",
            "->",
            "return_worker",
        ):
            return False
        claimant_incarnation_assignment = latest_assignment("claimant_incarnation")
        if claimant_incarnation_assignment is None or claimant_incarnation_assignment[1] != (
            "lease",
            "->",
            "return_incarnation",
        ):
            return False

    # The value written into the owner slot must remain the actual transition
    # claimant/parameter.  Direct member reads such as ``&helper->incarnation``
    # are allowed; a bare address escape, pointer movement, or reassignment is
    # not.
    value_variables = {value}
    if function.name == "llam_legacy_lease_complete_handback_locked":
        value_variables.update({"claimant", "claimant_incarnation"})

    # Freeze every identity value that feeds the owner tuple after its proof.
    # A local scalar such as ``incarnation`` is normally initialized by one
    # direct load, while claimant is extracted from the lease.  Start the
    # stability walk after that exact origin so the initializer remains a
    # valid declaration/split-assignment form, then reject grouped/cast/comma
    # lvalues and unary movement through the publication.
    protected_values = set(value_variables)
    protected_values.update(
        {
            "shard",
            "sequence",
            "incarnation",
            "claimant",
            "claimant_incarnation",
            "worker",
            "helper",
            "primary",
        }
    )

    def latest_direct_assignment_index(variable: str) -> int | None:
        latest: int | None = None
        for index in range(body_open + 1, member_index):
            if (
                tokens[index].value == variable
                and index + 1 < member_index
                and tokens[index + 1].value == "="
            ):
                latest = index
        return latest

    scalar_identity_values = {
        "sequence",
        "incarnation",
        "claimant_incarnation",
        "return_incarnation",
    }
    for variable in protected_values:
        origin = latest_direct_assignment_index(variable)
        start = origin + 1 if origin is not None else body_open + 1
        if _identifier_has_expression_write_or_move(
            tokens,
            variable,
            start,
            member_index,
            pointer=variable not in scalar_identity_values,
        ):
            return False

    for index in range(body_open + 1, member_index):
        if tokens[index].value not in value_variables:
            continue
        previous = tokens[index - 1].value if index > body_open + 1 else None
        following = tokens[index + 1].value if index + 1 < member_index else None
        if following in ASSIGNMENT_OPERATORS or previous in {"++", "--", "+", "-"} or following in {"++", "--", "+", "-"}:
            # The exact claimant extraction assignments are the only local
            # derivations allowed before a handback publication.
            if function.name == "llam_legacy_lease_complete_handback_locked" and following == "=":
                statement_end = _owner_statement_end(tokens, index + 2, member_index)
                rhs = tuple(tokens[item].value for item in range(index + 2, statement_end))
                expected = {
                    "claimant": ("lease", "->", "return_worker"),
                    "claimant_incarnation": ("lease", "->", "return_incarnation"),
                }.get(tokens[index].value)
                if rhs == expected:
                    continue
            return False
        if previous == "&" and not (
            index + 1 < member_index and tokens[index + 1].value in {"->", "."}
        ):
            return False

    def has_pattern(pattern: tuple[str, ...], allow_nested: bool = False) -> bool:
        width = len(pattern)
        for index in range(begin, max(begin, member_index - width + 1)):
            if _inside_constant_false_block(tokens, index, begin, end):
                continue
            if body_depth(index) != 1 and not (allow_nested and body_depth(index) == 2):
                continue
            if tuple(tokens[item].value for item in range(index, index + width)) == pattern:
                # Comparisons are proof only when they are part of a visible
                # failure guard.  Exact claimant extraction assignments are
                # data derivations, not guards, and remain allowed here.
                positive_transition = pattern == (
                    "lease",
                    "->",
                    "state",
                    "==",
                    "LLAM_LEASE_STOPPING",
                )
                if positive_transition and function.name == "llam_legacy_lease_rollback_helper_locked":
                    # STOPPING is a positive branch in rollback.  It is
                    # meaningful only when the write is inside that branch;
                    # for the normal transfer branch the exact failure guard
                    # against a non-transfer state is the proof instead.
                    positive_ok = any(
                        record[1] <= index < record[2]
                        and record[3] <= member_index < record[4]
                        for record in guard_records
                    ) or any(
                        tuple(tokens[item].value for item in range(index, index + width))
                        == (
                            "lease",
                            "->",
                            "state",
                            "!=",
                            "LLAM_LEASE_TRANSFER_TO_HELPER",
                        )
                        and in_returning_guard(index)
                        for index in range(begin, max(begin, member_index - 5))
                    )
                    if not positive_ok:
                        continue
                if (
                    "=" not in pattern
                    and not positive_transition
                    and not (
                        strict_failure_guard(index, pattern)
                        if function.name
                        != "llam_legacy_lease_complete_handback_locked"
                        else in_returning_guard(index)
                    )
                ):
                    continue
                return True
        return False

    def has_call(name: str, arguments: tuple[tuple[str, ...], ...]) -> bool:
        for call in _call_sites(tokens):
            if call.name != name or not begin <= call.name_index < member_index:
                continue
            if _inside_constant_false_block(tokens, call.name_index, begin, end):
                continue
            if body_depth(call.name_index) != 1:
                continue
            if len(call.args) != len(arguments):
                continue
            if all(_arg_values(tokens, actual) == expected for actual, expected in zip(call.args, arguments)):
                if name == "llam_worker_binding_live" and not (
                    strict_failure_guard(
                        call.name_index,
                        (
                            "!",
                            "llam_worker_binding_live",
                            "(",
                            *arguments[0],
                            ",",
                            *arguments[1],
                            ")",
                        ),
                    )
                    if function.name
                    != "llam_legacy_lease_complete_handback_locked"
                    else in_returning_guard(call.name_index)
                ):
                    continue
                return True
        return False

    # An owner write is authorized by a particular coordinator transition, not
    # by the presence of a marker or an arbitrary live-worker call.  The
    # required comparisons below intentionally mirror the native helpers.
    common_owner_guard = ("lease", "->", "owner", "!=", "NULL")
    if not has_pattern(
        common_owner_guard,
        allow_nested=function.name == "llam_legacy_lease_rollback_helper_locked",
    ):
        return False

    if function.name == "llam_legacy_lease_grant_helper_locked":
        return (
            has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_TRANSFER_TO_HELPER"))
            and has_pattern(("lease", "->", "transfer_sequence", "!=", "sequence"))
            and has_pattern(("helper", "!=", "&", "shard", "->", "opaque_helper_worker"))
            and has_pattern(("helper", "->", "role", "!=", "LLAM_WORKER_ROLE_OPAQUE_HELPER"))
            and has_pattern(("helper", "->", "compat_shard", "!=", "shard"))
            and has_pattern(("lease", "->", "state", "==", "LLAM_LEASE_STOPPING"))
            and has_call("llam_worker_binding_live", (("helper",), ("incarnation",)))
            and has_call(
                "atomic_load_explicit",
                (("&", "helper", "->", "incarnation"), ("memory_order_acquire",)),
            )
        )
    if function.name == "llam_legacy_lease_complete_handback_locked":
        return (
            has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_TRANSFER_TO_PRIMARY"))
            and has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_STOPPING"))
            and has_pattern(("lease", "->", "transfer_sequence", "!=", "sequence"))
            and has_pattern(("worker", "->", "compat_shard", "!=", "shard"))
            and has_pattern(("claimant", "=", "lease", "->", "return_worker"))
            and has_pattern(("claimant_incarnation", "=", "lease", "->", "return_incarnation"))
            and has_pattern(("claimant", "!=", "worker"))
            and has_call("llam_worker_binding_live", (("claimant",), ("claimant_incarnation",)))
        )
    if function.name == "llam_legacy_lease_reacquire_primary_locked":
        return (
            has_pattern(("worker", "!=", "&", "shard", "->", "primary_worker"))
            and has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_REDIRECTED_NO_EXECUTOR"))
            and has_pattern(("lease", "->", "owner", "!=", "NULL"))
            and has_pattern(("lease", "->", "return_worker", "!=", "NULL"))
            and has_call("llam_worker_binding_live", (("worker",), ("incarnation",)))
        )
    if function.name == "llam_legacy_lease_rollback_helper_locked":
        return (
            has_pattern(("primary", "!=", "&", "shard", "->", "primary_worker"))
            and has_pattern(("primary", "->", "role", "!=", "LLAM_WORKER_ROLE_PRIMARY"))
            and has_pattern(("primary", "->", "compat_shard", "!=", "shard"))
            and has_pattern(("lease", "->", "transfer_sequence", "!=", "sequence"))
            and has_pattern(("lease", "->", "return_worker", "!=", "NULL"))
            and has_pattern(("lease", "->", "return_incarnation", "!=", "0", "U"))
            and has_pattern(("lease", "->", "state", "==", "LLAM_LEASE_STOPPING"))
            and has_call("llam_worker_binding_live", (("primary",), ("incarnation",)))
        )
    return False


def _owner_clear_proof_before(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    member_index: int,
) -> bool:
    """Require the exact guard for an audited owner-clearing transition."""

    begin, end = _token_range_for_span(masked, function)
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return False

    def body_depth(token_index: int) -> int:
        depth = 0
        for index in range(body_open, token_index):
            if tokens[index].value == "{":
                depth += 1
            elif tokens[index].value == "}":
                depth -= 1
        return depth

    guard_records = _control_flow_records(tokens, begin, end)

    def in_returning_guard(token_index: int) -> bool:
        return any(
            record[1] <= token_index < record[2]
            and _has_unconditional_false_return(
                tokens,
                guard_records,
                record[3],
                record[4],
            )
            for record in guard_records
        )

    def top_level_parts(start: int, finish: int) -> list[tuple[int, int]]:
        parts: list[tuple[int, int]] = []
        part_start = start
        paren_depth = bracket_depth = brace_depth = 0
        for index in range(start, finish):
            value = tokens[index].value
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
            elif (
                value == "||"
                and paren_depth == 0
                and bracket_depth == 0
                and brace_depth == 0
            ):
                parts.append((part_start, index))
                part_start = index + 1
        parts.append((part_start, finish))
        return parts

    def strict_failure_guard(
        token_index: int, expected: tuple[str, ...]
    ) -> bool:
        for record in guard_records:
            if not record[1] <= token_index < record[2]:
                continue
            if record[4] > member_index or not _has_unconditional_false_return(
                tokens, guard_records, record[3], record[4]
            ):
                continue
            condition_values = tuple(
                tokens[index].value for index in range(record[1], record[2])
            )
            allowed_identifiers = {
                "lease",
                "owner",
                "owner_incarnation",
                "state",
                "transfer_sequence",
                "sequence",
                "worker",
                "return_worker",
                "return_incarnation",
                "incarnation",
                "shard",
                "NULL",
                "U",
                "llam_worker_binding_live",
                "llam_legacy_lease_merge_paused",
                "llam_legacy_lease_runtime_stopping",
            }
            if any(
                re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value)
                and value not in allowed_identifiers
                and not value.startswith("LLAM_")
                for value in condition_values
            ):
                continue
            for part_start, part_end in top_level_parts(record[1], record[2]):
                part = tuple(tokens[index].value for index in range(part_start, part_end))
                if part == expected:
                    return True
                if (
                    function.name == "llam_legacy_lease_enter_redirect_locked"
                    and part
                    == (
                        "(",
                        "lease",
                        "->",
                        "state",
                        "!=",
                        "LLAM_LEASE_PRIMARY_OWNED",
                        "&&",
                        "lease",
                        "->",
                        "state",
                        "!=",
                        "LLAM_LEASE_HELPER_OWNED",
                        "&&",
                        "lease",
                        "->",
                        "state",
                        "!=",
                        "LLAM_LEASE_TRANSFER_TO_HELPER",
                        ")",
                    )
                    and expected
                    in (
                        (
                            "lease",
                            "->",
                            "state",
                            "!=",
                            "LLAM_LEASE_PRIMARY_OWNED",
                        ),
                        (
                            "lease",
                            "->",
                            "state",
                            "!=",
                            "LLAM_LEASE_HELPER_OWNED",
                        ),
                        (
                            "lease",
                            "->",
                            "state",
                            "!=",
                            "LLAM_LEASE_TRANSFER_TO_HELPER",
                        ),
                    )
                ):
                    return True
        return False

    def has_pattern(pattern: tuple[str, ...]) -> bool:
        width = len(pattern)
        for index in range(begin, max(begin, member_index - width + 1)):
            if _inside_constant_false_block(tokens, index, begin, end):
                continue
            if body_depth(index) != 1:
                continue
            if tuple(tokens[item].value for item in range(index, index + width)) == pattern:
                if not strict_failure_guard(index, pattern):
                    continue
                return True
        return False

    def has_call(name: str, arguments: tuple[tuple[str, ...], ...]) -> bool:
        for call in _call_sites(tokens):
            if call.name != name or not begin <= call.name_index < member_index:
                continue
            if _inside_constant_false_block(tokens, call.name_index, begin, end):
                continue
            if body_depth(call.name_index) != 1:
                continue
            if len(call.args) == len(arguments) and all(
                _arg_values(tokens, actual) == expected
                for actual, expected in zip(call.args, arguments)
            ):
                if name == "llam_worker_binding_live" and not strict_failure_guard(
                    call.name_index,
                    (
                        "!",
                        "llam_worker_binding_live",
                        "(",
                        *arguments[0],
                        ",",
                        *arguments[1],
                        ")",
                    ),
                ):
                    continue
                return True
        return False

    if not _direct_lease_origin(masked, tokens, function, "lease", member_index):
        return False
    if function.name == "llam_legacy_lease_enter_redirect_locked":
        return (
            has_pattern(("lease", "->", "owner", "!=", "return_worker"))
            and has_pattern(("lease", "->", "owner_incarnation", "!=", "return_incarnation"))
            and has_pattern(("lease", "->", "state", "==", "LLAM_LEASE_STOPPING"))
            and has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_PRIMARY_OWNED"))
            and has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_HELPER_OWNED"))
            and has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_TRANSFER_TO_HELPER"))
            and has_call("llam_worker_binding_live", (("return_worker",), ("return_incarnation",)))
        )
    if function.name == "llam_legacy_lease_clear_owner_locked":
        return (
            has_pattern(("lease", "->", "state", "!=", "LLAM_LEASE_TRANSFER_TO_HELPER"))
            and has_pattern(("lease", "->", "transfer_sequence", "!=", "sequence"))
            and has_pattern(("lease", "->", "owner", "!=", "worker"))
            and has_pattern(("lease", "->", "owner_incarnation", "!=", "incarnation"))
            and has_call("llam_worker_binding_live", (("worker",), ("incarnation",)))
        )
    return False


def _unbind_owner_clear_proof(
    masked: str,
    tokens: Sequence[phase0._Token],
    function: _FunctionSpan,
    member_index: int,
) -> bool:
    """Recognize the scheduler identity teardown owner-clear guard."""

    if function.name != "llam_scheduler_unbind_identity":
        return False
    begin, end = _token_range_for_span(masked, function)
    body_open = next(
        (index for index in range(begin, end) if tokens[index].value == "{"),
        None,
    )
    if body_open is None:
        return False
    patterns = (
        ("shard", "->", "legacy_lease", ".", "state", "!=", "LLAM_LEASE_STOPPING"),
        ("shard", "->", "legacy_lease", ".", "owner", "==", "worker"),
        (
            "shard",
            "->",
            "legacy_lease",
            ".",
            "owner_incarnation",
            "==",
            "incarnation",
        ),
    )
    guards = _control_flow_records(tokens, begin, end)
    for guard in guards:
        if not guard[1] <= member_index < guard[4]:
            continue
        condition_values = tuple(tokens[index].value for index in range(guard[1], guard[2]))
        expected_condition = (
            *patterns[0],
            "&&",
            *patterns[1],
            "&&",
            *patterns[2],
        )
        if condition_values != expected_condition:
            continue
        if _inside_constant_false_block(tokens, guard[0], begin, end):
            continue
        # The owner clear must be the direct body statement governed by the
        # same visible positive tuple condition; a marker expression followed
        # by an unconditional clear is not a proof.
        if not guard[3] <= member_index < guard[4]:
            continue
        # The caller supplies the member token, so inspect the operator and
        # value directly instead of relying on spacing/source text.
        if (
            member_index + 2 >= end
            or tokens[member_index + 1].value != "="
            or _scalar_value(
                tokens,
                tuple(
                    index
                    for index in range(member_index + 2, min(member_index + 4, end))
                    if tokens[index].value != ";"
                ),
            )
            not in {"NULL", "0", "0U", "false"}
        ):
            continue
        return True
    return False


def _owner_assignment_errors(
    masked: str, relative_path: str | None = None
) -> list[tuple[int, str]]:
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

    # No object-like macro is part of the canonical owner API.  A macro can
    # hide a whole owner/null-store statement without leaving a member token,
    # so audited scheduler source rejects preprocessor definitions outright.
    if relative_path is not None and pathlib.PurePath(relative_path).as_posix() == (
        "src/core/sched/scheduler.c"
    ):
        for line, raw in enumerate(masked.splitlines(), start=1):
            if re.match(r"^\s*#\s*define\b", raw):
                fail(line)

    # C designated initializers are not part of the audited coordinator API.
    # In particular, an unrelated local ``{ .owner = ... }`` must not be
    # mistaken for the direct lease transitions below.  The exact init and
    # finalize helpers are the only names reserved for future audited
    # initializer forms.
    for index, token in enumerate(tokens[:-2]):
        if token.value != "." or tokens[index + 1].value not in {
            "owner",
            "owner_incarnation",
        } or tokens[index + 2].value != "=":
            continue
        if index == 0 or tokens[index - 1].value not in {"{", ","}:
            continue
        function = _function_for_token(masked, tokens, function_spans, index + 1)
        if function is None or function.name not in {
            "llam_legacy_lease_init",
            "llam_legacy_lease_finalize_stop_locked",
        }:
            fail(token.line)

    def conditional_write_is_audited(
        function: _FunctionSpan, begin: int, end: int, member_index: int
    ) -> bool:
        """Return whether a conditional owner-tuple write is canonical."""

        for index in range(begin, member_index):
            if tokens[index].value != "if" or index + 1 >= end:
                continue
            if tokens[index + 1].value != "(":
                continue
            condition_end = _matching_token(tokens, index + 1, "(", ")")
            if condition_end is None or condition_end >= member_index:
                continue
            body_open = condition_end + 1
            if body_open >= end or tokens[body_open].value != "{":
                continue
            body_close = _matching_token(tokens, body_open, "{", "}")
            if body_close is None or not body_open < member_index < body_close:
                continue
            condition = tuple(
                tokens[item].value for item in range(index + 2, condition_end)
            )
            if (
                function.name == "llam_legacy_lease_rollback_helper_locked"
                and condition == ("lease", "->", "state", "==", "LLAM_LEASE_STOPPING")
            ):
                continue
            return False
        return True

    # Transition helpers are deliberately closed: an unrecognized call can
    # hide an owner-slot write or alter the proof state without leaving a
    # member token for the lexical audit to inspect.
    for function in function_spans:
        if function.name not in LEASE_OWNER_WRITE_HELPERS | LEASE_OWNER_NULL_WRITE_HELPERS:
            continue
        begin, end = _token_range_for_span(masked, function)
        body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            end,
        )
        parameters = _function_parameter_names(masked, tokens, function)
        if body_open >= end or _parameter_has_escape_or_rebind(
            tokens, parameters, body_open, end
        ):
            # Owner transitions/clears are tied to the exact shard, claimant,
            # sequence, and incarnation supplied by the caller.  Rebinding or
            # escaping any parameter makes the later direct-looking owner
            # projection unverifiable.
            fail(function.start_line)
        # No owner transition may write through an arbitrary pointer slot.
        # Check both prefix unary forms (``++*slot``/``*slot++``) and ordinary
        # assignment/compound forms; relying on an earlier ``&lease->owner``
        # discovery would miss a named slot parameter with no visible member.
        for index in range(body_open + 1, end - 1):
            if tokens[index].value != "*" or not re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 1].value
            ):
                continue
            next_value = tokens[index + 2].value if index + 2 < end else None
            previous_value = tokens[index - 1].value if index > body_open + 1 else None
            if (
                next_value == "="
                and previous_value is not None
                and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", previous_value)
            ):
                # ``type *lease = ...`` is the permitted direct declaration
                # initializer.  A write through an actual slot has a
                # statement separator/unary token immediately before ``*``.
                continue
            if (
                next_value in ASSIGNMENT_OPERATORS | {"++", "--"}
                or previous_value in {"++", "--"}
            ):
                fail(tokens[index].line)
            if next_value == "(":
                # ``*(slot + offset) =`` is an indirect owner mutation even
                # though the dereference is not followed by a plain name.
                fail(tokens[index].line)
        for index in range(body_open + 1, end - 1):
            if tokens[index].value != "[":
                continue
            statement_start, statement_end = _statement_bounds(
                tokens, index, body_open + 1, end
            )
            if any(
                tokens[item].value in ASSIGNMENT_OPERATORS
                for item in range(index, statement_end)
            ):
                # Indexed lvalues are not part of the owner coordinator API;
                # this catches ``slot[0] = NULL`` and array-backed aliases.
                fail(tokens[index].line)
        for index in range(body_open + 1, end - 4):
            if (
                tokens[index].value == "("
                and tokens[index + 1].value == "*"
                and re.fullmatch(
                    r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 2].value
                )
                and tokens[index + 3].value == ")"
                and tokens[index + 4].value == "("
            ):
                # Function-pointer calls have no auditable owner proof.
                fail(tokens[index + 2].line)
        # The lease local itself is a single direct coordinator origin.  Do
        # not let a later assignment, parenthesized lvalue, address alias, or
        # indexed/pointer movement retain a lease-shaped owner projection.
        lease_origin_seen = False
        for index in range(body_open + 1, end):
            if tokens[index].value != "lease":
                continue
            following = tokens[index + 1].value if index + 1 < end else None
            previous = tokens[index - 1].value if index > body_open + 1 else None
            if following in {"->", "."}:
                continue
            addressed = _addressed_identifier_at(tokens, index - 1, end) if index > 0 else None
            if addressed is not None and addressed[0] == "lease":
                if addressed[1] >= end or tokens[addressed[1]].value not in {"->", "."}:
                    fail(tokens[index].line)
                    continue
            if following in ASSIGNMENT_OPERATORS:
                statement_end = _owner_statement_end(tokens, index + 2, end)
                rhs = tuple(tokens[item].value for item in range(index + 2, statement_end))
                direct_origin = rhs in {
                    ("llam_shard_legacy_lease_locked", "(", "shard", ")"),
                    ("llam_shard_legacy_lease_const_locked", "(", "shard", ")"),
                    ("&", "shard", "->", "legacy_lease"),
                }
                if not lease_origin_seen and following == "=" and direct_origin:
                    lease_origin_seen = True
                else:
                    fail(tokens[index].line)
                continue
            if previous in {"++", "--", "+", "-"} or following in {
                "++",
                "--",
                "[",
                "+",
                "-",
            }:
                fail(tokens[index].line)
                continue
            # A parenthesized/cast/comma lvalue hides the assignment operator
            # from the immediate-token test above.  Any assignment later in
            # this statement makes the lease an unsupported lvalue.
            statement_start, statement_end = _statement_bounds(
                tokens, index, body_open + 1, end
            )
            if any(
                tokens[item].value in ASSIGNMENT_OPERATORS and item > index
                for item in range(statement_start, statement_end)
            ):
                fail(tokens[index].line)
        definition_indices = {
            index
            for index in range(begin, body_open)
            if tokens[index].value == function.name
            and index + 1 < body_open
            and tokens[index + 1].value == "("
        }
        for call in calls:
            if not begin <= call.name_index < end or call.name_index in definition_indices:
                continue
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", call.name):
                continue
            allowed_calls = (
                LEASE_OWNER_NULL_ALLOWED_CALLS
                if function.name in LEASE_OWNER_NULL_WRITE_HELPERS
                else LEASE_OWNER_ALLOWED_CALLS
            )
            if call.name in allowed_calls or call.name in CONTROL_FLOW_NAMES:
                continue
            fail(tokens[call.name_index].line)

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
        if member not in {"owner", "owner_incarnation"}:
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

        if transition or null_transition:
            # Reject an owner member used as a non-canonical lvalue even when
            # parentheses, casts, or comma operators separate the member from
            # its assignment operator.  The direct ``lease->owner =`` form is
            # left to the transition-proof checks below.
            statement_start, statement_end = _statement_bounds(
                tokens, member_index, begin, end
            )
            direct_owner_lvalue = (
                receiver in {"lease", "legacy_lease"}
                and operator > begin
                and tokens[operator - 1].value == receiver
                and member_index + 1 < end
                and tokens[member_index + 1].value == "="
            )
            has_lvalue_operator = any(
                tokens[item].value in ASSIGNMENT_OPERATORS
                and item > member_index
                for item in range(member_index + 1, statement_end)
            )
            has_unary_operator = any(
                tokens[item].value in {"++", "--"}
                for item in range(statement_start, statement_end)
            )
            if (has_lvalue_operator and not direct_owner_lvalue) or has_unary_operator:
                fail(line)
                continue

        if transition or null_transition:
            # Resolve the lease pointer once.  Pointer arithmetic, indexing,
            # rebinding, or address escape after that origin invalidates every
            # subsequent owner/tuple proof, even if the final member spelling
            # is still ``lease->owner``.
            body_open = next(
                (item for item in range(begin, end) if tokens[item].value == "{"),
                begin,
            )
            for index in range(body_open + 1, member_index):
                if tokens[index].value != "lease":
                    continue
                previous = tokens[index - 1].value if index > body_open + 1 else None
                following = tokens[index + 1].value if index + 1 < end else None
                if previous in {"&", "++", "--", "+", "-"} or following in {
                    "++",
                    "--",
                    "+=",
                    "-=",
                    "*=",
                    "/=",
                    "%=",
                    "&=",
                    "|=",
                    "^=",
                    "[",
                    "+",
                    "-",
                }:
                    # The declaration/split assignment that establishes the
                    # canonical origin is the sole permitted ``lease =``.
                    if following == "=" and index > body_open + 1:
                        statement_end = _owner_statement_end(tokens, index + 1, end)
                        rhs = [tokens[item].value for item in range(index + 2, statement_end)]
                        if rhs == ["llam_shard_legacy_lease_locked", "(", "shard", ")"]:
                            continue
                    fail(tokens[index].line)
                    break

        # A lease pointer must be used as the exact local object.  Indexing,
        # pointer arithmetic, or a moved receiver is intentionally unsupported.
        direct_receiver = (
            operator > begin
            and tokens[operator - 1].value == receiver
            and receiver in {"lease", "legacy_lease"}
        )
        if receiver_is_local_lease and not direct_receiver:
            fail(line)
            continue
        if receiver_is_direct_shard_lease and not direct_receiver:
            fail(line)
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

        if (
            direct_assignment
            and (transition or (null_transition and receiver_is_local_lease))
            and not conditional_write_is_audited(function, begin, end, member_index)
        ):
            fail(line)
            continue

        if member == "owner_incarnation":
            expected_incarnation = LEASE_OWNER_INCARNATION_VALUE_NAMES.get(function.name)
            if direct_assignment:
                rhs = [tokens[index].value for index in range(member_index + 2, statement_end)]
                zero_value = rhs in (["NULL"], ["0"], ["0", "U"], ["false"])
                if next_value != "=" or (
                    expected_incarnation is None and not zero_value
                ):
                    fail(line)
                    continue
                if expected_incarnation is not None and rhs == [expected_incarnation]:
                    if function.name in LEASE_OWNER_WRITE_HELPERS:
                        owner_pattern = (
                            "lease",
                            "->",
                            "owner",
                            "=",
                            LEASE_OWNER_VALUE_NAMES[function.name],
                        )
                        owner_written = any(
                            tuple(tokens[index + offset].value for offset in range(len(owner_pattern)))
                            == owner_pattern
                            and index < member_index
                            for index in range(begin, max(begin, member_index - len(owner_pattern) + 1))
                        )
                        if not owner_written or not _owner_proof_before(
                            masked,
                            tokens,
                            function,
                            member_index,
                            LEASE_OWNER_VALUE_NAMES[function.name],
                        ):
                            fail(line)
                    else:
                        fail(line)
                    continue
                if zero_value:
                    if function.name in {
                        "llam_legacy_lease_init",
                        "llam_legacy_lease_finalize_stop_locked",
                    }:
                        allowed_tuple_clear = _direct_lease_origin(
                            masked, tokens, function, "lease", member_index
                        )
                    elif function.name in LEASE_OWNER_NULL_WRITE_HELPERS:
                        allowed_tuple_clear = (
                            _unbind_owner_clear_proof(masked, tokens, function, member_index)
                            if function.name == "llam_scheduler_unbind_identity"
                            else _owner_clear_proof_before(
                                masked, tokens, function, member_index
                            )
                        )
                    else:
                        allowed_tuple_clear = False
                    if not allowed_tuple_clear:
                        fail(line)
                    continue
                fail(line)
                continue
            if unary_mutation:
                fail(line)
                continue
            # Reads of the incarnation projection are descriptive; only a
            # direct write reaches this strict tuple branch.
            continue

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
                if function.name in {
                    "llam_legacy_lease_init",
                    "llam_legacy_lease_finalize_stop_locked",
                }:
                    allowed_clear = _direct_lease_origin(
                        masked, tokens, function, "lease", member_index
                    )
                else:
                    allowed_clear = _owner_clear_proof_before(
                        masked, tokens, function, member_index
                    )
                if not allowed_clear:
                    fail(line)
                continue
            if (
                receiver_is_direct_shard_lease
                and function.name == "llam_scheduler_unbind_identity"
                and value in {"NULL", "0", "0U", "false"}
            ):
                if not _unbind_owner_clear_proof(masked, tokens, function, member_index):
                    fail(line)
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
                and function.name in LEASE_OWNER_WRITE_HELPERS | LEASE_OWNER_NULL_WRITE_HELPERS
                and not declaration
            ):
                fail(token.line)

    # Unary/compound writes through an owner slot are just as much an
    # indirect owner mutation as ``*slot = value``.  Normalize an optional
    # parenthesis after the dereference and reject every write operator.
    for index, token in enumerate(tokens):
        if token.value != "*" or index + 1 >= len(tokens):
            continue
        slot = tokens[index + 1].value
        if slot not in owner_slot_names:
            continue
        operator_index = index + 2
        if operator_index < len(tokens) and tokens[operator_index].value == ")":
            operator_index += 1
        if operator_index >= len(tokens):
            continue
        if tokens[operator_index].value in {
            "++",
            "--",
            "=",
            "+=",
            "-=",
            "*=",
            "/=",
            "%=",
            "&=",
            "|=",
            "^=",
        }:
            fail(tokens[operator_index].line)

    # A transition helper cannot pass the local lease pointer to an unknown
    # sink.  The named helper allowlist above catches calls that mention an
    # owner member; this catches a plain ``sink(lease)`` before the member
    # walk has any receiver to inspect.
    for function in function_spans:
        if function.name not in LEASE_OWNER_WRITE_HELPERS | LEASE_OWNER_NULL_WRITE_HELPERS:
            continue
        begin, end = _token_range_for_span(masked, function)
        for call in calls:
            if not begin <= call.name_index < end or call.name in CONTROL_FLOW_NAMES:
                continue
            if any(_simple_identifier(tokens, argument) == "lease" for argument in call.args):
                if call.name not in LEASE_OWNER_ALLOWED_CALLS:
                    fail(tokens[call.name_index].line)
    for call in calls:
        if call.name in ATOMIC_STORE_NAMES and call.args:
            alias = _simple_identifier(tokens, call.args[0])
            function = _function_for_token(masked, tokens, function_spans, call.name_index)
            if alias in owner_slot_names or (
                function is not None
                and function.name in LEASE_OWNER_WRITE_HELPERS | LEASE_OWNER_NULL_WRITE_HELPERS
                and alias is not None
                and len(call.args) > 1
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
    strict_scheduler_path = pathlib.PurePath(relative_path).as_posix().startswith(
        ("src/core/sched/", "src/engine/watchdog/")
    )
    local_lock_wrappers: set[str] = set()
    for candidate in functions:
        cbegin, cend = _token_range_for_span(masked, candidate)
        cbody_open = next(
            (index for index in range(cbegin, cend) if tokens[index].value == "{"),
            cend,
        )
        if any(
            cbody_open < call.name_index < cend and call.name in lock_names
            for call in calls
        ) and not candidate.name.startswith("llam_") and candidate.name != "defined":
            local_lock_wrappers.add(candidate.name)

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
        if len(receiver_values) == 1 and re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*", receiver_values[0]
        ):
            return receiver_values[0], field, True
        # The only non-scalar receiver accepted by the lock audit is the
        # canonical runtime shard projection.  Parenthesized/dereferenced
        # pointers and casts are opaque acquisition channels.
        if (
            len(receiver_values) == 6
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver_values[0])
            and receiver_values[1] in {"->", "."}
            and receiver_values[2] == "shards"
            and receiver_values[3] == "["
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver_values[4])
            and receiver_values[5] == "]"
        ):
            return "".join(receiver_values), field, True
        return None, field, True

    def noncanonical_indexed_shard_receiver(call: _CallSite) -> bool:
        """Reject shard-array lock receivers whose index is not one name.

        Expressions such as ``i + 0`` and ``j + 0`` are semantically dynamic
        aliases, but a lexical audit cannot establish that relationship (or
        their ordering) safely.  The production form uses a single visible
        loop/index identifier, so every other index is fail-closed.
        """

        if not call.args:
            return False
        values = list(_arg_values(tokens, call.args[0]))
        if values and values[0] == "&":
            values = values[1:]
        if "shards" not in values or "[" not in values:
            return False
        try:
            opening = values.index("[", values.index("shards") + 1)
        except ValueError:
            return False
        depth = 0
        closing = None
        for index in range(opening, len(values)):
            if values[index] == "[":
                depth += 1
            elif values[index] == "]":
                depth -= 1
                if depth == 0:
                    closing = index
                    break
        if closing is None or closing + 2 >= len(values):
            return False
        field = values[-1]
        if field not in {"opaque_lock", "lock"}:
            return False
        index_values = values[opening + 1 : closing]
        return not (
            len(index_values) == 1
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", index_values[0])
        )

    def indexed_receiver_key(
        call: _CallSite,
    ) -> tuple[str, str, str] | None:
        """Recognize one canonical ``runtime->shards[index]`` receiver.

        The lexical audit can safely compare two indexed receivers only when
        both their runtime and index expressions are visible identifiers.  Any
        other shape remains opaque and is rejected by the caller.
        """

        if not call.args:
            return None
        values = list(_arg_values(tokens, call.args[0]))
        if values and values[0] == "&":
            values = values[1:]
        if len(values) != 8 or values[1] not in {"->", "."}:
            return None
        if values[2] != "shards" or values[3] != "[" or values[5] != "]":
            return None
        if values[6] not in {"->", "."}:
            return None
        if not all(re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) for value in (values[0], values[4])):
            return None
        return values[0], values[4], values[7]

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

    def temporal_alias_for(function: _FunctionSpan, name: str, before: int) -> str:
        """Resolve one scalar receiver using assignments before a lock call."""

        begin, end = _token_range_for_span(masked, function)
        aliases: dict[str, str] = {}
        for index in range(begin + 1, min(before, end)):
            if tokens[index].value != "=" or index == 0:
                continue
            lhs = tokens[index - 1].value
            rhs = tokens[index + 1].value if index + 1 < end else ""
            if lhs in {"first", "second"}:
                continue
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs):
                continue
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", rhs):
                aliases[lhs] = rhs
            else:
                aliases.pop(lhs, None)
        seen: set[str] = set()
        current = name
        while current in aliases and current not in seen:
            seen.add(current)
            current = aliases[current]
        return current

    def receiver_rebound_after_lock(
        function: _FunctionSpan, receiver: str, first_lock_index: int
    ) -> bool:
        """Reject an alias reassignment after it participated in a lock."""

        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver):
            return False
        begin, end = _token_range_for_span(masked, function)
        if _identifier_has_expression_write_or_move(
            tokens, receiver, first_lock_index + 1, end, pointer=True
        ):
            return True
        for index in range(max(begin + 1, first_lock_index + 1), end):
            if tokens[index].value != receiver:
                continue
            if index + 1 < end and tokens[index + 1].value in ASSIGNMENT_OPERATORS:
                # A later block may redeclare the canonical loop-local
                # pointer (``llam_shard_t *shard = ...``).  That is not a
                # rebind of the already-held receiver; an ordinary
                # ``shard = attacker`` assignment has a statement separator
                # before the name and remains rejected.
                if index > begin and tokens[index - 1].value == "*":
                    continue
                return True
            if index > first_lock_index and tokens[index - 1].value in {"++", "--"}:
                return True
            if index + 1 < end and tokens[index + 1].value in {"++", "--"}:
                return True
        return False

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

    def temporal_role_for(function: _FunctionSpan, name: str, before: int) -> str | None:
        """Resolve a first/second role from assignments before one lock."""

        begin, end = _token_range_for_span(masked, function)
        roles = {"first": "first", "second": "second"}
        for index in range(begin + 1, min(before, end)):
            if tokens[index].value != "=" or index == 0:
                continue
            lhs = tokens[index - 1].value
            rhs = tokens[index + 1].value if index + 1 < end else ""
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", lhs):
                continue
            if rhs in roles:
                roles[lhs] = roles[rhs]
            elif lhs not in {"first", "second"}:
                roles.pop(lhs, None)
        return roles.get(name)

    for function in functions:
        begin, end = _token_range_for_span(masked, function)
        function_body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            begin,
        )
        aliases = aliases_for(function)
        roles = role_aliases_for(function)
        held: dict[str, dict[str, bool]] = {}
        indexed_receivers: dict[str, set[str]] = {}
        held_opaque_receivers: set[str] = set()
        held_opaque_roles: set[str] = set()
        held_lock_roles: set[str] = set()
        first_lock_by_receiver: dict[str, int] = {}
        first_index_lock: dict[tuple[str, str], int] = {}
        function_calls = sorted(
            (call for call in calls if begin <= call.name_index < end),
            key=lambda call: call.name_index,
        )
        function_has_opaque_lock = any(
            direct_receiver_for_lock(call)[1] == "opaque_lock"
            for call in function_calls
        )
        lock_receivers = {
            direct_receiver_for_lock(call)[0]
            for call in function_calls
            if direct_receiver_for_lock(call)[0] is not None
        }
        mixed_receiver_protocol = function_has_opaque_lock and len(lock_receivers) > 1
        for call in function_calls:
            if noncanonical_indexed_shard_receiver(call):
                fail(tokens[call.name_index].line)
            receiver, which, field_mentioned = direct_receiver_for_lock(call)
            if receiver is None or which is None:
                if field_mentioned and strict_scheduler_path:
                    fail(tokens[call.name_index].line)
                continue
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", receiver):
                first_lock_by_receiver.setdefault(receiver, call.name_index)
                if receiver_rebound_after_lock(
                    function, receiver, first_lock_by_receiver[receiver]
                ):
                    fail(tokens[call.name_index].line)
            indexed = indexed_receiver_key(call)
            if indexed is not None:
                runtime_name, index_name, _field = indexed
                index_key = (runtime_name, index_name)
                prior_index_lock = first_index_lock.setdefault(
                    index_key, call.name_index
                )
                # Indexed lock receivers are stable only for the exact
                # acquisition pair.  A grouped/cast/comma assignment or
                # unary movement between the opaque and shard locks changes
                # the object selected by the second call while its spelling
                # remains ``rt->shards[index]``.
                if _identifier_has_expression_write_or_move(
                    tokens,
                    index_name,
                    prior_index_lock + 1,
                    call.name_index,
                ) or _identifier_has_expression_write_or_move(
                    tokens,
                    runtime_name,
                    prior_index_lock + 1,
                    call.name_index,
                    pointer=True,
                ):
                    fail(tokens[call.name_index].line)
                resolved_runtime = temporal_alias_for(
                    function, runtime_name, call.name_index
                )
                resolved_index = temporal_alias_for(
                    function, index_name, call.name_index
                )
                prior_indices = indexed_receivers.setdefault(runtime_name, set())
                prior_indices.add(resolved_index)
                if len(prior_indices) > 1:
                    # There is no lexical proof that two different indexed
                    # receivers are the same shard.  Refuse cross-shard order
                    # rather than silently treating a dynamic index as one
                    # lock object.
                    fail(tokens[call.name_index].line)
                if call.name == "pthread_mutex_unlock" and _field == "opaque_lock":
                    # A later lock in the same function may reuse the loop
                    # index for a different object after this opaque lock is
                    # released.  Do not carry the earlier acquisition's
                    # temporal interval across that release.
                    first_index_lock.pop(index_key, None)
            # Resolve aliases at this source position, not from their final
            # function-wide value.  A receiver reassigned between opaque and
            # shard locks must not inherit the first object's held state.
            symbol = temporal_alias_for(function, receiver, call.name_index)
            if receiver in aliases:
                symbol = temporal_alias_for(function, receiver, call.name_index)
            if indexed is not None:
                # Keep the index provenance in the held-lock key.  Two locks
                # using the same lexical receiver but different values of a
                # rebound index must not share state.
                symbol = f"{resolved_runtime}->shards[{resolved_index}]"
            state = held.setdefault(symbol, {"opaque_lock": False, "lock": False})
            role = temporal_role_for(function, receiver, call.name_index) or roles.get(symbol)
            if call.name == "pthread_mutex_unlock":
                state[which] = False
                if which == "opaque_lock":
                    held_opaque_receivers.discard(symbol)
                if which == "opaque_lock" and role is not None:
                    held_opaque_roles.discard(role)
                if which == "lock" and role is not None:
                    held_lock_roles.discard(role)
                continue
            if which == "opaque_lock" and state["lock"]:
                fail(tokens[call.name_index].line)
            if which == "lock" and mixed_receiver_protocol and not state["opaque_lock"]:
                # Once a function participates in the opaque/shard lock
                # protocol, every direct shard lock must have its matching
                # opaque lock held first.  Tracking only a global
                # ``opaque-before-any-lock`` order lets ``a->opaque_lock`` be
                # paired incorrectly with ``b->lock``.
                fail(tokens[call.name_index].line)
            if which == "opaque_lock":
                # More than one dynamic receiver is safe only when the
                # canonical first/second role ordering is visible.  Unknown
                # receiver pairs (for example shard/target) are refused
                # instead of guessing a global shard-ID order.
                if held_opaque_receivers and symbol not in held_opaque_receivers:
                    if role not in {"first", "second"} or any(
                        held_role not in {"first", "second"}
                        for held_role in held_opaque_roles
                    ):
                        fail(tokens[call.name_index].line)
            if which == "opaque_lock" and role in {"first", "second"}:
                if role == "first" and "second" in held_opaque_roles:
                    fail(tokens[call.name_index].line)
                if held_lock_roles:
                    # A two-shard merge must acquire both opaque locks before
                    # either shard lock, not just before its matching shard.
                    fail(tokens[call.name_index].line)
            state[which] = True
            if which == "opaque_lock":
                held_opaque_receivers.add(symbol)
            if which == "opaque_lock" and role in {"first", "second"}:
                held_opaque_roles.add(role)
            if which == "lock" and role in {"first", "second"}:
                held_lock_roles.add(role)

        # Obvious lock wrappers/macros cannot be proven by lexical receiver
        # tracking.  Reject them in scheduler/watchdog source rather than
        # silently accepting a hidden acquisition.
        if strict_scheduler_path:
            function_pointer_names: set[str] = set()
            for index in range(begin, end - 3):
                if (
                    tokens[index].value == "*"
                    and re.fullmatch(
                        r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 1].value
                    )
                    and tokens[index + 2].value == ")"
                    and tokens[index + 3].value == "("
                ):
                    function_pointer_names.add(tokens[index + 1].value)
                if (
                    tokens[index].value == "("
                    and index + 4 < end
                    and tokens[index + 1].value == "*"
                    and re.fullmatch(
                        r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 2].value
                    )
                    and tokens[index + 3].value == ")"
                    and tokens[index + 4].value == "("
                ):
                    function_pointer_names.add(tokens[index + 2].value)
                if (
                    index > function_body_open
                    and tokens[index].value == "("
                    and tokens[index + 1].value == "*"
                    and re.fullmatch(
                        r"[A-Za-z_][A-Za-z0-9_]*", tokens[index + 2].value
                    )
                    and tokens[index + 3].value == ")"
                    and index + 4 < end
                    and tokens[index + 4].value == "("
                ):
                    fail(tokens[index + 2].line)
            for call in all_calls:
                if not begin <= call.name_index < end or call.name in lock_names:
                    continue
                if call.name in function_pointer_names and call.open_index > function_body_open:
                    # A function-pointer call has no auditable receiver or
                    # lock ordering contract, regardless of the callee name.
                    fail(tokens[call.name_index].line)
                    continue
                if call.name in local_lock_wrappers and call.open_index > function_body_open:
                    # A locally defined non-LLAM wrapper may acquire either
                    # scheduler lock behind a name that does not reveal the
                    # protocol.  Its body is not an auditable direct lock site.
                    fail(tokens[call.name_index].line)
                    continue
                if call.name in CONTROL_FLOW_NAMES:
                    continue
                lowered = call.name.lower()
                argument_values = [
                    value
                    for argument in call.args
                    for value in _arg_values(tokens, argument)
                ]
                if (
                    "opaque_lock" in argument_values
                    and call.name not in lock_names
                    and call.name not in {"pthread_cond_wait", "pthread_cond_timedwait"}
                ) or (
                    "lock" in argument_values and call.name not in lock_names
                ):
                    # An unknown wrapper or function-pointer call can hide
                    # lock acquisition even when its spelling does not carry
                    # a scheduler-specific prefix.
                    fail(tokens[call.name_index].line)
                    continue
                # A non-LLAM wrapper with an opaque/lock-shaped name and a
                # shard receiver is not an auditable acquisition.  The
                # production source uses direct pthread calls or named LLAM
                # helpers; generic wrappers are intentionally unsupported.
                wrapper_name = (
                    not call.name.startswith("llam_")
                    and (
                        re.search(r"(?:^|_)(?:lock|opaque)(?:_|$)", lowered)
                        or lowered.endswith("_for_shard")
                    )
                    and any(
                        _simple_identifier(tokens, argument)
                        in {"shard", "target", "source", "first", "second"}
                        for argument in call.args
                    )
                )
                if wrapper_name:
                    fail(tokens[call.name_index].line)
                    continue
                if (
                    not call.name.startswith("llam_")
                    and re.search(r"(?:lock|mutex|opaque)", lowered)
                    and any(
                        _simple_identifier(tokens, argument)
                        in {"shard", "first", "second", "source", "target", "a", "b"}
                        for argument in call.args
                    )
                    and call.name not in {
                        "pthread_cond_wait",
                        "pthread_cond_timedwait",
                    }
                ):
                    # Generic mutex/opaque wrappers (including names such as
                    # ``take_mutex``) are not source-auditable.  Reject them
                    # even when the lock field is hidden behind a parameter.
                    fail(tokens[call.name_index].line)
                    continue
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
                    or lowered in {"lock", "lockit", "take_lock", "lock_fn"}
                ):
                    fail(tokens[call.name_index].line)
        # A mutex field may not be converted to a pointer and then handed to
        # an opaque wrapper or function-pointer alias.  The direct pthread
        # spelling is the sole exception; its complete argument is checked by
        # ``direct_receiver_for_lock`` above.  This deliberately rejects
        # pointer-hidden lock acquisition even when the wrapper name carries
        # no lock-specific token.
        scheduler_lock_path = pathlib.PurePath(relative_path).as_posix().startswith(
            ("src/core/sched/", "src/engine/watchdog/")
        )
        for index in range(begin, end):
            if not scheduler_lock_path or tokens[index].value != "&":
                continue
            member_index = None
            for item in range(index + 1, min(end, index + 12)):
                if tokens[item].value in {";", ",", ")", "}"}:
                    break
                if (
                    tokens[item].value in {"lock", "opaque_lock"}
                    and item > index + 1
                    and tokens[item - 1].value in {"->", "."}
                ):
                    member_index = item
                    break
            if member_index is None:
                continue
            direct_call = _call_containing(
                all_calls,
                index,
                lock_names | {"pthread_cond_wait", "pthread_cond_timedwait"},
            )
            if direct_call is None:
                fail(tokens[index].line)
                continue
            if direct_call.name in {"pthread_cond_wait", "pthread_cond_timedwait"}:
                if (
                    len(direct_call.args) >= 2
                    and direct_call.args[1][0] == index
                    and _arg_values(tokens, direct_call.args[1])
                    == ("&", "shard", "->", "opaque_lock")
                ):
                    continue
                fail(tokens[index].line)
                continue
            receiver, field, mentioned = direct_receiver_for_lock(direct_call)
            if (
                not mentioned
                or receiver is None
                or field != tokens[member_index].value
                or not direct_call.args
                or direct_call.args[0][0] != index
                or direct_call.args[0][-1] != member_index
            ):
                fail(tokens[index].line)
    return errors


def _watchdog_errors_legacy(relative_path: str, masked: str) -> list[tuple[int, str]]:
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

    # The canonical signal site has no preprocessor-defined wrapper.  An
    # object-like macro could otherwise hide a snapshot-output write or a
    # control-flow guard from this lexical proof.
    if path == "src/engine/watchdog/watchdog_probe.c":
        for line, raw in enumerate(masked.splitlines(), start=1):
            if re.match(r"^\s*#\s*define\b", raw):
                fail(line)

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


def _watchdog_errors(relative_path: str, masked: str) -> list[tuple[int, str]]:
    """Recognize only the lease-bound watchdog proof shape.

    This source audit intentionally does not attempt to infer arbitrary C
    control flow.  The one native signal site has a small, explicit contract:
    one original tuple snapshot, one validated tuple snapshot, a conjunction
    of all tuple/active-G predicates, and both shard locks held until the
    signal.  Unknown aliases, macros, conditional snapshots, and conditional
    cleanup are rejected.
    """

    path = pathlib.PurePath(relative_path).as_posix()
    if not path.startswith("src/engine/watchdog/"):
        return []
    errors: list[tuple[int, str]] = []
    tokens = phase0._tokens(masked)
    functions = _function_spans(masked)
    calls = _call_sites(tokens)

    def fail(line: int) -> None:
        error = (line, "Phase 1A watchdog target outside lease lifetime")
        if error not in errors:
            errors.append(error)

    for line, _receiver, _kind, member in _member_accesses(tokens):
        if member in WATCHDOG_TARGET_MEMBERS:
            fail(line)

    original_outputs = ("owner", "owner_incarnation", "epoch", "native_thread")
    validated_outputs = (
        "validated_owner",
        "validated_incarnation",
        "validated_epoch",
        "validated_native_thread",
    )

    def body_depth(body_open: int, token_index: int) -> int:
        depth = 0
        for index in range(body_open, token_index):
            if tokens[index].value == "{":
                depth += 1
            elif tokens[index].value == "}":
                depth -= 1
        return depth

    def snapshot_shape(call: _CallSite, outputs: tuple[str, ...]) -> bool:
        if call.name != "llam_legacy_lease_snapshot_watchdog_locked":
            return False
        if len(call.args) != 5 or _simple_identifier(tokens, call.args[0]) != "shard":
            return False
        return tuple(
            _addressed_identifier(tokens, call.args[index]) for index in range(1, 5)
        ) == outputs

    def mutex_receiver(call: _CallSite) -> tuple[str, str] | None:
        if not call.args:
            return None
        values = list(_arg_values(tokens, call.args[0]))
        if values and values[0] == "&":
            values = values[1:]
        if len(values) != 3 or values[1] not in {"->", "."}:
            return None
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", values[0]):
            return None
        if values[2] not in {"opaque_lock", "lock"}:
            return None
        return values[0], values[2]

    def if_records(begin: int, end: int) -> list[tuple[int, int, int, int, int]]:
        result: list[tuple[int, int, int, int, int]] = []
        for index in range(begin, end):
            if tokens[index].value != "if" or index + 1 >= end:
                continue
            if tokens[index + 1].value != "(":
                continue
            condition_end = _matching_token(tokens, index + 1, "(", ")")
            if condition_end is None:
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
            result.append((index, index + 2, condition_end, body_open, body_close))
        return result

    def top_level_parts(start: int, end: int, operator: str) -> list[tuple[int, int]]:
        parts: list[tuple[int, int]] = []
        part_start = start
        paren_depth = bracket_depth = brace_depth = 0
        for index in range(start, end):
            value = tokens[index].value
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
            elif (
                value == operator
                and paren_depth == 0
                and bracket_depth == 0
                and brace_depth == 0
            ):
                parts.append((part_start, index))
                part_start = index + 1
        parts.append((part_start, end))
        return parts

    def values(part: tuple[int, int]) -> tuple[str, ...]:
        return tuple(tokens[index].value for index in range(part[0], part[1]))

    def calls_in(part: tuple[int, int]) -> list[_CallSite]:
        return [
            candidate
            for candidate in calls
            if part[0] <= candidate.name_index < part[1]
        ]

    def has_unconditional_return(body_open: int, body_close: int) -> bool:
        # A failure guard is part of the source-level proof only when its
        # terminating return is the direct statement in a braced body.  An
        # unbraced return (or a nested ``if (flag) return``) can fall through
        # to the validated signal path, so do not treat it as unconditional.
        if (
            body_open >= body_close
            or tokens[body_open].value != "{"
        ):
            return False
        if any(
            body_open < record[3] < record[4] <= body_close
            for record in control_records
        ):
            # A nested conditional/jump inside the failure body is not the
            # canonical direct-return edge, even if another return happens to
            # appear later in the same block.
            return False
        return _has_unconditional_return(
            tokens,
            control_records,
            body_open,
            body_close,
        )

    def canonical_snapshot_failure_guard(
        snapshot: _CallSite,
        outputs: tuple[str, ...],
        records: Sequence[tuple[int, int, int, int, int]],
    ) -> bool:
        for _if_index, start, finish, body_open, body_close in records:
            if not start < snapshot.name_index < finish:
                continue
            part = (start, finish)
            expected_condition = (
                "!",
                *tuple(
                    tokens[index].value
                    for index in range(snapshot.name_index, snapshot.close_index + 1)
                ),
            )
            if values(part) != expected_condition:
                continue
            nested = calls_in((start + 1, finish))
            if len(nested) != 1 or nested[0] != snapshot:
                continue
            if not snapshot_shape(snapshot, outputs):
                continue
            if not has_unconditional_return(body_open, body_close):
                continue
            if any(
                tokens[index].value == "if"
                for index in range(body_open + 1, body_close)
            ):
                continue
            return True
        return False

    def positive_guard(start: int, finish: int) -> bool:
        condition = values((start, finish))
        if "||" in condition or "!" in condition:
            return False
        required = {
            ("validated_owner", "==", "owner"),
            ("validated_incarnation", "==", "owner_incarnation"),
            ("validated_epoch", "==", "epoch"),
            (
                "pthread_equal",
                "(",
                "validated_native_thread",
                ",",
                "native_thread",
                ")",
            ),
            (
                "llam_worker_load_active_g",
                "(",
                "validated_owner",
                ")",
                "==",
                "current",
            ),
            (
                "llam_worker_binding_live",
                "(",
                "validated_owner",
                ",",
                "validated_incarnation",
                ")",
            ),
        }
        parts = top_level_parts(start, finish, "&&")
        return len(parts) == len(required) and {values(part) for part in parts} == required

    def failure_guard(
        start: int,
        finish: int,
        validated_snapshot: _CallSite,
    ) -> bool:
        condition = values((start, finish))
        if "&&" in condition:
            return False
        required = {
            ("validated_owner", "!=", "owner"),
            ("validated_incarnation", "!=", "owner_incarnation"),
            ("validated_epoch", "!=", "epoch"),
            (
                "pthread_equal",
                "(",
                "validated_native_thread",
                ",",
                "native_thread",
                ")",
                "==",
                "0",
            ),
            (
                "llam_worker_load_active_g",
                "(",
                "validated_owner",
                ")",
                "!=",
                "current",
            ),
        }
        seen: set[tuple[str, ...]] = set()
        snapshot_seen = binding_seen = False
        for part in top_level_parts(start, finish, "||"):
            part_values = values(part)
            if part_values[:1] == ("!",):
                nested = calls_in((part[0] + 1, part[1]))
                if (
                    len(nested) == 1
                    and nested[0] == validated_snapshot
                    and snapshot_shape(nested[0], validated_outputs)
                ):
                    snapshot_seen = True
                    continue
                if part_values[1:] == (
                    "llam_worker_binding_live",
                    "(",
                    "validated_owner",
                    ",",
                    "validated_incarnation",
                    ")",
                ):
                    binding_seen = True
                    continue
                return False
            if part_values not in required or part_values in seen:
                return False
            seen.add(part_values)
        return snapshot_seen and binding_seen and seen == required

    for call in calls:
        if call.name != "pthread_kill" or len(call.args) < 2:
            continue
        function = _function_for_token(masked, tokens, functions, call.name_index)
        if function is None or function.name != "llam_watchdog_check_shard":
            fail(tokens[call.name_index].line)
            continue
        begin, end = _token_range_for_span(masked, function)
        body_open = next(
            (index for index in range(begin, end) if tokens[index].value == "{"),
            None,
        )
        if body_open is None:
            fail(tokens[call.name_index].line)
            continue
        control_records = _control_flow_records(tokens, begin, end)
        if any(tokens[index].value == "goto" for index in range(body_open + 1, end)):
            # The canonical signal path has no jumps.  In particular, a jump
            # into a positive guard can bypass its conjunction while retaining
            # the same lexical condition around the kill.
            fail(tokens[call.name_index].line)
            continue
        if any(
            tokens[index].value == ":"
            and index > body_open + 1
            and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", tokens[index - 1].value)
            and tokens[index - 2].value in {"{", "}", ";"}
            for index in range(body_open + 1, end)
        ):
            fail(tokens[call.name_index].line)
            continue
        if (
            _simple_identifier(tokens, call.args[0]) != "validated_native_thread"
            or _scalar_value(tokens, call.args[1]) != "LLAM_PREEMPT_SIGNAL"
        ):
            fail(tokens[call.name_index].line)
            continue
        snapshot_calls = [
            candidate
            for candidate in calls
            if candidate.name == "llam_legacy_lease_snapshot_watchdog_locked"
            and begin <= candidate.name_index < call.name_index
        ]
        if len(snapshot_calls) != 2:
            fail(tokens[call.name_index].line)
            continue
        original_snapshot, validated_snapshot = snapshot_calls
        if not snapshot_shape(original_snapshot, original_outputs) or not snapshot_shape(
            validated_snapshot, validated_outputs
        ):
            fail(tokens[call.name_index].line)
            continue
        records = if_records(begin, end)
        def in_if_condition(token_index: int) -> bool:
            return any(
                start < token_index < finish
                for _if_index, start, finish, _if_body_open, _if_body_close in records
            )

        def in_control_context(token_index: int) -> bool:
            return any(
                _control_contains(record, token_index) for record in control_records
            )

        original_top_level = (
            body_depth(body_open, original_snapshot.name_index) == 1
            and not in_if_condition(original_snapshot.name_index)
            and not in_control_context(original_snapshot.name_index)
        )
        if not original_top_level and not canonical_snapshot_failure_guard(
            original_snapshot, original_outputs, records
        ):
            fail(tokens[call.name_index].line)
            continue
        validated_top_level = (
            body_depth(body_open, validated_snapshot.name_index) == 1
            and not in_if_condition(validated_snapshot.name_index)
            and not in_control_context(validated_snapshot.name_index)
        )

        # No original or validated snapshot output may be rebound before the
        # signal, including pointer movement or compound updates.  Address
        # aliases are checked from function scope, not from the first
        # snapshot: an alias created before that call can still overwrite the
        # output after the snapshot has returned.
        rebound = False
        output_names = set(original_outputs) | set(validated_outputs)
        def declaration_zero_initializer(
            variable: str, variable_index: int, statement_start: int
        ) -> bool:
            """Recognize only the native zero-valued output declarations."""

            if variable_index + 1 >= call.name_index or tokens[variable_index + 1].value != "=":
                return False
            prefix = tuple(
                tokens[item].value for item in range(statement_start, variable_index)
            )
            if not any(
                value == "*"
                or value in {"uint64_t", "pthread_t"}
                or value.endswith("_t")
                for value in prefix
            ):
                return False
            finish = _owner_statement_end(tokens, variable_index + 2, call.name_index)
            return _is_zero_value(
                tokens,
                tuple(item for item in range(variable_index + 2, finish) if tokens[item].value != ";"),
            )

        def direct_current_load(variable_index: int) -> bool:
            if variable_index + 1 >= call.name_index or tokens[variable_index + 1].value != "=":
                return False
            finish = _owner_statement_end(tokens, variable_index + 2, call.name_index)
            rhs = tuple(tokens[item].value for item in range(variable_index + 2, finish))
            if not rhs or rhs[0] != "llam_worker_load_active_g":
                return False
            matching = [
                candidate
                for candidate in calls
                if candidate.name == "llam_worker_load_active_g"
                and variable_index < candidate.name_index < finish
                and len(candidate.args) == 1
                and _simple_identifier(tokens, candidate.args[0]) == "owner"
            ]
            return len(matching) == 1 and rhs == tuple(
                tokens[item].value
                for item in range(matching[0].name_index, matching[0].close_index + 1)
            )

        # Before the first snapshot, only declaration initializers with the
        # native zero values are permitted.  ``current`` has one additional
        # direct active-G load between the two snapshots.  Any attacker
        # assignment, including parenthesized/comma lvalues, invalidates the
        # tuple proof.
        for index in range(body_open + 1, call.name_index):
            if tokens[index].value not in output_names | {"current"}:
                continue
            statement_start, statement_end = _statement_bounds(
                tokens, index, body_open + 1, call.name_index
            )
            following = tokens[index + 1].value if index + 1 < call.name_index else None
            if following in ASSIGNMENT_OPERATORS:
                if (
                    index < original_snapshot.name_index
                    and tokens[index].value in output_names | {"current"}
                    and following == "="
                    and declaration_zero_initializer(
                        tokens[index].value, index, statement_start
                    )
                ):
                    continue
                if (
                    tokens[index].value == "current"
                    and original_snapshot.close_index < index < validated_snapshot.name_index
                    and following == "="
                    and direct_current_load(index)
                ):
                    continue
                rebound = True
                break
            if (
                index > body_open + 1
                and tokens[index - 1].value in {"++", "--"}
            ) or (
                index + 1 < call.name_index
                and tokens[index + 1].value in {"++", "--"}
            ):
                rebound = True
                break
            # A parenthesized/cast/comma lvalue can move the assignment past
            # the identifier.  Do not infer it as a read from the snapshot
            # outputs.
            if any(
                tokens[item].value in ASSIGNMENT_OPERATORS and item > index
                for item in range(index + 1, statement_end)
            ):
                rebound = True
                break
        if not rebound:
            # The declaration/zeroing forms above are allowed before the
            # original snapshot, but parenthesized prefix/postfix movement is
            # never a valid initialization of a snapshot output.  Check only
            # movement here; the address walk below retains its explicit
            # memset/snapshot exceptions for canonical output setup.
            for variable in (*original_outputs, "current"):
                if _identifier_has_expression_write_or_move(
                    tokens,
                    variable,
                    body_open + 1,
                    original_snapshot.name_index,
                    pointer=variable in {"owner", "native_thread", "current"},
                    include_assignment=False,
                    include_address=False,
                ):
                    rebound = True
                    break
        for index in range(original_snapshot.close_index + 1, call.name_index):
            if tokens[index].value not in output_names:
                continue
            if (
                index + 1 < call.name_index
                and tokens[index + 1].value in ASSIGNMENT_OPERATORS
            ) or (
                index > original_snapshot.close_index
                and tokens[index - 1].value in {"++", "--"}
            ) or (
                index + 1 < call.name_index
                and tokens[index + 1].value in {"++", "--"}
            ):
                rebound = True
                break
        # An output address may not escape through an alias.  The only
        # pre-snapshot address uses in the native implementation are the
        # direct snapshot arguments and zeroing ``memset`` calls.  Every other
        # ``&output`` is an unsupported write channel, including
        # ``pthread_t *p = &output``.
        if not rebound:
            for index in range(body_open + 1, call.name_index - 1):
                addressed = _addressed_identifier_at(tokens, index, call.name_index)
                if addressed is None:
                    continue
                output, addressed_end = addressed
                if output not in output_names or (
                    addressed_end < call.name_index
                    and tokens[addressed_end].value in {"->", "."}
                ):
                    continue
                containing_snapshot = _call_containing(
                    calls,
                    index,
                    {"llam_legacy_lease_snapshot_watchdog_locked"},
                )
                if containing_snapshot is not None:
                    continue
                containing_memset = _call_containing(calls, index, {"memset"})
                if containing_memset is not None and len(containing_memset.args) == 3:
                    if (
                        _addressed_identifier(tokens, containing_memset.args[0]) == output
                        and _is_zero_value(tokens, containing_memset.args[1])
                        and _arg_values(tokens, containing_memset.args[2])
                        == ("sizeof", "(", output, ")")
                    ):
                        continue
                rebound = True
                break
        # ``current`` is loaded from active-G after the original snapshot and
        # must remain the same object through the validated snapshot/kill
        # window.  It is intentionally checked only after the validated
        # snapshot because the canonical path initializes it between the two
        # snapshots.
        if not rebound:
            stability_names = output_names | {"current"}
            for index in range(validated_snapshot.close_index + 1, call.name_index):
                if tokens[index].value not in stability_names:
                    continue
                if (
                    index + 1 < call.name_index
                    and tokens[index + 1].value in ASSIGNMENT_OPERATORS
                ) or (
                    index > validated_snapshot.close_index
                    and tokens[index - 1].value in {"++", "--"}
                ) or (
                    index + 1 < call.name_index
                    and tokens[index + 1].value in {"++", "--"}
                ):
                    rebound = True
                    break
        if not rebound:
            # ``current`` is also a protected scalar: a bare address escape
            # before or after the validated snapshot could mutate the active-G
            # comparison operand without an obvious assignment token.
            for index in range(body_open + 1, call.name_index):
                addressed = _addressed_identifier_at(tokens, index, call.name_index)
                if addressed is None or addressed[0] != "current":
                    continue
                if (
                    addressed[1] < call.name_index
                    and tokens[addressed[1]].value in {"->", "."}
                ):
                    continue
                rebound = True
                break
        if not rebound:
            # Parenthesized/cast movement of the original snapshot outputs in
            # the interval before the validated snapshot is not visible to
            # the adjacent-token checks above.  The validated call itself
            # does not use these names, so this interval is safe to inspect as
            # one expression-closed provenance window.
            for variable in original_outputs:
                if _identifier_has_expression_write_or_move(
                    tokens,
                    variable,
                    original_snapshot.close_index + 1,
                    validated_snapshot.name_index,
                    pointer=variable in {"owner", "native_thread"},
                ):
                    rebound = True
                    break
        if not rebound:
            # After the second snapshot every validated output, plus the
            # active-G current value, must remain immutable through the signal
            # decision.  Direct member projections are not present in this
            # set, so grouped/cast/comma lvalues can fail closed here without
            # affecting the canonical snapshot call itself.
            for variable in (*validated_outputs, "current"):
                if _identifier_has_expression_write_or_move(
                    tokens,
                    variable,
                    validated_snapshot.close_index + 1,
                    call.name_index,
                    pointer=variable in {"validated_owner", "validated_native_thread", "current"},
                ):
                    rebound = True
                    break
        # Parenthesized lvalues such as ``((native_thread)) = attacker`` do
        # not place an assignment operator immediately after the identifier.
        # Any assignment statement mentioning a snapshot output after the
        # original snapshot is therefore unsupported unless it is the direct
        # validated snapshot call itself (which has no assignment operator).
        if not rebound:
            for index in range(original_snapshot.close_index + 1, call.name_index):
                if tokens[index].value not in output_names:
                    continue
                statement_start, statement_end = _statement_bounds(
                    tokens, index, begin, call.name_index
                )
                if any(
                    tokens[item].value in ASSIGNMENT_OPERATORS
                    and item > index
                    for item in range(statement_start, statement_end)
                ):
                    rebound = True
                    break
        if rebound:
            fail(tokens[call.name_index].line)
            continue
        signal_path_calls = {
            "pthread_equal",
            "pthread_mutex_unlock",
            "llam_worker_load_active_g",
            "llam_worker_binding_live",
            "llam_legacy_lease_snapshot_watchdog_locked",
            "llam_task_scan_ref_release",
            "llam_slice_ns",
            "llam_watchdog_preempt_pressure_locked",
            "llam_trace_shard",
            *ATOMIC_LOAD_NAMES,
            "atomic_exchange",
            *CONTROL_FLOW_NAMES,
        }
        for candidate in calls:
            if not validated_snapshot.close_index < candidate.name_index < call.name_index:
                continue
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", candidate.name):
                continue
            if candidate.name in signal_path_calls:
                continue
            # The signal window is deliberately operation-closed.  An
            # unknown wrapper can unlock/reacquire, mutate a snapshot, or
            # otherwise invalidate the lifetime proof without exposing a
            # member token for this lexical audit to inspect.
            fail(tokens[call.name_index].line)
            break
        else:
            pass
        if errors and errors[-1][0] == tokens[call.name_index].line:
            continue

        positive = failure = False
        guard_body: tuple[int, int] | None = None
        for _if_index, start, finish, if_body_open, if_body_close in records:
            if if_body_open < call.name_index < if_body_close:
                if positive_guard(start, finish):
                    positive = True
                    guard_body = (if_body_open, if_body_close)
            elif if_body_close < call.name_index and failure_guard(
                start, finish, validated_snapshot
            ) and has_unconditional_return(if_body_open, if_body_close):
                failure = True
                guard_body = (if_body_open, if_body_close)
        if positive:
            if not validated_top_level:
                fail(tokens[call.name_index].line)
                continue
        elif failure:
            if validated_top_level or guard_body is None:
                fail(tokens[call.name_index].line)
                continue
        else:
            fail(tokens[call.name_index].line)
            continue

        # An unlock after the validated snapshot is safe only in the
        # canonical failure-return cleanup body.  Any other unlock leaves a
        # reachable signal without the lease lifetime lock.
        protected_start = validated_snapshot.close_index + 1
        for candidate in calls:
            if candidate.name != "pthread_mutex_unlock" or not candidate.args:
                continue
            if not protected_start < candidate.name_index < call.name_index:
                continue
            if failure and guard_body is not None and guard_body[0] < candidate.name_index < guard_body[1]:
                continue
            if mutex_receiver(candidate) is not None:
                fail(tokens[call.name_index].line)
                break
        else:
            held = {"opaque_lock": False, "lock": False}
            for candidate in sorted(
                (
                    item
                    for item in calls
                    if begin <= item.name_index < protected_start
                    and item.name in {
                        "pthread_mutex_lock",
                        "pthread_mutex_trylock",
                        "pthread_mutex_timedlock",
                        "pthread_mutex_unlock",
                    }
                ),
                key=lambda item: item.name_index,
            ):
                receiver = mutex_receiver(candidate)
                if receiver is None or receiver[0] != "shard":
                    continue
                if candidate.name == "pthread_mutex_unlock":
                    held[receiver[1]] = False
                else:
                    held[receiver[1]] = True
            if not all(held.values()):
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
    extra.extend(_owner_assignment_errors(masked, relative_path))
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
