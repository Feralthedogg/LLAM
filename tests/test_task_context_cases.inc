/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

typedef struct task_context_case_state {
    void *expected_user_context;
    void *expected_slots[LLAM_TASK_CONTEXT_SLOT_COUNT];
    unsigned expected_shard;
    atomic_uint failures;
    atomic_uint runs;
} task_context_case_state_t;

typedef struct task_context_legacy_spawn_opts {
    uint32_t task_class;
    uint32_t stack_class;
    uint32_t flags;
    uint32_t reserved0;
    uint64_t deadline_ns;
    llam_cancel_token_t *cancel_token;
} task_context_legacy_spawn_opts_t;

static void task_context_record_failure(task_context_case_state_t *state) {
    atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
}

static void task_context_full_contract_task(void *arg) {
    task_context_case_state_t *state = arg;
    unsigned i;

    errno = EDOM;
    if (llam_task_user_context() != state->expected_user_context ||
        errno != EDOM) {
        task_context_record_failure(state);
    }
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        errno = ERANGE;
        if (llam_task_context_slot_get(i) != NULL || errno != ERANGE) {
            task_context_record_failure(state);
        }
        if (llam_task_context_slot_set(i, state->expected_slots[i]) != 0) {
            task_context_record_failure(state);
        }
    }
    errno = 0;
    if (llam_task_context_slot_get(LLAM_TASK_CONTEXT_SLOT_COUNT) != NULL ||
        errno != EINVAL) {
        task_context_record_failure(state);
    }
    errno = 0;
    if (llam_task_context_slot_set(LLAM_TASK_CONTEXT_SLOT_COUNT,
                                   state) != -1 ||
        errno != EINVAL) {
        task_context_record_failure(state);
    }
    llam_yield();
    if (llam_sleep_ns(1000000ULL) != 0) {
        task_context_record_failure(state);
    }
    errno = EILSEQ;
    if (llam_task_user_context() != state->expected_user_context ||
        errno != EILSEQ) {
        task_context_record_failure(state);
    }
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        errno = ENOTTY;
        if (llam_task_context_slot_get(i) != state->expected_slots[i] ||
            errno != ENOTTY) {
            task_context_record_failure(state);
        }
    }
    atomic_fetch_add_explicit(&state->runs, 1U, memory_order_release);
}

static void task_context_expect_clean_reuse_task(void *arg) {
    task_context_case_state_t *state = arg;
    unsigned i;

    if (llam_task_user_context() != state->expected_user_context) {
        task_context_record_failure(state);
    }
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        if (llam_task_context_slot_get(i) != NULL) {
            task_context_record_failure(state);
        }
    }
    atomic_fetch_add_explicit(&state->runs, 1U, memory_order_release);
}

static void task_context_forced_migration_task(void *arg) {
    task_context_case_state_t *state = arg;
    unsigned i;

    if (g_llam_tls_shard == NULL ||
        g_llam_tls_shard->id != state->expected_shard ||
        llam_task_user_context() != state->expected_user_context) {
        task_context_record_failure(state);
    }
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        if (llam_task_context_slot_get(i) != state->expected_slots[i]) {
            task_context_record_failure(state);
        }
    }
    atomic_fetch_add_explicit(&state->runs, 1U, memory_order_release);
}

