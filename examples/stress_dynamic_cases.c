/**
 * @file examples/stress_dynamic_cases.c
 * @brief Dynamic worker and live I/O migration stress cases.
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

#include "stress_internal.h"

static int stress_join_until_retry_oom(llam_task_t *task, uint64_t deadline_ns) {
    unsigned attempts;

    for (attempts = 0U; attempts < 32U; ++attempts) {
        int saved_errno;

        if (llam_join_until(task, deadline_ns) == 0) {
            return 0;
        }
        if (errno != ENOMEM || llam_deadline_passed(deadline_ns)) {
            return -1;
        }
        saved_errno = errno;
        llam_yield();
        errno = saved_errno;
    }
    return -1;
}

void dynamic_live_poll_watch_task(void *arg) {
    dynamic_live_poll_watch_state_t *state = arg;
    dynamic_live_poll_waiter_state_t *waiter_states = NULL;
    llam_task_t **waiters = NULL;
    llam_runtime_stats_t stats;
    atomic_uint completed;
    unsigned live_wait_floor = 0U;
    unsigned spawned = 0U;
    unsigned monitor_round;
    bool saw_scale_up = false;
    bool reached_live_floor = false;
    unsigned i;

    if (state == NULL) {
        stress_fail_msg("dynamic live poll state missing");
        return;
    }
    if (!stress_runtime_supports_multishot_poll()) {
        stress_print_phase_skipped("dynamic_live_poll_watch", "multishot poll unavailable");
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        return;
    }

    state->sv[0] = -1;
    state->sv[1] = -1;
    atomic_init(&completed, 0U);
    waiter_states = calloc(state->waiter_count, sizeof(*waiter_states));
    waiters = calloc(state->waiter_count, sizeof(*waiters));
    if (waiter_states == NULL || waiters == NULL) {
        stress_fail_msg("dynamic live poll alloc failed");
        free(waiter_states);
        free(waiters);
        return;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, state->sv) != 0) {
        stress_fail_msg("dynamic live poll socketpair failed");
        free(waiter_states);
        free(waiters);
        return;
    }

    for (i = 0; i < state->waiter_count; ++i) {
        waiter_states[i].fd = state->sv[0];
        waiter_states[i].completed = &completed;
        waiters[i] = llam_spawn(dynamic_live_poll_waiter_task,
                              &waiter_states[i],
                              &(llam_spawn_opts_t){
                                  .task_class = LLAM_TASK_CLASS_DEFAULT,
                                  .stack_class = LLAM_STACK_CLASS_DEFAULT,
                              });
        if (waiters[i] == NULL) {
            stress_fail_msg("dynamic live poll waiter spawn failed");
            goto cleanup;
        }
        spawned += 1U;
    }

    if (llam_runtime_collect_stats(&stats) != 0) {
        stress_fail_msg("dynamic live poll stats collect failed");
        goto cleanup;
    }
    live_wait_floor = stress_dynamic_live_wait_floor(&stats);
    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);

    for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
        if (llam_runtime_collect_stats(&stats) != 0) {
            stress_fail_msg("dynamic live poll monitor stats failed");
            goto cleanup;
        }
        saw_scale_up = saw_scale_up || stats.online_workers > live_wait_floor;
        if (saw_scale_up &&
            stats.active_workers > live_wait_floor &&
            stats.online_workers <= live_wait_floor &&
            atomic_load(&completed) == 0U) {
            reached_live_floor = true;
            break;
        }
        if (state->monitor_sleep_ns > 0U && llam_sleep_ns(state->monitor_sleep_ns) != 0) {
            stress_fail_msg("dynamic live poll monitor sleep failed");
            goto cleanup;
        }
    }

    if (stats.dynamic_workers != 0U &&
        stats.active_workers > live_wait_floor &&
        saw_scale_up && !reached_live_floor) {
        stress_fail_msg("dynamic live poll waiters did not downscale while parked");
    }

cleanup:
    if (state->sv[1] >= 0) {
        unsigned wake_rounds = spawned > 0U ? spawned : 1U;

        for (i = 0; i < wake_rounds && atomic_load(&completed) < spawned; ++i) {
            char byte = 'x';
            ssize_t rc = write(state->sv[1], &byte, 1U);

            if (rc != 1) {
                stress_fail_msg("dynamic live poll writer failed");
                break;
            }
            (void)llam_sleep_ns(1000ULL * 1000ULL);
        }
        (void)shutdown(state->sv[1], SHUT_WR);
    }

    if (state->sv[1] >= 0) {
        close(state->sv[1]);
        state->sv[1] = -1;
    }

    for (i = 0; i < spawned; ++i) {
        if (stress_join_until_retry_oom(waiters[i], llam_now_ns() + 10ULL * 1000ULL * 1000ULL * 1000ULL) != 0) {
            char message[128];

            (void)snprintf(message,
                           sizeof(message),
                           "dynamic live poll waiter join failed errno=%d index=%u",
                           errno,
                           i);
            stress_fail_msg(message);
        }
    }
    if (spawned == state->waiter_count && atomic_load(&completed) != state->waiter_count) {
        stress_fail_u32("dynamic live poll waiter completions", atomic_load(&completed), state->waiter_count);
    }
    stress_close_fd_pair(state->sv);
    free(waiter_states);
    free(waiters);
}

void dynamic_foreign_poll_watch_setup_task(void *arg) {
    dynamic_foreign_poll_watch_state_t *state = arg;
    unsigned source_shard;
    unsigned source_node;
    unsigned owner_node = UINT_MAX;
    unsigned monitor_round;
    unsigned i;

    if (state == NULL || g_llam_tls_shard == NULL) {
        stress_fail_msg("dynamic foreign poll setup state missing");
        return;
    }
    if (g_llam_runtime.experimental_shard_rings_multishot == 0U || g_llam_runtime.active_nodes <= 1U) {
        atomic_store(&state->skipped, 1U);
        atomic_store(&state->setup_done, 1U);
        return;
    }
    if (state->waiter_states == NULL || state->waiters == NULL) {
        stress_fail_msg("dynamic foreign poll setup alloc missing");
        atomic_store(&state->setup_failed, 1U);
        atomic_store(&state->setup_done, 1U);
        return;
    }

    source_shard = g_llam_tls_shard->id;
    source_node = g_llam_tls_shard->io_node_index;
    atomic_store(&state->source_shard, source_shard);
    atomic_store(&state->source_node, source_node);

    if (!stress_open_foreign_poll_pair(source_node, state->sv, &owner_node)) {
        stress_fail_msg("dynamic foreign poll no foreign owner pair");
        atomic_store(&state->setup_failed, 1U);
        atomic_store(&state->setup_done, 1U);
        return;
    }
    atomic_store(&state->owner_node, owner_node);

    for (i = 0U; i < state->waiter_count; ++i) {
        state->waiter_states[i].fd = state->sv[0];
        state->waiter_states[i].completed = &state->completed;
        state->waiters[i] = llam_spawn(dynamic_live_poll_waiter_task,
                                     &state->waiter_states[i],
                                     &(llam_spawn_opts_t){
                                         .task_class = LLAM_TASK_CLASS_DEFAULT,
                                         .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                     });
        if (state->waiters[i] == NULL) {
            stress_fail_msg("dynamic foreign poll waiter spawn failed");
            atomic_store(&state->setup_failed, 1U);
            break;
        }
        atomic_fetch_add(&state->spawned, 1U);
    }

    if (atomic_load(&state->setup_failed) == 0U) {
        for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
            unsigned total_waiters = 0U;
            unsigned source_owned_waiters = 0U;

            if (stress_poll_watch_waiter_counts(owner_node,
                                                state->sv[0],
                                                POLLIN,
                                                source_shard,
                                                &total_waiters,
                                                &source_owned_waiters) &&
                total_waiters >= atomic_load(&state->spawned)) {
                atomic_store(&state->observed_foreign_owner_waiters, 1U);
                break;
            }
            if (state->monitor_sleep_ns > 0U) {
                if (llam_sleep_ns(state->monitor_sleep_ns) != 0) {
                    stress_fail_msg("dynamic foreign poll setup observe wait failed");
                    atomic_store(&state->setup_failed, 1U);
                    break;
                }
            } else {
                llam_yield();
            }
        }
    }

    atomic_store(&state->setup_done, 1U);
    if (atomic_load(&state->setup_failed) == 0U && state->monitor_sleep_ns > 0U) {
        (void)llam_sleep_ns(state->monitor_sleep_ns);
    } else if (atomic_load(&state->setup_failed) == 0U) {
        llam_yield();
    }
}

void dynamic_foreign_poll_watch_scale_task(void *arg) {
    dynamic_foreign_poll_watch_state_t *state = arg;

    if (state == NULL) {
        stress_fail_msg("dynamic foreign poll scale state missing");
        return;
    }
    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
}

void dynamic_foreign_poll_watch_monitor_task(void *arg) {
    dynamic_foreign_poll_watch_state_t *state = arg;
    dynamic_poll_writer_state_t writer_state;
    llam_runtime_stats_t stats;
    llam_task_t *setup = NULL;
    llam_task_t *writer = NULL;
    unsigned setup_shard = UINT_MAX;
    unsigned spawned;
    unsigned source_shard;
    unsigned owner_node;
    unsigned live_wait_floor = 0U;
    unsigned monitor_round;
    bool saw_setup_scale_up = false;
    bool saw_waiter_rehome = false;
    bool saw_scale_up = false;
    bool reached_live_floor = false;
    unsigned i;

    if (state == NULL) {
        stress_fail_msg("dynamic foreign poll monitor state missing");
        return;
    }

    for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
        if (llam_runtime_collect_stats(&stats) != 0) {
            stress_fail_msg("dynamic foreign poll scale-up stats failed");
            goto cleanup;
        }
        if (stats.online_workers > stats.online_workers_floor) {
            saw_setup_scale_up = true;
            break;
        }
        if (state->monitor_sleep_ns > 0U) {
            if (llam_sleep_ns(state->monitor_sleep_ns) != 0) {
                stress_fail_msg("dynamic foreign poll scale-up wait failed");
                goto cleanup;
            }
        } else {
            llam_yield();
        }
    }

    if (!saw_setup_scale_up) {
        stress_fail_msg("dynamic foreign poll did not scale up before setup");
        goto cleanup;
    }

    setup_shard = g_llam_runtime.active_shards > 0U ? g_llam_runtime.active_shards - 1U : 0U;
    if (g_llam_tls_shard != NULL && setup_shard == g_llam_tls_shard->id && setup_shard > 0U) {
        setup_shard -= 1U;
    }
    setup = stress_spawn_on_shard(setup_shard,
                                  dynamic_foreign_poll_watch_setup_task,
                                  state,
                                  &(llam_spawn_opts_t){
                                      .task_class = LLAM_TASK_CLASS_DEFAULT,
                                      .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                      .flags = LLAM_SPAWN_F_PINNED,
                                  });
    if (setup == NULL) {
        stress_fail_msg("dynamic foreign poll setup spawn on target shard failed");
        goto cleanup;
    }

    for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
        if (atomic_load(&state->setup_done) != 0U) {
            break;
        }
        if (state->monitor_sleep_ns > 0U) {
            if (llam_sleep_ns(state->monitor_sleep_ns) != 0) {
                stress_fail_msg("dynamic foreign poll monitor wait failed");
                goto cleanup;
            }
        } else {
            llam_yield();
        }
    }

    if (atomic_load(&state->skipped) != 0U) {
        goto cleanup;
    }

    spawned = atomic_load(&state->spawned);
    source_shard = atomic_load(&state->source_shard);
    owner_node = atomic_load(&state->owner_node);
    if (atomic_load(&state->setup_done) == 0U ||
        atomic_load(&state->setup_failed) != 0U ||
        spawned == 0U ||
        owner_node == UINT_MAX) {
        stress_fail_msg("dynamic foreign poll setup failed");
        goto cleanup;
    }

    if (llam_runtime_collect_stats(&stats) != 0) {
        stress_fail_msg("dynamic foreign poll stats collect failed");
        goto cleanup;
    }
    live_wait_floor = stress_dynamic_live_wait_floor(&stats);
    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);

    for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
        unsigned total_waiters = 0U;
        unsigned source_owned_waiters = 0U;

        if (llam_runtime_collect_stats(&stats) != 0) {
            stress_fail_msg("dynamic foreign poll monitor stats failed");
            goto cleanup;
        }
        if (stress_poll_watch_waiter_counts(owner_node,
                                            state->sv[0],
                                            POLLIN,
                                            source_shard,
                                            &total_waiters,
                                            &source_owned_waiters) &&
            total_waiters >= spawned &&
            source_owned_waiters == 0U &&
            atomic_load(&state->completed) == 0U) {
            saw_waiter_rehome = true;
        }
        saw_scale_up = saw_scale_up || stats.online_workers > live_wait_floor;
        if (saw_waiter_rehome &&
            saw_scale_up &&
            stats.active_workers > live_wait_floor &&
            stats.online_workers <= live_wait_floor &&
            atomic_load(&state->completed) == 0U) {
            reached_live_floor = true;
            break;
        }
        if (state->monitor_sleep_ns > 0U && llam_sleep_ns(state->monitor_sleep_ns) != 0) {
            stress_fail_msg("dynamic foreign poll monitor sleep failed");
            goto cleanup;
        }
    }

    if (stats.dynamic_workers != 0U && !saw_waiter_rehome) {
        stress_fail_msg("dynamic foreign poll waiters did not rehome off source shard");
    }
    if (stats.dynamic_workers != 0U &&
        stats.active_workers > live_wait_floor &&
        saw_scale_up && !reached_live_floor) {
        stress_fail_msg("dynamic foreign poll waiters did not downscale while rehomed");
    }

cleanup:
    writer_state.fd = state->sv[1];
    writer_state.delay_ns = 0U;
    if (state->sv[1] >= 0) {
        writer = llam_spawn(dynamic_poll_writer_task,
                          &writer_state,
                          &(llam_spawn_opts_t){
                              .task_class = LLAM_TASK_CLASS_DEFAULT,
                              .stack_class = LLAM_STACK_CLASS_DEFAULT,
                          });
        if (writer == NULL) {
            stress_fail_msg("dynamic foreign poll writer spawn failed");
            close(state->sv[1]);
            state->sv[1] = -1;
        } else if (llam_join(writer) != 0) {
            stress_fail_msg("dynamic foreign poll writer join failed");
        }
    }

    spawned = atomic_load(&state->spawned);
    for (i = 0U; i < spawned; ++i) {
        if (state->waiters[i] != NULL && llam_join(state->waiters[i]) != 0) {
            stress_fail_msg("dynamic foreign poll waiter join failed");
        }
    }
    if (spawned == state->waiter_count && atomic_load(&state->completed) != spawned) {
        stress_fail_u32("dynamic foreign poll waiter completions", atomic_load(&state->completed), spawned);
    }
    if (setup != NULL && llam_join(setup) != 0) {
        stress_fail_msg("dynamic foreign poll setup join failed");
    }
    stress_close_fd_pair(state->sv);
}

void dynamic_live_accept_watch_task(void *arg) {
    dynamic_live_accept_watch_state_t *state = arg;
    dynamic_live_accept_waiter_state_t *waiter_states = NULL;
    dynamic_accept_connector_state_t *connector_states = NULL;
    llam_task_t **waiters = NULL;
    llam_task_t **connectors = NULL;
    llam_runtime_stats_t stats;
    atomic_uint completed;
    atomic_uint connected;
    unsigned live_wait_floor = 0U;
    unsigned spawned_waiters = 0U;
    unsigned spawned_connectors = 0U;
    unsigned min_online = UINT_MAX;
    unsigned max_online = 0U;
    unsigned last_online = 0U;
    unsigned monitor_round;
    bool saw_scale_up = false;
    bool reached_live_floor = false;
    unsigned i;

    if (state == NULL) {
        stress_fail_msg("dynamic live accept state missing");
        return;
    }
    if (!stress_runtime_supports_multishot_accept()) {
        stress_print_phase_skipped("dynamic_live_accept_watch", "multishot accept unavailable");
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        return;
    }

    state->listener_fd = -1;
    state->port = 0U;
    atomic_init(&completed, 0U);
    atomic_init(&connected, 0U);
    waiter_states = calloc(state->waiter_count, sizeof(*waiter_states));
    connector_states = calloc(state->waiter_count, sizeof(*connector_states));
    waiters = calloc(state->waiter_count, sizeof(*waiters));
    connectors = calloc(state->waiter_count, sizeof(*connectors));
    if (waiter_states == NULL || connector_states == NULL || waiters == NULL || connectors == NULL) {
        stress_fail_msg("dynamic live accept alloc failed");
        free(waiter_states);
        free(connector_states);
        free(waiters);
        free(connectors);
        return;
    }

    state->listener_fd = stress_create_loopback_listener(&state->port);
    if (state->listener_fd < 0) {
        stress_fail_msg("dynamic live accept listener create failed");
        goto cleanup;
    }

    for (i = 0; i < state->waiter_count; ++i) {
        waiter_states[i].listener_fd = state->listener_fd;
        waiter_states[i].completed = &completed;
        waiters[i] = llam_spawn(dynamic_live_accept_waiter_task,
                              &waiter_states[i],
                              &(llam_spawn_opts_t){
                                  .task_class = LLAM_TASK_CLASS_DEFAULT,
                                  .stack_class = LLAM_STACK_CLASS_DEFAULT,
                              });
        if (waiters[i] == NULL) {
            stress_fail_msg("dynamic live accept waiter spawn failed");
            goto cleanup;
        }
        spawned_waiters += 1U;
    }

    if (llam_runtime_collect_stats(&stats) != 0) {
        stress_fail_msg("dynamic live accept stats collect failed");
        goto cleanup;
    }
    live_wait_floor = stress_dynamic_live_wait_floor(&stats);
    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);

    for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
        if (llam_runtime_collect_stats(&stats) != 0) {
            stress_fail_msg("dynamic live accept monitor stats failed");
            goto cleanup;
        }
        last_online = stats.online_workers;
        if (stats.online_workers < min_online) {
            min_online = stats.online_workers;
        }
        if (stats.online_workers > max_online) {
            max_online = stats.online_workers;
        }
        saw_scale_up = saw_scale_up || stats.online_workers > live_wait_floor;
        if (saw_scale_up &&
            stats.active_workers > live_wait_floor &&
            stats.online_workers <= live_wait_floor &&
            atomic_load(&completed) == 0U) {
            reached_live_floor = true;
            break;
        }
        if (state->monitor_sleep_ns > 0U && llam_sleep_ns(state->monitor_sleep_ns) != 0) {
            stress_fail_msg("dynamic live accept monitor sleep failed");
            goto cleanup;
        }
    }

    if (stats.dynamic_workers != 0U &&
        stats.active_workers > live_wait_floor &&
        saw_scale_up && !reached_live_floor) {
        fprintf(stderr,
                "[stress] dynamic live accept stats floor=%u last_online=%u min_online=%u max_online=%u saw_scale_up=%u reached_floor=%u completed=%u\n",
                live_wait_floor,
                last_online,
                min_online == UINT_MAX ? 0U : min_online,
                max_online,
                saw_scale_up ? 1U : 0U,
                reached_live_floor ? 1U : 0U,
                atomic_load(&completed));
        stress_fail_msg("dynamic live accept waiters did not downscale while parked");
    }

cleanup:
    if (state->listener_fd >= 0 && state->port != 0U) {
        for (i = 0; i < state->waiter_count; ++i) {
            connector_states[i].port = state->port;
            connector_states[i].delay_ns = 0U;
            connector_states[i].completed = &connected;
            connectors[i] = llam_spawn(dynamic_accept_connector_task,
                                     &connector_states[i],
                                     &(llam_spawn_opts_t){
                                         .task_class = LLAM_TASK_CLASS_DEFAULT,
                                         .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                     });
            if (connectors[i] == NULL) {
                stress_fail_msg("dynamic live accept connector spawn failed");
                break;
            }
            spawned_connectors += 1U;
        }
    }
    if (spawned_connectors < spawned_waiters && state->listener_fd >= 0) {
        close(state->listener_fd);
        state->listener_fd = -1;
    }
    for (i = 0; i < spawned_connectors; ++i) {
        if (llam_join(connectors[i]) != 0) {
            stress_fail_msg("dynamic live accept connector join failed");
        }
    }
    if (spawned_connectors == state->waiter_count && atomic_load(&connected) != state->waiter_count) {
        stress_fail_u32("dynamic live accept connector completions", atomic_load(&connected), state->waiter_count);
    }
    for (i = 0; i < spawned_waiters; ++i) {
        if (llam_join(waiters[i]) != 0) {
            stress_fail_msg("dynamic live accept waiter join failed");
        }
    }
    if (spawned_waiters == state->waiter_count && atomic_load(&completed) != state->waiter_count) {
        stress_fail_u32("dynamic live accept waiter completions", atomic_load(&completed), state->waiter_count);
    }
    if (state->listener_fd >= 0) {
        close(state->listener_fd);
        state->listener_fd = -1;
    }
    free(waiter_states);
    free(connector_states);
    free(waiters);
    free(connectors);
}

void dynamic_live_inflight_io_task(void *arg) {
    dynamic_live_inflight_io_state_t *state = arg;
    dynamic_live_inflight_waiter_state_t *waiter_states = NULL;
    llam_task_t **waiters = NULL;
    int (*pairs)[2] = NULL;
    llam_runtime_stats_t stats;
    atomic_uint completed;
    unsigned live_wait_floor = 0U;
    unsigned spawned = 0U;
    unsigned monitor_round;
    bool saw_scale_up = false;
    bool reached_live_floor = false;
    unsigned i;

    if (state == NULL) {
        stress_fail_msg("dynamic live inflight state missing");
        return;
    }
    if (!stress_runtime_supports_async_read()) {
        stress_print_phase_skipped("dynamic_live_inflight_io", "async read unavailable");
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        return;
    }

    atomic_init(&completed, 0U);
    waiter_states = calloc(state->waiter_count, sizeof(*waiter_states));
    waiters = calloc(state->waiter_count, sizeof(*waiters));
    pairs = calloc(state->waiter_count, sizeof(*pairs));
    if (waiter_states == NULL || waiters == NULL || pairs == NULL) {
        stress_fail_msg("dynamic live inflight alloc failed");
        free(waiter_states);
        free(waiters);
        free(pairs);
        return;
    }

    for (i = 0; i < state->waiter_count; ++i) {
        pairs[i][0] = -1;
        pairs[i][1] = -1;
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pairs[i]) != 0) {
            stress_fail_msg("dynamic live inflight socketpair failed");
            goto cleanup;
        }
        waiter_states[i].fd = pairs[i][0];
        waiter_states[i].completed = &completed;
        waiters[i] = llam_spawn(dynamic_live_inflight_waiter_task,
                              &waiter_states[i],
                              &(llam_spawn_opts_t){
                                  .task_class = LLAM_TASK_CLASS_DEFAULT,
                                  .stack_class = LLAM_STACK_CLASS_DEFAULT,
                              });
        if (waiters[i] == NULL) {
            stress_fail_msg("dynamic live inflight waiter spawn failed");
            goto cleanup;
        }
        spawned += 1U;
    }

    if (llam_runtime_collect_stats(&stats) != 0) {
        stress_fail_msg("dynamic live inflight stats collect failed");
        goto cleanup;
    }
    live_wait_floor = stress_dynamic_live_wait_floor(&stats);
    run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);

    for (monitor_round = 0U; monitor_round < state->monitor_rounds; ++monitor_round) {
        if (llam_runtime_collect_stats(&stats) != 0) {
            stress_fail_msg("dynamic live inflight monitor stats failed");
            goto cleanup;
        }
        saw_scale_up = saw_scale_up || stats.online_workers > live_wait_floor;
        if (saw_scale_up &&
            stats.active_workers > live_wait_floor &&
            stats.online_workers <= live_wait_floor &&
            atomic_load(&completed) == 0U) {
            reached_live_floor = true;
            break;
        }
        if (state->monitor_sleep_ns > 0U && llam_sleep_ns(state->monitor_sleep_ns) != 0) {
            stress_fail_msg("dynamic live inflight monitor sleep failed");
            goto cleanup;
        }
    }

    if (stats.dynamic_workers != 0U &&
        stats.active_workers > live_wait_floor &&
        saw_scale_up && !reached_live_floor) {
        stress_fail_msg("dynamic live inflight waiters did not downscale while parked");
    }

cleanup:
    for (i = 0; i < state->waiter_count; ++i) {
        if (pairs[i][1] >= 0) {
            char byte = 'i';
            ssize_t rc = write(pairs[i][1], &byte, 1U);

            if (rc != 1) {
                stress_fail_msg("dynamic live inflight write failed");
            }
        }
    }
    for (i = 0; i < spawned; ++i) {
        if (llam_join(waiters[i]) != 0) {
            stress_fail_msg("dynamic live inflight waiter join failed");
        }
    }
    if (spawned == state->waiter_count && atomic_load(&completed) != state->waiter_count) {
        stress_fail_u32("dynamic live inflight waiter completions", atomic_load(&completed), state->waiter_count);
    }
    for (i = 0; i < state->waiter_count; ++i) {
        stress_close_fd_pair(pairs[i]);
    }
    free(waiter_states);
    free(waiters);
    free(pairs);
}
