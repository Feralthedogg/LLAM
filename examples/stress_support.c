/**
 * @file examples/stress_support.c
 * @brief Shared stress-test assertions, environment parsing, synthetic tasks, and platform helpers.
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

#if LLAM_PLATFORM_POSIX
#include <sys/resource.h>
#endif

atomic_uint g_failures;

/* Stress helpers accumulate failures instead of aborting so one run reports all violated invariants. */
void stress_fail_msg(const char *label) {
    fprintf(stderr, "[stress] fail: %s\n", label);
    atomic_fetch_add(&g_failures, 1U);
}

void stress_fail_u32(const char *label, unsigned got, unsigned expected) {
    fprintf(stderr, "[stress] fail: %s got=%u expected=%u\n", label, got, expected);
    atomic_fetch_add(&g_failures, 1U);
}

void stress_fail_errno(const char *label, int got, int expected) {
    fprintf(stderr, "[stress] fail: %s errno=%d expected=%d\n", label, got, expected);
    atomic_fetch_add(&g_failures, 1U);
}

void stress_close_fd_pair(int sv[2]) {
    if (sv == NULL) {
        return;
    }
    if (sv[0] >= 0) {
        close(sv[0]);
        sv[0] = -1;
    }
    if (sv[1] >= 0) {
        close(sv[1]);
        sv[1] = -1;
    }
}

#if LLAM_PLATFORM_WINDOWS
/*
 * Windows stress tests need a socketpair-like primitive for read/write/poll
 * races.  A loopback TCP pair is close enough for runtime I/O semantics and can
 * be driven through the same overlapped/IOCP path as normal sockets.
 */
int stress_socketpair_windows(int domain, int type, int protocol, int sv[2]) {
    SOCKET listener = INVALID_SOCKET;
    SOCKET client = INVALID_SOCKET;
    SOCKET server = INVALID_SOCKET;
    struct sockaddr_in addr;
    int addr_len = (int)sizeof(addr);
    BOOL no_delay = TRUE;
    u_long nonblocking = 1UL;

    (void)domain;
    (void)protocol;
    if (sv == NULL) {
        errno = EINVAL;
        return -1;
    }
    sv[0] = -1;
    sv[1] = -1;
    if (type != SOCK_STREAM) {
        errno = ENOTSUP;
        return -1;
    }

    listener = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (listener == INVALID_SOCKET) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (const struct sockaddr *)&addr, (int)sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 1) == SOCKET_ERROR ||
        getsockname(listener, (struct sockaddr *)&addr, &addr_len) == SOCKET_ERROR) {
        int saved_errno = llam_windows_wsa_error_to_errno(WSAGetLastError());

        closesocket(listener);
        errno = saved_errno;
        return -1;
    }

    client = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (client == INVALID_SOCKET) {
        int saved_errno = llam_windows_wsa_error_to_errno(WSAGetLastError());

        closesocket(listener);
        errno = saved_errno;
        return -1;
    }
    if (connect(client, (const struct sockaddr *)&addr, addr_len) == SOCKET_ERROR) {
        int saved_errno = llam_windows_wsa_error_to_errno(WSAGetLastError());

        closesocket(client);
        closesocket(listener);
        errno = saved_errno;
        return -1;
    }

    server = accept(listener, NULL, NULL);
    closesocket(listener);
    if (server == INVALID_SOCKET) {
        int saved_errno = llam_windows_wsa_error_to_errno(WSAGetLastError());

        closesocket(client);
        errno = saved_errno;
        return -1;
    }
    if ((uintptr_t)client > (uintptr_t)INT_MAX || (uintptr_t)server > (uintptr_t)INT_MAX) {
        closesocket(client);
        closesocket(server);
        errno = EMFILE;
        return -1;
    }
    if (ioctlsocket(client, FIONBIO, &nonblocking) != 0 ||
        ioctlsocket(server, FIONBIO, &nonblocking) != 0) {
        int saved_errno = llam_windows_wsa_error_to_errno(WSAGetLastError());

        closesocket(client);
        closesocket(server);
        errno = saved_errno;
        return -1;
    }

    (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, (int)sizeof(no_delay));
    (void)setsockopt(server, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, (int)sizeof(no_delay));
    sv[0] = (int)(uintptr_t)client;
    sv[1] = (int)(uintptr_t)server;
    return 0;
}
#endif

