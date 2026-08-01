/**
 * @file tests/test_runtime_core.c
 * @brief Core runtime lifecycle, spawn, introspection, blocking, and stats tests.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "llam/runtime.h"
#include "runtime_resource_plan.h"
#include "runtime_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#if LLAM_PLATFORM_POSIX
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

void llam_watchdog_autotune_tick(llam_runtime_t *rt, uint64_t now_ns);

#ifndef O_RDONLY
#define O_RDONLY 0
#endif

typedef struct core_state {
    atomic_uint failures;
    atomic_uint ran;
    atomic_uint blocking_calls;
    unsigned expected_flags;
    llam_task_class_t expected_class;
    int first_errno;
    char first_case[128];
} core_state_t;

typedef struct errno_task_args {
    core_state_t *state;
    int yield_errno;
    int sleep_errno;
} errno_task_args_t;

typedef struct double_join_state {
    core_state_t *state;
    llam_task_t *target;
    atomic_uint target_done;
    atomic_uint joined;
    atomic_uint busy;
} double_join_state_t;

typedef struct owner_diag_state {
    core_state_t core;
    llam_runtime_t fake_runtime;
    llam_channel_t *channel;
    llam_mutex_t *mutex;
    llam_cond_t *cond;
    llam_cancel_token_t *token;
    llam_task_group_t *group;
    llam_task_t *target;
    int payload;
} owner_diag_state_t;

typedef struct nested_runtime_create_state {
    core_state_t core;
    llam_runtime_t *created_runtime;
} nested_runtime_create_state_t;

#define BLOCK_POOL_GROWTH_TASKS 3U

typedef struct block_pool_growth_state {
    core_state_t core;
    llam_runtime_t *runtime;
    llam_task_t *tasks[BLOCK_POOL_GROWTH_TASKS];
    atomic_uint callbacks_started;
    atomic_uint callbacks_active;
    atomic_uint callbacks_active_peak;
    atomic_uint callbacks_completed;
    atomic_uint release_callbacks;
    unsigned confirmed_before_release;
    unsigned entered_before_release;
    unsigned live_before_release;
} block_pool_growth_state_t;

typedef struct native_thread_stats_state {
    core_state_t core;
    llam_runtime_t *runtime;
    llam_runtime_stats_t running_stats;
    unsigned expected_scheduler_threads;
    unsigned expected_io_threads;
} native_thread_stats_state_t;

typedef struct signal_wait_state {
    core_state_t core;
    llam_signal_set_t *set;
} signal_wait_state_t;

#define AUTOTUNE_HANDOFF_WORKERS 2U

typedef struct autotune_handoff_state {
    core_state_t core;
    atomic_uint started;
    atomic_uint stop;
    atomic_uint_fast64_t deadline_ns;
    llam_task_t *workers[AUTOTUNE_HANDOFF_WORKERS];
} autotune_handoff_state_t;

#if defined(__APPLE__)
typedef struct timer_handoff_state {
    core_state_t core;
    atomic_uint timer_armed;
    unsigned yields;
    uint64_t sleep_ns;
} timer_handoff_state_t;
#endif

#if LLAM_PLATFORM_POSIX
typedef struct dump_blocking_state {
    core_state_t core;
    atomic_uint blocking_started;
    atomic_uint release_blocking;
    atomic_uint task_done;
} dump_blocking_state_t;
#endif

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
typedef struct aarch64_simd_state {
    core_state_t *state;
    uint64_t expected_d8_bits;
} aarch64_simd_state_t;
#endif

typedef int (*runtime_core_test_fn)(void);

static int test_fail(const char *message) {
    fprintf(stderr, "[test_runtime_core] %s\n", message);
    return 1;
}

static int test_fail_errno(const char *message) {
    fprintf(stderr, "[test_runtime_core] %s: errno=%d (%s)\n", message, errno, strerror(errno));
    return 1;
}

static int run_named_test(const char *name, runtime_core_test_fn fn) {
    fprintf(stderr, "[test_runtime_core] begin %s\n", name);
    if (fn() != 0) {
        fprintf(stderr, "[test_runtime_core] fail %s\n", name);
        return 1;
    }
    fprintf(stderr, "[test_runtime_core] ok %s\n", name);
    return 0;
}

#define RUN_RUNTIME_CORE_TEST(fn) \
    do { \
        if (run_named_test(#fn, fn) != 0) { \
            return 1; \
        } \
    } while (0)

static void task_fail(core_state_t *state, const char *where, int err) {
    if (atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed) == 0U) {
        state->first_errno = err;
        (void)snprintf(state->first_case, sizeof(state->first_case), "%s", where);
    }
}

static void expect_runtime_owner_mismatch(core_state_t *state, const char *where, int rc) {
    if (rc != -1 || errno != EXDEV) {
        task_fail(state, where, errno);
    }
}

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
__attribute__((always_inline)) static inline void aarch64_set_d8_bits(uint64_t value) {
    __asm__ volatile("fmov d8, %0" : : "r"(value) : "v8");
}

__attribute__((always_inline)) static inline uint64_t aarch64_get_d8_bits(void) {
    uint64_t value;

    __asm__ volatile("fmov %0, d8" : "=r"(value));
    return value;
}
#endif

static void *blocking_callback(void *arg) {
    core_state_t *state = arg;

    atomic_fetch_add_explicit(&state->blocking_calls, 1U, memory_order_relaxed);
    return arg;
}

static void *blocking_null_callback(void *arg) {
    (void)arg;
    return NULL;
}

static void test_atomic_update_peak(atomic_uint *peak, unsigned value) {
    unsigned observed = atomic_load_explicit(peak, memory_order_relaxed);

    while (observed < value &&
           !atomic_compare_exchange_weak_explicit(peak,
                                                  &observed,
                                                  value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static void *block_pool_growth_callback(void *arg) {
    block_pool_growth_state_t *state = arg;
    struct timespec interval = {0, 1000000L};
    unsigned active;

    active = atomic_fetch_add_explicit(&state->callbacks_active,
                                       1U,
                                       memory_order_acq_rel) +
             1U;
    test_atomic_update_peak(&state->callbacks_active_peak, active);
    atomic_fetch_add_explicit(&state->callbacks_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_callbacks, memory_order_acquire) == 0U) {
        (void)nanosleep(&interval, NULL);
    }
    atomic_fetch_sub_explicit(&state->callbacks_active, 1U, memory_order_acq_rel);
    atomic_fetch_add_explicit(&state->callbacks_completed, 1U, memory_order_release);
    return state;
}

static void block_pool_growth_child(void *arg) {
    block_pool_growth_state_t *state = arg;
    void *result = NULL;

    if (llam_call_blocking_result(block_pool_growth_callback, state, &result) != 0 ||
        result != state) {
        task_fail(&state->core, "lazy blocking worker callback failed", errno);
    }
}

static void block_pool_growth_parent(void *arg) {
    block_pool_growth_state_t *state = arg;
    uint64_t deadline_ns = llam_now_ns() + UINT64_C(5000000000);
    unsigned i;

    for (i = 0U; i < BLOCK_POOL_GROWTH_TASKS; ++i) {
        state->tasks[i] = llam_runtime_spawn_ex(
            state->runtime, block_pool_growth_child, state, NULL, 0U);
        if (state->tasks[i] == NULL) {
            task_fail(&state->core, "lazy blocking worker child spawn failed", errno);
            break;
        }
    }

    while (i == BLOCK_POOL_GROWTH_TASKS &&
           (atomic_load_explicit(&state->runtime->block_pending, memory_order_acquire) <
                BLOCK_POOL_GROWTH_TASKS ||
            atomic_load_explicit(&state->callbacks_started, memory_order_acquire) < 2U)) {
        if (llam_now_ns() >= deadline_ns) {
            task_fail(&state->core, "lazy blocking pool did not reach two workers", ETIMEDOUT);
            break;
        }
        llam_yield();
    }

    state->confirmed_before_release =
        atomic_load_explicit(&state->runtime->block_threads_started, memory_order_acquire);
    state->entered_before_release =
        atomic_load_explicit(&state->runtime->block_threads_entered, memory_order_acquire);
    state->live_before_release =
        atomic_load_explicit(&state->runtime->block_threads_live, memory_order_acquire);
    atomic_store_explicit(&state->release_callbacks, 1U, memory_order_release);

    for (i = 0U; i < BLOCK_POOL_GROWTH_TASKS; ++i) {
        if (state->tasks[i] != NULL && llam_join(state->tasks[i]) != 0) {
            task_fail(&state->core, "lazy blocking worker child join failed", errno);
        }
        state->tasks[i] = NULL;
    }
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
}

static void native_thread_stats_task(void *arg) {
    native_thread_stats_state_t *state = arg;
    llam_runtime_t *rt = state->runtime;
    llam_shard_t *shard = g_llam_tls_shard;
    uint64_t deadline_ns = llam_now_ns() + UINT64_C(5000000000);
    unsigned runtime_owned;

    if (shard == NULL || shard->runtime != rt) {
        task_fail(&state->core, "native thread stats task lost its runtime", EINVAL);
        return;
    }

    pthread_mutex_lock(&shard->opaque_lock);
    if (llam_ensure_opaque_helper_locked(shard) != 0) {
        int saved_errno = errno;

        pthread_mutex_unlock(&shard->opaque_lock);
        task_fail(&state->core, "native thread stats opaque helper failed", saved_errno);
        return;
    }
    pthread_mutex_unlock(&shard->opaque_lock);

    while (atomic_load_explicit(&rt->scheduler_threads_live, memory_order_acquire) !=
               state->expected_scheduler_threads ||
           atomic_load_explicit(&rt->block_threads_live, memory_order_acquire) != 1U ||
           atomic_load_explicit(&rt->io_threads_live, memory_order_acquire) !=
               state->expected_io_threads ||
           atomic_load_explicit(&rt->controller_threads_live, memory_order_acquire) != 1U ||
           atomic_load_explicit(&rt->opaque_helper_threads_live, memory_order_acquire) != 1U ||
           atomic_load_explicit(&rt->host_threads_live, memory_order_acquire) != 1U) {
        if (llam_now_ns() >= deadline_ns) {
            task_fail(&state->core, "native thread counters did not converge", ETIMEDOUT);
            return;
        }
        llam_yield();
    }

    if (llam_runtime_collect_stats_ex(
            &state->running_stats, sizeof(state->running_stats)) != 0) {
        task_fail(&state->core, "native thread running stats failed", errno);
        return;
    }
    runtime_owned = state->expected_scheduler_threads + 1U +
                    state->expected_io_threads + 1U + 1U;
    if (state->running_stats.scheduler_threads != state->expected_scheduler_threads ||
        state->running_stats.blocking_threads != 1U ||
        state->running_stats.io_threads != state->expected_io_threads ||
        state->running_stats.controller_threads != 1U ||
        state->running_stats.opaque_helper_threads != 1U ||
        state->running_stats.runtime_owned_threads != runtime_owned ||
        state->running_stats.native_execution_threads != runtime_owned + 1U ||
        state->running_stats.configured_worker_max != rt->resource_plan.worker_max ||
        state->running_stats.configured_blocking_max != 2U ||
        state->running_stats.affinity_failures != 0U) {
        task_fail(&state->core, "native thread running stats were not truthful", EINVAL);
        return;
    }
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
}

#if LLAM_PLATFORM_POSIX
static void *dump_blocking_callback(void *arg) {
    dump_blocking_state_t *state = arg;

    atomic_store_explicit(&state->blocking_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release_blocking, memory_order_acquire) == 0U) {
        usleep(1000);
    }
    return arg;
}

static void dump_blocking_task(void *arg) {
    dump_blocking_state_t *state = arg;
    void *result = NULL;

    if (llam_call_blocking_result(dump_blocking_callback, state, &result) != 0 ||
        result != state) {
        task_fail(&state->core, "blocking job for concurrent runtime dump failed", errno);
        return;
    }
    atomic_store_explicit(&state->task_done, 1U, memory_order_release);
}

static void *dump_run_thread(void *arg) {
    dump_blocking_state_t *state = arg;

    if (llam_run() != 0) {
        task_fail(&state->core, "llam_run concurrent dump thread failed", errno);
    }
    return NULL;
}
#endif

static void inspect_task(void *arg) {
    core_state_t *state = arg;
    llam_task_t *self = llam_current_task();
    const char *state_name;
    void *blocking_result;

    if (self == NULL) {
        task_fail(state, "llam_current_task returned NULL", EINVAL);
        return;
    }
    if (llam_task_id(self) == 0U) {
        task_fail(state, "llam_task_id returned 0", EINVAL);
        return;
    }
    state_name = llam_task_state_name(self);
    if (state_name == NULL || strcmp(state_name, "UNKNOWN") == 0) {
        task_fail(state, "llam_task_state_name returned invalid name", EINVAL);
        return;
    }
    if (llam_task_class(self) != state->expected_class) {
        task_fail(state, "initial task class mismatch", EINVAL);
        return;
    }
    if ((llam_task_flags(self) & state->expected_flags) != state->expected_flags) {
        task_fail(state, "task flags mismatch", EINVAL);
        return;
    }

    if (llam_task_set_class(LLAM_TASK_CLASS_LATENCY) != 0) {
        task_fail(state, "llam_task_set_class failed", errno);
        return;
    }
    if (llam_task_class(self) != LLAM_TASK_CLASS_LATENCY) {
        task_fail(state, "llam_task_set_class did not update current task", EINVAL);
        return;
    }

    llam_task_safepoint();
    llam_yield();
    if (llam_sleep_ns(0U) != 0) {
        task_fail(state, "llam_sleep_ns(0)", errno);
        return;
    }
    if (llam_call_blocking(blocking_callback, state) != state) {
        task_fail(state, "llam_call_blocking", errno);
        return;
    }
    blocking_result = (void *)state;
    if (llam_call_blocking_result(blocking_null_callback, state, &blocking_result) != 0 ||
        blocking_result != NULL) {
        task_fail(state, "llam_call_blocking_result NULL callback", errno);
        return;
    }
    errno = 0;
    if (llam_call_blocking_result(blocking_callback, state, NULL) != -1 || errno != EINVAL) {
        task_fail(state, "llam_call_blocking_result NULL out", errno);
        return;
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void direct_yield_no_work_task(void *arg) {
    core_state_t *state = arg;
    llam_task_t *self = llam_current_task();

    if (self == NULL) {
        task_fail(state, "direct yield current task missing", EINVAL);
        return;
    }
    /*
     * This exercises the internal try-handoff API used by I/O and channel fast
     * paths.  With no local peer runnable, the call must fail without changing
     * the still-executing task from RUNNING to RUNNABLE.
     */
    if (llam_yield_to_local_runnable()) {
        task_fail(state, "direct yield unexpectedly found local work", EINVAL);
        return;
    }
    if (strcmp(llam_task_state_name(self), "RUNNING") != 0) {
        task_fail(state, "failed direct yield mutated current task state", EINVAL);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void errno_isolation_task(void *arg) {
    errno_task_args_t *args = arg;
    core_state_t *state = args->state;
    unsigned i;

    for (i = 0U; i < 16U; ++i) {
        errno = args->yield_errno;
        llam_yield();
        if (errno != args->yield_errno) {
            task_fail(state, "task-local errno was not preserved across yield", errno);
            return;
        }

        errno = args->sleep_errno;
        if (llam_sleep_ns(100000U) != 0) {
            task_fail(state, "llam_sleep_ns in errno isolation task", errno);
            return;
        }
        if (errno != args->sleep_errno) {
            task_fail(state, "task-local errno was not preserved across sleep", errno);
            return;
        }
    }

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void double_join_target_task(void *arg) {
    double_join_state_t *state = arg;

    if (llam_sleep_ns(20ULL * 1000ULL * 1000ULL) != 0) {
        task_fail(state->state, "double join target sleep", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->target_done, 1U, memory_order_relaxed);
}

static void double_join_waiter_task(void *arg) {
    double_join_state_t *state = arg;

    llam_yield();
    if (llam_join(state->target) == 0) {
        atomic_fetch_add_explicit(&state->joined, 1U, memory_order_relaxed);
        return;
    }
    if (errno == EBUSY) {
        atomic_fetch_add_explicit(&state->busy, 1U, memory_order_relaxed);
        return;
    }
    task_fail(state->state, "double join unexpected errno", errno);
}

static void request_stop_task(void *arg) {
    core_state_t *state = arg;

    if (llam_runtime_request_stop() != 0) {
        task_fail(state, "llam_runtime_request_stop in task", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void shutdown_from_task_task(void *arg) {
    core_state_t *state = arg;
    int saved_errno = E2BIG;

    /*
     * Managed tasks must never run full singleton teardown from their scheduler
     * stack.  The public shutdown entry point should degrade to request_stop
     * here, preserve errno, and let the host thread destroy resources later.
     */
    errno = saved_errno;
    llam_runtime_shutdown();
    if (errno != saved_errno) {
        task_fail(state, "managed shutdown clobbered errno", errno);
        return;
    }
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void detached_task(void *arg) {
    core_state_t *state = arg;

    llam_yield();
    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

#if LLAM_PLATFORM_POSIX
static void autotune_handoff_worker_task(void *arg) {
    autotune_handoff_state_t *state = arg;
    uint64_t deadline_ns = atomic_load_explicit(&state->deadline_ns, memory_order_acquire);

    atomic_fetch_add_explicit(&state->started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline_ns) {
        if (!llam_yield_to_local_runnable()) {
            llam_yield();
        }
    }
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
}

static void autotune_handoff_parent_task(void *arg) {
    autotune_handoff_state_t *state = arg;
    llam_spawn_opts_t spawn_opts;
    unsigned i;

    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = (uint32_t)LLAM_TASK_CLASS_DEFAULT;
    spawn_opts.stack_class = (uint32_t)LLAM_STACK_CLASS_DEFAULT;
    spawn_opts.flags = LLAM_SPAWN_F_PINNED;
    atomic_store_explicit(&state->deadline_ns,
                          llam_now_ns() + 150ULL * 1000ULL * 1000ULL,
                          memory_order_release);

    for (i = 0U; i < AUTOTUNE_HANDOFF_WORKERS; ++i) {
        state->workers[i] = llam_spawn(autotune_handoff_worker_task, state, &spawn_opts);
        if (state->workers[i] == NULL) {
            atomic_store_explicit(&state->stop, 1U, memory_order_release);
            task_fail(&state->core, "autotune handoff worker spawn failed", errno);
            return;
        }
    }
    while (atomic_load_explicit(&state->started, memory_order_acquire) < AUTOTUNE_HANDOFF_WORKERS) {
        llam_yield();
    }
    for (i = 0U; i < AUTOTUNE_HANDOFF_WORKERS; ++i) {
        if (state->workers[i] != NULL && llam_join(state->workers[i]) != 0) {
            task_fail(&state->core, "autotune handoff worker join failed", errno);
            return;
        }
        state->workers[i] = NULL;
    }
    atomic_store_explicit(&state->stop, 1U, memory_order_release);
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
}
#endif

#if defined(__APPLE__)
static void timer_handoff_sleep_task(void *arg) {
    timer_handoff_state_t *state = arg;

    /*
     * Store just before sleeping. On a deterministic single shard, this task
     * keeps running until the sleep parks, so paired yielders see a live timer.
     */
    atomic_store_explicit(&state->timer_armed, 1U, memory_order_release);
    if (llam_sleep_ns(state->sleep_ns) != 0) {
        task_fail(&state->core, "timer handoff sleeper failed", errno);
    }
}

static void timer_handoff_yield_task(void *arg) {
    timer_handoff_state_t *state = arg;
    unsigned i;

    while (atomic_load_explicit(&state->timer_armed, memory_order_acquire) == 0U) {
        llam_yield();
    }
    for (i = 0U; i < state->yields; ++i) {
        llam_yield();
    }
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
}
#endif

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
static void aarch64_simd_preservation_task(void *arg) {
    aarch64_simd_state_t *simd = arg;
    unsigned i;

    aarch64_set_d8_bits(simd->expected_d8_bits);
    for (i = 0U; i < 64U; ++i) {
        llam_yield();
        if (aarch64_get_d8_bits() != simd->expected_d8_bits) {
            task_fail(simd->state, "AArch64 d8 was not preserved across yield", EINVAL);
            return;
        }
    }

    atomic_fetch_add_explicit(&simd->state->ran, 1U, memory_order_relaxed);
}
#endif

static int test_preinit_contracts(void) {
    llam_runtime_stats_t stats;
    llam_task_local_key_t key = LLAM_TASK_LOCAL_INVALID_KEY;

    if (llam_runtime_default() == NULL) {
        return test_fail("llam_runtime_default returned NULL");
    }
    if (llam_current_task() != NULL) {
        return test_fail("llam_current_task outside runtime was not NULL");
    }
    if (llam_task_id(NULL) != 0U) {
        return test_fail("llam_task_id(NULL) did not return 0");
    }
    if (strcmp(llam_task_state_name(NULL), "UNKNOWN") != 0) {
        return test_fail("llam_task_state_name(NULL) did not return UNKNOWN");
    }
    if (llam_task_class(NULL) != LLAM_TASK_CLASS_DEFAULT) {
        return test_fail("llam_task_class(NULL) did not return default class");
    }
    if (llam_task_flags(NULL) != 0U) {
        return test_fail("llam_task_flags(NULL) did not return 0");
    }
    llam_task_safepoint();
    errno = 0;
    if (llam_task_set_class(999U) != -1 || errno != EINVAL) {
        return test_fail("llam_task_set_class invalid class did not fail with EINVAL");
    }
    errno = 0;
    if (llam_task_set_class(LLAM_TASK_CLASS_DEFAULT) != -1 || errno != ENOTSUP) {
        return test_fail("llam_task_set_class outside task did not fail with ENOTSUP");
    }
    if (llam_task_local_key_create(&key) != 0) {
        return test_fail_errno("llam_task_local_key_create outside runtime failed");
    }
    errno = 0;
    if (llam_task_local_get(key) != NULL || errno != ENOTSUP) {
        (void)llam_task_local_key_delete(key);
        return test_fail("llam_task_local_get outside task did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_local_set(key, &stats) != -1 || errno != ENOTSUP) {
        (void)llam_task_local_key_delete(key);
        return test_fail("llam_task_local_set outside task did not fail with ENOTSUP");
    }
    if (llam_task_local_key_delete(key) != 0) {
        return test_fail_errno("llam_task_local_key_delete failed");
    }
    errno = 0;
    if (llam_task_local_key_delete(key) != -1 || errno != EINVAL) {
        return test_fail("llam_task_local_key_delete inactive key did not fail with EINVAL");
    }

    errno = 0;
    if (llam_runtime_collect_stats(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_collect_stats(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex(&stats, 0U) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_collect_stats_ex zero size did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_write_stats_json(-1) != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_write_stats_json invalid fd did not fail with EINVAL");
    }
    memset(&stats, 0xA5, sizeof(stats));
    errno = ECHILD;
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0 ||
        errno != ECHILD ||
        stats.ctx_switches != 0U ||
        stats.active_workers != 0U ||
        stats.active_nodes != 0U) {
        return test_fail("pre-init default stats did not return an empty errno-preserving snapshot");
    }
#if LLAM_PLATFORM_POSIX
    {
        int pipe_fds[2];
        char json[8192];
        ssize_t nread;
        if (pipe(pipe_fds) != 0) {
            return test_fail_errno("pipe for pre-init stats json failed");
        }
        if (llam_runtime_write_stats_json(pipe_fds[1]) != 0) {
            int saved_errno = errno;
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            errno = saved_errno;
            return test_fail_errno("pre-init stats json failed");
        }
        close(pipe_fds[1]);
        nread = read(pipe_fds[0], json, sizeof(json) - 1U);
        close(pipe_fds[0]);
        if (nread <= 0) {
            return test_fail("pre-init stats json produced no data");
        }
        json[nread] = '\0';
        if (json[0] != '{' ||
            strstr(json, "\"ctx_switches\":0") == NULL ||
            strstr(json, "\"active_workers\":0") == NULL ||
            strstr(json, "\"scheduler_threads\":0") == NULL ||
            strstr(json, "\"runtime_owned_threads\":0") == NULL ||
            strstr(json, "\"native_execution_threads\":0") == NULL ||
            strstr(json, "\"affinity_failures\":0") == NULL ||
            strstr(json, "\"stack_cache_budget_bytes\":0") == NULL ||
            strstr(json, "\"stack_cache_cached_bytes\":0") == NULL ||
            strstr(json, "\"stack_cache_trim_requests\":0") == NULL ||
            strstr(json, "\"stack_cache_resident_valid\":0") == NULL ||
            strstr(json, "\"stack_cache_resident_sample_ns\":0") == NULL ||
            strstr(json, "\"stack_cache_process_quarantine_bytes\":0") == NULL ||
            strstr(json, "\"stack_cache_process_quarantine_mappings\":0") == NULL) {
            return test_fail("pre-init stats json was not an empty snapshot");
        }
    }
#endif
    memset(&stats, 0xA5, sizeof(stats));
    errno = 0;
    if (llam_runtime_init_ex((const llam_runtime_opts_t *)(const void *)&stats, 0U) != -1 ||
        errno != EINVAL) {
        return test_fail("llam_runtime_init_ex zero-sized opts did not fail with EINVAL");
    }
    {
        llam_runtime_opts_t invalid_opts;

        memset(&invalid_opts, 0, sizeof(invalid_opts));
        invalid_opts.profile = 999U;
        errno = 0;
        if (llam_runtime_init_ex(&invalid_opts, sizeof(invalid_opts)) != -1 || errno != EINVAL) {
            return test_fail("llam_runtime_init_ex invalid profile did not fail with EINVAL");
        }
        memset(&invalid_opts, 0, sizeof(invalid_opts));
        invalid_opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
        invalid_opts.preempt_mode = 999U;
        errno = 0;
        if (llam_runtime_init_ex(&invalid_opts, sizeof(invalid_opts)) != -1 || errno != EINVAL) {
            return test_fail("llam_runtime_init_ex invalid preempt mode did not fail with EINVAL");
        }
    }
    errno = 0;
    if (llam_spawn(NULL, NULL, NULL) != NULL || errno != EINVAL) {
        return test_fail("llam_spawn before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_spawn_ex(inspect_task, NULL, (const llam_spawn_opts_t *)(const void *)&stats, 0U) != NULL ||
        errno != EINVAL) {
        return test_fail("llam_spawn_ex zero-sized opts did not fail with EINVAL");
    }
    errno = 0;
    if (llam_run() != -1 || errno != EINVAL) {
        return test_fail("llam_run before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_request_stop() != -1 || errno != EINVAL) {
        return test_fail("llam_runtime_request_stop before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_detach(NULL) != -1 || errno != EINVAL) {
        return test_fail("llam_detach(NULL) before init did not fail with EINVAL");
    }
    errno = 0;
    if (llam_sleep_ns(0U) != -1 || errno != EINVAL) {
        return test_fail("llam_sleep_ns before init did not fail with EINVAL");
    }
    return 0;
}

static int test_runtime_registered_init_failure_rolls_back(void) {
    unsigned *cpus = NULL;
    unsigned cpu_count = llam_count_allowed_cpus(&cpus);
    llam_runtime_opts_t bad_opts;
    llam_runtime_opts_t good_opts;
    llam_runtime_t *runtime = (llam_runtime_t *)(uintptr_t)0x1U;

    free(cpus);
    if (cpu_count <= 1U) {
        return 0;
    }

    memset(&bad_opts, 0, sizeof(bad_opts));
    bad_opts.deterministic = 1U;
    bad_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    bad_opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL;
    bad_opts.sqpoll_cpu = INT32_MAX;

    /*
     * Invalid explicit SQPOLL CPU validation happens after the runtime is
     * registered and, on Windows, after Winsock startup.  Failure must roll the
     * partially initialized runtime back through the normal shutdown path so
     * backend resources are released and later init/create calls remain valid.
     */
    errno = 0;
    if (llam_runtime_init_ex(&bad_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 ||
        errno != EINVAL) {
        llam_runtime_shutdown();
        return test_fail("registered default-runtime init failure did not report EINVAL");
    }

    memset(&good_opts, 0, sizeof(good_opts));
    good_opts.deterministic = 1U;
    good_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init_ex(&good_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("default runtime did not reinitialize after registered init failure");
    }
    llam_runtime_shutdown();

    errno = 0;
    if (llam_runtime_create(&bad_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != -1 ||
        errno != EINVAL ||
        runtime != NULL) {
        llam_runtime_destroy(runtime);
        return test_fail("registered explicit-runtime init failure did not clear output/report EINVAL");
    }
    if (llam_runtime_create(&good_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("explicit runtime did not create after registered init failure");
    }
    llam_runtime_destroy(runtime);
    return 0;
}

static int test_legacy_runtime_init_ignores_resource_tail(void) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("resource-tail opts init failed");
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.affinity_policy = UINT32_MAX;

    /*
     * The source-compatible convenience wrapper is frozen at the 2.2 prefix:
     * old source that recompiles with a newer header must not silently opt into
     * newly appended resource policy.  The size-aware entry point is the only
     * path that may observe and reject this invalid tail.
     */
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("legacy runtime init consumed the resource tail");
    }
    llam_runtime_shutdown();

    errno = 0;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != -1 ||
        errno != EINVAL) {
        llam_runtime_shutdown();
        return test_fail("size-aware runtime init did not reject invalid affinity policy");
    }
    return 0;
}

typedef struct resource_plan_case {
    const char *name;
    llam_runtime_opts_t opts;
    size_t opts_size;
    const unsigned *allowed_cpus;
    unsigned allowed_cpu_count;
    bool affinity_supported;
    bool sqpoll_supported;
    int expected_errno;
    unsigned worker_min;
    unsigned worker_count;
    unsigned worker_max;
    unsigned blocking_min;
    unsigned blocking_max;
    unsigned selected_cpu_count;
    const unsigned *selected_cpus;
    bool sqpoll_reserved;
    int sqpoll_cpu;
} resource_plan_case_t;

#include "test_runtime_stack_cache_plan.inc"

#if defined(__linux__)
#define TEST_LEGACY_BLOCKING_ONE_CPU 1U
#else
#define TEST_LEGACY_BLOCKING_ONE_CPU 2U
#endif

static int test_runtime_resource_plan_resolver(void) {
    static const unsigned cpus_1[] = {7U};
    static const unsigned cpus_8[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    static const unsigned cpus_64[] = {
        0U,  1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,  9U,  10U, 11U, 12U,
        13U, 14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U, 22U, 23U, 24U, 25U,
        26U, 27U, 28U, 29U, 30U, 31U, 32U, 33U, 34U, 35U, 36U, 37U, 38U,
        39U, 40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 48U, 49U, 50U, 51U,
        52U, 53U, 54U, 55U, 56U, 57U, 58U, 59U, 60U, 61U, 62U, 63U,
    };
    static const unsigned cpus_sparse[] = {11U, 3U, 29U, 7U};
    static const uint32_t requested_sparse[] = {29U, 7U, 11U};
    static const unsigned expected_sparse[] = {29U, 7U, 11U};
    static const uint32_t requested_duplicate[] = {3U, 3U};
    static const uint32_t requested_disallowed[] = {3U, 99U};
    static const unsigned cpus_sqpoll[] = {0U, 2U, 4U, 6U};
    static const unsigned expected_sqpoll[] = {0U, 2U, 4U};
    static const resource_plan_case_t cases[] = {
        {
            .name = "one-cpu automatic",
            .opts = {.sqpoll_cpu = -1, .blocking_max = 1U},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_1,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .worker_min = 1U,
            .worker_count = 1U,
            .worker_max = 1U,
            .blocking_min = 0U,
            .blocking_max = 1U,
            .selected_cpu_count = 1U,
            .selected_cpus = cpus_1,
            .sqpoll_cpu = -1,
        },
        {
            .name = "eight-cpu fixed",
            .opts = {.sqpoll_cpu = -1, .worker_count = 4U, .blocking_min = 1U, .blocking_max = 4U},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_8,
            .allowed_cpu_count = 8U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .worker_min = 4U,
            .worker_count = 4U,
            .worker_max = 4U,
            .blocking_min = 1U,
            .blocking_max = 4U,
            .selected_cpu_count = 4U,
            .selected_cpus = cpus_8,
            .sqpoll_cpu = -1,
        },
        {
            .name = "sixty-four-cpu dynamic",
            .opts = {
                .sqpoll_cpu = -1,
                .worker_min = 4U,
                .worker_count = 16U,
                .worker_max = 32U,
                .blocking_min = 2U,
                .blocking_max = 8U,
                .affinity_policy = LLAM_RUNTIME_AFFINITY_PREFER,
                .task_prewarm_total = 64U,
                .stack_prewarm_total = 32U,
                .timer_prewarm_total = 128U,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_64,
            .allowed_cpu_count = 64U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .worker_min = 4U,
            .worker_count = 16U,
            .worker_max = 32U,
            .blocking_min = 2U,
            .blocking_max = 8U,
            .selected_cpu_count = 32U,
            .selected_cpus = cpus_64,
            .sqpoll_cpu = -1,
        },
        {
            .name = "sparse caller order",
            .opts = {
                .sqpoll_cpu = -1,
                .worker_count = 3U,
                .blocking_max = 2U,
                .cpu_count = 3U,
                .cpu_ids = requested_sparse,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_sparse,
            .allowed_cpu_count = 4U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .worker_min = 3U,
            .worker_count = 3U,
            .worker_max = 3U,
            .blocking_min = 0U,
            .blocking_max = 2U,
            .selected_cpu_count = 3U,
            .selected_cpus = expected_sparse,
            .sqpoll_cpu = -1,
        },
        {
            .name = "duplicate caller CPUs",
            .opts = {
                .sqpoll_cpu = -1,
                .worker_count = 2U,
                .blocking_max = 1U,
                .cpu_count = 2U,
                .cpu_ids = requested_duplicate,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_sparse,
            .allowed_cpu_count = 4U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EINVAL,
        },
        {
            .name = "disallowed caller CPU",
            .opts = {
                .sqpoll_cpu = -1,
                .worker_count = 2U,
                .blocking_max = 1U,
                .cpu_count = 2U,
                .cpu_ids = requested_disallowed,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_sparse,
            .allowed_cpu_count = 4U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EINVAL,
        },
        {
            .name = "deterministic worker conflict",
            .opts = {.deterministic = 1U, .sqpoll_cpu = -1, .worker_count = 2U, .blocking_max = 1U},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_8,
            .allowed_cpu_count = 8U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EINVAL,
        },
        {
            .name = "reversed blocking bounds",
            .opts = {.sqpoll_cpu = -1, .worker_count = 2U, .blocking_min = 5U, .blocking_max = 4U},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_8,
            .allowed_cpu_count = 8U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EINVAL,
        },
        {
            .name = "required affinity unsupported",
            .opts = {
                .sqpoll_cpu = -1,
                .worker_count = 2U,
                .blocking_max = 1U,
                .affinity_policy = LLAM_RUNTIME_AFFINITY_REQUIRE,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_8,
            .allowed_cpu_count = 8U,
            .affinity_supported = false,
            .sqpoll_supported = true,
            .expected_errno = ENOTSUP,
        },
        {
            .name = "automatic SQPOLL reservation",
            .opts = {
                .experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL,
                .sqpoll_cpu = -1,
                .worker_count = 3U,
                .blocking_max = 1U,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = expected_sqpoll,
            .allowed_cpu_count = 3U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EINVAL,
        },
        {
            .name = "four-CPU SQPOLL reservation",
            .opts = {
                .experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL,
                .sqpoll_cpu = -1,
                .worker_count = 3U,
                .blocking_max = 1U,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_sqpoll,
            .allowed_cpu_count = 4U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .worker_min = 3U,
            .worker_count = 3U,
            .worker_max = 3U,
            .blocking_min = 0U,
            .blocking_max = 1U,
            .selected_cpu_count = 3U,
            .selected_cpus = expected_sqpoll,
            .sqpoll_reserved = true,
            .sqpoll_cpu = 6,
        },
        {
            .name = "SQPOLL unsupported",
            .opts = {
                .experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL,
                .sqpoll_cpu = -1,
                .worker_count = 1U,
                .blocking_max = 1U,
            },
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_1,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = false,
            .expected_errno = ENOTSUP,
        },
        {
            .name = "stack prewarm above hard cap",
            .opts = {.sqpoll_cpu = -1, .worker_count = 1U, .blocking_max = 1U, .stack_prewarm_total = 4097U},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_1,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = E2BIG,
        },
        {
            .name = "metadata estimate overflow",
            .opts = {.sqpoll_cpu = -1, .worker_count = 1U, .blocking_max = 1U, .task_prewarm_total = UINT64_MAX},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_1,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EOVERFLOW,
        },
        {
            .name = "ambiguous worker bounds",
            .opts = {.sqpoll_cpu = -1, .worker_min = 1U, .blocking_max = 1U},
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_8,
            .allowed_cpu_count = 8U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .expected_errno = EINVAL,
        },
        {
            .name = "legacy prefix ignores new tail",
            .opts = {
                .sqpoll_cpu = -1,
                .worker_count = UINT32_MAX,
                .blocking_max = UINT32_MAX,
                .affinity_policy = UINT32_MAX,
                .stack_cache_budget_bytes = UINT64_MAX,
                .stack_cache_high_watermark_bytes = UINT64_MAX,
                .stack_cache_low_watermark_bytes = UINT64_MAX,
                .stack_cache_idle_ns = UINT64_MAX,
                .stack_cache_flags = UINT32_MAX,
            },
            .opts_size = LLAM_RUNTIME_OPTS_V2_2_SIZE,
            .allowed_cpus = cpus_1,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .worker_min = 1U,
            .worker_count = 1U,
            .worker_max = 1U,
            .blocking_min = TEST_LEGACY_BLOCKING_ONE_CPU,
            .blocking_max = TEST_LEGACY_BLOCKING_ONE_CPU,
            .selected_cpu_count = 1U,
            .selected_cpus = cpus_1,
            .sqpoll_cpu = -1,
        },
    };
    unsigned cpus_257[257];
    uint32_t requested_257[257];
    unsigned i;
    size_t case_index;

    for (i = 0U; i < 257U; ++i) {
        cpus_257[i] = i;
        requested_257[i] = i;
    }

    for (case_index = 0U; case_index < sizeof(cases) / sizeof(cases[0]); ++case_index) {
        const resource_plan_case_t *test_case = &cases[case_index];
        llam_runtime_resource_plan_input_t input = {
            .opts = &test_case->opts,
            .opts_size = test_case->opts_size,
            .allowed_cpus = test_case->allowed_cpus,
            .allowed_cpu_count = test_case->allowed_cpu_count,
            .affinity_supported = test_case->affinity_supported,
            .sqpoll_supported = test_case->sqpoll_supported,
            .page_size = 4096U,
        };
        llam_runtime_resource_plan_t plan;
        int rc;

        memset(&plan, 0xA5, sizeof(plan));
        errno = 0;
        rc = llam_runtime_resource_plan_resolve(&input, &plan);
        if (test_case->expected_errno != 0) {
            if (rc != -1 || errno != test_case->expected_errno) {
                fprintf(stderr,
                        "[test_runtime_core] resource plan case '%s' returned rc=%d errno=%d, expected errno=%d\n",
                        test_case->name,
                        rc,
                        errno,
                        test_case->expected_errno);
                return 1;
            }
            if (plan.worker_max != 0U || plan.selected_cpu_count != 0U ||
                plan.estimated_metadata_bytes != 0U) {
                return test_fail("failed resource plan exposed a partial result");
            }
            continue;
        }
        if (rc != 0) {
            fprintf(stderr,
                    "[test_runtime_core] resource plan case '%s' failed: errno=%d (%s)\n",
                    test_case->name,
                    errno,
                    strerror(errno));
            return 1;
        }
        if (plan.worker_min != test_case->worker_min ||
            plan.worker_count != test_case->worker_count ||
            plan.worker_max != test_case->worker_max ||
            plan.blocking_min != test_case->blocking_min ||
            plan.blocking_max != test_case->blocking_max ||
            plan.selected_cpu_count != test_case->selected_cpu_count ||
            plan.sqpoll_reserved != test_case->sqpoll_reserved ||
            plan.sqpoll_cpu != test_case->sqpoll_cpu ||
            plan.stack_cache_budget_bytes !=
                LLAM_RUNTIME_STACK_CACHE_DEFAULT_BUDGET_BYTES ||
            plan.stack_cache_high_watermark_bytes !=
                LLAM_RUNTIME_STACK_CACHE_DEFAULT_HIGH_WATERMARK_BYTES ||
            plan.stack_cache_low_watermark_bytes !=
                LLAM_RUNTIME_STACK_CACHE_DEFAULT_LOW_WATERMARK_BYTES ||
            plan.stack_cache_idle_ns != LLAM_RUNTIME_STACK_CACHE_DEFAULT_IDLE_NS ||
            plan.stack_cache_flags != 0U) {
            fprintf(stderr, "[test_runtime_core] resource plan case '%s' resolved wrong bounds\n", test_case->name);
            return 1;
        }
        for (i = 0U; i < plan.selected_cpu_count; ++i) {
            if (plan.selected_cpus[i] != test_case->selected_cpus[i]) {
                fprintf(stderr,
                        "[test_runtime_core] resource plan case '%s' reordered CPU %u\n",
                        test_case->name,
                        i);
                return 1;
            }
        }
        if (plan.estimated_metadata_bytes == 0U ||
            plan.estimated_stack_mapping_bytes !=
                plan.stack_prewarm_total * LLAM_RUNTIME_STACK_MAPPING_ESTIMATE_BYTES) {
            return test_fail("resource plan estimates were not resolved exactly");
        }
    }

    {
        llam_runtime_opts_t opts = {.sqpoll_cpu = -1, .blocking_max = 1U};
        llam_runtime_resource_plan_input_t input = {
            .opts = &opts,
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_257,
            .allowed_cpu_count = 257U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .page_size = 4096U,
        };
        llam_runtime_resource_plan_t plan;

        if (llam_runtime_resource_plan_resolve(&input, &plan) != 0 ||
            plan.worker_max != LLAM_RUNTIME_MAX_WORKERS ||
            plan.selected_cpu_count != LLAM_RUNTIME_MAX_WORKERS ||
            plan.selected_cpus[LLAM_RUNTIME_MAX_WORKERS - 1U] != 255U) {
            return test_fail_errno("257-CPU automatic resource plan did not cap safely");
        }
    }

    {
        llam_runtime_opts_t opts = {
            .sqpoll_cpu = -1,
            .worker_count = 1U,
            .blocking_max = 1U,
            .cpu_count = 257U,
            .cpu_ids = requested_257,
        };
        llam_runtime_resource_plan_input_t input = {
            .opts = &opts,
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus_257,
            .allowed_cpu_count = 257U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .page_size = 4096U,
        };
        llam_runtime_resource_plan_t plan;

        errno = 0;
        if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 || errno != E2BIG) {
            return test_fail("257-entry explicit CPU list did not fail with E2BIG");
        }
    }

    if (test_stack_cache_resource_plan(cpus_1) != 0) {
        return 1;
    }

    return 0;
}

static int test_runtime_total_prewarm_distribution(void) {
    static const unsigned shard_counts[] = {1U, 8U, 64U};
    static const uint64_t totals[] = {UINT64_C(257), UINT64_C(129), UINT64_C(1025)};
    uint64_t storage_objects = 0U;

    for (size_t resource = 0U;
         resource < sizeof(totals) / sizeof(totals[0]);
         ++resource) {
        for (size_t count_index = 0U;
             count_index < sizeof(shard_counts) / sizeof(shard_counts[0]);
             ++count_index) {
            unsigned count = shard_counts[count_index];
            uint64_t total = totals[resource];
            uint64_t base = total / count;
            uint64_t remainder = total % count;
            uint64_t sum = 0U;

            for (unsigned index = 0U; index < count; ++index) {
                uint64_t share = llam_runtime_prewarm_share(total, count, index);
                uint64_t expected = base + (index < remainder ? 1U : 0U);

                if (share != expected) {
                    fprintf(stderr,
                            "[test_runtime_core] prewarm resource=%zu total=%llu count=%u index=%u share=%llu expected=%llu\n",
                            resource,
                            (unsigned long long)total,
                            count,
                            index,
                            (unsigned long long)share,
                            (unsigned long long)expected);
                    return 1;
                }
                sum += share;
            }
            if (sum != total ||
                llam_runtime_prewarm_share(total, count, count) != 0U ||
                llam_runtime_prewarm_share(total, 0U, 0U) != 0U) {
                return test_fail("runtime-total prewarm distribution did not preserve its aggregate");
            }
        }
    }
    if (!llam_runtime_task_prewarm_storage_objects(1U, 64U, &storage_objects) ||
        storage_objects != LLAM_TASK_SLAB_COUNT ||
        !llam_runtime_task_prewarm_storage_objects(65U, 64U, &storage_objects) ||
        storage_objects != UINT64_C(64) * LLAM_TASK_SLAB_COUNT ||
        llam_runtime_task_prewarm_storage_objects(UINT64_MAX,
                                                  1U,
                                                  &storage_objects)) {
        return test_fail("task prewarm slab rounding was not checked exactly");
    }
    return 0;
}

static int test_runtime_resource_plan_initialization(void) {
    llam_runtime_opts_t bad_opts;
    llam_runtime_t *runtime = NULL;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_cpu_count;

    if (llam_runtime_opts_init(&bad_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("invalid resource opts init failed");
    }
    bad_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    bad_opts.worker_min = 2U;
    bad_opts.worker_count = 1U;
    bad_opts.worker_max = 2U;
    bad_opts.blocking_min = 1U;
    bad_opts.blocking_max = 1U;

    errno = 0;
    if (llam_runtime_create(&bad_opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != -1 ||
        errno != EINVAL ||
        runtime != NULL) {
        llam_runtime_destroy(runtime);
        return test_fail("invalid exact resource plan published a runtime");
    }

    if (assert_fixed_runtime_resource_stats(1U) != 0) {
        return 1;
    }
    allowed_cpu_count = llam_count_allowed_cpus(&allowed_cpus);
    free(allowed_cpus);
    if (allowed_cpu_count >= 2U &&
        assert_fixed_runtime_resource_stats(2U) != 0) {
        return 1;
    }
    return 0;
}

static int test_blocking_pool_grows_lazily_within_bounds(void) {
    block_pool_growth_state_t state;
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *raw_runtime = NULL;
    llam_task_t *parent = NULL;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    atomic_init(&state.callbacks_started, 0U);
    atomic_init(&state.callbacks_active, 0U);
    atomic_init(&state.callbacks_active_peak, 0U);
    atomic_init(&state.callbacks_completed, 0U);
    atomic_init(&state.release_callbacks, 0U);

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("lazy blocking pool opts init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 0U;
    opts.blocking_max = 2U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return test_fail_errno("lazy blocking pool runtime create failed");
    }
    if (llam_runtime_begin_public_op(runtime, &raw_runtime) != 0) {
        rc = test_fail_errno("lazy blocking pool runtime pin failed");
        goto cleanup;
    }
    state.runtime = raw_runtime;

    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("lazy blocking pool initial stats failed");
        goto cleanup;
    }
    if (stats.configured_blocking_min != 0U ||
        stats.configured_blocking_max != 2U ||
        stats.blocking_threads != 0U ||
        atomic_load_explicit(&raw_runtime->block_threads_started,
                             memory_order_acquire) != 0U) {
        rc = test_fail("zero-min blocking pool eagerly created workers");
        goto cleanup;
    }

    parent = llam_runtime_spawn_ex(runtime, block_pool_growth_parent, &state, NULL, 0U);
    if (parent == NULL) {
        rc = test_fail_errno("lazy blocking pool parent spawn failed");
        goto cleanup;
    }
    if (llam_runtime_run_handle(runtime) != 0 || llam_join(parent) != 0) {
        parent = NULL;
        rc = test_fail_errno("lazy blocking pool run/join failed");
        goto cleanup;
    }
    parent = NULL;

    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("lazy blocking pool final stats failed");
        goto cleanup;
    }
    if (atomic_load_explicit(&state.core.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.core.ran, memory_order_acquire) != 1U ||
        state.confirmed_before_release != 2U ||
        state.entered_before_release != 2U ||
        state.live_before_release != 2U ||
        atomic_load_explicit(&state.callbacks_started, memory_order_acquire) !=
            BLOCK_POOL_GROWTH_TASKS ||
        atomic_load_explicit(&state.callbacks_completed, memory_order_acquire) !=
            BLOCK_POOL_GROWTH_TASKS ||
        atomic_load_explicit(&state.callbacks_active, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.callbacks_active_peak, memory_order_acquire) != 2U ||
        atomic_load_explicit(&raw_runtime->block_threads_started, memory_order_acquire) != 2U ||
        atomic_load_explicit(&raw_runtime->block_threads_entered, memory_order_acquire) != 2U ||
        atomic_load_explicit(&raw_runtime->block_threads_exited, memory_order_acquire) != 0U ||
        atomic_load_explicit(&raw_runtime->block_threads_live, memory_order_acquire) != 2U ||
        stats.blocking_threads != 2U) {
        fprintf(stderr,
                "[test_runtime_core] lazy blocking pool mismatch: failures=%u ran=%u "
                "confirmed=%u/%u entered=%u/%u live=%u/%u exited=%u "
                "callbacks=%u/%u active=%u peak=%u stats_live=%u\n",
                atomic_load_explicit(&state.core.failures, memory_order_acquire),
                atomic_load_explicit(&state.core.ran, memory_order_acquire),
                state.confirmed_before_release,
                atomic_load_explicit(&raw_runtime->block_threads_started, memory_order_acquire),
                state.entered_before_release,
                atomic_load_explicit(&raw_runtime->block_threads_entered, memory_order_acquire),
                state.live_before_release,
                atomic_load_explicit(&raw_runtime->block_threads_live, memory_order_acquire),
                atomic_load_explicit(&raw_runtime->block_threads_exited, memory_order_acquire),
                atomic_load_explicit(&state.callbacks_started, memory_order_acquire),
                atomic_load_explicit(&state.callbacks_completed, memory_order_acquire),
                atomic_load_explicit(&state.callbacks_active, memory_order_acquire),
                atomic_load_explicit(&state.callbacks_active_peak, memory_order_acquire),
                stats.blocking_threads);
        goto cleanup;
    }
    rc = 0;

cleanup:
    atomic_store_explicit(&state.release_callbacks, 1U, memory_order_release);
    if (parent != NULL) {
        (void)llam_detach(parent);
    }
    if (raw_runtime != NULL) {
        llam_runtime_end_public_op(raw_runtime);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static int test_native_thread_diagnostics_are_live_counts(void) {
    native_thread_stats_state_t state;
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_task_t *task = NULL;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_cpu_count;
    unsigned worker_count;
    unsigned runtime_owned_after_run;
    int rc = 1;

    allowed_cpu_count = llam_count_allowed_cpus(&allowed_cpus);
    free(allowed_cpus);
    if (allowed_cpu_count == 0U) {
        return test_fail_errno("native thread diagnostics CPU discovery failed");
    }
    worker_count = allowed_cpu_count >= 2U ? 2U : 1U;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    state.runtime = &g_llam_runtime;
    state.expected_scheduler_threads = worker_count - 1U;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("native thread diagnostics opts init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = worker_count;
    opts.worker_count = worker_count;
    opts.worker_max = worker_count;
    opts.blocking_min = 1U;
    opts.blocking_max = 2U;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("native thread diagnostics runtime init failed");
    }
    for (unsigned i = 0U; i < g_llam_runtime.active_nodes; ++i) {
        if (g_llam_runtime.nodes[i].thread_started) {
            state.expected_io_threads += 1U;
        }
    }

    task = llam_spawn(native_thread_stats_task, &state, NULL);
    if (task == NULL) {
        rc = test_fail_errno("native thread diagnostics task spawn failed");
        goto cleanup;
    }
    if (llam_run() != 0 || llam_join(task) != 0) {
        task = NULL;
        rc = test_fail_errno("native thread diagnostics run/join failed");
        goto cleanup;
    }
    task = NULL;
    if (atomic_load_explicit(&state.core.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.core.ran, memory_order_acquire) != 1U) {
        rc = test_fail("native thread diagnostics managed checks failed");
        goto cleanup;
    }

    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("native thread post-run stats failed");
        goto cleanup;
    }
    runtime_owned_after_run =
        1U + state.expected_io_threads + 1U + 1U;
    if (stats.scheduler_threads != 0U ||
        stats.blocking_threads != 1U ||
        stats.io_threads != state.expected_io_threads ||
        stats.controller_threads != 1U ||
        stats.opaque_helper_threads != 1U ||
        stats.runtime_owned_threads != runtime_owned_after_run ||
        stats.native_execution_threads != runtime_owned_after_run ||
        atomic_load_explicit(&g_llam_runtime.host_threads_live,
                             memory_order_acquire) != 0U) {
        rc = test_fail("native thread post-run stats retained a host scheduler");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&g_llam_runtime.scheduler_threads_live,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.block_threads_live,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.io_threads_live,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.controller_threads_live,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.opaque_helper_threads_live,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_runtime.host_threads_live,
                             memory_order_acquire) != 0U) {
        return test_fail("native thread counters survived runtime shutdown");
    }
    return rc;
}

static void nested_runtime_create_task(void *arg) {
    nested_runtime_create_state_t *state = arg;
    llam_task_t *self_before = llam_current_task();
    llam_task_t *self_after;
    llam_runtime_opts_t opts;

    if (self_before == NULL) {
        task_fail(&state->core, "nested runtime create task had no current task", EINVAL);
        return;
    }

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &state->created_runtime) != 0) {
        task_fail(&state->core, "llam_runtime_create from managed task failed", errno);
        return;
    }
    if (state->created_runtime == NULL || state->created_runtime == llam_runtime_default()) {
        task_fail(&state->core, "nested runtime create returned invalid runtime handle", EINVAL);
        return;
    }

    /*
     * Creating an explicit runtime from inside a managed task must not erase the
     * caller's TLS task/shard context.  A regression here makes later sync/I/O
     * calls run as unmanaged host calls and breaks multi-runtime embedding.
     */
    self_after = llam_current_task();
    if (self_after == NULL || self_after != self_before) {
        task_fail(&state->core, "llam_runtime_create corrupted managed task TLS", EINVAL);
        return;
    }
    llam_yield();
    self_after = llam_current_task();
    if (self_after == NULL || self_after != self_before) {
        task_fail(&state->core, "managed task TLS was not stable after nested create yield", EINVAL);
        return;
    }
    atomic_fetch_add_explicit(&state->core.ran, 1U, memory_order_relaxed);
}

static int test_runtime_create_preserves_managed_tls(void) {
    nested_runtime_create_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *task;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("default runtime init for nested create test failed");
    }
    task = llam_spawn(nested_runtime_create_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("nested runtime create task spawn failed");
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("nested runtime create test run failed");
    }
    if (llam_join(task) != 0 && rc == 0) {
        rc = test_fail_errno("nested runtime create task join failed");
    }
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U && rc == 0) {
        errno = state.core.first_errno;
        rc = test_fail(state.core.first_case);
    }
    if (atomic_load_explicit(&state.core.ran, memory_order_relaxed) != 1U && rc == 0) {
        rc = test_fail("nested runtime create task did not complete");
    }
    /*
     * Foreign runtime destruction from the managed task is intentionally ignored;
     * the host that owns the newly-created handle tears it down after the task
     * exits so the test also covers post-run explicit destroy.
     */
    llam_runtime_destroy(state.created_runtime);
    llam_runtime_shutdown();
    return rc;
}

static int test_runtime_handle_api(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *second_runtime = NULL;
    llam_runtime_t *fake_runtime = (llam_runtime_t *)(void *)&state;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_create(&runtime_opts, sizeof(runtime_opts), &runtime) != 0) {
        return test_fail_errno("llam_runtime_create failed");
    }
    if (runtime == NULL || runtime == llam_runtime_default()) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_create returned unexpected handle");
    }
    errno = 0;
    if (llam_runtime_run_handle(NULL) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_run_handle(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex_handle(NULL, &stats, sizeof(stats)) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_collect_stats_ex_handle(NULL) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_run_handle(fake_runtime) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_run_handle(non-default) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_request_stop_rt(fake_runtime) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_request_stop_rt(non-default) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_collect_stats_ex_rt(fake_runtime, &stats, sizeof(stats)) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_collect_stats_ex_rt(non-default) did not fail with EINVAL");
    }
    errno = 0;
    if (llam_runtime_write_stats_json_rt(fake_runtime, 1) != -1 || errno != EINVAL) {
        llam_runtime_destroy(runtime);
        return test_fail("llam_runtime_write_stats_json_rt(non-default) did not fail with EINVAL");
    }
    if (llam_runtime_collect_stats_ex_rt(runtime, &stats, sizeof(stats)) != 0) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_runtime_collect_stats_ex_rt(default) failed");
    }
    if (llam_runtime_create(&runtime_opts, sizeof(runtime_opts), &second_runtime) != 0 ||
        second_runtime == NULL ||
        second_runtime == runtime ||
        second_runtime == llam_runtime_default()) {
        llam_runtime_destroy(runtime);
        return test_fail("second llam_runtime_create did not return an independent handle");
    }
    llam_runtime_destroy(second_runtime);
    second_runtime = NULL;
    task = llam_runtime_spawn_ex(runtime, detached_task, &state, NULL, 0U);
    if (task == NULL) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_spawn for runtime handle failed");
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_runtime_run_handle failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_destroy(runtime);
        return test_fail("runtime handle task did not run");
    }
    if (llam_join(task) != 0) {
        llam_runtime_destroy(runtime);
        return test_fail_errno("llam_join for runtime handle task failed");
    }
    llam_runtime_destroy(runtime);
    return 0;
}

static int test_runtime_lifecycle_and_task_contracts(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_stats_t stats;
    llam_runtime_stats_t prefix_stats;
    llam_task_t *task;
    unsigned expected_flags = LLAM_SPAWN_F_PINNED | LLAM_SPAWN_F_NO_PREEMPT;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    state.expected_flags = expected_flags;
    state.expected_class = LLAM_TASK_CLASS_BATCH;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 2U;
    runtime_opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init failed");
    }
    errno = 0;
    if (llam_runtime_init(&runtime_opts) != -1 || errno != EBUSY) {
        llam_runtime_shutdown();
        return test_fail("second llam_runtime_init did not fail with EBUSY");
    }
    errno = 0;
    if (llam_spawn(NULL, NULL, NULL) != NULL || errno != EINVAL) {
        llam_runtime_shutdown();
        return test_fail("llam_spawn(NULL) after init did not fail with EINVAL");
    }
    errno = 0;
    task = llam_runtime_spawn_ex(NULL, inspect_task, &state, NULL, 0U);
    if (task != NULL || errno != EINVAL) {
        if (task != NULL) {
            (void)llam_detach(task);
        }
        llam_runtime_shutdown();
        return test_fail("llam_runtime_spawn_ex(NULL) after init did not fail with EINVAL");
    }
    {
        llam_spawn_opts_t invalid_spawn_opts;

        memset(&invalid_spawn_opts, 0, sizeof(invalid_spawn_opts));
        invalid_spawn_opts.task_class = 999U;
        errno = 0;
        if (llam_spawn_ex(inspect_task, &state, &invalid_spawn_opts, sizeof(invalid_spawn_opts)) != NULL ||
            errno != EINVAL) {
            llam_runtime_shutdown();
            return test_fail("llam_spawn_ex invalid task class did not fail with EINVAL");
        }
        memset(&invalid_spawn_opts, 0, sizeof(invalid_spawn_opts));
        invalid_spawn_opts.stack_class = 999U;
        errno = 0;
        if (llam_spawn_ex(inspect_task, &state, &invalid_spawn_opts, sizeof(invalid_spawn_opts)) != NULL ||
            errno != EINVAL) {
            llam_runtime_shutdown();
            return test_fail("llam_spawn_ex invalid stack class did not fail with EINVAL");
        }
    }

    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = LLAM_TASK_CLASS_BATCH;
    spawn_opts.stack_class = LLAM_STACK_CLASS_LARGE;
    spawn_opts.flags = expected_flags;
    task = llam_spawn(inspect_task, &state, &spawn_opts);
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn inspect_task failed");
    }
    if (llam_task_id(task) == 0U ||
        llam_task_class(task) != LLAM_TASK_CLASS_BATCH ||
        (llam_task_flags(task) & expected_flags) != expected_flags) {
        llam_runtime_shutdown();
        return test_fail("spawned task metadata was not observable before run");
    }

    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("task or blocking callback did not run exactly once");
    }
    if (llam_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_runtime_collect_stats_ex failed");
    }
    if (stats.active_workers == 0U || stats.active_nodes == 0U || stats.yields == 0U ||
        stats.blocking_calls == 0U || stats.blocking_completions == 0U) {
        llam_runtime_shutdown();
        return test_fail("runtime stats did not record expected activity");
    }
    memset(&prefix_stats, 0xA5, sizeof(prefix_stats));
    if (llam_runtime_collect_stats_ex(&prefix_stats, offsetof(llam_runtime_stats_t, active_nodes)) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_runtime_collect_stats_ex prefix failed");
    }
    if (prefix_stats.ctx_switches == 0U || prefix_stats.yields == 0U ||
        prefix_stats.active_nodes != 0xA5A5A5A5U) {
        llam_runtime_shutdown();
        return test_fail("runtime stats prefix copy was not bounded");
    }
    if (llam_runtime_collect_stats(&stats) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_runtime_collect_stats wrapper failed");
    }
#if LLAM_PLATFORM_POSIX
    {
        int pipe_fds[2];
        char json[8192];
        ssize_t nread;

        if (pipe(pipe_fds) != 0) {
            llam_runtime_shutdown();
            return test_fail_errno("pipe for stats json failed");
        }
        if (llam_runtime_write_stats_json(pipe_fds[1]) != 0) {
            int saved_errno = errno;

            close(pipe_fds[0]);
            close(pipe_fds[1]);
            llam_runtime_shutdown();
            errno = saved_errno;
            return test_fail_errno("llam_runtime_write_stats_json failed");
        }
        close(pipe_fds[1]);
        nread = read(pipe_fds[0], json, sizeof(json) - 1U);
        close(pipe_fds[0]);
        if (nread <= 0) {
            llam_runtime_shutdown();
            return test_fail("stats json read produced no data");
        }
        json[nread] = '\0';
        if (json[0] != '{' ||
            strstr(json, "\"ctx_switches\":") == NULL ||
            strstr(json, "\"active_workers\":") == NULL ||
            strstr(json, "\"configured_worker_max\":") == NULL ||
            strstr(json, "\"configured_blocking_max\":") == NULL ||
            strstr(json, "\"scheduler_threads\":") == NULL ||
            strstr(json, "\"blocking_threads\":") == NULL ||
            strstr(json, "\"io_threads\":") == NULL ||
            strstr(json, "\"controller_threads\":") == NULL ||
            strstr(json, "\"opaque_helper_threads\":") == NULL ||
            strstr(json, "\"runtime_owned_threads\":") == NULL ||
            strstr(json, "\"native_execution_threads\":") == NULL ||
            strstr(json, "\"affinity_failures\":") == NULL ||
            strstr(json, "\"stack_cache_budget_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_high_watermark_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_low_watermark_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_idle_ns\":") == NULL ||
            strstr(json, "\"stack_cache_flags\":") == NULL ||
            strstr(json, "\"stack_cache_cached_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_cached_mappings\":") == NULL ||
            strstr(json, "\"stack_cache_committed_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_trim_requests\":") == NULL ||
            strstr(json, "\"stack_cache_discarded_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_released_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_budget_rejections\":") == NULL ||
            strstr(json, "\"stack_cache_secure_return_failures\":") == NULL ||
            strstr(json, "\"stack_cache_resident_valid\":") == NULL ||
            strstr(json, "\"stack_cache_resident_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_resident_sample_ns\":") == NULL ||
            strstr(json, "\"stack_cache_process_quarantine_bytes\":") == NULL ||
            strstr(json, "\"stack_cache_process_quarantine_mappings\":") == NULL ||
            strstr(json, "\"io_submit_syscalls\":") == NULL ||
            strstr(json, "\"yield_direct_attempts\":") == NULL ||
            strstr(json, "\"yield_direct_fail_push\":") == NULL ||
            strstr(json, "\"wake_handoff_attempts\":") == NULL ||
            strstr(json, "\"wake_handoff_fail_race\":") == NULL ||
            strstr(json, "\"autotune\":") == NULL ||
            strstr(json, "\"recognized_domains\":") == NULL ||
            strstr(json, "\"observable_domains\":") == NULL ||
            strstr(json, "\"controllable_domains\":") == NULL ||
            strstr(json, "\"active_observation_domains\":") == NULL ||
            strstr(json, "\"active_control_domains\":") == NULL ||
            strstr(json, "\"sample_period\":") == NULL ||
            strstr(json, "\"sampled_yield_handoff_fail_policy\":") == NULL ||
            strstr(json, "\"sampled_wake_handoff_hits\":") == NULL ||
            strstr(json, "\"sampled_wake_latency_p99_ns\":") == NULL) {
            llam_runtime_shutdown();
            return test_fail("stats json did not contain expected fields");
        }
        {
            const char *idle_spin_fallbacks = strstr(json, "\"idle_spin_fallbacks\":");

            if (idle_spin_fallbacks == NULL ||
                strstr(idle_spin_fallbacks + 1, "\"idle_spin_fallbacks\":") != NULL) {
                llam_runtime_shutdown();
                return test_fail("stats json field set is missing or duplicated");
            }
        }
    }
    {
        char path[] = "/tmp/llam-runtime-dump-XXXXXX";
        int dump_fd = mkstemp(path);
        off_t dump_size;
        char *dump = NULL;
        ssize_t dump_read;

        if (dump_fd < 0) {
            llam_runtime_shutdown();
            return test_fail_errno("mkstemp for runtime dump failed");
        }
        (void)unlink(path);
        llam_dump_runtime_state(dump_fd);
        dump_size = lseek(dump_fd, 0, SEEK_END);
        if (dump_size <= 0 || dump_size > (off_t)(1024U * 1024U) ||
            lseek(dump_fd, 0, SEEK_SET) < 0) {
            int saved_errno = errno;

            close(dump_fd);
            llam_runtime_shutdown();
            errno = saved_errno;
            return test_fail_errno("runtime dump sizing failed");
        }
        dump = malloc((size_t)dump_size + 1U);
        if (dump == NULL) {
            close(dump_fd);
            llam_runtime_shutdown();
            return test_fail_errno("runtime dump allocation failed");
        }
        dump_read = read(dump_fd, dump, (size_t)dump_size);
        close(dump_fd);
        if (dump_read != dump_size) {
            free(dump);
            llam_runtime_shutdown();
            return test_fail("runtime dump read was incomplete");
        }
        dump[dump_read] = '\0';
        if (strstr(dump, "lifecycle:") == NULL ||
            strstr(dump, "stop_requested=") == NULL ||
            strstr(dump, "active_io_waiters=") == NULL ||
            strstr(dump, "io_queues(") == NULL ||
            strstr(dump, "inflight_io_waiters=") == NULL ||
            strstr(dump, "wait_owner=") == NULL ||
            strstr(dump, "io_req=") == NULL ||
            strstr(dump, "block_job=") == NULL ||
            strstr(dump, "stack_cache:") == NULL ||
            strstr(dump, "cached_bytes=") == NULL ||
            strstr(dump, "resident_valid=") == NULL ||
            strstr(dump, "process_quarantine_mappings=") == NULL) {
            free(dump);
            llam_runtime_shutdown();
            return test_fail("runtime dump did not contain ownership diagnostics");
        }
        free(dump);
    }
#endif

    llam_runtime_shutdown();
    return 0;
}

static int test_request_stop_returns_success(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for request_stop failed");
    }
    if (llam_spawn(request_stop_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn request_stop task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run did not treat request_stop as success");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("request_stop task did not complete cleanly");
    }

    llam_runtime_shutdown();
    return 0;
}

static int test_shutdown_from_task_requests_stop(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for task shutdown failed");
    }
    if (llam_spawn(shutdown_from_task_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn task shutdown task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run did not complete after task shutdown");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("task shutdown did not degrade to stop cleanly");
    }

    llam_runtime_shutdown();
    return 0;
}

#if LLAM_PLATFORM_POSIX
static int test_runtime_dump_while_blocking_job_active(void) {
    dump_blocking_state_t state;
    llam_runtime_opts_t runtime_opts;
    pthread_t runner;
    bool runner_started = false;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    atomic_init(&state.blocking_started, 0U);
    atomic_init(&state.release_blocking, 0U);
    atomic_init(&state.task_done, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent dump failed");
    }
    if (llam_spawn(dump_blocking_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn concurrent dump task failed");
    }
    if (pthread_create(&runner, NULL, dump_run_thread, &state) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("pthread_create concurrent dump runner failed");
    }
    runner_started = true;

    for (unsigned i = 0U; i < 5000U &&
                          atomic_load_explicit(&state.blocking_started, memory_order_acquire) == 0U;
         ++i) {
        usleep(1000);
    }
    if (atomic_load_explicit(&state.blocking_started, memory_order_acquire) == 0U) {
        atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
        if (runner_started) {
            (void)pthread_join(runner, NULL);
        }
        llam_runtime_shutdown();
        return test_fail("blocking callback did not start before concurrent dump timeout");
    }

    /*
     * Runtime dumps are diagnostic reads that can run from another OS thread
     * while a blocking helper owns task wait state.  This loop keeps that
     * cross-thread ownership visible under TSan and guards against regressing
     * wait-owner fields back to plain data-racy pointers.
     */
    for (unsigned i = 0U; i < 64U; ++i) {
        FILE *dump = tmpfile();

        if (i == 32U) {
            atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
        }
        if (dump == NULL) {
            atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
            if (runner_started) {
                (void)pthread_join(runner, NULL);
            }
            llam_runtime_shutdown();
            return test_fail_errno("tmpfile for concurrent runtime dump failed");
        }
        llam_dump_runtime_state(fileno(dump));
        fclose(dump);
    }

    atomic_store_explicit(&state.release_blocking, 1U, memory_order_release);
    if (runner_started && pthread_join(runner, NULL) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("pthread_join concurrent dump runner failed");
    }
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.task_done, memory_order_acquire) != 1U) {
        fprintf(stderr,
                "[test_runtime_core] concurrent dump task failed at %s errno=%d (%s)\n",
                state.core.first_case,
                state.core.first_errno,
                strerror(state.core.first_errno));
        llam_runtime_shutdown();
        return 1;
    }

    llam_runtime_shutdown();
    return 0;
}
#endif

static int test_detach_contract(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_task_t *before_run;
    llam_task_t *after_run;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for detach failed");
    }
    before_run = llam_spawn(detached_task, &state, NULL);
    after_run = llam_spawn(detached_task, &state, NULL);
    if (before_run == NULL || after_run == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn detach tasks failed");
    }
    if (llam_detach(before_run) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_detach before run failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run detach failed");
    }
    if (llam_detach(after_run) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_detach after run failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return test_fail("detached tasks did not both complete");
    }

    llam_runtime_shutdown();
    return 0;
}

static void owner_diag_target_task(void *arg) {
    core_state_t *state = arg;

    atomic_fetch_add_explicit(&state->ran, 1U, memory_order_relaxed);
}

static void owner_diag_task(void *arg) {
    owner_diag_state_t *state = arg;
    llam_runtime_t *owner;
    llam_channel_t *raw_channel;
    llam_mutex_t *raw_mutex;
    llam_cond_t *raw_cond;
    llam_cancel_token_t *raw_token;
    llam_task_group_t *raw_group;
    void *out = NULL;
    llam_select_op_t op;
    size_t selected = 0U;

    raw_channel = llam_channel_resolve_public_handle(state->channel);
    if (raw_channel == NULL) {
        task_fail(&state->core, "channel owner diagnostic resolve", errno);
        return;
    }
    owner = raw_channel->owner_runtime;
    raw_channel->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "channel try send cross-runtime",
                                  llam_channel_try_send(state->channel, &state->payload));
    expect_runtime_owner_mismatch(&state->core,
                                  "channel try recv cross-runtime",
                                  llam_channel_try_recv_result(state->channel, &out));
    expect_runtime_owner_mismatch(&state->core,
                                  "channel close cross-runtime",
                                  llam_channel_close(state->channel));
    expect_runtime_owner_mismatch(&state->core,
                                  "channel destroy cross-runtime",
                                  llam_channel_destroy(state->channel));
    memset(&op, 0, sizeof(op));
    op.kind = LLAM_SELECT_OP_RECV;
    op.channel = state->channel;
    op.recv_out = &out;
    expect_runtime_owner_mismatch(&state->core,
                                  "channel select cross-runtime",
                                  llam_channel_select(&op, 1U, 0U, &selected));
    raw_channel->owner_runtime = owner;
    llam_channel_end_public_op(raw_channel);

    raw_mutex = llam_mutex_resolve_public_handle(state->mutex);
    if (raw_mutex == NULL) {
        task_fail(&state->core, "mutex owner diagnostic resolve", errno);
        return;
    }
    owner = raw_mutex->owner_runtime;
    raw_mutex->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "mutex trylock cross-runtime",
                                  llam_mutex_trylock(state->mutex));
    expect_runtime_owner_mismatch(&state->core,
                                  "mutex destroy cross-runtime",
                                  llam_mutex_destroy(state->mutex));
    raw_mutex->owner_runtime = owner;
    llam_mutex_end_public_op(raw_mutex);

    raw_cond = llam_cond_resolve_public_handle(state->cond);
    if (raw_cond == NULL) {
        task_fail(&state->core, "cond owner diagnostic resolve", errno);
        return;
    }
    owner = raw_cond->owner_runtime;
    raw_cond->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "cond signal cross-runtime",
                                  llam_cond_signal(state->cond));
    expect_runtime_owner_mismatch(&state->core,
                                  "cond broadcast cross-runtime",
                                  llam_cond_broadcast(state->cond));
    expect_runtime_owner_mismatch(&state->core,
                                  "cond destroy cross-runtime",
                                  llam_cond_destroy(state->cond));
    raw_cond->owner_runtime = owner;
    llam_cond_end_public_op(raw_cond);

    if (llam_cancel_token_retain_task_ref(state->token, &raw_token) != 0) {
        task_fail(&state->core, "cancel token owner diagnostic resolve", errno);
        return;
    }
    owner = raw_token->owner_runtime;
    raw_token->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "cancel token query cross-runtime",
                                  llam_cancel_token_is_cancelled(state->token));
    expect_runtime_owner_mismatch(&state->core,
                                  "cancel token cancel cross-runtime",
                                  llam_cancel_token_cancel(state->token));
    expect_runtime_owner_mismatch(&state->core,
                                  "cancel token destroy cross-runtime",
                                  llam_cancel_token_destroy(state->token));
    raw_token->owner_runtime = owner;
    llam_cancel_token_release_task_ref(raw_token);

    raw_group = llam_task_group_resolve_public_handle(state->group);
    if (raw_group == NULL) {
        task_fail(&state->core, "task group owner diagnostic resolve", errno);
        return;
    }
    owner = raw_group->owner_runtime;
    raw_group->owner_runtime = &state->fake_runtime;
    expect_runtime_owner_mismatch(&state->core,
                                  "task group cancel cross-runtime",
                                  llam_task_group_cancel(state->group));
    expect_runtime_owner_mismatch(&state->core,
                                  "task group join cross-runtime",
                                  llam_task_group_join_until(state->group, 0U));
    expect_runtime_owner_mismatch(&state->core,
                                  "task group destroy cross-runtime",
                                  llam_task_group_destroy(state->group));
    raw_group->owner_runtime = owner;
    llam_task_group_end_public_op(raw_group);

}

static int test_runtime_owner_mismatch_diagnostics(void) {
    owner_diag_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_runtime_t *owner;
    llam_task_t *diag_task = NULL;
    bool ran_runtime = false;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    state.payload = 42;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for owner diagnostics failed");
    }
    state.channel = llam_channel_create(1U);
    state.mutex = llam_mutex_create();
    state.cond = llam_cond_create();
    state.token = llam_cancel_token_create();
    state.group = llam_task_group_create();
    if (state.channel == NULL ||
        state.mutex == NULL ||
        state.cond == NULL ||
        state.token == NULL ||
        state.group == NULL) {
        rc = test_fail_errno("object create for owner diagnostics failed");
        goto cleanup;
    }
    state.target = llam_spawn(owner_diag_target_task, &state.core, NULL);
    if (state.target == NULL) {
        rc = test_fail_errno("spawn for owner diagnostics failed");
        goto cleanup;
    }
    {
        llam_task_t *raw_target = llam_task_resolve_public_handle(state.target);

        if (raw_target == NULL) {
            rc = test_fail_errno("resolve task for owner diagnostics failed");
            goto cleanup;
        }
        owner = raw_target->owner_runtime;
        raw_target->owner_runtime = &state.fake_runtime;
        expect_runtime_owner_mismatch(&state.core,
                                      "task join cross-runtime",
                                      llam_join_until(state.target, 0U));
        expect_runtime_owner_mismatch(&state.core,
                                      "task detach cross-runtime",
                                      llam_detach(state.target));
        raw_target->owner_runtime = owner;
        llam_task_end_public_op(raw_target);
    }
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U) {
        rc = test_fail("task owner diagnostics did not fail with EXDEV");
        goto cleanup;
    }
    diag_task = llam_spawn(owner_diag_task, &state, NULL);
    if (diag_task == NULL) {
        rc = test_fail_errno("spawn for owner diagnostics failed");
        goto cleanup;
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("llam_run for owner diagnostics failed");
        goto cleanup;
    }
    ran_runtime = true;
    if (llam_join(diag_task) != 0) {
        rc = test_fail_errno("llam_join for owner diagnostics failed");
        goto cleanup;
    }
    diag_task = NULL;
    if (llam_join(state.target) != 0) {
        rc = test_fail_errno("llam_join target for owner diagnostics failed");
        goto cleanup;
    }
    state.target = NULL;
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] owner diagnostics failed at %s errno=%d (%s)\n",
                state.core.first_case,
                state.core.first_errno,
                strerror(state.core.first_errno));
        goto cleanup;
    }
    if (atomic_load_explicit(&state.core.ran, memory_order_relaxed) != 1U) {
        rc = test_fail("owner diagnostics target task did not run");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (diag_task != NULL) {
        if (ran_runtime) {
            (void)llam_join(diag_task);
        } else {
            (void)llam_detach(diag_task);
        }
    }
    if (state.target != NULL) {
        if (ran_runtime) {
            (void)llam_join(state.target);
        } else {
            (void)llam_detach(state.target);
        }
    }
    if (state.group != NULL) {
        (void)llam_task_group_destroy(state.group);
    }
    if (state.token != NULL) {
        (void)llam_cancel_token_destroy(state.token);
    }
    if (state.cond != NULL) {
        (void)llam_cond_destroy(state.cond);
    }
    if (state.mutex != NULL) {
        (void)llam_mutex_destroy(state.mutex);
    }
    if (state.channel != NULL) {
        (void)llam_channel_destroy(state.channel);
    }
    llam_runtime_shutdown();
    return rc;
}

