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
            "           atomic_load_explicit(&shard->merge_pause_ack_worker_incarnation, memory_order_acquire) == incarnation;\n"
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

    def test_allows_lease_bound_watchdog_signal(self) -> None:
        self.assert_clean(
            "src/engine/watchdog/watchdog_probe.c",
            "void llam_watchdog_check_shard(llam_shard_t *shard) {\n"
            "    pthread_mutex_lock(&shard->opaque_lock);\n"
            "    pthread_mutex_lock(&shard->lock);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &owner, &incarnation, &epoch, &original_target);\n"
            "    llam_legacy_lease_snapshot_watchdog_locked(shard, &validated_owner, &validated_incarnation, &validated_epoch, &native_target);\n"
            "    if (llam_worker_binding_live(validated_owner, validated_incarnation) && pthread_equal(native_target, original_target)) {\n"
            "        pthread_kill(native_target, LLAM_PREEMPT_SIGNAL);\n"
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

    def test_rejects_unrelated_lease_owner_clear(self) -> None:
        self.assert_error(
            "src/core/sched/bad.c",
            "void bad(llam_legacy_lease_t *lease) {\n"
            "    lease->owner = NULL;\n"
            "}\n",
            "ownerless lease grant",
        )

    def test_allows_direct_lease_declaration_initializer(self) -> None:
        self.assert_clean(
            "src/core/sched/scheduler.c",
            "void llam_legacy_lease_grant_helper_locked(llam_shard_t *shard, llam_worker_t *helper) {\n"
            "    llam_legacy_lease_t *lease = llam_shard_legacy_lease_locked(shard);\n"
            "    if (lease->owner != NULL) return;\n"
            "    if (!llam_worker_binding_live(helper, 1U)) return;\n"
            "    if (lease->transfer_sequence == 0U) return;\n"
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

    def test_production_hook_audit_covers_scheduler_test_hooks(self) -> None:
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        self.assertIn(
            "llam_[[:alnum:]_]*_test_[[:alnum:]_]*",
            makefile,
        )
        self.assertIn("llam_scheduler_test_rebind_primary", makefile)
        self.assertIn(
            "$(PYTHON) scripts/audit_shared_exports.py ./$(SHLIB_REAL)",
            makefile,
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