/* Reset only the logical fields; allocation ownership is managed by cleanup. */
void stress_reset_dynamic_foreign_poll_state(dynamic_foreign_poll_watch_state_t *state) {
    if (state == NULL) {
        return;
    }
    state->sv[0] = -1;
    state->sv[1] = -1;
    atomic_init(&state->completed, 0U);
    atomic_init(&state->spawned, 0U);
    atomic_init(&state->setup_done, 0U);
    atomic_init(&state->setup_failed, 0U);
    atomic_init(&state->observed_foreign_owner_waiters, 0U);
    atomic_init(&state->skipped, 0U);
    atomic_init(&state->source_shard, UINT_MAX);
    atomic_init(&state->source_node, UINT_MAX);
    atomic_init(&state->owner_node, UINT_MAX);
}

/* Release dynamic waiter arrays and any sockets left by a partially run phase. */
void stress_cleanup_dynamic_foreign_poll_state(dynamic_foreign_poll_watch_state_t *state) {
    if (state == NULL) {
        return;
    }
    stress_close_fd_pair(state->sv);
    free(state->waiter_states);
    free(state->waiters);
    state->waiter_states = NULL;
    state->waiters = NULL;
}

/*
 * Internal stress-only spawn primitive that pins a task to a specific shard.
 * Public users should use llam_spawn*; this bypass exists to reproduce
 * migration, foreign-owner poll, and wake handoff races deterministically.
 */
llam_task_t *stress_spawn_on_shard(unsigned shard_id, llam_task_fn fn, void *arg, const llam_spawn_opts_t *opts) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_task_t *task;
    llam_stack_class_t stack_class = LLAM_STACK_CLASS_DEFAULT;
    llam_task_class_t task_class = LLAM_TASK_CLASS_DEFAULT;
    llam_shard_t *target;

    llam_task_safepoint();

    if (!rt->initialized || fn == NULL || shard_id >= rt->active_shards) {
        errno = EINVAL;
        return NULL;
    }

    if (opts != NULL) {
        stack_class = opts->stack_class;
        task_class = opts->task_class;
    }

    target = &rt->shards[shard_id];
    task = llam_task_alloc(target);
    if (task == NULL) {
        return NULL;
    }

    if (pthread_mutex_init(&task->lock, NULL) != 0) {
        llam_task_allocator_free(task);
        errno = ENOMEM;
        return NULL;
    }

    task->id = atomic_fetch_add(&rt->next_task_id, 1U) + 1U;
    task->state = LLAM_TASK_STATE_NEW;
    task->wait_reason = LLAM_WAIT_NONE;
    task->flags = opts != NULL ? opts->flags : 0U;
    if ((task->flags & LLAM_TASK_FLAG_LATENCY_CRITICAL) != 0U) {
        task_class = LLAM_TASK_CLASS_LATENCY;
    }
    task->task_class = task_class;
    task->deadline_ns = opts != NULL ? opts->deadline_ns : 0U;
    task->cancel_token = opts != NULL ? opts->cancel_token : NULL;
    if (task->cancel_token != NULL) {
        pthread_mutex_lock(&task->cancel_token->lock);
        task->cancel_token->refcount += 1U;
        pthread_mutex_unlock(&task->cancel_token->lock);
    }
    task->entry = fn;
    task->arg = arg;
    task->forced_yield_budget = rt->forced_yield_every;
    atomic_init(&task->preempt_requested, 0U);

    if (llam_alloc_task_stack(task, stack_class) != 0) {
        int saved_errno = errno;

        pthread_mutex_destroy(&task->lock);
        llam_task_allocator_free(task);
        errno = saved_errno;
        return NULL;
    }

    task->home_shard = shard_id;
    task->live_shard = shard_id;
    task->last_shard = shard_id;
    task->last_runnable_ns = llam_now_ns();

    llam_add_task_to_list(rt, task);
    llam_runtime_note_task_live(rt, target);

    pthread_mutex_lock(&target->lock);
    if (rt->experimental_dynamic_shards != 0U &&
        atomic_load_explicit(&target->online, memory_order_relaxed) == 0U) {
        atomic_store_explicit(&target->online, 1U, memory_order_release);
        llam_runtime_note_online_shards(rt, atomic_fetch_add_explicit(&rt->online_shards, 1U, memory_order_acq_rel) + 1U);
    }
    task->state = LLAM_TASK_STATE_RUNNABLE;
    if (target->opaque_redirect_active) {
        target->metrics.migrations += 1U;
        task->enqueue_hot = 0U;
        if (!llam_enqueue_opaque_redirect_task_locked(target, task, false)) {
            llam_enqueue_overflow_task(rt, task);
        }
    } else if (llam_lockfree_normq_enabled(rt) && (g_llam_tls_shard != target || g_llam_tls_task == NULL)) {
        task->enqueue_hot = 0U;
        if (llam_queue_push_bounded_locked(target, &target->inject_q, LLAM_INJECT_QUEUE_CAP, task)) {
            target->metrics.inject_enqueues += 1U;
        }
    } else {
        (void)llam_norm_queue_push_owner_locked(target, task);
    }
    llam_trace_shard(target, task, LLAM_TRACE_STATE, LLAM_TASK_STATE_NEW, LLAM_TASK_STATE_RUNNABLE, LLAM_WAIT_NONE);
    pthread_mutex_unlock(&target->lock);
    llam_kick_shard(target);
    return task;
}