static int test_ex_option_prefixes(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_task_t *task;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    state.expected_flags = 0U;
    state.expected_class = LLAM_TASK_CLASS_LATENCY;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.forced_yield_every = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    if (llam_runtime_init_ex(&runtime_opts, offsetof(llam_runtime_opts_t, experimental_flags)) != 0) {
        return test_fail_errno("llam_runtime_init_ex prefix opts failed");
    }

    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.task_class = LLAM_TASK_CLASS_LATENCY;
    spawn_opts.stack_class = LLAM_STACK_CLASS_LARGE;
    spawn_opts.flags = LLAM_SPAWN_F_PINNED;
    task = llam_spawn_ex(inspect_task, &state, &spawn_opts, offsetof(llam_spawn_opts_t, flags));
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn_ex prefix opts failed");
    }
    if (llam_task_class(task) != LLAM_TASK_CLASS_LATENCY ||
        (llam_task_flags(task) & LLAM_SPAWN_F_PINNED) != 0U) {
        llam_runtime_shutdown();
        return test_fail("llam_spawn_ex did not honor prefix-sized spawn opts");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run for ex option prefixes failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.blocking_calls, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("ex option prefix task did not run exactly once");
    }

    llam_runtime_shutdown();
    return 0;
}

static void timer_api_task(void *arg) {
    core_state_t *state = arg;
    llam_timer_t *timer = NULL;
    uint64_t ticks = 0U;

    if (llam_timer_create(1000000000ULL, &timer) != 0 || timer == NULL) {
        task_fail(state, "llam_timer_create", errno);
        return;
    }
    if (llam_timer_wait_until(timer, 0U, &ticks) != -1 || errno != ETIMEDOUT) {
        task_fail(state, "llam_timer_wait_until expired", errno);
        (void)llam_timer_destroy(timer);
        return;
    }
    if (llam_timer_reset(timer, llam_now_ns() + 1000000ULL, 1000000ULL) != 0 ||
        llam_timer_wait(timer, &ticks) != 0 ||
        ticks == 0U) {
        task_fail(state, "llam_timer_wait first tick", errno);
        (void)llam_timer_destroy(timer);
        return;
    }
    if (llam_timer_reset(timer, llam_now_ns(), 1000000ULL) != 0 ||
        llam_timer_wait(timer, &ticks) != 0 ||
        ticks == 0U) {
        task_fail(state, "llam_timer_wait immediate tick", errno);
        (void)llam_timer_destroy(timer);
        return;
    }
    if (llam_timer_cancel(timer) != 0 ||
        llam_timer_wait_until(timer, llam_now_ns() + 50000000ULL, &ticks) != -1 ||
        errno != ECANCELED) {
        task_fail(state, "llam_timer_cancel wait", errno);
        (void)llam_timer_destroy(timer);
        return;
    }
    if (llam_timer_destroy(timer) != 0) {
        task_fail(state, "llam_timer_destroy", errno);
        return;
    }
    if (llam_timer_destroy(timer) != -1 || errno != EINVAL) {
        task_fail(state, "llam_timer_destroy stale", errno);
    }
}

