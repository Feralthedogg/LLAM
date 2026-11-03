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
#include "runtime_test_watchdog.h"
#include "engine/runtime_watchdog_internal.h"
#if (LLAM_ASYNC_CONTEXT_SHARED && LLAM_ARCH_AARCH64)
#include "async_context_phase.h"
#include "fixtures/async_darwin_leaf.h"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define WATCHDOG_LEAF_ITERATIONS UINT64_C(300000000)

typedef struct watchdog_probe {
    llam_runtime_t *runtime;
    llam_task_t *task;
    llam_watchdog_test_gate_t gate;
    atomic_uint ran;
    atomic_uint errors;
    bool leaf;
    bool cycle;
    llam_mutex_t *mutexes[2];
    llam_task_t *cycle_tasks[2];
    atomic_uint locked;
    uint64_t result;
    int run_result;
    int run_error;
} watchdog_probe_t;

static void watchdog_cycle_task(watchdog_probe_t *probe, unsigned index) {
    bool owns_first = llam_mutex_lock(probe->mutexes[index]) == 0;
    bool owns_second = false;
    if (!owns_first) {
        atomic_fetch_add(&probe->errors, 1U);
    } else {
        atomic_fetch_add_explicit(&probe->locked, 1U, memory_order_release);
        while (atomic_load_explicit(&probe->locked, memory_order_acquire) != 2U &&
               !atomic_load_explicit(&probe->runtime->stop_requested, memory_order_acquire) &&
               atomic_load_explicit(&probe->errors, memory_order_acquire) == 0U) llam_yield();
        if (atomic_load(&probe->locked) == 2U && !atomic_load(&probe->runtime->stop_requested)) {
            owns_second = llam_mutex_lock(probe->mutexes[1U - index]) == 0;
            if (!owns_second && errno != ECANCELED) atomic_fetch_add(&probe->errors, 1U);
        }
    }
    if (owns_second && llam_mutex_unlock(probe->mutexes[1U - index]) != 0) {
        atomic_fetch_add(&probe->errors, 1U);
    }
    if (owns_first && llam_mutex_unlock(probe->mutexes[index]) != 0) {
        atomic_fetch_add(&probe->errors, 1U);
    }
    atomic_fetch_add_explicit(&probe->ran, 1U, memory_order_release);
}

static void watchdog_cycle_first(void *opaque) { watchdog_cycle_task(opaque, 0U); }
static void watchdog_cycle_second(void *opaque) { watchdog_cycle_task(opaque, 1U); }

static void watchdog_cycle_setup(void *opaque) {
    watchdog_probe_t *probe = opaque;
    llam_spawn_opts_t options;
    probe->mutexes[0] = llam_mutex_create();
    probe->mutexes[1] = llam_mutex_create();
    if (probe->mutexes[0] == NULL || probe->mutexes[1] == NULL ||
        llam_spawn_opts_init(&options, sizeof(options)) != 0) {
        atomic_fetch_add(&probe->errors, 1U);
        return;
    }
    /* Normal async lifetime debt remains, but these cycle tasks do not issue
     * irrelevant capture requests. Lifetime debt must NOT exempt a real cycle. */
    options.flags = LLAM_TASK_FLAG_NO_PREEMPT;
    probe->cycle_tasks[0] = llam_runtime_spawn_ex(probe->runtime, watchdog_cycle_first,
                                                probe, &options, sizeof(options));
    probe->cycle_tasks[1] = llam_runtime_spawn_ex(probe->runtime, watchdog_cycle_second,
                                                probe, &options, sizeof(options));
    if (probe->cycle_tasks[0] == NULL || probe->cycle_tasks[1] == NULL) {
        atomic_fetch_add(&probe->errors, 1U);
    }
}

