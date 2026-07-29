// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file tests/test_multi_runtime_stack_cache.inc
 * @brief Cross-runtime retained-stack ownership regression.
 */

typedef struct cross_stack_cache_state {
    llam_runtime_t *owner_runtime;
    llam_runtime_t *foreign_runtime;
    llam_task_t *foreign_task;
    atomic_uint failures;
    atomic_uint foreign_ran;
    unsigned owner_cached_before_spawn;
    unsigned owner_cached_after_spawn;
    int first_errno;
} cross_stack_cache_state_t;

static unsigned runtime_default_stack_cache_count(llam_runtime_t *runtime) {
    unsigned total = 0U;

    if (runtime == NULL || !runtime->stack_cache_lock_initialized) {
        return 0U;
    }
    pthread_mutex_lock(&runtime->stack_cache_lock);
    total = runtime->stack_cache_default_count;
    pthread_mutex_unlock(&runtime->stack_cache_lock);
    for (unsigned i = 0U; i < runtime->active_shards; ++i) {
        llam_shard_t *shard = &runtime->shards[i];

        pthread_mutex_lock(&shard->stack_cache_lock);
        total += shard->stack_cache_default_count;
        pthread_mutex_unlock(&shard->stack_cache_lock);
    }
    return total;
}

static void cross_stack_cache_spawn_task(void *arg) {
    cross_stack_cache_state_t *state = arg;

    state->owner_cached_before_spawn =
        runtime_default_stack_cache_count(state->owner_runtime);
    state->foreign_task = llam_runtime_spawn_ex(state->foreign_runtime,
                                                foreign_target_task,
                                                &state->foreign_ran,
                                                NULL,
                                                0U);
    state->first_errno = errno;
    state->owner_cached_after_spawn =
        runtime_default_stack_cache_count(state->owner_runtime);
    if (state->foreign_task == NULL ||
        state->owner_cached_before_spawn == 0U ||
        state->owner_cached_after_spawn != state->owner_cached_before_spawn) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static int test_cross_runtime_stack_cache_uses_task_owner(void) {
    llam_runtime_opts_t opts_a;
    llam_runtime_opts_t opts_b;
    llam_runtime_t *runtime_a = NULL;
    llam_runtime_t *runtime_b = NULL;
    llam_task_t *owner_task = NULL;
    cross_stack_cache_state_t state;
    unsigned initial_a;
    unsigned final_a;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.failures, 0U);
    atomic_init(&state.foreign_ran, 0U);
    if (init_runtime_opts(&opts_a) != 0 || init_runtime_opts(&opts_b) != 0) {
        return test_fail_errno("cross-stack-cache opts init failed");
    }
    opts_a.stack_prewarm_total = 2U;
    if (llam_runtime_create(&opts_a, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_a) != 0 ||
        llam_runtime_create(&opts_b, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime_b) != 0) {
        rc = test_fail_errno("cross-stack-cache runtime create failed");
        goto cleanup;
    }
    initial_a = runtime_default_stack_cache_count(runtime_a);
    if (initial_a != 2U) {
        rc = test_fail("cross-stack-cache fixture did not retain two owner mappings");
        goto cleanup;
    }

    state.owner_runtime = runtime_a;
    state.foreign_runtime = runtime_b;
    owner_task = llam_runtime_spawn_ex(runtime_a,
                                       cross_stack_cache_spawn_task,
                                       &state,
                                       NULL,
                                       0U);
    if (owner_task == NULL ||
        llam_runtime_run_handle(runtime_a) != 0 ||
        llam_join(owner_task) != 0) {
        owner_task = NULL;
        rc = test_fail_errno("cross-stack-cache owner task failed");
        goto cleanup;
    }
    owner_task = NULL;
    if (state.foreign_task == NULL ||
        llam_runtime_run_handle(runtime_b) != 0 ||
        llam_join(state.foreign_task) != 0) {
        state.foreign_task = NULL;
        rc = test_fail_errno("cross-stack-cache foreign task failed");
        goto cleanup;
    }
    state.foreign_task = NULL;
    final_a = runtime_default_stack_cache_count(runtime_a);
    if (atomic_load_explicit(&state.failures, memory_order_relaxed) != 0U ||
        atomic_load_explicit(&state.foreign_ran, memory_order_relaxed) != 1U ||
        state.owner_cached_before_spawn + 1U != initial_a ||
        state.owner_cached_after_spawn != state.owner_cached_before_spawn ||
        final_a != initial_a) {
        fprintf(stderr,
                "[test_multi_runtime_core] cross-stack-cache counts: "
                "initial=%u before=%u after=%u final=%u failures=%u\n",
                initial_a,
                state.owner_cached_before_spawn,
                state.owner_cached_after_spawn,
                final_a,
                atomic_load_explicit(&state.failures, memory_order_relaxed));
        errno = state.first_errno;
        rc = test_fail_errno("foreign spawn borrowed or returned an owner cache mapping");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (state.foreign_task != NULL) {
        (void)llam_detach(state.foreign_task);
    }
    if (owner_task != NULL) {
        (void)llam_detach(owner_task);
    }
    llam_runtime_destroy(runtime_b);
    llam_runtime_destroy(runtime_a);
    return rc;
}