bool stress_open_foreign_poll_pair(unsigned source_node_index, int sv_out[2], unsigned *owner_node_out) {
    unsigned attempt;

    if (sv_out == NULL) {
        return false;
    }
    sv_out[0] = -1;
    sv_out[1] = -1;

    for (attempt = 0U; attempt < 256U; ++attempt) {
        int candidate[2];
        unsigned owner_node;

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, candidate) != 0) {
            return false;
        }
        owner_node = llam_multishot_owner_node_index(&g_llam_runtime, source_node_index, candidate[0]);
        if (owner_node != source_node_index) {
            sv_out[0] = candidate[0];
            sv_out[1] = candidate[1];
            if (owner_node_out != NULL) {
                *owner_node_out = owner_node;
            }
            return true;
        }
        close(candidate[0]);
        close(candidate[1]);
    }
    return false;
}

bool stress_poll_watch_waiter_counts(unsigned node_index,
                                            int fd,
                                            short events,
                                            unsigned source_shard,
                                            unsigned *total_waiters_out,
                                            unsigned *source_owned_waiters_out) {
    llam_runtime_t *rt = &g_llam_runtime;
    unsigned total_waiters = 0U;
    unsigned source_owned_waiters = 0U;
    bool found = false;
    unsigned i;

    if (total_waiters_out != NULL) {
        *total_waiters_out = 0U;
    }
    if (source_owned_waiters_out != NULL) {
        *source_owned_waiters_out = 0U;
    }
    if (rt->active_nodes == 0U || fd < 0) {
        return false;
    }

    for (i = 0U; i < rt->active_nodes; ++i) {
        unsigned current = (node_index < rt->active_nodes) ? ((node_index + i) % rt->active_nodes) : i;

        pthread_mutex_lock(&rt->nodes[current].watch_lock);
        for (llam_poll_watch_t *watch = rt->nodes[current].poll_watches; watch != NULL; watch = watch->next) {
            if (watch->fd != fd || watch->events != events) {
                continue;
            }
            for (llam_io_req_t *req = watch->wait_head; req != NULL; req = req->next) {
                total_waiters += 1U;
                if (req->owner_shard == source_shard) {
                    source_owned_waiters += 1U;
                }
            }
            found = true;
        }
        pthread_mutex_unlock(&rt->nodes[current].watch_lock);
    }

    if (total_waiters_out != NULL) {
        *total_waiters_out = total_waiters;
    }
    if (source_owned_waiters_out != NULL) {
        *source_owned_waiters_out = source_owned_waiters;
    }
    return found;
}