static void watchdog_task(void *opaque) {
    watchdog_probe_t *probe = opaque;
    if (probe->leaf) {
#if (LLAM_ASYNC_CONTEXT_SHARED && LLAM_ARCH_AARCH64)
        llam_async_darwin_snapshot_t snapshot;
        uintptr_t low = (uintptr_t)g_llam_tls_task->stack_base;
        uintptr_t high = low + g_llam_tls_task->stack_size;
        uint64_t expected = WATCHDOG_LEAF_ITERATIONS * (WATCHDOG_LEAF_ITERATIONS + 1U) / 2U;
        probe->result = llam_async_darwin_leaf(WATCHDOG_LEAF_ITERATIONS, &snapshot);
        if (probe->result != expected ||
            !llam_async_darwin_snapshot_valid(&snapshot, expected, low, high)) {
            atomic_fetch_add_explicit(&probe->errors, 1U, memory_order_relaxed);
        }
#else
        atomic_fetch_add_explicit(&probe->errors, 1U, memory_order_relaxed);
#endif
    } else {
        probe->result = UINT64_C(0x6c6c616d);
    }
    atomic_fetch_add_explicit(&probe->ran, 1U, memory_order_release);
}

static void *watchdog_run(void *opaque) {
    watchdog_probe_t *probe = opaque;
    probe->run_result = llam_runtime_run_handle(probe->runtime);
    probe->run_error = probe->run_result != 0 ? errno : 0;
    return NULL;
}

static bool watchdog_consume_task(watchdog_probe_t *probe, llam_task_t **handle) {
    if (*handle == NULL) return false;
    if (probe->cycle) {
        /* Existing unmanaged join reports the runtime's fatal error even for
         * a completed G; failure releases its claim without consuming handle.
         * Detach then consumes that same handle through its normal cleanup. */
        if (llam_join(*handle) != -1 || errno != EDEADLK || llam_detach(*handle) != 0) return false;
    } else if (llam_join(*handle) != 0) {
        return false;
    }
    *handle = NULL;
    return true;
}

static void watchdog_print(const llam_watchdog_test_record_t *record) {
    printf("watchdog decision=%u streak=%u live=%u queues=%u active=%u listed=%u "
           "states[new/runq/running/parked/opaque/dead]=%u/%u/%u/%u/%u/%u "
           "selected=%" PRIu64 " state=%u listed=%u seen=%u progress=%" PRIu64
           " capture=%u restore=%u pending=%u ready=%u mailbox=%u debt=%" PRIu64
           " mutex_waiters=%u join_waiters=%u resolvers_busy=%u truncated=%u\n",
           record->decision, record->streak, record->live, record->queues, record->active,
           record->listed, record->states[0], record->states[1], record->states[2],
           record->states[3], record->states[4], record->states[5], record->selected_id,
           record->selected_state,
           record->selected_listed, record->selected_seen, record->progress,
           record->capture_ops, record->restore_ops, record->pending_phase, record->ready,
           record->mailbox, record->lifetime_debt, record->parked_mutex,
           record->parked_join, record->resolver_busy, record->truncated);
}

typedef struct wait_reference_probe {
    llam_runtime_t *runtime;
    llam_cancel_token_t *token;
    llam_mutex_t *mutex;
    llam_task_t *root;
    llam_task_t *waiter;
    atomic_uint release_owner;
    atomic_uint cancel_go;
    atomic_uint cancel_done;
    atomic_uint errors;
    int cancel_rc;
    int run_rc;
} wait_reference_probe_t;

static void wait_reference_waiter(void *opaque) {
    wait_reference_probe_t *probe = opaque;
    int rc = llam_mutex_lock(probe->mutex);
    if (rc != -1 || errno != ECANCELED) {
        atomic_fetch_add(&probe->errors, 1U);
        if (rc == 0) (void)llam_mutex_unlock(probe->mutex);
    }
}

static void wait_reference_owner(void *opaque) {
    wait_reference_probe_t *probe = opaque;
    llam_spawn_opts_t options;
    probe->mutex = llam_mutex_create();
    probe->token = llam_cancel_token_create();
    if (probe->mutex == NULL || probe->token == NULL || llam_mutex_lock(probe->mutex) != 0) {
        atomic_fetch_add(&probe->errors, 1U);
        return;
    }
    if (llam_spawn_opts_init(&options, sizeof(options)) == 0) {
        options.cancel_token = probe->token;
        probe->waiter = llam_runtime_spawn_ex(probe->runtime, wait_reference_waiter,
                                               probe, &options, sizeof(options));
    }
    if (probe->waiter == NULL) atomic_fetch_add(&probe->errors, 1U);
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    while (!atomic_load_explicit(&probe->release_owner, memory_order_acquire)) {
        if (llam_now_ns() >= deadline) {
            atomic_fetch_add(&probe->errors, 1U);
            break;
        }
        llam_yield();
    }
    if (llam_mutex_unlock(probe->mutex) != 0) atomic_fetch_add(&probe->errors, 1U);
}

