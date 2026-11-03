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

#include "runtime_internal.h"

#include <inttypes.h>
#include <stdio.h>

#if !LLAM_ASYNC_CONTEXT_SHARED || !LLAM_ARCH_AARCH64
int main(void) {
    puts("test_managed_async_registered: skipped (shared native adapter disabled)");
    return 77;
}
#else
#include "fixtures/async_darwin_leaf.h"
#if LLAM_ASYNC_CONTEXT_SIGNAL
#include "async_context_signal.h"
#else
#include "async_context_darwin.h"
#endif

/* This certifies the existing native transport fixture, not ordinary C. The
 * managed compiler/runtime tests separately establish admission of C bodies. */
/* Match the established native register oracle workload. The shorter 60M
 * loop can finish on a fast M before every native target is sampled; this is
 * a transport/lifetime gate, not a claim about request latency. */
#define REGISTERED_LEAF_ITERATIONS UINT64_C(300000000)
#define REGISTERED_EXTRA_WORKERS 1U
static bool registered_dynamic;

typedef struct registered_case registered_case_t;

typedef struct registered_leaf {
    registered_case_t *test;
    unsigned index;
    llam_worker_t *origin;
    llam_processor_t *initial_processor;
    uint64_t incarnation;
    uint64_t task_id;
    uint64_t result;
    uint64_t initial_p_generation;
    uint64_t final_p_generation;
    llam_async_darwin_snapshot_t snapshot;
} registered_leaf_t;

struct registered_case {
    llam_runtime_t *runtime;
    bool rendezvous;
    atomic_uint started;
    atomic_uint finished;
    atomic_uint errors;
    atomic_uint observer_ran;
    atomic_uint borrowed_p;
    unsigned leaf_count;
    registered_leaf_t *leaves;
};

static size_t registered_worker_count(const llam_runtime_t *runtime) {
    return runtime->active_shards + runtime->managed_worker_count;
}

static llam_worker_t *registered_worker_at(llam_runtime_t *runtime, size_t index) {
    return index < runtime->active_shards ? &runtime->shards[index].primary_worker :
        &runtime->managed_workers[index - runtime->active_shards];
}

static void registered_check_at(registered_case_t *test, bool valid, unsigned line) {
    if (!valid) {
        fprintf(stderr, "registered native check failed at line %u (M slot %zu)\n", line,
                g_llam_tls_worker != NULL ? g_llam_tls_worker->registry_slot : SIZE_MAX);
        atomic_fetch_add_explicit(&test->errors, 1U, memory_order_relaxed);
    }
}
#define registered_check(test, valid) registered_check_at((test), (valid), __LINE__)


static uint64_t registered_ticket(registered_case_t *test) {
    llam_worker_t *worker = g_llam_tls_worker;
    llam_processor_t *processor = g_llam_tls_processor;
    llam_processor_owner_ticket_t ticket;

    if (worker == NULL || processor == NULL ||
        !llam_processor_owner_ticket_snapshot(processor, &ticket)) {
        registered_check(test, false);
        return 0U;
    }
    registered_check(test, ticket.owner_m == worker &&
        ticket.worker_incarnation == atomic_load_explicit(
            &worker->binding_incarnation, memory_order_acquire) &&
        ticket.lease_state == LLAM_LEASE_REGISTERED_OWNED &&
        llam_processor_owner_ticket_validate(processor, &ticket));
    if (worker->associated_processor != processor) {
        atomic_fetch_add_explicit(&test->borrowed_p, 1U, memory_order_relaxed);
    }
    return ticket.p_generation;
}

