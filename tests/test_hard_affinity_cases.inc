/**
 * @file tests/test_hard_affinity_cases.inc
 * @brief Deterministic hard-affinity escape-route coverage.
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

typedef struct hard_affinity_fixture {
    llam_runtime_t runtime;
    llam_shard_t shards[2];
    llam_cldeque_t norm_cldeques[2];
} hard_affinity_fixture_t;

typedef struct hard_affinity_run_state {
    atomic_uint samples;
    atomic_uint mismatches;
    unsigned home_shard;
} hard_affinity_run_state_t;

static int hard_affinity_fixture_init(hard_affinity_fixture_t *fixture) {
    llam_runtime_t *rt;
    unsigned initialized_locks = 0U;

    if (fixture == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(fixture, 0, sizeof(*fixture));
    rt = &fixture->runtime;
    rt->shards = fixture->shards;
    rt->active_shards = 2U;
    atomic_init(&rt->initialized, false);
    atomic_init(&rt->stop_requested, false);
    atomic_init(&rt->fatal_errno, 0);
    atomic_init(&rt->deferred_fatal_pending, 0U);
    atomic_init(&rt->overflow_depth, 0U);
    atomic_init(&rt->online_shards, 2U);
    atomic_init(&rt->steal_pause_active, 0U);
    if (pthread_mutex_init(&rt->overflow_lock, NULL) != 0) {
        return -1;
    }
    rt->overflow_lock_initialized = true;

    for (unsigned i = 0U; i < 2U; ++i) {
        llam_shard_t *shard = &fixture->shards[i];

        shard->runtime = rt;
        shard->id = i;
        shard->norm_cldeque = &fixture->norm_cldeques[i];
        shard->event_fd = LLAM_INVALID_FD;
        shard->opaque_redirect_target_id = UINT_MAX;
        atomic_init(&shard->online, 1U);
        atomic_init(&shard->inflight_io_waiters, 0U);
        atomic_init(&shard->merge_pause_requested, 0U);
        atomic_init(&shard->merge_pause_ack, 0U);
        atomic_init(&shard->steal_pause_ack, 0U);
        atomic_init(&shard->event_pending, 0U);
        atomic_init(&shard->inject_depth, 0U);
        atomic_init(&shard->timer_count, 0U);
        atomic_init(&shard->timer_callbacks_active, 0U);
        atomic_init(&shard->current, NULL);
        atomic_init(&shard->live_tasks, 0U);
        atomic_init(&shard->norm_depth, 0U);
        llam_cldeque_init(shard->norm_cldeque);
        if (pthread_mutex_init(&shard->lock, NULL) != 0) {
            for (unsigned j = 0U; j < initialized_locks; ++j) {
                pthread_mutex_destroy(&fixture->shards[j].lock);
            }
            pthread_mutex_destroy(&rt->overflow_lock);
            rt->overflow_lock_initialized = false;
            return -1;
        }
        initialized_locks += 1U;
    }
    return 0;
}

static void hard_affinity_fixture_destroy(hard_affinity_fixture_t *fixture) {
    if (fixture == NULL) {
        return;
    }
    for (unsigned i = 0U; i < 2U; ++i) {
        pthread_mutex_destroy(&fixture->shards[i].lock);
    }
    if (fixture->runtime.overflow_lock_initialized) {
        pthread_mutex_destroy(&fixture->runtime.overflow_lock);
        fixture->runtime.overflow_lock_initialized = false;
    }
}

static void hard_affinity_task_init(llam_task_t *task,
                                    llam_runtime_t *rt,
                                    unsigned home_shard,
                                    bool pinned) {
    memset(task, 0, sizeof(*task));
    task->owner_runtime = rt;
    task->flags = pinned ? LLAM_TASK_FLAG_PINNED : 0U;
    task->home_shard = home_shard;
    task->live_shard = home_shard;
    task->alloc_owner_shard = home_shard;
    atomic_init(&task->public_handle_generation, 1U);
    atomic_init(&task->active_ops, 0U);
    atomic_init(&task->state, LLAM_TASK_STATE_RUNNABLE);
    atomic_init(&task->wait_reason, LLAM_WAIT_NONE);
    atomic_init(&task->last_shard, home_shard);
    atomic_init(&task->parked_shard, home_shard);
    atomic_init(&task->task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->base_task_class, LLAM_TASK_CLASS_DEFAULT);
    atomic_init(&task->task_listed, 0U);
    atomic_init(&task->scan_refs, 0U);
    atomic_init(&task->join_waiter_hint, 0U);
    atomic_init(&task->join_target, NULL);
    atomic_init(&task->active_wait_node, NULL);
    atomic_init(&task->active_wait_queue, NULL);
    atomic_init(&task->active_wait_queue_lock, NULL);
    atomic_init(&task->active_select_state, NULL);
    atomic_init(&task->active_wait_lifetime_ops, NULL);
    atomic_init(&task->wait_resolver_state, 0U);
    atomic_init(&task->wait_generation, 1U);
    atomic_init(&task->active_io_req, NULL);
    atomic_init(&task->active_io_generation, 0U);
    atomic_init(&task->active_block_job, NULL);
    atomic_init(&task->wake_error_code, 0);
    atomic_init(&task->opaque_blocking_depth, 0U);
    atomic_init(&task->preempt_requested, 0U);
    atomic_init(&task->completed, 0U);
    atomic_init(&task->reclaim_ready, 0U);
    atomic_init(&task->reclaim_claimed, 0U);
    atomic_init(&task->join_claimed, 0U);
    atomic_init(&task->detached, 0U);
}

static bool hard_affinity_queue_contains(const llam_queue_t *queue,
                                         const llam_task_t *expected) {
    const llam_task_t *task;

    if (queue == NULL || expected == NULL) {
        return false;
    }
    for (task = queue->head; task != NULL; task = task->queue_next) {
        if (task == expected) {
            return true;
        }
    }
    return false;
}

static int exercise_hard_affinity_predicate(void) {
    hard_affinity_fixture_t fixture;
    llam_task_t pinned;
    llam_task_t movable;
    llam_task_t foreign;
    int rc = 1;

    if (hard_affinity_fixture_init(&fixture) != 0) {
        return fail_errno("hard-affinity predicate fixture init failed");
    }
    hard_affinity_task_init(&pinned, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&movable, &fixture.runtime, 0U, false);
    hard_affinity_task_init(&foreign, NULL, 0U, false);

    if (llam_task_required_shard(&fixture.runtime, &pinned) != 0U ||
        llam_task_required_shard(&fixture.runtime, &movable) != UINT_MAX ||
        !llam_task_may_run_on_shard(&pinned, &fixture.shards[0]) ||
        llam_task_may_run_on_shard(&pinned, &fixture.shards[1]) ||
        !llam_task_may_run_on_shard(&movable, &fixture.shards[0]) ||
        !llam_task_may_run_on_shard(&movable, &fixture.shards[1]) ||
        llam_task_may_run_on_shard(&foreign, &fixture.shards[0])) {
        rc = fail_msg("hard-affinity predicate accepted an invalid execution shard");
        goto done;
    }

    pinned.home_shard = fixture.runtime.active_shards;
    if (llam_task_required_shard(&fixture.runtime, &pinned) != UINT_MAX ||
        atomic_load_explicit(&fixture.runtime.fatal_errno,
                             memory_order_acquire) != EPROTO) {
        rc = fail_msg("invalid pinned home did not fail closed");
        goto done;
    }
    rc = 0;

done:
    hard_affinity_fixture_destroy(&fixture);
    return rc;
}

static int exercise_hard_affinity_overflow_rotation(void) {
    hard_affinity_fixture_t fixture;
    llam_task_t home_first;
    llam_task_t home_second;
    llam_task_t movable;
    llam_task_t *taken;
    int rc = 1;

    if (hard_affinity_fixture_init(&fixture) != 0) {
        return fail_errno("hard-affinity overflow fixture init failed");
    }
    hard_affinity_task_init(&home_first, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&home_second, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&movable, &fixture.runtime, 0U, false);

    pthread_mutex_lock(&fixture.runtime.overflow_lock);
    llam_queue_push_tail(&fixture.runtime.overflow_q, &home_first);
    llam_queue_push_tail(&fixture.runtime.overflow_q, &movable);
    llam_queue_push_tail(&fixture.runtime.overflow_q, &home_second);
    atomic_store_explicit(&fixture.runtime.overflow_depth,
                          fixture.runtime.overflow_q.depth,
                          memory_order_release);
    pthread_mutex_unlock(&fixture.runtime.overflow_lock);

    taken = llam_take_overflow_task_for_shard(
        &fixture.runtime, &fixture.shards[1]);
    if (taken != &movable ||
        fixture.runtime.overflow_q.head != &home_first ||
        fixture.runtime.overflow_q.tail != &home_second ||
        fixture.runtime.overflow_q.depth != 2U) {
        rc = fail_msg("foreign overflow consumer did not rotate pinned tasks in FIFO order");
        goto done;
    }
    if (llam_take_overflow_task_for_shard(
            &fixture.runtime, &fixture.shards[0]) != &home_first ||
        llam_take_overflow_task_for_shard(
            &fixture.runtime, &fixture.shards[0]) != &home_second ||
        atomic_load_explicit(&fixture.runtime.overflow_depth,
                             memory_order_acquire) != 0U) {
        rc = fail_msg("home shard did not recover its pinned overflow tasks");
        goto done;
    }
    rc = 0;

done:
    hard_affinity_fixture_destroy(&fixture);
    return rc;
}

static int exercise_hard_affinity_opaque_redirect(void) {
    hard_affinity_fixture_t fixture;
    llam_task_t pinned_inject;
    llam_task_t pinned_hot;
    llam_task_t pinned_norm;
    llam_task_t movable_inject;
    llam_task_t movable_hot;
    llam_task_t movable_norm;
    int rc = 1;

    if (hard_affinity_fixture_init(&fixture) != 0) {
        return fail_errno("hard-affinity redirect fixture init failed");
    }
    hard_affinity_task_init(&pinned_inject, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&pinned_hot, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&pinned_norm, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&movable_inject, &fixture.runtime, 0U, false);
    hard_affinity_task_init(&movable_hot, &fixture.runtime, 0U, false);
    hard_affinity_task_init(&movable_norm, &fixture.runtime, 0U, false);

    pthread_mutex_lock(&fixture.shards[0].lock);
    llam_queue_push_tail(&fixture.shards[0].inject_q, &pinned_inject);
    llam_queue_push_tail(&fixture.shards[0].inject_q, &movable_inject);
    atomic_store_explicit(&fixture.shards[0].inject_depth, 2U,
                          memory_order_release);
    llam_queue_push_tail(&fixture.shards[0].hot_q, &pinned_hot);
    llam_queue_push_tail(&fixture.shards[0].hot_q, &movable_hot);
    (void)llam_norm_queue_push_owner_locked(
        &fixture.shards[0], &pinned_norm);
    (void)llam_norm_queue_push_owner_locked(
        &fixture.shards[0], &movable_norm);
    llam_activate_opaque_redirect_locked(&fixture.shards[0], NULL);
    pthread_mutex_unlock(&fixture.shards[0].lock);

    if (!fixture.shards[0].opaque_redirect_active ||
        !hard_affinity_queue_contains(
            &fixture.shards[0].inject_q, &pinned_inject) ||
        !hard_affinity_queue_contains(
            &fixture.shards[0].hot_q, &pinned_hot) ||
        !hard_affinity_queue_contains(
            &fixture.shards[0].norm_q, &pinned_norm) ||
        hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &pinned_inject) ||
        hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &pinned_hot) ||
        hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &pinned_norm) ||
        !hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &movable_inject) ||
        !hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &movable_hot) ||
        !hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &movable_norm)) {
        rc = fail_msg("opaque redirect migrated pinned home work");
        goto done;
    }
    rc = 0;

done:
    hard_affinity_fixture_destroy(&fixture);
    return rc;
}

static int exercise_hard_affinity_reinject_and_steal(void) {
    hard_affinity_fixture_t fixture;
    llam_task_t reinjected;
    llam_task_t stolen;
    llam_task_t filler;
    unsigned stolen_count;
    int rc = 1;

    if (hard_affinity_fixture_init(&fixture) != 0) {
        return fail_errno("hard-affinity reinject fixture init failed");
    }
    hard_affinity_task_init(&reinjected, &fixture.runtime, 0U, true);
    reinjected.state = LLAM_TASK_STATE_PARKED;
    reinjected.wait_reason = LLAM_WAIT_JOIN;
    llam_reinject_task_on_shard(&fixture.runtime,
                                &reinjected,
                                1U,
                                true,
                                LLAM_TRACE_WAKE,
                                LLAM_WAIT_JOIN);
    if (!hard_affinity_queue_contains(
            &fixture.shards[0].inject_q, &reinjected) ||
        hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &reinjected)) {
        rc = fail_msg("explicit foreign reinject moved a pinned task");
        goto done;
    }

    (void)llam_queue_pop_head(&fixture.shards[0].inject_q);
    atomic_store_explicit(&fixture.shards[0].inject_depth, 0U,
                          memory_order_release);
    hard_affinity_task_init(&stolen, &fixture.runtime, 0U, true);
    hard_affinity_task_init(&filler, &fixture.runtime, 1U, false);
    pthread_mutex_lock(&fixture.shards[1].lock);
    (void)llam_norm_queue_push_owner_locked(&fixture.shards[1], &filler);
    (void)llam_norm_queue_push_owner_locked(&fixture.shards[1], &stolen);
    pthread_mutex_unlock(&fixture.shards[1].lock);

    stolen_count = llam_steal_from_victim(
        &fixture.shards[0], &fixture.shards[1]);
    if (stolen_count != 0U ||
        !hard_affinity_queue_contains(
            &fixture.shards[0].inject_q, &stolen) ||
        hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &stolen)) {
        rc = fail_msg("steal rejection returned a pinned task to a foreign victim");
        goto done;
    }
    rc = 0;

done:
    hard_affinity_fixture_destroy(&fixture);
    return rc;
}

static int exercise_hard_affinity_direct_handoff(void) {
    hard_affinity_fixture_t fixture;
    llam_task_t locked_candidate;
    llam_task_t unlocked_candidate;
    llam_task_t *taken;
    int rc = 1;

    if (hard_affinity_fixture_init(&fixture) != 0) {
        return fail_errno("hard-affinity handoff fixture init failed");
    }
    hard_affinity_task_init(
        &locked_candidate, &fixture.runtime, 0U, true);
    pthread_mutex_lock(&fixture.shards[1].lock);
    llam_queue_push_tail(
        &fixture.shards[1].hot_q, &locked_candidate);
    taken = llam_take_handoff_task_locked(&fixture.shards[1]);
    pthread_mutex_unlock(&fixture.shards[1].lock);
    if (taken != NULL ||
        hard_affinity_queue_contains(
            &fixture.shards[1].hot_q, &locked_candidate) ||
        llam_take_overflow_task_for_shard(
            &fixture.runtime, &fixture.shards[0]) != &locked_candidate) {
        rc = fail_msg("locked direct handoff accepted a foreign pinned task");
        goto done;
    }

    fixture.runtime.experimental_lockfree_normq = 1U;
    hard_affinity_task_init(
        &unlocked_candidate, &fixture.runtime, 0U, true);
    pthread_mutex_lock(&fixture.shards[1].lock);
    if (!llam_norm_queue_push_owner_locked(
            &fixture.shards[1], &unlocked_candidate)) {
        pthread_mutex_unlock(&fixture.shards[1].lock);
        rc = fail_msg("unlocked handoff candidate setup failed");
        goto done;
    }
    pthread_mutex_unlock(&fixture.shards[1].lock);
    taken = llam_take_handoff_task_unlocked(&fixture.shards[1]);
    if (taken != NULL ||
        atomic_load_explicit(&fixture.shards[1].norm_depth,
                             memory_order_acquire) != 0U ||
        !hard_affinity_queue_contains(
            &fixture.shards[0].inject_q, &unlocked_candidate) ||
        hard_affinity_queue_contains(
            &fixture.shards[1].inject_q, &unlocked_candidate)) {
        rc = fail_msg("owner direct handoff accepted a foreign pinned task");
        goto done;
    }
    rc = 0;

done:
    hard_affinity_fixture_destroy(&fixture);
    return rc;
}

static int exercise_hard_affinity_merge_and_rehome_scans(void) {
    hard_affinity_fixture_t merge_fixture;
#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS
    submit_rehome_fixture_t parked_fixture;
    submit_rehome_fixture_t submit_fixture;
    inflight_reuse_fixture_t inflight_fixture;
#endif
    llam_task_t pinned_runnable;
    unsigned migrated = UINT_MAX;
    bool ok;
    int rc = 1;

    if (hard_affinity_fixture_init(&merge_fixture) != 0) {
        return fail_errno("hard-affinity merge fixture init failed");
    }
    hard_affinity_task_init(
        &pinned_runnable, &merge_fixture.runtime, 0U, true);
    pthread_mutex_lock(&merge_fixture.shards[0].lock);
    pthread_mutex_lock(&merge_fixture.shards[1].lock);
    llam_queue_push_tail(
        &merge_fixture.shards[0].inject_q, &pinned_runnable);
    ok = llam_merge_runnable_queues_locked(&merge_fixture.shards[0],
                                           &merge_fixture.shards[1],
                                           &migrated);
    llam_merge_rehome_task(&merge_fixture.shards[0],
                           &merge_fixture.shards[1],
                           &pinned_runnable);
    pthread_mutex_unlock(&merge_fixture.shards[1].lock);
    pthread_mutex_unlock(&merge_fixture.shards[0].lock);
    if (ok || migrated != 0U || pinned_runnable.home_shard != 0U) {
        rc = fail_msg("merge rewrote or migrated a pinned task");
        goto merge_done;
    }
    hard_affinity_fixture_destroy(&merge_fixture);

#if defined(LLAM_ENABLE_TEST_HOOKS) && !LLAM_PLATFORM_WINDOWS
    if (init_submit_rehome_fixture(&parked_fixture, false) != 0) {
        return fail_errno("hard-affinity parked rehome fixture init failed");
    }
    parked_fixture.task.flags = LLAM_TASK_FLAG_PINNED;
    parked_fixture.task.wait_reason = LLAM_WAIT_JOIN;
    atomic_store_explicit(&parked_fixture.task.join_target,
                          &parked_fixture.caller,
                          memory_order_release);
    parked_fixture.shards[0].all_tasks = &parked_fixture.task;
    migrated = UINT_MAX;
    llam_rehome_parked_waiters(&parked_fixture.runtime,
                               &parked_fixture.shards[0],
                               &parked_fixture.shards[1],
                               &migrated);
    if (migrated != 0U || parked_fixture.task.parked_shard != 0U ||
        parked_fixture.task.home_shard != 0U) {
        destroy_submit_rehome_fixture(&parked_fixture);
        return fail_msg("parked-wait scan migrated a pinned task");
    }
    destroy_submit_rehome_fixture(&parked_fixture);

    if (init_submit_rehome_fixture(&submit_fixture, true) != 0) {
        return fail_errno("hard-affinity submit rehome fixture init failed");
    }
    submit_fixture.task.flags = LLAM_TASK_FLAG_PINNED;
    migrated = UINT_MAX;
    ok = llam_rehome_node_submit_waiters(&submit_fixture.nodes[0],
                                         &submit_fixture.shards[0],
                                         &submit_fixture.shards[1],
                                         &migrated);
    if (ok || migrated != 0U ||
        atomic_load_explicit(&submit_fixture.req.owner_shard,
                             memory_order_acquire) != 0U) {
        destroy_submit_rehome_fixture(&submit_fixture);
        return fail_msg("submit-queue scan migrated a pinned task");
    }
    destroy_submit_rehome_fixture(&submit_fixture);

    if (init_inflight_reuse_fixture(&inflight_fixture, 0U, 0U) != 0) {
        return fail_errno("hard-affinity inflight rehome fixture init failed");
    }
    inflight_fixture.task.flags = LLAM_TASK_FLAG_PINNED;
    migrated = UINT_MAX;
    llam_rehome_inflight_io_waiters(&inflight_fixture.runtime,
                                    &inflight_fixture.shards[0],
                                    &inflight_fixture.shards[1],
                                    &migrated);
    if (migrated != 0U ||
        atomic_load_explicit(
            &inflight_fixture.task.embedded_io_req.inflight_owner_shard,
            memory_order_acquire) != 0U ||
        inflight_fixture.task.parked_shard != 0U) {
        destroy_inflight_reuse_fixture(&inflight_fixture);
        return fail_msg("inflight-I/O scan migrated a pinned task");
    }
    destroy_inflight_reuse_fixture(&inflight_fixture);
#endif
    return 0;

merge_done:
    hard_affinity_fixture_destroy(&merge_fixture);
    return rc;
}

static void hard_affinity_recording_task(void *opaque) {
    hard_affinity_run_state_t *state = opaque;

    for (unsigned i = 0U; i < 32U; ++i) {
        unsigned shard_id =
            g_llam_tls_shard != NULL ? g_llam_tls_shard->id : UINT_MAX;

        atomic_fetch_add_explicit(&state->samples, 1U,
                                  memory_order_relaxed);
        if (shard_id != state->home_shard) {
            atomic_fetch_add_explicit(&state->mismatches, 1U,
                                      memory_order_relaxed);
        }
        llam_yield();
    }
}

static int exercise_hard_affinity_final_dispatch_guard(void) {
    llam_runtime_opts_t opts;
    llam_spawn_opts_t spawn_opts;
    hard_affinity_run_state_t state;
    llam_runtime_t *runtime = NULL;
    llam_runtime_t *raw_runtime = NULL;
    llam_task_t *handle = NULL;
    llam_task_t *task = NULL;
    llam_task_t *removed = NULL;
    unsigned foreign_id;
    int rc = 1;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.samples, 0U);
    atomic_init(&state.mismatches, 0U);
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return fail_errno("hard-affinity dispatch opts init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    opts.worker_min = 2U;
    opts.worker_count = 2U;
    opts.worker_max = 2U;
    opts.blocking_min = 0U;
    opts.blocking_max = 1U;
    if (llam_runtime_create(
            &opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) != 0) {
        return fail_errno("hard-affinity dispatch runtime create failed");
    }
    if (llam_runtime_begin_public_op(runtime, &raw_runtime) != 0) {
        rc = fail_errno("hard-affinity dispatch runtime pin failed");
        goto done;
    }
    atomic_store_explicit(
        &raw_runtime->next_spawn_shard, 0U, memory_order_relaxed);
    pthread_mutex_lock(&raw_runtime->shards[0].lock);
    raw_runtime->shards[0].opaque_redirect_active = true;
    raw_runtime->shards[0].opaque_redirect_target_id = 1U;
    pthread_mutex_unlock(&raw_runtime->shards[0].lock);

    memset(&spawn_opts, 0, sizeof(spawn_opts));
    spawn_opts.flags = LLAM_SPAWN_F_PINNED;
    handle = llam_runtime_spawn_ex(runtime,
                                   hard_affinity_recording_task,
                                   &state,
                                   &spawn_opts,
                                   sizeof(spawn_opts));
    if (handle == NULL) {
        rc = fail_errno("hard-affinity dispatch task spawn failed");
        goto done;
    }
    task = llam_task_resolve_public_handle(handle);
    if (task == NULL || task->home_shard != 0U ||
        task->home_shard >= raw_runtime->active_shards) {
        rc = fail_msg("hard-affinity dispatch task did not resolve");
        goto done;
    }
    pthread_mutex_lock(&raw_runtime->shards[0].lock);
    raw_runtime->shards[0].opaque_redirect_active = false;
    raw_runtime->shards[0].opaque_redirect_target_id = UINT_MAX;
    pthread_mutex_unlock(&raw_runtime->shards[0].lock);
    state.home_shard = task->home_shard;
    foreign_id = (task->home_shard + 1U) % raw_runtime->active_shards;

    pthread_mutex_lock(&raw_runtime->shards[task->home_shard].lock);
    if (raw_runtime->shards[task->home_shard].inject_q.head == task) {
        removed = llam_queue_pop_head(
            &raw_runtime->shards[task->home_shard].inject_q);
        atomic_store_explicit(
            &raw_runtime->shards[task->home_shard].inject_depth,
            raw_runtime->shards[task->home_shard].inject_q.depth,
            memory_order_release);
    } else if (raw_runtime->shards[task->home_shard].hot_q.head == task) {
        removed = llam_queue_pop_head(
            &raw_runtime->shards[task->home_shard].hot_q);
    } else {
        removed = llam_norm_queue_pop_owner_locked(
            &raw_runtime->shards[task->home_shard]);
    }
    pthread_mutex_unlock(&raw_runtime->shards[task->home_shard].lock);
    if (removed != task) {
        rc = fail_msg("hard-affinity dispatch task was not on its home queue");
        goto done;
    }

    pthread_mutex_lock(&raw_runtime->shards[foreign_id].lock);
    /*
     * Bypass producer and inject-drain guards so the scheduler's last-chance
     * dispatch boundary is the only component that can contain this entry.
     */
    llam_queue_push_tail(&raw_runtime->shards[foreign_id].hot_q, task);
    pthread_mutex_unlock(&raw_runtime->shards[foreign_id].lock);
    llam_task_end_public_op(task);
    task = NULL;
    llam_runtime_end_public_op(raw_runtime);
    raw_runtime = NULL;

    if (llam_runtime_run_handle(runtime) != 0 || llam_join(handle) != 0) {
        handle = NULL;
        rc = fail_errno("hard-affinity dispatch task did not complete");
        goto done;
    }
    handle = NULL;
    if (atomic_load_explicit(&state.samples, memory_order_acquire) != 32U ||
        atomic_load_explicit(&state.mismatches, memory_order_acquire) != 0U) {
        rc = fail_msg("final dispatch guard executed a pinned task off home");
        goto done;
    }
    rc = 0;

done:
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

static int exercise_hard_affinity_cases(void) {
    if (exercise_hard_affinity_predicate() != 0 ||
        exercise_hard_affinity_overflow_rotation() != 0 ||
        exercise_hard_affinity_opaque_redirect() != 0 ||
        exercise_hard_affinity_reinject_and_steal() != 0 ||
        exercise_hard_affinity_direct_handoff() != 0 ||
        exercise_hard_affinity_merge_and_rehome_scans() != 0 ||
        exercise_hard_affinity_final_dispatch_guard() != 0) {
        return 1;
    }
    return 0;
}
