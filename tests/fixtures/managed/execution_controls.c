/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#include "execution_controls.h"
#include "llam/executor.h"
#include "llam/managed.h"

__attribute__((annotate("llam-native-affine"))) void execution_fixture_abort(void);

static void execution_controls_error(execution_controls_work_t *work) {
    atomic_store_explicit(&work->errors, 1U, memory_order_release);
}

void fixture_critical_peer(void *argument) {
    execution_controls_work_t *work = argument;
    atomic_store_explicit(&work->peer_ready, 1U, memory_order_release);
    for (uint64_t i = 0U; i != UINT64_C(2000000000); ++i) {
        unsigned phase = atomic_load_explicit(&work->phase, memory_order_acquire);
        if (phase == 2U) execution_controls_error(work);
        if (phase >= 3U) {
            atomic_store_explicit(&work->peer_done, 1U, memory_order_release);
            return;
        }
    }
    execution_controls_error(work);
}

static int fixture_critical_begin(execution_controls_work_t *work) {
    for (uint64_t i = 0U; i != UINT64_C(2000000000); ++i) {
        if (atomic_load_explicit(&work->peer_ready, memory_order_acquire)) {
            if (llam_preempt_region_enter() != 0) break;
            if (llam_preempt_region_enter() == 0) return 0;
            (void)llam_preempt_region_leave();
            break;
        }
    }
    execution_controls_error(work);
    atomic_store_explicit(&work->phase, 3U, memory_order_release);
    return -1;
}

void fixture_critical_entry(void *argument) {
    execution_controls_work_t *work = argument;
    /* Both rendezvous loops are call-free before entry. One P cannot advance
     * both tasks without real native capture outside the critical region. */
    if (fixture_critical_begin(work) != 0) return;
    if (llam_sleep_ns(1U) != -1) execution_controls_error(work);
    atomic_store_explicit(&work->phase, 2U, memory_order_release);
    uint64_t i = 0U;
    for (; i != UINT64_C(2000000000); ++i) {
        if (atomic_load_explicit(&work->probe_done, memory_order_acquire)) break;
    }
    work->iterations = i;
    if (i == UINT64_C(2000000000)) execution_controls_error(work);
    if (llam_preempt_region_leave() != 0 || llam_sleep_ns(1U) != -1) execution_controls_error(work);
    atomic_store_explicit(&work->phase, 3U, memory_order_release);
    if (llam_preempt_region_leave() != 0) execution_controls_error(work);
    if (llam_cancel_token_is_cancelled(work->token) == 1 && llam_sleep_ns(UINT64_C(1000000)) == -1)
        atomic_store_explicit(&work->cancellation_seen, 1U, memory_order_release);
    for (i = 0U; i != UINT64_C(2000000000); ++i) {
        if (atomic_load_explicit(&work->peer_done, memory_order_acquire)) return;
    }
    execution_controls_error(work);
}

static void fixture_local_error(local_executor_native_work_t *work) {
    atomic_store_explicit(&work->errors, 1U, memory_order_release);
}

static uint64_t fixture_local_identity(local_executor_native_work_t *work) {
    void *identity = NULL;
    if (llam_call_native_affine_result(work->identity, NULL, &identity) != 0) {
        fixture_local_error(work);
        return 0U;
    }
    return (uint64_t)(uintptr_t)identity;
}

static void fixture_local_child(void *argument) {
    local_executor_native_child_t *child = argument;
    local_executor_native_work_t *work = child->work;
    if (llam_task_local_set_owned(work->key, child, work->cleanup) != 0) fixture_local_error(work);
    child->before = fixture_local_identity(work);
    uint64_t i;
    for (i = 0U; i != UINT64_C(2000000000); ++i)
        if (atomic_load_explicit(&work->go, memory_order_acquire)) break;
    if (i == UINT64_C(2000000000)) fixture_local_error(work);
    llam_yield();
    if (llam_sleep_ns(UINT64_C(1000000)) != 0) fixture_local_error(work);
    child->after = fixture_local_identity(work);
    if (llam_preempt_region_enter() != 0) fixture_local_error(work);
    else {
        work->shared_value += 1U;
        if (llam_preempt_region_leave() != 0) fixture_local_error(work);
    }
    child->value = 42U;
    atomic_store_explicit(&child->ready, 1U, memory_order_release);
    for (i = 0U; i != UINT64_C(10000); ++i) {
        if (atomic_load_explicit(&work->release_children, memory_order_acquire)) return;
        if (llam_sleep_ns(UINT64_C(1000000)) != 0) fixture_local_error(work);
    }
    execution_fixture_abort();
}

static bool fixture_local_wait_children(local_executor_native_child_t children[3]) {
    for (uint64_t i = 0U; i != UINT64_C(2000000000); ++i) {
        if (atomic_load_explicit(&children[0].ready, memory_order_acquire) &&
            atomic_load_explicit(&children[1].ready, memory_order_acquire) &&
            atomic_load_explicit(&children[2].ready, memory_order_acquire)) return true;
    }
    return false;
}

void fixture_local_root(void *argument) {
    local_executor_native_work_t *work = argument;
    work->root_identity = fixture_local_identity(work);
    llam_task_group_t *group = llam_runtime_task_group_create(work->runtime);
    if (group == NULL) { fixture_local_error(work); return; }
    local_executor_native_child_t children[3] = {0};
    llam_spawn_opts_t options = {0};
    options.local_executor = work->executor;
    for (unsigned i = 0U; i != 3U; ++i) {
        children[i].work = work;
        atomic_init(&children[i].ready, 0U);
        if (llam_task_group_spawn_ex(group, fixture_local_child, &children[i], &options, sizeof(options)) == NULL)
            fixture_local_error(work);
    }
    atomic_store_explicit(&work->go, 1U, memory_order_release);
    /* No call/poll here: only async capture can let same-M children finish. */
    if (!fixture_local_wait_children(children)) execution_fixture_abort();
    if (llam_local_executor_close(work->executor) != 0 || llam_cancel_token_cancel(work->token) != 0)
        fixture_local_error(work);
    atomic_store_explicit(&work->draining, 1U, memory_order_release);
    if (llam_task_group_drain(group) != 0) execution_fixture_abort();
    for (unsigned i = 0U; i != 3U; ++i) {
        if (children[i].value != 43U || children[i].before != work->root_identity ||
            children[i].after != work->root_identity || children[i].cleanup != work->root_identity)
            execution_fixture_abort();
    }
    if (work->root_identity == 0U || work->shared_value != 3U || llam_task_group_destroy(group) != 0)
        fixture_local_error(work);
    atomic_store_explicit(&work->checked, 1U, memory_order_release);
}
