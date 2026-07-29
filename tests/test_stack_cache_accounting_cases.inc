/**
 * @file tests/test_stack_cache_accounting_cases.inc
 * @brief Time-driven trim and coherent stack-cache accounting contracts.
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

static int test_controller_releases_quiescent_idle_entries(void) {
    stack_cache_fixture_t fixture;
    llam_runtime_stats_t stats;
    struct timespec interval = {0, 1000000L};
    uint64_t deadline_ns;
    int rc = 1;

    if (fixture_init(&fixture,
                     10U,
                     10U,
                     2U,
                     UINT64_C(5000000)) != 0) {
        return 1;
    }
    mark_all_cached_mappings_old(fixture.runtime);
    atomic_store_explicit(&fixture.runtime->stack_cache_last_idle_scan_ns,
                          1U,
                          memory_order_release);
    deadline_ns = llam_now_ns() + UINT64_C(2000000000);
    do {
        if (fixture_stats(&fixture, &stats) != 0) {
            goto cleanup;
        }
        if (stats.stack_cache_cached_mappings == 0U) {
            break;
        }
        (void)nanosleep(&interval, NULL);
    } while (llam_now_ns() < deadline_ns);
    if (stats.stack_cache_cached_mappings != 0U ||
        stats.stack_cache_cached_bytes != 0U ||
        stats.stack_cache_trim_requests == 0U) {
        rc = fail_msg(
            "controller did not trim an idle cache without new returns");
        goto cleanup;
    }
    rc = 0;

cleanup:
    fixture_destroy(&fixture);
    return rc;
}

typedef struct stack_cache_account_observer {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    bool entered;
    bool release;
} stack_cache_account_observer_t;

typedef struct stack_cache_stats_call {
    stack_cache_fixture_t *fixture;
    llam_runtime_stats_t stats;
    atomic_uint started;
    atomic_uint done;
    int result;
} stack_cache_stats_call_t;

typedef struct stack_cache_add_call {
    stack_cache_fixture_t *fixture;
    atomic_uint done;
    int result;
} stack_cache_add_call_t;

static void observe_and_block_stack_cache_account(void *context) {
    stack_cache_account_observer_t *observer = context;

    if (observer == NULL) {
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

static void *collect_stack_cache_stats_main(void *arg) {
    stack_cache_stats_call_t *call = arg;

    atomic_store_explicit(&call->started, 1U, memory_order_release);
    call->result = fixture_stats(call->fixture, &call->stats);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static void *add_stack_cache_mapping_main(void *arg) {
    stack_cache_add_call_t *call = arg;

    call->result = fixture_add_mapping(call->fixture, NULL);
    atomic_store_explicit(&call->done, 1U, memory_order_release);
    return NULL;
}

static int test_stats_wait_for_coherent_accounting(void) {
    stack_cache_fixture_t fixture;
    stack_cache_account_observer_t observer;
    stack_cache_stats_call_t stats_call;
    stack_cache_add_call_t add_call;
    struct timespec interval = {0, 10000000L};
    pthread_t stats_thread;
    pthread_t add_thread;
    bool observer_initialized = false;
    bool stats_started = false;
    bool add_started = false;
    bool stats_completed_early = false;
    int rc = 1;

    if (fixture_init(&fixture,
                     10U,
                     10U,
                     2U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    memset(&observer, 0, sizeof(observer));
    if (pthread_mutex_init(&observer.lock, NULL) != 0 ||
        pthread_cond_init(&observer.cv, NULL) != 0) {
        rc = fail_msg("stack-cache account observer initialization failed");
        goto cleanup;
    }
    observer_initialized = true;
    memset(&stats_call, 0, sizeof(stats_call));
    stats_call.fixture = &fixture;
    atomic_init(&stats_call.started, 0U);
    atomic_init(&stats_call.done, 0U);
    memset(&add_call, 0, sizeof(add_call));
    add_call.fixture = &fixture;
    atomic_init(&add_call.done, 0U);

    llam_runtime_test_reset_stack_cache_account_hook();
    llam_runtime_test_set_stack_cache_account_observer(
        observe_and_block_stack_cache_account,
        &observer);
    if (pthread_create(&add_thread,
                       NULL,
                       add_stack_cache_mapping_main,
                       &add_call) != 0) {
        rc = fail_errno("stack-cache account writer creation failed");
        goto cleanup_observer;
    }
    add_started = true;
    pthread_mutex_lock(&observer.lock);
    while (!observer.entered) {
        pthread_cond_wait(&observer.cv, &observer.lock);
    }
    pthread_mutex_unlock(&observer.lock);
    if (pthread_create(&stats_thread,
                       NULL,
                       collect_stack_cache_stats_main,
                       &stats_call) != 0) {
        rc = fail_errno("stack-cache stats reader creation failed");
        goto release_observer;
    }
    stats_started = true;
    while (atomic_load_explicit(&stats_call.started,
                                memory_order_acquire) == 0U) {
        llam_pause_cpu();
    }
    (void)nanosleep(&interval, NULL);
    stats_completed_early =
        atomic_load_explicit(&stats_call.done, memory_order_acquire) != 0U;

release_observer:
    pthread_mutex_lock(&observer.lock);
    observer.release = true;
    pthread_cond_broadcast(&observer.cv);
    pthread_mutex_unlock(&observer.lock);
    if (add_started) {
        (void)pthread_join(add_thread, NULL);
        add_started = false;
    }
    if (stats_started) {
        (void)pthread_join(stats_thread, NULL);
        stats_started = false;
    }
    if (stats_completed_early ||
        add_call.result != 0 ||
        stats_call.result != 0 ||
        stats_call.stats.stack_cache_cached_mappings != 2U ||
        stats_call.stats.stack_cache_cached_bytes !=
            2U * fixture.mapping_bytes ||
        stats_call.stats.stack_cache_committed_bytes !=
            2U * fixture.stack_size) {
        rc = fail_msg(
            "stats exposed a partially published stack-cache account");
        goto cleanup_observer;
    }
    rc = 0;

cleanup_observer:
    llam_runtime_test_reset_stack_cache_account_hook();
    if (observer_initialized) {
        (void)pthread_cond_destroy(&observer.cv);
        (void)pthread_mutex_destroy(&observer.lock);
    }
cleanup:
    if (add_started || stats_started) {
        return fail_msg("stack-cache account test left a thread unjoined");
    }
    fixture_destroy(&fixture);
    return rc;
}
