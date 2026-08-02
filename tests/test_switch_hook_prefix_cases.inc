/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

static void switch_hook_dump_events(const switch_hook_case_state_t *state) {
    unsigned count =
        atomic_load_explicit(&state->event_count, memory_order_acquire);
    unsigned i;

    fprintf(stderr,
            "switch-hook diagnostics: events=%u failures=%u direct=%u migrated=%u\n",
            count,
            atomic_load_explicit(&state->failures, memory_order_acquire),
            atomic_load_explicit(&state->direct_handoff_succeeded,
                                 memory_order_acquire),
            atomic_load_explicit(&state->migration_succeeded,
                                 memory_order_acquire));
    for (i = 0U; i < count && i < SWITCH_HOOK_MAX_EVENTS; ++i) {
        fprintf(stderr,
                "  event[%u] kind=%u task=%u errno=%d shard=%u context=%p\n",
                i,
                (unsigned)state->events[i].kind,
                state->events[i].task_id,
                state->events[i].observed_errno,
                state->events[i].shard_id,
                state->events[i].user_context);
    }
}

static bool switch_hook_task_event_matches(
    const switch_hook_case_state_t *state,
    unsigned task_id,
    unsigned task_event_index,
    switch_hook_event_kind_t kind,
    switch_hook_task_context_t *context,
    int observed_errno) {
    unsigned event_count =
        atomic_load_explicit(&state->event_count, memory_order_acquire);
    unsigned matched = 0U;

    for (unsigned i = 0U;
         i < event_count && i < SWITCH_HOOK_MAX_EVENTS;
         ++i) {
        if (state->events[i].task_id != task_id) {
            continue;
        }
        if (matched == task_event_index) {
            return switch_hook_event_matches(&state->events[i],
                                             kind,
                                             context,
                                             observed_errno);
        }
        matched += 1U;
    }
    return false;
}

static bool switch_hook_migration_tail_matches(
    const switch_hook_case_state_t *state,
    switch_hook_task_context_t *migration_context,
    switch_hook_task_context_t *mover_context) {
    return switch_hook_task_event_matches(
               state,
               migration_context->task_id,
               3U,
               SWITCH_HOOK_EVENT_RESUME,
               migration_context,
               EDOM) &&
           switch_hook_task_event_matches(
               state,
               migration_context->task_id,
               4U,
               SWITCH_HOOK_EVENT_BODY,
               migration_context,
               EDOM) &&
           switch_hook_task_event_matches(
               state,
               migration_context->task_id,
               5U,
               SWITCH_HOOK_EVENT_SUSPEND,
               migration_context,
               EDOM) &&
           switch_hook_task_event_matches(
               state,
               mover_context->task_id,
               2U,
               SWITCH_HOOK_EVENT_SUSPEND,
               mover_context,
               E2BIG);
}

static void switch_hook_serial_main_task(void *arg) {
    switch_hook_task_context_t *context = arg;
    switch_hook_case_state_t *state = context->state;

    if (errno != 0 || g_llam_tls_shard == NULL ||
        g_llam_tls_shard->id != 0U) {
        switch_hook_case_fail(state);
    }
    switch_hook_case_body(context);
    atomic_store_explicit(&state->runtime->steal_pause_active,
                          0U,
                          memory_order_release);
    llam_wake_all_shards(state->runtime);
    errno = EDOM;
    llam_yield();
    /*
     * A compiler may retain the source worker's C TLS address across this
     * stackful migration. Switch hooks resolve TLS at each boundary, so avoid
     * reading or assigning raw errno again in this migrated C frame.
     */
    if (atomic_load_explicit(&state->suspend_hold_started,
                             memory_order_acquire) != 1U) {
        switch_hook_case_fail(state);
    }
    switch_hook_case_body(context);
}

static void switch_hook_serial_peer_task(void *arg) {
    switch_hook_task_context_t *context = arg;
    switch_hook_case_state_t *state = context->state;
    uint64_t deadline_ns;

    if (errno != 0) {
        switch_hook_case_fail(state);
    }
    switch_hook_case_body(context);
    llam_wake_all_shards(state->runtime);
    deadline_ns = llam_now_ns() + UINT64_C(10000000);
    while (llam_now_ns() < deadline_ns) {
        atomic_signal_fence(memory_order_seq_cst);
    }
    errno = ERANGE;
}

