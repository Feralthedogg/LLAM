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
#include "engine/runtime_watchdog_internal.h"
#include "async_preempt_abi.h"
#include <stdio.h>

#if !defined(__linux__) || !defined(__x86_64__) || !defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
int main(void) {
    puts("test_async_preempt_multi_linux_x86_64: skipped (experimental backend unavailable)");
    return 77;
}
#else

extern unsigned llam_async_safe_leaf(atomic_uint *release);
extern char llam_async_safe_leaf_begin[];
extern char llam_async_safe_leaf_end[];
extern unsigned llam_async_avx_leaf(atomic_uint *release);
extern char llam_async_avx_leaf_begin[];
extern char llam_async_avx_leaf_end[];

#define MULTI_MAX_WORKERS 4U
#define MULTI_OFFLOADERS 2U
typedef struct multi_state multi_state_t;
typedef struct multi_item {
    multi_state_t *state;
    unsigned id;
} multi_item_t;
struct multi_state {
    llam_runtime_t *runtime;
    unsigned workers;
    atomic_uint release;
    atomic_uint foreign_release;
    atomic_uint started;
    atomic_uint finished;
    atomic_uint callbacks_started;
    atomic_uint callbacks_done;
    atomic_uint nested;
    atomic_uint failures;
    atomic_uint controller_done;
    atomic_uint rescue_stop;
    atomic_uint rescued;
    multi_item_t leaves[2U * MULTI_MAX_WORKERS];
    multi_item_t offloads[MULTI_OFFLOADERS];
};

static int set_worker_affinity(unsigned count, cpu_set_t *previous) {
    cpu_set_t selected;
    unsigned cpu;
    unsigned found = 0U;

    if (sched_getaffinity(0, sizeof(*previous), previous) != 0) {
        return -1;
    }
    CPU_ZERO(&selected);
    for (cpu = 0U; cpu < CPU_SETSIZE && found < count; ++cpu) {
        if (CPU_ISSET((int)cpu, previous)) {
            CPU_SET((int)cpu, &selected);
            ++found;
        }
    }
    if (found != count) {
        errno = ENOTSUP;
        return -1;
    }
    return sched_setaffinity(0, sizeof(selected), &selected);
}

