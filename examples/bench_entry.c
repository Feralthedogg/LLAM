/**
 * @file examples/bench_entry.c
 * @brief Benchmark driver that configures LLAM and runs each benchmark case.
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

#include "bench_internal.h"

static int bench_run_case(const char *name,
                          llam_task_fn entry,
                          void *state,
                          uint64_t *samples_ns,
                          unsigned total_rounds,
                          unsigned warmup_rounds,
                          unsigned ops_per_round,
                          const llam_runtime_opts_t *opts) {
    llam_runtime_stats_t stats;

    if (llam_runtime_init(opts) != 0) {
        perror("llam_runtime_init");
        return -1;
    }
    if (llam_spawn(entry,
                 state,
                 &(llam_spawn_opts_t){
                     .task_class = LLAM_TASK_CLASS_DEFAULT,
                     .stack_class = LLAM_STACK_CLASS_DEFAULT,
                     .flags = LLAM_SPAWN_F_PINNED,
                 }) == NULL) {
        perror("llam_spawn");
        llam_runtime_shutdown();
        return -1;
    }
    if (llam_run() != 0) {
        perror("llam_run");
        llam_dump_runtime_state(STDOUT_FILENO);
        llam_runtime_shutdown();
        return -1;
    }
    if (llam_runtime_collect_stats(&stats) != 0) {
        perror("llam_runtime_collect_stats");
        llam_runtime_shutdown();
        return -1;
    }
    bench_print_report(name, total_rounds, warmup_rounds, ops_per_round, samples_ns, &stats);
    llam_runtime_shutdown();
    return 0;
}

int main(void) {
    llam_runtime_opts_t runtime_opts = {
        .deterministic = 0U,
        .forced_yield_every = 0U,
        .experimental_worker_rings = 0U,
        .experimental_worker_rings_multishot = 0U,
        .experimental_dynamic_workers = 0U,
        .experimental_lockfree_normq = 1U,
        .experimental_huge_alloc = 0U,
    };
    unsigned rounds = bench_env_u32("LLAM_BENCH_ROUNDS", 21U, 512U);
    unsigned warmup_rounds = bench_env_u32_allow_zero("LLAM_BENCH_WARMUP_ROUNDS", 0U, 128U);
    unsigned total_rounds = rounds + warmup_rounds;
    unsigned spawn_tasks = bench_env_u32("LLAM_BENCH_SPAWN_TASKS", 128U, 4096U);
    unsigned channel_messages = bench_env_u32("LLAM_BENCH_CHANNEL_MESSAGES", 1024U, 16384U);
    unsigned io_messages = bench_env_u32("LLAM_BENCH_IO_MESSAGES", 256U, 8192U);
    unsigned poll_events = bench_env_u32("LLAM_BENCH_POLL_EVENTS", 256U, 8192U);
    unsigned sleep_tasks = bench_env_u32("LLAM_BENCH_SLEEP_TASKS", spawn_tasks < 512U ? 512U : spawn_tasks, 8192U);
    unsigned sleep_yields = bench_env_u32("LLAM_BENCH_SLEEP_YIELDS", 4U, 64U);
    unsigned sleep_us = bench_env_u32("LLAM_BENCH_SLEEP_US", 30000U, 1000000U);
    unsigned opaque_scopes = bench_env_u32("LLAM_BENCH_OPAQUE_SCOPES", 16U, 1024U);
    unsigned idle_spin_ns = bench_env_u32("LLAM_IDLE_SPIN_NS", 0U, 1000000U);
    unsigned idle_spin_iters = bench_env_u32("LLAM_IDLE_SPIN_ITERS", 0U, 1000000U);
    int sqpoll_cpu = bench_env_i32("LLAM_SQPOLL_CPU", -1, -1, 4096);
    const char *runtime_profile = llam_example_env_get("LLAM_RUNTIME_PROFILE");
    uint64_t *samples = calloc(total_rounds, sizeof(*samples));
    int rc = 0;

    runtime_opts.experimental_worker_rings = bench_env_flag_default("LLAM_EXPERIMENTAL_WORKER_RINGS", 0U);
    runtime_opts.experimental_worker_rings_multishot = bench_env_flag_default("LLAM_EXPERIMENTAL_WORKER_RINGS_MULTISHOT", 0U);
    runtime_opts.experimental_dynamic_workers = bench_env_flag_default("LLAM_EXPERIMENTAL_DYNAMIC_WORKERS", 1U);
    runtime_opts.experimental_lockfree_normq = bench_env_flag_default("LLAM_EXPERIMENTAL_LOCKFREE_NORMQ", 1U);
    runtime_opts.experimental_huge_alloc = bench_env_flag_default("LLAM_EXPERIMENTAL_HUGE_ALLOC", 0U);
    runtime_opts.idle_spin_ns = idle_spin_ns;
    runtime_opts.idle_spin_max_iters = idle_spin_iters;
    runtime_opts.experimental_sqpoll = bench_env_flag_default("LLAM_EXPERIMENTAL_SQPOLL", 0U);
    runtime_opts.sqpoll_cpu = sqpoll_cpu;
    if (samples == NULL) {
        perror("calloc");
        return 1;
    }

    printf("[bench] config rounds=%u warmup=%u profile=%s worker_rings=%u worker_rings_multishot=%u dynamic_workers=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u sqpoll_cpu=%d idle_spin_ns=%u idle_spin_iters=%u spawn_tasks=%u channel_messages=%u io_messages=%u poll_events=%u sleep_tasks=%u sleep_yields=%u sleep_us=%u opaque_scopes=%u\n",
           rounds,
           warmup_rounds,
           runtime_profile != NULL && runtime_profile[0] != '\0' ? runtime_profile : "balanced",
           runtime_opts.experimental_worker_rings,
           runtime_opts.experimental_worker_rings_multishot,
           runtime_opts.experimental_dynamic_workers,
           runtime_opts.experimental_lockfree_normq,
           runtime_opts.experimental_huge_alloc,
           runtime_opts.experimental_sqpoll,
           runtime_opts.sqpoll_cpu,
           idle_spin_ns,
           idle_spin_iters,
           spawn_tasks,
           channel_messages,
           io_messages,
           poll_events,
           sleep_tasks,
           sleep_yields,
           sleep_us,
           opaque_scopes);

    if (bench_case_selected("spawn_join")) {
        bench_spawn_state_t state = {
            .rounds = total_rounds,
            .tasks_per_round = spawn_tasks,
            .yields_per_task = 2U,
            .samples_ns = samples,
        };

        atomic_init(&state.failures, 0U);
        memset(samples, 0, total_rounds * sizeof(*samples));
        rc = bench_run_case("spawn_join", bench_spawn_task, &state, samples, total_rounds, warmup_rounds, spawn_tasks, &runtime_opts);
        if (rc != 0 || atomic_load(&state.failures) != 0U) {
            rc = 1;
            goto done;
        }
    }

    if (bench_case_selected("channel_pingpong")) {
        bench_ping_state_t state = {
            .rounds = total_rounds,
            .messages_per_round = channel_messages,
            .request = NULL,
            .response = NULL,
            .samples_ns = samples,
        };

        atomic_init(&state.failures, 0U);
        memset(samples, 0, total_rounds * sizeof(*samples));
        rc = bench_run_case("channel_pingpong", bench_channel_task, &state, samples, total_rounds, warmup_rounds, channel_messages, &runtime_opts);
        if (rc != 0 || atomic_load(&state.failures) != 0U) {
            rc = 1;
            goto done;
        }
    }

    if (bench_case_selected("io_echo") && bench_platform_supports_darwin_io_cases()) {
        bench_io_state_t state = {
            .rounds = total_rounds,
            .messages_per_round = io_messages,
            .samples_ns = samples,
        };

        atomic_init(&state.failures, 0U);
        memset(samples, 0, total_rounds * sizeof(*samples));
        rc = bench_run_case("io_echo", bench_io_task, &state, samples, total_rounds, warmup_rounds, io_messages, &runtime_opts);
        if (rc != 0 || atomic_load(&state.failures) != 0U) {
            rc = 1;
            goto done;
        }
    } else if (bench_case_selected("io_echo")) {
        printf("[bench] name=io_echo skipped=platform uses reduced Darwin profile\n");
    }

    if (bench_case_selected("poll_wake") && bench_platform_supports_darwin_io_cases()) {
        bench_poll_state_t state = {
            .rounds = total_rounds,
            .events_per_round = poll_events,
            .samples_ns = samples,
        };

        atomic_init(&state.failures, 0U);
        memset(samples, 0, total_rounds * sizeof(*samples));
        rc = bench_run_case("poll_wake", bench_poll_task, &state, samples, total_rounds, warmup_rounds, poll_events, &runtime_opts);
        if (rc != 0 || atomic_load(&state.failures) != 0U) {
            rc = 1;
            goto done;
        }
    } else if (bench_case_selected("poll_wake")) {
        printf("[bench] name=poll_wake skipped=platform uses reduced Darwin profile\n");
    }

    if (bench_case_selected("sleep_fanout")) {
        bench_sleep_state_t state = {
            .rounds = total_rounds,
            .tasks_per_round = sleep_tasks,
            .pre_sleep_yields = sleep_yields,
            .sleep_ns = (uint64_t)sleep_us * 1000ULL,
            .samples_ns = samples,
        };

        atomic_init(&state.failures, 0U);
        memset(samples, 0, total_rounds * sizeof(*samples));
        rc = bench_run_case("sleep_fanout", bench_sleep_task, &state, samples, total_rounds, warmup_rounds, sleep_tasks, &runtime_opts);
        if (rc != 0 || atomic_load(&state.failures) != 0U) {
            rc = 1;
            goto done;
        }
    }

    if (bench_case_selected("opaque_block")) {
        bench_opaque_state_t state = {
            .rounds = total_rounds,
            .scopes_per_round = opaque_scopes,
            .companion_yields = 4U,
            .sleep_us = 200U,
            .samples_ns = samples,
        };

        atomic_init(&state.failures, 0U);
        memset(samples, 0, total_rounds * sizeof(*samples));
        rc = bench_run_case("opaque_block", bench_opaque_task, &state, samples, total_rounds, warmup_rounds, opaque_scopes, &runtime_opts);
        if (rc != 0 || atomic_load(&state.failures) != 0U) {
            rc = 1;
            goto done;
        }
    }

done:
    free(samples);
    return rc;
}