static int test_waitable_timer_api(void) {
    core_state_t state;
    llam_runtime_opts_t opts;

    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    atomic_init(&state.failures, 0U);
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("timer runtime init failed");
    }
    if (llam_spawn(timer_api_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("timer task spawn failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("timer runtime run failed");
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] timer API failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        return 1;
    }
    return 0;
}

#if LLAM_PLATFORM_POSIX
static uint64_t test_stat_timespec_to_ns(const struct timespec *value) {
    return (uint64_t)value->tv_sec * UINT64_C(1000000000) +
           (uint64_t)value->tv_nsec;
}

static int test_stat_path_metadata_contract(void) {
    char directory[] = "/tmp/llam-stat-path-XXXXXX";
    char file_path[512];
    char link_path[512];
    struct timespec requested_times[2];
    struct stat native_stat;
    llam_file_stat_t stat_result;
    const struct timespec *native_atime;
    const struct timespec *native_mtime;
    uint64_t expected_atime;
    uint64_t expected_mtime;
    int saved_errno;
    int fd = -1;
    int rc = 1;

    if (mkdtemp(directory) == NULL) {
        return test_fail_errno("stat metadata fixture directory failed");
    }
    (void)snprintf(file_path, sizeof(file_path), "%s/target", directory);
    (void)snprintf(link_path, sizeof(link_path), "%s/link", directory);

    fd = open(file_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || write(fd, "x", 1U) != 1) {
        rc = test_fail_errno("stat metadata fixture file failed");
        goto cleanup;
    }
    close(fd);
    fd = -1;

    requested_times[0].tv_sec = 1700000000;
    requested_times[0].tv_nsec = 123456789;
    requested_times[1].tv_sec = 1700000001;
    requested_times[1].tv_nsec = 987654321;
    if (utimensat(AT_FDCWD, file_path, requested_times, 0) != 0 ||
        stat(file_path, &native_stat) != 0) {
        rc = test_fail_errno("stat metadata timestamp fixture failed");
        goto cleanup;
    }
#if LLAM_PLATFORM_DARWIN
    native_atime = &native_stat.st_atimespec;
    native_mtime = &native_stat.st_mtimespec;
#else
    native_atime = &native_stat.st_atim;
    native_mtime = &native_stat.st_mtim;
#endif
    expected_atime = test_stat_timespec_to_ns(native_atime);
    expected_mtime = test_stat_timespec_to_ns(native_mtime);

    memset(&stat_result, 0, sizeof(stat_result));
    if (llam_stat_path_ex(file_path, &stat_result, sizeof(stat_result)) != 0) {
        rc = test_fail_errno("stat metadata regular-file query failed");
        goto cleanup;
    }
    if (stat_result.atime_ns != expected_atime ||
        stat_result.mtime_ns != expected_mtime) {
        rc = test_fail("stat metadata lost subsecond timestamp precision");
        goto cleanup;
    }

    if (symlink(file_path, link_path) != 0) {
        rc = test_fail_errno("stat metadata symlink fixture failed");
        goto cleanup;
    }
    memset(&stat_result, 0, sizeof(stat_result));
    if (llam_stat_path_ex(link_path, &stat_result, sizeof(stat_result)) != 0) {
        rc = test_fail_errno("stat metadata symlink query failed");
        goto cleanup;
    }
    if (stat_result.type != LLAM_FILE_TYPE_SYMLINK) {
        rc = test_fail("stat metadata did not classify a symbolic link");
        goto cleanup;
    }
    rc = 0;

cleanup:
    saved_errno = errno;
    if (fd >= 0) {
        close(fd);
    }
    (void)unlink(link_path);
    (void)unlink(file_path);
    (void)rmdir(directory);
    errno = saved_errno;
    return rc;
}
#endif

static void blocking_wrapper_task(void *arg) {
    core_state_t *state = arg;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    llam_file_stat_t stat_result;
    llam_handle_t handle = LLAM_INVALID_HANDLE;
    int gai_error = 0;
    const char *null_path =
#if LLAM_PLATFORM_WINDOWS
        "NUL";
#else
        "/dev/null";
#endif

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    if (llam_getaddrinfo_result("localhost", "80", &hints, &result, &gai_error) != 0 ||
        gai_error != 0 ||
        result == NULL) {
        task_fail(state, "llam_getaddrinfo_result", gai_error != 0 ? gai_error : errno);
        return;
    }
    llam_freeaddrinfo_result(result);
    if (llam_stat_path_ex(".", &stat_result, LLAM_FILE_STAT_CURRENT_SIZE) != 0 ||
        stat_result.type == 0U) {
        task_fail(state, "llam_stat_path_ex", errno);
        return;
    }
    if (llam_open_async(null_path, O_RDONLY, 0U, &handle) != 0 ||
        LLAM_HANDLE_IS_INVALID(handle)) {
        task_fail(state, "llam_open_async", errno);
        return;
    }
    if (llam_close_handle(handle) != 0) {
        task_fail(state, "llam_close_handle after open_async", errno);
    }
}

static int test_blocking_wrappers_api(void) {
    core_state_t state;
    llam_runtime_opts_t opts;

    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    atomic_init(&state.failures, 0U);
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("blocking wrappers runtime init failed");
    }
    if (llam_spawn(blocking_wrapper_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("blocking wrapper task spawn failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("blocking wrappers runtime run failed");
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] blocking wrapper failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        return 1;
    }
    return 0;
}

#if LLAM_PLATFORM_LINUX
static void signal_waiter_task(void *arg) {
    signal_wait_state_t *state = arg;
    llam_signal_event_t event;

    if (llam_signal_wait_until(state->set, llam_now_ns() + 2000000000ULL, &event) != 0 ||
        event.signo != SIGUSR1 ||
        event.sequence == 0U) {
        task_fail(&state->core, "llam_signal_wait_until", errno);
    }
}