static void *wait_reference_run(void *opaque) {
    wait_reference_probe_t *probe = opaque;
    probe->run_rc = llam_runtime_run_handle(probe->runtime);
    return NULL;
}

static void *wait_reference_cancel(void *opaque) {
    wait_reference_probe_t *probe = opaque;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    unsigned go;
    while ((go = atomic_load_explicit(&probe->cancel_go, memory_order_acquire)) == 0U &&
           llam_now_ns() < deadline) llam_watchdog_pause_briefly();
    probe->cancel_rc = go == 1U ? llam_cancel_token_cancel(probe->token) : -1;
    atomic_store_explicit(&probe->cancel_done, 1U, memory_order_release);
    return NULL;
}

static int watchdog_test_wait_reference(void) {
    wait_reference_probe_t probe = {0};
    llam_runtime_opts_t options;
    pthread_t host, canceller;
    bool host_started = false, cancel_started = false;
    bool observed = false;
    int result = 1;
    const llam_wait_dependency_t empty = {0};

    if (llam_task_wait_dependency_matches(NULL) ||
        llam_task_wait_dependency_matches(&empty)) return 1;

    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_create(&options, sizeof(options), &probe.runtime) != 0) return 1;
    probe.root = llam_runtime_spawn_ex(probe.runtime, wait_reference_owner, &probe, NULL, 0U);
    if (probe.root == NULL) goto cleanup;
    if (pthread_create(&canceller, NULL, wait_reference_cancel, &probe) != 0) goto cleanup;
    cancel_started = true;
    if (pthread_create(&host, NULL, wait_reference_run, &probe) != 0) goto cleanup;
    host_started = true;
    uint64_t deadline = llam_now_ns() + UINT64_C(2000000000);
    llam_shard_t *shard = &probe.runtime->shards[0];
    while (llam_now_ns() < deadline && !observed) {
        llam_wait_dependency_t snapshot = {0};
        pthread_mutex_lock(&shard->opaque_lock);
        pthread_mutex_lock(&shard->lock);
        for (llam_task_t *task = shard->all_tasks; task != NULL; task = task->all_next) {
            if (task->entry == wait_reference_waiter &&
                atomic_load_explicit(&task->state, memory_order_acquire) == LLAM_TASK_STATE_PARKED &&
                llam_worker_load_active_g(&shard->primary_worker) != task &&
                llam_worker_load_active_g(&shard->opaque_helper_worker) != task &&
                llam_task_wait_dependency_read_locked(probe.runtime, task, 2U, &snapshot)) break;
        }
        bool captured = snapshot.pinned && snapshot.mutex != NULL && snapshot.owner != 0U &&
                        llam_task_wait_dependency_matches(&snapshot);
        pthread_mutex_unlock(&shard->lock);
        pthread_mutex_unlock(&shard->opaque_lock);
        if (captured) {
            /* Keep only the lifetime reference. Cancellation's metric/wake
             * publication must be able to acquire P and complete normally. */
            atomic_store_explicit(&probe.cancel_go, 1U, memory_order_release);
            while (llam_now_ns() < deadline) {
                if (!llam_task_wait_dependency_matches(&snapshot)) {
                    observed = true;
                    break;
                }
                llam_watchdog_pause_briefly();
            }
        }
        if (snapshot.pinned) llam_task_wait_resolver_end(snapshot.task);
        if (!observed) llam_watchdog_pause_briefly();
    }
