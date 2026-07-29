/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SWITCH_HOOK_TEST_FAIL
#error "define SWITCH_HOOK_TEST_FAIL before including switch-hook cases"
#endif
#ifndef SWITCH_HOOK_TEST_FAIL_ERRNO
#error "define SWITCH_HOOK_TEST_FAIL_ERRNO before including switch-hook cases"
#endif

#define SWITCH_HOOK_MAX_EVENTS 64U
#define SWITCH_HOOK_MAX_RESUMES 4U
#define SWITCH_HOOK_SERIAL_PEERS 8U

typedef enum switch_hook_event_kind {
    SWITCH_HOOK_EVENT_RESUME = 1,
    SWITCH_HOOK_EVENT_BODY = 2,
    SWITCH_HOOK_EVENT_SUSPEND = 3,
} switch_hook_event_kind_t;

typedef struct switch_hook_event {
    switch_hook_event_kind_t kind;
    void *user_context;
    int observed_errno;
    unsigned task_id;
    unsigned shard_id;
} switch_hook_event_t;

typedef struct switch_hook_case_state switch_hook_case_state_t;

typedef struct switch_hook_task_context {
    switch_hook_case_state_t *state;
    unsigned task_id;
    atomic_uint hook_active;
    atomic_uint resume_count;
    unsigned resume_shards[SWITCH_HOOK_MAX_RESUMES];
} switch_hook_task_context_t;

struct switch_hook_case_state {
    switch_hook_event_t events[SWITCH_HOOK_MAX_EVENTS];
    atomic_uint event_count;
    atomic_uint failures;
    atomic_uint hooks_active;
    atomic_uint hooks_active_peak;
    atomic_uint first_resume_barrier_expected;
    atomic_uint first_resume_barrier_arrived;
    atomic_uint direct_handoff_succeeded;
    atomic_uint migration_succeeded;
    atomic_uint quiet_task_runs;
    atomic_uint suspend_hold_task_id;
    atomic_uint suspend_hold_started;
    uint64_t suspend_hold_ns;
    llam_runtime_t *runtime;
    llam_task_t *migration_task;
    unsigned source_shard;
    unsigned target_shard;
};

static atomic_uint switch_hook_poison_calls;

static void switch_hook_case_fail(switch_hook_case_state_t *state) {
    if (state != NULL) {
        atomic_fetch_add_explicit(&state->failures, 1U, memory_order_relaxed);
    }
}