int stress_create_loopback_listener(uint16_t *out_port) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int fd;
    int one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one)) != 0) {
        close(fd);
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 128) != 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&addr, &addrlen) != 0) {
        close(fd);
        return -1;
    }
    if (out_port != NULL) {
        *out_port = ntohs(addr.sin_port);
    }
    return fd;
}

unsigned stress_round_count(void) {
    const char *value = llam_example_env_get("LLAM_STRESS_ROUNDS");
    char *end = NULL;
    unsigned long parsed = 0;

    if (value == NULL || *value == '\0') {
        return 4U;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0U) {
        return 4U;
    }
    if (parsed > 64U) {
        parsed = 64U;
    }
    return (unsigned)parsed;
}

unsigned stress_env_u32(const char *name, unsigned default_value, unsigned max_value) {
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

unsigned stress_env_flag_default(const char *name, unsigned default_value) {
    const char *value = llam_example_env_get(name);

    if (value == NULL || value[0] == '\0') {
        return default_value;
    }
    return strcmp(value, "0") != 0 ? 1U : 0U;
}

int stress_env_i32(const char *name, int default_value, int min_value, int max_value) {
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

unsigned stress_fd_budget_waiters(unsigned requested, unsigned fds_per_waiter, unsigned reserve_fds) {
#if LLAM_PLATFORM_POSIX
    struct rlimit limit;
    unsigned long long soft_limit;
    unsigned long long available;
    unsigned long long capped;

    if (requested == 0U || fds_per_waiter == 0U || getrlimit(RLIMIT_NOFILE, &limit) != 0 ||
        limit.rlim_cur == RLIM_INFINITY) {
        return requested;
    }

    soft_limit = (unsigned long long)limit.rlim_cur;
    if (soft_limit <= (unsigned long long)reserve_fds) {
        capped = 1ULL;
    } else {
        available = soft_limit - (unsigned long long)reserve_fds;
        capped = available / (unsigned long long)fds_per_waiter;
        if (capped == 0ULL) {
            capped = 1ULL;
        }
    }
    if (capped > (unsigned long long)requested) {
        return requested;
    }
    if (capped < (unsigned long long)requested) {
        fprintf(stderr,
                "[stress] fd budget clamped dynamic waiters requested=%u effective=%llu nofile=%llu reserve=%u fds_per_waiter=%u\n",
                requested,
                capped,
                soft_limit,
                reserve_fds,
                fds_per_waiter);
    }
    return (unsigned)capped;
#else
    (void)fds_per_waiter;
    (void)reserve_fds;
    return requested;
#endif
}

bool stress_platform_prefers_indefinite_ready_poll(void) {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

bool stress_platform_supports_foreign_poll_watch(void) {
    return true;
}

bool stress_platform_supports_recv_watch(void) {
#if defined(__APPLE__)
    return false;
#else
    return true;
#endif
}

bool stress_platform_supports_nested_opaque(void) {
    return true;
}

bool stress_platform_supports_owned_buffer_stress(void) {
#if LLAM_PLATFORM_WINDOWS
    return false;
#else
    return true;
#endif
}

bool stress_platform_supports_basic_poll_stress(void) {
    return true;
}

bool stress_platform_supports_poll_watch_stress(void) {
#if defined(__APPLE__)
    return true;
#else
    return true;
#endif
}

bool stress_platform_supports_io_cancel_stress(void) {
    return true;
}

bool stress_platform_supports_inflight_io_stress(void) {
#if defined(__APPLE__)
    return true;
#else
    return true;
#endif
}

bool stress_platform_supports_seqpacket_socketpair(void) {
    static int cached = -1;

    if (cached < 0) {
        int sv[2];

        cached = socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0 ? 1 : 0;
        if (cached > 0) {
            close(sv[0]);
            close(sv[1]);
        }
    }
    return cached > 0;
}

bool stress_runtime_supports_multishot_accept(void) {
    unsigned i;

    if (!g_llam_runtime.initialized || g_llam_runtime.nodes == NULL) {
        return false;
    }
    for (i = 0U; i < g_llam_runtime.active_nodes; ++i) {
        if (g_llam_runtime.nodes[i].ring_ready && g_llam_runtime.nodes[i].supports_multishot_accept) {
            return true;
        }
    }
    return false;
}

bool stress_runtime_supports_multishot_poll(void) {
    unsigned i;

    if (!g_llam_runtime.initialized || g_llam_runtime.nodes == NULL) {
        return false;
    }
    for (i = 0U; i < g_llam_runtime.active_nodes; ++i) {
        if (g_llam_runtime.nodes[i].ring_ready && g_llam_runtime.nodes[i].supports_multishot_poll) {
            return true;
        }
    }
    return false;
}

bool stress_runtime_supports_multishot_recv(void) {
    unsigned i;

    if (!g_llam_runtime.initialized || g_llam_runtime.nodes == NULL) {
        return false;
    }
    for (i = 0U; i < g_llam_runtime.active_nodes; ++i) {
        if (g_llam_runtime.nodes[i].ring_ready && g_llam_runtime.nodes[i].supports_multishot_recv) {
            return true;
        }
    }
    return false;
}

bool stress_runtime_supports_async_read(void) {
    unsigned i;

    if (!g_llam_runtime.initialized || g_llam_runtime.nodes == NULL) {
        return false;
    }
    for (i = 0U; i < g_llam_runtime.active_nodes; ++i) {
        if (g_llam_runtime.nodes[i].ring_ready &&
            (g_llam_runtime.nodes[i].supports_read || g_llam_runtime.nodes[i].supports_recv)) {
            return true;
        }
    }
    return false;
}

void stress_print_phase_skipped(const char *phase_name, const char *reason) {
    printf("[stress] phase=%s skipped=%s\n", phase_name, reason);
}

void stress_trace_step(const char *name) {
    const char *enabled = llam_example_env_get("LLAM_STRESS_TRACE_PHASES");

    if (enabled != NULL && enabled[0] != '\0' && strcmp(enabled, "0") != 0) {
        printf("[stress] step=%s\n", name);
        fflush(stdout);
    }
}

uint32_t stress_current_mxcsr(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t value;

    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
#else
    switch (fegetround()) {
    case FE_DOWNWARD:
        return 1U << 13;
    case FE_UPWARD:
        return 2U << 13;
    case FE_TOWARDZERO:
        return 3U << 13;
    case FE_TONEAREST:
    default:
        return 0U;
    }
#endif
}

uint16_t stress_current_x87_cw(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint16_t value;

    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
#else
    return (uint16_t)(stress_current_mxcsr() >> 3);
#endif
}

void stress_set_fp_round(unsigned mode) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t mxcsr = stress_current_mxcsr();
    uint16_t x87_cw = stress_current_x87_cw();

    mxcsr &= ~(3U << 13);
    mxcsr |= (mode & 3U) << 13;
    x87_cw &= (uint16_t)~(3U << 10);
    x87_cw |= (uint16_t)((mode & 3U) << 10);

    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(x87_cw));
#else
    int fe_round = FE_TONEAREST;

    switch (mode & 3U) {
    case 1U:
        fe_round = FE_DOWNWARD;
        break;
    case 2U:
        fe_round = FE_UPWARD;
        break;
    case 3U:
        fe_round = FE_TOWARDZERO;
        break;
    default:
        break;
    }
    (void)fesetround(fe_round);
#endif
}

unsigned stress_fp_round_mode(void) {
    unsigned mxcsr_mode = (stress_current_mxcsr() >> 13) & 3U;
    unsigned x87_mode = (unsigned)((stress_current_x87_cw() >> 10) & 3U);

    return mxcsr_mode == x87_mode ? mxcsr_mode : 0xFFU;
}