static void signal_sender_task(void *arg) {
    signal_wait_state_t *state = arg;

    (void)llam_sleep_ns(10000000ULL);
    if (kill(getpid(), SIGUSR1) != 0) {
        task_fail(&state->core, "kill SIGUSR1", errno);
    }
}
#endif

static int test_signal_wait_api(void) {
#if LLAM_PLATFORM_LINUX
    signal_wait_state_t state;
    llam_runtime_opts_t opts;
    llam_signal_set_t *duplicate = NULL;
    llam_signal_opts_t signal_opts;
    int signo = SIGUSR1;
    int rc = 0;

    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    memset(&signal_opts, 0, sizeof(signal_opts));
    atomic_init(&state.core.failures, 0U);
    if (llam_signal_set_create_ex(&signo, 1U, &signal_opts, 0U, &state.set) != -1 ||
        errno != EINVAL ||
        state.set != NULL) {
        return test_fail("signal opts with zero size did not fail with EINVAL");
    }
    if (llam_signal_set_create_ex(&signo, 1U, NULL, 0U, &state.set) != 0 || state.set == NULL) {
        return test_fail_errno("signal set create failed");
    }
    if (llam_signal_set_create_ex(&signo, 1U, NULL, 0U, &duplicate) != -1 || errno != EBUSY) {
        (void)llam_signal_set_destroy(state.set);
        return test_fail("duplicate signal set did not fail with EBUSY");
    }
    opts.deterministic = 1U;
    opts.forced_yield_every = 1U;
    if (llam_runtime_init(&opts) != 0) {
        (void)llam_signal_set_destroy(state.set);
        return test_fail_errno("signal runtime init failed");
    }
    if (llam_spawn(signal_waiter_task, &state, NULL) == NULL ||
        llam_spawn(signal_sender_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        (void)llam_signal_set_destroy(state.set);
        return test_fail_errno("signal task spawn failed");
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("signal runtime run failed");
    }
    llam_runtime_shutdown();
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] signal wait failed at %s errno=%d (%s)\n",
                state.core.first_case,
                state.core.first_errno,
                strerror(state.core.first_errno));
        rc = 1;
    }
    if (llam_signal_set_destroy(state.set) != 0) {
        rc = test_fail_errno("signal set destroy failed");
    }
    return rc;
#elif LLAM_PLATFORM_POSIX
    llam_signal_set_t *set = NULL;
    llam_signal_opts_t signal_opts;
    int signo = SIGTERM;

    memset(&signal_opts, 0, sizeof(signal_opts));
    if (llam_signal_set_create_ex(&signo, 1U, &signal_opts, 0U, &set) != -1 ||
        errno != EINVAL ||
        set != NULL) {
        return test_fail("non-Linux signal opts with zero size did not fail with EINVAL");
    }
    if (llam_signal_set_create_ex(&signo, 1U, NULL, 0U, &set) != -1 || errno != ENOTSUP) {
        if (set != NULL) {
            (void)llam_signal_set_destroy(set);
        }
        return test_fail("non-Linux signal set did not fail with ENOTSUP");
    }
    return 0;
#else
    return 0;
#endif
}

static int test_errno_is_task_local_across_switches(void) {
    core_state_t state;
    errno_task_args_t task_a;
    errno_task_args_t task_b;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    task_a.state = &state;
    task_a.yield_errno = ECHILD;
    task_a.sleep_errno = EADDRINUSE;
    task_b.state = &state;
    task_b.yield_errno = ENAMETOOLONG;
    task_b.sleep_errno = EALREADY;

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for errno isolation failed");
    }
    if (llam_spawn(errno_isolation_task, &task_a, NULL) == NULL ||
        llam_spawn(errno_isolation_task, &task_b, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn errno isolation task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run errno isolation failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return test_fail("errno isolation tasks did not both complete");
    }

    llam_runtime_shutdown();
    return 0;
}

static int test_direct_yield_failure_keeps_task_running(void) {
    core_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for direct yield failure failed");
    }
    if (llam_spawn(direct_yield_no_work_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn direct yield no-work task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run direct yield failure failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("direct yield no-work task did not run exactly once");
    }

    llam_runtime_shutdown();
    return 0;
}

#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
static int test_aarch64_simd_is_preserved_across_switches(void) {
    core_state_t state;
    aarch64_simd_state_t task_a;
    aarch64_simd_state_t task_b;
    llam_runtime_opts_t runtime_opts;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);

    task_a.state = &state;
    task_a.expected_d8_bits = UINT64_C(0x3ff123456789abcd);
    task_b.state = &state;
    task_b.expected_d8_bits = UINT64_C(0x400fedcba9876543);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for AArch64 SIMD preservation failed");
    }
    if (llam_spawn(aarch64_simd_preservation_task, &task_a, NULL) == NULL ||
        llam_spawn(aarch64_simd_preservation_task, &task_b, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn AArch64 SIMD preservation task failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run AArch64 SIMD preservation failed");
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                state.first_case,
                state.first_errno,
                strerror(state.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 2U) {
        llam_runtime_shutdown();
        return test_fail("AArch64 SIMD preservation tasks did not both complete");
    }

    llam_runtime_shutdown();
    return 0;
}
#endif

static int test_concurrent_join_contract(void) {
    core_state_t core;
    double_join_state_t state;
    llam_runtime_opts_t runtime_opts;

    memset(&core, 0, sizeof(core));
    atomic_init(&core.failures, 0U);
    atomic_init(&core.ran, 0U);
    atomic_init(&core.blocking_calls, 0U);

    memset(&state, 0, sizeof(state));
    state.state = &core;
    atomic_init(&state.target_done, 0U);
    atomic_init(&state.joined, 0U);
    atomic_init(&state.busy, 0U);

    memset(&runtime_opts, 0, sizeof(runtime_opts));
    runtime_opts.deterministic = 1U;
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (llam_runtime_init(&runtime_opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent join failed");
    }
    state.target = llam_spawn(double_join_target_task, &state, NULL);
    if (state.target == NULL ||
        llam_spawn(double_join_waiter_task, &state, NULL) == NULL ||
        llam_spawn(double_join_waiter_task, &state, NULL) == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_spawn concurrent join tasks failed");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run concurrent join failed");
    }
    if (atomic_load_explicit(&core.failures, memory_order_relaxed) != 0U) {
        fprintf(stderr,
                "[test_runtime_core] task failed at %s errno=%d (%s)\n",
                core.first_case,
                core.first_errno,
                strerror(core.first_errno));
        llam_runtime_shutdown();
        return 1;
    }
    if (atomic_load_explicit(&state.target_done, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.joined, memory_order_relaxed) != 1U ||
        atomic_load_explicit(&state.busy, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("concurrent join did not produce one success and one EBUSY");
    }

    llam_runtime_shutdown();
    return 0;
}

#if LLAM_PLATFORM_POSIX
typedef struct init_race_barrier {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    unsigned count;
    unsigned generation;
} init_race_barrier_t;

typedef struct init_race_state {
    init_race_barrier_t *barrier;
    atomic_uint successes;
    atomic_uint busy_failures;
    atomic_uint unexpected_failures;
} init_race_state_t;

static void init_race_barrier_wait(init_race_barrier_t *barrier) {
    unsigned generation;

    pthread_mutex_lock(&barrier->lock);
    generation = barrier->generation;
    barrier->count += 1U;
    if (barrier->count == 2U) {
        barrier->count = 0U;
        barrier->generation += 1U;
        pthread_cond_broadcast(&barrier->cv);
    } else {
        while (generation == barrier->generation) {
            pthread_cond_wait(&barrier->cv, &barrier->lock);
        }
    }
    pthread_mutex_unlock(&barrier->lock);
}

static void *init_race_thread(void *arg) {
    init_race_state_t *state = arg;
    llam_runtime_t *runtime = NULL;

    init_race_barrier_wait(state->barrier);
    if (llam_runtime_create(NULL, 0U, &runtime) == 0) {
        /*
         * Explicit runtime handles are independent. Destroy the handle inside
         * the creating thread so this race test does not leave heap runtimes in
         * the live registry while the next round starts.
         */
        llam_runtime_destroy(runtime);
        atomic_fetch_add_explicit(&state->successes, 1U, memory_order_relaxed);
    } else if (errno == EBUSY) {
        atomic_fetch_add_explicit(&state->busy_failures, 1U, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&state->unexpected_failures, 1U, memory_order_relaxed);
    }
    return NULL;
}

typedef struct init_shutdown_race_state {
    init_race_barrier_t *barrier;
    atomic_uint init_done;
    int init_rc;
    int init_errno;
} init_shutdown_race_state_t;

typedef struct init_stats_race_state {
    atomic_uint start;
    atomic_uint stop;
    atomic_uint failures;
    atomic_uint snapshots;
} init_stats_race_state_t;

typedef struct trace_race_state {
    llam_shard_t *shard;
    atomic_uint start;
} trace_race_state_t;

typedef struct run_race_state {
    init_race_barrier_t *barrier;
    atomic_uint *attempting;
    atomic_uint *failed;
    int rc;
    int err;
} run_race_state_t;

typedef struct run_hold_state {
    atomic_uint started;
    atomic_uint release;
    atomic_uint *attempting;
    atomic_uint *failed;
} run_hold_state_t;

typedef struct spawn_race_state {
    atomic_uint ready;
    atomic_uint start;
    atomic_uint failures;
} spawn_race_state_t;

typedef struct runtime_env_snapshot {
    uint32_t preempt_poll_period;
    uint64_t preempt_quantum_ns;
    uint64_t task_slab_grows;
    unsigned stack_cache_default_count;
    unsigned channel_safepoint_interval;
    unsigned trace_events_enabled;
    unsigned stack_sampling_enabled;
    unsigned task_list_eager;
    unsigned cheap_safepoint;
} runtime_env_snapshot_t;

#if LLAM_PLATFORM_POSIX
static void test_host_thread_yield(void) {
    /*
     * These race tests use unmanaged pthreads.  A pure busy spin can starve
     * the final participant on small BSD VMs before the LLAM race is reached.
     */
    usleep(100U);
}
#endif

static char *test_dup_env_value(const char *name) {
    const char *value = getenv(name);
    size_t bytes;
    char *copy;

    if (value == NULL) {
        return NULL;
    }
    bytes = strlen(value) + 1U;
    copy = malloc(bytes);
    if (copy != NULL) {
        memcpy(copy, value, bytes);
    }
    return copy;
}

static void test_restore_env_value(const char *name, char *value) {
    if (value != NULL) {
        setenv(name, value, 1);
        free(value);
    } else {
        unsetenv(name);
    }
}

#include "test_runtime_stack_cache_authority.inc"

static int test_runtime_total_prewarm_authority(void) {
    static const char *const names[] = {
        "LLAM_TASK_CACHE_PREWARM",
        "LLAM_STACK_CACHE_PREWARM",
        "LLAM_TIMER_HEAP_PREWARM",
        "LLAM_TASK_CACHE_PREWARM_TOTAL",
        "LLAM_STACK_CACHE_PREWARM_TOTAL",
        "LLAM_TIMER_HEAP_PREWARM_TOTAL",
    };
    char *saved[sizeof(names) / sizeof(names[0])];
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    unsigned *cpus = NULL;
    unsigned worker_count;
    size_t saved_count = 0U;
    int rc = 1;

    memset(saved, 0, sizeof(saved));
    for (size_t i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        saved[i] = test_dup_env_value(names[i]);
        saved_count = i + 1U;
        if (unsetenv(names[i]) != 0) {
            rc = test_fail_errno("clearing prewarm authority environment failed");
            goto cleanup;
        }
    }

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("exact prewarm opts init failed");
        goto cleanup;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 1U;
    opts.blocking_max = 1U;
    opts.task_prewarm_total = 17U;
    opts.stack_prewarm_total = 3U;
    opts.timer_prewarm_total = 7U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0 ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("exact runtime-total prewarm create/stats failed");
        goto cleanup;
    }
    if (stats.requested_task_prewarm_total != 17U ||
        stats.achieved_task_prewarm_total != 17U ||
        stats.requested_stack_prewarm_total != 3U ||
        stats.achieved_stack_prewarm_total != 3U ||
        stats.requested_timer_prewarm_total != 7U ||
        stats.achieved_timer_prewarm_total != 7U ||
        stats.task_prewarm_source != LLAM_RUNTIME_PREWARM_PUBLIC_EXACT ||
        stats.stack_prewarm_source != LLAM_RUNTIME_PREWARM_PUBLIC_EXACT ||
        stats.timer_prewarm_source != LLAM_RUNTIME_PREWARM_PUBLIC_EXACT) {
        rc = test_fail("exact runtime-total prewarm diagnostics were inconsistent");
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;

    if (setenv("LLAM_TASK_CACHE_PREWARM", "11", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "11", 1) != 0 ||
        setenv("LLAM_TIMER_HEAP_PREWARM", "11", 1) != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM_TOTAL", "5", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM_TOTAL", "3", 1) != 0 ||
        setenv("LLAM_TIMER_HEAP_PREWARM_TOTAL", "7", 1) != 0) {
        rc = test_fail_errno("setting prewarm authority environment failed");
        goto cleanup;
    }
    opts.task_prewarm_total = 9U;
    opts.stack_prewarm_total = 0U;
    opts.timer_prewarm_total = 0U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0 ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("mixed prewarm authority create/stats failed");
        goto cleanup;
    }
    if (stats.requested_task_prewarm_total != 9U ||
        stats.achieved_task_prewarm_total != 9U ||
        stats.task_prewarm_source != LLAM_RUNTIME_PREWARM_PUBLIC_EXACT ||
        stats.requested_stack_prewarm_total != 3U ||
        stats.achieved_stack_prewarm_total != 3U ||
        stats.stack_prewarm_source != LLAM_RUNTIME_PREWARM_ENV_TOTAL ||
        stats.requested_timer_prewarm_total != 7U ||
        stats.achieved_timer_prewarm_total != 7U ||
        stats.timer_prewarm_source != LLAM_RUNTIME_PREWARM_ENV_TOTAL) {
        rc = test_fail("public and _TOTAL prewarm authority precedence regressed");
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;

    if (unsetenv("LLAM_TASK_CACHE_PREWARM_TOTAL") != 0 ||
        unsetenv("LLAM_STACK_CACHE_PREWARM_TOTAL") != 0 ||
        unsetenv("LLAM_TIMER_HEAP_PREWARM_TOTAL") != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM", "2", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "3", 1) != 0 ||
        setenv("LLAM_TIMER_HEAP_PREWARM", "4", 1) != 0) {
        rc = test_fail_errno("setting legacy prewarm environment failed");
        goto cleanup;
    }
    worker_count = llam_count_allowed_cpus(&cpus) >= 2U ? 2U : 1U;
    free(cpus);
    cpus = NULL;
    opts.worker_min = worker_count;
    opts.worker_count = worker_count;
    opts.worker_max = worker_count;
    opts.task_prewarm_total = 0U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0 ||
        llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("legacy prewarm authority create/stats failed");
        goto cleanup;
    }
    if (stats.requested_task_prewarm_total != UINT64_C(2) * worker_count ||
        stats.achieved_task_prewarm_total != UINT64_C(2) * worker_count ||
        stats.requested_stack_prewarm_total != 3U ||
        stats.achieved_stack_prewarm_total != 3U ||
        stats.requested_timer_prewarm_total != UINT64_C(4) * worker_count ||
        stats.achieved_timer_prewarm_total != UINT64_C(4) * worker_count ||
        stats.task_prewarm_source != LLAM_RUNTIME_PREWARM_ENV_LEGACY ||
        stats.stack_prewarm_source != LLAM_RUNTIME_PREWARM_ENV_LEGACY ||
        stats.timer_prewarm_source != LLAM_RUNTIME_PREWARM_ENV_LEGACY) {
        rc = test_fail("legacy prewarm inputs lost historical scope or reporting");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    free(cpus);
    for (size_t i = 0U; i < saved_count; ++i) {
        test_restore_env_value(names[i], saved[i]);
    }
    return rc;
}

static int collect_runtime_env_snapshot(runtime_env_snapshot_t *snapshot) {
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;

    if (snapshot == NULL) {
        return test_fail("runtime env snapshot output was NULL");
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("runtime env snapshot opts init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("runtime env snapshot init failed");
    }
    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("runtime env snapshot stats failed");
    }
    snapshot->preempt_poll_period = stats.preempt_poll_period;
    snapshot->preempt_quantum_ns = stats.preempt_quantum_ns;
    snapshot->channel_safepoint_interval = g_llam_runtime.channel_safepoint_interval;
    snapshot->trace_events_enabled = g_llam_runtime.trace_events_enabled;
    snapshot->stack_sampling_enabled = g_llam_runtime.stack_sampling_enabled;
    snapshot->task_list_eager = g_llam_runtime.task_list_eager;
    snapshot->cheap_safepoint = g_llam_runtime.cheap_safepoint;
    snapshot->task_slab_grows = 0U;
    snapshot->stack_cache_default_count = g_llam_runtime.stack_cache_default_count;
    for (unsigned i = 0U; i < g_llam_runtime.active_shards; ++i) {
        snapshot->task_slab_grows += g_llam_runtime.shards[i].allocator.slab_grows;
        snapshot->stack_cache_default_count += g_llam_runtime.shards[i].stack_cache_default_count;
    }
    llam_runtime_shutdown();
    return 0;
}

static void *init_stats_race_stats_thread(void *arg) {
    init_stats_race_state_t *state = arg;

    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0U) {
        llam_runtime_stats_t stats;

        /*
         * Stats are part of the embedding boundary.  A host monitoring thread
         * may call this while another thread is still constructing the
         * singleton; the call must either wait for a stable snapshot or return
         * a valid pre-init snapshot, never walk partially published arrays.
         */
        if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&state->snapshots, 1U, memory_order_relaxed);
        }
        usleep(100);
    }
    return NULL;
}

static void *init_stats_race_dump_thread(void *arg) {
    init_stats_race_state_t *state = arg;
    int fd;

    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    fd = open("/dev/null", O_WRONLY);
    if (fd < 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        return NULL;
    }
    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0U) {
        /*
         * Human/JSON diagnostics are allowed during host-side init monitoring.
         * They must not read default-runtime fields before the runtime has been
         * registered or lifecycle-serialized.
         */
        llam_dump_runtime_state(fd);
        if (llam_runtime_write_stats_json(fd) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            continue;
        }
        atomic_fetch_add_explicit(&state->snapshots, 1U, memory_order_relaxed);
        usleep(100);
    }
    close(fd);
    return NULL;
}

static void *init_stats_race_sleep_thread(void *arg) {
    init_stats_race_state_t *state = arg;

    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    while (atomic_load_explicit(&state->stop, memory_order_acquire) == 0U) {
        errno = 0;
        if (llam_sleep_ns(0U) != 0 && errno != EINVAL) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&state->snapshots, 1U, memory_order_relaxed);
        }
        usleep(100);
    }
    return NULL;
}

static int run_profile_yield_stats(uint32_t profile, llam_runtime_stats_t *stats) {
    core_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *task;

    if (stats == NULL) {
        return test_fail("profile yield stats output was NULL");
    }
    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.ran, 0U);
    atomic_init(&state.blocking_calls, 0U);
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.profile = profile;

    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("runtime init for profile yield stats failed");
    }
    task = llam_spawn(detached_task, &state, NULL);
    if (task == NULL) {
        llam_runtime_shutdown();
        return test_fail_errno("spawn for profile yield stats failed");
    }
    if (llam_run() != 0 || llam_join(task) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("run/join for profile yield stats failed");
    }
    if (atomic_load_explicit(&state.ran, memory_order_relaxed) != 1U) {
        llam_runtime_shutdown();
        return test_fail("profile yield stats task did not run");
    }
    memset(stats, 0, sizeof(*stats));
    if (llam_runtime_collect_stats_ex(stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("collect stats for profile yield stats failed");
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_direct_yield_auto_policy_is_profile_scoped(void) {
    char *saved_stats = test_dup_env_value("LLAM_DIRECT_HANDOFF_STATS");
    char *saved_handoff = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF");
    llam_runtime_stats_t fast_stats;
    llam_runtime_stats_t debug_stats;
    int rc = 1;

    /*
     * The auto direct-yield policy depends on the current runtime profile.
     * Running release-fast first must not cache direct handoff on for a later
     * debug-safe runtime in the same host process.
     */
    if (setenv("LLAM_DIRECT_HANDOFF_STATS", "1", 1) != 0 ||
        unsetenv("LLAM_YIELD_DIRECT_HANDOFF") != 0) {
        rc = test_fail_errno("setenv for direct-yield profile policy failed");
        goto cleanup_env;
    }
    if (run_profile_yield_stats(LLAM_RUNTIME_PROFILE_RELEASE_FAST, &fast_stats) != 0 ||
        run_profile_yield_stats(LLAM_RUNTIME_PROFILE_DEBUG_SAFE, &debug_stats) != 0) {
        goto cleanup_env;
    }
    if (fast_stats.yield_direct_attempts == 0U) {
        rc = test_fail("release-fast runtime did not exercise direct-yield auto policy");
        goto cleanup_env;
    }
    if (debug_stats.yield_direct_attempts != 0U) {
        fprintf(stderr,
                "[test_runtime_core] debug-safe direct-yield attempts leaked from previous profile: %llu\n",
                (unsigned long long)debug_stats.yield_direct_attempts);
        rc = 1;
        goto cleanup_env;
    }
    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_DIRECT_HANDOFF_STATS", saved_stats);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF", saved_handoff);
    return rc;
}

static int test_direct_yield_timer_policy_is_bounded(void) {
#if defined(__APPLE__)
    char *saved_stats = test_dup_env_value("LLAM_DIRECT_HANDOFF_STATS");
    char *saved_handoff = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF");
    char *saved_allow_timers = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS");
    char *saved_burst = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_BURST");
    timer_handoff_state_t state;
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_task_t *sleeper = NULL;
    llam_task_t *first = NULL;
    llam_task_t *second = NULL;
    int rc = 1;

    if (setenv("LLAM_DIRECT_HANDOFF_STATS", "1", 1) != 0 ||
        unsetenv("LLAM_YIELD_DIRECT_HANDOFF") != 0 ||
        unsetenv("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS") != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_BURST", "64", 1) != 0) {
        rc = test_fail_errno("setenv for timer direct-yield policy failed");
        goto cleanup_env;
    }

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    atomic_init(&state.timer_armed, 0U);
    state.yields = 512U;
    state.sleep_ns = 20ULL * 1000ULL * 1000ULL;
    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;

    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("timer direct-yield runtime init failed");
        goto cleanup_env;
    }
    sleeper = llam_spawn(timer_handoff_sleep_task, &state, NULL);
    first = llam_spawn(timer_handoff_yield_task, &state, NULL);
    second = llam_spawn(timer_handoff_yield_task, &state, NULL);
    if (sleeper == NULL || first == NULL || second == NULL) {
        rc = test_fail_errno("timer direct-yield spawn failed");
        goto cleanup_runtime;
    }
    if (llam_run() != 0 ||
        llam_join(sleeper) != 0 ||
        llam_join(first) != 0 ||
        llam_join(second) != 0) {
        rc = test_fail_errno("timer direct-yield run/join failed");
        goto cleanup_runtime;
    }
    sleeper = NULL;
    first = NULL;
    second = NULL;
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.core.ran, memory_order_relaxed) != 2U) {
        rc = test_fail("timer direct-yield tasks did not complete cleanly");
        goto cleanup_runtime;
    }
    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("collect timer direct-yield stats failed");
        goto cleanup_runtime;
    }
    if (stats.yield_direct_attempts == 0U || stats.yield_direct_fast_hits == 0U) {
        fprintf(stderr,
                "[test_runtime_core] Darwin timer direct-yield policy produced no fast hits: attempts=%llu hits=%llu fail_policy=%llu\n",
                (unsigned long long)stats.yield_direct_attempts,
                (unsigned long long)stats.yield_direct_fast_hits,
                (unsigned long long)stats.yield_direct_fail_policy);
        rc = 1;
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (sleeper != NULL) {
        (void)llam_detach(sleeper);
    }
    if (first != NULL) {
        (void)llam_detach(first);
    }
    if (second != NULL) {
        (void)llam_detach(second);
    }
    llam_runtime_shutdown();
cleanup_env:
    test_restore_env_value("LLAM_DIRECT_HANDOFF_STATS", saved_stats);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF", saved_handoff);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS", saved_allow_timers);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_BURST", saved_burst);
    return rc;
#else
    return 0;
#endif
}

static int test_autotune_handoff_budget_actuates(void) {
    char *saved_autotune = test_dup_env_value("LLAM_AUTOTUNE");
    char *saved_domains = test_dup_env_value("LLAM_AUTOTUNE_DOMAINS");
    char *saved_decision_interval = test_dup_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS");
    char *saved_min_hold = test_dup_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS");
    char *saved_wake_p99 = test_dup_env_value("LLAM_AUTOTUNE_WAKE_P99_NS");
    char *saved_sample_period = test_dup_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD");
    char *saved_handoff = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF");
    char *saved_burst = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_BURST");
    char *saved_allow_timers = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS");
    char *saved_live_limit = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT");
    char *saved_trace = test_dup_env_value("LLAM_TRACE_EVENTS");
    char *saved_run_timing = test_dup_env_value("LLAM_RUN_TIMING");
    char *saved_wake_latency = test_dup_env_value("LLAM_WAKE_LATENCY_METRICS");
    char *saved_task_list = test_dup_env_value("LLAM_TASK_LIST_EAGER");
    char *saved_strict = test_dup_env_value("LLAM_STRICT_SAFEPOINT");
    char *saved_light_safepoint = test_dup_env_value("LLAM_DIAG_LIGHT_SAFEPOINT");
    char *saved_stack_sampling = test_dup_env_value("LLAM_STACK_SAMPLING");
    char *saved_profile = test_dup_env_value("LLAM_RUNTIME_PROFILE");
    autotune_handoff_state_t state;
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_task_t *parent = NULL;
    uint64_t decisions;
    uint64_t commits;
    uint64_t min_hold_ns;
    unsigned budget;
    unsigned mode;
    int rc = 1;

    if (setenv("LLAM_AUTOTUNE", "on", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DOMAINS", "handoff", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", "1000000", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_MIN_HOLD_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_WAKE_P99_NS", "0", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_SAMPLE_PERIOD", "1", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF", "2", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_BURST", "1", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS", "1", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT", "0", 1) != 0 ||
        setenv("LLAM_TRACE_EVENTS", "0", 1) != 0 ||
        setenv("LLAM_RUN_TIMING", "0", 1) != 0 ||
        setenv("LLAM_WAKE_LATENCY_METRICS", "0", 1) != 0 ||
        setenv("LLAM_TASK_LIST_EAGER", "0", 1) != 0 ||
        setenv("LLAM_STRICT_SAFEPOINT", "0", 1) != 0 ||
        unsetenv("LLAM_DIAG_LIGHT_SAFEPOINT") != 0 ||
        setenv("LLAM_STACK_SAMPLING", "0", 1) != 0 ||
        setenv("LLAM_RUNTIME_PROFILE", "balanced", 1) != 0) {
        rc = test_fail_errno("setenv for autotune handoff budget failed");
        goto cleanup_env;
    }

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    atomic_init(&state.started, 0U);
    atomic_init(&state.stop, 0U);
    atomic_init(&state.deadline_ns, 0U);
    memset(&opts, 0, sizeof(opts));
    opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;

    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("autotune handoff runtime init failed");
        goto cleanup_env;
    }
    parent = llam_spawn(autotune_handoff_parent_task, &state, NULL);
    if (parent == NULL) {
        rc = test_fail_errno("autotune handoff parent spawn failed");
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || llam_join(parent) != 0) {
        rc = test_fail_errno("autotune handoff run/join failed");
        parent = NULL;
        goto cleanup_runtime;
    }
    parent = NULL;
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.core.ran, memory_order_relaxed) != AUTOTUNE_HANDOFF_WORKERS + 1U) {
        rc = test_fail("autotune handoff workload did not complete cleanly");
        goto cleanup_runtime;
    }
    memset(&stats, 0, sizeof(stats));
    if (llam_runtime_collect_stats_ex(&stats, LLAM_RUNTIME_STATS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("collect autotune handoff stats failed");
        goto cleanup_runtime;
    }
    decisions = atomic_load_explicit(&g_llam_runtime.autotune.decisions, memory_order_acquire);
    commits = atomic_load_explicit(&g_llam_runtime.autotune.commits, memory_order_acquire);
    min_hold_ns = atomic_load_explicit(&g_llam_runtime.autotune.min_hold_ns, memory_order_acquire);
    budget = llam_runtime_direct_handoff_budget(&g_llam_runtime);
    mode = atomic_load_explicit(&g_llam_runtime.autotune.mode, memory_order_acquire);
    if (mode != LLAM_AUTOTUNE_INTERNAL_ON ||
        decisions == 0U ||
        commits == 0U ||
        budget <= 1U ||
        min_hold_ns != 1U ||
        stats.yield_direct_attempts < 32U ||
        stats.yield_direct_fast_hits < 16U) {
        fprintf(stderr,
                "[test_runtime_core] autotune handoff did not actuate: "
                "mode=%u decisions=%llu commits=%llu budget=%u min_hold=%llu "
                "attempts=%llu hits=%llu fail_policy=%llu\n",
                mode,
                (unsigned long long)decisions,
                (unsigned long long)commits,
                budget,
                (unsigned long long)min_hold_ns,
                (unsigned long long)stats.yield_direct_attempts,
                (unsigned long long)stats.yield_direct_fast_hits,
                (unsigned long long)stats.yield_direct_fail_policy);
        rc = 1;
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (parent != NULL) {
        (void)llam_detach(parent);
    }
    atomic_store_explicit(&state.stop, 1U, memory_order_release);
    llam_runtime_shutdown();
cleanup_env:
    test_restore_env_value("LLAM_AUTOTUNE", saved_autotune);
    test_restore_env_value("LLAM_AUTOTUNE_DOMAINS", saved_domains);
    test_restore_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", saved_decision_interval);
    test_restore_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS", saved_min_hold);
    test_restore_env_value("LLAM_AUTOTUNE_WAKE_P99_NS", saved_wake_p99);
    test_restore_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD", saved_sample_period);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF", saved_handoff);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_BURST", saved_burst);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS", saved_allow_timers);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT", saved_live_limit);
    test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
    test_restore_env_value("LLAM_RUN_TIMING", saved_run_timing);
    test_restore_env_value("LLAM_WAKE_LATENCY_METRICS", saved_wake_latency);
    test_restore_env_value("LLAM_TASK_LIST_EAGER", saved_task_list);
    test_restore_env_value("LLAM_STRICT_SAFEPOINT", saved_strict);
    test_restore_env_value("LLAM_DIAG_LIGHT_SAFEPOINT", saved_light_safepoint);
    test_restore_env_value("LLAM_STACK_SAMPLING", saved_stack_sampling);
    test_restore_env_value("LLAM_RUNTIME_PROFILE", saved_profile);
    return rc;
}