static void switch_hook_case_update_peak(atomic_uint *peak, unsigned value) {
    unsigned observed = atomic_load_explicit(peak, memory_order_relaxed);

    while (observed < value &&
           !atomic_compare_exchange_weak_explicit(peak,
                                                  &observed,
                                                  value,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
    }
}

static void switch_hook_case_record(switch_hook_case_state_t *state,
                                    switch_hook_task_context_t *task_context,
                                    switch_hook_event_kind_t kind,
                                    int observed_errno) {
    unsigned index;

    if (state == NULL || task_context == NULL) {
        switch_hook_case_fail(state);
        return;
    }
    index = atomic_fetch_add_explicit(&state->event_count, 1U, memory_order_relaxed);
    if (index >= SWITCH_HOOK_MAX_EVENTS) {
        switch_hook_case_fail(state);
        return;
    }
    state->events[index].kind = kind;
    state->events[index].user_context = task_context;
    state->events[index].observed_errno = observed_errno;
    state->events[index].task_id = task_context->task_id;
    state->events[index].shard_id =
        g_llam_tls_shard != NULL ? g_llam_tls_shard->id : UINT_MAX;
}

static void switch_hook_case_callback(switch_hook_event_kind_t kind,
                                      void *hook_context,
                                      void *task_user_context) {
    switch_hook_case_state_t *state = hook_context;
    switch_hook_task_context_t *task_context = task_user_context;
    unsigned active;

    if (state == NULL || task_context == NULL ||
        task_context->state != state) {
        switch_hook_case_fail(state);
        return;
    }
    if (atomic_exchange_explicit(&task_context->hook_active,
                                 1U,
                                 memory_order_acq_rel) != 0U) {
        switch_hook_case_fail(state);
    }
    active =
        atomic_fetch_add_explicit(&state->hooks_active,
                                  1U,
                                  memory_order_acq_rel) +
        1U;
    switch_hook_case_update_peak(&state->hooks_active_peak, active);
    switch_hook_case_record(state, task_context, kind, errno);

    if (kind == SWITCH_HOOK_EVENT_RESUME) {
        unsigned resume_index =
            atomic_fetch_add_explicit(&task_context->resume_count,
                                      1U,
                                      memory_order_relaxed);
        unsigned expected =
            atomic_load_explicit(&state->first_resume_barrier_expected,
                                 memory_order_acquire);

        if (resume_index < SWITCH_HOOK_MAX_RESUMES) {
            task_context->resume_shards[resume_index] =
                g_llam_tls_shard != NULL ? g_llam_tls_shard->id : UINT_MAX;
        } else {
            switch_hook_case_fail(state);
        }
        if (resume_index == 0U && expected != 0U) {
            uint64_t deadline_ns = llam_now_ns() + UINT64_C(2000000000);

            atomic_fetch_add_explicit(&state->first_resume_barrier_arrived,
                                      1U,
                                      memory_order_acq_rel);
            while (atomic_load_explicit(&state->first_resume_barrier_arrived,
                                        memory_order_acquire) < expected) {
                if (llam_now_ns() >= deadline_ns) {
                    switch_hook_case_fail(state);
                    break;
                }
                atomic_signal_fence(memory_order_seq_cst);
            }
        }
        errno = EACCES;
    } else {
        unsigned hold_task_id =
            atomic_load_explicit(&state->suspend_hold_task_id,
                                 memory_order_acquire);

        errno = EBUSY;
        if (hold_task_id == task_context->task_id &&
            atomic_exchange_explicit(&state->suspend_hold_started,
                                     1U,
                                     memory_order_acq_rel) == 0U) {
            uint64_t deadline_ns = llam_now_ns() + state->suspend_hold_ns;

            while (llam_now_ns() < deadline_ns) {
                atomic_signal_fence(memory_order_seq_cst);
            }
        }
    }

    atomic_fetch_sub_explicit(&state->hooks_active, 1U, memory_order_acq_rel);
    atomic_store_explicit(&task_context->hook_active, 0U, memory_order_release);
}

static void switch_hook_case_resume(void *hook_context,
                                    void *task_user_context) {
    switch_hook_case_callback(SWITCH_HOOK_EVENT_RESUME,
                              hook_context,
                              task_user_context);
}

static void switch_hook_case_suspend(void *hook_context,
                                     void *task_user_context) {
    switch_hook_case_callback(SWITCH_HOOK_EVENT_SUSPEND,
                              hook_context,
                              task_user_context);
}

static void switch_hook_case_poison(void *hook_context,
                                    void *task_user_context) {
    (void)hook_context;
    (void)task_user_context;
    atomic_fetch_add_explicit(&switch_hook_poison_calls,
                              1U,
                              memory_order_relaxed);
}

static void switch_hook_case_init(switch_hook_case_state_t *state) {
    memset(state, 0, sizeof(*state));
    atomic_init(&state->event_count, 0U);
    atomic_init(&state->failures, 0U);
    atomic_init(&state->hooks_active, 0U);
    atomic_init(&state->hooks_active_peak, 0U);
    atomic_init(&state->first_resume_barrier_expected, 0U);
    atomic_init(&state->first_resume_barrier_arrived, 0U);
    atomic_init(&state->direct_handoff_succeeded, 0U);
    atomic_init(&state->migration_succeeded, 0U);
    atomic_init(&state->quiet_task_runs, 0U);
    atomic_init(&state->suspend_hold_task_id, 0U);
    atomic_init(&state->suspend_hold_started, 0U);
}

static void switch_hook_task_context_init(switch_hook_task_context_t *context,
                                          switch_hook_case_state_t *state,
                                          unsigned task_id) {
    unsigned i;

    memset(context, 0, sizeof(*context));
    context->state = state;
    context->task_id = task_id;
    atomic_init(&context->hook_active, 0U);
    atomic_init(&context->resume_count, 0U);
    for (i = 0U; i < SWITCH_HOOK_MAX_RESUMES; ++i) {
        context->resume_shards[i] = UINT_MAX;
    }
}

static void switch_hook_case_body(switch_hook_task_context_t *context) {
    int logical_errno = errno;

    switch_hook_case_record(context->state,
                            context,
                            SWITCH_HOOK_EVENT_BODY,
                            logical_errno);
    errno = logical_errno;
}

static void switch_hook_single_task(void *arg) {
    switch_hook_task_context_t *context = arg;

    if (errno != 0) {
        switch_hook_case_fail(context->state);
    }
    switch_hook_case_body(context);
    errno = EDOM;
    llam_yield();
    if (errno != EDOM) {
        switch_hook_case_fail(context->state);
    }
    switch_hook_case_body(context);
    errno = ERANGE;
}

static void switch_hook_quiet_task(void *arg) {
    switch_hook_case_state_t *state = arg;

    atomic_fetch_add_explicit(&state->quiet_task_runs,
                              1U,
                              memory_order_release);
}

static bool switch_hook_event_matches(const switch_hook_event_t *event,
                                      switch_hook_event_kind_t kind,
                                      switch_hook_task_context_t *context,
                                      int observed_errno) {
    return event != NULL &&
           event->kind == kind &&
           event->user_context == context &&
           event->task_id == context->task_id &&
           event->observed_errno == observed_errno;
}

static int test_switch_hook_single_task_contract(void) {
    switch_hook_case_state_t state;
    switch_hook_task_context_t context;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    unsigned before_destroy;
    int rc = 1;

    switch_hook_case_init(&state);
    switch_hook_task_context_init(&context, &state, 1U);
    if (llam_runtime_opts_init(&runtime_opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts,
                             LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook single-task option initialization failed");
    }
    runtime_opts.worker_min = 1U;
    runtime_opts.worker_count = 1U;
    runtime_opts.worker_max = 1U;
    runtime_opts.on_task_resume = switch_hook_case_resume;
    runtime_opts.on_task_suspend = switch_hook_case_suspend;
    runtime_opts.switch_hook_context = &state;
    spawn_opts.user_context = &context;
    if (llam_runtime_create(&runtime_opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook single-task runtime creation failed");
    }
    task = llam_runtime_spawn_ex(runtime,
                                 switch_hook_single_task,
                                 &context,
                                 &spawn_opts,
                                 LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (task == NULL ||
        llam_runtime_run_handle(runtime) != 0 ||
        llam_join(task) != 0) {
        task = NULL;
        (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook single-task lifecycle failed");
        goto out;
    }
    task = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.event_count, memory_order_acquire) != 6U ||
        !switch_hook_event_matches(&state.events[0],
                                   SWITCH_HOOK_EVENT_RESUME,
                                   &context,
                                   0) ||
        !switch_hook_event_matches(&state.events[1],
                                   SWITCH_HOOK_EVENT_BODY,
                                   &context,
                                   0) ||
        !switch_hook_event_matches(&state.events[2],
                                   SWITCH_HOOK_EVENT_SUSPEND,
                                   &context,
                                   EDOM) ||
        !switch_hook_event_matches(&state.events[3],
                                   SWITCH_HOOK_EVENT_RESUME,
                                   &context,
                                   EDOM) ||
        !switch_hook_event_matches(&state.events[4],
                                   SWITCH_HOOK_EVENT_BODY,
                                   &context,
                                   EDOM) ||
        !switch_hook_event_matches(&state.events[5],
                                   SWITCH_HOOK_EVENT_SUSPEND,
                                   &context,
                                   ERANGE)) {
        (void)SWITCH_HOOK_TEST_FAIL(
            "switch-hook scheduler/task ordering or errno isolation was wrong");
        goto out;
    }
    before_destroy =
        atomic_load_explicit(&state.event_count, memory_order_acquire);
    llam_runtime_destroy(runtime);
    runtime = NULL;
    if (atomic_load_explicit(&state.event_count, memory_order_acquire) !=
        before_destroy) {
        (void)SWITCH_HOOK_TEST_FAIL(
            "runtime destroy emitted a synthetic task switch hook");
        goto out;
    }
    rc = 0;

out:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static void switch_hook_migration_task(void *arg) {
    switch_hook_task_context_t *context = arg;
    switch_hook_case_state_t *state = context->state;

    if (g_llam_tls_shard == NULL ||
        g_llam_tls_shard->id != state->source_shard ||
        errno != 0) {
        switch_hook_case_fail(state);
    }
    switch_hook_case_body(context);
    errno = EDOM;
    if (!llam_yield_to_local_runnable()) {
        switch_hook_case_fail(state);
        return;
    }
    atomic_store_explicit(&state->direct_handoff_succeeded,
                          1U,
                          memory_order_release);
    /*
     * A compiler may cache the address of a C TLS variable across the
     * stackful switch call. The boundary hook is deliberately the authoritative
     * migration observation; it resolves TLS afresh on the destination worker.
     * Do not read or assign raw errno again in this migrated C frame.
     */
    switch_hook_case_body(context);
}

static void switch_hook_mover_task(void *arg) {
    switch_hook_task_context_t *context = arg;
    switch_hook_case_state_t *state = context->state;
    llam_task_t *moved;
    bool pushed = false;

    if (g_llam_tls_shard == NULL ||
        g_llam_tls_shard->id != state->source_shard ||
        errno != 0) {
        switch_hook_case_fail(state);
    }
    switch_hook_case_body(context);

    pthread_mutex_lock(&state->runtime->shards[state->source_shard].lock);
    moved = llam_norm_queue_pop_owner_locked(
        &state->runtime->shards[state->source_shard]);
    pthread_mutex_unlock(&state->runtime->shards[state->source_shard].lock);
    if (moved == state->migration_task) {
        pthread_mutex_lock(&state->runtime->shards[state->target_shard].lock);
        pushed = llam_norm_queue_push_owner_locked(
            &state->runtime->shards[state->target_shard],
            moved);
        pthread_mutex_unlock(&state->runtime->shards[state->target_shard].lock);
    } else {
        switch_hook_case_fail(state);
    }
    if (!pushed) {
        switch_hook_case_fail(state);
    } else {
        atomic_store_explicit(&state->migration_succeeded,
                              1U,
                              memory_order_release);
    }
    atomic_store_explicit(&state->runtime->steal_pause_active,
                          0U,
                          memory_order_release);
    llam_kick_shard(&state->runtime->shards[state->target_shard]);
    errno = E2BIG;
}

static int switch_hook_validate_task_sequence(
    const switch_hook_case_state_t *state,
    unsigned task_id,
    const switch_hook_event_kind_t *expected,
    unsigned expected_count) {
    unsigned event_count =
        atomic_load_explicit(&state->event_count, memory_order_acquire);
    unsigned matched = 0U;
    unsigned i;

    for (i = 0U; i < event_count && i < SWITCH_HOOK_MAX_EVENTS; ++i) {
        if (state->events[i].task_id != task_id) {
            continue;
        }
        if (matched >= expected_count ||
            state->events[i].kind != expected[matched]) {
            return -1;
        }
        matched += 1U;
    }
    return matched == expected_count ? 0 : -1;
}

static void switch_hook_dump_events(const switch_hook_case_state_t *state);

static int test_switch_hook_direct_handoff_and_migration(void) {
    static const switch_hook_event_kind_t migration_sequence[] = {
        SWITCH_HOOK_EVENT_RESUME,
        SWITCH_HOOK_EVENT_BODY,
        SWITCH_HOOK_EVENT_SUSPEND,
        SWITCH_HOOK_EVENT_RESUME,
        SWITCH_HOOK_EVENT_BODY,
        SWITCH_HOOK_EVENT_SUSPEND,
    };
    static const switch_hook_event_kind_t mover_sequence[] = {
        SWITCH_HOOK_EVENT_RESUME,
        SWITCH_HOOK_EVENT_BODY,
        SWITCH_HOOK_EVENT_SUSPEND,
    };
    switch_hook_case_state_t state;
    switch_hook_task_context_t migration_context;
    switch_hook_task_context_t mover_context;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *migration_handle = NULL;
    llam_task_t *mover_handle = NULL;
    llam_task_t *migration_task = NULL;
    llam_task_t *mover_task = NULL;
    llam_task_t *popped_migration;
    llam_task_t *popped_mover;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_count;
    int rc = 1;

    allowed_count = llam_count_allowed_cpus(&allowed_cpus);
    free(allowed_cpus);
    if (allowed_count < 2U) {
        fprintf(stderr,
                "skip switch-hook migration: fewer than two allowed CPUs\n");
        return 0;
    }

    switch_hook_case_init(&state);
    switch_hook_task_context_init(&migration_context, &state, 1U);
    switch_hook_task_context_init(&mover_context, &state, 2U);
    if (llam_runtime_opts_init(&runtime_opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts,
                             LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook migration option initialization failed");
    }
    runtime_opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    runtime_opts.worker_min = 2U;
    runtime_opts.worker_count = 2U;
    runtime_opts.worker_max = 2U;
    runtime_opts.experimental_flags =
        LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    runtime_opts.on_task_resume = switch_hook_case_resume;
    runtime_opts.on_task_suspend = switch_hook_case_suspend;
    runtime_opts.switch_hook_context = &state;
    if (llam_runtime_create(&runtime_opts,
                            LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                            &runtime) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook migration runtime creation failed");
    }
    state.runtime = runtime;
    spawn_opts.user_context = &migration_context;
    migration_handle = llam_runtime_spawn_ex(runtime,
                                             switch_hook_migration_task,
                                             &migration_context,
                                             &spawn_opts,
                                             LLAM_SPAWN_OPTS_CURRENT_SIZE);
    spawn_opts.user_context = &mover_context;
    mover_handle = llam_runtime_spawn_ex(runtime,
                                         switch_hook_mover_task,
                                         &mover_context,
                                         &spawn_opts,
                                         LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (migration_handle == NULL || mover_handle == NULL) {
        (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook migration task spawn failed");
        goto out;
    }
    migration_task = llam_task_resolve_public_handle(migration_handle);
    mover_task = llam_task_resolve_public_handle(mover_handle);
    if (migration_task == NULL || mover_task == NULL ||
        migration_task->home_shard == mover_task->home_shard) {
        (void)SWITCH_HOOK_TEST_FAIL(
            "switch-hook migration tasks did not resolve on distinct shards");
        goto out;
    }
    state.source_shard = migration_task->home_shard;
    state.target_shard = mover_task->home_shard;
    state.migration_task = migration_task;
    atomic_store_explicit(&runtime->steal_pause_active,
                          1U,
                          memory_order_release);

    pthread_mutex_lock(&runtime->shards[state.source_shard].lock);
    popped_migration = llam_queue_pop_head(
        &runtime->shards[state.source_shard].inject_q);
    atomic_store_explicit(&runtime->shards[state.source_shard].inject_depth,
                          runtime->shards[state.source_shard].inject_q.depth,
                          memory_order_release);
    pthread_mutex_unlock(&runtime->shards[state.source_shard].lock);
    pthread_mutex_lock(&runtime->shards[state.target_shard].lock);
    popped_mover = llam_queue_pop_head(
        &runtime->shards[state.target_shard].inject_q);
    atomic_store_explicit(&runtime->shards[state.target_shard].inject_depth,
                          runtime->shards[state.target_shard].inject_q.depth,
                          memory_order_release);
    pthread_mutex_unlock(&runtime->shards[state.target_shard].lock);
    if (popped_migration != migration_task || popped_mover != mover_task) {
        (void)SWITCH_HOOK_TEST_FAIL(
            "switch-hook migration setup did not own both inject entries");
        goto out;
    }
    pthread_mutex_lock(&runtime->shards[state.source_shard].lock);
    if (!llam_norm_queue_push_owner_locked(
            &runtime->shards[state.source_shard],
            mover_task) ||
        !llam_norm_queue_push_owner_locked(
            &runtime->shards[state.source_shard],
            migration_task)) {
        pthread_mutex_unlock(&runtime->shards[state.source_shard].lock);
        (void)SWITCH_HOOK_TEST_FAIL(
            "switch-hook migration setup could not stage direct handoff");
        goto out;
    }
    pthread_mutex_unlock(&runtime->shards[state.source_shard].lock);
    llam_task_end_public_op(migration_task);
    migration_task = NULL;
    llam_task_end_public_op(mover_task);
    mover_task = NULL;

    if (llam_runtime_run_handle(runtime) != 0 ||
        llam_join(migration_handle) != 0 ||
        llam_join(mover_handle) != 0) {
        migration_handle = NULL;
        mover_handle = NULL;
        (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
            "switch-hook migration lifecycle failed");
        goto out;
    }
    migration_handle = NULL;
    mover_handle = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.direct_handoff_succeeded,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.migration_succeeded,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&state.event_count, memory_order_acquire) != 9U ||
        atomic_load_explicit(&migration_context.resume_count,
                             memory_order_acquire) != 2U ||
        migration_context.resume_shards[0] != state.source_shard ||
        migration_context.resume_shards[1] != state.target_shard ||
        switch_hook_validate_task_sequence(&state,
                                           migration_context.task_id,
                                           migration_sequence,
                                           6U) != 0 ||
        switch_hook_validate_task_sequence(&state,
                                           mover_context.task_id,
                                           mover_sequence,
                                           3U) != 0 ||
        state.events[0].kind != SWITCH_HOOK_EVENT_RESUME ||
        state.events[0].task_id != migration_context.task_id ||
        state.events[1].kind != SWITCH_HOOK_EVENT_BODY ||
        state.events[1].task_id != migration_context.task_id ||
        state.events[2].kind != SWITCH_HOOK_EVENT_SUSPEND ||
        state.events[2].task_id != migration_context.task_id ||
        state.events[3].kind != SWITCH_HOOK_EVENT_RESUME ||
        state.events[3].task_id != mover_context.task_id ||
        state.events[4].kind != SWITCH_HOOK_EVENT_BODY ||
        state.events[4].task_id != mover_context.task_id ||
        !switch_hook_event_matches(&state.events[6],
                                   SWITCH_HOOK_EVENT_RESUME,
                                   &migration_context,
                                   EDOM) ||
        !switch_hook_event_matches(&state.events[7],
                                   SWITCH_HOOK_EVENT_BODY,
                                   &migration_context,
                                   EDOM) ||
        !switch_hook_event_matches(&state.events[8],
                                   SWITCH_HOOK_EVENT_SUSPEND,
                                   &migration_context,
                                   EDOM)) {
        switch_hook_dump_events(&state);
        (void)SWITCH_HOOK_TEST_FAIL(
            "direct handoff hook order or scheduler-thread migration was wrong");
        goto out;
    }
    rc = 0;

out:
    if (migration_task != NULL) {
        llam_task_end_public_op(migration_task);
    }
    if (mover_task != NULL) {
        llam_task_end_public_op(mover_task);
    }
    if (migration_handle != NULL) {
        (void)llam_detach(migration_handle);
    }
    if (mover_handle != NULL) {
        (void)llam_detach(mover_handle);
    }
    llam_runtime_destroy(runtime);
    return rc;
}

static void switch_hook_concurrent_task(void *arg) {
    switch_hook_task_context_t *context = arg;

    if (errno != 0) {
        switch_hook_case_fail(context->state);
    }
    switch_hook_case_body(context);
    errno = ERANGE;
}

static int test_switch_hook_concurrent_tasks(void) {
    switch_hook_case_state_t state;
    switch_hook_task_context_t first_context;
    switch_hook_task_context_t second_context;
    llam_runtime_opts_t runtime_opts;
    llam_spawn_opts_t spawn_opts;
    llam_runtime_t *runtime = NULL;
    llam_task_t *first = NULL;
    llam_task_t *second = NULL;
    unsigned *allowed_cpus = NULL;
    unsigned allowed_count;
    int rc = 1;

    allowed_count = llam_count_allowed_cpus(&allowed_cpus);
    free(allowed_cpus);
    if (allowed_count < 2U) {
        fprintf(stderr,
                "skip concurrent switch hooks: fewer than two allowed CPUs\n");
        return 0;
    }

    switch_hook_case_init(&state);
    switch_hook_task_context_init(&first_context, &state, 1U);
    switch_hook_task_context_init(&second_context, &state, 2U);
    atomic_store_explicit(&state.first_resume_barrier_expected,
                          2U,
                          memory_order_release);
    if (llam_runtime_opts_init(&runtime_opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0 ||
        llam_spawn_opts_init(&spawn_opts,
                             LLAM_SPAWN_OPTS_CURRENT_SIZE) != 0) {
        return SWITCH_HOOK_TEST_FAIL_ERRNO(
            "concurrent switch-hook option initialization failed");
    }
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
            "concurrent switch-hook runtime creation failed");
    }
    atomic_store_explicit(&runtime->steal_pause_active,
                          1U,
                          memory_order_release);
    spawn_opts.user_context = &first_context;
    first = llam_runtime_spawn_ex(runtime,
                                  switch_hook_concurrent_task,
                                  &first_context,
                                  &spawn_opts,
                                  LLAM_SPAWN_OPTS_CURRENT_SIZE);
    spawn_opts.user_context = &second_context;
    second = llam_runtime_spawn_ex(runtime,
                                   switch_hook_concurrent_task,
                                   &second_context,
                                   &spawn_opts,
                                   LLAM_SPAWN_OPTS_CURRENT_SIZE);
    if (first == NULL || second == NULL ||
        llam_runtime_run_handle(runtime) != 0 ||
        llam_join(first) != 0 ||
        llam_join(second) != 0) {
        first = NULL;
        second = NULL;
        (void)SWITCH_HOOK_TEST_FAIL_ERRNO(
            "concurrent switch-hook task lifecycle failed");
        goto out;
    }
    first = NULL;
    second = NULL;
    if (atomic_load_explicit(&state.failures, memory_order_acquire) != 0U ||
        atomic_load_explicit(&state.first_resume_barrier_arrived,
                             memory_order_acquire) != 2U ||
        atomic_load_explicit(&state.hooks_active_peak,
                             memory_order_acquire) < 2U ||
        atomic_load_explicit(&first_context.hook_active,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&second_context.hook_active,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&first_context.resume_count,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&second_context.resume_count,
                             memory_order_acquire) != 1U) {
        (void)SWITCH_HOOK_TEST_FAIL(
            "switch hooks were not concurrent across tasks or serialized per task");
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
    llam_runtime_destroy(runtime);
    return rc;
}

#include "test_switch_hook_prefix_cases.inc"

#undef SWITCH_HOOK_SERIAL_PEERS
#undef SWITCH_HOOK_MAX_RESUMES
#undef SWITCH_HOOK_MAX_EVENTS
