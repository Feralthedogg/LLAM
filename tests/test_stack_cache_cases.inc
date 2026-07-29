/**
 * @file tests/test_stack_cache_cases.inc
 * @brief Runtime-wide stack-cache trim and memory-pressure contracts.
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

#include "runtime_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define fail_msg stack_cache_fail_msg
#define fail_errno stack_cache_fail_errno

typedef struct stack_cache_fixture {
    llam_runtime_t *runtime;
    size_t stack_size;
    uint64_t mapping_bytes;
    uint64_t budget_bytes;
} stack_cache_fixture_t;

static int fail_msg(const char *message) {
    fprintf(stderr, "test_stack_cache: %s\n", message);
    return 1;
}

static int fail_errno(const char *message) {
    fprintf(stderr,
            "test_stack_cache: %s: errno=%d (%s)\n",
            message,
            errno,
            strerror(errno));
    return 1;
}

#include "test_stack_cache_burst_metrics.inc"

static int fixture_init(stack_cache_fixture_t *fixture,
                        uint64_t budget_mappings,
                        uint64_t high_mappings,
                        uint64_t low_mappings,
                        uint64_t idle_ns) {
    llam_runtime_opts_t opts;
    size_t page_size = (size_t)llam_page_size();

    memset(fixture, 0, sizeof(*fixture));
    fixture->stack_size = llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT);
    fixture->mapping_bytes = (uint64_t)fixture->stack_size + page_size;
    fixture->budget_bytes = budget_mappings * fixture->mapping_bytes;
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("runtime options initialization failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 1U;
    opts.blocking_max = 1U;
    opts.task_prewarm_total = 1U;
    opts.stack_prewarm_total = 1U;
    opts.timer_prewarm_total = 1U;
    opts.stack_cache_budget_bytes = fixture->budget_bytes;
    opts.stack_cache_high_watermark_bytes =
        high_mappings * fixture->mapping_bytes;
    opts.stack_cache_low_watermark_bytes =
        low_mappings * fixture->mapping_bytes;
    opts.stack_cache_idle_ns = idle_ns;
    if (llam_runtime_create(&opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &fixture->runtime) != 0) {
        return fail_errno("runtime creation failed");
    }
    return 0;
}

static void fixture_destroy(stack_cache_fixture_t *fixture) {
    if (fixture == NULL) {
        return;
    }
    llam_runtime_destroy(fixture->runtime);
    fixture->runtime = NULL;
}

static int fixture_stats(const stack_cache_fixture_t *fixture,
                         llam_runtime_stats_t *stats) {
    if (llam_runtime_collect_stats_ex_handle(fixture->runtime,
                                             stats,
                                             sizeof(*stats)) != 0) {
        return fail_errno("stack-cache statistics collection failed");
    }
    return 0;
}

static int fixture_add_mapping(stack_cache_fixture_t *fixture,
                               llam_shard_t *preferred_shard) {
    void *mapping = NULL;
    void *stack_base = NULL;
    size_t mapping_size = 0U;

    if (llam_stack_vm_map(fixture->stack_size,
                          &mapping,
                          &mapping_size,
                          &stack_base) != 0) {
        return fail_errno("stack mapping allocation failed");
    }
    if (mapping_size != fixture->mapping_bytes ||
        !llam_stack_cache_return_mapping(fixture->runtime,
                                         preferred_shard,
                                         mapping,
                                         mapping_size,
                                         stack_base,
                                         fixture->stack_size)) {
        return fail_msg("stack mapping was not accepted by the cache");
    }
    return 0;
}

static int test_manual_trim_and_pressure(void) {
    stack_cache_fixture_t fixture;
    llam_runtime_stats_t before;
    llam_runtime_stats_t after;
    uint64_t released = UINT64_MAX;
    int rc = 1;

    errno = 0;
    if (llam_runtime_stack_cache_trim_ex(NULL, 0U, &released) != -1 ||
        errno != EINVAL || released != 0U) {
        return fail_msg("trim accepted a NULL runtime");
    }
    errno = 0;
    if (llam_runtime_stack_cache_trim_ex(
            (llam_runtime_t *)(uintptr_t)0x1234U,
            0U,
            &released) != -1 ||
        errno != EINVAL || released != 0U ||
        llam_runtime_stack_cache_trim_ex(
            (llam_runtime_t *)(uintptr_t)0x1234U,
            0U,
            NULL) != -1 ||
        errno != EINVAL ||
        llam_runtime_notify_memory_pressure(NULL) != -1 ||
        errno != EINVAL) {
        return fail_msg("trim accepted an invalid handle or output");
    }
    if (fixture_init(&fixture,
                     12U,
                     12U,
                     4U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    for (unsigned index = 0U; index < 5U; ++index) {
        llam_shard_t *preferred =
            (index & 1U) != 0U ? &fixture.runtime->shards[0] : NULL;

        if (fixture_add_mapping(&fixture, preferred) != 0) {
            goto cleanup;
        }
    }
    if (fixture_stats(&fixture, &before) != 0 ||
        before.stack_cache_cached_mappings != 6U ||
        before.stack_cache_cached_bytes !=
            6U * fixture.mapping_bytes) {
        rc = fail_msg("manual-trim fixture cache was not exact");
        goto cleanup;
    }
    released = UINT64_MAX;
    if (llam_runtime_stack_cache_trim_ex(
            fixture.runtime,
            3U * fixture.mapping_bytes,
            &released) != 0 ||
        fixture_stats(&fixture, &after) != 0) {
        rc = fail_errno("manual stack-cache trim failed");
        goto cleanup;
    }
    if (after.stack_cache_cached_mappings != 3U ||
        after.stack_cache_cached_bytes !=
            3U * fixture.mapping_bytes ||
        released != 3U * fixture.mapping_bytes ||
        after.stack_cache_released_bytes -
                before.stack_cache_released_bytes !=
            released ||
        after.stack_cache_trim_requests !=
            before.stack_cache_trim_requests + 1U) {
        rc = fail_msg("manual trim did not reach and account its target");
        goto cleanup;
    }
    released = UINT64_MAX;
    errno = 0;
    if (llam_runtime_stack_cache_trim_ex(
            fixture.runtime,
            fixture.budget_bytes + fixture.mapping_bytes,
            &released) != -1 ||
        errno != EINVAL || released != 0U) {
        rc = fail_msg("manual trim accepted a target above its budget");
        goto cleanup;
    }
    before = after;
    if (llam_runtime_notify_memory_pressure(fixture.runtime) != 0 ||
        fixture_stats(&fixture, &after) != 0) {
        rc = fail_errno("memory-pressure trim failed");
        goto cleanup;
    }
    if (after.stack_cache_cached_mappings != 0U ||
        after.stack_cache_cached_bytes != 0U ||
        after.stack_cache_committed_bytes != 0U ||
        after.stack_cache_released_bytes -
                before.stack_cache_released_bytes !=
            before.stack_cache_cached_bytes ||
        after.stack_cache_trim_requests !=
            before.stack_cache_trim_requests + 1U) {
        rc = fail_msg("memory pressure did not release the whole cache");
        goto cleanup;
    }
    rc = 0;

cleanup:
    fixture_destroy(&fixture);
    return rc;
}

static int test_high_low_hysteresis(void) {
    stack_cache_fixture_t fixture;
    llam_runtime_stats_t stats;
    uint64_t first_trim_requests;
    int rc = 1;

    if (fixture_init(&fixture,
                     10U,
                     4U,
                     2U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    for (unsigned index = 0U; index < 3U; ++index) {
        if (fixture_add_mapping(&fixture, &fixture.runtime->shards[0]) != 0) {
            goto cleanup;
        }
    }
    if (fixture_stats(&fixture, &stats) != 0 ||
        stats.stack_cache_cached_mappings != 4U ||
        stats.stack_cache_trim_requests != 0U) {
        rc = fail_msg("high watermark trimmed before it was crossed");
        goto cleanup;
    }
    if (fixture_add_mapping(&fixture, NULL) != 0 ||
        fixture_stats(&fixture, &stats) != 0 ||
        stats.stack_cache_cached_mappings > 2U ||
        stats.stack_cache_cached_bytes >
            2U * fixture.mapping_bytes ||
        stats.stack_cache_trim_requests != 1U) {
        rc = fail_msg("high-water crossing did not trim toward low");
        goto cleanup;
    }
    first_trim_requests = stats.stack_cache_trim_requests;
    for (unsigned index = 0U; index < 3U; ++index) {
        if (fixture_add_mapping(&fixture, NULL) != 0) {
            goto cleanup;
        }
    }
    if (fixture_stats(&fixture, &stats) != 0 ||
        stats.stack_cache_cached_mappings > 2U ||
        stats.stack_cache_trim_requests != first_trim_requests + 1U) {
        rc = fail_msg("high/low hysteresis did not re-arm");
        goto cleanup;
    }
    rc = 0;

cleanup:
    fixture_destroy(&fixture);
    return rc;
}

static void mark_all_cached_mappings_old(llam_runtime_t *runtime) {
    llam_stack_cache_entry_t *entry;

    pthread_mutex_lock(&runtime->shards[0].stack_cache_lock);
    for (entry = runtime->shards[0].stack_cache_default;
         entry != NULL;
         entry = entry->next) {
        entry->last_return_ns = 1U;
    }
    pthread_mutex_unlock(&runtime->shards[0].stack_cache_lock);

    pthread_mutex_lock(&runtime->stack_cache_lock);
    for (entry = runtime->stack_cache_default;
         entry != NULL;
         entry = entry->next) {
        entry->last_return_ns = 1U;
    }
    pthread_mutex_unlock(&runtime->stack_cache_lock);
}

static int test_idle_trim_selects_only_old_entries(void) {
    stack_cache_fixture_t fixture;
    llam_runtime_stats_t before;
    llam_runtime_stats_t after;
    int rc = 1;

    if (fixture_init(&fixture,
                     10U,
                     10U,
                     2U,
                     UINT64_C(1000000000)) != 0) {
        return 1;
    }
    if (fixture_add_mapping(&fixture, NULL) != 0 ||
        fixture_add_mapping(&fixture, &fixture.runtime->shards[0]) != 0 ||
        fixture_stats(&fixture, &before) != 0) {
        goto cleanup;
    }
    mark_all_cached_mappings_old(fixture.runtime);
    atomic_store_explicit(&fixture.runtime->stack_cache_last_idle_scan_ns,
                          1U,
                          memory_order_release);
    if (fixture_add_mapping(&fixture, NULL) != 0 ||
        fixture_stats(&fixture, &after) != 0) {
        goto cleanup;
    }
    if (before.stack_cache_cached_mappings != 3U ||
        after.stack_cache_cached_mappings != 1U ||
        after.stack_cache_cached_bytes != fixture.mapping_bytes ||
        after.stack_cache_trim_requests !=
            before.stack_cache_trim_requests + 1U) {
        rc = fail_msg("idle trim did not preserve the newly returned mapping");
        goto cleanup;
    }
    rc = 0;

cleanup:
    fixture_destroy(&fixture);
    return rc;
}

#include "test_stack_cache_accounting_cases.inc"

typedef struct stack_vm_lock_observer {
    llam_runtime_t *runtime;
    atomic_uint calls;
    atomic_uint lock_failures;
} stack_vm_lock_observer_t;

static void observe_stack_vm_release(
    llam_test_stack_vm_operation_t operation,
    void *context) {
    stack_vm_lock_observer_t *observer = context;

    if (operation != LLAM_TEST_STACK_VM_RELEASE || observer == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&observer->calls, 1U, memory_order_relaxed);
    if (pthread_mutex_trylock(&observer->runtime->stack_cache_lock) != 0) {
        atomic_fetch_add_explicit(&observer->lock_failures,
                                  1U,
                                  memory_order_relaxed);
    } else {
        pthread_mutex_unlock(&observer->runtime->stack_cache_lock);
    }
    for (unsigned shard_id = 0U;
         shard_id < observer->runtime->active_shards;
         ++shard_id) {
        if (pthread_mutex_trylock(
                &observer->runtime->shards[shard_id].stack_cache_lock) != 0) {
            atomic_fetch_add_explicit(&observer->lock_failures,
                                      1U,
                                      memory_order_relaxed);
        } else {
            pthread_mutex_unlock(
                &observer->runtime->shards[shard_id].stack_cache_lock);
        }
    }
}

static int test_trim_releases_vm_outside_cache_locks(void) {
    stack_cache_fixture_t fixture;
    stack_vm_lock_observer_t observer;
    uint64_t released = 0U;
    int rc = 1;

    if (fixture_init(&fixture,
                     10U,
                     10U,
                     2U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    if (fixture_add_mapping(&fixture, NULL) != 0 ||
        fixture_add_mapping(&fixture, &fixture.runtime->shards[0]) != 0) {
        goto cleanup;
    }
    memset(&observer, 0, sizeof(observer));
    observer.runtime = fixture.runtime;
    atomic_init(&observer.calls, 0U);
    atomic_init(&observer.lock_failures, 0U);
    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_observer(observe_stack_vm_release,
                                            &observer);
    if (llam_runtime_stack_cache_trim_ex(fixture.runtime,
                                         0U,
                                         &released) != 0) {
        rc = fail_errno("VM-outside-lock trim failed");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
    if (released != 3U * fixture.mapping_bytes ||
        atomic_load_explicit(&observer.calls, memory_order_acquire) != 3U ||
        atomic_load_explicit(&observer.lock_failures,
                             memory_order_acquire) != 0U) {
        rc = fail_msg("trim performed a VM release while a cache lock was held");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_test_reset_stack_vm_hooks();
    fixture_destroy(&fixture);
    return rc;
}

typedef struct stack_cache_concurrency {
    stack_cache_fixture_t *fixture;
    atomic_uint failures;
    unsigned iterations;
} stack_cache_concurrency_t;

static void *cache_churn_main(void *arg) {
    stack_cache_concurrency_t *state = arg;
    stack_cache_fixture_t *fixture = state->fixture;

    for (unsigned iteration = 0U;
         iteration < state->iterations;
         ++iteration) {
        void *mapping = NULL;
        void *stack_base = NULL;
        size_t mapping_size = 0U;

        if (!llam_stack_cache_pop_mapping(fixture->runtime,
                                          NULL,
                                          fixture->stack_size,
                                          &mapping,
                                          &mapping_size,
                                          &stack_base) &&
            llam_stack_vm_map(fixture->stack_size,
                              &mapping,
                              &mapping_size,
                              &stack_base) != 0) {
            atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed);
            continue;
        }
        if (!llam_stack_cache_return_mapping(fixture->runtime,
                                             NULL,
                                             mapping,
                                             mapping_size,
                                             stack_base,
                                             fixture->stack_size)) {
            atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed);
        }
    }
    return NULL;
}

static void *cache_trim_main(void *arg) {
    stack_cache_concurrency_t *state = arg;

    unsigned trim_iterations = state->iterations / 2U;

    if (trim_iterations == 0U) {
        trim_iterations = 1U;
    }
    for (unsigned iteration = 0U;
         iteration < trim_iterations;
         ++iteration) {
        uint64_t released = 0U;

        if (llam_runtime_stack_cache_trim_ex(
                state->fixture->runtime,
                2U * state->fixture->mapping_bytes,
                &released) != 0) {
            atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed);
        }
    }
    return NULL;
}

static void *cache_stats_main(void *arg) {
    stack_cache_concurrency_t *state = arg;

    for (unsigned iteration = 0U;
         iteration < state->iterations;
         ++iteration) {
        llam_runtime_stats_t stats;

        if (llam_runtime_collect_stats_ex_handle(state->fixture->runtime,
                                                 &stats,
                                                 sizeof(stats)) != 0 ||
            stats.stack_cache_cached_bytes >
                state->fixture->budget_bytes) {
            atomic_fetch_add_explicit(&state->failures,
                                      1U,
                                      memory_order_relaxed);
        }
    }
    return NULL;
}

static int test_concurrent_trim_churn_stats_and_shutdown(void) {
    stack_cache_fixture_t fixture;
    stack_cache_concurrency_t state;
    llam_runtime_stats_t stats;
    pthread_t churn_thread;
    pthread_t trim_thread;
    pthread_t stats_thread;
    bool churn_started = false;
    bool trim_started = false;
    bool stats_started = false;
    int rc = 1;

    if (fixture_init(&fixture,
                     16U,
                     16U,
                     4U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    state.fixture = &fixture;
    atomic_init(&state.failures, 0U);
    state.iterations = stack_cache_test_iterations();
    if (pthread_create(&churn_thread, NULL, cache_churn_main, &state) != 0) {
        rc = fail_errno("cache churn thread creation failed");
        goto cleanup;
    }
    churn_started = true;
    if (pthread_create(&trim_thread, NULL, cache_trim_main, &state) != 0) {
        rc = fail_errno("cache trim thread creation failed");
        goto cleanup;
    }
    trim_started = true;
    if (pthread_create(&stats_thread, NULL, cache_stats_main, &state) != 0) {
        rc = fail_errno("cache stats thread creation failed");
        goto cleanup;
    }
    stats_started = true;
    if (pthread_join(churn_thread, NULL) != 0 ||
        pthread_join(trim_thread, NULL) != 0 ||
        pthread_join(stats_thread, NULL) != 0) {
        return fail_msg("concurrent stack-cache thread join failed");
    }
    churn_started = false;
    trim_started = false;
    stats_started = false;
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        llam_runtime_notify_memory_pressure(fixture.runtime) != 0 ||
        fixture_stats(&fixture, &stats) != 0 ||
        stats.stack_cache_cached_bytes != 0U ||
        stats.stack_cache_cached_mappings != 0U ||
        stats.stack_cache_committed_bytes != 0U ||
        atomic_load_explicit(&fixture.runtime->fatal_errno,
                             memory_order_acquire) != 0) {
        rc = fail_msg("concurrent trim/churn/stats authority diverged");
        goto cleanup;
    }
    if (stack_cache_test_report_long_burst(state.iterations, &stats) != 0) {
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (churn_started) {
        (void)pthread_join(churn_thread, NULL);
    }
    if (trim_started) {
        (void)pthread_join(trim_thread, NULL);
    }
    if (stats_started) {
        (void)pthread_join(stats_thread, NULL);
    }
    fixture_destroy(&fixture);
    return rc;
}

typedef struct stack_cache_blocking_observer {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    bool entered;
    bool release;
    atomic_uint claimed;
} stack_cache_blocking_observer_t;

typedef struct stack_cache_trim_destroy_call {
    llam_runtime_t *runtime;
    atomic_uint started;
    atomic_uint done;
    int result;
    int error_code;
} stack_cache_trim_destroy_call_t;

static void observe_and_block_first_stack_vm_release(
    llam_test_stack_vm_operation_t operation,
    void *context) {
    stack_cache_blocking_observer_t *observer = context;

    if (operation != LLAM_TEST_STACK_VM_RELEASE || observer == NULL ||
        atomic_exchange_explicit(&observer->claimed,
                                 1U,
                                 memory_order_acq_rel) != 0U) {
        return;
    }
    pthread_mutex_lock(&observer->lock);
    observer->entered = true;
    pthread_cond_broadcast(&observer->cv);
    while (!observer->release) {
        pthread_cond_wait(&observer->cv, &observer->lock);
    }
    pthread_mutex_unlock(&observer->lock);
}

static void *blocking_trim_main(void *arg) {
    stack_cache_trim_destroy_call_t *call = arg;
    uint64_t released = 0U;

    atomic_store_explicit(&call->started, 1U, memory_order_release);
    call->result = llam_runtime_stack_cache_trim_ex(call->runtime,
                                                    0U,
                                                    &released);
    call->error_code = call->result != 0 ? errno : 0;
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void *concurrent_destroy_main(void *arg) {
    stack_cache_trim_destroy_call_t *call = arg;

    atomic_store_explicit(&call->started, 1U, memory_order_release);
    llam_runtime_destroy(call->runtime);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static int test_destroy_waits_for_active_trim(void) {
    stack_cache_fixture_t fixture;
    stack_cache_blocking_observer_t observer;
    stack_cache_trim_destroy_call_t trim_call;
    stack_cache_trim_destroy_call_t destroy_call;
    pthread_t trim_thread;
    pthread_t destroy_thread;
    bool trim_started = false;
    bool destroy_started = false;
    int rc = 1;

    if (fixture_init(&fixture,
                     10U,
                     10U,
                     2U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    if (fixture_add_mapping(&fixture, NULL) != 0 ||
        fixture_add_mapping(&fixture, &fixture.runtime->shards[0]) != 0) {
        goto cleanup;
    }
    memset(&observer, 0, sizeof(observer));
    if (pthread_mutex_init(&observer.lock, NULL) != 0 ||
        pthread_cond_init(&observer.cv, NULL) != 0) {
        rc = fail_msg("blocking VM observer initialization failed");
        goto cleanup;
    }
    atomic_init(&observer.claimed, 0U);
    memset(&trim_call, 0, sizeof(trim_call));
    trim_call.runtime = fixture.runtime;
    atomic_init(&trim_call.started, 0U);
    atomic_init(&trim_call.done, 0U);
    memset(&destroy_call, 0, sizeof(destroy_call));
    destroy_call.runtime = fixture.runtime;
    atomic_init(&destroy_call.started, 0U);
    atomic_init(&destroy_call.done, 0U);

    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_observer(
        observe_and_block_first_stack_vm_release,
        &observer);
    if (pthread_create(&trim_thread,
                       NULL,
                       blocking_trim_main,
                       &trim_call) != 0) {
        rc = fail_errno("blocking trim thread creation failed");
        goto cleanup_observer;
    }
    trim_started = true;
    pthread_mutex_lock(&observer.lock);
    while (!observer.entered) {
        pthread_cond_wait(&observer.cv, &observer.lock);
    }
    pthread_mutex_unlock(&observer.lock);
    if (pthread_create(&destroy_thread,
                       NULL,
                       concurrent_destroy_main,
                       &destroy_call) != 0) {
        rc = fail_errno("concurrent destroy thread creation failed");
        goto release_observer;
    }
    destroy_started = true;
    while (atomic_load_explicit(&destroy_call.started,
                                memory_order_acquire) == 0U) {
        llam_pause_cpu();
    }
    for (unsigned spin = 0U; spin < 10000U; ++spin) {
        if (atomic_load_explicit(&destroy_call.done,
                                 memory_order_acquire) != 0U) {
            break;
        }
        llam_pause_cpu();
    }
    if (atomic_load_explicit(&destroy_call.done,
                             memory_order_acquire) != 0U) {
        rc = fail_msg("runtime destroy bypassed an active trim pin");
        goto release_observer;
    }
    rc = 0;

release_observer:
    pthread_mutex_lock(&observer.lock);
    observer.release = true;
    pthread_cond_broadcast(&observer.cv);
    pthread_mutex_unlock(&observer.lock);
    if (trim_started) {
        (void)pthread_join(trim_thread, NULL);
        trim_started = false;
    }
    if (destroy_started) {
        (void)pthread_join(destroy_thread, NULL);
        destroy_started = false;
        fixture.runtime = NULL;
    }
    if (rc == 0 &&
        (trim_call.result != 0 ||
         atomic_load_explicit(&trim_call.done, memory_order_acquire) == 0U ||
         atomic_load_explicit(&destroy_call.done, memory_order_acquire) == 0U)) {
        rc = fail_msg("trim/destroy race did not finish cleanly");
    }

cleanup_observer:
    llam_runtime_test_reset_stack_vm_hooks();
    (void)pthread_cond_destroy(&observer.cv);
    (void)pthread_mutex_destroy(&observer.lock);
cleanup:
    if (trim_started || destroy_started) {
        return fail_msg("trim/destroy cleanup left a thread unjoined");
    }
    fixture_destroy(&fixture);
    return rc;
}

#include "test_stack_cache_failure_cases.inc"

static int llam_test_stack_cache_run(void) {
    if (test_manual_trim_and_pressure() != 0 ||
        test_high_low_hysteresis() != 0 ||
        test_idle_trim_selects_only_old_entries() != 0 ||
        test_controller_releases_quiescent_idle_entries() != 0 ||
        test_stats_wait_for_coherent_accounting() != 0 ||
        test_trim_releases_vm_outside_cache_locks() != 0 ||
        test_concurrent_trim_churn_stats_and_shutdown() != 0 ||
        test_trim_release_failure_preserves_ownership() != 0 ||
        test_pop_failure_preserves_release_ownership() != 0 ||
        test_uncached_release_failure_quarantines_mapping() != 0 ||
        test_shutdown_release_failure_quarantines_mapping() != 0 ||
        test_destroy_waits_for_active_trim() != 0) {
        return 1;
    }
    return 0;
}

#undef fail_errno
#undef fail_msg
