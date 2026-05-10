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

#if defined(__APPLE__) || LLAM_PLATFORM_WINDOWS
    return true;
#else
    return false;
#endif
}

static void bench_close_fd(llam_fd_t fd) {
#if LLAM_PLATFORM_WINDOWS
    if (!LLAM_FD_IS_INVALID(fd)) {
        (void)closesocket(fd);
    }
#else
    (void)close(fd);
#endif
}

#if LLAM_PLATFORM_WINDOWS && LLAM_BENCH_HAVE_AFUNIX
static int bench_socketpair_windows_afunix(llam_fd_t sv[2]) {
    static atomic_uint sequence;
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_un addr;
    unsigned seq = atomic_fetch_add_explicit(&sequence, 1U, memory_order_relaxed);
    char path[UNIX_PATH_MAX];

    snprintf(path, sizeof(path), ".\\llam-bench-%lu-%u.sock", (unsigned long)GetCurrentProcessId(), seq);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        errno = EIO;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1U);
    (void)DeleteFileA(path);
    if (bind(listener, (const struct sockaddr *)&addr, (int)sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        (void)DeleteFileA(path);
        errno = EIO;
        return -1;
    }

    client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        (void)DeleteFileA(path);
        errno = EIO;
        return -1;
    }
    if (connect(client, (const struct sockaddr *)&addr, (int)sizeof(addr)) == SOCKET_ERROR) {
        closesocket(client);
        closesocket(listener);
        (void)DeleteFileA(path);
        errno = EIO;
        return -1;
    }
    server = accept(listener, NULL, NULL);
    closesocket(listener);
    (void)DeleteFileA(path);
    if (server == INVALID_SOCKET) {
        closesocket(client);
        errno = EIO;
        return -1;
    }

    sv[0] = (llam_fd_t)client;
    sv[1] = (llam_fd_t)server;
    return 0;
}
#endif

static int bench_socketpair(llam_fd_t sv[2]) {
#if LLAM_PLATFORM_WINDOWS
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addr_len = (int)sizeof(addr);
    BOOL no_delay = TRUE;

    if (sv == NULL) {
        errno = EINVAL;
        return -1;
    }
    sv[0] = LLAM_INVALID_FD;
    sv[1] = LLAM_INVALID_FD;

#if LLAM_BENCH_HAVE_AFUNIX
    if (bench_env_flag_default("LLAM_BENCH_WINDOWS_AFUNIX", 1U) != 0U &&
        bench_socketpair_windows_afunix(sv) == 0) {
        return 0;
    }
    sv[0] = LLAM_INVALID_FD;
    sv[1] = LLAM_INVALID_FD;
#endif

    listener = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET) {
        errno = EIO;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (const struct sockaddr *)&addr, (int)sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&addr, &addr_len) == SOCKET_ERROR) {
        closesocket(listener);
        errno = EIO;
        return -1;
    }

    client = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        errno = EIO;
        return -1;
    }
    if (connect(client, (const struct sockaddr *)&addr, addr_len) == SOCKET_ERROR) {
        closesocket(client);
        closesocket(listener);
        errno = EIO;
        return -1;
    }
    server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (server == INVALID_SOCKET) {
        closesocket(client);
        errno = EIO;
        return -1;
    }

    (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, (int)sizeof(no_delay));
    (void)setsockopt(server, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, (int)sizeof(no_delay));
    sv[0] = (llam_fd_t)client;
    sv[1] = (llam_fd_t)server;
    return 0;
#else
    int pair[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        return -1;
    }
    sv[0] = (llam_fd_t)pair[0];
    sv[1] = (llam_fd_t)pair[1];
    return 0;
#endif
}