cleanup:
    if (!observed) atomic_store_explicit(&probe.cancel_go, 2U, memory_order_release);
    if (cancel_started) {
        uint64_t finish = llam_now_ns() + UINT64_C(3000000000);
        while (!atomic_load_explicit(&probe.cancel_done, memory_order_acquire) &&
               llam_now_ns() < finish) llam_watchdog_pause_briefly();
        if (!atomic_load_explicit(&probe.cancel_done, memory_order_acquire)) _Exit(124);
        if (pthread_join(canceller, NULL) != 0) atomic_fetch_add(&probe.errors, 1U);
    }
    /* Keep the mutex owned until cancellation has actually finished. */
    atomic_store_explicit(&probe.release_owner, 1U, memory_order_release);
    if (host_started && pthread_join(host, NULL) != 0) atomic_fetch_add(&probe.errors, 1U);
    if (observed && probe.cancel_rc == 0 && probe.run_rc == 0 && !atomic_load(&probe.errors)) result = 0;
    if (host_started && probe.root != NULL && llam_join(probe.root) != 0) result = 1;
    if (host_started && probe.waiter != NULL && llam_join(probe.waiter) != 0) result = 1;
    if (probe.mutex != NULL && llam_mutex_destroy(probe.mutex) != 0) result = 1;
    if (probe.token != NULL && llam_cancel_token_destroy(probe.token) != 0) result = 1;
    llam_runtime_destroy(probe.runtime);
    if (result == 0) puts("watchdog wait reference: concurrent cancellation and drain passed");
    else fprintf(stderr, "watchdog wait reference: observed=%u cancel=%d run=%d errors=%u\n",
                 (unsigned)observed, probe.cancel_rc, probe.run_rc, atomic_load(&probe.errors));
    return result;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--wait-reference") == 0) {
        return watchdog_test_wait_reference();
    }
    watchdog_probe_t probe = {0};
    llam_runtime_opts_t options;
    pthread_t host;
    bool host_started = false;
    bool enabled = false;
    bool lazy = false;
    unsigned point = LLAM_WATCHDOG_TEST_DEQUEUE;
    int result = 1;

    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "--lazy") == 0) lazy = true;
    else if (argc == 2 && strcmp(argv[1], "--async-dequeue") == 0) enabled = true;
    else if (argc == 2 && strcmp(argv[1], "--async-return") == 0) {
        enabled = true;
        point = LLAM_WATCHDOG_TEST_RETURN;
        probe.leaf = true;
    } else if (argc == 2 && (strcmp(argv[1], "--parked") == 0 ||
                             strcmp(argv[1], "--async-parked") == 0)) {
        probe.cycle = true;
        enabled = strcmp(argv[1], "--async-parked") == 0;
        point = LLAM_WATCHDOG_TEST_NO_PAUSE;
    } else if (argc != 1) return 2;
#if !(LLAM_ASYNC_CONTEXT_SHARED && LLAM_ARCH_AARCH64)
    if (enabled) {
        puts("test_watchdog_liveness_internal: skipped (shared native adapter unavailable)");
        return 77;
    }
#endif
    if (llam_runtime_opts_init(&options, sizeof(options)) != 0) return 1;
    options.deterministic = 1U;
    options.profile = lazy ? LLAM_RUNTIME_PROFILE_RELEASE_FAST : LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = LLAM_PREEMPT_STRICT;
    if (llam_runtime_create(&options, sizeof(options), &probe.runtime) != 0) goto cleanup;
#if (LLAM_ASYNC_CONTEXT_SHARED && LLAM_ARCH_AARCH64)
    if (enabled) {
        const llam_async_preempt_range_t range = {
            (uintptr_t)llam_async_darwin_leaf_begin, (uintptr_t)llam_async_darwin_leaf_end
        };
        if (llam_async_shared_enable(probe.runtime, &range, 1U, true) != 0) goto cleanup;
    }