static void registered_leaf_task(void *argument) {
    registered_leaf_t *leaf = argument;
    registered_case_t *test = leaf->test;
    llam_task_t *task = g_llam_tls_task;
    uintptr_t low = (uintptr_t)task->stack_base;
    uintptr_t high = low + task->stack_size;
    sigset_t mask;
    llam_async_shared_stats_t before = {0}, after = {0};
    int task_errno = 71 + (int)leaf->index;
    int expected_mask = leaf->index == 0U ? 1 : 0;

    leaf->origin = g_llam_tls_worker;
    leaf->incarnation = atomic_load_explicit(&leaf->origin->binding_incarnation,
                                            memory_order_acquire);
    leaf->task_id = task->id;
    leaf->initial_processor = g_llam_tls_processor;
    leaf->initial_p_generation = registered_ticket(test);
    registered_check(test, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
        sigismember(&mask, SIGUSR2) == 0);
#if LLAM_PLATFORM_DARWIN
    registered_check(test, sigismember(&mask, LLAM_PREEMPT_SIGNAL) == 1);
    /* A pending request signal must stay out of any Mach-saved G frame. */
    registered_check(test, pthread_kill(pthread_self(), LLAM_PREEMPT_SIGNAL) == 0);
#endif
    if (expected_mask) sigaddset(&mask, SIGUSR2);
    else sigdelset(&mask, SIGUSR2);
    registered_check(test, pthread_sigmask(SIG_SETMASK, &mask, NULL) == 0);

    atomic_fetch_add_explicit(&test->started, 1U, memory_order_release);
    if (test->rendezvous) {
        /* On the multi-P case both native Ms must be running before either
         * finite leaf begins. This is not a host rescue or manual capture. */
        while (atomic_load_explicit(&test->started, memory_order_acquire) !=
               test->leaf_count) atomic_signal_fence(memory_order_seq_cst);
    }
    registered_check(test, llam_async_shared_worker_stats(leaf->origin, &before));
    registered_check(test, llam_worker_has_safe_managed_g(leaf->origin, g_llam_tls_processor, task));
    uint64_t leaf_started = llam_now_ns();
    errno = task_errno;
    leaf->result = llam_async_darwin_leaf(REGISTERED_LEAF_ITERATIONS, &leaf->snapshot);
    int leaf_errno = errno;
    if (leaf_errno != task_errno)
        fprintf(stderr, "registered native errno: M=%zu G=%llu expected=%d actual=%d saved=%d\n",
            leaf->origin->registry_slot, (unsigned long long)task->id, task_errno, leaf_errno, task->saved_errno);
    registered_check(test, leaf_errno == task_errno);
    registered_check(test, leaf->origin == g_llam_tls_worker &&
        leaf->incarnation == atomic_load_explicit(
            &g_llam_tls_worker->binding_incarnation, memory_order_acquire));
    /* With no cooperative boundary in the leaf, another G cannot have run on
     * this M unless this exact G first reached the native capture landing. */
    registered_check(test, llam_async_shared_worker_stats(leaf->origin, &after) &&
                           after.captures > before.captures);
    if (after.captures == before.captures) {
        fprintf(stderr, "missed native capture: M=%zu requests=%" PRIu64 " elapsed_ns=%" PRIu64 "\n",
                leaf->origin->registry_slot, after.requests - before.requests, llam_now_ns() - leaf_started);
    }
    registered_check(test, pthread_sigmask(SIG_SETMASK, NULL, &mask) == 0 &&
        sigismember(&mask, SIGUSR2) == expected_mask);
    registered_check(test, leaf->result == REGISTERED_LEAF_ITERATIONS *
        (REGISTERED_LEAF_ITERATIONS + 1U) / 2U &&
        llam_async_darwin_snapshot_valid(&leaf->snapshot, leaf->result, low, high));
    leaf->final_p_generation = registered_ticket(test);
    registered_check(test, g_llam_tls_processor != leaf->initial_processor ||
                           leaf->final_p_generation > leaf->initial_p_generation);
    atomic_fetch_add_explicit(&test->finished, 1U, memory_order_release);
}

static void registered_observer_task(void *argument) {
    registered_case_t *test = argument;
    registered_check(test, registered_ticket(test) != 0U);
    registered_check(test, llam_sleep_ns(UINT64_C(1000000)) == 0);
    registered_check(test, atomic_load_explicit(&test->finished, memory_order_acquire) <
                           test->leaf_count);
    if (test->rendezvous) {
        /* A valid run may restore both leaves on their home Ps. Exercise
         * timer-driven reacquisition until the round-robin claimant actually
         * borrows a P, instead of assuming the first timing sample did so. */
        uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
        while (atomic_load_explicit(&test->borrowed_p, memory_order_acquire) == 0U &&
               llam_now_ns() < deadline) {
            if (llam_sleep_ns(UINT64_C(100000)) != 0) {
                registered_check(test, false);
                break;
            }
            (void)registered_ticket(test);
        }
        registered_check(test, atomic_load_explicit(&test->borrowed_p, memory_order_acquire) != 0U);
    }
    atomic_store_explicit(&test->observer_ran, 1U, memory_order_release);
}