static void init_autotune_fake_handoff_metrics(llam_metrics_t *metrics) {
    unsigned bucket;

    memset(metrics, 0, sizeof(*metrics));
    atomic_init(&metrics->yield_direct_attempts, 0U);
    atomic_init(&metrics->yield_direct_fast_hits, 0U);
    atomic_init(&metrics->yield_direct_locked_hits, 0U);
    atomic_init(&metrics->yield_direct_fail_policy, 0U);
    atomic_init(&metrics->yield_direct_fail_budget, 0U);
    atomic_init(&metrics->yield_direct_fail_no_work, 0U);
    atomic_init(&metrics->yield_direct_fail_push, 0U);
    atomic_init(&metrics->wake_handoff_attempts, 0U);
    atomic_init(&metrics->wake_handoff_hits, 0U);
    atomic_init(&metrics->wake_handoff_fail_policy, 0U);
    atomic_init(&metrics->wake_handoff_fail_budget, 0U);
    atomic_init(&metrics->wake_handoff_fail_race, 0U);
    atomic_init(&metrics->autotune_wake_latency_samples, 0U);
    for (bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
        atomic_init(&metrics->autotune_wake_latency_buckets[bucket], 0U);
    }
    atomic_init(&metrics->idle_spin_hits, 0U);
    atomic_init(&metrics->idle_spin_fallbacks, 0U);
    atomic_init(&metrics->idle_spin_ns, 0U);
    atomic_init(&metrics->queue_overflows, 0U);
}

static void add_autotune_fake_yield_metrics(llam_shard_t *shard,
                                            uint64_t attempts,
                                            uint64_t hits,
                                            uint64_t budget_failures) {
    llam_metrics_t *metrics = &shard->metrics;

    atomic_fetch_add_explicit(&metrics->yield_direct_attempts, attempts, memory_order_relaxed);
    atomic_fetch_add_explicit(&metrics->yield_direct_fast_hits, hits, memory_order_relaxed);
    atomic_fetch_add_explicit(&metrics->yield_direct_fail_policy, budget_failures, memory_order_relaxed);
    atomic_fetch_add_explicit(&metrics->yield_direct_fail_budget, budget_failures, memory_order_relaxed);
}

static void add_autotune_fake_yield_no_work_metrics(llam_shard_t *shard,
                                                    uint64_t attempts,
                                                    uint64_t hits,
                                                    uint64_t no_work_failures) {
    llam_metrics_t *metrics = &shard->metrics;

    atomic_fetch_add_explicit(&metrics->yield_direct_attempts, attempts, memory_order_relaxed);
    atomic_fetch_add_explicit(&metrics->yield_direct_fast_hits, hits, memory_order_relaxed);
    atomic_fetch_add_explicit(&metrics->yield_direct_fail_no_work, no_work_failures, memory_order_relaxed);
}

static int test_autotune_handoff_freezes_no_work_low_hit(void) {
    char *saved_autotune = test_dup_env_value("LLAM_AUTOTUNE");
    char *saved_domains = test_dup_env_value("LLAM_AUTOTUNE_DOMAINS");
    char *saved_decision_interval = test_dup_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS");
    char *saved_min_hold = test_dup_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS");
    char *saved_wake_p99 = test_dup_env_value("LLAM_AUTOTUNE_WAKE_P99_NS");
    char *saved_sample_period = test_dup_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD");
    llam_runtime_t rt;
    llam_shard_t shard;
    unsigned budget;
    unsigned phase;
    uint64_t commits;
    uint64_t rollbacks;
    int rc = 1;

    if (setenv("LLAM_AUTOTUNE", "on", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DOMAINS", "handoff", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_MIN_HOLD_NS", "0", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_WAKE_P99_NS", "0", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_SAMPLE_PERIOD", "1", 1) != 0) {
        rc = test_fail_errno("setenv for autotune no-work low-hit probe failed");
        goto cleanup_env;
    }

    memset(&rt, 0, sizeof(rt));
    memset(&shard, 0, sizeof(shard));
    init_autotune_fake_handoff_metrics(&shard.metrics);
    rt.shards = &shard;
    rt.active_shards = 1U;
    rt.direct_handoff_burst = 1U;
    atomic_init(&rt.direct_handoff_budget, 1U);
    shard.runtime = &rt;
    shard.id = 0U;
    llam_autotune_init(&rt);

    llam_watchdog_autotune_tick(&rt, 1U);
    add_autotune_fake_yield_no_work_metrics(&shard, 64U, 32U, 32U);
    llam_watchdog_autotune_tick(&rt, 3U);

    budget = llam_runtime_direct_handoff_budget(&rt);
    phase = atomic_load_explicit(&rt.autotune.phase, memory_order_acquire);
    commits = atomic_load_explicit(&rt.autotune.commits, memory_order_acquire);
    rollbacks = atomic_load_explicit(&rt.autotune.rollbacks, memory_order_acquire);
    if (budget != 1U ||
        phase != LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD ||
        commits != 0U ||
        rollbacks != 0U ||
        rt.autotune.handoff_probe_budget != 0U) {
        fprintf(stderr,
                "[test_runtime_core] autotune no-work low-hit path opened a probe: "
                "budget=%u phase=%u commits=%llu rollbacks=%llu probe_budget=%u\n",
                budget,
                phase,
                (unsigned long long)commits,
                (unsigned long long)rollbacks,
                rt.autotune.handoff_probe_budget);
        goto cleanup_env;
    }

    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_AUTOTUNE", saved_autotune);
    test_restore_env_value("LLAM_AUTOTUNE_DOMAINS", saved_domains);
    test_restore_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", saved_decision_interval);
    test_restore_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS", saved_min_hold);
    test_restore_env_value("LLAM_AUTOTUNE_WAKE_P99_NS", saved_wake_p99);
    test_restore_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD", saved_sample_period);
    return rc;
}

static int test_autotune_handoff_probe_defers_low_sample(void) {
    char *saved_autotune = test_dup_env_value("LLAM_AUTOTUNE");
    char *saved_domains = test_dup_env_value("LLAM_AUTOTUNE_DOMAINS");
    char *saved_decision_interval = test_dup_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS");
    char *saved_min_hold = test_dup_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS");
    char *saved_wake_p99 = test_dup_env_value("LLAM_AUTOTUNE_WAKE_P99_NS");
    char *saved_sample_period = test_dup_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD");
    llam_runtime_t rt;
    llam_shard_t shard;
    unsigned budget;
    unsigned phase;
    uint64_t rollbacks;
    int rc = 1;

    if (setenv("LLAM_AUTOTUNE", "on", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DOMAINS", "handoff", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_MIN_HOLD_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_WAKE_P99_NS", "0", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_SAMPLE_PERIOD", "1", 1) != 0) {
        rc = test_fail_errno("setenv for autotune low-sample probe failed");
        goto cleanup_env;
    }

    memset(&rt, 0, sizeof(rt));
    memset(&shard, 0, sizeof(shard));
    init_autotune_fake_handoff_metrics(&shard.metrics);
    rt.shards = &shard;
    rt.active_shards = 1U;
    rt.direct_handoff_burst = 1U;
    atomic_init(&rt.direct_handoff_budget, 1U);
    shard.runtime = &rt;
    shard.id = 0U;
    llam_autotune_init(&rt);

    llam_watchdog_autotune_tick(&rt, 1U);
    add_autotune_fake_yield_metrics(&shard, 64U, 32U, 32U);
    llam_watchdog_autotune_tick(&rt, 3U);
    budget = llam_runtime_direct_handoff_budget(&rt);
    phase = atomic_load_explicit(&rt.autotune.phase, memory_order_acquire);
    if (budget != 2U || phase != LLAM_AUTOTUNE_INTERNAL_PHASE_PROBE) {
        fprintf(stderr,
                "[test_runtime_core] autotune low-sample probe did not open: budget=%u phase=%u\n",
                budget,
                phase);
        goto cleanup_env;
    }

    add_autotune_fake_yield_metrics(&shard, 10U, 5U, 5U);
    llam_watchdog_autotune_tick(&rt, 5U);
    budget = llam_runtime_direct_handoff_budget(&rt);
    phase = atomic_load_explicit(&rt.autotune.phase, memory_order_acquire);
    rollbacks = atomic_load_explicit(&rt.autotune.rollbacks, memory_order_acquire);
    if (budget != 2U ||
        phase != LLAM_AUTOTUNE_INTERNAL_PHASE_PROBE ||
        rollbacks != 0U ||
        rt.autotune.handoff_probe_attempts != 10U ||
        rt.autotune.handoff_probe_hits != 5U) {
        fprintf(stderr,
                "[test_runtime_core] autotune low-sample probe was not deferred: "
                "budget=%u phase=%u rollbacks=%llu attempts=%llu hits=%llu\n",
                budget,
                phase,
                (unsigned long long)rollbacks,
                (unsigned long long)rt.autotune.handoff_probe_attempts,
                (unsigned long long)rt.autotune.handoff_probe_hits);
        goto cleanup_env;
    }

    add_autotune_fake_yield_metrics(&shard, 22U, 11U, 11U);
    llam_watchdog_autotune_tick(&rt, 7U);
    budget = llam_runtime_direct_handoff_budget(&rt);
    phase = atomic_load_explicit(&rt.autotune.phase, memory_order_acquire);
    rollbacks = atomic_load_explicit(&rt.autotune.rollbacks, memory_order_acquire);
    if (budget != 1U ||
        phase != LLAM_AUTOTUNE_INTERNAL_PHASE_BACKOFF ||
        rollbacks != 1U ||
        rt.autotune.handoff_probe_attempts != 0U ||
        rt.autotune.handoff_probe_hits != 0U) {
        fprintf(stderr,
                "[test_runtime_core] autotune accumulated low-gain probe did not roll back: "
                "budget=%u phase=%u rollbacks=%llu attempts=%llu hits=%llu\n",
                budget,
                phase,
                (unsigned long long)rollbacks,
                (unsigned long long)rt.autotune.handoff_probe_attempts,
                (unsigned long long)rt.autotune.handoff_probe_hits);
        goto cleanup_env;
    }

    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_AUTOTUNE", saved_autotune);
    test_restore_env_value("LLAM_AUTOTUNE_DOMAINS", saved_domains);
    test_restore_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", saved_decision_interval);
    test_restore_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS", saved_min_hold);
    test_restore_env_value("LLAM_AUTOTUNE_WAKE_P99_NS", saved_wake_p99);
    test_restore_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD", saved_sample_period);
    return rc;
}

static int test_autotune_handoff_wake_guardrail_rolls_back(void) {
    char *saved_autotune = test_dup_env_value("LLAM_AUTOTUNE");
    char *saved_domains = test_dup_env_value("LLAM_AUTOTUNE_DOMAINS");
    char *saved_decision_interval = test_dup_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS");
    char *saved_min_hold = test_dup_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS");
    char *saved_wake_p99 = test_dup_env_value("LLAM_AUTOTUNE_WAKE_P99_NS");
    char *saved_sample_period = test_dup_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD");
    char *saved_handoff = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF");
    char *saved_burst = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_BURST");
    char *saved_allow_timers = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS");
    char *saved_live_limit = test_dup_env_value("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT");
    char *saved_trace = test_dup_env_value("LLAM_TRACE_EVENTS");
    char *saved_run_timing = test_dup_env_value("LLAM_RUN_TIMING");
    char *saved_wake_latency = test_dup_env_value("LLAM_WAKE_LATENCY_METRICS");
    char *saved_task_list = test_dup_env_value("LLAM_TASK_LIST_EAGER");
    char *saved_strict = test_dup_env_value("LLAM_STRICT_SAFEPOINT");
    char *saved_light_safepoint = test_dup_env_value("LLAM_DIAG_LIGHT_SAFEPOINT");
    char *saved_stack_sampling = test_dup_env_value("LLAM_STACK_SAMPLING");
    char *saved_profile = test_dup_env_value("LLAM_RUNTIME_PROFILE");
    autotune_handoff_state_t state;
    llam_runtime_opts_t opts;
    llam_task_t *parent = NULL;
    uint64_t decisions;
    uint64_t rollbacks;
    uint64_t guardrail_trips;
    uint64_t sampled_latency_samples;
    uint64_t sampled_latency_p99_ns;
    unsigned budget;
    unsigned mode;
    int rc = 1;

    if (setenv("LLAM_AUTOTUNE", "on", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DOMAINS", "handoff", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", "1000000", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_MIN_HOLD_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_WAKE_P99_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_SAMPLE_PERIOD", "1", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF", "2", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_BURST", "8", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS", "1", 1) != 0 ||
        setenv("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT", "0", 1) != 0 ||
        setenv("LLAM_TRACE_EVENTS", "0", 1) != 0 ||
        setenv("LLAM_RUN_TIMING", "0", 1) != 0 ||
        setenv("LLAM_WAKE_LATENCY_METRICS", "0", 1) != 0 ||
        setenv("LLAM_TASK_LIST_EAGER", "0", 1) != 0 ||
        setenv("LLAM_STRICT_SAFEPOINT", "0", 1) != 0 ||
        unsetenv("LLAM_DIAG_LIGHT_SAFEPOINT") != 0 ||
        setenv("LLAM_STACK_SAMPLING", "0", 1) != 0 ||
        setenv("LLAM_RUNTIME_PROFILE", "balanced", 1) != 0) {
        rc = test_fail_errno("setenv for autotune handoff wake guardrail failed");
        goto cleanup_env;
    }

    memset(&state, 0, sizeof(state));
    atomic_init(&state.core.failures, 0U);
    atomic_init(&state.core.ran, 0U);
    atomic_init(&state.core.blocking_calls, 0U);
    atomic_init(&state.started, 0U);
    atomic_init(&state.stop, 0U);
    atomic_init(&state.deadline_ns, 0U);
    memset(&opts, 0, sizeof(opts));
    opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;

    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("autotune handoff wake guardrail runtime init failed");
        goto cleanup_env;
    }
    parent = llam_spawn(autotune_handoff_parent_task, &state, NULL);
    if (parent == NULL) {
        rc = test_fail_errno("autotune handoff wake guardrail parent spawn failed");
        goto cleanup_runtime;
    }
    if (llam_run() != 0 || llam_join(parent) != 0) {
        rc = test_fail_errno("autotune handoff wake guardrail run/join failed");
        parent = NULL;
        goto cleanup_runtime;
    }
    parent = NULL;
    if (atomic_load_explicit(&state.core.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.core.ran, memory_order_relaxed) != AUTOTUNE_HANDOFF_WORKERS + 1U) {
        rc = test_fail("autotune handoff wake guardrail workload did not complete cleanly");
        goto cleanup_runtime;
    }

    decisions = atomic_load_explicit(&g_llam_runtime.autotune.decisions, memory_order_acquire);
    rollbacks = atomic_load_explicit(&g_llam_runtime.autotune.rollbacks, memory_order_acquire);
    guardrail_trips = atomic_load_explicit(&g_llam_runtime.autotune.guardrail_trips, memory_order_acquire);
    sampled_latency_samples =
        atomic_load_explicit(&g_llam_runtime.autotune.sampled_wake_latency_samples, memory_order_acquire);
    sampled_latency_p99_ns =
        atomic_load_explicit(&g_llam_runtime.autotune.sampled_wake_latency_p99_ns, memory_order_acquire);
    budget = llam_runtime_direct_handoff_budget(&g_llam_runtime);
    mode = atomic_load_explicit(&g_llam_runtime.autotune.mode, memory_order_acquire);
    /*
     * The sampled latency fields describe the most recent decision window,
     * which can be a short trailing window after an earlier guardrail fired.
     * Rollbacks and guardrail trips are cumulative, so they are the stable
     * evidence that this workload crossed the wake-latency threshold.  The
     * live budget is not stable evidence: later decision windows may open a
     * new probe and legitimately raise it again before the workload exits.
     */
    if (mode != LLAM_AUTOTUNE_INTERNAL_ON ||
        decisions == 0U ||
        rollbacks == 0U ||
        guardrail_trips == 0U) {
        fprintf(stderr,
                "[test_runtime_core] autotune wake guardrail did not roll back: "
                "mode=%u decisions=%llu rollbacks=%llu guardrails=%llu "
                "samples=%llu p99=%llu budget=%u\n",
                mode,
                (unsigned long long)decisions,
                (unsigned long long)rollbacks,
                (unsigned long long)guardrail_trips,
                (unsigned long long)sampled_latency_samples,
                (unsigned long long)sampled_latency_p99_ns,
                budget);
        rc = 1;
        goto cleanup_runtime;
    }
    rc = 0;

cleanup_runtime:
    if (parent != NULL) {
        (void)llam_detach(parent);
    }
    atomic_store_explicit(&state.stop, 1U, memory_order_release);
    llam_runtime_shutdown();
cleanup_env:
    test_restore_env_value("LLAM_AUTOTUNE", saved_autotune);
    test_restore_env_value("LLAM_AUTOTUNE_DOMAINS", saved_domains);
    test_restore_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", saved_decision_interval);
    test_restore_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS", saved_min_hold);
    test_restore_env_value("LLAM_AUTOTUNE_WAKE_P99_NS", saved_wake_p99);
    test_restore_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD", saved_sample_period);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF", saved_handoff);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_BURST", saved_burst);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS", saved_allow_timers);
    test_restore_env_value("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT", saved_live_limit);
    test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
    test_restore_env_value("LLAM_RUN_TIMING", saved_run_timing);
    test_restore_env_value("LLAM_WAKE_LATENCY_METRICS", saved_wake_latency);
    test_restore_env_value("LLAM_TASK_LIST_EAGER", saved_task_list);
    test_restore_env_value("LLAM_STRICT_SAFEPOINT", saved_strict);
    test_restore_env_value("LLAM_DIAG_LIGHT_SAFEPOINT", saved_light_safepoint);
    test_restore_env_value("LLAM_STACK_SAMPLING", saved_stack_sampling);
    test_restore_env_value("LLAM_RUNTIME_PROFILE", saved_profile);
    return rc;
}

