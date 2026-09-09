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

from audit_production_test_hooks import HOOK_RE, SAFE_SELF_TEST_RE, _symbols
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
            "    atomic_store_explicit(&worker->last_run_started_ns, 0U, memory_order_release);\n"
            "}\n",
        )

    def test_rejects_malformed_active_g_allowlist_surfaces(self) -> None:
        self.assert_error(
            "src/internal/runtime_types.h",
            "struct llam_worker { int active_g; };\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/lifecycle/init.c",
            "void init(llam_runtime_t *rt, unsigned index) {\n"
            "    atomic_init(&foo.primary_worker.active_g, NULL);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/lifecycle/init.c",
            "void init(llam_runtime_t *rt, unsigned index) {\n"
            "    atomic_init(&rt->shards[index].primary_worker.active_g, task);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    worker->active_g = task;\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task, llam_worker_t *other) {\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release); other->active_g = task;\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_worker_t *other, llam_task_t *task) {\n"
            "    atomic_store_explicit(&other->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
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
            "    return atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire) == epoch &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire) == incarnation &&\n"
            "           llam_shard_pause_owner_matches_locked(shard, epoch, incarnation);\n"
            "}\n",
        )
        self.assert_clean(
            "src/engine/watchdog/watchdog_merge.c",
            "void llam_shard_release_merge_pause(llam_shard_t *shard) {\n"
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

    def test_rejects_watchdog_proof_invalidated_after_snapshot(self) -> None:
        for unlock_relock in (
            "pthread_mutex_unlock(&shard->lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n",
            "pthread_mutex_unlock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n",
        ):
            with self.subTest(unlock_relock=unlock_relock):
                self.assert_error(
                    "src/engine/watchdog/watchdog_probe.c",
                    "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
                    "    pthread_mutex_lock(&shard->opaque_lock);\n"
                    "    pthread_mutex_lock(&shard->lock);\n"
                    "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
                    "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, native_thread)) {\n"
                    f"        {unlock_relock}"
                    "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
                    "    }\n"
                    "}\n",
                    "watchdog target outside lease lifetime",
                )

    def test_rejects_watchdog_target_reassignment_after_snapshot(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
            "    validated_native_thread = arbitrary_target;\n"
            "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, native_thread))\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_equality_with_arbitrary_second_operand(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
            "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, arbitrary_target))\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_proof_in_non_dominating_branch(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
            "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, native_thread)) {\n"
            "        (void)owner;\n"
            "    }\n"
            "    pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_predicate_and_snapshot_lifetime_bypasses(self) -> None:
        prefix = (
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(\n"
            "        shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(\n"
            "        shard, &validated_owner, &validated_incarnation, &validated_epoch,\n"
            "        &validated_native_thread);\n"
        )
        suffix = (
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n"
        )
        fixtures = (
            (
                "    if (!llam_worker_binding_live(validated_owner, validated_incarnation) &&\n"
                "        pthread_equal(validated_native_thread, native_thread)) {\n"
                + suffix,
                "negated binding predicate",
            ),
            (
                "    if (llam_worker_binding_live(validated_owner, validated_incarnation) ||\n"
                "        pthread_equal(validated_native_thread, native_thread)) {\n"
                + suffix,
                "OR binding predicate",
            ),
            (
                "    if (llam_worker_binding_live(validated_owner, validated_incarnation) &&\n"
                "        pthread_equal(validated_native_thread, native_thread)) {\n"
                + suffix,
                "missing epoch comparison",
            ),
            (
                "    if (flag) {\n"
                "        llam_legacy_lease_snapshot_watchdog_locked(\n"
                "            shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
                "    }\n"
                "    if (validated_owner == owner &&\n"
                "        validated_incarnation == owner_incarnation &&\n"
                "        validated_epoch == epoch &&\n"
                "        pthread_equal(validated_native_thread, native_thread) &&\n"
                "        llam_worker_load_active_g(validated_owner) == current &&\n"
                "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
                + suffix,
                "non-dominating original snapshot",
            ),
            (
                "    native_thread = attacker;\n"
                "    if (validated_owner == owner &&\n"
                "        validated_incarnation == owner_incarnation &&\n"
                "        validated_epoch == epoch &&\n"
                "        pthread_equal(validated_native_thread, native_thread) &&\n"
                "        llam_worker_load_active_g(validated_owner) == current &&\n"
                "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
                + suffix,
                "reassigned original handle",
            ),
            (
                "    MUTATE_SNAPSHOT(&validated_native_thread);\n"
                "    if (validated_owner == owner &&\n"
                "        validated_incarnation == owner_incarnation &&\n"
                "        validated_epoch == epoch &&\n"
                "        pthread_equal(validated_native_thread, native_thread) &&\n"
                "        llam_worker_load_active_g(validated_owner) == current &&\n"
                "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
                + suffix,
                "macro-hidden snapshot mutation",
            ),
            (
                "    if (validated_owner == owner &&\n"
                "        validated_incarnation == owner_incarnation &&\n"
                "        validated_epoch == epoch &&\n"
                "        pthread_equal(validated_native_thread, native_thread) &&\n"
                "        llam_worker_load_active_g(validated_owner) == current &&\n"
                "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
                "        pthread_mutex_unlock(&shard->lock);\n"
                "        if (other) return;\n"
                "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
                "    }\n"
                "}\n",
                "conditional unlock before reachable kill",
            ),
        )
        for body, description in fixtures:
            with self.subTest(description=description):
                self.assert_error(
                    "src/engine/watchdog/watchdog_probe.c",
                    prefix + body,
                    "watchdog target outside lease lifetime",
                )

    def test_allows_lease_bound_watchdog_signal(self) -> None:
        self.assert_clean(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    if (validated_owner == owner &&\n"
            "        validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch &&\n"
            "        pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
        )

    def test_rejects_watchdog_signals_without_target_binding_proof(self) -> None:
        fixtures = (
            (
                "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
                "    pthread_mutex_lock(&shard->opaque_lock);\n"
                "    pthread_mutex_lock(&shard->lock);\n"
                "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
                "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, original_target))\n"
                "        pthread_kill(foo(shard), LLAM_PREEMPT_SIGNAL);\n"
                "}\n",
                "alternate watchdog signal operand",
            ),
            (
                "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
                "    pthread_mutex_lock(&shard->opaque_lock);\n"
                "    pthread_mutex_lock(&shard->lock);\n"
                "    pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
                "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
                "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, original_target)) return;\n"
                "}\n",
                "watchdog kill before snapshot",
            ),
            (
                "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
                "    pthread_mutex_lock(&shard->opaque_lock);\n"
                "    pthread_mutex_lock(&shard->lock);\n"
                "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
                "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, validated_native_thread))\n"
                "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
                "}\n",
                "tautological watchdog equality",
            ),
            (
                "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
                "    pthread_mutex_lock(&shard->opaque_lock);\n"
                "    pthread_mutex_lock(&shard->lock);\n"
                "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &validated_native_thread);\n"
                "    if (llam_worker_binding_live(owner, incarnation) && pthread_equal(validated_native_thread, original_target))\n"
                "        pthread_kill(original_target, LLAM_PREEMPT_SIGNAL);\n"
                "}\n",
                "original watchdog target operand",
            ),
        )
        for source, description in fixtures:
            with self.subTest(description=description):
                self.assert_error(
                    "src/engine/watchdog/watchdog_probe.c",
                    source,
                    "watchdog target outside lease lifetime",
                )

    def test_rejects_unversioned_or_misbound_pause_ack_matchers(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    if (atomic_load(&shard->merge_pause_ack)) return true;\n"
            "    if (false) {\n"
            "        atomic_load(&shard->merge_pause_ack_sequence) == request;\n"
            "        atomic_load(&shard->merge_pause_ack_lease_epoch) == epoch;\n"
            "        atomic_load(&shard->merge_pause_ack_worker_incarnation) == incarnation;\n"
            "    }\n"
            "    return false;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    bool ack = macro_hidden_ack(shard);\n"
            "    return ack;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    atomic_uint *ack = &shard->merge_pause_ack;\n"
            "    return atomic_load(ack) != 0U &&\n"
            "           atomic_load(&shard->merge_pause_ack_sequence) == request &&\n"
            "           atomic_load(&shard->merge_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->merge_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return atomic_load(&shard->merge_pause_ack) != 0U &&\n"
            "           atomic_load(&shard->merge_pause_ack_sequence) == wrong_request &&\n"
            "           atomic_load(&shard->merge_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->merge_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_sequence, request_sequence, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, epoch_value, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation, worker_incarnation, memory_order_release);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_reads_routed_only_through_dead_matcher_call(self) -> None:
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "bool decoy(llam_shard_t *shard) {\n"
            "    if (false)\n"
            "        llam_shard_merge_pause_ack_matches_locked(shard, 1U, 1U, 1U);\n"
            "    return atomic_load(&shard->merge_pause_ack_sequence) != 0U;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    if (false)\n"
            "        llam_shard_merge_pause_ack_matches_locked(shard, 1U, 1U, 1U);\n"
            "    return atomic_load(&shard->merge_pause_ack_sequence) != 0U;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_tuple_stores_hidden_in_dead_branch(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    if (false) {\n"
            "        atomic_store(&shard->merge_pause_ack_sequence, request_sequence);\n"
            "        atomic_store(&shard->merge_pause_ack_lease_epoch, epoch_value);\n"
            "        atomic_store(&shard->merge_pause_ack_worker_incarnation, worker_incarnation);\n"
            "    }\n"
            "    atomic_store(&shard->merge_pause_ack, 1U);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_unknown_macro_hidden_ack_state(self) -> None:
        self.assert_error(
            "src/engine/watchdog/bad.c",
            "bool bad(llam_shard_t *shard) {\n"
            "    return PUBLISH_ACK_UNSAFE(shard);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_matcher_without_live_request_generation(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return atomic_load(&shard->merge_pause_ack) != 0U &&\n"
            "           atomic_load(&shard->merge_pause_ack_sequence) == request &&\n"
            "           atomic_load(&shard->merge_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->merge_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_predicate_route_and_publication_bypasses(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return atomic_load(&shard->merge_pause_requested) != 0U &&\n"
            "           atomic_load(&shard->merge_pause_request_sequence) == request &&\n"
            "           (atomic_load(&shard->merge_pause_ack) != 0U || attacker) &&\n"
            "           atomic_load(&shard->merge_pause_ack_sequence) == request &&\n"
            "           atomic_load(&shard->merge_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->merge_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_merge_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return atomic_load(&shard->merge_pause_requested) != 0U &&\n"
            "           atomic_load(&shard->merge_pause_request_sequence) == request &&\n"
            "           !(atomic_load(&shard->merge_pause_ack) != 0U) &&\n"
            "           atomic_load(&shard->merge_pause_ack_sequence) == request &&\n"
            "           atomic_load(&shard->merge_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->merge_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_steal_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return llam_runtime_steal_pause_active(shard->runtime) &&\n"
            "           atomic_load(&shard->runtime->steal_pause_request_sequence) == wrong &&\n"
            "           atomic_load(&shard->steal_pause_ack) != 0U &&\n"
            "           atomic_load(&shard->steal_pause_request_sequence) == request &&\n"
            "           atomic_load(&shard->steal_pause_ack_sequence) == request &&\n"
            "           atomic_load(&shard->steal_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->steal_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    if (flag)\n"
            "        (void)llam_shard_merge_pause_ack_matches_locked(shard, 1U, 1U, 1U);\n"
            "    return atomic_load(&shard->merge_pause_ack_sequence) != 0U;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    if (guard) atomic_store(&shard->merge_pause_ack_sequence, request_sequence);\n"
            "    if (guard) atomic_store(&shard->merge_pause_ack_lease_epoch, epoch_value);\n"
            "    if (guard) atomic_store(&shard->merge_pause_ack_worker_incarnation, worker_incarnation);\n"
            "    atomic_store(&shard->merge_pause_ack, 1U);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    PUBLISH_PAUSE(shard);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_indirect_lease_owner_writes(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    llam_legacy_lease_t *lease_alias = &shard->legacy_lease;\n"
            "    if (lease_alias->owner != NULL) return;\n"
            "    lease_alias->owner = worker;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    llam_worker_t **slot = &shard->legacy_lease.owner;\n"
            "    if (shard->legacy_lease.owner != NULL) return;\n"
            "    *slot = worker;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    llam_worker_t **slot = &shard->legacy_lease.owner;\n"
            "    if (shard->legacy_lease.owner != NULL) return;\n"
            "    atomic_store(slot, worker);\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    llam_worker_t **slot = get_owner_slot(shard);\n"
            "    if (shard->legacy_lease.owner != NULL) return;\n"
            "    *slot = worker;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_compound_and_macro_hidden_owner_mutations(self) -> None:
        for expression in (
            "lease->owner += helper;",
            "++lease->owner;",
            "SET_OWNER(lease->owner, helper);",
        ):
            with self.subTest(expression=expression):
                self.assert_error(
                    "src/core/sched/scheduler.c",
                    "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
                    "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
                    "    if (lease->owner != NULL) return;\n"
                    "    if (!llam_worker_binding_live(helper, 1U)) return;\n"
                    "    if (lease->transfer_sequence == 0U) return;\n"
                    f"    {expression}\n"
                    "}\n",
                    "ownerless lease grant",
                )

    def test_rejects_fake_lease_alias_origin(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease;\n"
            "    llam_legacy_lease_t *real;\n"
            "    lease = fake_lease(shard);\n"
            "    real = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->owner != NULL) return;\n"
            "    if (!llam_worker_binding_live(helper, 1U)) return;\n"
            "    if (lease->transfer_sequence == 0U) return;\n"
            "    lease->owner = helper;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_conditional_owner_value_and_dead_proof(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (false) {\n"
            "        if (lease->owner != NULL) return;\n"
            "        if (!llam_worker_binding_live(helper, 1U)) return;\n"
            "        if (lease->transfer_sequence == 0U) return;\n"
            "    }\n"
            "    lease->owner = helper ? attacker : helper;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_marker_pointer_and_unknown_store_bypasses(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->owner != NULL) return;\n"
            "    if (!llam_worker_binding_live(attacker, 1U)) return;\n"
            "    (void)lease->transfer_sequence;\n"
            "    lease->owner = helper;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence ||\n"
            "        lease->owner != NULL ||\n"
            "        !llam_worker_binding_live(helper, incarnation)) return;\n"
            "    lease[attacker].owner = helper;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    llam_worker_t **slot;\n"
            "    slot = get_owner_slot(shard);\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence ||\n"
            "        lease->owner != NULL ||\n"
            "        !llam_worker_binding_live(helper, incarnation)) return;\n"
            "    atomic_store(slot, NULL);\n"
            "}\n",
            "ownerless lease grant",
        )

        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    uint64_t incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || lease->owner != NULL ||\n"
            "        helper != &shard->opaque_helper_worker ||\n"
            "        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER ||\n"
            "        helper->compat_shard != shard ||\n"
            "        !llam_worker_binding_live(helper, incarnation) ||\n"
            "        lease->state == LLAM_LEASE_STOPPING) return false;\n"
            "    if (guard) { lease->owner = helper; }\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_unrelated_lease_owner_clear(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_legacy_lease_t *lease) {\n"
            "    lease->owner = NULL;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_unproved_transition_and_unbind_owner_clears(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_clear_owner_locked(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation, uint64_t sequence) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    lease->owner = NULL;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    uint64_t incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || lease->owner != NULL ||\n"
            "        helper != &shard->opaque_helper_worker ||\n"
            "        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER ||\n"
            "        helper->compat_shard != shard ||\n"
            "        !llam_worker_binding_live(helper, incarnation) ||\n"
            "        lease->state == LLAM_LEASE_STOPPING) return false;\n"
            "    lease->owner = helper;\n"
            "    lease->owner_incarnation = attacker;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_enter_redirect_locked(llam_shard_t *shard, llam_worker_t *return_worker, uint64_t return_incarnation) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (flag) lease->owner = NULL;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_unbind_identity(llam_shard_t *shard, llam_worker_t *worker) {\n"
            "    uint64_t incarnation = 1U;\n"
            "    if (shard->legacy_lease.owner == worker)\n"
            "        shard->legacy_lease.owner = NULL;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_allows_direct_lease_declaration_initializer(self) -> None:
        self.assert_clean(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    uint64_t incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence ||\n"
            "        lease->owner != NULL ||\n"
            "        helper != &shard->opaque_helper_worker ||\n"
            "        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER ||\n"
            "        helper->compat_shard != shard ||\n"
            "        !llam_worker_binding_live(helper, incarnation) ||\n"
            "        lease->state == LLAM_LEASE_STOPPING) return false;\n"
            "    lease->owner = helper;\n"
            "}\n",
        )

    def test_covers_all_scheduler_lock_acquisition_forms_and_aliases(self) -> None:
        for call in ("pthread_mutex_trylock", "pthread_mutex_timedlock"):
            with self.subTest(call=call):
                self.assert_error(
                    "src/core/sched/bad.c",
                    f"void bad(llam_shard_t *shard) {{\n"
                    f"    {call}(&shard->lock, deadline);\n"
                    f"    {call}(&shard->opaque_lock, deadline);\n"
                    "}\n",
                    "opaque_lock must precede shard->lock",
                )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    llam_shard_t *s = shard;\n"
            "    pthread_mutex_lock(&s->lock);\n"
            "    pthread_mutex_lock(&s->opaque_lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *first, llam_shard_t *second) {\n"
            "    pthread_mutex_trylock(&first->lock);\n"
            "    pthread_mutex_trylock(&first->opaque_lock);\n"
            "    (void)second;\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *first, llam_shard_t *second) {\n"
            "    pthread_mutex_trylock(&first->lock);\n"
            "    pthread_mutex_trylock(&second->opaque_lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_clean(
            "src/engine/watchdog/watchdog_scale.c",
            "void merge(llam_shard_t *first, llam_shard_t *second) {\n"
            "    pthread_mutex_lock(&first->opaque_lock);\n"
            "    pthread_mutex_trylock(&second->opaque_lock);\n"
            "    pthread_mutex_timedlock(&first->lock, deadline);\n"
            "    pthread_mutex_lock(&second->lock);\n"
            "    pthread_mutex_unlock(&second->lock);\n"
            "    pthread_mutex_unlock(&first->lock);\n"
            "    pthread_mutex_unlock(&second->opaque_lock);\n"
            "    pthread_mutex_unlock(&first->opaque_lock);\n"
            "}\n",
        )

    def test_rejects_cross_shard_opaque_lock_reverse_order(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_scale.c",
            "void merge(llam_shard_t *first, llam_shard_t *second) {\n"
            "    pthread_mutex_lock(&second->opaque_lock);\n"
            "    pthread_mutex_lock(&first->opaque_lock);\n"
            "    pthread_mutex_lock(&first->lock);\n"
            "    pthread_mutex_lock(&second->lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_cross_shard_opaque_lock_reverse_order_through_aliases(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_scale.c",
            "void merge(llam_shard_t *first, llam_shard_t *second) {\n"
            "    llam_shard_t *a = first;\n"
            "    llam_shard_t *b = second;\n"
            "    pthread_mutex_lock(&b->opaque_lock);\n"
            "    pthread_mutex_lock(&a->opaque_lock);\n"
            "    pthread_mutex_lock(&a->lock);\n"
            "    pthread_mutex_lock(&b->lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_parenthesized_and_unknown_scheduler_lock_forms(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&(shard->lock));\n"
            "    pthread_mutex_lock(&(shard->opaque_lock));\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_unknown_and_indexed_scheduler_lock_forms(self) -> None:
        for source in (
            "void bad(llam_shard_t *shard) {\n"
            "    take_lock(&shard->lock);\n"
            "    take_lock(&shard->opaque_lock);\n"
            "}\n",
            "void bad(llam_shard_t *shard) {\n"
            "    void (*lock_fn)(void *) = 0;\n"
            "    lock_fn(&shard->lock);\n"
            "    lock_fn(&shard->opaque_lock);\n"
            "}\n",
            "void bad(llam_runtime_t *rt) {\n"
            "    pthread_mutex_lock(&rt->shards[i].lock);\n"
            "    pthread_mutex_lock(&rt->shards[j].opaque_lock);\n"
            "}\n",
        ):
            with self.subTest(source=source):
                self.assert_error(
                    "src/core/sched/bad.c",
                    source,
                    "opaque_lock must precede shard->lock",
                )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    lock_shard_lock(shard);\n"
            "    lock_shard_opaque(shard);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    acquire_shard_lock(shard);\n"
            "    acquire_shard_opaque(shard);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    SHARD_LOCK(shard);\n"
            "    SHARD_OPAQUE(shard);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_active_g_macro_publication_and_worker_reassignment(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    PUBLISH_ACTIVE_G(worker, task);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_worker_t *other, llam_task_t *task) {\n"
            "    worker = other;\n"
            "    atomic_store(&worker->active_g, task);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_active_g_unary_worker_movement(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    ++worker;\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_watchdog_conditional_failure_return(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    if (!llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread) ||\n"
            "        !llam_worker_binding_live(validated_owner, validated_incarnation) ||\n"
            "        validated_owner != owner || validated_incarnation != owner_incarnation ||\n"
            "        validated_epoch != epoch || pthread_equal(validated_native_thread, native_thread) == 0 ||\n"
            "        llam_worker_load_active_g(validated_owner) != current) {\n"
            "        if (other) return;\n"
            "    }\n"
            "    pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_goto_into_positive_guard(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    goto signal;\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "signal:\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_snapshot_output_address_escape(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    pthread_t *slot = &validated_native_thread;\n"
            "    *slot = attacker;\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_unbraced_conditional_snapshot(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    if (flag) llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_ack_tuple_field_routed_to_wrong_matcher_parameter(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    uint64_t request_sequence = 1U;\n"
            "    uint64_t epoch_value = 1U;\n"
            "    uint64_t worker_incarnation = atomic_load_explicit(&shard->merge_pause_ack_sequence, memory_order_acquire);\n"
            "    return llam_shard_merge_pause_ack_matches_locked(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_request_alias_write_before_matcher(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    uint64_t request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    uint64_t epoch_value = atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire);\n"
            "    uint64_t worker_incarnation = atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire);\n"
            "    uint64_t *slot = &request_sequence;\n"
            "    *slot = attacker;\n"
            "    return llam_shard_merge_pause_ack_matches_locked(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_publication_request_value_reassignment(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    request_sequence = attacker;\n"
            "    atomic_store(&shard->merge_pause_ack_sequence, request_sequence);\n"
            "    atomic_store(&shard->merge_pause_ack_lease_epoch, shard->legacy_lease.epoch);\n"
            "    atomic_store(&shard->merge_pause_ack_worker_incarnation, worker_incarnation);\n"
            "    atomic_store(&shard->merge_pause_ack, 1U);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_unknown_sampling_helper_macro(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    uint64_t request_sequence = 1U;\n"
            "    uint64_t epoch_value = 1U;\n"
            "    uint64_t worker_incarnation = 1U;\n"
            "    (void)atomic_load(&shard->merge_pause_ack_sequence);\n"
            "    return ACK_DECISION(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_matcher_with_wrong_steal_request_generation(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_shard_steal_pause_ack_matches_locked(llam_shard_t *shard, uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return llam_runtime_steal_pause_active(shard->runtime) &&\n"
            "           atomic_load(&shard->runtime->steal_pause_request_sequence) == wrong &&\n"
            "           atomic_load(&shard->steal_pause_ack) != 0U &&\n"
            "           atomic_load(&shard->steal_pause_request_sequence) == request &&\n"
            "           atomic_load(&shard->steal_pause_ack_sequence) == request &&\n"
            "           atomic_load(&shard->steal_pause_ack_lease_epoch) == epoch &&\n"
            "           atomic_load(&shard->steal_pause_ack_worker_incarnation) == incarnation;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_owner_marker_expressions_without_failure_guards(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    (void)(lease->owner != NULL);\n"
            "    (void)(lease->state != LLAM_LEASE_TRANSFER_TO_HELPER);\n"
            "    (void)(lease->transfer_sequence != sequence);\n"
            "    (void)(helper != &shard->opaque_helper_worker);\n"
            "    (void)(helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER);\n"
            "    (void)(helper->compat_shard != shard);\n"
            "    (void)llam_worker_binding_live(helper, incarnation);\n"
            "    lease->owner = helper;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_lease_pointer_movement(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->owner != NULL || lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || !llam_worker_binding_live(helper, incarnation)) return false;\n"
            "    ++lease;\n"
            "    lease->owner = helper;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_slot_unary_or_compound_mutation(self) -> None:
        for expression in ("(*slot)++;", "++(*slot);", "*slot += 1;"):
            with self.subTest(expression=expression):
                self.assert_error(
                    "src/core/sched/scheduler.c",
                    "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
                    "    llam_worker_t **slot = &shard->legacy_lease.owner;\n"
                    "    if (shard->legacy_lease.owner != NULL || !llam_worker_binding_live(helper, incarnation)) return false;\n"
                    f"    {expression}\n"
                    "}\n",
                    "ownerless lease grant",
                )

    def test_rejects_unrelated_designated_owner_initializer(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_worker_t *worker) {\n"
            "    llam_legacy_lease_t lease = { .owner = worker };\n"
            "    (void)lease;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_pointer_escape_after_direct_origin(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->owner != NULL || lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || !llam_worker_binding_live(helper, incarnation)) return false;\n"
            "    llam_legacy_lease_t **slot = &lease;\n"
            "    *slot = attacker;\n"
            "    lease->owner = helper;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_active_g_worker_address_escape(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_worker_t *other, llam_task_t *task) {\n"
            "    llam_worker_t **slot = &worker;\n"
            "    *slot = other;\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_active_g_slot_writeback_without_direct_rebind(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_worker_t *other, llam_task_t *task) {\n"
            "    llam_worker_t **slot;\n"
            "    slot = &worker;\n"
            "    *slot = other;\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_parenthesized_active_g_worker_address_escape(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    llam_worker_t **slot = &((worker));\n"
            "    (void)slot;\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_generic_scheduler_lock_wrappers(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *shard) {\n"
            "    lock_for_shard(shard);\n"
            "    opaque_for_shard(shard);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_noncanonical_index_expression_lock_order(self) -> None:
        for index in ("i + 0", "j + 0"):
            with self.subTest(index=index):
                self.assert_error(
                    "src/core/sched/bad.c",
                    "void bad(llam_runtime_t *rt) {\n"
                    "    pthread_mutex_lock(&rt->shards[i].lock);\n"
                    f"    pthread_mutex_lock(&rt->shards[{index}].opaque_lock);\n"
                    "}\n",
                    "opaque_lock must precede shard->lock",
                )

    def test_rejects_unordered_arbitrary_index_receivers(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_runtime_t *rt, unsigned i, unsigned j) {\n"
            "    pthread_mutex_lock(&rt->shards[i].lock);\n"
            "    pthread_mutex_lock(&rt->shards[j].opaque_lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_ack_matcher_arithmetic_argument(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard, uint64_t attacker) {\n"
            "    return llam_shard_merge_pause_ack_matches_locked(\n"
            "        shard, atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire) + attacker,\n"
            "        atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire),\n"
            "        atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire));\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_matcher_result_recombination(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard, bool attacker) {\n"
            "    bool matched = llam_shard_merge_pause_ack_matches_locked(\n"
            "        shard, atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire),\n"
            "        atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire),\n"
            "        atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire));\n"
            "    return attacker || matched;\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_alias_created_before_source_initialization(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard, uint64_t attacker) {\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t *request_slot = &request_sequence;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    *request_slot = attacker;\n"
            "    uint64_t epoch_value = atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire);\n"
            "    uint64_t worker_incarnation = atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire);\n"
            "    return llam_shard_merge_pause_ack_matches_locked(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_publication_tuple_overwrite_after_canonical_store(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard, uint64_t attacker) {\n"
            "    llam_worker_t *worker = g_llam_tls_worker;\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t incarnation;\n"
            "    if (shard == NULL || atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) == 0U) return;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, shard->legacy_lease.epoch, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation, incarnation, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_sequence, request_sequence, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, attacker, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_owner_proof_hidden_behind_conditional_flag(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence, bool flag) {\n"
            "    llam_legacy_lease_t *lease;\n"
            "    uint64_t incarnation;\n"
            "    lease = llam_shard_legacy_lease_locked(shard);\n"
            "    incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);\n"
            "    if (flag && (lease->owner != NULL || lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || helper != &shard->opaque_helper_worker ||\n"
            "        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER || helper->compat_shard != shard ||\n"
            "        !llam_worker_binding_live(helper, incarnation) || lease->state == LLAM_LEASE_STOPPING)) return false;\n"
            "    lease->owner = helper;\n"
            "    lease->owner_incarnation = incarnation;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_clear_proof_hidden_behind_conditional_flag(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_legacy_lease_clear_owner_locked(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation, uint64_t sequence, bool flag) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (flag && (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || lease->owner != worker ||\n"
            "        lease->owner_incarnation != incarnation || !llam_worker_binding_live(worker, incarnation))) return false;\n"
            "    lease->owner = NULL;\n"
            "    lease->owner_incarnation = 0U;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_nested_lease_rebind_before_owner_write(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence, bool flag) {\n"
            "    llam_legacy_lease_t *lease;\n"
            "    uint64_t incarnation;\n"
            "    lease = llam_shard_legacy_lease_locked(shard);\n"
            "    incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);\n"
            "    if (flag) { lease = attacker; }\n"
            "    if (lease->owner != NULL || lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || helper != &shard->opaque_helper_worker ||\n"
            "        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER || helper->compat_shard != shard ||\n"
            "        !llam_worker_binding_live(helper, incarnation) || lease->state == LLAM_LEASE_STOPPING) return false;\n"
            "    lease->owner = helper;\n"
            "    lease->owner_incarnation = incarnation;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_unknown_indirect_owner_null_slot(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_clear_owner_locked(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation, uint64_t sequence) {\n"
            "    llam_worker_t **slot = get_owner_slot(shard);\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER || lease->transfer_sequence != sequence ||\n"
            "        lease->owner != worker || lease->owner_incarnation != incarnation ||\n"
            "        !llam_worker_binding_live(worker, incarnation)) return;\n"
            "    *slot = NULL;\n"
            "    lease->owner_incarnation = 0U;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_watchdog_current_reassignment_after_snapshot(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    current = attacker_task;\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_conditional_snapshot_failure_guard(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    if (!llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread) && attacker)\n"
            "        return;\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_pre_snapshot_output_alias(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    pthread_t *native_slot = &native_thread;\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    *native_slot = attacker_thread;\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation))\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_mixed_shard_lock_and_pointer_hidden_lock_wrapper(self) -> None:
        fixtures = (
            "void bad(llam_shard_t *a, llam_shard_t *b) {\n"
            "    pthread_mutex_lock(&a->opaque_lock);\n"
            "    pthread_mutex_lock(&b->lock);\n"
            "}\n",
            "void bad(llam_shard_t *shard) {\n"
            "    pthread_mutex_t *p = &shard->lock;\n"
            "    pthread_mutex_t *q = &shard->opaque_lock;\n"
            "    take_mutex(p);\n"
            "    take_mutex(q);\n"
            "}\n",
        )
        for source in fixtures:
            with self.subTest(source=source):
                self.assert_error(
                    "src/core/sched/bad.c",
                    source,
                    "opaque_lock must precede shard->lock",
                )

    def test_production_hook_audit_rejects_external_definition(self) -> None:
        from audit_production_test_hooks import audit, _nm_command

        if _nm_command() is None:
            self.skipTest("nm/llvm-nm unavailable")
        with tempfile.TemporaryDirectory(prefix="llam-hook-audit-") as raw_root:
            root = Path(raw_root)
            source = root / "bad.c"
            artifact = root / "bad.o"
            source.write_text(
                "void llam_scheduler_test_external(void) {}\n",
                encoding="utf-8",
            )
            result = subprocess.run(
                ["cc", "-c", str(source), "-o", str(artifact)],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if result.returncode != 0:
                self.skipTest(f"cc unavailable: {result.stderr}")
            self.assertTrue(any("llam_scheduler_test_external" in v for v in audit((artifact,))))

    def test_production_hook_audit_falls_back_to_llvm_nm(self) -> None:
        from audit_production_test_hooks import _nm_command
        from unittest.mock import patch

        def which(name: str) -> str | None:
            return "/usr/bin/llvm-nm" if name == "llvm-nm" else None

        with patch("audit_production_test_hooks.shutil.which", side_effect=which):
            self.assertEqual("/usr/bin/llvm-nm", _nm_command())

    def test_production_hook_audit_covers_scheduler_test_hooks(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            "$(PYTHON) scripts/audit_production_test_hooks.py ./libllam_runtime.a ./$(SHLIB_REAL)",
            makefile,
        )
        self.assertNotIn("grep -E", makefile)
        self.assertIn(
            "$(PYTHON) scripts/audit_shared_exports.py ./$(SHLIB_REAL)",
            makefile,
        )
        self.assertIn("audit_production_test_hooks", cmake)
        self.assertIn("scripts/audit_production_test_hooks.py", cmake)

    def test_cmake_production_hook_audit_allows_only_local_fallbacks(self) -> None:
        symbols = list(
            _symbols(
                "foo.o:\n"
                "00000000 t _llam_legacy_lease_test_pause\n"
                "00000000 T _llam_scheduler_test_rebind_primary\n"
                "00000000 T _llam_broker_client_self_test_exchange\n"
            )
        )
        local_type, local_name = symbols[0]
        exported_type, exported_name = symbols[1]
        safe_type, safe_name = symbols[2]
        self.assertTrue(HOOK_RE.fullmatch(local_name))
        self.assertEqual("t", local_type)
        self.assertTrue(HOOK_RE.fullmatch(exported_name))
        self.assertEqual("T", exported_type)
        self.assertTrue(HOOK_RE.fullmatch(safe_name))
        self.assertTrue(SAFE_SELF_TEST_RE.match(safe_name))
        self.assertEqual("T", safe_type)

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

    def test_rejects_ack_matcher_owner_omission_inversion_and_parameter_escape(self) -> None:
        prefix = (
            "bool llam_shard_merge_pause_ack_matches_locked(const llam_shard_t *shard, "
            "uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return shard != NULL && request != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire) == epoch &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire) == incarnation%s;\n"
            "}\n"
        )
        fixtures = (
            ("", "owner helper omitted"),
            (" && llam_shard_pause_owner_matches_locked(shard, epoch, incarnation) == 0U", "owner helper inverted"),
            (" && llam_shard_pause_owner_matches_locked(attacker, epoch, incarnation)", "owner receiver attacker"),
            (" && llam_shard_pause_owner_matches_locked(shard, attacker, incarnation)", "owner epoch attacker"),
            (" && llam_shard_pause_owner_matches_locked(shard, epoch, attacker)", "owner incarnation attacker"),
        )
        for suffix, description in fixtures:
            with self.subTest(description=description):
                self.assert_error(
                    "src/core/sched/scheduler.c",
                    prefix % suffix,
                    "unversioned pause acknowledgement",
                )

    def test_rejects_ack_matcher_and_sampler_receiver_rebinding(self) -> None:
        matcher = (
            "bool llam_shard_merge_pause_ack_matches_locked(const llam_shard_t *shard, "
            "uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return shard != NULL && request != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire) == epoch &&\n"
            "           atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire) == incarnation &&\n"
            "           llam_shard_pause_owner_matches_locked(shard, epoch, incarnation);\n"
            "}\n"
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            matcher.replace("const llam_shard_t *shard,", "const llam_shard_t *shard,")
            .replace("    return shard != NULL", "    shard = attacker_shard;\n"
                     "    return shard != NULL"),
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            matcher.replace(
                "    return shard != NULL",
                "    uint64_t *slot = &epoch;\n"
                "    return shard != NULL",
            ),
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard, uint64_t attacker) {\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t epoch_value;\n"
            "    uint64_t worker_incarnation;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    epoch_value = atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire);\n"
            "    worker_incarnation = atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire);\n"
            "    shard = attacker_shard;\n"
            "    return llam_shard_merge_pause_ack_matches_locked(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_sampling_and_publication_provenance_escapes(self) -> None:
        sampler = (
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t epoch_value;\n"
            "    uint64_t worker_incarnation;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    epoch_value = atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire);\n"
            "    worker_incarnation = atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire);\n"
            "    return llam_shard_merge_pause_ack_matches_locked(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n"
        )
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            sampler.replace(
                "    return llam_shard_merge_pause_ack_matches_locked",
                "    llam_shard_t **slot = &shard;\n"
                "    *slot = attacker_shard;\n"
                "    return llam_shard_merge_pause_ack_matches_locked",
            ),
            "unversioned pause acknowledgement",
        )
        publication = (
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    llam_worker_t *worker = g_llam_tls_worker;\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t incarnation;\n"
            "    if (shard == NULL || atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) == 0U) return;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, shard->legacy_lease.epoch, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation, incarnation, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_sequence, request_sequence, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);\n"
            "}\n"
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            publication.replace(
                "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);",
                "    llam_worker_t **slot = &worker;\n"
                "    *slot = attacker_worker;\n"
                "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);",
            ),
            "unversioned pause acknowledgement",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            publication.replace(
                "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);",
                "    atomic_store_explicit(&shard->merge_pause_ack_sequence, attacker, memory_order_release);\n"
                "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);",
            ),
            "unversioned pause acknowledgement",
        )

    def test_rejects_owner_noncanonical_lvalues_and_identity_rebinding(self) -> None:
        canonical = (
            "bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    llam_legacy_lease_t *lease;\n"
            "    uint64_t incarnation;\n"
            "    lease = llam_shard_legacy_lease_locked(shard);\n"
            "    incarnation = atomic_load_explicit(&helper->incarnation, memory_order_acquire);\n"
            "    if (lease->owner != NULL || lease->state != LLAM_LEASE_TRANSFER_TO_HELPER ||\n"
            "        lease->transfer_sequence != sequence || helper != &shard->opaque_helper_worker ||\n"
            "        helper->role != LLAM_WORKER_ROLE_OPAQUE_HELPER || helper->compat_shard != shard ||\n"
            "        !llam_worker_binding_live(helper, incarnation) || lease->state == LLAM_LEASE_STOPPING) return false;\n"
            "%s"
            "    lease->owner_incarnation = incarnation;\n"
            "    return true;\n"
            "}\n"
        )
        for body, description in (
            ("    (lease->owner) = helper;\n", "parenthesized owner assignment"),
            ("    (lease->owner) += 1;\n", "parenthesized owner compound"),
            ("    lease->owner = helper;\n    shard = attacker_shard;\n", "shard rebind after proof"),
            ("    lease->owner = helper;\n    uint64_t *slot = &sequence;\n", "sequence address escape"),
        ):
            with self.subTest(description=description):
                self.assert_error(
                    "src/core/sched/scheduler.c",
                    canonical % body,
                    "ownerless lease grant",
                )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, llam_worker_t **slot, uint64_t sequence) {\n"
            "    ++*slot;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_lock_temporal_aliases_indexed_rebinds_and_unknown_wrappers(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t *a, llam_shard_t *b) {\n"
            "    llam_shard_t *x = a;\n"
            "    pthread_mutex_lock(&x->opaque_lock);\n"
            "    x = b;\n"
            "    pthread_mutex_lock(&x->lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_runtime_t *rt, unsigned i, unsigned j) {\n"
            "    unsigned x = i;\n"
            "    pthread_mutex_lock(&rt->shards[x].opaque_lock);\n"
            "    x = j;\n"
            "    pthread_mutex_lock(&rt->shards[x].lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void acquire(llam_shard_t *s) { pthread_mutex_lock(&s->lock); }\n"
            "void acquire2(llam_shard_t *s) { pthread_mutex_lock(&s->opaque_lock); }\n"
            "void bad(llam_shard_t *s) { acquire(s); acquire2(s); }\n",
            "opaque_lock must precede shard->lock",
        )
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_shard_t **p) {\n"
            "    pthread_mutex_lock(&(*p)->opaque_lock);\n"
            "    pthread_mutex_lock(&(*p)->lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_active_g_noncanonical_operations_and_parameter_escapes(self) -> None:
        fixtures = (
            (
                "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
                "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
                "    atomic_store_explicit(&worker->active_g, attacker, memory_order_release);\n"
                "}\n",
                "extra publication store",
            ),
            (
                "llam_task_t *llam_worker_load_active_g(const llam_worker_t *worker) {\n"
                "    atomic_store_explicit(&worker->active_g, attacker, memory_order_release);\n"
                "    return atomic_load_explicit(&worker->active_g, memory_order_acquire);\n"
                "}\n",
                "load helper store",
            ),
            (
                "void llam_worker_clear_active_g(llam_worker_t *worker) {\n"
                "    atomic_store_explicit(&worker->active_g, attacker, memory_order_relaxed);\n"
                "}\n",
                "wrong clear value/order",
            ),
            (
                "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
                "    worker = attacker_worker;\n"
                "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
                "}\n",
                "worker rebind",
            ),
            (
                "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
                "    llam_worker_t **slot = &worker;\n"
                "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
                "}\n",
                "worker address escape",
            ),
        )
        for source, description in fixtures:
            with self.subTest(description=description):
                self.assert_error(
                    "src/core/sched/scheduler.c",
                    source,
                    "Phase 0 execution authority member: active_g",
                )

    def test_rejects_watchdog_output_and_protection_wrapper_escape(self) -> None:
        source = (
            "void drop_protection(llam_shard_t *s) {\n"
            "    pthread_mutex_unlock(&s->lock);\n"
            "    pthread_mutex_unlock(&s->opaque_lock);\n"
            "}\n"
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    pthread_t *slot = &validated_native_thread;\n"
            "    drop_protection(shard);\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation))\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "}\n"
        )
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            source,
            "watchdog target outside lease lifetime",
        )

    def test_rejects_steal_matcher_without_positive_owner_tuple(self) -> None:
        base = (
            "bool llam_shard_steal_pause_ack_matches_locked(const llam_shard_t *shard, "
            "uint64_t request, uint64_t epoch, uint64_t incarnation) {\n"
            "    return shard != NULL && request != 0U &&\n"
            "           llam_runtime_steal_pause_active(shard->runtime) &&\n"
            "           atomic_load_explicit(&shard->runtime->steal_pause_request_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->steal_pause_ack, memory_order_acquire) != 0U &&\n"
            "           atomic_load_explicit(&shard->steal_pause_request_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->steal_pause_ack_sequence, memory_order_acquire) == request &&\n"
            "           atomic_load_explicit(&shard->steal_pause_ack_lease_epoch, memory_order_acquire) == epoch &&\n"
            "           atomic_load_explicit(&shard->steal_pause_ack_worker_incarnation, memory_order_acquire) == incarnation%s;\n"
            "}\n"
        )
        for suffix in (
            "",
            " && llam_shard_pause_owner_matches_locked(shard, epoch, incarnation) == 0U",
        ):
            with self.subTest(suffix=suffix):
                self.assert_error(
                    "src/core/sched/scheduler.c",
                    base % suffix,
                    "unversioned pause acknowledgement",
                )

    def test_rejects_ack_tuple_temporary_parenthesized_rebind(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_merge.c",
            "bool llam_shard_wait_merge_pause_ack(llam_shard_t *shard) {\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t epoch_value;\n"
            "    uint64_t worker_incarnation;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    epoch_value = atomic_load_explicit(&shard->merge_pause_ack_lease_epoch, memory_order_acquire);\n"
            "    worker_incarnation = atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire);\n"
            "    (epoch_value) = attacker;\n"
            "    return llam_shard_merge_pause_ack_matches_locked(shard, request_sequence, epoch_value, worker_incarnation);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_publication_relaxed_or_indirect_tuple_store(self) -> None:
        source = (
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    llam_worker_t *worker = g_llam_tls_worker;\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t incarnation;\n"
            "    if (shard == NULL || atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) == 0U) return;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, shard->legacy_lease.epoch, memory_order_relaxed);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation, incarnation, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_sequence, request_sequence, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);\n"
            "}\n"
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            source,
            "unversioned pause acknowledgement",
        )

    def test_rejects_ack_publication_tuple_address_alias_before_origin(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_scheduler_publish_merge_pause_ack_locked(llam_shard_t *shard) {\n"
            "    llam_worker_t *worker = g_llam_tls_worker;\n"
            "    uint64_t request_sequence;\n"
            "    uint64_t incarnation;\n"
            "    uint64_t *slot = &request_sequence;\n"
            "    if (shard == NULL || atomic_load_explicit(&shard->merge_pause_requested, memory_order_acquire) == 0U) return;\n"
            "    request_sequence = atomic_load_explicit(&shard->merge_pause_request_sequence, memory_order_acquire);\n"
            "    *slot = attacker;\n"
            "    incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_lease_epoch, shard->legacy_lease.epoch, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_worker_incarnation, incarnation, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack_sequence, request_sequence, memory_order_release);\n"
            "    atomic_store_explicit(&shard->merge_pause_ack, 1U, memory_order_release);\n"
            "}\n",
            "unversioned pause acknowledgement",
        )

    def test_rejects_owner_cast_comma_and_null_slot_lvalues(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_clear_owner_locked(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation, uint64_t sequence) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER || lease->transfer_sequence != sequence ||\n"
            "        lease->owner != worker || lease->owner_incarnation != incarnation ||\n"
            "        !llam_worker_binding_live(worker, incarnation)) return false;\n"
            "    ((llam_legacy_lease_t *)lease)->owner = NULL;\n"
            "    lease->owner_incarnation = 0U;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_clear_owner_locked(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation, uint64_t sequence) {\n"
            "    llam_worker_t **slot = get_owner_slot(shard);\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->state != LLAM_LEASE_TRANSFER_TO_HELPER || lease->transfer_sequence != sequence ||\n"
            "        lease->owner != worker || lease->owner_incarnation != incarnation ||\n"
            "        !llam_worker_binding_live(worker, incarnation)) return false;\n"
            "    *slot = NULL;\n"
            "    lease->owner_incarnation = 0U;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_designated_initializer_in_transition_helper(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper, uint64_t sequence) {\n"
            "    llam_legacy_lease_t replacement = { .owner = helper };\n"
            "    (void)replacement;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_owner_incarnation_and_claimant_parameter_rebind(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "bool llam_legacy_lease_reacquire_primary_locked(llam_shard_t *shard, llam_worker_t *worker, uint64_t incarnation) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    incarnation = attacker_incarnation;\n"
            "    lease->owner = worker;\n"
            "    lease->owner_incarnation = incarnation;\n"
            "    return true;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_rejects_watchdog_current_pre_snapshot_rebind(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    current = attacker_task;\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_watchdog_unknown_signal_window_call(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &owner_incarnation, &epoch, &native_thread);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &validated_native_thread);\n"
            "    if (validated_owner == owner && validated_incarnation == owner_incarnation &&\n"
            "        validated_epoch == epoch && pthread_equal(validated_native_thread, native_thread) &&\n"
            "        llam_worker_load_active_g(validated_owner) == current &&\n"
            "        llam_worker_binding_live(validated_owner, validated_incarnation)) {\n"
            "        unknown_signal_window_helper(shard);\n"
            "        pthread_kill(validated_native_thread, LLAM_PREEMPT_SIGNAL);\n"
            "    }\n"
            "}\n",
            "watchdog target outside lease lifetime",
        )

    def test_rejects_active_g_conditional_and_unknown_helper_operations(self) -> None:
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    if (attacker) atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )
        self.assert_error(
            "src/core/sched/scheduler.c",
            "void llam_worker_publish_active_g(llam_worker_t *worker, llam_task_t *task) {\n"
            "    unknown_active_g_helper(worker);\n"
            "    atomic_store_explicit(&worker->active_g, task, memory_order_release);\n"
            "}\n",
            "Phase 0 execution authority member: active_g",
        )

    def test_rejects_lock_alias_rebind_after_first_acquisition(self) -> None:
        self.assert_error(
            "src/engine/watchdog/watchdog_scale.c",
            "void merge(llam_shard_t *first, llam_shard_t *second) {\n"
            "    llam_shard_t *alias = first;\n"
            "    pthread_mutex_lock(&alias->opaque_lock);\n"
            "    alias = second;\n"
            "    pthread_mutex_lock(&alias->opaque_lock);\n"
            "    pthread_mutex_lock(&alias->lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )

    def test_rejects_arbitrary_indexed_lock_receiver(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_runtime_t *rt, unsigned i) {\n"
            "    pthread_mutex_lock(&rt->shards[get_index(i)].opaque_lock);\n"
            "    pthread_mutex_lock(&rt->shards[get_index(i)].lock);\n"
            "}\n",
            "opaque_lock must precede shard->lock",
        )


if __name__ == "__main__":
    unittest.main()