static int make_runtime(llam_runtime_t **runtime) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) {
        return -1;
    }
    opts.deterministic = 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create(&opts, sizeof(opts), runtime) != 0) {
        return -1;
    }
    if (llam_async_preempt_register_safe_range(*runtime,
        (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0) {
        return -1;
    }
    return llam_async_preempt_register_safe_range(*runtime,
        (uintptr_t)llam_async_avx_leaf_begin, (uintptr_t)llam_async_avx_leaf_end);
}

static int make_dynamic_runtime(llam_runtime_t **runtime) {
    llam_runtime_opts_t opts;

    if (llam_runtime_opts_init(&opts, sizeof(opts)) != 0) return -1;
    opts.deterministic = 0U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ |
                              LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS;
    opts.preempt_mode = LLAM_PREEMPT_AUTO;
    opts.preempt_quantum_ns = UINT64_C(1000000);
    if (llam_runtime_create(&opts, sizeof(opts), runtime) != 0) return -1;
    if (llam_async_preempt_register_safe_range(*runtime,
        (uintptr_t)llam_async_safe_leaf_begin, (uintptr_t)llam_async_safe_leaf_end) != 0) {
        return -1;
    }
    return llam_async_preempt_register_safe_range(*runtime,
        (uintptr_t)llam_async_avx_leaf_begin, (uintptr_t)llam_async_avx_leaf_end);
}

static void fail(multi_state_t *state, const char *message) {
    fprintf(stderr, "multi async: %s\n", message);
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static bool same_origin(llam_worker_t *origin, uint64_t incarnation, pthread_t thread,
                         int *errno_address) {
    return g_llam_tls_worker == origin && pthread_equal(thread, pthread_self()) &&
        &errno == errno_address &&
        atomic_load_explicit(&origin->binding_incarnation, memory_order_acquire) == incarnation &&
        llam_async_preempt_task_origin_shard(g_llam_tls_task) == origin->compat_shard->id;
}

static void busy_leaf(void *opaque) {
    multi_item_t *item = opaque;
    multi_state_t *state = item->state;
    llam_worker_t *origin = g_llam_tls_worker;
    uint64_t incarnation = atomic_load_explicit(&origin->binding_incarnation, memory_order_acquire);
    pthread_t thread = pthread_self();
    int *errno_address = &errno;
    int expected_errno = EDOM + (int)item->id;
    unsigned i;
    unsigned registers_ok;
    llam_async_preempt_state_t *capture_state = (llam_async_preempt_state_t *)atomic_load_explicit(
        &origin->async_preempt_state, memory_order_acquire);

    for (i = 0U; i < state->workers; ++i) {
        llam_async_preempt_state_t *peer = (llam_async_preempt_state_t *)atomic_load_explicit(
            &state->runtime->shards[i].primary_worker.async_preempt_state, memory_order_acquire);
        if (peer == NULL || atomic_load_explicit(&peer->frame_probe_status, memory_order_acquire) != 2U) {
            fail(state, "user code ran before all native M probes completed");
        }
    }
    atomic_fetch_add_explicit(&state->started, 1U, memory_order_release);
    errno = expected_errno;
    registers_ok = (item->id & 1U) != 0U && (capture_state->xsave_mask & UINT64_C(4)) != 0U
                       ? llam_async_avx_leaf(&state->release) : llam_async_safe_leaf(&state->release);
    if (registers_ok != 1U || errno != expected_errno ||
        !same_origin(origin, incarnation, thread, errno_address)) {
        fail(state, "capture changed registers/errno/native origin");
    }
    llam_yield();
    if (llam_sleep_ns(UINT64_C(100000)) != 0 ||
        !same_origin(origin, incarnation, thread, errno_address)) {
        fail(state, "cooperative yield/timer wake changed origin");
    }
    atomic_fetch_add_explicit(&state->finished, 1U, memory_order_release);
}

static void *nested_callback(void *opaque) {
    multi_state_t *state = opaque;

    if (g_llam_tls_worker != NULL || g_llam_tls_task != NULL) {
        fail(state, "native callback impersonated a managed M/G");
    }
    atomic_fetch_add_explicit(&state->nested, 1U, memory_order_release);
    return state;
}

static void *foreign_callback(void *opaque) {
    multi_item_t *item = opaque;
    multi_state_t *state = item->state;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
    void *nested_result = NULL;
    pthread_t thread = pthread_self();

    atomic_fetch_add_explicit(&state->callbacks_started, 1U, memory_order_release);
    if (llam_call_blocking_result(nested_callback, state, &nested_result) != 0 ||
        nested_result != state || !pthread_equal(thread, pthread_self())) {
        fail(state, "native inline nested callback failed");
    }
    while (atomic_load_explicit(&state->foreign_release, memory_order_acquire) == 0U) {
        (void)nanosleep(&delay, NULL);
    }
    atomic_fetch_add_explicit(&state->callbacks_done, 1U, memory_order_release);
    errno = E2BIG;
    return item;
}

static void offload_task(void *opaque) {
    multi_item_t *item = opaque;
    multi_state_t *state = item->state;
    llam_worker_t *origin = g_llam_tls_worker;
    uint64_t incarnation = atomic_load_explicit(&origin->binding_incarnation, memory_order_acquire);
    pthread_t thread = pthread_self();
    int *errno_address = &errno;
    void *result = NULL;
    int rc = llam_call_blocking_result(foreign_callback, item, &result);
    int err = errno;

    if (!same_origin(origin, incarnation, thread, errno_address) ||
        atomic_load_explicit(&state->callbacks_done, memory_order_acquire) == 0U ||
        rc != 0 || result != item || err != E2BIG) {
        fail(state, "offload completion lost result lifetime or origin");
    }
}

static void controller_task(void *opaque) {
    multi_state_t *state = opaque;
    bool ready;
    unsigned i;

    do {
        ready = atomic_load_explicit(&state->started, memory_order_acquire) == 2U * state->workers &&
                atomic_load_explicit(&state->callbacks_started, memory_order_acquire) == MULTI_OFFLOADERS;
        for (i = 0U; i < state->workers; ++i) {
            ready = ready && atomic_load_explicit(
                &state->runtime->shards[i].metrics.async_preempt_captures, memory_order_acquire) >= 6U &&
                llam_async_preempt_ready_depth(&state->runtime->shards[i]) >= 1U;
        }
        if (!ready) {
            llam_yield();
        }
    } while (!ready && atomic_load_explicit(&state->rescued, memory_order_acquire) == 0U);
    if (!ready || atomic_load_explicit(&state->finished, memory_order_acquire) != 0U) {
        fail(state, "multiple M capsule intervals did not overlap before release");
    }
    errno = 0;
    if (llam_enter_blocking() != -1 || errno != ENOTSUP ||
        atomic_load_explicit(&g_llam_tls_task->opaque_blocking_depth, memory_order_acquire) != 0U ||
        g_llam_tls_worker != &g_llam_tls_shard->primary_worker) {
        fail(state, "inline foreign refusal changed the managed binding");
    }
    atomic_store_explicit(&state->foreign_release, 1U, memory_order_release);
    atomic_store_explicit(&state->release, 1U, memory_order_release);
    atomic_store_explicit(&state->controller_done, 1U, memory_order_release);
}

static void *rescue_thread(void *opaque) {
    multi_state_t *state = opaque;
    uint64_t deadline = llam_now_ns() + UINT64_C(5000000000);
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000L};

    while (atomic_load_explicit(&state->rescue_stop, memory_order_acquire) == 0U &&
           llam_now_ns() < deadline) {
        (void)nanosleep(&delay, NULL);
    }
    if (atomic_load_explicit(&state->rescue_stop, memory_order_acquire) == 0U) {
        atomic_store_explicit(&state->rescued, 1U, memory_order_release);
        atomic_store_explicit(&state->release, 1U, memory_order_release);
        atomic_store_explicit(&state->foreign_release, 1U, memory_order_release);
    }
    return NULL;
}

static int run_multi(unsigned workers) {
    multi_state_t state = {0};
    llam_task_t *tasks[2U * MULTI_MAX_WORKERS + MULTI_OFFLOADERS + 1U] = {0};
    llam_spawn_opts_t spawn;
    cpu_set_t previous;
    pthread_t rescue;
    bool rescue_started = false;
    unsigned i;
    unsigned count = 0U;
    int result = 1;
    int run_rc;

    if (set_worker_affinity(workers, &previous) != 0) {
        return 77;
    }
    state.workers = workers;
    atomic_init(&state.release, 0U);
    atomic_init(&state.foreign_release, 0U);
    atomic_init(&state.started, 0U);
    atomic_init(&state.finished, 0U);
    atomic_init(&state.callbacks_started, 0U);
    atomic_init(&state.callbacks_done, 0U);
    atomic_init(&state.nested, 0U);
    atomic_init(&state.failures, 0U);
    atomic_init(&state.controller_done, 0U);
    atomic_init(&state.rescue_stop, 0U);
    atomic_init(&state.rescued, 0U);
    if (make_runtime(&state.runtime) != 0 || state.runtime->active_shards != workers ||
        llam_async_preempt_enable_runtime(state.runtime) != 0 ||
        llam_spawn_opts_init(&spawn, sizeof(spawn)) != 0) {
        goto cleanup;
    }
    spawn.flags = LLAM_SPAWN_F_PINNED;
    for (i = 0U; i < 2U * workers; ++i) {
        state.leaves[i] = (multi_item_t){&state, i};
        tasks[count] = llam_runtime_spawn_ex(state.runtime, busy_leaf, &state.leaves[i], &spawn, sizeof(spawn));
        if (tasks[count++] == NULL) {
            goto cleanup;
        }
    }
    for (i = 0U; i < MULTI_OFFLOADERS; ++i) {
        state.offloads[i] = (multi_item_t){&state, i};
        tasks[count] = llam_runtime_spawn_ex(state.runtime, offload_task, &state.offloads[i], &spawn, sizeof(spawn));
        if (tasks[count++] == NULL) {
            goto cleanup;
        }
    }
    spawn.cancel_token = NULL;
    tasks[count] = llam_runtime_spawn_ex(state.runtime, controller_task, &state, &spawn, sizeof(spawn));
    if (tasks[count++] == NULL || pthread_create(&rescue, NULL, rescue_thread, &state) != 0) {
        goto cleanup;
    }
    rescue_started = true;
    run_rc = llam_runtime_run_handle(state.runtime);
    atomic_store_explicit(&state.rescue_stop, 1U, memory_order_release);
    (void)pthread_join(rescue, NULL);
    rescue_started = false;
    if (run_rc != 0 && atomic_load(&state.runtime->preempt_capability.failure_reason) ==
                            LLAM_PREEMPT_FAILURE_SIGNAL_FRAME_UNSUPPORTED &&
        atomic_load(&state.started) == 0U && atomic_load(&state.callbacks_started) == 0U) {
        result = 77;
        goto cleanup;
    }
    if (run_rc != 0 || atomic_load(&state.failures) != 0U || atomic_load(&state.rescued) != 0U ||
        atomic_load(&state.finished) != 2U * workers || atomic_load(&state.nested) != MULTI_OFFLOADERS ||
        atomic_load(&state.controller_done) != 1U || atomic_load(&state.runtime->block_pending) != 0U) {
        fprintf(stderr, "multi run failed: workers=%u rc=%d started=%u finished=%u callbacks=%u failures=%u rescue=%u\n",
                workers, run_rc, atomic_load(&state.started), atomic_load(&state.finished),
                atomic_load(&state.callbacks_started), atomic_load(&state.failures), atomic_load(&state.rescued));
        goto cleanup;
    }
    for (i = 0U; i < workers; ++i) {
        llam_shard_t *shard = &state.runtime->shards[i];
        if (llam_async_preempt_has_origin_obligations(shard) ||
            llam_async_preempt_ready_depth(shard) != 0U || shard->opaque_helper_thread_started) {
            goto cleanup;
        }
    }
    for (i = 0U; i < count; ++i) {
        if (llam_join(tasks[i]) != 0) {
            goto cleanup;
        }
    }
    printf("test_async_preempt_multi_linux_x86_64: %u primary M, overlapping capsules, origin waits/offload passed\n", workers);
    result = 0;
cleanup:
    atomic_store(&state.rescue_stop, 1U);
    atomic_store(&state.release, 1U);
    atomic_store(&state.foreign_release, 1U);
    if (rescue_started) {
        (void)pthread_join(rescue, NULL);
    }
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    return result;
}

static void count_task(void *opaque) {
    atomic_uint *ran = opaque;
    atomic_fetch_add_explicit(ran, 1U, memory_order_release);
}

typedef struct quota_spawner {
    llam_runtime_t *runtime;
    atomic_uint *go;
    atomic_uint *ran;
    llam_task_t *task;
    int error;
} quota_spawner_t;

static void *quota_spawn_thread(void *opaque) {
    quota_spawner_t *spawner = opaque;
    while (atomic_load_explicit(spawner->go, memory_order_acquire) == 0U) {
        sched_yield();
    }
    spawner->task = llam_runtime_spawn_ex(spawner->runtime, count_task, spawner->ran, NULL, 0U);
    spawner->error = errno;
    return NULL;
}

static int test_startup_rollback(void) {
    cpu_set_t previous;
    llam_runtime_t *runtime = NULL;
    atomic_uint ran = ATOMIC_VAR_INIT(0U);
    unsigned kind;
    unsigned i;
    int result = 1;

    if (set_worker_affinity(3U, &previous) != 0) {
        return 77;
    }
    if (make_runtime(&runtime) != 0) {
        goto cleanup;
    }
    llam_async_preempt_test_fail_allocation_after(LLAM_ASYNC_TEST_FAIL_WORKER, 1U);
    if (llam_async_preempt_enable_runtime(runtime) != -1 || errno != ENOMEM ||
        atomic_load(&runtime->async_preempt_enabled) != 0U || runtime->async_preempt_control != NULL) {
        goto cleanup;
    }
    for (i = 0U; i < 3U; ++i) {
        if (atomic_load(&runtime->shards[i].primary_worker.async_preempt_state) != 0U) {
            goto cleanup;
        }
    }
    if (llam_async_preempt_enable_runtime(runtime) != 0) {
        goto cleanup;
    }
    llam_runtime_destroy(runtime);
    runtime = NULL;
    for (kind = LLAM_ASYNC_TEST_FAIL_THREAD_START; kind <= LLAM_ASYNC_TEST_FAIL_WORKER_PROBE; ++kind) {
        if (make_runtime(&runtime) != 0 || llam_async_preempt_enable_runtime(runtime) != 0 ||
            llam_runtime_spawn_ex(runtime, count_task, &ran, NULL, 0U) == NULL) {
            goto cleanup;
        }
        llam_async_preempt_test_fail_worker(kind, 2U);
        if (llam_runtime_run_handle(runtime) != -1 ||
            errno != (kind == LLAM_ASYNC_TEST_FAIL_THREAD_START ? EAGAIN : EIO) ||
            atomic_load(&ran) != 0U || atomic_load(&runtime->exec_started)) {
            goto cleanup;
        }
        for (i = 0U; i < 3U; ++i) {
            llam_worker_t *worker = &runtime->shards[i].primary_worker;
            if (atomic_load(&worker->native_thread_live) || worker->sigaltstack_installed ||
                runtime->shards[i].thread_started ||
                llam_async_preempt_has_origin_obligations(&runtime->shards[i])) {
                goto cleanup;
            }
        }
        /* Never-started G storage is owned by teardown after a run failure. */
        llam_runtime_destroy(runtime);
        runtime = NULL;
    }
    puts("test_async_preempt_multi_linux_x86_64: nth-M allocation/start/probe rollback, user dispatch=0 passed");
    result = 0;
cleanup:
    llam_async_preempt_test_fail_allocation(0U);
    llam_async_preempt_test_fail_worker(0U, 0U);
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    if (result != 0) {
        fprintf(stderr, "multi startup rollback failed: errno=%d ran=%u\n", errno, atomic_load(&ran));
    }
    return result;
}

static int test_capsule_budget(void) {
    const char *name = "LLAM_ASYNC_CAPSULE_BUDGET_BYTES";
    const char *initial = getenv(name);
    char *saved = initial != NULL ? strdup(initial) : NULL;
    cpu_set_t previous;
    llam_runtime_t *runtime = NULL;
    llam_task_t *tasks[3] = {0};
    atomic_uint ran = ATOMIC_VAR_INIT(0U);
    uint64_t charge;
    char limit[32];
    unsigned point;
    int result = 1;

    if ((initial != NULL && saved == NULL) || set_worker_affinity(2U, &previous) != 0) {
        free(saved);
        return 77;
    }
    if (unsetenv(name) != 0 || make_runtime(&runtime) != 0 || llam_async_preempt_enable_runtime(runtime) != 0) {
        goto cleanup;
    }
    charge = llam_async_preempt_test_capsule_charge(runtime);
    llam_runtime_destroy(runtime);
    runtime = NULL;
    (void)snprintf(limit, sizeof(limit), "%llu", (unsigned long long)(2U * charge));
    if (charge == 0U || setenv(name, limit, 1) != 0 || make_runtime(&runtime) != 0 ||
        llam_async_preempt_enable_runtime(runtime) != 0) {
        goto cleanup;
    }
    for (point = LLAM_ASYNC_TEST_FAIL_TASK; point <= LLAM_ASYNC_TEST_FAIL_BANK1; ++point) {
        llam_async_preempt_test_fail_allocation(point);
        if (llam_runtime_spawn_ex(runtime, count_task, &ran, NULL, 0U) != NULL || errno != ENOMEM ||
            llam_async_preempt_test_capsule_bytes(runtime) != 0U) {
            goto cleanup;
        }
    }
    {
        quota_spawner_t spawners[8] = {0};
        pthread_t threads[8];
        atomic_uint go = ATOMIC_VAR_INIT(0U);
        unsigned created = 0U;
        unsigned admitted = 0U;
        for (point = 0U; point < 8U; ++point) {
            spawners[point].runtime = runtime;
            spawners[point].go = &go;
            spawners[point].ran = &ran;
            if (pthread_create(&threads[point], NULL, quota_spawn_thread, &spawners[point]) != 0) {
                break;
            }
            ++created;
        }
        atomic_store_explicit(&go, 1U, memory_order_release);
        for (point = 0U; point < created; ++point) {
            (void)pthread_join(threads[point], NULL);
        }
        if (created != 8U) goto cleanup;
        for (point = 0U; point < 8U; ++point) {
            if (spawners[point].task != NULL) {
                if (admitted >= 2U) goto cleanup;
                tasks[admitted++] = spawners[point].task;
            } else if (spawners[point].error != ENOMEM) {
                goto cleanup;
            }
        }
        if (admitted != 2U) goto cleanup;
    }
    if (tasks[0] == NULL || tasks[1] == NULL ||
        llam_async_preempt_test_capsule_bytes(runtime) != 2U * charge ||
        llam_runtime_spawn_ex(runtime, count_task, &ran, NULL, 0U) != NULL || errno != ENOMEM ||
        llam_async_preempt_test_capsule_bytes(runtime) != 2U * charge || atomic_load(&ran) != 0U ||
        llam_runtime_run_handle(runtime) != 0 || atomic_load(&ran) != 2U ||
        llam_async_preempt_test_capsule_bytes(runtime) != 0U) {
        goto cleanup;
    }
    /* Completed public handles remain live, but retired capsule quota is reusable. */
    tasks[2] = llam_runtime_spawn_ex(runtime, count_task, &ran, NULL, 0U);
    if (tasks[2] == NULL || llam_runtime_run_handle(runtime) != 0 || atomic_load(&ran) != 3U ||
        llam_async_preempt_test_capsule_bytes(runtime) != 0U) {
        goto cleanup;
    }
    for (point = 0U; point < 3U; ++point) {
        if (llam_join(tasks[point]) != 0) {
            goto cleanup;
        }
    }
    /* Existing destructor assertions additionally verify the never-started reservation. */
    if (llam_runtime_spawn_ex(runtime, count_task, &ran, NULL, 0U) == NULL ||
        llam_async_preempt_test_capsule_bytes(runtime) != charge) {
        goto cleanup;
    }
    result = 0;
cleanup:
    llam_async_preempt_test_fail_allocation(0U);
    if (runtime != NULL) {
        llam_runtime_destroy(runtime);
    }
    if (saved != NULL) {
        (void)setenv(name, saved, 1);
    } else {
        (void)unsetenv(name);
    }
    free(saved);
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    if (result == 0) {
        puts("test_async_preempt_multi_linux_x86_64: capsule quota, allocation rollback, repeated run and retirement reuse passed");
    } else {
        fprintf(stderr, "multi capsule quota failed: errno=%d ran=%u\n", errno, atomic_load(&ran));
    }
    return result;
}

typedef struct slot_state {
    llam_runtime_t *runtime;
    unsigned mode;
    atomic_uint started;
    atomic_uint release;
    atomic_uint callback_done;
    atomic_uint queued_called;
    atomic_uint child_returned;
    atomic_uint queued_returned;
    atomic_uint rejected_called;
    atomic_uint nested;
    atomic_uint failures;
    int run_rc;
} slot_state_t;
typedef struct slot_child_arg {
    slot_state_t *state;
    bool queued;
    bool cancelled;
} slot_child_arg_t;

static void slot_fail(slot_state_t *state, const char *message) {
    fprintf(stderr, "blocking quota: %s\n", message);
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static bool slot_wait(slot_state_t *state, atomic_uint *value, unsigned target) {
    uint64_t deadline = llam_now_ns() + UINT64_C(3000000000);
    while (atomic_load_explicit(value, memory_order_acquire) != target) {
        if (llam_now_ns() >= deadline) {
            slot_fail(state, "fixture progress deadline expired");
            return false;
        }
        llam_yield();
    }
    return true;
}

static bool slot_wait_retired(slot_state_t *state) {
    uint64_t deadline = llam_now_ns() + UINT64_C(3000000000);
    while (llam_async_preempt_test_block_slots(state->runtime) != 0U) {
        if (llam_now_ns() >= deadline) {
            slot_fail(state, "job reservation did not retire");
            return false;
        }
        llam_yield();
    }
    return true;
}

static void *slot_rejected_callback(void *opaque) {
    slot_state_t *state = opaque;
    atomic_fetch_add_explicit(&state->rejected_called, 1U, memory_order_release);
    return state;
}

static void *slot_nested_callback(void *opaque) {
    slot_state_t *state = opaque;
    if (g_llam_tls_task != NULL || g_llam_tls_worker != NULL ||
        llam_async_preempt_test_block_slots(state->runtime) != 1U) {
        slot_fail(state, "inline nested callback acquired another pool ticket");
    }
    atomic_fetch_add_explicit(&state->nested, 1U, memory_order_release);
    return state;
}

static void *slot_recursive_callback(void *opaque) {
    slot_state_t *state = opaque;
    pthread_t thread = pthread_self();
    void *result = NULL;
    if (llam_call_blocking_result(slot_nested_callback, state, &result) != 0 ||
        result != state || !pthread_equal(thread, pthread_self())) {
        slot_fail(state, "native recursion did not remain synchronous on its worker");
    }
    return state;
}

static void *slot_held_callback(void *opaque) {
    slot_child_arg_t *arg = opaque;
    slot_state_t *state = arg->state;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
    if (arg->queued) {
        atomic_fetch_add_explicit(&state->queued_called, 1U, memory_order_release);
        return arg;
    }
    atomic_store_explicit(&state->started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->release, memory_order_acquire) == 0U) {
        (void)nanosleep(&delay, NULL);
    }
    atomic_store_explicit(&state->callback_done, 1U, memory_order_release);
    return arg;
}

static void slot_child(void *opaque) {
    slot_child_arg_t *arg = opaque;
    slot_state_t *state = arg->state;
    void *out = NULL;
    int rc = llam_call_blocking_result(slot_held_callback, arg, &out);
    if (arg->cancelled ? (rc != -1 || errno != ECANCELED || out != NULL)
                       : (rc != 0 || out != arg)) {
        slot_fail(state, "child callback completion/cancel result mismatch");
    }
    if (!arg->queued && atomic_load_explicit(&state->callback_done, memory_order_acquire) == 0U) {
        slot_fail(state, "running cancellation resumed the G before callback return");
    }
    atomic_store_explicit(arg->queued ? &state->queued_returned : &state->child_returned,
                          1U, memory_order_release);
}

static void slot_expect_full(slot_state_t *state, unsigned slots) {
    void *out = (void *)(uintptr_t)1U;
    uint64_t parks = atomic_load_explicit(&g_llam_tls_shard->metrics.parks, memory_order_acquire);
    if (llam_call_blocking_result(slot_rejected_callback, state, &out) != -1 || errno != EAGAIN ||
        out != NULL || atomic_load_explicit(&state->rejected_called, memory_order_acquire) != 0U ||
        atomic_load_explicit(&g_llam_tls_shard->metrics.parks, memory_order_acquire) != parks ||
        llam_async_preempt_test_block_slots(state->runtime) != slots) {
        slot_fail(state, "full admission executed/parked or released another job's capacity");
    }
}

static void slot_root(void *opaque) {
    slot_state_t *state = opaque;
    llam_cancel_token_t *token = NULL;
    llam_task_t *first = NULL;
    llam_task_t *queued = NULL;
    llam_spawn_opts_t spawn;
    slot_child_arg_t first_arg = {state, false, state->mode == 1U};
    slot_child_arg_t queued_arg = {state, true, true};
    void *out = NULL;

    if (state->mode == 3U) {
        if (llam_call_blocking_result(slot_rejected_callback, state, &out) != -1 || errno != ECANCELED ||
            out != NULL || llam_async_preempt_test_block_slots(state->runtime) != 0U ||
            atomic_load(&state->runtime->block_pending) != 0U || atomic_load(&state->rejected_called) != 0U) {
            slot_fail(state, "close after reservation did not roll preparation back");
        }
        return;
    }
    llam_async_preempt_test_fail_allocation(LLAM_ASYNC_TEST_FAIL_BLOCK_JOB);
    if (llam_call_blocking_result(slot_rejected_callback, state, &out) != -1 || errno != ENOMEM ||
        out != NULL || llam_async_preempt_test_block_slots(state->runtime) != 0U) {
        slot_fail(state, "job allocation failure leaked its early reservation");
    }
    token = llam_cancel_token_create();
    if (token == NULL || llam_spawn_opts_init(&spawn, sizeof(spawn)) != 0) {
        slot_fail(state, "token/spawn setup failed");
        goto finish;
    }
    spawn.cancel_token = state->mode == 1U ? token : NULL;
    first = llam_runtime_spawn_ex(state->runtime, slot_child, &first_arg, &spawn, sizeof(spawn));
    if (first == NULL || !slot_wait(state, &state->started, 1U)) {
        goto finish;
    }
    if (state->mode == 2U) {
        uint64_t deadline = llam_now_ns() + UINT64_C(3000000000);
        spawn.cancel_token = token;
        queued = llam_runtime_spawn_ex(state->runtime, slot_child, &queued_arg, &spawn, sizeof(spawn));
        if (queued == NULL) {
            slot_fail(state, "queued child spawn failed");
            goto finish;
        }
        while (atomic_load_explicit(&state->runtime->block_pending, memory_order_acquire) != 2U) {
            if (llam_now_ns() >= deadline) {
                slot_fail(state, "second job did not queue behind the held worker");
                goto finish;
            }
            llam_yield();
        }
    }
    if (llam_join_until(first, llam_now_ns()) != -1 || errno != ETIMEDOUT) {
        slot_fail(state, "join timeout consumed the running callback task");
    }
    slot_expect_full(state, state->mode);
    if (llam_cancel_token_cancel(token) != 0) {
        slot_fail(state, "cancellation request failed");
    }
    if (state->mode == 2U) {
        (void)slot_wait(state, &state->queued_returned, 1U);
    } else if (atomic_load(&state->child_returned) != 0U) {
        slot_fail(state, "running callback returned early on cancel");
    }
    slot_expect_full(state, state->mode);
finish:
    atomic_store_explicit(&state->release, 1U, memory_order_release);
    if (first != NULL && llam_join(first) != 0) {
        slot_fail(state, "first child join failed");
    }
    if (queued != NULL && llam_join(queued) != 0) {
        slot_fail(state, "queued child join failed");
    }
    if (token != NULL && llam_cancel_token_destroy(token) != 0) {
        slot_fail(state, "token still referenced after both children joined");
    }
    if (slot_wait_retired(state) &&
        (llam_call_blocking_result(slot_recursive_callback, state, &out) != 0 || out != state)) {
        slot_fail(state, "quota was not reusable for a recursive native callback");
    }
    (void)slot_wait_retired(state);
    if (atomic_load(&state->queued_called) != 0U || atomic_load(&state->nested) != 1U) {
        slot_fail(state, "cancelled queued callback executed or recursion did not complete");
    }
}

static void *slot_run_thread(void *opaque) {
    slot_state_t *state = opaque;
    state->run_rc = llam_runtime_run_handle(state->runtime);
    return NULL;
}

static int test_block_slots(unsigned mode) {
    const char *name = "LLAM_ASYNC_BLOCK_JOB_LIMIT";
    const char *initial = getenv(name);
    char *saved = initial != NULL ? strdup(initial) : NULL;
    slot_state_t state = {0};
    cpu_set_t previous;
    llam_task_t *root = NULL;
    int result = 1;

    if ((initial != NULL && saved == NULL) || set_worker_affinity(1U, &previous) != 0) {
        free(saved);
        return 77;
    }
    state.mode = mode;
    atomic_init(&state.started, 0U);
    atomic_init(&state.release, 0U);
    atomic_init(&state.callback_done, 0U);
    atomic_init(&state.queued_called, 0U);
    atomic_init(&state.child_returned, 0U);
    atomic_init(&state.queued_returned, 0U);
    atomic_init(&state.rejected_called, 0U);
    atomic_init(&state.nested, 0U);
    atomic_init(&state.failures, 0U);
    if (setenv(name, mode == 2U ? "2" : "1", 1) != 0 || make_runtime(&state.runtime) != 0 ||
        llam_async_preempt_enable_runtime(state.runtime) != 0 || state.runtime->block_worker_count != 1U) {
        goto cleanup;
    }
    root = llam_runtime_spawn_ex(state.runtime, slot_root, &state, NULL, 0U);
    if (root == NULL) {
        goto cleanup;
    }
    if (mode == 3U) {
        pthread_t driver;
        uint64_t deadline = llam_now_ns() + UINT64_C(3000000000);
        (void)llam_async_preempt_test_block_reserve_gate(1U);
        if (pthread_create(&driver, NULL, slot_run_thread, &state) != 0) {
            goto cleanup;
        }
        while (llam_async_preempt_test_block_reserve_gate(UINT_MAX) != 2U && llam_now_ns() < deadline) {
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
        }
        if (llam_async_preempt_test_block_reserve_gate(UINT_MAX) != 2U ||
            llam_async_preempt_test_block_slots(state.runtime) != 1U ||
            atomic_load(&state.runtime->block_pending) != 0U ||
            llam_runtime_request_stop_rt(state.runtime) != 0) {
            slot_fail(&state, "reservation/close gate was not reached");
        }
        (void)llam_async_preempt_test_block_reserve_gate(0U);
        (void)pthread_join(driver, NULL);
    } else {
        state.run_rc = llam_runtime_run_handle(state.runtime);
    }
    if (state.run_rc == 0 && atomic_load(&state.failures) == 0U && llam_join(root) == 0 &&
        llam_async_preempt_test_block_slots(state.runtime) == 0U &&
        atomic_load(&state.runtime->block_pending) == 0U &&
        llam_async_preempt_test_capsule_bytes(state.runtime) == 0U) {
        result = 0;
        printf("test_async_preempt_multi_linux_x86_64: blocking tickets mode=%u (running/queued-cancel/prepare-stop) passed\n", mode);
    }
cleanup:
    (void)llam_async_preempt_test_block_reserve_gate(0U);
    llam_async_preempt_test_fail_allocation(0U);
    atomic_store(&state.release, 1U);
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    if (saved != NULL) {
        (void)setenv(name, saved, 1);
    } else {
        (void)unsetenv(name);
    }
    free(saved);
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    return result;
}

typedef struct stop_state stop_state_t;
typedef struct stop_item { stop_state_t *state; unsigned id; } stop_item_t;
struct stop_state {
    llam_runtime_t *runtime;
    unsigned workers;
    bool destroy;
    atomic_uint release[MULTI_MAX_WORKERS];
    atomic_uint foreign_release;
    atomic_uint callbacks_started;
    atomic_uint callbacks_done;
    atomic_uint timers_started;
    atomic_uint timers_done;
    atomic_uint busy_started;
    atomic_uint busy_done;
    atomic_uint foreign_done;
    atomic_uint controllers_done;
    atomic_uint new_calls;
    atomic_uint failures;
    stop_item_t items[MULTI_MAX_WORKERS];
    int run_rc;
};

static void stop_fail(stop_state_t *state, const char *message) {
    fprintf(stderr, "async stop drain: %s\n", message);
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static void *stop_foreign_callback(void *opaque) {
    stop_item_t *item = opaque;
    stop_state_t *state = item->state;
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
    atomic_fetch_add_explicit(&state->callbacks_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->foreign_release, memory_order_acquire) == 0U) {
        (void)nanosleep(&delay, NULL);
    }
    atomic_fetch_add_explicit(&state->callbacks_done, 1U, memory_order_release);
    return item;
}

static void stop_foreign_task(void *opaque) {
    stop_item_t *item = opaque;
    stop_state_t *state = item->state;
    void *result = NULL;
    if (llam_call_blocking_result(stop_foreign_callback, item, &result) != -1 ||
        errno != ECANCELED || result != NULL ||
        atomic_load_explicit(&state->callbacks_done, memory_order_acquire) == 0U) {
        stop_fail(state, "callback did not drain safely through cancellation");
    }
    atomic_fetch_add_explicit(&state->foreign_done, 1U, memory_order_release);
}

static void stop_timer_task(void *opaque) {
    stop_item_t *item = opaque;
    stop_state_t *state = item->state;
    atomic_fetch_add_explicit(&state->timers_started, 1U, memory_order_release);
    if (llam_sleep_ns(UINT64_C(10000000000)) != -1 || errno != ECANCELED) {
        stop_fail(state, "timer cancellation was not serviced during drain");
    }
    atomic_fetch_add_explicit(&state->timers_done, 1U, memory_order_release);
}

static void stop_busy_task(void *opaque) {
    stop_item_t *item = opaque;
    stop_state_t *state = item->state;
    while ((atomic_load_explicit(&state->callbacks_started, memory_order_acquire) != state->workers ||
            atomic_load_explicit(&state->timers_started, memory_order_acquire) != state->workers) &&
           !atomic_load_explicit(&state->runtime->stop_requested, memory_order_acquire)) {
        llam_yield();
    }
    if (g_llam_tls_shard->id != item->id) {
        stop_fail(state, "fixture busy task was not on its intended primary");
    }
    atomic_fetch_add_explicit(&state->busy_started, 1U, memory_order_release);
    if (llam_async_safe_leaf(&state->release[item->id]) != 1U) {
        stop_fail(state, "drain capture corrupted a no-poll leaf");
    }
    atomic_fetch_add_explicit(&state->busy_done, 1U, memory_order_release);
}

static void *stop_unadmitted_callback(void *opaque) {
    stop_state_t *state = opaque;
    atomic_fetch_add_explicit(&state->new_calls, 1U, memory_order_release);
    return state;
}

static void stop_controller_task(void *opaque) {
    stop_item_t *item = opaque;
    stop_state_t *state = item->state;
    void *out = NULL;
    while (!atomic_load_explicit(&state->runtime->stop_requested, memory_order_acquire)) {
        llam_yield();
    }
    if (g_llam_tls_shard->id != item->id || atomic_load_explicit(
        &g_llam_tls_shard->metrics.async_preempt_captures, memory_order_acquire) == 0U) {
        stop_fail(state, "controller ran without that M's actual drain capture");
    }
    if (!llam_async_preempt_test_drain_guards(g_llam_tls_worker)) {
        stop_fail(state, "drain authorization accepted a stale/helper/foreign descriptor");
    }
    if (llam_call_blocking_result(stop_unadmitted_callback, state, &out) != -1 || errno != ECANCELED ||
        out != NULL || llam_runtime_spawn_ex(state->runtime, count_task, &state->new_calls, NULL, 0U) != NULL) {
        stop_fail(state, "close accepted a new job or G");
    }
    if (state->destroy) {
        llam_processor_owner_ticket_t ordinary;
        llam_worker_t *worker;
        uint64_t incarnation;
        uint64_t epoch;
        pthread_t thread;
        pthread_mutex_lock(&g_llam_tls_shard->opaque_lock);
        pthread_mutex_lock(&g_llam_tls_shard->lock);
        bool ticket_snapshot = llam_processor_owner_ticket_snapshot_pair_locked(g_llam_tls_shard, &ordinary);
        bool watchdog_snapshot = llam_legacy_lease_snapshot_watchdog_locked(g_llam_tls_shard,
            &worker, &incarnation, &epoch, &thread);
        bool shutdown_visible = atomic_load_explicit(&state->runtime->shutdown_requested, memory_order_acquire);
        if (ticket_snapshot || watchdog_snapshot) {
            fprintf(stderr, "async stop drain snapshot: ticket=%u watchdog=%u shutdown=%u lease=%u\n",
                ticket_snapshot ? 1U : 0U, watchdog_snapshot ? 1U : 0U,
                shutdown_visible ? 1U : 0U, g_llam_tls_shard->legacy_lease.state);
            stop_fail(state, "ordinary STOPPING snapshot contract was widened");
        }
        pthread_mutex_unlock(&g_llam_tls_shard->lock);
        pthread_mutex_unlock(&g_llam_tls_shard->opaque_lock);
    }
    if (item->id == 0U) {
        atomic_store_explicit(&state->foreign_release, 1U, memory_order_release);
    }
    /* Only this admitted G, on the same M, may release its busy peer. */
    atomic_store_explicit(&state->release[item->id], 1U, memory_order_release);
    atomic_fetch_add_explicit(&state->controllers_done, 1U, memory_order_release);
}

static void *stop_run_driver(void *opaque) {
    stop_state_t *state = opaque;
    state->run_rc = llam_runtime_run_handle(state->runtime);
    return NULL;
}
static void *stop_destroy_driver(void *opaque) {
    stop_state_t *state = opaque;
    llam_runtime_destroy(state->runtime);
    return NULL;
}

static int test_stop_service(bool destroy) {
    stop_state_t state = {0};
    llam_task_t *tasks[4U * MULTI_MAX_WORKERS] = {0};
    llam_task_fn functions[] = {stop_foreign_task, stop_timer_task, stop_busy_task, stop_controller_task};
    llam_spawn_opts_t spawn;
    cpu_set_t previous;
    pthread_t driver;
    pthread_t destroyer;
    unsigned count = 0U;
    unsigned i;
    unsigned group;
    uint64_t deadline;
    int result = 1;

    state.workers = 2U;
    state.destroy = destroy;
    if (set_worker_affinity(state.workers, &previous) != 0) {
        return 77;
    }
    for (i = 0U; i < MULTI_MAX_WORKERS; ++i) {
        atomic_init(&state.release[i], 0U);
        state.items[i] = (stop_item_t){&state, i};
    }
    atomic_init(&state.foreign_release, 0U);
    atomic_init(&state.callbacks_started, 0U);
    atomic_init(&state.callbacks_done, 0U);
    atomic_init(&state.timers_started, 0U);
    atomic_init(&state.timers_done, 0U);
    atomic_init(&state.busy_started, 0U);
    atomic_init(&state.busy_done, 0U);
    atomic_init(&state.foreign_done, 0U);
    atomic_init(&state.controllers_done, 0U);
    atomic_init(&state.new_calls, 0U);
    atomic_init(&state.failures, 0U);
    if (make_runtime(&state.runtime) != 0 || llam_async_preempt_enable_runtime(state.runtime) != 0 ||
        llam_spawn_opts_init(&spawn, sizeof(spawn)) != 0) {
        goto cleanup;
    }
    spawn.flags = LLAM_SPAWN_F_PINNED;
    for (group = 0U; group < 4U; ++group) {
        for (i = 0U; i < state.workers; ++i) {
            tasks[count] = llam_runtime_spawn_ex(state.runtime, functions[group], &state.items[i], &spawn, sizeof(spawn));
            if (tasks[count++] == NULL) {
                goto cleanup;
            }
        }
    }
    llam_async_preempt_test_hold_normal_requests(true);
    if (pthread_create(&driver, NULL, stop_run_driver, &state) != 0) {
        goto cleanup;
    }
    deadline = llam_now_ns() + UINT64_C(3000000000);
    while (llam_now_ns() < deadline) {
        bool ready = atomic_load_explicit(&state.busy_started, memory_order_acquire) == state.workers;
        for (i = 0U; i < state.workers; ++i) {
            llam_async_preempt_state_t *worker_state = (llam_async_preempt_state_t *)atomic_load_explicit(
                &state.runtime->shards[i].primary_worker.async_preempt_state, memory_order_acquire);
            ready = ready && atomic_load_explicit(&worker_state->status, memory_order_acquire) ==
                                 LLAM_ASYNC_PREEMPT_STATUS_REQUESTED;
        }
        if (ready) {
            for (i = 0U; i < state.workers; ++i) {
                if (atomic_load_explicit(&state.runtime->shards[i].metrics.async_preempt_captures,
                                         memory_order_acquire) != 0U) {
                    stop_fail(&state, "a normal capture escaped the delayed-delivery gate");
                }
            }
            break;
        }
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (llam_now_ns() >= deadline) {
        stop_fail(&state, "normal REQUESTED/busy fixture gate did not complete");
    }
    if (destroy) {
        if (pthread_create(&destroyer, NULL, stop_destroy_driver, &state) != 0) {
            (void)llam_runtime_request_stop_rt(state.runtime);
            (void)pthread_join(driver, NULL);
            goto cleanup;
        }
        (void)pthread_join(driver, NULL);
        (void)pthread_join(destroyer, NULL);
        state.runtime = NULL; /* Destroy already consumed it; do not inspect freed storage. */
    } else {
        (void)llam_runtime_request_stop_rt(state.runtime);
        (void)pthread_join(driver, NULL);
        for (i = 0U; i < count; ++i) {
            if (llam_join(tasks[i]) != 0) {
                stop_fail(&state, "plain-stop task join failed");
            }
        }
    }
    if (state.run_rc == 0 && atomic_load(&state.failures) == 0U &&
        atomic_load(&state.controllers_done) == state.workers &&
        atomic_load(&state.busy_done) == state.workers &&
        atomic_load(&state.timers_done) == state.workers &&
        atomic_load(&state.foreign_done) == state.workers &&
        atomic_load(&state.callbacks_done) == state.workers && atomic_load(&state.new_calls) == 0U) {
        result = 0;
        printf("test_async_preempt_multi_linux_x86_64: %s drain replaced stale normal requests and serviced busy/timer/offload Gs\n",
               destroy ? "destroy" : "plain-stop");
    }
cleanup:
    llam_async_preempt_test_hold_normal_requests(false);
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    return result;
}

static int test_startup_close(void) {
    stop_state_t state = {0};
    cpu_set_t previous;
    pthread_t driver;
    pthread_t destroyer;
    uint64_t deadline;
    int result = 1;

    atomic_init(&state.new_calls, 0U);
    if (set_worker_affinity(2U, &previous) != 0) {
        return 77;
    }
    if (make_runtime(&state.runtime) != 0 || llam_async_preempt_enable_runtime(state.runtime) != 0 ||
        llam_runtime_spawn_ex(state.runtime, count_task, &state.new_calls, NULL, 0U) == NULL ||
        llam_runtime_request_stop_rt(state.runtime) != 0 ||
        llam_runtime_run_handle(state.runtime) != -1 || errno != ECANCELED || atomic_load(&state.new_calls) != 0U) {
        goto cleanup;
    }
    llam_runtime_destroy(state.runtime);
    state.runtime = NULL;
    if (make_runtime(&state.runtime) != 0 || llam_async_preempt_enable_runtime(state.runtime) != 0 ||
        llam_runtime_spawn_ex(state.runtime, count_task, &state.new_calls, NULL, 0U) == NULL) {
        goto cleanup;
    }
    llam_async_preempt_test_fail_worker(LLAM_ASYNC_TEST_HOLD_WORKER_PROBE, 1U);
    (void)llam_async_preempt_test_prepare_gate(1U);
    if (pthread_create(&driver, NULL, stop_run_driver, &state) != 0) {
        goto cleanup;
    }
    deadline = llam_now_ns() + UINT64_C(3000000000);
    while (llam_async_preempt_test_prepare_gate(UINT_MAX) != 2U && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (llam_async_preempt_test_prepare_gate(UINT_MAX) != 2U ||
        pthread_create(&destroyer, NULL, stop_destroy_driver, &state) != 0) {
        (void)llam_async_preempt_test_prepare_gate(0U);
        (void)pthread_join(driver, NULL);
        goto cleanup;
    }
    while (!atomic_load_explicit(&state.runtime->shutdown_requested, memory_order_acquire) &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    bool closed_before_release = atomic_load_explicit(&state.runtime->shutdown_requested, memory_order_acquire);
    (void)llam_async_preempt_test_prepare_gate(0U);
    (void)pthread_join(driver, NULL);
    (void)pthread_join(destroyer, NULL);
    state.runtime = NULL;
    if (closed_before_release && state.run_rc == -1 && atomic_load(&state.new_calls) == 0U) {
        result = 0;
        puts("test_async_preempt_multi_linux_x86_64: stop/destroy during startup aborted without user dispatch or live-G drain");
    }
cleanup:
    (void)llam_async_preempt_test_prepare_gate(0U);
    llam_async_preempt_test_fail_worker(0U, 0U);
    if (state.runtime != NULL) {
        llam_runtime_destroy(state.runtime);
    }
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    return result;
}

static int test_invalid_resource_config(void) {
    const char *names[] = {"LLAM_ASYNC_CAPSULE_BUDGET_BYTES", "LLAM_ASYNC_BLOCK_JOB_LIMIT"};
    const char *invalid[] = {"", "0", "-1", "+1", "1x", " 16", "18446744073709551616"};
    char *saved[2] = {0};
    cpu_set_t previous;
    llam_runtime_t *runtime = NULL;
    unsigned n;
    unsigned v;
    int result = 1;

    if (set_worker_affinity(2U, &previous) != 0) {
        return 77;
    }
    for (n = 0U; n < 2U; ++n) {
        const char *value = getenv(names[n]);
        if (value != NULL) {
            saved[n] = strdup(value);
            if (saved[n] == NULL) goto cleanup;
        }
        (void)unsetenv(names[n]);
    }
    for (n = 0U; n < 2U; ++n) {
        for (v = 0U; v < sizeof(invalid) / sizeof(invalid[0]); ++v) {
            if (setenv(names[n], invalid[v], 1) != 0 || make_runtime(&runtime) != 0 ||
                llam_async_preempt_enable_runtime(runtime) != -1 || errno != EINVAL ||
                atomic_load(&runtime->async_preempt_enabled) != 0U || runtime->async_preempt_control != NULL) {
                goto cleanup;
            }
            llam_runtime_destroy(runtime);
            runtime = NULL;
        }
        (void)unsetenv(names[n]);
    }
    result = 0;
    puts("test_async_preempt_multi_linux_x86_64: malformed/zero/overflow resource config rejected before activation");
cleanup:
    if (runtime != NULL) llam_runtime_destroy(runtime);
    for (n = 0U; n < 2U; ++n) {
        if (saved[n] != NULL) (void)setenv(names[n], saved[n], 1);
        else (void)unsetenv(names[n]);
        free(saved[n]);
    }
    (void)sched_setaffinity(0, sizeof(previous), &previous);
    return result;
}

typedef struct dynamic_state {
    llam_runtime_t *runtime;
    int run_rc;
    llam_channel_t *channels[6U];
    atomic_uint channels_ready;
    llam_task_t *waiters[6U];
    llam_task_t *setup;
    llam_task_t *pressure[8U];
    atomic_uint waiters_started;
    atomic_uint waiters_done;
    atomic_uint pressure_started;
    atomic_uint pressure_done;
    atomic_uint route_done;
    atomic_uint failures;
    atomic_uint setup_failures;
    atomic_uint affinity_failures;
    atomic_uint channel_failures;
    atomic_uint origin_failures;
    atomic_uint pressure_failures;
    atomic_uint route_failures;
} dynamic_state_t;

typedef struct dynamic_wait_item {
    dynamic_state_t *state;
    unsigned shard_id;
} dynamic_wait_item_t;

typedef struct dynamic_pressure_item {
    dynamic_state_t *state;
    atomic_uint release;
} dynamic_pressure_item_t;

static dynamic_wait_item_t g_dynamic_wait_items[6U];
static dynamic_pressure_item_t g_dynamic_pressure_items[8U];

static void dynamic_prepare_channels(void *opaque) {
    dynamic_state_t *state = opaque;
    unsigned i;
    for (i = 0U; i < 6U; ++i) {
        state->channels[i] = llam_channel_create(1U);
        if (state->channels[i] == NULL) {
            atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
            atomic_fetch_add_explicit(&state->setup_failures, 1U, memory_order_relaxed);
            atomic_store_explicit(&state->channels_ready, 2U, memory_order_release);
            return;
        }
    }
    atomic_store_explicit(&state->channels_ready, 1U, memory_order_release);
}

static void dynamic_wait_task(void *opaque) {
    dynamic_wait_item_t *item = opaque;
    dynamic_state_t *state = item->state;
    llam_worker_t *origin = g_llam_tls_worker;
    uint64_t incarnation = atomic_load_explicit(&origin->binding_incarnation, memory_order_acquire);
    pthread_t native_thread = pthread_self();
    int *errno_address = &errno;
    void *value = NULL;
    if (g_llam_tls_shard == NULL || g_llam_tls_shard->id != item->shard_id) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
        atomic_fetch_add_explicit(&state->affinity_failures, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->waiters_started, 1U, memory_order_release);
    while (atomic_load_explicit(&state->channels_ready, memory_order_acquire) == 0U) {
        (void)llam_yield();
    }
    if (atomic_load_explicit(&state->channels_ready, memory_order_acquire) != 1U) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
        atomic_fetch_add_explicit(&state->channel_failures, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&state->waiters_done, 1U, memory_order_release);
        return;
    }
    if (llam_channel_recv_result(state->channels[item->shard_id], &value) == 0 || errno != EPIPE) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
        atomic_fetch_add_explicit(&state->channel_failures, 1U, memory_order_relaxed);
    }
    if (g_llam_tls_worker != origin || !pthread_equal(native_thread, pthread_self()) ||
        &errno != errno_address || atomic_load_explicit(&origin->binding_incarnation,
        memory_order_acquire) != incarnation || g_llam_tls_shard == NULL ||
        g_llam_tls_shard->id != item->shard_id) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
        atomic_fetch_add_explicit(&state->origin_failures, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->waiters_done, 1U, memory_order_release);
}

static void dynamic_pressure_task(void *opaque) {
    dynamic_pressure_item_t *item = opaque;
    dynamic_state_t *state = item->state;
    atomic_fetch_add_explicit(&state->pressure_started, 1U, memory_order_release);
    if (llam_async_safe_leaf(&item->release) != 1U || g_llam_tls_shard == NULL ||
        g_llam_tls_shard->id != 0U) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
        atomic_fetch_add_explicit(&state->pressure_failures, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->pressure_done, 1U, memory_order_release);
}

static void dynamic_route_probe(void *opaque) {
    dynamic_state_t *state = opaque;
    if (g_llam_tls_shard == NULL || g_llam_tls_shard->id != 2U) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_release);
        atomic_fetch_add_explicit(&state->route_failures, 1U, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&state->route_done, 1U, memory_order_release);
}

static void *dynamic_run_driver(void *opaque) {
    dynamic_state_t *state = opaque;
    state->run_rc = llam_runtime_run_handle(state->runtime);
    return NULL;
}

static bool wait_count(atomic_uint *value, unsigned target, uint64_t deadline) {
    const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
    while (atomic_load_explicit(value, memory_order_acquire) < target &&
           llam_now_ns() < deadline) {
        (void)nanosleep(&delay, NULL);
    }
    return atomic_load_explicit(value, memory_order_acquire) >= target;
}

static bool dynamic_waiters_parked(const llam_channel_t *const channels[6U]) {
    unsigned i;
    for (i = 0U; i < 6U; ++i) {
        llam_channel_t *channel = llam_channel_resolve_public_handle(channels[i]);
        bool parked;
        if (channel == NULL) return false;
        pthread_mutex_lock(&channel->lock);
        parked = channel->recv_waiters.head != NULL;
        pthread_mutex_unlock(&channel->lock);
        llam_channel_end_public_op(channel);
        if (!parked) return false;
    }
    return true;
}

static int test_dynamic_idle_domains(void) {
    dynamic_state_t state = {0};
    llam_spawn_opts_t spawn;
    cpu_set_t previous;
    pthread_t driver;
    bool driver_started = false;
    bool affinity_set = false;
    unsigned i;
    int result = 1;
    uint64_t deadline;
    unsigned phase = 0U;

    if (set_worker_affinity(6U, &previous) != 0) return 77;
    affinity_set = true;
    atomic_init(&state.waiters_started, 0U);
    atomic_init(&state.waiters_done, 0U);
    atomic_init(&state.pressure_started, 0U);
    atomic_init(&state.pressure_done, 0U);
    atomic_init(&state.route_done, 0U);
    atomic_init(&state.failures, 0U);
    atomic_init(&state.setup_failures, 0U);
    atomic_init(&state.affinity_failures, 0U);
    atomic_init(&state.channel_failures, 0U);
    atomic_init(&state.origin_failures, 0U);
    atomic_init(&state.pressure_failures, 0U);
    atomic_init(&state.route_failures, 0U);
    atomic_init(&state.channels_ready, 0U);
    if (make_dynamic_runtime(&state.runtime) != 0 ||
        state.runtime->active_shards != 6U || state.runtime->dynamic_online_floor != 4U ||
        llam_async_preempt_enable_runtime(state.runtime) != 0 ||
        llam_spawn_opts_init(&spawn, sizeof(spawn)) != 0) {
        goto cleanup;
    }
    phase = 1U;

    state.setup = llam_runtime_spawn_ex(state.runtime, dynamic_prepare_channels,
        &state, NULL, 0U);
    if (state.setup == NULL) goto cleanup;

    /* A stale pause cleanup must not clear a later scheduler operation. */
    {
        uint64_t stale = llam_shard_request_merge_pause(&state.runtime->shards[5]);
        uint64_t current = llam_shard_request_merge_pause(&state.runtime->shards[5]);
        if (stale == 0U || current == 0U || stale == current ||
            llam_shard_release_merge_pause_if_sequence(&state.runtime->shards[5], stale) ||
            !llam_shard_merge_pause_requested(&state.runtime->shards[5]) ||
            !llam_shard_release_merge_pause_if_sequence(&state.runtime->shards[5], current) ||
            llam_shard_merge_pause_requested(&state.runtime->shards[5])) {
            goto cleanup;
        }
    }

    spawn.flags = LLAM_SPAWN_F_PINNED;
    /* Explicitly start the two high-id primaries before targeted admission. */
    for (i = 4U; i < 6U; ++i) {
        llam_shard_t *shard = &state.runtime->shards[i];
        pthread_mutex_lock(&shard->lock);
        bool changed = llam_processor_try_set_online(&shard->processor, true);
        if (changed) {
            llam_runtime_note_online_shards(state.runtime,
                atomic_fetch_add_explicit(&state.runtime->online_shards, 1U, memory_order_acq_rel) + 1U);
        }
        pthread_mutex_unlock(&shard->lock);
        if (changed) llam_kick_shard(shard);
    }
    for (i = 0U; i < 6U; ++i) {
        atomic_store_explicit(&state.runtime->next_spawn_shard, i, memory_order_release);
        g_dynamic_wait_items[i] = (dynamic_wait_item_t){&state, i};
        state.waiters[i] = llam_runtime_spawn_ex(state.runtime, dynamic_wait_task,
            &g_dynamic_wait_items[i], &spawn, sizeof(spawn));
        if (state.waiters[i] == NULL) goto cleanup;
    }
    if (pthread_create(&driver, NULL, dynamic_run_driver, &state) != 0) {
        goto cleanup;
    }
    driver_started = true;
    phase = 2U;
    deadline = llam_now_ns() + UINT64_C(6000000000);
    if (!wait_count(&state.waiters_started, 6U, deadline)) goto cleanup;
    {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        while (!dynamic_waiters_parked((const llam_channel_t *const *)state.channels) &&
               llam_now_ns() < deadline) {
            (void)nanosleep(&delay, NULL);
        }
        if (!dynamic_waiters_parked((const llam_channel_t *const *)state.channels)) goto cleanup;
    }
    for (i = 0U; i < 6U; ++i) {
        if (!llam_processor_online(&state.runtime->shards[i].processor)) goto cleanup;
    }
    for (i = 0U; i < 4U; ++i) {
        if (llam_channel_close(state.channels[i]) != 0) {
            fprintf(stderr, "dynamic idle test: channel %u close failed errno=%d owner=%p runtime=%p\\n",
                    i, errno, (void *)state.channels[i]->owner_runtime, (void *)state.runtime);
            goto cleanup;
        }
    }
    phase = 3U;
    deadline = llam_now_ns() + UINT64_C(6000000000);
    if (!wait_count(&state.waiters_done, 4U, deadline)) goto cleanup;
    while (llam_runtime_online_shards(state.runtime) > state.runtime->dynamic_online_floor &&
           llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    if (llam_runtime_online_shards(state.runtime) != 4U ||
        !llam_processor_online(&state.runtime->shards[4].processor) ||
        !llam_processor_online(&state.runtime->shards[5].processor)) {
        fprintf(stderr, "dynamic idle test: online=%u expected floor=4 with bound M4/M5 online\n",
                llam_runtime_online_shards(state.runtime));
        goto cleanup;
    }
    phase = 4U;
    if (state.setup != NULL && llam_join(state.setup) != 0) goto cleanup;
    state.setup = NULL;
    for (i = 4U; i < 6U; ++i) {
        llam_async_preempt_state_t *mstate = (llam_async_preempt_state_t *)atomic_load_explicit(
            &state.runtime->shards[i].primary_worker.async_preempt_state, memory_order_acquire);
        if (mstate == NULL || atomic_load_explicit(&mstate->affine_live_g, memory_order_acquire) != 1U) {
            goto cleanup;
        }
    }

    /* Load P0 until the normal scaler reopens a worker; then route work there. */
    phase = 5U;
    atomic_store_explicit(&state.runtime->next_spawn_shard, 0U, memory_order_release);
    for (i = 0U; i < 8U; ++i) {
        atomic_store_explicit(&state.runtime->next_spawn_shard, 0U, memory_order_release);
        g_dynamic_pressure_items[i].state = &state;
        atomic_init(&g_dynamic_pressure_items[i].release, 0U);
        state.pressure[i] = llam_runtime_spawn_ex(state.runtime, dynamic_pressure_task,
            &g_dynamic_pressure_items[i], &spawn, sizeof(spawn));
        if (state.pressure[i] == NULL) {
            fprintf(stderr, "dynamic idle test: pressure spawn %u failed: errno=%d online=%u stop=%u\n",
                    i, errno, llam_runtime_online_shards(state.runtime),
                    atomic_load_explicit(&state.runtime->stop_requested, memory_order_acquire));
            goto cleanup;
        }
    }
    deadline = llam_now_ns() + UINT64_C(6000000000);
    while (llam_runtime_online_shards(state.runtime) < 5U && llam_now_ns() < deadline) {
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
    }
    {
        llam_async_preempt_state_t *reactivated = (llam_async_preempt_state_t *)atomic_load_explicit(
            &state.runtime->shards[2].primary_worker.async_preempt_state, memory_order_acquire);
        if (llam_runtime_online_shards(state.runtime) < 5U ||
            !wait_count(&state.pressure_started, 1U, deadline) || reactivated == NULL ||
            atomic_load_explicit(&reactivated->frame_probe_status, memory_order_acquire) != 2U) {
            goto cleanup;
        }
    }
    phase = 6U;
    atomic_store_explicit(&state.runtime->next_spawn_shard, 2U, memory_order_release);
    {
        llam_task_t *probe = llam_runtime_spawn_ex(state.runtime, dynamic_route_probe,
            &state, &spawn, sizeof(spawn));
        if (probe == NULL || !wait_count(&state.route_done, 1U, deadline) || llam_join(probe) != 0) {
            goto cleanup;
        }
    }
    phase = 7U;
    for (i = 0U; i < 8U; ++i) {
        atomic_store_explicit(&g_dynamic_pressure_items[i].release, 1U, memory_order_release);
    }
    for (i = 4U; i < 6U; ++i) (void)llam_channel_close(state.channels[i]);
    deadline = llam_now_ns() + UINT64_C(6000000000);
    if (!wait_count(&state.pressure_done, 8U, deadline) ||
        !wait_count(&state.waiters_done, 6U, deadline)) {
        goto cleanup;
    }
    if (driver_started) {
        (void)pthread_join(driver, NULL);
        driver_started = false;
        if (state.run_rc != 0) goto cleanup;
    }
    phase = 8U;
    for (i = 0U; i < 6U; ++i) {
        if (state.waiters[i] != NULL && llam_join(state.waiters[i]) != 0) goto cleanup;
        state.waiters[i] = NULL;
    }
    for (i = 0U; i < 8U; ++i) {
        if (state.pressure[i] != NULL && llam_join(state.pressure[i]) != 0) goto cleanup;
        state.pressure[i] = NULL;
    }
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U) goto cleanup;
    phase = 9U;
    result = 0;
    puts("test_async_preempt_multi_linux_x86_64: dynamic idle-only offlining preserved bound M4/M5, respected floor and reopened M2 under load");
cleanup:
    if (result != 0 && state.runtime != NULL) {
        fprintf(stderr,
                "dynamic idle test failed: phase=%u active=%u floor=%u online=%u waiters=%u/%u channels=%u pressure=%u/%u route=%u run_rc=%d errno=%d failures=%u/%u/%u/%u/%u/%u\n",
                phase,
                state.runtime->active_shards,
                state.runtime->dynamic_online_floor,
                llam_runtime_online_shards(state.runtime),
                atomic_load_explicit(&state.waiters_started, memory_order_acquire),
                atomic_load_explicit(&state.waiters_done, memory_order_acquire),
                atomic_load_explicit(&state.channels_ready, memory_order_acquire),
                atomic_load_explicit(&state.pressure_started, memory_order_acquire),
                atomic_load_explicit(&state.pressure_done, memory_order_acquire),
                atomic_load_explicit(&state.route_done, memory_order_acquire),
                state.run_rc,
                errno,
                atomic_load_explicit(&state.setup_failures, memory_order_acquire),
                atomic_load_explicit(&state.affinity_failures, memory_order_acquire),
                atomic_load_explicit(&state.channel_failures, memory_order_acquire),
                atomic_load_explicit(&state.origin_failures, memory_order_acquire),
                atomic_load_explicit(&state.pressure_failures, memory_order_acquire),
                atomic_load_explicit(&state.route_failures, memory_order_acquire));
    }
    for (i = 0U; i < 6U; ++i) if (state.channels[i] != NULL) (void)llam_channel_close(state.channels[i]);
    for (i = 0U; i < 8U; ++i) {
        atomic_store_explicit(&g_dynamic_pressure_items[i].release, 1U, memory_order_release);
    }
    if (driver_started) (void)pthread_join(driver, NULL);
    for (i = 0U; i < 6U; ++i) if (state.channels[i] != NULL) (void)llam_channel_destroy(state.channels[i]);
    if (state.runtime != NULL) llam_runtime_destroy(state.runtime);
    if (affinity_set) (void)sched_setaffinity(0, sizeof(previous), &previous);
    return result;
}

int main(void) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    int rc = run_multi(2U);
    if (rc == 0) {
        rc = run_multi(4U);
    }
    if (rc == 0) {
        rc = test_startup_rollback();
    }
    if (rc == 0) {
        rc = test_capsule_budget();
    }
    for (unsigned mode = 1U; mode <= 3U && rc == 0; ++mode) {
        rc = test_block_slots(mode);
    }
    if (rc == 0) {
        rc = test_stop_service(false);
    }
    if (rc == 0) {
        rc = test_stop_service(true);
    }
    if (rc == 0) {
        rc = test_startup_close();
    }
    if (rc == 0) {
        rc = test_invalid_resource_config();
    }
    if (rc == 0) {
        rc = test_dynamic_idle_domains();
    }
    return rc;
}
#endif