static int test_switch_hook_same_task_serialization(void) {
    switch_hook_case_state_t state;
    switch_hook_task_context_t contexts[SWITCH_HOOK_SERIAL_PEERS + 1U];
    llam_task_t *handles[SWITCH_HOOK_SERIAL_PEERS + 1U] = {NULL};
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *raw_runtime = NULL;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_count;
    unsigned i;
    int rc = 1;

    allowed_count = llam_count_allowed_cpus(&allowed_cpus);
    free(allowed_cpus);
    if (allowed_count < 2U) {
        fprintf(stderr,
                "skip same-task switch-hook serialization: fewer than two allowed CPUs\n");
        return 0;
    }

    switch_hook_case_init(&state);
    for (i = 0U; i <= SWITCH_HOOK_SERIAL_PEERS; ++i) {
        switch_hook_task_context_init(&contexts[i], &state, i + 1U);
    }
    atomic_store_explicit(&state.suspend_hold_task_id,
                          contexts[0].task_id,
                          memory_order_release);
    state.suspend_hold_ns = UINT64_C(50000000);
    if (llam_runtime_opts_init(&runtime_opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts,
                             LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "same-task switch-hook option initialization failed");
    }
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    runtime_opts.worker_min = 2U;
    runtime_opts.worker_count = 2U;
    runtime_opts.worker_max = 2U;
    runtime_opts.on_task_resume = switch_hook_case_resume;
    runtime_opts.on_task_suspend = switch_hook_case_suspend;
    runtime_opts.switch_hook_context = &state;
    if (llam_runtime_create(&runtime_opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "same-task switch-hook runtime creation failed");
    }
    if (llam_runtime_begin_public_op(runtime, &raw_runtime) != 0) {
        (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
            "same-task switch-hook runtime pin failed");
        goto out;
    }
    state.runtime = raw_runtime;
    atomic_store_explicit(&raw_runtime->steal_pause_active,
                          1U,
                          memory_order_release);
    for (i = 0U; i <= SWITCH_HOOK_SERIAL_PEERS; ++i) {
        atomic_store_explicit(&raw_runtime->next_spawn_shard,
                              0U,
                              memory_order_relaxed);
        spawn_opts.user_context = &contexts[i];
        handles[i] = llam_runtime_spawn_ex(
            runtime,
            i == 0U ? switch_hook_serial_main_task
                    : switch_hook_serial_peer_task,
            &contexts[i],
            &spawn_opts,
            LLAM_SPAWN_OPTS_CURRENT_SIZE);
        if (handles[i] == NULL) {
            (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
                "same-task switch-hook task spawn failed");
            goto out;
        }
    }
    if (llam_runtime_run_handle(runtime) != 0) {
        (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
            "same-task switch-hook runtime failed");
        goto out;
    }
    for (i = 0U; i <= SWITCH_HOOK_SERIAL_PEERS; ++i) {
        if (llam_join(handles[i]) != 0) {
            (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
                "same-task switch-hook join failed");
            goto out;
        }
        handles[i] = NULL;
    }
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.event_count,
                             memory_order_acquire) !=
            6U + SWITCH_HOOK_SERIAL_PEERS * 3U ||
        atomic_load_explicit(&contexts[0].resume_count,
                             memory_order_acquire) != 2U ||
        contexts[0].resume_shards[0] != 0U ||
        contexts[0].resume_shards[1] != 1U ||
        atomic_load_explicit(&contexts[0].hook_active,
                             memory_order_acquire) != 0U) {
        switch_hook_dump_events(&state);
        (void)SWITCH_HOOK_TEST_FAIL(
            "same task resumed before its suspend hook completed");
        goto out;
    }
    for (i = 1U; i <= SWITCH_HOOK_SERIAL_PEERS; ++i) {
        if (atomic_load_explicit(&contexts[i].resume_count,
                                 memory_order_acquire) != 1U ||
            atomic_load_explicit(&contexts[i].hook_active,
                                 memory_order_acquire) != 0U) {
            (void)SWITCH_HOOK_TEST_FAIL(
                "peer switch-hook lifecycle was not exact");
            goto out;
        }
    }
    rc = 0;

out:
    for (i = 0U; i <= SWITCH_HOOK_SERIAL_PEERS; ++i) {
        if (handles[i] != NULL) {
            (void)llam_detach(handles[i]);
        }
    }
    if (raw_runtime != NULL) {
        llam_runtime_end_public_op(raw_runtime);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static int switch_hook_run_quiet_runtime(llam_runtime_opts_t *opts,
                                         size_t opts_size,
                                         switch_hook_case_state_t *state) {
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int rc = -1;

    if (llam_runtime_create(opts, opts_size, &runtime) != 0) {
        return -1;
    }
    task = llam_runtime_spawn_ex(runtime,
                                 switch_hook_quiet_task,
                                 state,
                                 NULL,
                                 0U);
    if (task != NULL &&
        llam_runtime_run_handle(runtime) == 0 &&
        llam_join(task) == 0) {
        task = NULL;
        rc = 0;
    }
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static int test_switch_hook_null_and_legacy_prefix(void) {
    switch_hook_case_state_t state;
    llam_runtime_opts_t opts;
    size_t legacy_size = offsetof(llam_runtime_opts_t, on_task_resume);

    switch_hook_case_init(&state);
    atomic_store_explicit(&switch_hook_poison_calls,
                          0U,
                          memory_order_release);
    if (llam_runtime_opts_init(&opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook prefix option initialization failed");
    }
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    if (switch_hook_run_quiet_runtime(&opts,
                                      LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                                      &state) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "null switch-hook runtime failed");
    }

    opts.on_task_resume = switch_hook_case_poison;
    opts.on_task_suspend = switch_hook_case_poison;
    opts.switch_hook_context = &state;
    if (switch_hook_run_quiet_runtime(&opts, legacy_size, &state) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "legacy switch-hook prefix runtime failed");
    }
    if (atomic_load_explicit(&state.quiet_task_runs,
                             memory_order_acquire) != 2U ||
        atomic_load_explicit(&switch_hook_poison_calls,
                             memory_order_acquire) != 0U) {
        return SWITCH_HOOK_TEST_FAIL(
            "null callbacks or legacy prefix consumed switch-hook poison");
    }
    return 0;
}

static int exercise_switch_hook_cases(void) {
    if (test_switch_hook_single_task_contract() != 0 ||
        test_switch_hook_direct_handoff_and_migration() != 0 ||
        test_switch_hook_concurrent_tasks() != 0 ||
        test_switch_hook_same_task_serialization() != 0 ||
        test_switch_hook_null_and_legacy_prefix() != 0) {
        return 1;
    }
    return 0;
}
