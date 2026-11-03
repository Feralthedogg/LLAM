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
 * See LICENSES/Apache-2.0.txt.
 */

#include "runtime_internal.h"
#include "engine/runtime_watchdog_internal.h"

#include <stdatomic.h>
#include <stdio.h>

#if !LLAM_PLATFORM_WINDOWS
#include <signal.h>
#endif

#if !LLAM_PLATFORM_WINDOWS
static void relief_test_signal_handler(int signal_number) {
    (void)signal_number;
}
#endif

static int fail(const char *message) {
    fprintf(stderr, "test_relief_worker_internal: %s\n", message);
    return 1;
}

static int create_runtime(llam_runtime_t **out) {
    llam_runtime_opts_t opts;

    *out = NULL;
    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_poll_period = 1U;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    return llam_runtime_create(&opts, sizeof(opts), out);
}

int main(void) {
    llam_runtime_t *runtime = NULL;
    llam_shard_t *shard;
    llam_worker_t *worker;
    llam_task_t *task = NULL;
    llam_processor_owner_ticket_t ticket;
    llam_relief_defer_reason_t reason;
    int result = 1;
#if !LLAM_PLATFORM_WINDOWS
    struct sigaction previous_preempt_action;
    bool preempt_action_installed = false;
#endif

    llam_scheduler_clear_identity();
    if (create_runtime(&runtime) != 0 || runtime == NULL ||
        runtime->shards == NULL || runtime->active_shards == 0U) {
        llam_runtime_destroy(runtime);
        return fail("could not create relief-eligibility runtime");
    }
    shard = &runtime->shards[0];
    worker = &shard->primary_worker;
    task = llam_task_alloc(shard);
    if (task == NULL) {
        llam_runtime_destroy(runtime);
        return fail("could not allocate relief-eligibility task");
    }
    llam_scheduler_bind_identity(shard, worker, &shard->scheduler_ctx);
    g_llam_tls_task = task;
    atomic_store_explicit(&worker->run_generation, 1U, memory_order_release);
    atomic_store_explicit(&task->state, LLAM_TASK_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&task->run_generation, 1U, memory_order_release);
    llam_worker_publish_active_g(worker, task);

    if (!llam_processor_relief_snapshot(shard,
                                        worker,
                                        task,
                                        &ticket,
                                        &reason) ||
        reason != LLAM_RELIEF_DEFER_NONE ||
        ticket.owner_m != worker || ticket.processor != &shard->processor ||
        atomic_load_explicit(&worker->current_p, memory_order_acquire) !=
            &shard->processor) {
        fail("safe managed G with a current P ticket was not relief-eligible");
        goto cleanup;
    }

    atomic_store_explicit(&worker->preempt_disable_depth, 1U, memory_order_release);
    if (llam_processor_relief_snapshot(shard,
                                       worker,
                                       task,
                                       &ticket,
                                       &reason) ||
        reason != LLAM_RELIEF_DEFER_PREEMPT_DISABLED) {
        fail("preemption-disabled M was not rejected with an explicit reason");
        goto cleanup;
    }
    atomic_store_explicit(&worker->preempt_disable_depth, 0U, memory_order_release);

    atomic_store_explicit(&worker->runtime_critical_depth, 1U, memory_order_release);
    atomic_store_explicit(&worker->execution_state,
                          LLAM_WORKER_EXEC_RUNTIME_UNSAFE,
                          memory_order_release);
    if (llam_processor_relief_snapshot(shard,
                                       worker,
                                       task,
                                       &ticket,
                                       &reason) ||
        reason != LLAM_RELIEF_DEFER_RUNTIME_CRITICAL) {
        fail("runtime-critical M was not rejected with an explicit reason");
        goto cleanup;
    }
    atomic_store_explicit(&worker->runtime_critical_depth, 0U, memory_order_release);

    atomic_store_explicit(&worker->execution_state,
                          (unsigned)99U,
                          memory_order_release);
    if (llam_processor_relief_snapshot(shard,
                                       worker,
                                       task,
                                       &ticket,
                                       &reason) ||
        reason != LLAM_RELIEF_DEFER_UNKNOWN_STATE) {
        fail("unknown M state was not rejected fail-closed");
        goto cleanup;
    }

    atomic_store_explicit(&worker->execution_state,
                          LLAM_WORKER_EXEC_MANAGED_SAFE,
                          memory_order_release);
#if !LLAM_PLATFORM_WINDOWS
    {
        struct sigaction preempt_action;
        uint64_t now_ns = llam_now_ns();
        uint64_t last_safepoint_ns = now_ns > UINT64_C(1000000000)
                                         ? now_ns - UINT64_C(1000000000)
                                         : 1U;

        memset(&preempt_action, 0, sizeof(preempt_action));
        preempt_action.sa_handler = relief_test_signal_handler;
        if (sigemptyset(&preempt_action.sa_mask) != 0 ||
            sigaction(LLAM_PREEMPT_SIGNAL,
                      &preempt_action,
                      &previous_preempt_action) != 0) {
            fail("could not install the bounded no-op watchdog signal handler");
            goto cleanup;
        }
        preempt_action_installed = true;
        atomic_store_explicit(&worker->last_safepoint_ns,
                              last_safepoint_ns,
                              memory_order_release);
        atomic_store_explicit(&shard->processor.inject_depth,
                              1U,
                              memory_order_release);
        atomic_store_explicit(&task->preempt_requested, 0U, memory_order_release);
        llam_watchdog_check_shard(shard, now_ns);
        atomic_store_explicit(&shard->processor.inject_depth, 0U, memory_order_release);
        atomic_store_explicit(&task->preempt_requested, 0U, memory_order_release);
        atomic_store_explicit(&worker->last_safepoint_ns,
                              llam_now_ns(),
                              memory_order_release);
        if (atomic_load_explicit(&shard->metrics.relief_deferred_unannounced,
                                 memory_order_acquire) != 1U ||
            atomic_load_explicit(&shard->metrics.preempt_signals,
                                 memory_order_acquire) != 1U ||
            atomic_load_explicit(&worker->current_p, memory_order_acquire) !=
                &shard->processor ||
            llam_worker_load_active_g(worker) != task ||
            worker->foreign_token.state != LLAM_FOREIGN_TOKEN_INACTIVE ||
            shard->opaque_helper_thread_started) {
            fail("overdue no-safepoint G was not kept on its cooperative M/P owner");
            goto cleanup;
        }
        (void)sigaction(LLAM_PREEMPT_SIGNAL, &previous_preempt_action, NULL);
        preempt_action_installed = false;
    }
#endif

    atomic_store_explicit(&worker->current_p, NULL, memory_order_release);
    if (llam_processor_relief_snapshot(shard,
                                       worker,
                                       task,
                                       &ticket,
                                       &reason) ||
        reason != LLAM_RELIEF_DEFER_STALE_P_TICKET) {
        fail("missing current-P publication was not rejected");
        goto cleanup;
    }
    result = 0;

cleanup:
#if !LLAM_PLATFORM_WINDOWS
    if (preempt_action_installed) {
        (void)sigaction(LLAM_PREEMPT_SIGNAL, &previous_preempt_action, NULL);
    }
#endif
    atomic_store_explicit(&worker->runtime_critical_depth, 0U, memory_order_release);
    atomic_store_explicit(&worker->preempt_disable_depth, 0U, memory_order_release);
    atomic_store_explicit(&worker->current_p, &shard->processor, memory_order_release);
    atomic_store_explicit(&worker->execution_state,
                          LLAM_WORKER_EXEC_MANAGED_SAFE,
                          memory_order_release);
    llam_scheduler_unbind_identity(shard, worker);
    llam_scheduler_clear_identity();
    if (task != NULL) {
        llam_task_allocator_free(task);
        llam_allocator_quiescent(shard);
    }
    llam_runtime_destroy(runtime);
    if (result == 0) {
        puts("test_relief_worker_internal: fail-closed M/P checks passed");
    }
    return result;
}