static int test_unsigned_runtime_env_rejects_malformed_input(void) {
    char *saved_preempt_poll = test_dup_env_value("LLAM_PREEMPT_POLL_PERIOD");
    char *saved_preempt_quantum = test_dup_env_value("LLAM_PREEMPT_QUANTUM_NS");
    char *saved_channel_interval = test_dup_env_value("LLAM_CHANNEL_SAFEPOINT_INTERVAL");
    char *saved_task_prewarm = test_dup_env_value("LLAM_TASK_CACHE_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");
    const char *huge_unsigned = "999999999999999999999999999999999999";
    runtime_env_snapshot_t baseline;
    runtime_env_snapshot_t signed_snapshot;
    int rc = 1;

    /*
     * strtoul/strtoull accept "-1" as a huge unsigned value on many libc
     * implementations and also accept numeric prefixes such as "7x" unless
     * the caller checks the parse tail. Runtime tuning knobs are documented as
     * unsigned decimal values, so malformed text must leave the compiled/profile
     * default intact instead of silently selecting a partial value or max cap.
     */
    if (unsetenv("LLAM_PREEMPT_POLL_PERIOD") != 0 ||
        unsetenv("LLAM_PREEMPT_QUANTUM_NS") != 0 ||
        unsetenv("LLAM_CHANNEL_SAFEPOINT_INTERVAL") != 0 ||
        unsetenv("LLAM_TASK_CACHE_PREWARM") != 0 ||
        unsetenv("LLAM_STACK_CACHE_PREWARM") != 0) {
        rc = test_fail_errno("unsetenv for unsigned runtime env baseline failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&baseline) != 0) {
        goto cleanup_env;
    }

    if (setenv("LLAM_PREEMPT_POLL_PERIOD", "-1", 1) != 0 ||
        setenv("LLAM_PREEMPT_QUANTUM_NS", "-1", 1) != 0 ||
        setenv("LLAM_CHANNEL_SAFEPOINT_INTERVAL", "-1", 1) != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM", "-1", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "-1", 1) != 0) {
        rc = test_fail_errno("setenv for negative unsigned runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&signed_snapshot) != 0) {
        goto cleanup_env;
    }
    if (baseline.preempt_poll_period != signed_snapshot.preempt_poll_period ||
        baseline.preempt_quantum_ns != signed_snapshot.preempt_quantum_ns ||
        baseline.channel_safepoint_interval != signed_snapshot.channel_safepoint_interval ||
        baseline.task_slab_grows != signed_snapshot.task_slab_grows ||
        baseline.stack_cache_default_count != signed_snapshot.stack_cache_default_count) {
        fprintf(stderr,
                "[test_runtime_core] negative unsigned env changed runtime config: "
                "baseline(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u) "
                "signed(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u)\n",
                baseline.preempt_poll_period,
                (unsigned long long)baseline.preempt_quantum_ns,
                baseline.channel_safepoint_interval,
                (unsigned long long)baseline.task_slab_grows,
                baseline.stack_cache_default_count,
                signed_snapshot.preempt_poll_period,
                (unsigned long long)signed_snapshot.preempt_quantum_ns,
                signed_snapshot.channel_safepoint_interval,
                (unsigned long long)signed_snapshot.task_slab_grows,
                signed_snapshot.stack_cache_default_count);
        goto cleanup_env;
    }

    if (setenv("LLAM_PREEMPT_POLL_PERIOD", "+7", 1) != 0 ||
        setenv("LLAM_PREEMPT_QUANTUM_NS", "+7", 1) != 0 ||
        setenv("LLAM_CHANNEL_SAFEPOINT_INTERVAL", "+7", 1) != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM", "+7", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "+7", 1) != 0) {
        rc = test_fail_errno("setenv for signed-plus unsigned runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&signed_snapshot) != 0) {
        goto cleanup_env;
    }
    if (baseline.preempt_poll_period != signed_snapshot.preempt_poll_period ||
        baseline.preempt_quantum_ns != signed_snapshot.preempt_quantum_ns ||
        baseline.channel_safepoint_interval != signed_snapshot.channel_safepoint_interval ||
        baseline.task_slab_grows != signed_snapshot.task_slab_grows ||
        baseline.stack_cache_default_count != signed_snapshot.stack_cache_default_count) {
        fprintf(stderr,
                "[test_runtime_core] signed-plus unsigned env changed runtime config: "
                "baseline(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u) "
                "signed(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u)\n",
                baseline.preempt_poll_period,
                (unsigned long long)baseline.preempt_quantum_ns,
                baseline.channel_safepoint_interval,
                (unsigned long long)baseline.task_slab_grows,
                baseline.stack_cache_default_count,
                signed_snapshot.preempt_poll_period,
                (unsigned long long)signed_snapshot.preempt_quantum_ns,
                signed_snapshot.channel_safepoint_interval,
                (unsigned long long)signed_snapshot.task_slab_grows,
                signed_snapshot.stack_cache_default_count);
        goto cleanup_env;
    }

    if (setenv("LLAM_PREEMPT_POLL_PERIOD", " +7", 1) != 0 ||
        setenv("LLAM_PREEMPT_QUANTUM_NS", " +7", 1) != 0 ||
        setenv("LLAM_CHANNEL_SAFEPOINT_INTERVAL", " +7", 1) != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM", " +7", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", " +7", 1) != 0) {
        rc = test_fail_errno("setenv for whitespace signed-plus unsigned runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&signed_snapshot) != 0) {
        goto cleanup_env;
    }
    if (baseline.preempt_poll_period != signed_snapshot.preempt_poll_period ||
        baseline.preempt_quantum_ns != signed_snapshot.preempt_quantum_ns ||
        baseline.channel_safepoint_interval != signed_snapshot.channel_safepoint_interval ||
        baseline.task_slab_grows != signed_snapshot.task_slab_grows ||
        baseline.stack_cache_default_count != signed_snapshot.stack_cache_default_count) {
        fprintf(stderr,
                "[test_runtime_core] whitespace signed-plus unsigned env changed runtime config: "
                "baseline(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u) "
                "signed(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u)\n",
                baseline.preempt_poll_period,
                (unsigned long long)baseline.preempt_quantum_ns,
                baseline.channel_safepoint_interval,
                (unsigned long long)baseline.task_slab_grows,
                baseline.stack_cache_default_count,
                signed_snapshot.preempt_poll_period,
                (unsigned long long)signed_snapshot.preempt_quantum_ns,
                signed_snapshot.channel_safepoint_interval,
                (unsigned long long)signed_snapshot.task_slab_grows,
                signed_snapshot.stack_cache_default_count);
        goto cleanup_env;
    }

    if (setenv("LLAM_PREEMPT_POLL_PERIOD", "7x", 1) != 0 ||
        setenv("LLAM_PREEMPT_QUANTUM_NS", "7x", 1) != 0 ||
        setenv("LLAM_CHANNEL_SAFEPOINT_INTERVAL", "7x", 1) != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM", "7x", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "7x", 1) != 0) {
        rc = test_fail_errno("setenv for malformed unsigned runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&signed_snapshot) != 0) {
        goto cleanup_env;
    }
    if (baseline.preempt_poll_period != signed_snapshot.preempt_poll_period ||
        baseline.preempt_quantum_ns != signed_snapshot.preempt_quantum_ns ||
        baseline.channel_safepoint_interval != signed_snapshot.channel_safepoint_interval ||
        baseline.task_slab_grows != signed_snapshot.task_slab_grows ||
        baseline.stack_cache_default_count != signed_snapshot.stack_cache_default_count) {
        fprintf(stderr,
                "[test_runtime_core] malformed unsigned env changed runtime config: "
                "baseline(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u) "
                "malformed(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u)\n",
                baseline.preempt_poll_period,
                (unsigned long long)baseline.preempt_quantum_ns,
                baseline.channel_safepoint_interval,
                (unsigned long long)baseline.task_slab_grows,
                baseline.stack_cache_default_count,
                signed_snapshot.preempt_poll_period,
                (unsigned long long)signed_snapshot.preempt_quantum_ns,
                signed_snapshot.channel_safepoint_interval,
                (unsigned long long)signed_snapshot.task_slab_grows,
                signed_snapshot.stack_cache_default_count);
        goto cleanup_env;
    }

    if (setenv("LLAM_PREEMPT_POLL_PERIOD", huge_unsigned, 1) != 0 ||
        setenv("LLAM_PREEMPT_QUANTUM_NS", huge_unsigned, 1) != 0 ||
        setenv("LLAM_CHANNEL_SAFEPOINT_INTERVAL", huge_unsigned, 1) != 0 ||
        setenv("LLAM_TASK_CACHE_PREWARM", huge_unsigned, 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", huge_unsigned, 1) != 0) {
        rc = test_fail_errno("setenv for overflowing unsigned runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&signed_snapshot) != 0) {
        goto cleanup_env;
    }
    if (baseline.preempt_poll_period != signed_snapshot.preempt_poll_period ||
        baseline.preempt_quantum_ns != signed_snapshot.preempt_quantum_ns ||
        baseline.channel_safepoint_interval != signed_snapshot.channel_safepoint_interval ||
        baseline.task_slab_grows != signed_snapshot.task_slab_grows ||
        baseline.stack_cache_default_count != signed_snapshot.stack_cache_default_count) {
        fprintf(stderr,
                "[test_runtime_core] overflowing unsigned env changed runtime config: "
                "baseline(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u) "
                "overflow(period=%u quantum=%llu channel=%u task_slabs=%llu stacks=%u)\n",
                baseline.preempt_poll_period,
                (unsigned long long)baseline.preempt_quantum_ns,
                baseline.channel_safepoint_interval,
                (unsigned long long)baseline.task_slab_grows,
                baseline.stack_cache_default_count,
                signed_snapshot.preempt_poll_period,
                (unsigned long long)signed_snapshot.preempt_quantum_ns,
                signed_snapshot.channel_safepoint_interval,
                (unsigned long long)signed_snapshot.task_slab_grows,
                signed_snapshot.stack_cache_default_count);
        goto cleanup_env;
    }
    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_PREEMPT_POLL_PERIOD", saved_preempt_poll);
    test_restore_env_value("LLAM_PREEMPT_QUANTUM_NS", saved_preempt_quantum);
    test_restore_env_value("LLAM_CHANNEL_SAFEPOINT_INTERVAL", saved_channel_interval);
    test_restore_env_value("LLAM_TASK_CACHE_PREWARM", saved_task_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return rc;
}

static int test_runtime_env_flags_accept_false_tokens(void) {
    char *saved_trace = test_dup_env_value("LLAM_TRACE_EVENTS");
    char *saved_task_list = test_dup_env_value("LLAM_TASK_LIST_EAGER");
    char *saved_light_safepoint = test_dup_env_value("LLAM_DIAG_LIGHT_SAFEPOINT");
    runtime_env_snapshot_t snapshot;
    int rc = 1;

    /*
     * Boolean runtime flags must not use the historical "anything except 0"
     * parser. Operators commonly use false/no/off in shell environments; those
     * tokens must disable the knob rather than silently enabling diagnostics or
     * experimental behavior.
     */
    if (setenv("LLAM_TRACE_EVENTS", "yes", 1) != 0 ||
        setenv("LLAM_TASK_LIST_EAGER", "on", 1) != 0 ||
        setenv("LLAM_DIAG_LIGHT_SAFEPOINT", "true", 1) != 0) {
        rc = test_fail_errno("setenv for true boolean runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&snapshot) != 0) {
        goto cleanup_env;
    }
    if (snapshot.trace_events_enabled == 0U ||
        snapshot.task_list_eager == 0U ||
        snapshot.cheap_safepoint == 0U ||
        snapshot.stack_sampling_enabled != 0U) {
        fprintf(stderr,
                "[test_runtime_core] true boolean env parsed incorrectly: "
                "trace=%u task_list=%u cheap=%u stack_sampling=%u\n",
                snapshot.trace_events_enabled,
                snapshot.task_list_eager,
                snapshot.cheap_safepoint,
                snapshot.stack_sampling_enabled);
        goto cleanup_env;
    }

    if (setenv("LLAM_TRACE_EVENTS", "false", 1) != 0 ||
        setenv("LLAM_TASK_LIST_EAGER", "no", 1) != 0 ||
        setenv("LLAM_DIAG_LIGHT_SAFEPOINT", "off", 1) != 0) {
        rc = test_fail_errno("setenv for false boolean runtime env failed");
        goto cleanup_env;
    }
    if (collect_runtime_env_snapshot(&snapshot) != 0) {
        goto cleanup_env;
    }
    if (snapshot.trace_events_enabled != 0U ||
        snapshot.task_list_eager != 0U ||
        snapshot.cheap_safepoint != 0U ||
        snapshot.stack_sampling_enabled == 0U) {
        fprintf(stderr,
                "[test_runtime_core] false boolean env parsed incorrectly: "
                "trace=%u task_list=%u cheap=%u stack_sampling=%u\n",
                snapshot.trace_events_enabled,
                snapshot.task_list_eager,
                snapshot.cheap_safepoint,
                snapshot.stack_sampling_enabled);
        goto cleanup_env;
    }
    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
    test_restore_env_value("LLAM_TASK_LIST_EAGER", saved_task_list);
    test_restore_env_value("LLAM_DIAG_LIGHT_SAFEPOINT", saved_light_safepoint);
    return rc;
}

static void *init_shutdown_race_init_thread(void *arg) {
    init_shutdown_race_state_t *state = arg;
    llam_runtime_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;
    init_race_barrier_wait(state->barrier);
    errno = 0;
    state->init_rc = llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE);
    state->init_errno = errno;
    atomic_store_explicit(&state->init_done, 1U, memory_order_release);
    return NULL;
}

static void *init_shutdown_race_shutdown_thread(void *arg) {
    init_shutdown_race_state_t *state = arg;

    init_race_barrier_wait(state->barrier);
    while (atomic_load_explicit(&state->init_done, memory_order_acquire) == 0U) {
        llam_runtime_shutdown();
    }
    llam_runtime_shutdown();
    return NULL;
}

static void *trace_race_thread(void *arg) {
    trace_race_state_t *state = arg;

    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
    }
    for (unsigned i = 0U; i < 20000U; ++i) {
        llam_trace_shard(state->shard,
                         NULL,
                         LLAM_TRACE_IDLE,
                         LLAM_TASK_STATE_RUNNABLE,
                         LLAM_TASK_STATE_PARKED,
                         LLAM_WAIT_NONE);
    }
    return NULL;
}

static void *run_race_thread(void *arg) {
    run_race_state_t *state = arg;

    init_race_barrier_wait(state->barrier);
    atomic_fetch_add_explicit(state->attempting, 1U, memory_order_release);
    errno = 0;
    state->rc = llam_run();
    state->err = errno;
    if (state->rc != 0) {
        atomic_fetch_add_explicit(state->failed, 1U, memory_order_release);
    }
    return NULL;
}

static void run_hold_task(void *arg) {
    run_hold_state_t *state = arg;

    atomic_store_explicit(&state->started, 1U, memory_order_release);
    /*
     * Keep the first scheduler driver alive until the contender has actually
     * lost the run-token race.  Counting "about to call llam_run" is not
     * enough on slower BSD schedulers: the winner can drain before the second
     * host thread enters llam_run(), producing two sequential successes instead
     * of a true concurrent-run failure.
     *
     * Use a short timer sleep instead of a pure yield spin.  DragonFlyBSD can
     * otherwise let the watchdog sample a synthetic test-only state where a
     * live task is repeatedly yielding but no external wake source is visible,
     * causing an EDEADLK false positive before the losing llam_run() returns.
     */
    for (unsigned i = 0U;
         i < 10000U && atomic_load_explicit(&state->release, memory_order_acquire) == 0U;
         ++i) {
        if (atomic_load_explicit(state->attempting, memory_order_acquire) >= 2U &&
            atomic_load_explicit(state->failed, memory_order_acquire) >= 1U) {
            break;
        }
        if (llam_sleep_ns(1000000ULL) != 0) {
            break;
        }
    }
}

static void spawn_race_noop_task(void *arg) {
    (void)arg;
}

static void *spawn_race_thread(void *arg) {
    spawn_race_state_t *state = arg;

    atomic_fetch_add_explicit(&state->ready, 1U, memory_order_release);
    while (atomic_load_explicit(&state->start, memory_order_acquire) == 0U) {
        test_host_thread_yield();
    }
    for (unsigned i = 0U; i < 2000U; ++i) {
        llam_task_t *task = llam_spawn(spawn_race_noop_task, NULL, NULL);

        if (task == NULL) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
            continue;
        }
        if (llam_detach(task) != 0) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
    }
    return NULL;
}

static int test_concurrent_runtime_init_contract(void) {
    /*
     * Explicit runtime handles may be constructed concurrently. The lifecycle
     * gate still protects each partially initialized runtime object, but it
     * must not collapse independent handles back into singleton EBUSY behavior.
     */
    for (unsigned round = 0U; round < 128U; ++round) {
        init_race_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U, 0U};
        init_race_state_t state;
        pthread_t first;
        pthread_t second;
        int first_rc;
        int second_rc;

        memset(&state, 0, sizeof(state));
        state.barrier = &barrier;
        atomic_init(&state.successes, 0U);
        atomic_init(&state.busy_failures, 0U);
        atomic_init(&state.unexpected_failures, 0U);
        first_rc = pthread_create(&first, NULL, init_race_thread, &state);
        second_rc = first_rc == 0
                        ? pthread_create(&second, NULL, init_race_thread, &state)
                        : first_rc;
        if (first_rc != 0 || second_rc != 0) {
            if (first_rc == 0) {
                pthread_join(first, NULL);
            }
            errno = first_rc != 0 ? first_rc : second_rc;
            llam_runtime_shutdown();
            return test_fail_errno("pthread_create for concurrent runtime init failed");
        }
        pthread_join(first, NULL);
        pthread_join(second, NULL);
        if (atomic_load_explicit(&state.successes, memory_order_relaxed) != 2U ||
            atomic_load_explicit(&state.busy_failures, memory_order_relaxed) != 0U ||
            atomic_load_explicit(&state.unexpected_failures, memory_order_relaxed) != 0U) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent runtime init race round=%u successes=%u busy=%u unexpected=%u\n",
                    round,
                    atomic_load_explicit(&state.successes, memory_order_relaxed),
                    atomic_load_explicit(&state.busy_failures, memory_order_relaxed),
                    atomic_load_explicit(&state.unexpected_failures, memory_order_relaxed));
            return 1;
        }
    }
    return 0;
}

static int test_concurrent_init_shutdown_contract(void) {
    /*
     * Shutdown is public and idempotent, so a host thread must not be able to
     * tear down partially published initialization storage while another host
     * thread still consumes it.  The prewarm knobs make the init window wide
     * enough to catch the race on unprotected builds.
     */
    char *saved_timer_prewarm = test_dup_env_value("LLAM_TIMER_HEAP_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");

    if (setenv("LLAM_TIMER_HEAP_PREWARM", "4096", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "64", 1) != 0) {
        test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
        test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
        return test_fail_errno("setenv for init/shutdown race failed");
    }

    for (unsigned round = 0U; round < 64U; ++round) {
        init_race_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U, 0U};
        init_shutdown_race_state_t state;
        pthread_t init_thread;
        pthread_t shutdown_thread;
        int init_thread_rc;
        int shutdown_thread_rc;

        memset(&state, 0, sizeof(state));
        state.barrier = &barrier;
        atomic_init(&state.init_done, 0U);
        init_thread_rc = pthread_create(&init_thread, NULL, init_shutdown_race_init_thread, &state);
        shutdown_thread_rc = init_thread_rc == 0
                                 ? pthread_create(&shutdown_thread,
                                                  NULL,
                                                  init_shutdown_race_shutdown_thread,
                                                  &state)
                                 : init_thread_rc;
        if (init_thread_rc != 0 || shutdown_thread_rc != 0) {
            if (init_thread_rc == 0) {
                pthread_join(init_thread, NULL);
            }
            errno = init_thread_rc != 0 ? init_thread_rc : shutdown_thread_rc;
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("pthread_create for init/shutdown race failed");
        }
        pthread_join(init_thread, NULL);
        pthread_join(shutdown_thread, NULL);
        if (state.init_rc != 0 && state.init_errno != EBUSY) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent init/shutdown race round=%u rc=%d errno=%d (%s)\n",
                    round,
                    state.init_rc,
                    state.init_errno,
                    strerror(state.init_errno));
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return 1;
        }
        llam_runtime_shutdown();
    }

    test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return 0;
}

static int test_concurrent_init_stats_contract(void) {
    /*
     * Regression coverage for diagnostics racing the construction window.
     * active_shards is configured before the shard array is allocated; stats
     * must not trust that count unless lifecycle serialization and pointer
     * availability both say the array is safe to walk.
     */
    char *saved_timer_prewarm = test_dup_env_value("LLAM_TIMER_HEAP_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");

    if (setenv("LLAM_TIMER_HEAP_PREWARM", "8192", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "128", 1) != 0) {
        test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
        test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
        return test_fail_errno("setenv for init/stats race failed");
    }

    for (unsigned round = 0U; round < 64U; ++round) {
        init_stats_race_state_t state;
        llam_runtime_opts_t opts;
        pthread_t stats_thread;
        int thread_rc;

        memset(&state, 0, sizeof(state));
        memset(&opts, 0, sizeof(opts));
        atomic_init(&state.start, 0U);
        atomic_init(&state.stop, 0U);
        atomic_init(&state.failures, 0U);
        atomic_init(&state.snapshots, 0U);
        opts.deterministic = 0U;
        opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;

        thread_rc = pthread_create(&stats_thread, NULL, init_stats_race_stats_thread, &state);
        if (thread_rc != 0) {
            errno = thread_rc;
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("pthread_create for init/stats race failed");
        }
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            pthread_join(stats_thread, NULL);
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("llam_runtime_init for init/stats race failed");
        }
        atomic_store_explicit(&state.stop, 1U, memory_order_release);
        pthread_join(stats_thread, NULL);
        if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
            fprintf(stderr,
                    "[test_runtime_core] init/stats race round=%u stats_failures=%u snapshots=%u\n",
                    round,
                    atomic_load_explicit(&state.failures, memory_order_relaxed),
                    atomic_load_explicit(&state.snapshots, memory_order_relaxed));
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return 1;
        }
        llam_runtime_shutdown();
    }

    test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return 0;
}

static int test_concurrent_init_dump_contract(void) {
    char *saved_timer_prewarm = test_dup_env_value("LLAM_TIMER_HEAP_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");

    if (setenv("LLAM_TIMER_HEAP_PREWARM", "8192", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "128", 1) != 0) {
        test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
        test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
        return test_fail_errno("setenv for init/dump race failed");
    }

    for (unsigned round = 0U; round < 64U; ++round) {
        init_stats_race_state_t state;
        llam_runtime_opts_t opts;
        pthread_t dump_thread;
        int thread_rc;

        memset(&state, 0, sizeof(state));
        memset(&opts, 0, sizeof(opts));
        atomic_init(&state.start, 0U);
        atomic_init(&state.stop, 0U);
        atomic_init(&state.failures, 0U);
        atomic_init(&state.snapshots, 0U);
        opts.deterministic = 0U;
        opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;

        thread_rc = pthread_create(&dump_thread, NULL, init_stats_race_dump_thread, &state);
        if (thread_rc != 0) {
            errno = thread_rc;
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("pthread_create for init/dump race failed");
        }
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            pthread_join(dump_thread, NULL);
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("llam_runtime_init for init/dump race failed");
        }
        atomic_store_explicit(&state.stop, 1U, memory_order_release);
        pthread_join(dump_thread, NULL);
        if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
            fprintf(stderr,
                    "[test_runtime_core] init/dump race round=%u dump_failures=%u snapshots=%u\n",
                    round,
                    atomic_load_explicit(&state.failures, memory_order_relaxed),
                    atomic_load_explicit(&state.snapshots, memory_order_relaxed));
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return 1;
        }
        llam_runtime_shutdown();
    }

    test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return 0;
}

static int test_concurrent_init_sleep_contract(void) {
    char *saved_timer_prewarm = test_dup_env_value("LLAM_TIMER_HEAP_PREWARM");
    char *saved_stack_prewarm = test_dup_env_value("LLAM_STACK_CACHE_PREWARM");

    if (setenv("LLAM_TIMER_HEAP_PREWARM", "8192", 1) != 0 ||
        setenv("LLAM_STACK_CACHE_PREWARM", "128", 1) != 0) {
        test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
        test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
        return test_fail_errno("setenv for init/sleep race failed");
    }

    for (unsigned round = 0U; round < 64U; ++round) {
        init_stats_race_state_t state;
        llam_runtime_opts_t opts;
        pthread_t sleep_thread;
        int thread_rc;

        memset(&state, 0, sizeof(state));
        memset(&opts, 0, sizeof(opts));
        atomic_init(&state.start, 0U);
        atomic_init(&state.stop, 0U);
        atomic_init(&state.failures, 0U);
        atomic_init(&state.snapshots, 0U);
        opts.deterministic = 0U;
        opts.profile = LLAM_RUNTIME_PROFILE_BALANCED;

        thread_rc = pthread_create(&sleep_thread, NULL, init_stats_race_sleep_thread, &state);
        if (thread_rc != 0) {
            errno = thread_rc;
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("pthread_create for init/sleep race failed");
        }
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
            atomic_store_explicit(&state.stop, 1U, memory_order_release);
            pthread_join(sleep_thread, NULL);
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return test_fail_errno("llam_runtime_init for init/sleep race failed");
        }
        atomic_store_explicit(&state.stop, 1U, memory_order_release);
        pthread_join(sleep_thread, NULL);
        if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
            fprintf(stderr,
                    "[test_runtime_core] init/sleep race round=%u sleep_failures=%u snapshots=%u\n",
                    round,
                    atomic_load_explicit(&state.failures, memory_order_relaxed),
                    atomic_load_explicit(&state.snapshots, memory_order_relaxed));
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
            test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
            return 1;
        }
        llam_runtime_shutdown();
    }

    test_restore_env_value("LLAM_TIMER_HEAP_PREWARM", saved_timer_prewarm);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM", saved_stack_prewarm);
    return 0;
}

static int test_concurrent_trace_ring_contract(void) {
    /*
     * Trace events can be emitted by peer wake paths, I/O completions,
     * watchdogs, and shard owners.  Force concurrent producers onto one shard
     * so TSan-backed runs catch the diagnostic ring regressing to plain writes.
     */
    enum { TRACE_RACE_THREADS = 4U, TRACE_RACE_ITERS = 20000U };
    char *saved_trace = test_dup_env_value("LLAM_TRACE_EVENTS");
    pthread_t threads[TRACE_RACE_THREADS];
    bool joined[TRACE_RACE_THREADS];
    trace_race_state_t state;
    llam_runtime_opts_t opts;
    unsigned started = 0U;

    memset(joined, 0, sizeof(joined));
    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    atomic_init(&state.start, 0U);
    if (setenv("LLAM_TRACE_EVENTS", "1", 1) != 0) {
        test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
        return test_fail_errno("setenv for trace race failed");
    }
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init(&opts) != 0) {
        test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
        return test_fail_errno("llam_runtime_init for trace race failed");
    }
    state.shard = &g_llam_runtime.shards[0];
    for (started = 0U; started < TRACE_RACE_THREADS; ++started) {
        int rc = pthread_create(&threads[started], NULL, trace_race_thread, &state);

        if (rc != 0) {
            errno = rc;
            atomic_store_explicit(&state.start, 1U, memory_order_release);
            for (unsigned i = 0U; i < started; ++i) {
                if (!joined[i]) {
                    (void)pthread_join(threads[i], NULL);
                    joined[i] = true;
                }
            }
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
            return test_fail_errno("pthread_create for trace race failed");
        }
    }
    atomic_store_explicit(&state.start, 1U, memory_order_release);
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            llam_runtime_shutdown();
            test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
            return test_fail_errno("pthread_join for trace race failed");
        }
        joined[i] = true;
    }
    if (atomic_load_explicit(&state.shard->trace_head, memory_order_acquire) <
        TRACE_RACE_THREADS * TRACE_RACE_ITERS) {
        llam_runtime_shutdown();
        test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
        return test_fail("concurrent trace ring lost reservations");
    }
    llam_runtime_shutdown();
    test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
    return 0;
}
#include "test_optional_shard_storage_cases.inc"
static int test_concurrent_run_contract(void) {
    /*
     * llam_run() is the single scheduler driver for shard zero.  Concurrent
     * unmanaged callers must produce one runner and one EINVAL failure; two
     * successful callers would execute the same shard state at the same time.
     *
     * Keep one initialized runtime across all rounds. This isolates the run
     * token race from rapid backend thread/kqueue teardown churn on BSD VMs,
     * which is covered separately by the lifecycle race tests above.
     */
    llam_runtime_opts_t opts;

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent run failed");
    }
    for (unsigned round = 0U; round < 128U; ++round) {
        init_race_barrier_t barrier = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0U, 0U};
        atomic_uint attempting;
        atomic_uint failed;
        run_hold_state_t hold;
        run_race_state_t first = {&barrier, &attempting, &failed, 123, 0};
        run_race_state_t second = {&barrier, &attempting, &failed, 123, 0};
        pthread_t first_thread;
        pthread_t second_thread;
        int first_rc;
        int second_rc;

        atomic_init(&attempting, 0U);
        atomic_init(&failed, 0U);
        atomic_init(&hold.started, 0U);
        atomic_init(&hold.release, 0U);
        hold.attempting = &attempting;
        hold.failed = &failed;
        if (llam_spawn(run_hold_task, &hold, NULL) == NULL) {
            llam_runtime_shutdown();
            return test_fail_errno("llam_spawn for concurrent run hold task failed");
        }
        first_rc = pthread_create(&first_thread, NULL, run_race_thread, &first);
        second_rc = first_rc == 0
                        ? pthread_create(&second_thread, NULL, run_race_thread, &second)
                        : first_rc;
        if (first_rc != 0 || second_rc != 0) {
            if (first_rc == 0) {
                pthread_join(first_thread, NULL);
            }
            errno = first_rc != 0 ? first_rc : second_rc;
            llam_runtime_shutdown();
            return test_fail_errno("pthread_create for concurrent run failed");
        }
        for (unsigned i = 0U; i < 10000U &&
                              atomic_load_explicit(&hold.started, memory_order_acquire) == 0U;
             ++i) {
            usleep(100);
        }
        for (unsigned i = 0U; i < 10000U &&
                              atomic_load_explicit(&failed, memory_order_acquire) == 0U;
             ++i) {
            usleep(100);
        }
        atomic_store_explicit(&hold.release, 1U, memory_order_release);
        pthread_join(first_thread, NULL);
        pthread_join(second_thread, NULL);
        if (first.rc == 0 && second.rc == 0) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent run double-success round=%u\n",
                    round);
            llam_runtime_shutdown();
            return 1;
        }
        if ((first.rc != 0 && first.err != EINVAL) ||
            (second.rc != 0 && second.err != EINVAL)) {
            fprintf(stderr,
                    "[test_runtime_core] concurrent run unexpected result round=%u first=(%d,%d) second=(%d,%d)\n",
                    round,
                    first.rc,
                    first.err,
                    second.rc,
                    second.err);
            llam_runtime_shutdown();
            return 1;
        }
    }
    llam_runtime_shutdown();
    return 0;
}

static int test_concurrent_spawn_contract(void) {
    /*
     * Unmanaged embedders can submit work from multiple OS threads before a
     * scheduler driver starts.  Keep the placement cursor race-covered so TSan
     * catches regressions in shard selection.
     */
    enum { SPAWN_RACE_THREADS = 8U };
    pthread_t threads[SPAWN_RACE_THREADS];
    spawn_race_state_t state;
    llam_runtime_opts_t opts;
    unsigned started = 0U;

    memset(&state, 0, sizeof(state));
    memset(&opts, 0, sizeof(opts));
    atomic_init(&state.ready, 0U);
    atomic_init(&state.start, 0U);
    atomic_init(&state.failures, 0U);
    opts.deterministic = 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    if (llam_runtime_init(&opts) != 0) {
        return test_fail_errno("llam_runtime_init for concurrent spawn failed");
    }
    for (started = 0U; started < SPAWN_RACE_THREADS; ++started) {
        int rc = pthread_create(&threads[started], NULL, spawn_race_thread, &state);

        if (rc != 0) {
            errno = rc;
            atomic_store_explicit(&state.start, 1U, memory_order_release);
            for (unsigned i = 0U; i < started; ++i) {
                pthread_join(threads[i], NULL);
            }
            llam_runtime_shutdown();
            return test_fail_errno("pthread_create for concurrent spawn failed");
        }
    }
    for (unsigned wait = 0U;
         wait < 100000U &&
         atomic_load_explicit(&state.ready, memory_order_acquire) != SPAWN_RACE_THREADS;
         ++wait) {
        test_host_thread_yield();
    }
    if (atomic_load_explicit(&state.ready, memory_order_acquire) != SPAWN_RACE_THREADS) {
        atomic_store_explicit(&state.start, 1U, memory_order_release);
        for (unsigned i = 0U; i < started; ++i) {
            pthread_join(threads[i], NULL);
        }
        llam_runtime_shutdown();
        return test_fail("concurrent spawn pthreads did not all reach barrier");
    }
    atomic_store_explicit(&state.start, 1U, memory_order_release);
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            llam_runtime_shutdown();
            return test_fail_errno("pthread_join for concurrent spawn failed");
        }
    }
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        llam_runtime_shutdown();
        return test_fail("concurrent unmanaged spawn produced failures");
    }
    if (llam_run() != 0) {
        llam_runtime_shutdown();
        return test_fail_errno("llam_run for concurrent spawn failed");
    }
    llam_runtime_shutdown();
    return 0;
}

typedef struct active_op_release_state {
    _Atomic size_t active_ops;
    unsigned values[2];
} active_op_release_state_t;

typedef struct active_op_release_call {
    active_op_release_state_t *state;
    unsigned index;
} active_op_release_call_t;

static void *active_op_release_thread(void *opaque) {
    active_op_release_call_t *call = opaque;

    call->state->values[call->index] = call->index + 1U;
    llam_public_active_op_end(&call->state->active_ops);
    return NULL;
}

static int test_public_active_op_release_sequence(void) {
    active_op_release_state_t state;
    active_op_release_call_t calls[2];
    pthread_t threads[2];
    unsigned started = 0U;
    bool values_visible;

    memset(&state, 0, sizeof(state));
    llam_public_active_op_init(&state.active_ops);
    if (llam_public_active_op_try_begin(&state.active_ops) != 0 ||
        llam_public_active_op_try_begin(&state.active_ops) != 0) {
        return test_fail_errno("active-op release-sequence setup failed");
    }
    for (started = 0U; started < 2U; ++started) {
        int rc;

        calls[started].state = &state;
        calls[started].index = started;
        rc = pthread_create(&threads[started],
                            NULL,
                            active_op_release_thread,
                            &calls[started]);
        if (rc != 0) {
            errno = rc;
            for (unsigned i = started; i < 2U; ++i) {
                llam_public_active_op_end(&state.active_ops);
            }
            for (unsigned i = 0U; i < started; ++i) {
                (void)pthread_join(threads[i], NULL);
            }
            return test_fail_errno("active-op release-sequence pthread_create failed");
        }
    }
    while (llam_public_active_op_count(&state.active_ops) != 0U) {
        test_host_thread_yield();
    }
    /* This read is ordered only by the release-RMW/acquire-zero protocol. */
    values_visible = state.values[0] == 1U && state.values[1] == 2U;
    for (unsigned i = 0U; i < 2U; ++i) {
        if (pthread_join(threads[i], NULL) != 0) {
            return test_fail_errno("active-op release-sequence pthread_join failed");
        }
    }
    if (!values_visible) {
        return test_fail("active-op acquire-zero missed a protected writer");
    }
    return 0;
}

enum { GROUP_PROMISE_SPAWNS = 33U };

typedef struct group_promise_call {
    llam_task_group_t *group;
    atomic_uint *ready;
    atomic_uint *start;
    llam_task_t *result;
    int result_errno;
} group_promise_call_t;

static void *group_promise_spawn_thread(void *opaque) {
    group_promise_call_t *call = opaque;

    atomic_fetch_add_explicit(call->ready, 1U, memory_order_release);
    while (atomic_load_explicit(call->start, memory_order_acquire) == 0U) {
        test_host_thread_yield();
    }
    errno = 0;
    call->result = llam_task_group_spawn(call->group,
                                         spawn_race_noop_task,
                                         NULL,
                                         NULL);
    call->result_errno = errno;
    return NULL;
}

static void group_promise_lock_allocators(llam_runtime_t *rt) {
    for (unsigned i = 0U; i < rt->active_shards; ++i) {
        pthread_mutex_lock(&rt->shards[i].allocator.lock);
    }
}

static void group_promise_unlock_allocators(llam_runtime_t *rt) {
    for (unsigned i = rt->active_shards; i > 0U; --i) {
        pthread_mutex_unlock(&rt->shards[i - 1U].allocator.lock);
    }
}

static int test_task_group_unique_spawn_reservations(void) {
    pthread_t threads[GROUP_PROMISE_SPAWNS];
    group_promise_call_t calls[GROUP_PROMISE_SPAWNS];
    atomic_uint ready;
    atomic_uint start;
    llam_task_group_t *group = NULL;
    llam_task_group_t *raw_group = NULL;
    llam_runtime_t *rt = NULL;
    unsigned started = 0U;
    bool allocators_locked = false;
    bool spawn_failed = false;
    int result = 1;

    memset(calls, 0, sizeof(calls));
    atomic_init(&ready, 0U);
    atomic_init(&start, 0U);
    if (llam_runtime_init(NULL) != 0) {
        return test_fail_errno("group promise runtime init failed");
    }
    group = llam_task_group_create();
    raw_group = group != NULL ? llam_task_group_resolve_public_handle(group) : NULL;
    if (raw_group == NULL || raw_group->owner_runtime == NULL) {
        (void)llam_task_group_destroy(group);
        llam_runtime_shutdown();
        return test_fail_errno("group promise raw observation setup failed");
    }
    rt = raw_group->owner_runtime;
    group_promise_lock_allocators(rt);
    allocators_locked = true;
    for (started = 0U; started < GROUP_PROMISE_SPAWNS; ++started) {
        int rc;

        calls[started].group = group;
        calls[started].ready = &ready;
        calls[started].start = &start;
        rc = pthread_create(&threads[started],
                            NULL,
                            group_promise_spawn_thread,
                            &calls[started]);
        if (rc != 0) {
            errno = rc;
            goto cleanup;
        }
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) !=
           GROUP_PROMISE_SPAWNS) {
        test_host_thread_yield();
    }
    atomic_store_explicit(&start, 1U, memory_order_release);
    for (unsigned attempt = 0U; attempt < 50000U; ++attempt) {
        size_t active;

        pthread_mutex_lock(&raw_group->lock);
        active = raw_group->active_spawns;
        pthread_mutex_unlock(&raw_group->lock);
        if (active == GROUP_PROMISE_SPAWNS) {
            break;
        }
        if (attempt + 1U == 50000U) {
            goto cleanup;
        }
        test_host_thread_yield();
    }
    pthread_mutex_lock(&raw_group->lock);
    if (raw_group->count != 0U ||
        raw_group->active_spawns != GROUP_PROMISE_SPAWNS ||
        raw_group->capacity < raw_group->count + raw_group->active_spawns) {
        pthread_mutex_unlock(&raw_group->lock);
        goto cleanup;
    }
    pthread_mutex_unlock(&raw_group->lock);
    group_promise_unlock_allocators(rt);
    allocators_locked = false;
    for (unsigned i = 0U; i < started; ++i) {
        if (pthread_join(threads[i], NULL) != 0 || calls[i].result == NULL) {
            spawn_failed = true;
        }
    }
    started = 0U;
    if (spawn_failed) {
        goto cleanup;
    }
    llam_task_group_end_public_op(raw_group);
    raw_group = NULL;
    if (llam_run() != 0 || llam_task_group_join(group) != 0) {
        goto cleanup;
    }
    if (llam_task_group_destroy(group) != 0) {
        goto cleanup;
    }
    group = NULL;
    result = 0;