static llam_runtime_t *registered_runtime(bool single_p, bool automatic) {
    llam_runtime_opts_t options;
    llam_runtime_t *runtime = calloc(1U, sizeof(*runtime));
    const llam_async_preempt_range_t range = {
        (uintptr_t)llam_async_darwin_leaf_begin,
        (uintptr_t)llam_async_darwin_leaf_end
    };

    if (runtime == NULL || llam_runtime_opts_init(&options, sizeof(options)) != 0) {
        free(runtime);
        return NULL;
    }
    options.deterministic = single_p ? 1U : 0U;
    options.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    options.preempt_mode = automatic ? LLAM_PREEMPT_AUTO : LLAM_PREEMPT_STRICT;
    options.preempt_quantum_ns = UINT64_C(1000000);
    if (!single_p && !registered_dynamic) {
        unsigned *cpus = NULL;
        unsigned available = llam_count_allowed_cpus(&cpus);
        free(cpus);
        if (available == 0U) { free(runtime); return NULL; }
        /* A fixed two-P topology exercises actual concurrent native workers
         * without making the transport oracle depend on the host CPU count. */
        if (available >= 2U) options.worker_count = 2U;
    }
    if (registered_dynamic) options.experimental_flags |= LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    if (llam_runtime_init_registered_rt(runtime, &options, sizeof(options), true,
                                        REGISTERED_EXTRA_WORKERS) != 0) {
        free(runtime);
        return NULL;
    }
    if (llam_async_shared_enable(runtime, &range, 1U, true) != 0) {
        llam_runtime_destroy(runtime);
        return NULL;
    }
    return runtime;
}

static bool registered_drained(llam_runtime_t *runtime, uint64_t runs) {
    llam_async_shared_stats_t stats = {0};
    bool valid = llam_async_shared_stats(runtime, &stats) &&
        stats.captures != 0U && stats.captures == stats.restores &&
        stats.restores == stats.witnesses && stats.execution_debt == 0U &&
        stats.binding_ops == 0U && stats.retire_depth == 0U &&
        stats.finalizer_depth == 0U && stats.charged_bytes == 0U &&
        stats.bindings_created == registered_worker_count(runtime) * runs &&
        stats.bindings_closed == stats.bindings_created;

    if (!valid) {
        fprintf(stderr, "registered native drain: captures=%" PRIu64
            " restores=%" PRIu64 " witnesses=%" PRIu64 " debt=%" PRIu64
            " ops=%" PRIu64 " retire=%" PRIu64 " final=%" PRIu64
            " bytes=%" PRIu64 " bindings=%" PRIu64 "/%" PRIu64 "\n",
            stats.captures, stats.restores, stats.witnesses, stats.execution_debt,
            stats.binding_ops, stats.retire_depth, stats.finalizer_depth,
            stats.charged_bytes, stats.bindings_created, stats.bindings_closed);
    }
    for (size_t i = 0U; i < registered_worker_count(runtime); ++i) {
        llam_worker_t *worker = registered_worker_at(runtime, i);
        valid = valid && !atomic_load_explicit(&worker->native_thread_live,
                                               memory_order_acquire) &&
            atomic_load_explicit(&worker->current_p, memory_order_acquire) == NULL &&
            llam_worker_load_active_g(worker) == NULL &&
            atomic_load_explicit(&worker->async_preempt_state, memory_order_acquire) == 0U;
    }
    return valid;
}

