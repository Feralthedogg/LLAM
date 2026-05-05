/**
 * @file examples/stress_entry.c
 * @brief Stress executable entry point and runtime option setup.
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

int stress_run_multi_phase(const char *phase_name,
                                  const stress_phase_entry_t *entries,
                                  unsigned entry_count,
                                  unsigned spawn_start_shard,
                                  unsigned override_online_floor,
                                  const nm_runtime_opts_t *opts,
                                  unsigned require_dynamic_motion,
                                  unsigned require_floor_reach) {
    nm_runtime_stats_t stats;
    unsigned failures_before = atomic_load(&g_failures);
    unsigned i;

    printf("[stress] phase=%s deterministic=%u forced_yield_every=%u shard_rings=%u shard_rings_multishot=%u dynamic_shards=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u sqpoll_cpu=%d\n",
           phase_name,
           opts->deterministic,
           opts->forced_yield_every,
           opts->experimental_shard_rings,
           opts->experimental_shard_rings_multishot,
           opts->experimental_dynamic_shards,
           opts->experimental_lockfree_normq,
           opts->experimental_huge_alloc,
           opts->experimental_sqpoll,
           opts->sqpoll_cpu);
    if (nm_runtime_init(opts) != 0) {
        perror("nm_runtime_init");
        return 1;
    }
    if (spawn_start_shard == UINT_MAX - 1U) {
        spawn_start_shard = g_nm_runtime.active_shards > 1U ? g_nm_runtime.active_shards - 2U : 0U;
    } else if (spawn_start_shard == UINT_MAX - 2U) {
        unsigned online_shards = atomic_load_explicit(&g_nm_runtime.online_shards, memory_order_acquire);

        spawn_start_shard = online_shards > 0U ? online_shards - 1U : 0U;
    }
    if (spawn_start_shard != UINT_MAX && spawn_start_shard < g_nm_runtime.active_shards) {
        g_nm_runtime.next_spawn_shard = spawn_start_shard;
    }
    if (override_online_floor != UINT_MAX && override_online_floor > 0U) {
        g_nm_runtime.dynamic_online_floor =
            override_online_floor < g_nm_runtime.active_shards ? override_online_floor : g_nm_runtime.active_shards;
    }
    for (i = 0U; i < entry_count; ++i) {
        if (entries[i].task_fn == NULL) {
            continue;
        }
        if (nm_spawn(entries[i].task_fn,
                     entries[i].arg,
                     &(nm_spawn_opts_t){
                         .task_class = NM_TASK_CLASS_DEFAULT,
                         .stack_class = NM_STACK_CLASS_DEFAULT,
                         .flags = NM_SPAWN_F_PINNED,
                     }) == NULL) {
            perror("nm_spawn");
            nm_runtime_shutdown();
            return 1;
        }
    }
    if (nm_run() != 0) {
        perror("nm_run");
        nm_dump_runtime_state(STDOUT_FILENO);
        nm_runtime_shutdown();
        return 1;
    }
    if (nm_runtime_collect_stats(&stats) != 0) {
        perror("nm_runtime_collect_stats");
        nm_runtime_shutdown();
        return 1;
    }
    stress_print_phase_stats(phase_name, &stats);
    if (require_dynamic_motion != 0U &&
        stats.dynamic_shards != 0U &&
        stats.active_shards > stats.online_shards_floor &&
        stats.online_shards_max <= stats.online_shards_min) {
        stress_fail_msg("dynamic phase did not move online shard range");
    }
    if (require_floor_reach != 0U &&
        stats.dynamic_shards != 0U &&
        stats.active_shards > stats.online_shards_floor &&
        stats.online_shards_min > stats.online_shards_floor) {
        stress_fail_msg("dynamic phase did not return to base online floor");
    }
    if (atomic_load(&g_failures) != failures_before) {
        nm_dump_runtime_state(STDOUT_FILENO);
        nm_runtime_shutdown();
        return 1;
    }
    nm_runtime_shutdown();
    return 0;
}

int main(void) {
    nm_runtime_opts_t runtime_opts = {
        .deterministic = 1U,
        .forced_yield_every = 1U,
        .experimental_shard_rings = 0U,
        .experimental_shard_rings_multishot = 0U,
        .experimental_dynamic_shards = 0U,
        .experimental_lockfree_normq = 1U,
        .experimental_huge_alloc = 0U,
        .experimental_sqpoll = 0U,
        .sqpoll_cpu = -1,
    };
    nm_runtime_opts_t dynamic_runtime_opts;
    unsigned stress_rounds = stress_round_count();
    unsigned deterministic_phase = 1U;
    unsigned dynamic_phase = 0U;
    unsigned dynamic_rounds;
    unsigned dynamic_sleep_tasks;
    unsigned dynamic_sleep_yields;
    unsigned dynamic_sleep_us;
    dynamic_suite_state_t dynamic_state;
    dynamic_live_poll_watch_state_t live_poll_state;
    dynamic_live_accept_watch_state_t live_accept_state;
    dynamic_foreign_poll_watch_state_t foreign_poll_state;
    dynamic_live_inflight_io_state_t inflight_io_state;
    dynamic_idle_poll_watch_state_t idle_poll_state;
    dynamic_idle_accept_watch_state_t idle_accept_state;
    dynamic_idle_recv_watch_state_t idle_recv_state;
    unsigned dynamic_live_poll_waiters;
    unsigned dynamic_live_poll_monitor_rounds;
    unsigned dynamic_live_poll_monitor_us;
    int rc = 0;

    runtime_opts.experimental_shard_rings = stress_env_flag_default("LLAM_EXPERIMENTAL_WORKER_RINGS", 0U);
    runtime_opts.experimental_shard_rings_multishot =
        stress_env_flag_default("LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT", 0U);
    runtime_opts.experimental_dynamic_shards = stress_env_flag_default("LLAM_EXPERIMENTAL_DYNAMIC_WORKERS", 1U);
    runtime_opts.experimental_lockfree_normq = stress_env_flag_default("LLAM_EXPERIMENTAL_LOCKFREE_NORMQ", 1U);
    runtime_opts.experimental_huge_alloc = stress_env_flag_default("LLAM_EXPERIMENTAL_HUGE_ALLOC", 0U);
    runtime_opts.experimental_sqpoll = stress_env_flag_default("LLAM_EXPERIMENTAL_SQPOLL", 0U);
    runtime_opts.sqpoll_cpu = stress_env_i32("LLAM_SQPOLL_CPU", -1, -1, 4096);
    deterministic_phase = stress_env_flag_default("LLAM_STRESS_DETERMINISTIC_PHASE", 1U);
    dynamic_phase = stress_env_flag_default("LLAM_STRESS_DYNAMIC_PHASE", runtime_opts.experimental_dynamic_shards != 0U ? 1U : 0U);
    dynamic_rounds = stress_env_u32("LLAM_STRESS_DYNAMIC_ROUNDS", 1U, 64U);
    dynamic_sleep_tasks = stress_env_u32("LLAM_STRESS_DYNAMIC_SLEEP_TASKS", 512U, 8192U);
    dynamic_sleep_yields = stress_env_u32("LLAM_STRESS_DYNAMIC_SLEEP_YIELDS", 4U, 64U);
    dynamic_sleep_us = stress_env_u32("LLAM_STRESS_DYNAMIC_SLEEP_US", 30000U, 1000000U);
    dynamic_live_poll_waiters = stress_env_u32("LLAM_STRESS_DYNAMIC_LIVE_POLL_WAITERS", 128U, 2048U);
    dynamic_live_poll_monitor_rounds = stress_env_u32("LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_ROUNDS", 256U, 4096U);
    dynamic_live_poll_monitor_us = stress_env_u32("LLAM_STRESS_DYNAMIC_LIVE_POLL_MONITOR_US", 2000U, 1000000U);
    dynamic_state.round_count = dynamic_rounds;
    dynamic_state.sleep_tasks = dynamic_sleep_tasks;
    dynamic_state.sleep_yields = dynamic_sleep_yields;
    dynamic_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    live_poll_state.waiter_count = dynamic_live_poll_waiters;
    live_poll_state.sleep_tasks = dynamic_sleep_tasks;
    live_poll_state.sleep_yields = dynamic_sleep_yields;
    live_poll_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    live_poll_state.monitor_rounds = dynamic_live_poll_monitor_rounds;
    live_poll_state.monitor_sleep_ns = (uint64_t)dynamic_live_poll_monitor_us * 1000ULL;
    live_poll_state.sv[0] = -1;
    live_poll_state.sv[1] = -1;
    live_accept_state.waiter_count = dynamic_live_poll_waiters;
    live_accept_state.sleep_tasks = dynamic_sleep_tasks;
    live_accept_state.sleep_yields = dynamic_sleep_yields;
    live_accept_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    live_accept_state.monitor_rounds = dynamic_live_poll_monitor_rounds;
    live_accept_state.monitor_sleep_ns = (uint64_t)dynamic_live_poll_monitor_us * 1000ULL;
    live_accept_state.listener_fd = -1;
    live_accept_state.port = 0U;
    foreign_poll_state.waiter_count = dynamic_live_poll_waiters;
    foreign_poll_state.sleep_tasks = dynamic_sleep_tasks;
    foreign_poll_state.sleep_yields = dynamic_sleep_yields;
    foreign_poll_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    foreign_poll_state.monitor_rounds = dynamic_live_poll_monitor_rounds;
    foreign_poll_state.monitor_sleep_ns = (uint64_t)dynamic_live_poll_monitor_us * 1000ULL;
    foreign_poll_state.waiter_states = calloc(dynamic_live_poll_waiters, sizeof(*foreign_poll_state.waiter_states));
    foreign_poll_state.waiters = calloc(dynamic_live_poll_waiters, sizeof(*foreign_poll_state.waiters));
    if (foreign_poll_state.waiter_states == NULL || foreign_poll_state.waiters == NULL) {
        fprintf(stderr, "[stress] foreign poll alloc failed\n");
        stress_cleanup_dynamic_foreign_poll_state(&foreign_poll_state);
        return 1;
    }
    stress_reset_dynamic_foreign_poll_state(&foreign_poll_state);
    inflight_io_state.waiter_count = dynamic_live_poll_waiters;
    inflight_io_state.sleep_tasks = dynamic_sleep_tasks;
    inflight_io_state.sleep_yields = dynamic_sleep_yields;
    inflight_io_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    inflight_io_state.monitor_rounds = dynamic_live_poll_monitor_rounds;
    inflight_io_state.monitor_sleep_ns = (uint64_t)dynamic_live_poll_monitor_us * 1000ULL;
    idle_poll_state.sleep_tasks = dynamic_sleep_tasks;
    idle_poll_state.sleep_yields = dynamic_sleep_yields;
    idle_poll_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    idle_poll_state.sv[0] = -1;
    idle_poll_state.sv[1] = -1;
    idle_accept_state.sleep_tasks = dynamic_sleep_tasks;
    idle_accept_state.sleep_yields = dynamic_sleep_yields;
    idle_accept_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    idle_accept_state.listener_fd = -1;
    idle_accept_state.port = 0U;
    idle_recv_state.sleep_tasks = dynamic_sleep_tasks;
    idle_recv_state.sleep_yields = dynamic_sleep_yields;
    idle_recv_state.sleep_ns = (uint64_t)dynamic_sleep_us * 1000ULL;
    idle_recv_state.sv[0] = -1;
    idle_recv_state.sv[1] = -1;
    dynamic_runtime_opts = runtime_opts;
    dynamic_runtime_opts.deterministic = 0U;
    dynamic_runtime_opts.forced_yield_every = 0U;
    if (deterministic_phase == 0U && dynamic_phase == 0U) {
        fprintf(stderr, "[stress] no phases enabled\n");
        rc = 1;
        goto cleanup;
    }
    printf("[stress] config rounds=%u deterministic_phase=%u dynamic_phase=%u dynamic_rounds=%u dynamic_sleep_tasks=%u dynamic_sleep_yields=%u dynamic_sleep_us=%u dynamic_live_poll_waiters=%u dynamic_live_poll_monitor_rounds=%u dynamic_live_poll_monitor_us=%u shard_rings=%u shard_rings_multishot=%u dynamic_shards=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u sqpoll_cpu=%d\n",
           stress_rounds,
           deterministic_phase,
           dynamic_phase,
           dynamic_rounds,
           dynamic_sleep_tasks,
           dynamic_sleep_yields,
           dynamic_sleep_us,
           dynamic_live_poll_waiters,
           dynamic_live_poll_monitor_rounds,
           dynamic_live_poll_monitor_us,
           runtime_opts.experimental_shard_rings,
           runtime_opts.experimental_shard_rings_multishot,
           runtime_opts.experimental_dynamic_shards,
           runtime_opts.experimental_lockfree_normq,
           runtime_opts.experimental_huge_alloc,
           runtime_opts.experimental_sqpoll,
           runtime_opts.sqpoll_cpu);
    atomic_init(&g_failures, 0U);
    if (deterministic_phase != 0U &&
        stress_run_phase("deterministic", stress_suite_task, &stress_rounds, &runtime_opts, 0U, 0U) != 0) {
        fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
        rc = 1;
        goto cleanup;
    }
    if (dynamic_phase != 0U &&
        stress_run_phase("dynamic", dynamic_suite_task, &dynamic_state, &dynamic_runtime_opts, 1U, 0U) != 0) {
        fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
        rc = 1;
        goto cleanup;
    }
    if (dynamic_phase != 0U &&
        stress_run_phase("dynamic_live_accept_watch",
                         dynamic_live_accept_watch_task,
                         &live_accept_state,
                         &dynamic_runtime_opts,
                         1U,
                         0U) != 0) {
        fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
        rc = 1;
        goto cleanup;
    }
    if (dynamic_phase != 0U && stress_platform_supports_inflight_io_stress()) {
        if (stress_run_phase("dynamic_live_inflight_io",
                             dynamic_live_inflight_io_task,
                             &inflight_io_state,
                             &dynamic_runtime_opts,
                             1U,
                             0U) != 0) {
            fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
            rc = 1;
            goto cleanup;
        }
    } else if (dynamic_phase != 0U) {
        stress_print_phase_skipped("dynamic_live_inflight_io", "platform uses reduced Darwin inflight profile");
    }
    if (dynamic_phase != 0U && stress_platform_supports_poll_watch_stress()) {
        if (stress_run_phase("dynamic_live_poll_watch",
                             dynamic_live_poll_watch_task,
                             &live_poll_state,
                             &dynamic_runtime_opts,
                             1U,
                             0U) != 0) {
            fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
            rc = 1;
            goto cleanup;
        }
    } else if (dynamic_phase != 0U) {
        stress_print_phase_skipped("dynamic_live_poll_watch", "platform uses reduced Darwin poll-watch profile");
    }
    if (dynamic_phase != 0U &&
        dynamic_runtime_opts.experimental_shard_rings_multishot != 0U &&
        stress_platform_supports_foreign_poll_watch()) {
        const stress_phase_entry_t foreign_poll_entries[] = {
            {
                .task_fn = dynamic_foreign_poll_watch_scale_task,
                .arg = &foreign_poll_state,
            },
            {
                .task_fn = dynamic_foreign_poll_watch_monitor_task,
                .arg = &foreign_poll_state,
            },
        };

        memset(foreign_poll_state.waiter_states, 0, foreign_poll_state.waiter_count * sizeof(*foreign_poll_state.waiter_states));
        memset(foreign_poll_state.waiters, 0, foreign_poll_state.waiter_count * sizeof(*foreign_poll_state.waiters));
        stress_reset_dynamic_foreign_poll_state(&foreign_poll_state);
        if (stress_run_multi_phase("dynamic_foreign_poll_watch",
                                   foreign_poll_entries,
                                   (unsigned)(sizeof(foreign_poll_entries) / sizeof(foreign_poll_entries[0])),
                                   UINT_MAX,
                                   UINT_MAX,
                                   &dynamic_runtime_opts,
                                   1U,
                                   1U) != 0) {
            fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
            rc = 1;
            goto cleanup;
        }
    } else if (dynamic_phase != 0U &&
               dynamic_runtime_opts.experimental_shard_rings_multishot != 0U &&
               !stress_platform_supports_foreign_poll_watch()) {
        stress_print_phase_skipped("dynamic_foreign_poll_watch", "platform lacks foreign watch rehome backend");
    }
    if (dynamic_phase != 0U &&
        stress_run_phase("dynamic_idle_accept_watch",
                         dynamic_idle_accept_watch_task,
                         &idle_accept_state,
                         &dynamic_runtime_opts,
                         1U,
                         1U) != 0) {
        fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
        rc = 1;
        goto cleanup;
    }
    if (dynamic_phase != 0U && stress_platform_supports_poll_watch_stress()) {
        if (stress_run_phase("dynamic_idle_poll_watch",
                             dynamic_idle_poll_watch_task,
                             &idle_poll_state,
                             &dynamic_runtime_opts,
                             1U,
                             1U) != 0) {
            fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
            rc = 1;
            goto cleanup;
        }
    } else if (dynamic_phase != 0U) {
        stress_print_phase_skipped("dynamic_idle_poll_watch", "platform uses reduced Darwin poll-watch profile");
    }
    if (dynamic_phase != 0U && stress_platform_supports_recv_watch()) {
        if (stress_run_phase("dynamic_idle_recv_watch",
                             dynamic_idle_recv_watch_task,
                             &idle_recv_state,
                             &dynamic_runtime_opts,
                             1U,
                             1U) != 0) {
            fprintf(stderr, "[stress] failures=%u\n", atomic_load(&g_failures));
            rc = 1;
            goto cleanup;
        }
    } else if (dynamic_phase != 0U) {
        stress_print_phase_skipped("dynamic_idle_recv_watch", "platform lacks recv watch backend");
    }

cleanup:
    stress_close_fd_pair(live_poll_state.sv);
    if (live_accept_state.listener_fd >= 0) {
        close(live_accept_state.listener_fd);
        live_accept_state.listener_fd = -1;
    }
    stress_cleanup_dynamic_foreign_poll_state(&foreign_poll_state);
    if (idle_accept_state.listener_fd >= 0) {
        close(idle_accept_state.listener_fd);
        idle_accept_state.listener_fd = -1;
    }
    stress_close_fd_pair(idle_poll_state.sv);
    stress_close_fd_pair(idle_recv_state.sv);
    if (rc == 0) {
        printf("[stress] ok\n");
    }
    return rc;
}