#endif
    llam_spawn_opts_t spawn_options;
    if (llam_spawn_opts_init(&spawn_options, sizeof(spawn_options)) != 0) goto cleanup;
    if (probe.cycle) spawn_options.flags = LLAM_TASK_FLAG_NO_PREEMPT;
    probe.task = llam_runtime_spawn_ex(probe.runtime,
        probe.cycle ? watchdog_cycle_setup : watchdog_task, &probe,
        &spawn_options, sizeof(spawn_options));
    if (probe.task == NULL) goto cleanup;
    probe.gate.runtime = probe.runtime;
    probe.gate.point = point;
    llam_watchdog_test_set_gate(&probe.gate);
    if (pthread_create(&host, NULL, watchdog_run, &probe) != 0) goto cleanup;
    host_started = true;
    uint64_t deadline = llam_now_ns() + UINT64_C(3000000000);
    while (atomic_load_explicit(&probe.gate.decision_ready, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        llam_watchdog_pause_briefly();
    }
    if (atomic_load_explicit(&probe.gate.decision_ready, memory_order_acquire) == 0U) {
        fputs("watchdog: no actual decision before deadline; release is cleanup, NOT a pass\n", stderr);
        goto cleanup;
    }
    watchdog_print(&probe.gate.record);
    atomic_store_explicit(&probe.gate.release, 1U, memory_order_release);
    if (pthread_join(host, NULL) != 0) goto cleanup;
    host_started = false;
    printf("watchdog real run=%d errno=%d task_ran=%u errors=%u finite_result=%" PRIu64 "\n",
           probe.run_result, probe.run_error, atomic_load(&probe.ran),
           atomic_load(&probe.errors), probe.result);
    if (!watchdog_consume_task(&probe, &probe.task)) goto cleanup;
    if (probe.cycle) {
        for (unsigned i = 0U; i < 2U; ++i) {
            if (!watchdog_consume_task(&probe, &probe.cycle_tasks[i])) goto cleanup;
        }
        if (probe.run_result != -1 || probe.run_error != EDEADLK ||
            probe.gate.record.decision != LLAM_WATCHDOG_TEST_FATAL ||
            probe.gate.record.states[LLAM_TASK_STATE_PARKED] != 2U ||
            probe.gate.record.parked_mutex != 2U || probe.gate.record.live != 2U ||
            probe.gate.record.queues != 0U || probe.gate.record.active != 0U ||
            probe.gate.record.capture_ops != 0U || probe.gate.record.restore_ops != 0U ||
            probe.gate.record.pending_phase != 0U || probe.gate.record.ready != 0U ||
            probe.gate.record.mailbox != 0U || probe.gate.record.truncated != 0U ||
            (enabled && probe.gate.record.lifetime_debt != 2U) ||
            atomic_load(&probe.ran) != 2U || atomic_load(&probe.errors) != 0U) goto cleanup;
        result = 0;
        goto cleanup;
    }
    if (probe.run_result != 0 || atomic_load(&probe.ran) != 1U || atomic_load(&probe.errors) != 0U ||
        probe.gate.record.decision == LLAM_WATCHDOG_TEST_FATAL ||
        probe.gate.record.queues != 0U || probe.gate.record.active != 0U ||
        probe.gate.record.live != 1U || probe.gate.record.truncated != 0U) goto cleanup;
    if (point == LLAM_WATCHDOG_TEST_DEQUEUE &&
        probe.gate.record.selected_state != LLAM_TASK_STATE_RUNNABLE) goto cleanup;
#if (LLAM_ASYNC_CONTEXT_SHARED && LLAM_ARCH_AARCH64)
    if (point == LLAM_WATCHDOG_TEST_RETURN &&
        (probe.gate.record.selected_state != LLAM_TASK_STATE_RUNNING ||
         probe.gate.record.capture_ops != 1U)) goto cleanup;
#endif
    result = 0;
cleanup:
    atomic_store_explicit(&probe.gate.release, 1U, memory_order_release);
    if (host_started) {
        /* Failure cleanup targets only this runtime. It is never a successful
         * rescue and also releases a broken detector's genuinely parked cycle. */
        llam_request_stop(probe.runtime);
        if (pthread_join(host, NULL) != 0) result = 1;
    }
    llam_watchdog_test_set_gate(NULL);
    for (unsigned i = 0U; i < 2U; ++i) {
        if (probe.mutexes[i] != NULL && llam_mutex_destroy(probe.mutexes[i]) != 0) result = 1;
    }
    /* Gate storage stays alive through destroy, which joins its controller.
     * No selected task pointer is dereferenced after the captured observation. */
    if (probe.runtime != NULL) llam_runtime_destroy(probe.runtime);
    return result;
}