static int registered_run(bool single_p, bool automatic) {
    llam_runtime_t *runtime = registered_runtime(single_p, automatic);
    registered_case_t test = {.runtime = runtime, .rendezvous = !single_p && !registered_dynamic};
    llam_task_t **tasks = NULL;
    int result = 1;
#if LLAM_PLATFORM_DARWIN
    stack_t original_stack;
    if (sigaltstack(NULL, &original_stack) != 0) return 1;
#endif

    if (runtime == NULL) return 1;
    if (!single_p && runtime->active_shards < 2U) {
        puts("registered multi-P case: skipped (host exposes fewer than 2 CPUs)");
        llam_runtime_destroy(runtime);
        return 77;
    }
    test.leaf_count = single_p ? 2U : runtime->active_shards;
    test.leaves = calloc(test.leaf_count, sizeof(*test.leaves));
    tasks = calloc((size_t)test.leaf_count + 1U, sizeof(*tasks));
    if (test.leaves == NULL || tasks == NULL) goto done;
    for (unsigned run = 0U; run < 2U; ++run) {
        atomic_store(&test.started, 0U);
        atomic_store(&test.finished, 0U);
        atomic_store(&test.errors, 0U);
        atomic_store(&test.observer_ran, 0U);
        atomic_store(&test.borrowed_p, 0U);
        for (unsigned i = 0U; i < test.leaf_count; ++i) {
            test.leaves[i].test = &test;
            test.leaves[i].index = i;
            tasks[i] = llam_runtime_spawn_ex(runtime, registered_leaf_task,
                                              &test.leaves[i], NULL, 0U);
            if (tasks[i] == NULL) goto done;
        }
        tasks[test.leaf_count] = llam_runtime_spawn_ex(runtime,
            registered_observer_task, &test, NULL, 0U);
        if (tasks[test.leaf_count] == NULL || llam_runtime_run_handle(runtime) != 0) {
            fprintf(stderr, "registered native run failed: errno=%d\n", errno);
            goto done;
        }
#if LLAM_PLATFORM_DARWIN
        stack_t restored_stack;
        if (sigaltstack(NULL, &restored_stack) != 0 ||
            (restored_stack.ss_flags & SS_ONSTACK) != 0 ||
            ((restored_stack.ss_flags ^ original_stack.ss_flags) & SS_DISABLE) != 0 ||
            ((original_stack.ss_flags & SS_DISABLE) == 0 &&
             (restored_stack.ss_sp != original_stack.ss_sp || restored_stack.ss_size != original_stack.ss_size))) {
            fprintf(stderr, "native stack restore: before flags=%d after=%d sp=%p/%p size=%zu/%zu\n",
                original_stack.ss_flags, restored_stack.ss_flags, original_stack.ss_sp, restored_stack.ss_sp,
                original_stack.ss_size, restored_stack.ss_size);
            goto done;
        }
#endif
        for (unsigned i = 0U; i < runtime->active_shards; ++i)
            if (atomic_load_explicit(&runtime->shards[i].metrics.preempt_signals, memory_order_acquire) != 0U) {
                fprintf(stderr, "native unexpected cooperative signals: P=%u count=%llu\n", i,
                    (unsigned long long)atomic_load_explicit(&runtime->shards[i].metrics.preempt_signals, memory_order_acquire));
                goto done;
            }
        for (unsigned i = 0U; i < test.leaf_count + 1U; ++i) {
            if (llam_join(tasks[i]) != 0) {
                fprintf(stderr, "registered native join: run=%u task=%u errno=%d\n", run, i, errno);
                goto done;
            }
            tasks[i] = NULL;
        }
        if (atomic_load(&test.started) != test.leaf_count ||
            atomic_load(&test.finished) != test.leaf_count ||
            atomic_load(&test.observer_ran) != 1U || atomic_load(&test.errors) != 0U ||
            !registered_drained(runtime, run + 1U)) {
            fprintf(stderr, "registered native state: started=%u finished=%u observer=%u errors=%u\n",
                atomic_load(&test.started), atomic_load(&test.finished),
                atomic_load(&test.observer_ran), atomic_load(&test.errors));
            goto done;
        }
        if (!single_p && !registered_dynamic && atomic_load(&test.borrowed_p) == 0U) {
            fprintf(stderr, "registered native: no foreign P observed (run=%u, leaves=%u)\n", run, test.leaf_count);
            goto done;
        }
        for (unsigned i = 0U; i < test.leaf_count; ++i) {
            llam_async_shared_stats_t worker_stats = {0};
            if (!llam_async_shared_worker_stats(test.leaves[i].origin, &worker_stats) ||
                worker_stats.captures == 0U) {
                fprintf(stderr, "registered native: missing worker captures (run=%u, leaf=%u)\n", run, i);
                goto done;
            }
            if (!single_p && !registered_dynamic) {
                for (unsigned j = 0U; j < i; ++j) {
                    if (test.leaves[i].origin == test.leaves[j].origin) {
                        fprintf(stderr, "registered native: shared origin (run=%u, leaves=%u/%u)\n", run, i, j);
                        goto done;
                    }
                }
            }
        }
        for (size_t i = 0U; i < registered_worker_count(runtime); ++i) {
            llam_worker_t *worker = registered_worker_at(runtime, i);
            uint64_t incarnation = atomic_load_explicit(&worker->incarnation,
                                                        memory_order_acquire);
            if (incarnation != run + 1U) {
                fprintf(stderr, "registered native: incarnation=%" PRIu64 " expected=%u M=%zu\n",
                    incarnation, run + 1U, i);
                goto done;
            }
        }
    }
    printf("registered native: %zu M / %u P, two runs, original-M exact restore, "
           "timer progress and balanced native lifetime\n",
           registered_worker_count(runtime), runtime->active_shards);
    result = 0;
done:
    llam_runtime_destroy(runtime);
    free(tasks);
    free(test.leaves);
    return result;
}