static int test_task_context_unmanaged_contract(void) {
    int value;

    errno = 0;
    if (llam_task_user_context() != NULL || errno != ENOTSUP) {
        return test_fail("unmanaged task user context did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_context_slot_get(0U) != NULL || errno != ENOTSUP) {
        return test_fail("unmanaged task context get did not fail with ENOTSUP");
    }
    errno = 0;
    if (llam_task_context_slot_set(0U, &value) != -1 || errno != ENOTSUP) {
        return test_fail("unmanaged task context set did not fail with ENOTSUP");
    }
    return 0;
}

static int test_task_context_lifecycle_and_prefix(void) {
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *first = NULL;
    llam_task_t *second = NULL;
    llam_task_t *legacy = NULL;
    task_context_case_state_t first_state;
    task_context_case_state_t second_state;
    task_context_case_state_t legacy_state;
    struct {
        task_context_legacy_spawn_opts_t known;
        void *poison_user_context;
    } legacy_opts;
    int rc = 1;
    unsigned i;

    _Static_assert(sizeof(task_context_legacy_spawn_opts_t) ==
                       offsetof(llam_spawn_opts_t, user_context),
                   "legacy spawn prefix must end before user_context");
    memset(&first_state, 0, sizeof(first_state));
    memset(&second_state, 0, sizeof(second_state));
    memset(&legacy_state, 0, sizeof(legacy_state));
    first_state.expected_user_context = &first_state;
    second_state.expected_user_context = &second_state;
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        first_state.expected_slots[i] =
            (void *)(uintptr_t)(UINT64_C(0x1000) + i * UINT64_C(0x10));
    }
    atomic_init(&first_state.failures, 0U);
    atomic_init(&first_state.runs, 0U);
    atomic_init(&second_state.failures, 0U);
    atomic_init(&second_state.runs, 0U);
    atomic_init(&legacy_state.failures, 0U);
    atomic_init(&legacy_state.runs, 0U);

    if (llam_runtime_opts_init(&runtime_opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts,
                             LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("task context option initialization failed");
    }
    runtime_opts.worker_min = 1U;
    runtime_opts.worker_count = 1U;
    runtime_opts.worker_max = 1U;
    if (llam_runtime_create(&runtime_opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return test_fail_errno("task context runtime creation failed");
    }

    spawn_opts.user_context = first_state.expected_user_context;
    first = llam_runtime_spawn_ex(runtime,
                                  task_context_full_contract_task,
                                  &first_state,
                                  &spawn_opts,
                                  LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (first == NULL) {
        (void)test_fail_errno("task context first spawn failed");
        goto out;
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        (void)test_fail_errno("task context first runtime drive failed");
        goto out;
    }
    if (llam_join(first) != 0) {
        (void)test_fail_errno("task context first join failed");
        goto out;
    }
    first = NULL;
    if (atomic_load_explicit(&first_state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&first_state.runs, memory_order_acquire) != 1U) {
        (void)test_fail("task context did not survive yield and park");
        goto out;
    }

    spawn_opts.user_context = second_state.expected_user_context;
    second = llam_runtime_spawn_ex(runtime,
                                   task_context_expect_clean_reuse_task,
                                   &second_state,
                                   &spawn_opts,
                                   LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (second == NULL) {
        (void)test_fail_errno("task context recycled spawn failed");
        goto out;
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        (void)test_fail_errno("task context recycled runtime drive failed");
        goto out;
    }
    if (llam_join(second) != 0) {
        (void)test_fail_errno("task context recycled join failed");
        goto out;
    }
    second = NULL;
    if (atomic_load_explicit(&second_state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&second_state.runs, memory_order_acquire) != 1U) {
        (void)test_fail("recycled task retained prior inline context");
        goto out;
    }

    memset(&legacy_opts, 0, sizeof(legacy_opts));
    legacy_opts.known.task_class = LLAM_TASK_CLASS_DEFAULT;
    legacy_opts.known.stack_class = LLAM_STACK_CLASS_DEFAULT;
    legacy_opts.poison_user_context = &legacy_opts;
    legacy = llam_runtime_spawn_ex(
        runtime,
        task_context_expect_clean_reuse_task,
        &legacy_state,
        (const llam_spawn_opts_t *)(const void *)&legacy_opts,
        sizeof(legacy_opts.known));
    if (legacy == NULL) {
        (void)test_fail_errno("legacy task context prefix spawn failed");
        goto out;
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        (void)test_fail_errno("legacy task context prefix runtime drive failed");
        goto out;
    }
    if (llam_join(legacy) != 0) {
        (void)test_fail_errno("legacy task context prefix join failed");
        goto out;
    }
    legacy = NULL;
    if (atomic_load_explicit(&legacy_state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&legacy_state.runs, memory_order_acquire) != 1U) {
        (void)test_fail("legacy spawn prefix consumed user-context poison tail");
        goto out;
    }
    rc = 0;

out:
    if (first != NULL) {
        (void)llam_detach(first);
    }
    if (second != NULL) {
        (void)llam_detach(second);
    }
    if (legacy != NULL) {
        (void)llam_detach(legacy);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static int test_task_context_survives_forced_queue_migration(void) {
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *raw_runtime = NULL;
    llam_task_t *handle = NULL;
    llam_task_t *task = NULL;
    task_context_case_state_t state;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_count;
    unsigned source_id;
    unsigned target_id;
    unsigned i;
    int rc = 1;

    allowed_count = llam_count_allowed_cpus(&allowed_cpus);
    free(allowed_cpus);
    if (allowed_count < 2U) {
        fprintf(stderr,
                "[test_runtime_core] skip task context forced migration: fewer than two allowed CPUs\n");
        return 0;
    }

    memset(&state, 0, sizeof(state));
    state.expected_user_context = &state;
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        state.expected_slots[i] =
            (void *)(uintptr_t)(UINT64_C(0x2000) + i * UINT64_C(0x10));
    }
    atomic_init(&state.failures, 0U);
    atomic_init(&state.runs, 0U);
    if (llam_runtime_opts_init(&runtime_opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts,
                             LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("forced migration option initialization failed");
    }
    runtime_opts.worker_min = 2U;
    runtime_opts.worker_count = 2U;
    runtime_opts.worker_max = 2U;
    spawn_opts.user_context = state.expected_user_context;
    if (llam_runtime_create(&runtime_opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return test_fail_errno("forced migration runtime creation failed");
    }
    if (llam_runtime_begin_public_op(runtime, &raw_runtime) != 0) {
        (void)test_fail_errno("forced migration runtime pin failed");
        goto out;
    }
    handle = llam_runtime_spawn_ex(runtime,
                                   task_context_forced_migration_task,
                                   &state,
                                   &spawn_opts,
                                   LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (handle == NULL) {
        (void)test_fail_errno("forced migration task spawn failed");
        goto out;
    }
    task = llam_task_resolve_public_handle(handle);
    if (task == NULL) {
        (void)test_fail("forced migration task handle did not resolve");
        goto out;
    }
    source_id = task->home_shard;
    target_id = (source_id + 1U) % raw_runtime->active_shards;
    state.expected_shard = target_id;
    for (i = 0U; i < LLAM_TASK_CONTEXT_SLOT_COUNT; ++i) {
        task->context_slots[i] = state.expected_slots[i];
    }
    pthread_mutex_lock(&raw_runtime->shards[source_id].lock);
    if (llam_norm_queue_pop_owner_locked(&raw_runtime->shards[source_id]) != task) {
        pthread_mutex_unlock(&raw_runtime->shards[source_id].lock);
        llam_task_end_public_op(task);
        task = NULL;
        (void)test_fail("forced migration source queue did not contain task");
        goto out;
    }
    pthread_mutex_unlock(&raw_runtime->shards[source_id].lock);
    pthread_mutex_lock(&raw_runtime->shards[target_id].lock);
    if (!llam_norm_queue_push_owner_locked(&raw_runtime->shards[target_id], task)) {
        pthread_mutex_unlock(&raw_runtime->shards[target_id].lock);
        llam_task_end_public_op(task);
        task = NULL;
        (void)test_fail("forced migration target queue rejected task");
        goto out;
    }
    pthread_mutex_unlock(&raw_runtime->shards[target_id].lock);
    llam_task_end_public_op(task);
    task = NULL;

    if (llam_runtime_run_handle(runtime) != 0 || llam_join(handle) != 0) {
        (void)test_fail_errno("forced migration task lifecycle failed");
        goto out;
    }
    handle = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.runs, memory_order_acquire) != 1U) {
        (void)test_fail("task context did not follow forced queue migration");
        goto out;
    }
    rc = 0;

out:
    if (task != NULL) {
        llam_task_end_public_op(task);
    }
    if (handle != NULL) {
        (void)llam_detach(handle);
    }
    if (raw_runtime != NULL) {
        llam_runtime_end_public_op(raw_runtime);
    }
    llam_runtime_destroy(runtime);
    return rc;
}