static bool bench_reuse_socketpair_enabled(void) {
    const char *env = llam_example_env_get("LLAM_BENCH_REUSE_SOCKETPAIR");

    if (env != NULL && env[0] != '\0') {
        return strcmp(env, "0") != 0;
    }
#if LLAM_PLATFORM_WINDOWS
    return false;
#else
    return true;
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

    printf("[bench] name=%s rounds=%u warmup=%u ops=%u ops_per_sec=%.2f p50_us=%.2f p99_us=%.2f ctx_switches=%llu idle_polls=%llu idle_spin_hits=%llu idle_spin_fallbacks=%llu idle_spin_us=%.2f io_submits=%llu io_submit_calls=%llu io_submit_syscalls=%llu overflows=%llu overflow_depth=%llu dynamic_workers=%u online_workers=%u online_workers_floor=%u online_workers_min=%u online_workers_max=%u worker_rings=%u worker_rings_multishot=%u lockfree_normq=%u huge_alloc=%u sqpoll=%u active_workers=%u active_nodes=%u yield_direct_attempts=%llu yield_direct_fast_hits=%llu yield_direct_locked_hits=%llu yield_direct_fail_context=%llu yield_direct_fail_policy=%llu yield_direct_fail_no_work=%llu yield_direct_fail_self=%llu yield_direct_fail_push=%llu opaque_block_avg_us=%.2f opaque_block_max_us=%.2f opaque_block_samples=%llu opaque_enter_wait_avg_us=%.2f opaque_enter_wait_max_us=%.2f opaque_enter_wait_samples=%llu opaque_leave_wait_avg_us=%.2f opaque_leave_wait_max_us=%.2f opaque_leave_wait_samples=%llu\n",
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
           (unsigned long long)stats->yield_direct_attempts,
           (unsigned long long)stats->yield_direct_fast_hits,
           (unsigned long long)stats->yield_direct_locked_hits,
           (unsigned long long)stats->yield_direct_fail_context,
           (unsigned long long)stats->yield_direct_fail_policy,
           (unsigned long long)stats->yield_direct_fail_no_work,
           (unsigned long long)stats->yield_direct_fail_self,
           (unsigned long long)stats->yield_direct_fail_push,
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
            tasks[i] = llam_spawn(bench_spawn_child_task, &state->yields_per_task, NULL);
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
        peer = llam_spawn(bench_ping_peer_task, state, NULL);
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

static void bench_select_sender_task(void *arg) {
    bench_select_state_t *state = arg;
    unsigned i;

    for (i = 0; i < state->ops_per_round; ++i) {
        void *token = (void *)(uintptr_t)(i + 1U);

        llam_yield();
        if (llam_channel_send(state->primary, token) != 0) {
            bench_fail(&state->failures, "select sender send failed");
            return;
        }
    }
}

static int bench_select_recv_once(bench_select_state_t *state, uint64_t deadline_ns, unsigned i) {
    void *received = NULL;
    size_t selected = SIZE_MAX;
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

    if (llam_channel_select(ops, 2U, deadline_ns, &selected) != 0) {
        if (state->mode == BENCH_SELECT_TIMEOUT && errno == ETIMEDOUT) {
            return 0;
        }
        bench_fail(&state->failures, "select recv failed");
        return -1;
    }
    if (state->mode == BENCH_SELECT_TIMEOUT) {
        bench_fail(&state->failures, "select timeout unexpectedly selected");
        return -1;
    }
    if (selected != 0U || received != (void *)(uintptr_t)(i + 1U)) {
        bench_fail(&state->failures, "select recv mismatch");
        return -1;
    }
    return 0;
}

void bench_select_task(void *arg) {
    bench_select_state_t *state = arg;
    unsigned round;

    for (round = 0; round < state->rounds; ++round) {
        llam_channel_t *primary = llam_channel_create(1U);
        llam_channel_t *secondary = llam_channel_create(1U);
        llam_task_t *sender = NULL;
        uint64_t start_ns;
        unsigned i;

        if (primary == NULL || secondary == NULL) {
            bench_fail(&state->failures, "select channel create failed");
            if (primary != NULL) {
                (void)llam_channel_destroy(primary);
            }
            if (secondary != NULL) {
                (void)llam_channel_destroy(secondary);
            }
            return;
        }
        state->primary = primary;
        state->secondary = secondary;

        if (state->mode == BENCH_SELECT_PARK_WAKE) {
            sender = llam_spawn(bench_select_sender_task, state, NULL);
            if (sender == NULL) {
                bench_fail(&state->failures, "select sender spawn failed");
                (void)llam_channel_destroy(primary);
                (void)llam_channel_destroy(secondary);
                return;
            }
        }

        start_ns = llam_now_ns();
        for (i = 0; i < state->ops_per_round; ++i) {
            if (state->mode == BENCH_SELECT_READY &&
                llam_channel_send(primary, (void *)(uintptr_t)(i + 1U)) != 0) {
                bench_fail(&state->failures, "select ready seed send failed");
                goto done_round;
            }
            if (bench_select_recv_once(state,
                                       state->mode == BENCH_SELECT_TIMEOUT ? 0U : UINT64_MAX,
                                       i) != 0) {
                goto done_round;
            }
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;

done_round:
        if (atomic_load(&state->failures) != 0U) {
            (void)llam_channel_close(primary);
            (void)llam_channel_close(secondary);
        }
        if (sender != NULL && llam_join(sender) != 0) {
            bench_fail(&state->failures, "select sender join failed");
        }
        state->primary = NULL;
        state->secondary = NULL;
        (void)llam_channel_destroy(primary);
        (void)llam_channel_destroy(secondary);
        if (atomic_load(&state->failures) != 0U) {
            return;
        }
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
    llam_fd_t sv[2] = {LLAM_INVALID_FD, LLAM_INVALID_FD};
    bool reuse_pair = bench_reuse_socketpair_enabled();
    unsigned round;

    if (reuse_pair && bench_socketpair(sv) != 0) {
        bench_fail(&state->failures, "io socketpair failed");
        return;
    }

    for (round = 0; round < state->rounds; ++round) {
        bench_echo_state_t peer_state;
        llam_task_t *peer;
        uint64_t start_ns;
        unsigned i;

        if (!reuse_pair && bench_socketpair(sv) != 0) {
            bench_fail(&state->failures, "io socketpair failed");
            return;
        }

        peer_state.fd = sv[1];
        peer_state.messages_per_round = state->messages_per_round;
        peer = llam_spawn(bench_echo_peer_task, &peer_state, NULL);
        if (peer == NULL) {
            bench_fail(&state->failures, "io peer spawn failed");
            goto done;
        }

        start_ns = llam_now_ns();
        for (i = 0; i < state->messages_per_round; ++i) {
            char out = (char)(i & 0x7F);
            char in = 0;

            if (llam_write(sv[0], &out, 1U) != 1) {
                bench_fail(&state->failures, "io write failed");
                goto done;
            }
            if (llam_read(sv[0], &in, 1U) != 1 || in != out) {
                bench_fail(&state->failures, "io read mismatch");
                goto done;
            }
        }
        if (llam_join(peer) != 0) {
            bench_fail(&state->failures, "io peer join failed");
            goto done;
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
        if (!reuse_pair) {
            bench_close_fd(sv[0]);
            bench_close_fd(sv[1]);
            sv[0] = LLAM_INVALID_FD;
            sv[1] = LLAM_INVALID_FD;
        }
    }

done:
    bench_close_fd(sv[0]);
    bench_close_fd(sv[1]);
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
    bool fused_read = bench_env_flag_default("LLAM_BENCH_POLL_FUSED", 0U) != 0U;
    llam_fd_t sv[2] = {LLAM_INVALID_FD, LLAM_INVALID_FD};
    bool reuse_pair = bench_reuse_socketpair_enabled();
    unsigned round;

    if (reuse_pair && bench_socketpair(sv) != 0) {
        bench_fail(&state->failures, "poll socketpair failed");
        return;
    }

    for (round = 0; round < state->rounds; ++round) {
        bench_poll_writer_state_t writer_state;
        llam_task_t *writer;
        uint64_t start_ns;
        unsigned i;

        if (!reuse_pair && bench_socketpair(sv) != 0) {
            bench_fail(&state->failures, "poll socketpair failed");
            return;
        }

        writer_state.fd = sv[1];
        writer_state.events_per_round = state->events_per_round;
        writer = llam_spawn(bench_poll_writer_task, &writer_state, NULL);
        if (writer == NULL) {
            bench_fail(&state->failures, "poll writer spawn failed");
            goto done;
        }

        start_ns = llam_now_ns();
        for (i = 0; i < state->events_per_round; ++i) {
            short revents = 0;
            char byte = 0;
            int poll_rc;

            if (fused_read) {
                if (llam_read_when_ready(sv[0],
                                       &byte,
                                       1U,
                                       bench_platform_prefers_indefinite_ready_poll() ? -1 : 1000) != 1) {
                    bench_fail(&state->failures, "poll fused read failed");
                    goto done;
                }
                continue;
            }
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
                goto done;
            }
            if (llam_read(sv[0], &byte, 1U) != 1) {
                bench_fail(&state->failures, "poll read failed");
                goto done;
            }
        }
        if (llam_join(writer) != 0) {
            bench_fail(&state->failures, "poll writer join failed");
            goto done;
        }
        state->samples_ns[round] = llam_now_ns() - start_ns;
        if (!reuse_pair) {
            bench_close_fd(sv[0]);
            bench_close_fd(sv[1]);
            sv[0] = LLAM_INVALID_FD;
            sv[1] = LLAM_INVALID_FD;
        }
    }

done:
    bench_close_fd(sv[0]);
    bench_close_fd(sv[1]);
}

static void bench_opaque_companion_task(void *arg) {
    unsigned yields = *(unsigned *)arg;
    unsigned i;

    for (i = 0; i < yields; ++i) {
        llam_yield();
    }
}

static void bench_blocking_sleep_us(unsigned sleep_us) {
#if LLAM_PLATFORM_WINDOWS
    LARGE_INTEGER due_time;
    HANDLE timer;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

    if (sleep_us == 0U) {
        return;
    }
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == NULL) {
        timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    if (timer == NULL) {
        Sleep((DWORD)((sleep_us + 999U) / 1000U));
        return;
    }
    due_time.QuadPart = -((LONGLONG)sleep_us * 10LL);
    if (!SetWaitableTimer(timer, &due_time, 0, NULL, NULL, FALSE)) {
        CloseHandle(timer);
        Sleep((DWORD)((sleep_us + 999U) / 1000U));
        return;
    }
    (void)WaitForSingleObject(timer, INFINITE);
    CloseHandle(timer);
#else
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
#endif
}

void bench_opaque_task(void *arg) {
    bench_opaque_state_t *state = arg;
    unsigned round;

    for (round = 0; round < state->rounds; ++round) {
        uint64_t start_ns = llam_now_ns();
        unsigned i;

        for (i = 0; i < state->scopes_per_round; ++i) {
            llam_task_t *companion = llam_spawn(bench_opaque_companion_task, &state->companion_yields, NULL);

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
            tasks[i] = llam_spawn(bench_sleep_child_task, state, NULL);
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