static int registered_startup_abort(void) {
    llam_runtime_t *runtime = registered_runtime(true, false);
    registered_case_t test = {.runtime = runtime};
    registered_leaf_t leaf = {.test = &test};
    llam_async_shared_stats_t stats = {0};
    int result = 1;

    if (runtime == NULL) return 1;
    if (llam_runtime_spawn_ex(runtime, registered_leaf_task, &leaf, NULL, 0U) == NULL) {
        goto done;
    }
#if LLAM_ASYNC_CONTEXT_SIGNAL
    llam_async_signal_test_fail_probe();
#else
    llam_darwin_async_test_fail(LLAM_DARWIN_ASYNC_TEST_FAIL_READ);
#endif
    if (llam_runtime_run_handle(runtime) != -1 || errno != ENOTSUP || atomic_load(&test.started) != 0U ||
        !llam_async_shared_stats(runtime, &stats) || stats.captures != 0U ||
        stats.restores != 0U || stats.execution_debt != 0U || stats.binding_ops != 0U ||
        stats.retire_depth != 0U || stats.finalizer_depth != 0U ||
        stats.bindings_created != stats.bindings_closed) goto done;
    for (size_t i = 0U; i < registered_worker_count(runtime); ++i) {
        if (atomic_load_explicit(&registered_worker_at(runtime, i)->native_thread_live,
                                 memory_order_acquire)) goto done;
    }
    result = 0;
done:
    /* NEW tasks have retained storage but never acquired execution debt. */
    llam_runtime_destroy(runtime);
    if (result == 0) puts("registered native: all-M startup abort admitted no user G");
    return result;
}

int main(int argc, char **argv) {
    sigset_t original, mask;
    int result;

    if (pthread_sigmask(SIG_SETMASK, NULL, &original) != 0) return 1;
    mask = original;
    sigdelset(&mask, SIGUSR2);
    if (pthread_sigmask(SIG_SETMASK, &mask, NULL) != 0) return 1;
    if (argc == 2 && strcmp(argv[1], "--startup-abort") == 0) {
        result = registered_startup_abort();
    } else if (argc == 2 && strcmp(argv[1], "--dynamic") == 0) {
        registered_dynamic = true;
        result = registered_run(false, false);
    } else if (argc == 2 && strcmp(argv[1], "--multi-p") == 0) {
        result = registered_run(false, false);
    } else if (argc == 2 && strcmp(argv[1], "--auto") == 0) {
        result = registered_run(true, true);
    } else if (argc == 1) {
        result = registered_run(true, false);
    } else {
        result = 2;
    }
    if (pthread_sigmask(SIG_SETMASK, &original, NULL) != 0) result = 1;
    return result;
}
#endif
