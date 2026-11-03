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

"""Behavior tests for the Phase 0 worker identity source-boundary scanner."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from check_worker_identity_boundary import (
    PROVIDED_MAGIC2_BRIDGE_PATH,
    PROVIDED_MAGIC2_PATH,
    _tokens,
    scan_repository,
    scan_source,
)


ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(__file__).with_name("check_worker_identity_boundary.py")


class WorkerIdentityBoundaryTest(unittest.TestCase):
    def assert_error(self, relative_path: str, source: str, message: str) -> None:
        errors = scan_source(relative_path, source)
        self.assertTrue(any(message in error for error in errors), errors)

    def test_token_lines_without_repeated_prefix_scans(self) -> None:
        class CountedSource(str):
            inspected = 0

            def count(self, substring, start=0, end=None):
                stop = len(self) if end is None else end
                self.inspected += max(0, stop - start)
                return super().count(substring, start, stop)

        source = CountedSource("\n  int item = 0;\n" * 1000)
        tokens = _tokens(source)
        self.assertEqual(5000, len(tokens))
        self.assertEqual(("int", 2), (tokens[0].value, tokens[0].line))
        self.assertEqual((";", 2000), (tokens[-1].value, tokens[-1].line))
        # Work accounting avoids a wall-clock threshold on slow CI hosts.
        self.assertLessEqual(source.inspected, len(source) * 2)

    def test_accepts_exact_supplied_magic2_snapshot(self) -> None:
        source = (ROOT / PROVIDED_MAGIC2_PATH).read_text(encoding="utf-8")
        self.assertEqual([], scan_source(PROVIDED_MAGIC2_PATH, source))

    def test_rejects_modified_supplied_magic2_snapshot(self) -> None:
        self.assert_error(PROVIDED_MAGIC2_PATH, "", "snapshot hash mismatch")

    def test_magic2_exception_is_path_specific(self) -> None:
        self.assert_error(
            "src/internal/other_magic2.h", "worker->executor;",
            "Phase 0 execution authority",
        )

    def test_rejects_missing_magic2_snapshot_when_bridge_exists(self) -> None:
        with tempfile.TemporaryDirectory(prefix="llam-worker-magic2-") as raw_root:
            root = Path(raw_root)
            bridge = root / PROVIDED_MAGIC2_BRIDGE_PATH
            bridge.parent.mkdir(parents=True)
            bridge.write_text("/* bridge fixture */\n", encoding="utf-8")
            errors = scan_repository(root)
        self.assertTrue(any("snapshot is missing" in error for error in errors), errors)

    def test_rejects_raw_context_role_check(self) -> None:
        self.assert_error(
            "src/core/task/example.c",
            "if (g_llam_tls_scheduler_ctx == &shard->scheduler_ctx) {}",
            "raw scheduler-context role check",
        )

    def test_rejects_reversed_raw_context_role_check(self) -> None:
        self.assert_error(
            "src/core/task/example.c",
            "if (&shard->opaque_scheduler_ctx != g_llam_tls_scheduler_ctx) {}",
            "raw scheduler-context role check",
        )

    def test_rejects_parenthesized_raw_context_role_check(self) -> None:
        self.assert_error(
            "src/core/task/example.c",
            "if ((g_llam_tls_scheduler_ctx) == (&shard->scheduler_ctx)) {}",
            "raw scheduler-context role check",
        )

    def test_rejects_long_raw_context_expression(self) -> None:
        self.assert_error(
            "src/core/task/example.c",
            "if (g_llam_tls_scheduler_ctx == "
            "&rt->shards[0].a.b.c.d.e.f.g.scheduler_ctx) {}",
            "raw scheduler-context role check",
        )

    def test_allows_context_routing_expression(self) -> None:
        errors = scan_source(
            "src/core/task/example.c",
            "ctx = g_llam_tls_scheduler_ctx != NULL ? "
            "g_llam_tls_scheduler_ctx : &shard->scheduler_ctx;",
        )
        self.assertEqual([], errors)

    def test_allows_scheduler_context_null_check(self) -> None:
        errors = scan_source(
            "src/core/task/example.c",
            "if (g_llam_tls_scheduler_ctx != NULL) {}",
        )
        self.assertEqual([], errors)

    def test_allows_debug_equivalence_checks_in_runtime_state(self) -> None:
        errors = scan_source(
            "src/internal/runtime_state.h",
            "#ifndef NDEBUG\n"
            "bool legacy = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#endif\n",
        )
        self.assertEqual([], errors)

    def test_rejects_debug_comparison_in_runtime_state_elif_branch(self) -> None:
        self.assert_error(
            "src/internal/runtime_state.h",
            "#ifndef NDEBUG\n"
            "bool legacy = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#elif defined(LLAM_RELEASE_POLICY)\n"
            "bool production = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#endif\n",
            "raw scheduler-context role check",
        )

    def test_rejects_debug_comparison_under_unrelated_outer_guard(self) -> None:
        self.assert_error(
            "src/internal/runtime_state.h",
            "#if defined(LLAM_PLATFORM)\n"
            "#ifndef NDEBUG\n"
            "bool legacy = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#endif\n"
            "#endif\n",
            "raw scheduler-context role check",
        )

    def test_rejects_debug_equivalence_checks_outside_runtime_state(self) -> None:
        self.assert_error(
            "src/core/task/example.c",
            "#ifndef NDEBUG\n"
            "bool legacy = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#endif\n",
            "raw scheduler-context role check",
        )

    def test_rejects_future_execution_authority_names(self) -> None:
        self.assert_error(
            "src/internal/runtime_state.h",
            "bool worker_is_executor(llam_worker_t *worker);",
            "Phase 0 execution authority",
        )

    def test_rejects_can_execute_processor_name(self) -> None:
        self.assert_error(
            "src/internal/runtime_state.h",
            "bool can_execute_processor(const llam_processor_t *processor);",
            "Phase 0 execution authority",
        )

    def test_rejects_future_authority_member_declarations(self) -> None:
        forbidden = (
            "active_g",
            "owns_processor",
            "preempt_target",
            "executor",
            "current_g",
            "lease_epoch",
        )
        for member in forbidden:
            with self.subTest(member=member):
                self.assert_error(
                    "src/internal/runtime_types.h",
                    f"struct phase_zero_worker {{ unsigned {member}; }};",
                    "Phase 0 execution authority",
                )

    def test_rejects_production_selected_elif_comparison(self) -> None:
        self.assert_error(
            "src/internal/runtime_state.h",
            "#ifndef NDEBUG\n"
            "#if defined(LLAM_DEBUG_EQUIVALENCE)\n"
            "bool legacy = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#elif defined(LLAM_RELEASE_POLICY)\n"
            "bool production = g_llam_tls_scheduler_ctx == &shard->scheduler_ctx;\n"
            "#endif\n"
            "#endif\n",
            "raw scheduler-context role check",
        )

    def test_rejects_every_future_execution_authority_identifier(self) -> None:
        forbidden = (
            "worker_is_executor",
            "worker_owns_processor",
            "processor_current_worker",
            "worker->active_g",
            "worker->owns_processor",
            "worker->preempt_target",
            "processor->executor",
            "processor->current_g",
            "processor->lease_epoch",
        )
        for identifier in forbidden:
            with self.subTest(identifier=identifier):
                self.assert_error(
                    "src/internal/runtime_types.h",
                    f"void phase_zero_probe(void) {{ (void){identifier}; }}",
                    "Phase 0 execution authority",
                )

    def test_rejects_obsolete_shard_signal_stack_members(self) -> None:
        obsolete = (
            "signal_stack",
            "signal_stack_size",
            "previous_sigaltstack",
            "sigaltstack_installed",
        )
        for member in obsolete:
            with self.subTest(member=member):
                self.assert_error(
                    "src/internal/runtime_types.h",
                    f"void phase_zero_probe(void) {{ (void)shard->{member}; }}",
                    "obsolete shard signal-stack access",
                )

    def test_rejects_obsolete_members_through_shard_array_access(self) -> None:
        self.assert_error(
            "src/core/lifecycle/shutdown.c",
            "void phase_zero_probe(void) { (void)rt->shards[0].signal_stack; }",
            "obsolete shard signal-stack access",
        )

    def test_allows_combined_signal_stack_mapping_storage(self) -> None:
        errors = scan_source(
            "src/internal/runtime_types.h",
            "void phase_zero_probe(void) {\n"
            "    (void)shard->signal_stack_mapping;\n"
            "    (void)shard->signal_stack_mapping_size;\n"
            "}\n",
        )
        self.assertEqual([], errors)

    def test_allows_worker_local_signal_stack_members(self) -> None:
        errors = scan_source(
            "src/core/platform/platform.c",
            "void phase_zero_probe(void) {\n"
            "    (void)worker->signal_stack;\n"
            "    (void)worker->signal_stack_size;\n"
            "    (void)worker->previous_sigaltstack;\n"
            "    (void)worker->sigaltstack_installed;\n"
            "}\n",
        )
        self.assertEqual([], errors)

    def test_cli_reports_repository_boundary_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="llam-worker-boundary-") as raw_root:
            root = Path(raw_root)
            source = root / "src" / "core" / "task" / "bad.c"
            source.parent.mkdir(parents=True)
            source.write_text(
                "void bad(void) {\n"
                "    if (g_llam_tls_scheduler_ctx == &shard->scheduler_ctx) {}\n"
                "}\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                [sys.executable, str(CHECKER), "--root", str(root)],
                cwd=ROOT,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
        self.assertEqual(1, result.returncode, result.stdout + result.stderr)
        self.assertIn("bad.c:2", result.stderr)
        self.assertIn("raw scheduler-context role check", result.stderr)


if __name__ == "__main__":
    unittest.main()
