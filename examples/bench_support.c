/**
 * @file examples/bench_support.c
 * @brief Shared benchmark environment parsing, timing, reporting, and platform helpers.
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

static void bench_fail(atomic_uint *failures, const char *label) {
    fprintf(stderr, "[bench] fail: %s\n", label);
    atomic_fetch_add(failures, 1U);
}

unsigned bench_env_u32(const char *name, unsigned default_value, unsigned max_value) {
    const char *value = llam_example_env_get(name);
    char *end = NULL;
    unsigned long parsed = 0UL;

    if (value == NULL || *value == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0UL) {
        return default_value;
    }
    if (parsed > max_value) {
        parsed = max_value;
    }
    return (unsigned)parsed;
}

unsigned bench_env_u32_allow_zero(const char *name, unsigned default_value, unsigned max_value) {
    const char *value = llam_example_env_get(name);
    char *end = NULL;
    unsigned long parsed = 0UL;

    if (value == NULL || *value == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return default_value;
    }
    if (parsed > max_value) {
        parsed = max_value;
    }
    return (unsigned)parsed;
}

unsigned bench_env_flag_default(const char *name, unsigned default_value) {
    const char *value = llam_example_env_get(name);

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    return strcmp(value, "0") != 0 ? 1U : 0U;
}

int bench_env_i32(const char *name, int default_value, int min_value, int max_value) {
    const char *value = llam_example_env_get(name);
    char *end = NULL;
    long parsed = 0L;

    if (value == NULL || *value == '\0') {
        return default_value;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return default_value;
    }
    if (parsed < (long)min_value) {
        parsed = (long)min_value;
    }
    if (parsed > (long)max_value) {
        parsed = (long)max_value;
    }
    return (int)parsed;
}

static bool bench_platform_prefers_indefinite_ready_poll(void) {
    const char *env = llam_example_env_get("LLAM_BENCH_POLL_INDEFINITE");

    if (env != NULL && env[0] != '\0') {
        return strcmp(env, "0") != 0;
    }

#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool bench_platform_supports_darwin_io_cases(void) {
    return true;
}

bool bench_case_selected(const char *name) {
    const char *only = llam_example_env_get("LLAM_BENCH_ONLY");

    if (only == NULL || only[0] == '\0') {
        return true;
    }
    return strcmp(only, name) == 0;
}

static bool bench_csv_enabled(void) {
    return bench_env_flag_default("LLAM_BENCH_CSV", 0U) != 0U;
}

static int bench_compare_u64(const void *lhs, const void *rhs) {
    uint64_t a = *(const uint64_t *)lhs;
    uint64_t b = *(const uint64_t *)rhs;

    if (a < b) {
        return -1;
    }
    if (a > b) {
        return 1;
    }
    return 0;
}

static uint64_t bench_percentile_ns(const uint64_t *samples, unsigned count, unsigned percentile) {
    unsigned long long scaled = 0ULL;
    unsigned index = 0U;

    if (samples == NULL || count == 0U) {
        return 0U;
    }

    scaled = (unsigned long long)(count - 1U) * (unsigned long long)percentile + 99ULL;
    index = (unsigned)(scaled / 100ULL);
    if (index >= count) {
        index = count - 1U;
    }
    return samples[index];
}

void bench_print_report(const char *name,
                        unsigned total_rounds,
                        unsigned warmup_rounds,
                        unsigned ops_per_round,
                        uint64_t *samples_ns,
                        const llam_runtime_stats_t *stats) {
    unsigned measured_rounds = total_rounds;
    uint64_t *measured_samples = samples_ns;
    uint64_t total_ns = 0U;
    uint64_t p50_ns;
    uint64_t p99_ns;
    double ops_per_sec = 0.0;
    double opaque_block_avg_us = 0.0;
    double opaque_enter_wait_avg_us = 0.0;
    double opaque_leave_wait_avg_us = 0.0;
    unsigned i;

    if (warmup_rounds < total_rounds) {
        measured_rounds = total_rounds - warmup_rounds;
        measured_samples = samples_ns + warmup_rounds;
    } else {
        warmup_rounds = 0U;
    }

    for (i = 0; i < measured_rounds; ++i) {
        total_ns += measured_samples[i];
    }
    qsort(measured_samples, measured_rounds, sizeof(*measured_samples), bench_compare_u64);
    p50_ns = bench_percentile_ns(measured_samples, measured_rounds, 50U);
    p99_ns = bench_percentile_ns(measured_samples, measured_rounds, 99U);
    if (total_ns > 0U) {
        ops_per_sec = (double)ops_per_round * (double)measured_rounds * 1000000000.0 / (double)total_ns;
    }
    if (stats->opaque_block_samples > 0U) {
        opaque_block_avg_us = (double)stats->opaque_block_ns / (double)stats->opaque_block_samples / 1000.0;
    }
    if (stats->opaque_enter_wait_samples > 0U) {
        opaque_enter_wait_avg_us =
            (double)stats->opaque_enter_wait_ns / (double)stats->opaque_enter_wait_samples / 1000.0;
    }
    if (stats->opaque_leave_wait_samples > 0U) {
        opaque_leave_wait_avg_us =
            (double)stats->opaque_leave_wait_ns / (double)stats->opaque_leave_wait_samples / 1000.0;
    }

    printf("[bench] name=%s rounds=%u warmup=%u ops=%u ops_per_sec=%.2f p50_us=%.2f p99_us=%.2f ctx_switches=%llu idle_polls=%llu idle_spin_hits=%llu idle_spin_fallbacks=%llu idle_spin_us=%.2f io_submits=%llu io_submit_calls=%llu io_submit_syscalls=%llu overflows=%llu overflow_depth=%llu dynamic_workers=%u online_workers=%u online_workers_floor=%u online_workers_min=%u online_workers_max=%u worker_rings=%u worker_rings_multishot=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u active_workers=%u active_nodes=%u opaque_block_avg_us=%.2f opaque_block_max_us=%.2f opaque_block_samples=%llu opaque_enter_wait_avg_us=%.2f opaque_enter_wait_max_us=%.2f opaque_enter_wait_samples=%llu opaque_leave_wait_avg_us=%.2f opaque_leave_wait_max_us=%.2f opaque_leave_wait_samples=%llu\n",
           name,
           measured_rounds,
           warmup_rounds,
           ops_per_round,
           ops_per_sec,
           (double)p50_ns / 1000.0,
           (double)p99_ns / 1000.0,
           (unsigned long long)stats->ctx_switches,
           (unsigned long long)stats->idle_polls,
           (unsigned long long)stats->idle_spin_hits,
           (unsigned long long)stats->idle_spin_fallbacks,
           (double)stats->idle_spin_ns / 1000.0,
           (unsigned long long)stats->io_submits,
           (unsigned long long)stats->io_submit_calls,
           (unsigned long long)stats->io_submit_syscalls,
           (unsigned long long)stats->queue_overflows,
           (unsigned long long)stats->overflow_depth,
           stats->dynamic_workers,
           stats->online_workers,
           stats->online_workers_floor,
           stats->online_workers_min,
           stats->online_workers_max,
           stats->worker_rings,
           stats->worker_rings_multishot,
           stats->lockfree_normq,
           stats->huge_alloc,
           stats->sqpoll,
           stats->active_workers,
           stats->active_nodes,
           opaque_block_avg_us,
           (double)stats->opaque_block_max_ns / 1000.0,
           (unsigned long long)stats->opaque_block_samples,
           opaque_enter_wait_avg_us,
           (double)stats->opaque_enter_wait_max_ns / 1000.0,
           (unsigned long long)stats->opaque_enter_wait_samples,
           opaque_leave_wait_avg_us,
           (double)stats->opaque_leave_wait_max_ns / 1000.0,
           (unsigned long long)stats->opaque_leave_wait_samples);
    if (bench_csv_enabled()) {
        printf("[bench-csv] name,rounds,warmup,ops,ops_per_sec,p50_us,p99_us\n");
        printf("[bench-csv] %s,%u,%u,%u,%.2f,%.2f,%.2f\n",
               name,
               measured_rounds,
               warmup_rounds,
               ops_per_round,
               ops_per_sec,
               (double)p50_ns / 1000.0,
               (double)p99_ns / 1000.0);
    }
}

static void bench_spawn_child_task(void *arg) {
    unsigned yields = *(unsigned *)arg;
    unsigned i;

    for (i = 0; i < yields; ++i) {
        llam_yield();
    }
}

void bench_spawn_task(void *arg) {
    bench_spawn_state_t *state = arg;
    llam_task_t **tasks = calloc(state->tasks_per_round, sizeof(*tasks));
    unsigned round;

    if (tasks == NULL) {
        bench_fail(&state->failures, "spawn/join task array alloc failed");
        return;
    }
    for (round = 0; round < state->rounds; ++round) {
        uint64_t start_ns;
        unsigned i;

        memset(tasks, 0, state->tasks_per_round * sizeof(*tasks));
        start_ns = llam_now_ns();
        for (i = 0; i < state->tasks_per_round; ++i) {
            tasks[i] = llam_spawn(bench_spawn_child_task,
                                &state->yields_per_task,
                                &(llam_spawn_opts_t){
                                    .task_class = LLAM_TASK_CLASS_DEFAULT,
                                    .stack_class = LLAM_STACK_CLASS_DEFAULT,
            });
            if (tasks[i] == NULL) {
                bench_fail(&state->failures, "spawn/join child spawn failed");
                goto done;
            }
        }
        for (i = 0; i < state->tasks_per_round; ++i) {
            if (llam_join(tasks[i]) != 0) {
                bench_fail(&state->failures, "spawn/join child join failed");
                goto done;
            }
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
    }

done:
    free(tasks);
}

static void bench_ping_peer_task(void *arg) {
    bench_ping_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->messages_per_round; ++i) {
        void *value = llam_channel_recv(state->request);

        if (value == NULL) {
            bench_fail(&state->failures, "channel ping recv failed");
            return;
        }
        if (llam_channel_send(state->response, value) != 0) {
            bench_fail(&state->failures, "channel ping send failed");
            return;
        }
    }
}

void bench_channel_task(void *arg) {
    bench_ping_state_t *state = arg;
    unsigned round;

    for (round = 0; round < state->rounds; ++round) {
        llam_channel_t *request = llam_channel_create(1U);
        llam_channel_t *response = llam_channel_create(1U);
        llam_task_t *peer;
        uint64_t start_ns;
        unsigned i;

        if (request == NULL || response == NULL) {
            bench_fail(&state->failures, "channel create failed");
            if (request != NULL) {
                llam_channel_destroy(request);
            }
            if (response != NULL) {
                llam_channel_destroy(response);
            }
            return;
        }

        state->request = request;
        state->response = response;
        peer = llam_spawn(bench_ping_peer_task,
                        state,
                        &(llam_spawn_opts_t){
                            .task_class = LLAM_TASK_CLASS_DEFAULT,
                            .stack_class = LLAM_STACK_CLASS_DEFAULT,
                        });
        if (peer == NULL) {
            bench_fail(&state->failures, "channel peer spawn failed");
            llam_channel_destroy(request);
            llam_channel_destroy(response);
            return;
        }

        start_ns = llam_now_ns();
        for (i = 0; i < state->messages_per_round; ++i) {
            void *token = (void *)(uintptr_t)(i + 1U);
            void *echo;

            if (llam_channel_send(request, token) != 0) {
                bench_fail(&state->failures, "channel send failed");
                llam_channel_destroy(request);
                llam_channel_destroy(response);
                return;
            }
            echo = llam_channel_recv(response);
            if (echo != token) {
                bench_fail(&state->failures, "channel recv mismatch");
                llam_channel_destroy(request);
                llam_channel_destroy(response);
                return;
            }
        }
        if (llam_join(peer) != 0) {
            bench_fail(&state->failures, "channel peer join failed");
            llam_channel_destroy(request);
            llam_channel_destroy(response);
            return;
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
        state->request = NULL;
        state->response = NULL;
        llam_channel_destroy(request);
        llam_channel_destroy(response);
    }
}

static void bench_echo_peer_task(void *arg) {
    bench_echo_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->messages_per_round; ++i) {
        char byte = 0;

        if (llam_read(state->fd, &byte, 1U) != 1) {
            return;
        }
        if (llam_write(state->fd, &byte, 1U) != 1) {
            return;
        }
    }
}

void bench_io_task(void *arg) {
    bench_io_state_t *state = arg;
    unsigned round;

    for (round = 0; round < state->rounds; ++round) {
        int sv[2];
        bench_echo_state_t peer_state;
        llam_task_t *peer;
        uint64_t start_ns;
        unsigned i;

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            bench_fail(&state->failures, "io socketpair failed");
            return;
        }

        peer_state.fd = sv[1];
        peer_state.messages_per_round = state->messages_per_round;
        peer = llam_spawn(bench_echo_peer_task,
                        &peer_state,
                        &(llam_spawn_opts_t){
                            .task_class = LLAM_TASK_CLASS_DEFAULT,
                            .stack_class = LLAM_STACK_CLASS_DEFAULT,
                        });
        if (peer == NULL) {
            bench_fail(&state->failures, "io peer spawn failed");
            close(sv[0]);
            close(sv[1]);
            return;
        }

        start_ns = llam_now_ns();
        for (i = 0; i < state->messages_per_round; ++i) {
            char out = (char)(i & 0x7F);
            char in = 0;

            if (llam_write(sv[0], &out, 1U) != 1) {
                bench_fail(&state->failures, "io write failed");
                close(sv[0]);
                close(sv[1]);
                return;
            }
            if (llam_read(sv[0], &in, 1U) != 1 || in != out) {
                bench_fail(&state->failures, "io read mismatch");
                close(sv[0]);
                close(sv[1]);
                return;
            }
        }
        if (llam_join(peer) != 0) {
            bench_fail(&state->failures, "io peer join failed");
            close(sv[0]);
            close(sv[1]);
            return;
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
        close(sv[0]);
        close(sv[1]);
    }
}

static void bench_poll_writer_task(void *arg) {
    bench_poll_writer_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->events_per_round; ++i) {
        char byte = 'p';

        llam_yield();
        if (llam_write(state->fd, &byte, 1U) != 1) {
            return;
        }
    }
}

void bench_poll_task(void *arg) {
    bench_poll_state_t *state = arg;
    unsigned round;

    for (round = 0; round < state->rounds; ++round) {
        int sv[2];
        bench_poll_writer_state_t writer_state;
        llam_task_t *writer;
        uint64_t start_ns;
        unsigned i;

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            bench_fail(&state->failures, "poll socketpair failed");
            return;
        }

        writer_state.fd = sv[1];
        writer_state.events_per_round = state->events_per_round;
        writer = llam_spawn(bench_poll_writer_task,
                          &writer_state,
                          &(llam_spawn_opts_t){
                              .task_class = LLAM_TASK_CLASS_DEFAULT,
                              .stack_class = LLAM_STACK_CLASS_DEFAULT,
                          });
        if (writer == NULL) {
            bench_fail(&state->failures, "poll writer spawn failed");
            close(sv[0]);
            close(sv[1]);
            return;
        }

        start_ns = llam_now_ns();
        for (i = 0; i < state->events_per_round; ++i) {
            short revents = 0;
            char byte = 0;
            int poll_rc;

            poll_rc = llam_poll_fd(sv[0], POLLIN, bench_platform_prefers_indefinite_ready_poll() ? -1 : 1000, &revents);
            if (poll_rc != 1 || (revents & POLLIN) == 0) {
                fprintf(stderr,
                        "[bench] poll_wait rc=%d revents=0x%x errno=%d round=%u event=%u\n",
                        poll_rc,
                        (unsigned)revents,
                        errno,
                        round,
                        i);
                bench_fail(&state->failures, "poll wait failed");
                close(sv[0]);
                close(sv[1]);
                return;
            }
            if (llam_read(sv[0], &byte, 1U) != 1) {
                bench_fail(&state->failures, "poll read failed");
                close(sv[0]);
                close(sv[1]);
                return;
            }
        }
        if (llam_join(writer) != 0) {
            bench_fail(&state->failures, "poll writer join failed");
            close(sv[0]);
            close(sv[1]);
            return;
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
        close(sv[0]);
        close(sv[1]);
    }
}

static void bench_opaque_companion_task(void *arg) {
    unsigned yields = *(unsigned *)arg;
    unsigned i;

    for (i = 0; i < yields; ++i) {
        llam_yield();
    }
}

static void bench_blocking_sleep_us(unsigned sleep_us) {
    for (;;) {
        struct timeval tv;
        int rc;

        tv.tv_sec = (time_t)(sleep_us / 1000000U);
        tv.tv_usec = (suseconds_t)(sleep_us % 1000000U);
#if defined(__linux__) && defined(SYS_select)
        rc = (int)syscall(SYS_select, 0, NULL, NULL, NULL, &tv);
#else
        rc = select(0, NULL, NULL, NULL, &tv);
#endif
        if (rc == 0) {
            return;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return;
    }
}

void bench_opaque_task(void *arg) {
    bench_opaque_state_t *state = arg;
    unsigned round;

    for (round = 0; round < state->rounds; ++round) {
        uint64_t start_ns = llam_now_ns();
        unsigned i;

        for (i = 0; i < state->scopes_per_round; ++i) {
            llam_task_t *companion = llam_spawn(bench_opaque_companion_task,
                                            &state->companion_yields,
                                            &(llam_spawn_opts_t){
                                                .task_class = LLAM_TASK_CLASS_DEFAULT,
                                                .stack_class = LLAM_STACK_CLASS_DEFAULT,
                                            });

            if (companion == NULL) {
                bench_fail(&state->failures, "opaque companion spawn failed");
                return;
            }
            if (llam_enter_blocking() != 0) {
                bench_fail(&state->failures, "opaque enter failed");
                return;
            }
            bench_blocking_sleep_us(state->sleep_us);
            if (llam_leave_blocking() != 0) {
                bench_fail(&state->failures, "opaque leave failed");
                return;
            }
            if (llam_join(companion) != 0) {
                bench_fail(&state->failures, "opaque companion join failed");
                return;
            }
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
    }
}

static void bench_sleep_child_task(void *arg) {
    bench_sleep_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->pre_sleep_yields; ++i) {
        llam_yield();
    }
    if (llam_sleep_ns(state->sleep_ns) != 0) {
        bench_fail(&state->failures, "sleep_fanout child sleep failed");
    }
}

void bench_sleep_task(void *arg) {
    bench_sleep_state_t *state = arg;
    llam_task_t **tasks = calloc(state->tasks_per_round, sizeof(*tasks));
    unsigned round;

    if (tasks == NULL) {
        bench_fail(&state->failures, "sleep_fanout task array alloc failed");
        return;
    }
    for (round = 0; round < state->rounds; ++round) {
        uint64_t start_ns;
        unsigned i;

        memset(tasks, 0, state->tasks_per_round * sizeof(*tasks));
        start_ns = llam_now_ns();
        for (i = 0; i < state->tasks_per_round; ++i) {
            tasks[i] = llam_spawn(bench_sleep_child_task,
                                state,
                                &(llam_spawn_opts_t){
                                    .task_class = LLAM_TASK_CLASS_DEFAULT,
                                    .stack_class = LLAM_STACK_CLASS_DEFAULT,
            });
            if (tasks[i] == NULL) {
                bench_fail(&state->failures, "sleep_fanout child spawn failed");
                goto done;
            }
        }
        for (i = 0; i < state->tasks_per_round; ++i) {
            if (llam_join(tasks[i]) != 0) {
                bench_fail(&state->failures, "sleep_fanout child join failed");
                goto done;
            }
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
    }

done:
    free(tasks);
}
