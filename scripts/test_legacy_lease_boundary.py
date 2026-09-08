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

"""Behavior tests for the Phase 1A legacy execution-lease boundary scanner."""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from check_legacy_lease_boundary import scan_source


ROOT = Path(__file__).resolve().parents[1]
CHECKER = Path(__file__).with_name("check_legacy_lease_boundary.py")


class LegacyLeaseBoundaryTest(unittest.TestCase):
    def assert_error(self, relative_path: str, source: str, message: str) -> None:
        errors = scan_source(relative_path, source)
        self.assertTrue(any(message in error for error in errors), errors)

    def assert_clean(self, relative_path: str, source: str) -> None:
        self.assertEqual([], scan_source(relative_path, source))

    def test_allows_the_exact_worker_local_active_g_surface(self) -> None:
        self.assert_clean(
            "src/internal/runtime_types.h",
            "struct llam_worker { _Atomic(llam_task_t *) active_g; };\n",
        )
        self.assert_clean(
            "src/core/lifecycle/init.c",
            "void init(llam_runtime_t *rt, unsigned i) {\n"
            "    atomic_init(&rt->shards[i].primary_worker.active_g, NULL);\n"
            "    atomic_init(&rt->shards[i].opaque_helper_worker.active_g, NULL);\n"
            "}\n",
        )
        self.assert_clean(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n"
            "llam_task_t *llam_worker_load_active_g(const llam_worker_t *worker) {\n"
            "    return atomic_load_explicit(&worker->active_g, memory_order_acquire);\n"
            "}\n"
            "void llam_worker_clear_active_g(llam_worker_t *worker) {\n"
            "    atomic_store_explicit(&worker->active_g, NULL, memory_order_release);\n"
            "}\n",
        )

    def test_rejects_active_g_outside_the_allowlist(self) -> None:
        self.assert_error(
            "src/core/task/bad.c",
            "void bad(llam_worker_t *worker, llam_task_t *task) {\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/lifecycle/bad.c",
            "void bad(llam_shard_t *shard) { (void)shard->active_g; }\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_independent_current_authority(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard, llam_task_t *task) {\n"
            "    shard->current = task;\n"
            "}\n",
            "independent shard current authority",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_runtime_t *rt, unsigned i, llam_task_t *task) {\n"
            "    rt->shards[i].current = task;\n"
            "}\n",
            "independent shard current authority",
        )

    def test_rejects_prohibited_worker_and_processor_authorities(self) -> None:
        fixtures = (
            ("processor->executor", "Phase 0 execution authority"),
            ("worker->owns_p", "Phase 1A execution authority"),
            ("worker->preempt_target", "Phase 0 execution authority"),
            ("processor->current_g", "Phase 0 execution authority"),
        )
        for expression, message in fixtures:
            with self.subTest(expression=expression):
                self.assert_error(
                    "src/internal/runtime_types.h",
                    f"void bad(void *processor, void *worker) {{ (void){expression}; }}\n",
                    message,
                )

    def test_rejects_copied_processor_resources(self) -> None:
        self.assert_error(
            "src/internal/runtime_types.h",
            "struct llam_processor { llam_queue_t *queues; };\n",
            "copied P resource",
        )
        for expression in (
            "processor->norm_q = shard->norm_q;",
            "shard->processor.timers = shard->timers;",
            "processor->allocator = shard->allocator;",
        ):
            with self.subTest(expression=expression):
                self.assert_error(
                    "src/core/sched/bad.c",
                    f"void bad(void) {{ {expression} }}\n",
                    "copied P resource",
                )

    def test_rejects_independent_preempt_target(self) -> None:
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    pthread_t target = shard->preempt_thread;\n"
            "    (void)target;\n"
            "}\n",
            "independent preempt target",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard, pthread_t target) {\n"
            "    shard->preempt_thread = target;\n"
            "}\n",
            "independent preempt target",
        )

    def test_rejects_unversioned_pause_ack_decisions(self) -> None:
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "bool bad(llam_shard_t *shard) {\n"
            "    return atomic_load_explicit(&shard->merge_pause_ack, memory_order_acquire) != 0U;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "bool bad(llam_shard_t *shard) {\n"
            "    if (shard->steal_pause_ack) return true;\n"
            "    return false;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_allows_versioned_pause_ack_matchers_and_publication(self) -> None:
        self.assert_clean(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(const llam_shard_t *shard,\n"
            "                                                  uint64_t request,\n"
            "                                                  uint64_t epoch,\n"
            "                                                  uint64_t incarnation) {\n"
            "    return atomic_load_explicit(&shard->merge_pause_ack, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire) == epoch &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire) == incarnation;\n"
            "}\n",
        )
        self.assert_clean(
            "src/engine/watchdog/watchdog_merge.c",
            "void reset(llam_shard_t *shard) {\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_sequence, 0U, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, 0U, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation, 0U, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack, 0U, memory_order_release);\n"
            "}\n",
        )

    def test_rejects_watchdog_target_reads_outside_lease_lifetime(self) -> None:
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    pthread_kill(shard->thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "void bad(llam_shard_t *shard, pthread_t target) {\n"
            "    pthread_kill(target, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_allows_lease_bound_watchdog_signal(self) -> None:
        self.assert_clean(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &target);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(target, native_target)) {\n"
            "        pthread_kill(native_target, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
        )

    def test_rejects_ownerless_grant(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    shard->legacy_lease.owner = worker;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    if (shard->legacy_lease.owner == NULL) {\n"
            "        shard->legacy_lease.owner = worker;\n"
            "    }\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    shard->legacy_lease.owner = worker;\n"
            "    if (shard->legacy_lease.owner != NULL) return;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_reverse_scheduler_lock_order(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_cli_reports_repository_boundary_failure(self) -> None:
        with tempfile.TemporaryDirectory(prefix="llam-legacy-lease-boundary-") as raw_root:
            root = Path(raw_root)
            source = root / "src" / "core" / "sched" / "bad.c"
            source.parent.mkdir(parents=True)
            source.write_text(
                "void bad(llam_shard_t *shard) {\n"
                "    shard->current = NULL;\n"
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
        self.assertIn("independent shard current authority", result.stderr)


if __name__ == "__main__":
    unittest.main()