cleanup:
    atomic_store_explicit(&start, 1U, memory_order_release);
    if (allocators_locked) {
        group_promise_unlock_allocators(rt);
    }
    for (unsigned i = 0U; i < started; ++i) {
        (void)pthread_join(threads[i], NULL);
    }
    if (raw_group != NULL) {
        llam_task_group_end_public_op(raw_group);
    }
    if (group != NULL) {
        (void)llam_run();
        (void)llam_task_group_join(group);
        (void)llam_task_group_destroy(group);
    }
    llam_runtime_shutdown();
    if (result != 0) {
        return test_fail("task-group promised capacity did not cover all active spawns");
    }
    return 0;
}
#endif

typedef struct timer_ownership_contract_state {
    llam_channel_t *channel;
    atomic_uint failures;
} timer_ownership_contract_state_t;

static void timer_ownership_contract_task(void *opaque) {
    timer_ownership_contract_state_t *state = opaque;
    llam_task_t *task = g_llam_tls_task;
    llam_shard_t *shard = g_llam_tls_shard;
    llam_channel_t *channel;
    llam_wait_node_t *node;
    llam_timer_node_t *timer;
    llam_channel_select_state_t select_state;
    uint64_t generation;

    channel = llam_channel_resolve_public_handle(state->channel);
    node = llam_sync_wait_node_acquire(shard);
    if (channel == NULL || node == NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        if (channel != NULL) {
            llam_channel_end_public_op(channel);
        }
        return;
    }
    llam_task_set_wait_node_tracking(task,
                                     node,
                                     &channel->recv_waiters,
                                     &channel->lock,
                                     &channel->active_ops,
                                     shard->id,
                                     LLAM_WAIT_CHANNEL_RECV);
    if (llam_arm_task_wait_deadline(task,
                                    shard,
                                    llam_now_ns() + UINT64_C(60000000000)) != 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    } else {
        bool valid;
        bool removed;
        pthread_mutex_lock(&shard->lock);
        timer = task->active_timer;
        generation = (uint64_t)atomic_load_explicit(&task->wait_generation,
                                                    memory_order_acquire);
        valid = timer != NULL && timer->task == task &&
                timer->wait_generation == generation &&
                timer->wait_lifetime_ops == &channel->active_ops &&
                timer->select_state == NULL && !timer->holds_task_ref &&
                llam_public_active_op_count(&channel->active_ops) == 2U;
        removed = llam_timer_remove_locked(shard, task);
        if (!valid || !removed ||
            llam_public_active_op_count(&channel->active_ops) != 1U) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
        pthread_mutex_unlock(&shard->lock);
    }
    task->state = LLAM_TASK_STATE_RUNNING;
    task->wait_reason = LLAM_WAIT_NONE;
    llam_task_clear_wait_tracking_or_abort(task);
    llam_sync_wait_node_release(shard, node);
    llam_channel_end_public_op(channel);

    memset(&select_state, 0, sizeof(select_state));
    atomic_init(&select_state.completed, LLAM_SELECT_PENDING);
    atomic_init(&select_state.wake_armed, 0U);
    atomic_init(&select_state.wake_queued, 0U);
    atomic_init(&select_state.timer_refs, 0U);
    llam_task_set_select_tracking(task,
                                  &select_state,
                                  shard->id,
                                  LLAM_WAIT_CHANNEL_RECV);
    if (llam_arm_task_wait_deadline(task,
                                    shard,
                                    llam_now_ns() + UINT64_C(60000000000)) != 0) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    } else {
        bool valid;
        bool removed;

        pthread_mutex_lock(&shard->lock);
        timer = task->active_timer;
        valid = timer != NULL && timer->select_state == &select_state &&
                atomic_load_explicit(&select_state.timer_refs,
                                     memory_order_acquire) == 1U;
        removed = llam_timer_remove_locked(shard, task);
        if (!valid || !removed ||
            atomic_load_explicit(&select_state.timer_refs,
                                 memory_order_acquire) != 0U) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
        pthread_mutex_unlock(&shard->lock);
    }
    task->state = LLAM_TASK_STATE_RUNNING;
    task->wait_reason = LLAM_WAIT_NONE;
    llam_task_clear_wait_tracking_or_abort(task);
}

static int test_timer_wait_ownership_contract(void) {
    timer_ownership_contract_state_t state;
    llam_task_t *task;
    int result = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    if (llam_runtime_init(NULL) != 0) {
        return test_fail_errno("timer ownership runtime init failed");
    }
    state.channel = llam_channel_create(1U);
    task = state.channel != NULL
               ? llam_spawn(timer_ownership_contract_task, &state, NULL)
               : NULL;
    if (task == NULL || llam_run() != 0 || llam_join(task) != 0 ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        result = 1;
    }
    if (state.channel != NULL && llam_channel_destroy(state.channel) != 0) {
        result = 1;
    }
    llam_runtime_shutdown();
    return result != 0
               ? test_fail_errno("timer ownership/generation contract failed")
               : 0;
}

static int test_select_completion_ownership_contract(void) {
    llam_task_t task;
    llam_wait_node_t node;
    llam_wait_node_t queued_node;
    llam_channel_select_state_t state;
    llam_channel_select_state_t queued_state;

    memset(&task, 0, sizeof(task));
    memset(&node, 0, sizeof(node));
    memset(&state, 0, sizeof(state));
    memset(&queued_node, 0, sizeof(queued_node));
    memset(&queued_state, 0, sizeof(queued_state));
    atomic_init(&task.state, LLAM_TASK_STATE_PARKED);
    atomic_init(&state.completed, LLAM_SELECT_PENDING);
    atomic_init(&state.wake_armed, 0U);
    atomic_init(&state.wake_queued, 0U);
    node.task = &task;
    node.select_state = &state;
    if (llam_channel_select_complete_node(&node, NULL, 0) !=
            LLAM_SELECT_COMPLETION_INLINE ||
        atomic_load_explicit(&state.completed, memory_order_acquire) !=
            LLAM_SELECT_COMPLETED_INLINE ||
        llam_channel_select_arm_wait(&state) != LLAM_SELECT_COMPLETED_INLINE ||
        llam_channel_select_complete_node(&node, NULL, 0) !=
            LLAM_SELECT_COMPLETION_LOST) {
        return test_fail("select completion-before-arm ownership was ambiguous");
    }
    atomic_init(&queued_state.completed, LLAM_SELECT_PENDING);
    atomic_init(&queued_state.wake_armed, 0U);
    atomic_init(&queued_state.wake_queued, 0U);
    queued_node.task = &task;
    queued_node.select_state = &queued_state;
    if (llam_channel_select_arm_wait(&queued_state) != LLAM_SELECT_ARMED ||
        atomic_load_explicit(&queued_state.wake_armed, memory_order_acquire) != 1U ||
        llam_channel_select_complete_node(&queued_node, NULL, 0) !=
            LLAM_SELECT_COMPLETION_QUEUED ||
        atomic_load_explicit(&queued_state.completed, memory_order_acquire) !=
            LLAM_SELECT_COMPLETED_QUEUED ||
        atomic_load_explicit(&queued_state.wake_queued, memory_order_acquire) !=
            1U) {
        return test_fail("select arm-before-completion lost its required wake");
    }
    return 0;
}

#if LLAM_PLATFORM_POSIX
#define WAIT_RESOLVER_TEST_SPINS 20000U

typedef struct wait_resolver_close_call {
    llam_task_t *task;
    atomic_uint done;
    bool result;
} wait_resolver_close_call_t;

typedef struct wait_resolver_cleanup_call {
    llam_task_t *task;
    llam_wait_node_t *node;
    atomic_uint done;
} wait_resolver_cleanup_call_t;

typedef struct wait_resolver_cancel_call {
    llam_task_t *task;
    atomic_uint done;
} wait_resolver_cancel_call_t;

typedef struct wait_resolver_block_release_call {
    llam_runtime_t *runtime;
    llam_block_job_t *job;
    atomic_uint done;
} wait_resolver_block_release_call_t;

static void wait_resolver_test_task_init(llam_task_t *task, llam_runtime_t *rt) {
    memset(task, 0, sizeof(*task));
    task->owner_runtime = rt;
    atomic_init(&task->active_ops, 0U);
    atomic_init(&task->state, LLAM_TASK_STATE_NEW);
    atomic_init(&task->wait_reason, LLAM_WAIT_NONE);
    atomic_init(&task->last_shard, 0U);
    atomic_init(&task->scan_refs, 0U);
    atomic_init(&task->join_waiter_hint, 0U);
    atomic_init(&task->join_target, NULL);
    atomic_init(&task->active_wait_node, NULL);
    atomic_init(&task->active_wait_queue, NULL);
    atomic_init(&task->active_wait_queue_lock, NULL);
    atomic_init(&task->active_select_state, NULL);
    atomic_init(&task->active_wait_lifetime_ops, NULL);
    atomic_init(&task->wait_resolver_state, LLAM_WAIT_RESOLVER_CLOSED_BIT);
    atomic_init(&task->wait_generation, 0U);
    atomic_init(&task->active_io_req, NULL);
    atomic_init(&task->active_io_generation, 0U);
    atomic_init(&task->active_block_job, NULL);
    atomic_init(&task->wake_error_code, 0);
}

static void wait_resolver_test_node_init(llam_wait_node_t *node,
                                         llam_runtime_t *rt,
                                         llam_task_t *task) {
    memset(node, 0, sizeof(*node));
    node->owner_runtime = rt;
    node->task = task;
    atomic_init(&node->wake_armed, 0U);
    atomic_init(&node->wake_completed, 0U);
    atomic_init(&node->wake_queued, 0U);
}

static bool wait_resolver_test_uint_reaches(atomic_uint *value,
                                            unsigned mask,
                                            unsigned expected) {
    const struct timespec interval = {.tv_sec = 0, .tv_nsec = 100000L};
    unsigned i;

    for (i = 0U; i < WAIT_RESOLVER_TEST_SPINS; ++i) {
        if ((atomic_load_explicit(value, memory_order_acquire) & mask) == expected) {
            return true;
        }
        (void)nanosleep(&interval, NULL);
    }
    return false;
}

static void *wait_resolver_close_main(void *opaque) {
    wait_resolver_close_call_t *call = opaque;

    call->result = llam_task_close_wait_resolvers(call->task);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void *wait_resolver_cleanup_main(void *opaque) {
    wait_resolver_cleanup_call_t *call = opaque;

    llam_task_clear_wait_tracking_or_abort(call->task);
    atomic_store_explicit(&call->task->state,
                          LLAM_TASK_STATE_RUNNING,
                          memory_order_release);
    atomic_store_explicit(&call->task->wait_reason,
                          LLAM_WAIT_NONE,
                          memory_order_release);
    if (call->node != NULL) {
        llam_wait_node_reset(call->node, call->task->owner_runtime, UINT_MAX);
    }
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void *wait_resolver_cancel_main(void *opaque) {
    wait_resolver_cancel_call_t *call = opaque;

    llam_cancel_task_wait(call->task);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void *wait_resolver_block_release_main(void *opaque) {
    wait_resolver_block_release_call_t *call = opaque;

    llam_block_job_release(call->runtime, call->job);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static int test_wait_resolver_gate_contract(void) {
    llam_task_t task;
    wait_resolver_close_call_t close_call;
    pthread_t closer;

    wait_resolver_test_task_init(&task, NULL);
    if (llam_task_wait_resolver_try_begin(&task) ||
        !llam_task_publish_wait_tracking(&task) ||
        !llam_task_wait_resolver_try_begin(&task) ||
        !llam_task_wait_resolver_try_begin(&task)) {
        return test_fail_errno("wait resolver gate did not publish/claim cleanly");
    }
    memset(&close_call, 0, sizeof(close_call));
    close_call.task = &task;
    atomic_init(&close_call.done, 0U);
    if (pthread_create(&closer, NULL, wait_resolver_close_main, &close_call) != 0) {
        llam_task_wait_resolver_end(&task);
        llam_task_wait_resolver_end(&task);
        return test_fail("wait resolver close thread creation failed");
    }
    if (!wait_resolver_test_uint_reaches(&task.wait_resolver_state,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT) ||
        atomic_load_explicit(&close_call.done, memory_order_acquire) != 0U ||
        llam_task_wait_resolver_try_begin(&task)) {
        llam_task_wait_resolver_end(&task);
        llam_task_wait_resolver_end(&task);
        (void)pthread_join(closer, NULL);
        return test_fail("closed wait resolver gate admitted or failed to drain claims");
    }
    llam_task_wait_resolver_end(&task);
    if (atomic_load_explicit(&close_call.done, memory_order_acquire) != 0U) {
        llam_task_wait_resolver_end(&task);
        (void)pthread_join(closer, NULL);
        return test_fail("wait resolver close returned before its final claim drained");
    }
    llam_task_wait_resolver_end(&task);
    if (pthread_join(closer, NULL) != 0 || !close_call.result ||
        atomic_load_explicit(&task.wait_resolver_state, memory_order_acquire) !=
            LLAM_WAIT_RESOLVER_CLOSED_BIT) {
        return test_fail("wait resolver close did not reach quiescence");
    }

    atomic_store_explicit(&task.wait_resolver_state, UINT_MAX, memory_order_release);
    errno = 0;
    if (llam_task_close_wait_resolvers(&task) || errno != EOVERFLOW) {
        return test_fail("saturated wait resolver gate did not fail closed");
    }
    atomic_store_explicit(&task.wait_resolver_state,
                          LLAM_WAIT_RESOLVER_CLOSED_BIT,
                          memory_order_release);
    return 0;
}

static bool wait_tracking_owner_snapshot_matches(const llam_task_t *task,
                                                 const llam_wait_node_t *node,
                                                 const llam_wait_queue_t *queue,
                                                 pthread_mutex_t *queue_lock,
                                                 const llam_channel_select_state_t *select_state,
                                                 const llam_block_job_t *job,
                                                 const llam_task_t *join_target,
                                                 unsigned state,
                                                 llam_wait_reason_t reason) {
    return atomic_load_explicit(&((llam_task_t *)task)->active_wait_node,
                                memory_order_acquire) == node &&
           atomic_load_explicit(&((llam_task_t *)task)->active_wait_queue,
                                memory_order_acquire) == queue &&
           atomic_load_explicit(&((llam_task_t *)task)->active_wait_queue_lock,
                                memory_order_acquire) == queue_lock &&
           atomic_load_explicit(&((llam_task_t *)task)->active_select_state,
                                memory_order_acquire) == select_state &&
           atomic_load_explicit(&((llam_task_t *)task)->active_block_job,
                                memory_order_acquire) == job &&
           atomic_load_explicit(&((llam_task_t *)task)->join_target,
                                memory_order_acquire) == join_target &&
           atomic_load_explicit(&((llam_task_t *)task)->state,
                                memory_order_acquire) == state &&
           atomic_load_explicit(&((llam_task_t *)task)->wait_reason,
                                memory_order_acquire) == (unsigned)reason;
}

static int test_wait_tracking_setters_fail_closed_on_saturation(void) {
    llam_task_t task;
    llam_task_t join_target;
    llam_wait_node_t old_node;
    llam_wait_node_t new_node;
    llam_wait_queue_t old_queue = {0};
    llam_wait_queue_t new_queue = {0};
    llam_channel_select_state_t old_select;
    llam_channel_select_state_t new_select;
    llam_block_job_t old_job;
    llam_block_job_t new_job;
    llam_io_req_t req;
    pthread_mutex_t old_lock;
    pthread_mutex_t new_lock;
    uint64_t generation;

    memset(&join_target, 0, sizeof(join_target));
    memset(&old_select, 0, sizeof(old_select));
    memset(&new_select, 0, sizeof(new_select));
    memset(&old_job, 0, sizeof(old_job));
    memset(&new_job, 0, sizeof(new_job));
    memset(&req, 0, sizeof(req));
    if (pthread_mutex_init(&old_lock, NULL) != 0 ||
        pthread_mutex_init(&new_lock, NULL) != 0) {
        return test_fail("wait tracking saturation lock initialization failed");
    }
    wait_resolver_test_task_init(&task, NULL);
    wait_resolver_test_node_init(&old_node, NULL, &task);
    wait_resolver_test_node_init(&new_node, NULL, &task);
    atomic_init(&req.operation_generation, 9U);
    atomic_store_explicit(&task.active_wait_node, &old_node, memory_order_release);
    atomic_store_explicit(&task.active_wait_queue, &old_queue, memory_order_release);
    atomic_store_explicit(&task.active_wait_queue_lock, &old_lock, memory_order_release);
    atomic_store_explicit(&task.active_select_state, &old_select, memory_order_release);
    atomic_store_explicit(&task.active_block_job, &old_job, memory_order_release);
    atomic_store_explicit(&task.join_target, &join_target, memory_order_release);
    atomic_store_explicit(&task.state, LLAM_TASK_STATE_RUNNING, memory_order_release);
    atomic_store_explicit(&task.wait_reason, LLAM_WAIT_JOIN, memory_order_release);
    atomic_store_explicit(&task.wait_generation, 41U, memory_order_release);
    generation = atomic_load_explicit(&task.wait_generation, memory_order_acquire);
    atomic_store_explicit(&task.wait_resolver_state, UINT_MAX, memory_order_release);

#define EXPECT_SATURATED_SETTER_FAILURE(expr)                                                     \
    do {                                                                                          \
        errno = 0;                                                                                \
        if ((expr) || errno != EOVERFLOW ||                                                       \
            !wait_tracking_owner_snapshot_matches(&task,                                         \
                                                  &old_node,                                      \
                                                  &old_queue,                                     \
                                                  &old_lock,                                      \
                                                  &old_select,                                    \
                                                  &old_job,                                       \
                                                  &join_target,                                   \
                                                  LLAM_TASK_STATE_RUNNING,                        \
                                                  LLAM_WAIT_JOIN) ||                              \
            atomic_load_explicit(&task.wait_generation, memory_order_acquire) != generation) {   \
            (void)pthread_mutex_destroy(&new_lock);                                               \
            (void)pthread_mutex_destroy(&old_lock);                                               \
            return test_fail("saturated wait setter overwrote its prior owner");                 \
        }                                                                                         \
    } while (0)

    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_wait_node_tracking(&task,
                                                                     &new_node,
                                                                     &new_queue,
                                                                     &new_lock,
                                                                     NULL,
                                                                     3U,
                                                                     LLAM_WAIT_MUTEX));
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_select_tracking(&task,
                                                                  &new_select,
                                                                  3U,
                                                                  LLAM_WAIT_CHANNEL_RECV));
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_join_tracking(&task, &task, 3U));
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_sleep_tracking(&task, &new_node, 3U));
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_block_tracking(&task, &new_job, 3U));
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_io_tracking(&task, &req, 3U));
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_clear_wait_tracking(&task));

    atomic_store_explicit(&task.wait_resolver_state,
                          LLAM_WAIT_RESOLVER_CLOSED_BIT,
                          memory_order_release);
    atomic_store_explicit(&task.wait_generation, UINT64_MAX, memory_order_release);
    generation = UINT64_MAX;
    EXPECT_SATURATED_SETTER_FAILURE(llam_task_set_sleep_tracking(&task, &new_node, 3U));
#undef EXPECT_SATURATED_SETTER_FAILURE

    (void)pthread_mutex_destroy(&new_lock);
    (void)pthread_mutex_destroy(&old_lock);
    return 0;
}

static int test_wait_tracking_lock_safe_overflow(void) {
    llam_runtime_t rt;
    llam_shard_t shard;
    llam_task_t task;
    atomic_uint counter;
    pid_t child;
    int status;

    memset(&rt, 0, sizeof(rt));
    memset(&shard, 0, sizeof(shard));
    atomic_init(&rt.initialized, 1U);
    atomic_init(&rt.fatal_errno, 0);
    atomic_init(&rt.deferred_fatal_pending, 0U);
    rt.active_shards = 1U;
    rt.shards = &shard;
    shard.runtime = &rt;
    if (pthread_mutex_init(&shard.lock, NULL) != 0) {
        return test_fail("wait overflow shard lock initialization failed");
    }
    wait_resolver_test_task_init(&task, &rt);
    child = fork();
    if (child < 0) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail_errno("wait overflow fork failed");
    }
    if (child == 0) {
        atomic_store_explicit(&task.wait_resolver_state,
                              LLAM_WAIT_RESOLVER_REF_MASK - 1U,
                              memory_order_release);
        pthread_mutex_lock(&shard.lock);
        (void)llam_task_wait_resolver_try_begin(&task);
        _exit(2);
    }
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGABRT) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("open wait resolver saturation did not hard-fail lock-safely");
    }

    child = fork();
    if (child < 0) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail_errno("closed wait overflow fork failed");
    }
    if (child == 0) {
        atomic_store_explicit(&task.wait_resolver_state, UINT_MAX, memory_order_release);
        pthread_mutex_lock(&shard.lock);
        (void)llam_task_wait_resolver_try_begin(&task);
        _exit(2);
    }
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGABRT) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("closed wait resolver saturation did not hard-fail lock-safely");
    }

    child = fork();
    if (child < 0) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail_errno("wait underflow fork failed");
    }
    if (child == 0) {
        atomic_store_explicit(&task.wait_resolver_state,
                              LLAM_WAIT_RESOLVER_CLOSED_BIT,
                              memory_order_release);
        pthread_mutex_lock(&shard.lock);
        llam_task_wait_resolver_end(&task);
        _exit(2);
    }
    if (waitpid(child, &status, 0) != child || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGABRT) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("wait resolver underflow did not hard-fail lock-safely");
    }

    atomic_store_explicit(&task.wait_resolver_state, UINT_MAX, memory_order_release);
    pthread_mutex_lock(&shard.lock);
    errno = 0;
    if (llam_task_close_wait_resolvers(&task) || errno != EOVERFLOW) {
        pthread_mutex_unlock(&shard.lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("wait resolver close overflow did not return under owner lock");
    }
    pthread_mutex_unlock(&shard.lock);
    if (atomic_load_explicit(&rt.fatal_errno, memory_order_acquire) != EOVERFLOW) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("wait resolver overflow was not recorded lock-safely");
    }

    atomic_init(&counter, UINT_MAX);
    atomic_store_explicit(&rt.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&rt.deferred_fatal_pending, 0U, memory_order_release);
    pthread_mutex_lock(&shard.lock);
    errno = 0;
    if (llam_sync_note_inflight_waiter(&rt, &counter, 1U) ||
        errno != EOVERFLOW) {
        pthread_mutex_unlock(&shard.lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("inflight waiter overflow did not return under owner lock");
    }
    pthread_mutex_unlock(&shard.lock);
    if (atomic_load_explicit(&counter, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&rt.fatal_errno, memory_order_acquire) != EOVERFLOW ||
        atomic_load_explicit(&rt.deferred_fatal_pending, memory_order_acquire) == 0U) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("inflight waiter overflow was not deferred lock-safely");
    }

    atomic_store_explicit(&counter, 0U, memory_order_release);
    atomic_store_explicit(&rt.fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&rt.deferred_fatal_pending, 0U, memory_order_release);
    pthread_mutex_lock(&shard.lock);
    errno = 0;
    if (llam_sync_complete_inflight_waiter(&rt, &counter, 1U) || errno != EINVAL) {
        pthread_mutex_unlock(&shard.lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("inflight waiter underflow did not return under owner lock");
    }
    pthread_mutex_unlock(&shard.lock);
    if (atomic_load_explicit(&counter, memory_order_acquire) != 0U ||
        atomic_load_explicit(&rt.fatal_errno, memory_order_acquire) != EINVAL ||
        atomic_load_explicit(&rt.deferred_fatal_pending, memory_order_acquire) == 0U) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("inflight waiter underflow was not deferred lock-safely");
    }
    (void)pthread_mutex_destroy(&shard.lock);
    return 0;
}

typedef struct saturated_channel_wait_state {
    llam_channel_t *channel;
    atomic_uint failures;
} saturated_channel_wait_state_t;

static void saturated_channel_wait_task(void *opaque) {
    saturated_channel_wait_state_t *state = opaque;
    llam_task_t *task = g_llam_tls_task;
    llam_channel_t *channel;
    void *value = NULL;

    atomic_store_explicit(&task->wait_resolver_state, UINT_MAX, memory_order_release);
    errno = 0;
    if (llam_channel_recv_result(state->channel, &value) == 0 || errno != EOVERFLOW ||
        atomic_load_explicit(&task->state, memory_order_acquire) != LLAM_TASK_STATE_RUNNING ||
        atomic_load_explicit(&task->wait_reason, memory_order_acquire) != LLAM_WAIT_NONE ||
        atomic_load_explicit(&task->active_wait_node, memory_order_acquire) != NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
    channel = llam_channel_resolve_public_handle(state->channel);
    if (channel == NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    } else {
        pthread_mutex_lock(&channel->lock);
        if (channel->recv_waiters.head != NULL || channel->recv_waiters.tail != NULL) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
        }
        pthread_mutex_unlock(&channel->lock);
        llam_channel_end_public_op(channel);
    }
    atomic_store_explicit(&task->wait_resolver_state,
                          LLAM_WAIT_RESOLVER_CLOSED_BIT,
                          memory_order_release);
    atomic_store_explicit(&task->owner_runtime->fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&task->owner_runtime->deferred_fatal_pending,
                          0U,
                          memory_order_release);
}

static int test_wait_tracking_caller_rolls_back_saturation(void) {
    saturated_channel_wait_state_t state;
    llam_task_t *task;
    int result = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    if (llam_runtime_init(NULL) != 0) {
        return test_fail_errno("wait saturation caller runtime init failed");
    }
    state.channel = llam_channel_create(1U);
    task = state.channel != NULL
               ? llam_spawn(saturated_channel_wait_task, &state, NULL)
               : NULL;
    if (task == NULL || llam_run() != 0 || llam_join(task) != 0 ||
        atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U) {
        result = 1;
    }
    if (state.channel != NULL && llam_channel_destroy(state.channel) != 0) {
        result = 1;
    }
    llam_runtime_shutdown();
    return result != 0
               ? test_fail_errno("saturated wait caller failed to unlink its queue node")
               : 0;
}

static int test_active_io_tracking_counter_saturation_rolls_back(void) {
    llam_runtime_t rt;
    llam_task_t task;
    llam_io_req_t req;

    memset(&rt, 0, sizeof(rt));
    memset(&req, 0, sizeof(req));
    atomic_init(&rt.initialized, 1U);
    atomic_init(&rt.active_io_waiters, UINT_MAX);
    atomic_init(&rt.fatal_errno, 0);
    atomic_init(&rt.deferred_fatal_pending, 0U);
    wait_resolver_test_task_init(&task, &rt);
    atomic_init(&req.operation_generation, 11U);
    atomic_store_explicit(&task.state, LLAM_TASK_STATE_RUNNING, memory_order_release);
    errno = 0;
    if (llam_task_set_io_tracking(&task, &req, 0U) || errno != EOVERFLOW ||
        atomic_load_explicit(&rt.active_io_waiters, memory_order_acquire) != UINT_MAX ||
        atomic_load_explicit(&rt.fatal_errno, memory_order_acquire) != EOVERFLOW ||
        atomic_load_explicit(&task.active_io_req, memory_order_acquire) != NULL ||
        atomic_load_explicit(&task.active_io_generation, memory_order_acquire) != 0U ||
        atomic_load_explicit(&task.state, memory_order_acquire) != LLAM_TASK_STATE_RUNNING ||
        atomic_load_explicit(&task.wait_reason, memory_order_acquire) != LLAM_WAIT_NONE ||
        atomic_load_explicit(&task.wait_resolver_state, memory_order_acquire) !=
            LLAM_WAIT_RESOLVER_CLOSED_BIT) {
        return test_fail("active I/O tracking counter saturation was not rolled back");
    }
    return 0;
}

typedef struct deferred_fatal_wait_state {
    llam_channel_t *wait_channel;
    llam_channel_t *trigger_channel;
    _Atomic(llam_task_t *) raw_waiter;
    atomic_uint helper_failed;
    atomic_uint fallback_stop;
} deferred_fatal_wait_state_t;

static void deferred_fatal_parked_task(void *opaque) {
    deferred_fatal_wait_state_t *state = opaque;
    void *value = NULL;

    atomic_store_explicit(&state->raw_waiter, g_llam_tls_task, memory_order_release);
    if (llam_channel_recv_result(state->wait_channel, &value) == 0 || errno != ECANCELED) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
    }
}

