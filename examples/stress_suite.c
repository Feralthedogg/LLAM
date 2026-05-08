/**
 * @file examples/stress_suite.c
 * @brief Stress-suite orchestration for deterministic, timeout, I/O, and dynamic-worker phases.
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

#if !LLAM_PLATFORM_WINDOWS
#include <pthread.h>
#endif

typedef struct stress_phase_watchdog {
    atomic_uint done;
    unsigned timeout_sec;
    const char *phase_name;
} stress_phase_watchdog_t;

static unsigned stress_suite_runtime_flag_enabled(const llam_runtime_opts_t *opts, uint64_t flag) {
    return opts != NULL && (opts->experimental_flags & flag) != 0U ? 1U : 0U;
}

static void *stress_phase_watchdog_main(void *arg) {
    stress_phase_watchdog_t *watchdog = arg;
    unsigned ticks;
    unsigned limit = watchdog->timeout_sec * 10U;

    for (ticks = 0U; ticks < limit; ++ticks) {
        if (atomic_load_explicit(&watchdog->done, memory_order_acquire) != 0U) {
            return NULL;
        }
        usleep(100U * 1000U);
    }
    if (atomic_load_explicit(&watchdog->done, memory_order_acquire) == 0U) {
        fprintf(stderr, "[stress] phase watchdog timeout phase=%s seconds=%u\n",
                watchdog->phase_name,
                watchdog->timeout_sec);
        llam_dump_runtime_state(STDERR_FILENO);
        fflush(stderr);
    }
    return NULL;
}

static int stress_start_phase_watchdog(stress_phase_watchdog_t *watchdog,
                                       pthread_t *thread,
                                       const char *phase_name) {
    unsigned timeout_sec = stress_env_u32("LLAM_STRESS_PHASE_WATCHDOG_SEC", 0U, 3600U);

    if (timeout_sec == 0U) {
        return 0;
    }
    atomic_init(&watchdog->done, 0U);
    watchdog->timeout_sec = timeout_sec;
    watchdog->phase_name = phase_name;
    if (pthread_create(thread, NULL, stress_phase_watchdog_main, watchdog) != 0) {
        stress_fail_msg("phase watchdog start failed");
        return -1;
    }
    return 1;
}

static void stress_stop_phase_watchdog(stress_phase_watchdog_t *watchdog, pthread_t thread, int started) {
    if (started <= 0) {
        return;
    }
    atomic_store_explicit(&watchdog->done, 1U, memory_order_release);
    (void)pthread_join(thread, NULL);
}

void run_poll_paths(void) {
    int ready_sv[2];
    int timeout_sv[2];
    llam_task_t *writer;
    short revents = 0;
    int rc;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, ready_sv) != 0) {
        stress_fail_msg("poll ready socketpair failed");
        return;
    }
    writer = llam_spawn(poll_writer_task,
                      &ready_sv[1],
                      &(llam_spawn_opts_t){
                          .task_class = LLAM_TASK_CLASS_DEFAULT,
                          .stack_class = LLAM_STACK_CLASS_DEFAULT,
                      });
    if (writer == NULL) {
        stress_fail_msg("poll writer spawn failed");
        close(ready_sv[0]);
        close(ready_sv[1]);
        return;
    }
    rc = llam_poll_fd(ready_sv[0], POLLIN, stress_platform_prefers_indefinite_ready_poll() ? -1 : 20, &revents);
    if (rc != 1 || (revents & POLLIN) == 0) {
        stress_fail_msg("poll ready path failed");
    }
    if (llam_join(writer) != 0) {
        stress_fail_msg("poll writer join failed");
    }
    close(ready_sv[0]);
    close(ready_sv[1]);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, timeout_sv) != 0) {
        stress_fail_msg("poll timeout socketpair failed");
        return;
    }
    revents = 0;
    rc = llam_poll_fd(timeout_sv[0], POLLIN, 1, &revents);
    if (rc != 0 || revents != 0) {
        stress_fail_msg("poll timeout path failed");
    }
    close(timeout_sv[0]);
    close(timeout_sv[1]);
}

void run_fp_isolation_path(void) {
    fp_round_state_t down;
    fp_round_state_t up;
    llam_task_t *down_task;
    llam_task_t *up_task;

    down.mode = 1U;
    down.yields = 4U;
    atomic_init(&down.completed, 0U);
    up.mode = 2U;
    up.yields = 4U;
    atomic_init(&up.completed, 0U);

    stress_set_fp_round(0U);
    down_task = llam_spawn(fp_round_task,
                         &down,
                         &(llam_spawn_opts_t){
                             .task_class = LLAM_TASK_CLASS_DEFAULT,
                             .stack_class = LLAM_STACK_CLASS_DEFAULT,
                             .flags = LLAM_SPAWN_F_PINNED,
                         });
    up_task = llam_spawn(fp_round_task,
                       &up,
                       &(llam_spawn_opts_t){
                           .task_class = LLAM_TASK_CLASS_DEFAULT,
                           .stack_class = LLAM_STACK_CLASS_DEFAULT,
                           .flags = LLAM_SPAWN_F_PINNED,
                       });
    if (down_task == NULL || up_task == NULL) {
        stress_fail_msg("fp isolation task spawn failed");
        if (down_task != NULL) {
            (void)llam_join(down_task);
        }
        if (up_task != NULL) {
            (void)llam_join(up_task);
        }
        return;
    }
    if (llam_join(down_task) != 0 || llam_join(up_task) != 0) {
        stress_fail_msg("fp isolation join failed");
        return;
    }
    if (atomic_load(&down.completed) != 1U || atomic_load(&up.completed) != 1U) {
        stress_fail_msg("fp isolation completion count failed");
        return;
    }
    if (stress_fp_round_mode() != 0U) {
        stress_fail_u32("fp parent mode after isolation", stress_fp_round_mode(), 0U);
    }
}

void run_fp_inherit_path(void) {
    fp_inherit_state_t state;
    llam_task_t *child;

    state.expected_mode = 1U;
    atomic_init(&state.observed_mode, 0xFFU);
    atomic_init(&state.completed, 0U);
    stress_set_fp_round(state.expected_mode);
    child = llam_spawn(fp_inherit_child_task,
                     &state,
                     &(llam_spawn_opts_t){
                         .task_class = LLAM_TASK_CLASS_DEFAULT,
                         .stack_class = LLAM_STACK_CLASS_DEFAULT,
                         .flags = LLAM_SPAWN_F_PINNED,
                     });
    if (child == NULL) {
        stress_fail_msg("fp inherit child spawn failed");
        stress_set_fp_round(0U);
        return;
    }
    if (llam_join(child) != 0) {
        stress_fail_msg("fp inherit child join failed");
        stress_set_fp_round(0U);
        return;
    }
    if (atomic_load(&state.completed) != 1U) {
        stress_fail_u32("fp inherit completion count", atomic_load(&state.completed), 1U);
    }
    if (atomic_load(&state.observed_mode) != state.expected_mode) {
        stress_fail_u32("fp inherit observed mode", atomic_load(&state.observed_mode), state.expected_mode);
    }
    if (stress_fp_round_mode() != state.expected_mode) {
        stress_fail_u32("fp parent mode after inherit child", stress_fp_round_mode(), state.expected_mode);
    }
    stress_set_fp_round(0U);
}

void run_poll_cancel_timeout_race(void) {
    enum { kPollRaceRounds = 8 };
    unsigned i;

    for (i = 0; i < kPollRaceRounds; ++i) {
        poll_cancel_race_state_t state;
        llam_task_t *waiter;
        llam_task_t *trigger;
        int sv[2];

        state.fd = -1;
        state.token = NULL;
        atomic_init(&state.cancel_hits, 0U);
        atomic_init(&state.timeout_hits, 0U);
        atomic_init(&state.triggered, 0U);
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            stress_fail_msg("poll cancel-timeout socketpair failed");
            return;
        }
        state.fd = sv[0];
        state.token = llam_cancel_token_create();
        if (state.token == NULL) {
            stress_fail_msg("poll cancel-timeout token create failed");
            close(sv[0]);
            close(sv[1]);
            return;
        }

        waiter = llam_spawn(poll_cancel_race_waiter_task,
                          &state,
                          &(llam_spawn_opts_t){
                              .task_class = LLAM_TASK_CLASS_DEFAULT,
                              .stack_class = LLAM_STACK_CLASS_DEFAULT,
                              .cancel_token = state.token,
                          });
        trigger = llam_spawn(poll_cancel_race_trigger_task,
                           &state,
                           &(llam_spawn_opts_t){
                               .task_class = LLAM_TASK_CLASS_DEFAULT,
                               .stack_class = LLAM_STACK_CLASS_DEFAULT,
                           });
        if (waiter == NULL || trigger == NULL) {
            stress_fail_msg("poll cancel-timeout task spawn failed");
            if (waiter != NULL) {
                (void)llam_join(waiter);
            }
            if (trigger != NULL) {
                (void)llam_join(trigger);
            }
            (void)llam_cancel_token_destroy(state.token);
            close(sv[0]);
            close(sv[1]);
            return;
        }

        if (llam_join(waiter) != 0 || llam_join(trigger) != 0) {
            stress_fail_msg("poll cancel-timeout join failed");
            (void)llam_cancel_token_destroy(state.token);
            close(sv[0]);
            close(sv[1]);
            return;
        }
        if (atomic_load(&state.triggered) != 1U) {
            stress_fail_u32("poll cancel-timeout trigger count", atomic_load(&state.triggered), 1U);
        }
        if (atomic_load(&state.cancel_hits) + atomic_load(&state.timeout_hits) != 1U) {
            stress_fail_msg("poll cancel-timeout outcome count failed");
        }
        if (llam_sleep_ns(500000ULL) != 0) {
            stress_fail_msg("poll cancel-timeout post wait sleep failed");
        }
        (void)llam_cancel_token_destroy(state.token);
        close(sv[0]);
        close(sv[1]);
    }
}

typedef enum stress_select_race_mode {
    STRESS_SELECT_RACE_SEND = 1,
    STRESS_SELECT_RACE_CLOSE = 2,
    STRESS_SELECT_RACE_CANCEL = 3,
} stress_select_race_mode_t;

typedef struct stress_select_race_state {
    llam_channel_t *primary;
    llam_channel_t *secondary;
    llam_cancel_token_t *token;
    stress_select_race_mode_t mode;
    atomic_uint waiting;
    atomic_uint completed;
} stress_select_race_state_t;

static void stress_select_race_waiter_task(void *arg) {
    stress_select_race_state_t *state = arg;
    void *received = NULL;
    size_t selected = SIZE_MAX;
    int saved_errno = 0;
    llam_select_op_t ops[2] = {
        {
            .kind = LLAM_SELECT_OP_RECV,
            .channel = state->primary,
            .recv_out = &received,
        },
        {
            .kind = LLAM_SELECT_OP_RECV,
            .channel = state->secondary,
            .recv_out = &received,
        },
    };

    atomic_store_explicit(&state->waiting, 1U, memory_order_release);
    if (llam_channel_select(ops, 2U, UINT64_MAX, &selected) != 0) {
        saved_errno = errno;
        if (state->mode == STRESS_SELECT_RACE_CANCEL && saved_errno == ECANCELED) {
            atomic_fetch_add_explicit(&state->completed, 1U, memory_order_relaxed);
            return;
        }
        stress_fail_errno("channel select race errno", saved_errno, ECANCELED);
        return;
    }

    if (state->mode == STRESS_SELECT_RACE_SEND) {
        if (selected != 1U || received != (void *)(uintptr_t)0x5E1EC7U) {
            stress_fail_msg("channel select send race selected wrong operation");
            return;
        }
    } else if (state->mode == STRESS_SELECT_RACE_CLOSE) {
        if (selected != 1U || ops[1].result_errno != EPIPE) {
            stress_fail_msg("channel select close race selected wrong operation");
            return;
        }
    } else {
        stress_fail_msg("channel select cancel race unexpectedly selected");
        return;
    }
    atomic_fetch_add_explicit(&state->completed, 1U, memory_order_relaxed);
}

static void stress_select_race_trigger_task(void *arg) {
    stress_select_race_state_t *state = arg;
    unsigned spins;

    for (spins = 0U; spins < 32U; ++spins) {
        if (atomic_load_explicit(&state->waiting, memory_order_acquire) != 0U) {
            break;
        }
        llam_yield();
    }
    llam_yield();
    if (state->mode == STRESS_SELECT_RACE_SEND) {
        if (llam_channel_send(state->secondary, (void *)(uintptr_t)0x5E1EC7U) != 0) {
            stress_fail_msg("channel select race send failed");
        }
    } else if (state->mode == STRESS_SELECT_RACE_CLOSE) {
        if (llam_channel_close(state->secondary) != 0) {
            stress_fail_msg("channel select race close failed");
        }
    } else if (state->token != NULL) {
        if (llam_cancel_token_cancel(state->token) != 0) {
            stress_fail_msg("channel select race cancel failed");
        }
    }
}

static void stress_select_race_run_once(stress_select_race_mode_t mode) {
    stress_select_race_state_t state;
    llam_spawn_opts_t wait_opts;
    llam_task_t *waiter;
    llam_task_t *trigger;

    memset(&state, 0, sizeof(state));
    atomic_init(&state.waiting, 0U);
    atomic_init(&state.completed, 0U);
    state.mode = mode;
    state.primary = llam_channel_create(1U);
    state.secondary = llam_channel_create(1U);
    if (state.primary == NULL || state.secondary == NULL) {
        stress_fail_msg("channel select race channel create failed");
        if (state.primary != NULL) {
            (void)llam_channel_destroy(state.primary);
        }
        if (state.secondary != NULL) {
            (void)llam_channel_destroy(state.secondary);
        }
        return;
    }
    if (mode == STRESS_SELECT_RACE_CANCEL) {
        state.token = llam_cancel_token_create();
        if (state.token == NULL) {
            stress_fail_msg("channel select race token create failed");
            (void)llam_channel_destroy(state.primary);
            (void)llam_channel_destroy(state.secondary);
            return;
        }
    }

    memset(&wait_opts, 0, sizeof(wait_opts));
    wait_opts.cancel_token = state.token;
    waiter = llam_spawn(stress_select_race_waiter_task, &state, &wait_opts);
    trigger = llam_spawn(stress_select_race_trigger_task, &state, NULL);
    if (waiter == NULL || trigger == NULL) {
        stress_fail_msg("channel select race spawn failed");
        (void)llam_channel_close(state.primary);
        (void)llam_channel_close(state.secondary);
        if (waiter != NULL) {
            (void)llam_join(waiter);
        }
        if (trigger != NULL) {
            (void)llam_join(trigger);
        }
    } else if (llam_join(waiter) != 0 || llam_join(trigger) != 0) {
        stress_fail_msg("channel select race join failed");
    }
    if (atomic_load_explicit(&state.completed, memory_order_relaxed) != 1U) {
        stress_fail_msg("channel select race did not complete exactly once");
    }
    if (state.token != NULL && llam_cancel_token_destroy(state.token) != 0) {
        stress_fail_msg("channel select race token destroy failed");
    }
    if (llam_channel_destroy(state.primary) != 0 || llam_channel_destroy(state.secondary) != 0) {
        stress_fail_msg("channel select race channel destroy failed");
    }
}

void run_channel_select_race_paths(void) {
    enum { kSelectRaceRounds = 8 };
    unsigned i;

    for (i = 0U; i < kSelectRaceRounds; ++i) {
        void *received = NULL;
        size_t selected = SIZE_MAX;
        llam_channel_t *primary = llam_channel_create(1U);
        llam_channel_t *secondary = llam_channel_create(1U);
        llam_select_op_t ops[2];

        if (primary == NULL || secondary == NULL) {
            stress_fail_msg("channel select timeout channel create failed");
            if (primary != NULL) {
                (void)llam_channel_destroy(primary);
            }
            if (secondary != NULL) {
                (void)llam_channel_destroy(secondary);
            }
            return;
        }
        memset(ops, 0, sizeof(ops));
        ops[0].kind = LLAM_SELECT_OP_RECV;
        ops[0].channel = primary;
        ops[0].recv_out = &received;
        ops[1].kind = LLAM_SELECT_OP_RECV;
        ops[1].channel = secondary;
        ops[1].recv_out = &received;
        errno = 0;
        if (llam_channel_select(ops, 2U, llam_now_ns(), &selected) != -1 || errno != ETIMEDOUT) {
            stress_fail_msg("channel select immediate timeout failed");
        }
        if (llam_channel_destroy(primary) != 0 || llam_channel_destroy(secondary) != 0) {
            stress_fail_msg("channel select timeout channel destroy failed");
        }

        stress_select_race_run_once(STRESS_SELECT_RACE_SEND);
        stress_select_race_run_once(STRESS_SELECT_RACE_CLOSE);
        stress_select_race_run_once(STRESS_SELECT_RACE_CANCEL);
    }
}

void run_nested_opaque_path(void) {
    nested_opaque_state_t state;
    llam_task_t *task;

    atomic_init(&state.companion_steps, 0U);
    atomic_init(&state.completed_scopes, 0U);
    state.scopes = 3U;
    task = llam_spawn(nested_opaque_scope_task,
                    &state,
                    &(llam_spawn_opts_t){
                        .task_class = LLAM_TASK_CLASS_DEFAULT,
                        .stack_class = LLAM_STACK_CLASS_DEFAULT,
                        .flags = LLAM_SPAWN_F_SYS_TASK | LLAM_SPAWN_F_PINNED,
                    });
    if (task == NULL) {
        stress_fail_msg("nested opaque task spawn failed");
        return;
    }
    if (llam_join(task) != 0) {
        stress_fail_msg("nested opaque task join failed");
        return;
    }
    if (atomic_load(&state.completed_scopes) != state.scopes) {
        stress_fail_u32("nested opaque completed scopes", atomic_load(&state.completed_scopes), state.scopes);
    }
    if (atomic_load(&state.companion_steps) != state.scopes * 4U) {
        stress_fail_u32("nested opaque companion steps", atomic_load(&state.companion_steps), state.scopes * 4U);
    }
}

void stress_suite_task(void *arg) {
    unsigned round_count = arg != NULL ? *(unsigned *)arg : 4U;
    unsigned round;

    (void)arg;
    for (round = 0; round < round_count; ++round) {
        stress_trace_step("run_spawn_join_storm");
        run_spawn_join_storm();
        stress_trace_step("run_fp_isolation_path");
        run_fp_isolation_path();
        stress_trace_step("run_fp_inherit_path");
        run_fp_inherit_path();
        stress_trace_step("run_channel_ping_pong");
        run_channel_ping_pong();
        stress_trace_step("run_mutex_convoy");
        run_mutex_convoy();
        stress_trace_step("run_join_timeout_path");
        run_join_timeout_path();
        stress_trace_step("run_mutex_timeout_path");
        run_mutex_timeout_path();
        stress_trace_step("run_cancel_path");
        run_cancel_path();
        stress_trace_step("run_cond_cancel_path");
        run_cond_cancel_path();
        stress_trace_step("run_channel_timeout_paths");
        run_channel_timeout_paths();
        stress_trace_step("run_channel_select_race_paths");
        run_channel_select_race_paths();
        if (stress_platform_supports_owned_buffer_stress()) {
            stress_trace_step("run_owned_read_paths");
            run_owned_read_paths();
            stress_trace_step("run_recv_owned_peek_path");
            run_recv_owned_peek_path();
            stress_trace_step("run_recv_owned_multishot_path");
            run_recv_owned_multishot_path();
        }
        if (stress_platform_supports_basic_poll_stress()) {
            stress_trace_step("run_poll_paths");
            run_poll_paths();
            stress_trace_step("run_poll_cancel_timeout_race");
            run_poll_cancel_timeout_race();
        }
        if (stress_platform_supports_io_cancel_stress()) {
            stress_trace_step("run_io_cancel_path");
            run_io_cancel_path();
        }
        stress_trace_step("run_block_cancel_path");
        run_block_cancel_path();
        stress_trace_step("run_opaque_reuse");
        run_opaque_reuse();
        if (stress_platform_supports_nested_opaque()) {
            stress_trace_step("run_nested_opaque_path");
            run_nested_opaque_path();
        }
        stress_trace_step("llam_yield");
        llam_yield();
    }
}

void dynamic_suite_task(void *arg) {
    dynamic_suite_state_t *state = arg;
    unsigned round_count = state != NULL ? state->round_count : 2U;
    unsigned round;

    for (round = 0; round < round_count; ++round) {
        run_spawn_join_storm();
        run_channel_ping_pong();
        run_dynamic_sleep_fanout(state->sleep_tasks, state->sleep_yields, state->sleep_ns);
        run_dynamic_join_timeout_path();
        run_dynamic_mutex_timeout_path();
        run_dynamic_cond_timeout_path();
        run_dynamic_channel_timeout_paths();
        run_channel_select_race_paths();
        if (stress_platform_supports_basic_poll_stress()) {
            run_dynamic_poll_paths();
        }
        if (stress_platform_supports_owned_buffer_stress()) {
            run_owned_read_paths();
            run_recv_owned_peek_path();
            run_recv_owned_multishot_path();
        }
        if (stress_platform_supports_io_cancel_stress()) {
            run_io_cancel_path();
        }
        run_opaque_reuse();
        llam_yield();
    }
}

void stress_print_phase_stats(const char *phase_name, const llam_runtime_stats_t *stats) {
    printf("[stress] phase=%s active_shards=%u online_floor=%u online_min=%u online_max=%u active_nodes=%u dynamic_shards=%u shard_rings=%u shard_rings_multishot=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u ctx_switches=%llu io_submit_syscalls=%llu\n",
           phase_name,
           stats->active_workers,
           stats->online_workers_floor,
           stats->online_workers_min,
           stats->online_workers_max,
           stats->active_nodes,
           stats->dynamic_workers,
           stats->worker_rings,
           stats->worker_rings_multishot,
           stats->lockfree_normq,
           stats->huge_alloc,
           stats->sqpoll,
           (unsigned long long)stats->ctx_switches,
           (unsigned long long)stats->io_submit_syscalls);
}

int stress_run_phase(const char *phase_name,
                            llam_task_fn task_fn,
                            void *arg,
                            const llam_runtime_opts_t *opts,
                            unsigned require_dynamic_motion,
                            unsigned require_floor_reach) {
    llam_runtime_stats_t stats;
    stress_phase_watchdog_t watchdog;
    pthread_t watchdog_thread;
    int watchdog_started = 0;
    unsigned failures_before = atomic_load(&g_failures);

    printf("[stress] phase=%s deterministic=%u forced_yield_every=%u shard_rings=%u shard_rings_multishot=%u dynamic_shards=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u sqpoll_cpu=%d\n",
           phase_name,
           opts->deterministic,
           opts->forced_yield_every,
           stress_suite_runtime_flag_enabled(opts, LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS),
           stress_suite_runtime_flag_enabled(opts, LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT),
           stress_suite_runtime_flag_enabled(opts, LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS),
           stress_suite_runtime_flag_enabled(opts, LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ),
           stress_suite_runtime_flag_enabled(opts, LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC),
           stress_suite_runtime_flag_enabled(opts, LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL),
           opts->sqpoll_cpu);
    if (llam_runtime_init(opts) != 0) {
        perror("llam_runtime_init");
        return 1;
    }
    if (llam_spawn(task_fn,
                 arg,
                 &(llam_spawn_opts_t){
                     .task_class = LLAM_TASK_CLASS_DEFAULT,
                     .stack_class = LLAM_STACK_CLASS_DEFAULT,
                     .flags = LLAM_SPAWN_F_PINNED,
                 }) == NULL) {
        perror("llam_spawn");
        llam_runtime_shutdown();
        return 1;
    }
    watchdog_started = stress_start_phase_watchdog(&watchdog, &watchdog_thread, phase_name);
    if (watchdog_started < 0) {
        llam_runtime_shutdown();
        return 1;
    }
    if (llam_run() != 0) {
        stress_stop_phase_watchdog(&watchdog, watchdog_thread, watchdog_started);
        perror("llam_run");
        llam_dump_runtime_state(STDOUT_FILENO);
        llam_runtime_shutdown();
        return 1;
    }
    stress_stop_phase_watchdog(&watchdog, watchdog_thread, watchdog_started);
    if (llam_runtime_collect_stats(&stats) != 0) {
        perror("llam_runtime_collect_stats");
        llam_runtime_shutdown();
        return 1;
    }
    stress_print_phase_stats(phase_name, &stats);
    if (require_dynamic_motion != 0U &&
        stats.dynamic_workers != 0U &&
        stats.active_workers > stats.online_workers_floor &&
        stats.online_workers_max <= stats.online_workers_min) {
        stress_fail_msg("dynamic phase did not move online shard range");
    }
    if (require_floor_reach != 0U &&
        stats.dynamic_workers != 0U &&
        stats.active_workers > stats.online_workers_floor &&
        stats.online_workers_min > stats.online_workers_floor) {
        stress_fail_msg("dynamic phase did not return to base online floor");
    }
    if (atomic_load(&g_failures) != failures_before) {
        llam_dump_runtime_state(STDOUT_FILENO);
        llam_runtime_shutdown();
        return 1;
    }
    llam_runtime_shutdown();
    return 0;
}