static void deferred_fatal_trigger_task(void *opaque) {
    deferred_fatal_wait_state_t *state = opaque;
    llam_task_t *waiter = NULL;
    void *value = NULL;
    unsigned i;

    for (i = 0U; i < WAIT_RESOLVER_TEST_SPINS; ++i) {
        waiter = atomic_load_explicit(&state->raw_waiter, memory_order_acquire);
        if (waiter != NULL &&
            atomic_load_explicit(&waiter->state, memory_order_acquire) ==
                LLAM_TASK_STATE_PARKED &&
            atomic_load_explicit(&waiter->active_wait_node, memory_order_acquire) != NULL) {
            break;
        }
        (void)llam_yield();
    }
    if (waiter == NULL || i == WAIT_RESOLVER_TEST_SPINS) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
        return;
    }

    atomic_store_explicit(&g_llam_tls_task->wait_generation,
                          UINT64_MAX,
                          memory_order_release);
    errno = 0;
    if (llam_channel_recv_result(state->trigger_channel, &value) == 0 ||
        errno != EOVERFLOW ||
        atomic_load_explicit(&g_llam_tls_task->state, memory_order_acquire) !=
            LLAM_TASK_STATE_RUNNING ||
        atomic_load_explicit(&g_llam_tls_task->active_wait_node,
                             memory_order_acquire) != NULL) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
    }
}

static void *deferred_fatal_monitor_main(void *opaque) {
    deferred_fatal_wait_state_t *state = opaque;
    llam_runtime_t *rt = llam_runtime_default_storage();
    unsigned i;

    for (i = 0U; i < WAIT_RESOLVER_TEST_SPINS; ++i) {
        if (atomic_load_explicit(&rt->fatal_errno, memory_order_acquire) == EOVERFLOW) {
            break;
        }
        test_host_thread_yield();
    }
    if (i == WAIT_RESOLVER_TEST_SPINS) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
        llam_request_stop(rt);
        return NULL;
    }
    for (i = 0U; i < WAIT_RESOLVER_TEST_SPINS; ++i) {
        if (atomic_load_explicit(&rt->stop_requested, memory_order_acquire)) {
            return NULL;
        }
        test_host_thread_yield();
    }

    /* Bound a regression: the test must never leave its scheduler hung. */
    atomic_store_explicit(&state->fallback_stop, 1U, memory_order_release);
    llam_request_stop(rt);
    return NULL;
}

static int test_deferred_fatal_stops_and_drains_parked_wait(void) {
    deferred_fatal_wait_state_t state;
    llam_runtime_t *rt;
    llam_task_t *waiter;
    llam_task_t *trigger;
    pthread_t monitor;
    int run_rc;
    int run_errno;
    int result = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.raw_waiter, NULL);
    atomic_init(&state.helper_failed, 0U);
    atomic_init(&state.fallback_stop, 0U);
    if (llam_runtime_init(NULL) != 0) {
        return test_fail_errno("deferred fatal runtime init failed");
    }
    rt = llam_runtime_default_storage();
    state.wait_channel = llam_channel_create(1U);
    state.trigger_channel = llam_channel_create(1U);
    waiter = state.wait_channel != NULL && state.trigger_channel != NULL
               ? llam_spawn(deferred_fatal_parked_task, &state, NULL)
               : NULL;
    trigger = waiter != NULL
                  ? llam_spawn(deferred_fatal_trigger_task, &state, NULL)
                  : NULL;
    if (trigger == NULL ||
        pthread_create(&monitor, NULL, deferred_fatal_monitor_main, &state) != 0) {
        if (waiter != NULL) {
            llam_request_stop(rt);
        }
        if (state.wait_channel != NULL) {
            (void)llam_channel_destroy(state.wait_channel);
        }
        if (state.trigger_channel != NULL) {
            (void)llam_channel_destroy(state.trigger_channel);
        }
        llam_runtime_shutdown();
        return test_fail("deferred fatal drain setup failed");
    }

    errno = 0;
    run_rc = llam_run();
    run_errno = errno;
    if (pthread_join(monitor, NULL) != 0 || run_rc == 0 || run_errno != EOVERFLOW ||
        atomic_load_explicit(&state.fallback_stop, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.helper_failed, memory_order_acquire) != 0U ||
        atomic_load_explicit(&rt->live_tasks, memory_order_acquire) != 0U) {
        result = 1;
    }
    /* The fatal runtime is terminal to public calls; clear only to reclaim test handles. */
    atomic_store_explicit(&rt->fatal_errno, 0, memory_order_release);
    {
        int waiter_join = llam_join(waiter);
        int trigger_join = llam_join(trigger);
        unsigned live = atomic_load_explicit(&rt->live_tasks, memory_order_acquire);

        if (waiter_join != 0 || trigger_join != 0 || live != 0U) {
            result = 1;
        }
    }
    if (state.wait_channel != NULL && llam_channel_destroy(state.wait_channel) != 0) {
        result = 1;
    }
    if (state.trigger_channel != NULL && llam_channel_destroy(state.trigger_channel) != 0) {
        result = 1;
    }
    llam_runtime_shutdown();
    return result != 0
               ? test_fail_errno("deferred fatal did not stop and drain a parked wait")
               : 0;
}

typedef struct prestopped_deferred_fatal_state {
    llam_channel_t *channel;
    _Atomic(llam_task_t *) raw_waiter;
    atomic_uint helper_failed;
    atomic_uint fallback_wake;
} prestopped_deferred_fatal_state_t;

static void prestopped_deferred_fatal_waiter(void *opaque) {
    prestopped_deferred_fatal_state_t *state = opaque;
    void *value = NULL;

    atomic_store_explicit(&state->raw_waiter, g_llam_tls_task, memory_order_release);
    if (llam_channel_recv_result(state->channel, &value) == 0 || errno != ECANCELED) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
    }
}

static void *prestopped_deferred_fatal_main(void *opaque) {
    prestopped_deferred_fatal_state_t *state = opaque;
    llam_runtime_t *rt = llam_runtime_default_storage();
    llam_task_t *waiter = NULL;
    unsigned i;

    for (i = 0U; i < WAIT_RESOLVER_TEST_SPINS; ++i) {
        waiter = atomic_load_explicit(&state->raw_waiter, memory_order_acquire);
        if (waiter != NULL &&
            atomic_load_explicit(&waiter->state, memory_order_acquire) ==
                LLAM_TASK_STATE_PARKED &&
            atomic_load_explicit(&waiter->active_wait_node, memory_order_acquire) != NULL) {
            break;
        }
        test_host_thread_yield();
    }
    if (waiter == NULL || i == WAIT_RESOLVER_TEST_SPINS) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
        llam_request_stop(rt);
        return NULL;
    }

    /* Model a stop flag that was published without the wake side effects. */
    atomic_store_explicit(&rt->stop_requested, true, memory_order_release);
    atomic_store_explicit(&rt->active_io_waiters, UINT_MAX, memory_order_release);
    if (llam_runtime_note_active_io_waiter(rt, 1)) {
        atomic_store_explicit(&state->helper_failed, 1U, memory_order_release);
    }
    for (i = 0U; i < WAIT_RESOLVER_TEST_SPINS; ++i) {
        if (atomic_load_explicit(&waiter->state, memory_order_acquire) !=
                LLAM_TASK_STATE_PARKED ||
            atomic_load_explicit(&waiter->active_wait_node, memory_order_acquire) == NULL) {
            return NULL;
        }
        test_host_thread_yield();
    }

    atomic_store_explicit(&state->fallback_wake, 1U, memory_order_release);
    llam_request_stop(rt);
    return NULL;
}

static int test_deferred_fatal_rewakes_prestopped_runtime(void) {
    prestopped_deferred_fatal_state_t state;
    llam_runtime_t *rt;
    llam_task_t *waiter;
    pthread_t injector;
    int run_rc;
    int run_errno;
    int result = 0;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.raw_waiter, NULL);
    atomic_init(&state.helper_failed, 0U);
    atomic_init(&state.fallback_wake, 0U);
    if (llam_runtime_init(NULL) != 0) {
        return test_fail_errno("pre-stopped deferred fatal runtime init failed");
    }
    rt = llam_runtime_default_storage();
    state.channel = llam_channel_create(1U);
    waiter = state.channel != NULL
                 ? llam_spawn(prestopped_deferred_fatal_waiter, &state, NULL)
                 : NULL;
    if (waiter == NULL ||
        pthread_create(&injector, NULL, prestopped_deferred_fatal_main, &state) != 0) {
        if (waiter != NULL) {
            llam_request_stop(rt);
        }
        if (state.channel != NULL) {
            (void)llam_channel_destroy(state.channel);
        }
        llam_runtime_shutdown();
        return test_fail("pre-stopped deferred fatal setup failed");
    }

    errno = 0;
    run_rc = llam_run();
    run_errno = errno;
    if (pthread_join(injector, NULL) != 0 || run_rc == 0 || run_errno != EOVERFLOW ||
        atomic_load_explicit(&state.helper_failed, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.fallback_wake, memory_order_acquire) != 0U ||
        atomic_load_explicit(&rt->live_tasks, memory_order_acquire) != 0U) {
        result = 1;
    }
    atomic_store_explicit(&rt->fatal_errno, 0, memory_order_release);
    atomic_store_explicit(&rt->active_io_waiters, 0U, memory_order_release);
    if (llam_join(waiter) != 0) {
        result = 1;
    }
    if (state.channel != NULL && llam_channel_destroy(state.channel) != 0) {
        result = 1;
    }
    llam_runtime_shutdown();
    return result != 0
               ? test_fail_errno("deferred fatal did not re-wake a pre-stopped runtime")
               : 0;
}

static int test_wait_resolver_scalar_cancel_drain(void) {
    llam_runtime_t rt;
    llam_shard_t shard;
    llam_task_t task;
    llam_wait_node_t node;
    llam_wait_queue_t queue = {0};
    pthread_mutex_t queue_lock;
    wait_resolver_cancel_call_t cancel_call;
    wait_resolver_cleanup_call_t cleanup_call;
    pthread_t canceller;
    pthread_t cleanup;
    uint64_t generation;
    bool removed;

    memset(&rt, 0, sizeof(rt));
    memset(&shard, 0, sizeof(shard));
    atomic_init(&rt.initialized, 0U);
    rt.active_shards = 1U;
    rt.shards = &shard;
    shard.runtime = &rt;
    shard.id = 0U;
    if (pthread_mutex_init(&shard.lock, NULL) != 0) {
        return test_fail("scalar resolver lock initialization failed");
    }
    if (pthread_mutex_init(&queue_lock, NULL) != 0) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("scalar resolver lock initialization failed");
    }
    wait_resolver_test_task_init(&task, &rt);
    wait_resolver_test_node_init(&node, &rt, &task);
    llam_wait_queue_push_tail(&queue, &node);
    llam_task_set_wait_node_tracking(&task,
                                     &node,
                                     &queue,
                                     &queue_lock,
                                     NULL,
                                     0U,
                                     LLAM_WAIT_MUTEX);
    generation = atomic_load_explicit(&task.wait_generation, memory_order_acquire);

    memset(&cancel_call, 0, sizeof(cancel_call));
    cancel_call.task = &task;
    atomic_init(&cancel_call.done, 0U);
    memset(&cleanup_call, 0, sizeof(cleanup_call));
    cleanup_call.task = &task;
    cleanup_call.node = &node;
    atomic_init(&cleanup_call.done, 0U);

    pthread_mutex_lock(&queue_lock);
    if (pthread_create(&canceller, NULL, wait_resolver_cancel_main, &cancel_call) != 0) {
        pthread_mutex_unlock(&queue_lock);
        llam_task_clear_wait_tracking_or_abort(&task);
        llam_wait_node_reset(&node, &rt, UINT_MAX);
        (void)pthread_mutex_destroy(&queue_lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("scalar resolver race setup failed");
    }
    if (!wait_resolver_test_uint_reaches(&task.wait_resolver_state,
                                         LLAM_WAIT_RESOLVER_REF_MASK,
                                         1U)) {
        (void)llam_wait_queue_remove(&queue, &node);
        atomic_store_explicit(&task.state, LLAM_TASK_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&task.wait_reason, LLAM_WAIT_NONE, memory_order_release);
        pthread_mutex_unlock(&queue_lock);
        (void)pthread_join(canceller, NULL);
        llam_task_clear_wait_tracking_or_abort(&task);
        llam_wait_node_reset(&node, &rt, UINT_MAX);
        (void)pthread_mutex_destroy(&queue_lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("scalar cancellation resolver did not claim the wait");
    }
    if (pthread_create(&cleanup, NULL, wait_resolver_cleanup_main, &cleanup_call) != 0) {
        (void)llam_wait_queue_remove(&queue, &node);
        atomic_store_explicit(&task.state, LLAM_TASK_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&task.wait_reason, LLAM_WAIT_NONE, memory_order_release);
        pthread_mutex_unlock(&queue_lock);
        (void)pthread_join(canceller, NULL);
        llam_task_clear_wait_tracking_or_abort(&task);
        llam_wait_node_reset(&node, &rt, UINT_MAX);
        (void)pthread_mutex_destroy(&queue_lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("scalar cleanup thread creation failed");
    }
    if (!wait_resolver_test_uint_reaches(&task.wait_resolver_state,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT) ||
        atomic_load_explicit(&cleanup_call.done, memory_order_acquire) != 0U ||
        atomic_load_explicit(&task.active_wait_node, memory_order_acquire) != &node ||
        atomic_load_explicit(&task.wait_generation, memory_order_acquire) != generation) {
        pthread_mutex_unlock(&queue_lock);
        (void)pthread_join(canceller, NULL);
        (void)pthread_join(cleanup, NULL);
        return test_fail("scalar wait owner was cleared before resolver quiescence");
    }
    removed = llam_wait_queue_remove(&queue, &node);
    pthread_mutex_unlock(&queue_lock);
    if (pthread_join(canceller, NULL) != 0 || pthread_join(cleanup, NULL) != 0 ||
        !removed || atomic_load_explicit(&cancel_call.done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&cleanup_call.done, memory_order_acquire) == 0U ||
        atomic_load_explicit(&task.active_wait_node, memory_order_acquire) != NULL ||
        atomic_load_explicit(&task.wait_resolver_state, memory_order_acquire) !=
            LLAM_WAIT_RESOLVER_CLOSED_BIT ||
        queue.head != NULL || node.task != NULL) {
        return test_fail("scalar cancellation resolver outlived recycled wait state");
    }
    (void)pthread_mutex_destroy(&queue_lock);
    (void)pthread_mutex_destroy(&shard.lock);
    return 0;
}

static int test_wait_resolver_select_cancel_drain(void) {
    llam_runtime_t rt;
    llam_shard_t shard;
    llam_task_t task;
    llam_channel_t channel;
    llam_wait_node_t node;
    llam_channel_select_state_t state;
    llam_select_op_t op;
    llam_wait_node_t *nodes[1] = {&node};
    llam_channel_t *channels[1] = {&channel};
    llam_channel_t *op_channels[1] = {&channel};
    wait_resolver_cancel_call_t cancel_call;
    wait_resolver_cleanup_call_t cleanup_call;
    pthread_t canceller;
    pthread_t cleanup;

    memset(&rt, 0, sizeof(rt));
    memset(&shard, 0, sizeof(shard));
    memset(&channel, 0, sizeof(channel));
    memset(&state, 0, sizeof(state));
    memset(&op, 0, sizeof(op));
    atomic_init(&rt.initialized, 0U);
    rt.active_shards = 1U;
    rt.shards = &shard;
    shard.runtime = &rt;
    shard.id = 0U;
    channel.owner_runtime = &rt;
    if (pthread_mutex_init(&shard.lock, NULL) != 0) {
        return test_fail("select resolver lock initialization failed");
    }
    if (pthread_mutex_init(&channel.lock, NULL) != 0) {
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("select resolver lock initialization failed");
    }
    wait_resolver_test_task_init(&task, &rt);
    wait_resolver_test_node_init(&node, &rt, &task);
    op.kind = LLAM_SELECT_OP_RECV;
    node.select_state = &state;
    state.owner_runtime = &rt;
    state.ops = &op;
    state.nodes = nodes;
    state.channels = channels;
    state.op_channels = op_channels;
    state.op_count = 1U;
    state.channel_count = 1U;
    state.selected_index = SIZE_MAX;
    atomic_init(&state.completed, LLAM_SELECT_PENDING);
    atomic_init(&state.wake_armed, 0U);
    atomic_init(&state.wake_queued, 0U);
    atomic_init(&state.timer_refs, 0U);
    llam_wait_queue_push_tail(&channel.recv_waiters, &node);
    llam_task_set_select_tracking(&task, &state, 0U, LLAM_WAIT_CHANNEL_RECV);

    memset(&cancel_call, 0, sizeof(cancel_call));
    cancel_call.task = &task;
    atomic_init(&cancel_call.done, 0U);
    memset(&cleanup_call, 0, sizeof(cleanup_call));
    cleanup_call.task = &task;
    cleanup_call.node = &node;
    atomic_init(&cleanup_call.done, 0U);

    pthread_mutex_lock(&channel.lock);
    if (pthread_create(&canceller, NULL, wait_resolver_cancel_main, &cancel_call) != 0) {
        (void)llam_wait_queue_remove(&channel.recv_waiters, &node);
        pthread_mutex_unlock(&channel.lock);
        llam_task_clear_wait_tracking_or_abort(&task);
        llam_wait_node_reset(&node, &rt, UINT_MAX);
        (void)pthread_mutex_destroy(&channel.lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("select resolver race setup failed");
    }
    if (!wait_resolver_test_uint_reaches(&state.completed,
                                         UINT_MAX,
                                         LLAM_SELECT_COMPLETING)) {
        (void)llam_wait_queue_remove(&channel.recv_waiters, &node);
        atomic_store_explicit(&task.state, LLAM_TASK_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&task.wait_reason, LLAM_WAIT_NONE, memory_order_release);
        pthread_mutex_unlock(&channel.lock);
        (void)pthread_join(canceller, NULL);
        llam_task_clear_wait_tracking_or_abort(&task);
        llam_wait_node_reset(&node, &rt, UINT_MAX);
        (void)pthread_mutex_destroy(&channel.lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("select cancellation resolver did not enter cleanup");
    }
    if (pthread_create(&cleanup, NULL, wait_resolver_cleanup_main, &cleanup_call) != 0) {
        (void)llam_wait_queue_remove(&channel.recv_waiters, &node);
        atomic_store_explicit(&task.state, LLAM_TASK_STATE_RUNNING, memory_order_release);
        atomic_store_explicit(&task.wait_reason, LLAM_WAIT_NONE, memory_order_release);
        pthread_mutex_unlock(&channel.lock);
        (void)pthread_join(canceller, NULL);
        llam_task_clear_wait_tracking_or_abort(&task);
        llam_wait_node_reset(&node, &rt, UINT_MAX);
        (void)pthread_mutex_destroy(&channel.lock);
        (void)pthread_mutex_destroy(&shard.lock);
        return test_fail("select cleanup thread creation failed");
    }
    if (!wait_resolver_test_uint_reaches(&task.wait_resolver_state,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT) ||
        atomic_load_explicit(&cleanup_call.done, memory_order_acquire) != 0U ||
        atomic_load_explicit(&task.active_select_state, memory_order_acquire) != &state ||
        node.select_state != &state) {
        pthread_mutex_unlock(&channel.lock);
        (void)pthread_join(canceller, NULL);
        (void)pthread_join(cleanup, NULL);
        return test_fail("select state was cleared before resolver quiescence");
    }
    pthread_mutex_unlock(&channel.lock);
    if (pthread_join(canceller, NULL) != 0 || pthread_join(cleanup, NULL) != 0 ||
        atomic_load_explicit(&state.completed, memory_order_acquire) !=
            LLAM_SELECT_COMPLETED_INLINE ||
        state.error_code != ECANCELED || channel.recv_waiters.head != NULL ||
        atomic_load_explicit(&task.active_select_state, memory_order_acquire) != NULL ||
        atomic_load_explicit(&task.wait_resolver_state, memory_order_acquire) !=
            LLAM_WAIT_RESOLVER_CLOSED_BIT ||
        node.select_state != NULL) {
        return test_fail("select cancellation resolver outlived stack wait state");
    }
    (void)pthread_mutex_destroy(&channel.lock);
    (void)pthread_mutex_destroy(&shard.lock);
    return 0;
}

static int test_wait_resolver_block_job_recycle_drain(void) {
    llam_runtime_t rt;
    llam_task_t task;
    llam_wait_node_t node;
    llam_block_job_t job;
    llam_block_job_t *reused;
    wait_resolver_block_release_call_t release_call;
    pthread_t releaser;

    memset(&rt, 0, sizeof(rt));
    memset(&job, 0, sizeof(job));
    atomic_init(&rt.initialized, 0U);
    atomic_init(&rt.block_job_free, NULL);
    if (pthread_mutex_init(&rt.block_lock, NULL) != 0) {
        return test_fail("block resolver lock initialization failed");
    }
    wait_resolver_test_task_init(&task, &rt);
    wait_resolver_test_node_init(&node, &rt, &task);
    atomic_store_explicit(&task.scan_refs, 1U, memory_order_release);
    atomic_init(&job.result, NULL);
    atomic_init(&job.error_code, 0);
    atomic_init(&job.state, LLAM_BLOCK_JOB_QUEUED);
    job.task = &task;
    job.wait_node = &node;
    job.holds_task_ref = true;
    llam_task_set_block_tracking(&task, &job, 0U);
    if (!llam_task_wait_resolver_try_begin(&task)) {
        return test_fail_errno("block resolver claim failed");
    }

    memset(&release_call, 0, sizeof(release_call));
    release_call.runtime = &rt;
    release_call.job = &job;
    atomic_init(&release_call.done, 0U);
    if (pthread_create(&releaser, NULL, wait_resolver_block_release_main, &release_call) != 0) {
        llam_task_wait_resolver_end(&task);
        return test_fail("block release thread creation failed");
    }
    if (!wait_resolver_test_uint_reaches(&task.wait_resolver_state,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT,
                                         LLAM_WAIT_RESOLVER_CLOSED_BIT) ||
        llam_task_active_block_job_load(&task) != NULL ||
        atomic_load_explicit(&release_call.done, memory_order_acquire) != 0U ||
        job.task != &task || job.wait_node != &node ||
        atomic_load_explicit(&rt.block_job_free, memory_order_acquire) != NULL) {
        llam_task_wait_resolver_end(&task);
        (void)pthread_join(releaser, NULL);
        return test_fail("blocking job recycled before resolver quiescence");
    }
    llam_task_wait_resolver_end(&task);
    if (pthread_join(releaser, NULL) != 0 ||
        atomic_load_explicit(&release_call.done, memory_order_acquire) == 0U ||
        job.task != NULL || job.wait_node != NULL || job.holds_task_ref ||
        atomic_load_explicit(&task.scan_refs, memory_order_acquire) != 0U ||
        atomic_load_explicit(&rt.block_job_free, memory_order_acquire) != &job) {
        return test_fail("blocking job was not recycled after resolver drain");
    }
    reused = llam_block_job_alloc(&rt);
    if (reused != &job || reused->task != NULL || reused->wait_node != NULL) {
        return test_fail("blocking job pool did not safely reuse the drained slot");
    }
    llam_block_job_release(&rt, reused);
    (void)pthread_mutex_destroy(&rt.block_lock);
    return 0;
}
#endif

#include "test_task_context_cases.inc"
#define SWITCH_HOOK_TEST_FAIL(message) test_fail(message)
#define SWITCH_HOOK_TEST_FAIL_ERRNO(message) test_fail_errno(message)
#include "test_switch_hook_cases.inc"
#undef SWITCH_HOOK_TEST_FAIL_ERRNO
#undef SWITCH_HOOK_TEST_FAIL
#include "test_autotune_domain_cases.inc"

int main(void) {
    RUN_RUNTIME_CORE_TEST(test_task_context_unmanaged_contract);
    RUN_RUNTIME_CORE_TEST(test_task_context_lifecycle_and_prefix);
    RUN_RUNTIME_CORE_TEST(test_task_context_survives_forced_queue_migration);
    RUN_RUNTIME_CORE_TEST(exercise_switch_hook_cases);
    RUN_RUNTIME_CORE_TEST(test_preinit_contracts);
    RUN_RUNTIME_CORE_TEST(test_runtime_registered_init_failure_rolls_back);
    RUN_RUNTIME_CORE_TEST(test_legacy_runtime_init_ignores_resource_tail);
    RUN_RUNTIME_CORE_TEST(test_runtime_resource_plan_resolver);
    RUN_RUNTIME_CORE_TEST(test_runtime_total_prewarm_distribution);
    RUN_RUNTIME_CORE_TEST(test_runtime_resource_plan_initialization);
    RUN_RUNTIME_CORE_TEST(test_blocking_pool_grows_lazily_within_bounds);
    RUN_RUNTIME_CORE_TEST(test_native_thread_diagnostics_are_live_counts);
    RUN_RUNTIME_CORE_TEST(test_runtime_create_preserves_managed_tls);
#if LLAM_PLATFORM_POSIX
    RUN_RUNTIME_CORE_TEST(test_direct_yield_auto_policy_is_profile_scoped);
    RUN_RUNTIME_CORE_TEST(test_direct_yield_timer_policy_is_bounded);
    RUN_RUNTIME_CORE_TEST(test_autotune_handoff_budget_actuates);
    RUN_RUNTIME_CORE_TEST(test_autotune_handoff_freezes_no_work_low_hit);
    RUN_RUNTIME_CORE_TEST(test_autotune_handoff_probe_defers_low_sample);
    RUN_RUNTIME_CORE_TEST(test_autotune_handoff_wake_guardrail_rolls_back);
    RUN_RUNTIME_CORE_TEST(test_autotune_domain_capabilities);
    RUN_RUNTIME_CORE_TEST(test_autotune_handoff_min_hold);
    RUN_RUNTIME_CORE_TEST(test_runtime_total_prewarm_authority);
    RUN_RUNTIME_CORE_TEST(test_stack_cache_runtime_byte_authority);
    RUN_RUNTIME_CORE_TEST(test_unsigned_runtime_env_rejects_malformed_input);
    RUN_RUNTIME_CORE_TEST(test_runtime_env_flags_accept_false_tokens);
#endif
    RUN_RUNTIME_CORE_TEST(test_runtime_handle_api);
    RUN_RUNTIME_CORE_TEST(test_runtime_lifecycle_and_task_contracts);
    RUN_RUNTIME_CORE_TEST(test_request_stop_returns_success);
    RUN_RUNTIME_CORE_TEST(test_shutdown_from_task_requests_stop);
    RUN_RUNTIME_CORE_TEST(test_runtime_owner_mismatch_diagnostics);
    RUN_RUNTIME_CORE_TEST(test_timer_wait_ownership_contract);
    RUN_RUNTIME_CORE_TEST(test_select_completion_ownership_contract);
#if LLAM_PLATFORM_POSIX
    RUN_RUNTIME_CORE_TEST(test_wait_resolver_gate_contract);
    RUN_RUNTIME_CORE_TEST(test_wait_tracking_setters_fail_closed_on_saturation);
    RUN_RUNTIME_CORE_TEST(test_wait_tracking_lock_safe_overflow);
    RUN_RUNTIME_CORE_TEST(test_wait_tracking_caller_rolls_back_saturation);
    RUN_RUNTIME_CORE_TEST(test_active_io_tracking_counter_saturation_rolls_back);
    RUN_RUNTIME_CORE_TEST(test_deferred_fatal_stops_and_drains_parked_wait);
    RUN_RUNTIME_CORE_TEST(test_deferred_fatal_rewakes_prestopped_runtime);
    RUN_RUNTIME_CORE_TEST(test_wait_resolver_scalar_cancel_drain);
    RUN_RUNTIME_CORE_TEST(test_wait_resolver_select_cancel_drain);
    RUN_RUNTIME_CORE_TEST(test_wait_resolver_block_job_recycle_drain);
    RUN_RUNTIME_CORE_TEST(test_public_active_op_release_sequence);
    RUN_RUNTIME_CORE_TEST(test_task_group_unique_spawn_reservations);
    RUN_RUNTIME_CORE_TEST(test_runtime_dump_while_blocking_job_active);
    RUN_RUNTIME_CORE_TEST(test_concurrent_runtime_init_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_init_shutdown_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_init_stats_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_init_dump_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_init_sleep_contract);
    RUN_RUNTIME_CORE_TEST(test_optional_shard_storage_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_trace_ring_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_run_contract);
    RUN_RUNTIME_CORE_TEST(test_concurrent_spawn_contract);
#endif
    RUN_RUNTIME_CORE_TEST(test_detach_contract);
    RUN_RUNTIME_CORE_TEST(test_ex_option_prefixes);
    RUN_RUNTIME_CORE_TEST(test_waitable_timer_api);
#if LLAM_PLATFORM_POSIX
    RUN_RUNTIME_CORE_TEST(test_stat_path_metadata_contract);
#endif
    RUN_RUNTIME_CORE_TEST(test_blocking_wrappers_api);
    RUN_RUNTIME_CORE_TEST(test_signal_wait_api);
    RUN_RUNTIME_CORE_TEST(test_errno_is_task_local_across_switches);
    RUN_RUNTIME_CORE_TEST(test_direct_yield_failure_keeps_task_running);
#if LLAM_ARCH_AARCH64 && !LLAM_PLATFORM_WINDOWS
    RUN_RUNTIME_CORE_TEST(test_aarch64_simd_is_preserved_across_switches);
#endif
    RUN_RUNTIME_CORE_TEST(test_concurrent_join_contract);
    printf("[test_runtime_core] ok\n");
    return 0;
}
