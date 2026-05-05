/**
 * @file src/io/runtime_io_api_direct_tuning.c
 * @brief Runtime tuning knobs for direct I/O and readiness behavior.
 *
 * @details
 * The public I/O path tries cheap direct syscalls before parking a task on a
 * backend request. This file owns environment-controlled policy for direct
 * blocking fallbacks, cooperative yields after socket writes, poll peeking, and
 * redirecting long blocking polls through opaque-blocking compensation.
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

#include "runtime_io_api_internal.h"

/**
 * @brief Check whether direct blocking read/write fallback is enabled.
 *
 * @return true when @c LLAM_DIRECT_BLOCKING_IO is set to a non-zero value.
 */
static bool nm_direct_blocking_io_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_DIRECT_BLOCKING_IO");

        // Cache env parsing once; this predicate is on I/O hot paths.
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether direct blocking poll fallback is enabled.
 *
 * @param timeout_ms Requested poll timeout.
 * @return true when direct blocking poll should be attempted.
 */
static bool nm_direct_blocking_poll_enabled(int timeout_ms) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_DIRECT_BLOCKING_POLL");

        if (env != NULL && env[0] != '\0') {
            value = strcmp(env, "0") != 0 ? 1 : 0;
        } else {
#if defined(__linux__)
            value = 2;
#else
            value = 0;
#endif
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    if (value == 2) {
#if defined(__linux__)
        nm_runtime_t *rt = &g_nm_runtime;
        nm_shard_t *shard = g_nm_tls_shard;
        nm_node_t *node;

        // Auto mode on Linux uses direct blocking poll for long finite waits,
        // and for infinite waits only when the backend poll path is unavailable.
        if (timeout_ms > 0) {
            return true;
        }
        if (shard == NULL || rt == NULL || shard->io_node_index >= rt->active_nodes) {
            return false;
        }
        node = &rt->nodes[shard->io_node_index];
        return timeout_ms < 0 && (!node->ring_ready || !node->supports_poll);
#else
        (void)timeout_ms;
        return false;
#endif
    }
    (void)timeout_ms;
    return value != 0;
}

/**
 * @brief Return timeout threshold for opaque redirect around direct poll.
 *
 * @return Millisecond threshold; 0 disables redirect hinting.
 */
static unsigned nm_direct_poll_redirect_timeout_ms(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_POLL_REDIRECT_TIMEOUT_MS");

#if defined(__linux__)
        value = 1000;
#else
        value = 0;
#endif
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            long parsed;

            errno = 0;
            parsed = strtol(env, &end, 10);
            if (errno == 0 && end != env && *end == '\0') {
                if (parsed < 0) {
                    parsed = 0;
                } else if (parsed > 3600000L) {
                    parsed = 3600000L;
                }
                value = (int)parsed;
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Check whether socket-write handoff is enabled.
 *
 * @return true if small blocking socket writes may yield after completion.
 */
static bool nm_write_handoff_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_WRITE_HANDOFF");

#if defined(__APPLE__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#elif defined(__linux__)
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#else
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether direct I/O operations should cooperatively yield.
 *
 * @return true if enabled by platform default or environment override.
 */
bool nm_io_coop_yield_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_COOP_YIELD");

#if defined(__APPLE__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#elif defined(__linux__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#else
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether poll fallback paths should cooperatively yield.
 *
 * @return true if enabled by platform default or environment override.
 */
bool nm_io_poll_coop_yield_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_POLL_COOP_YIELD");

#if defined(__APPLE__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#elif defined(__linux__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#else
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether poll paths should perform an extra yield.
 *
 * @return true if enabled.
 */
bool nm_io_poll_extra_yield_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_POLL_EXTRA_YIELD");

#if defined(__APPLE__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#else
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether socket poll-readiness should use MSG_PEEK.
 *
 * @return true if enabled.
 */
static bool nm_poll_socket_peek_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_POLL_SOCKET_PEEK");

#if defined(__APPLE__)
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#else
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether an fd is a blocking socket.
 *
 * @param fd File descriptor.
 * @return true if it is a socket without O_NONBLOCK.
 */
static bool nm_fd_is_blocking_socket(int fd) {
    int flags;
    int so_type = 0;
    socklen_t so_type_len = sizeof(so_type);

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || (flags & O_NONBLOCK) != 0) {
        return false;
    }
    return getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &so_type_len) == 0;
}

/**
 * @brief Check whether an fd is in blocking mode.
 *
 * @param fd File descriptor.
 * @return true if O_NONBLOCK is not set.
 */
static bool nm_fd_is_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && (flags & O_NONBLOCK) == 0;
}

/**
 * @brief Check whether the current shard has local runnable work.
 *
 * @return true if local queues are non-empty.
 */
bool nm_io_shard_has_local_work(void) {
    bool has_local_work;
    nm_shard_t *shard = g_nm_tls_shard;

    if (shard == NULL) {
        return false;
    }
    if (nm_norm_queue_depth(shard) > 0U) {
        return true;
    }
    // Recheck under shard lock to include hot/inject queues and avoid missing a
    // just-enqueued task when deciding whether to hand off after a write.
    pthread_mutex_lock(&shard->lock);
    has_local_work = nm_norm_queue_depth(shard) > 0U || shard->hot_q.depth > 0U || shard->inject_q.depth > 0U;
    pthread_mutex_unlock(&shard->lock);
    return has_local_work;
}

/**
 * @brief Check whether write handoff requires queued local work.
 *
 * @return true if handoff should be skipped when the shard is otherwise idle.
 */
static bool nm_write_handoff_requires_work(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_WRITE_HANDOFF_REQUIRE_WORK");

        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Return the recent-yield suppression window for write handoff.
 *
 * @return Nanosecond window; 0 disables suppression.
 */
static uint64_t nm_write_handoff_recent_yield_ns(void) {
    static atomic_ullong cached = ATOMIC_VAR_INIT(UINT64_MAX);
    uint64_t value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value == UINT64_MAX) {
        const char *env = nm_env_get("LLAM_IO_WRITE_HANDOFF_RECENT_YIELD_NS");

#if defined(__APPLE__)
        value = 1000000ULL;
#else
        value = 0ULL;
#endif
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long long parsed;

            errno = 0;
            parsed = strtoull(env, &end, 10);
            if (errno == 0 && end != env && *end == '\0') {
                value = (uint64_t)parsed;
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value;
}

/**
 * @brief Check whether write handoff should inspect fd blocking/socket state.
 *
 * @return true if fd validation should run before yielding.
 */
static bool nm_write_handoff_check_fd_enabled(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = nm_env_get("LLAM_IO_WRITE_HANDOFF_CHECK_FD");

#if defined(__APPLE__) || defined(__linux__)
        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
#else
        value = (env == NULL || env[0] == '\0' || strcmp(env, "0") != 0) ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Optionally yield after a small blocking socket write.
 *
 * @param fd           Descriptor written.
 * @param count        Bytes attempted/written by the caller.
 * @param known_socket Whether caller already knows @p fd is a socket.
 */
void nm_maybe_handoff_after_socket_write(int fd, size_t count, bool known_socket) {
    uint64_t recent_yield_ns;

    if (!nm_write_handoff_enabled() || count > 256U || g_nm_tls_task == NULL || g_nm_tls_shard == NULL) {
        return;
    }
    recent_yield_ns = nm_write_handoff_recent_yield_ns();
    if (recent_yield_ns > 0U && g_nm_tls_task->last_yield_ns > 0U) {
        // nm_yield uses UINT64_MAX to mark the first post-yield write without a
        // clock read.
        if (g_nm_tls_task->last_yield_ns == UINT64_MAX) {
            g_nm_tls_task->last_yield_ns = 0U;
            return;
        } else {
            uint64_t now_ns = nm_now_ns();

            if (now_ns >= g_nm_tls_task->last_yield_ns && now_ns - g_nm_tls_task->last_yield_ns <= recent_yield_ns) {
                return;
            }
        }
    }
    if (nm_write_handoff_requires_work()) {
        if (!nm_io_shard_has_local_work()) {
            return;
        }
    }
    if (known_socket && !nm_write_handoff_check_fd_enabled()) {
        g_nm_tls_io_handoff_yield += 1U;
        nm_yield();
        g_nm_tls_io_handoff_yield -= 1U;
        return;
    }
    if (known_socket) {
        if (!nm_fd_is_blocking(fd)) {
            return;
        }
    } else if (!nm_fd_is_blocking_socket(fd)) {
        return;
    }
    g_nm_tls_io_handoff_yield += 1U;
    nm_yield();
    g_nm_tls_io_handoff_yield -= 1U;
}

/**
 * @brief Try a blocking read/write/recv directly inside opaque-blocking mode.
 *
 * @return 1 if handled, 0 if caller should use normal path, -1 on error.
 */
int nm_try_direct_blocking_rw(int fd,
                                     void *buf,
                                     size_t count,
                                     bool write_op,
                                     bool recv_op,
                                     int recv_flags,
                                     ssize_t *result_out) {
    ssize_t rc;
    int saved_errno = 0;

    if (result_out != NULL) {
        *result_out = -1;
    }
    if (!nm_direct_blocking_io_enabled() || g_nm_tls_task == NULL || g_nm_tls_shard == NULL ||
        g_nm_tls_task->cancel_token != NULL || !nm_fd_is_blocking_socket(fd)) {
        return 0;
    }
    if (nm_enter_blocking() != 0) {
        return -1;
    }
    if (!g_nm_tls_task->opaque_uses_helper && !g_nm_tls_task->opaque_uses_redirect) {
        // Runtime policy refused helper/redirect compensation, so do not block
        // the scheduler thread directly.
        (void)nm_leave_blocking();
        return 0;
    }

    for (;;) {
        if (write_op) {
            rc = write(fd, buf, count);
        } else if (recv_op) {
            rc = recv(fd, buf, count, recv_flags);
        } else {
            rc = read(fd, buf, count);
        }
        if (rc >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        saved_errno = errno;
        break;
    }

    if (nm_leave_blocking() != 0 && rc >= 0) {
        return -1;
    }
    if (rc >= 0) {
        if (result_out != NULL) {
            *result_out = rc;
        }
        return 1;
    }
    errno = saved_errno;
    return -1;
}

/**
 * @brief Try to answer a POLLIN readiness check with nonblocking MSG_PEEK.
 *
 * @return 1 ready, 0 not ready, or INT_MIN when unsupported/unhandled.
 */
int nm_try_socket_pollin_now(int fd, short events, short *revents) {
#if defined(MSG_PEEK) && defined(MSG_DONTWAIT)
    char byte;
    ssize_t rc;

    if (!nm_poll_socket_peek_enabled() || (events & POLLIN) == 0 || (events & POLLOUT) != 0) {
        return INT_MIN;
    }
    for (;;) {
        rc = recv(fd, &byte, 1U, MSG_PEEK | MSG_DONTWAIT);
        if (rc > 0) {
            if (revents != NULL) {
                *revents = POLLIN;
            }
            return 1;
        }
        if (rc == 0) {
            return INT_MIN;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (revents != NULL) {
                *revents = 0;
            }
            return 0;
        }
        return INT_MIN;
    }
#else
    (void)fd;
    (void)events;
    (void)revents;
    return INT_MIN;
#endif
}

/**
 * @brief Try a direct blocking poll under opaque-blocking compensation.
 *
 * @return poll result if handled, INT_MIN if caller should use normal path, or
 *         -1 on error.
 */
int nm_try_direct_blocking_poll(int fd, short events, int timeout_ms, short *revents) {
    struct pollfd pfd;
    int rc;
    int saved_errno = 0;
    unsigned redirect_threshold_ms;
    bool redirect_hint = false;

    if (!nm_direct_blocking_poll_enabled(timeout_ms) || timeout_ms == 0 || g_nm_tls_task == NULL || g_nm_tls_shard == NULL ||
        g_nm_tls_task->cancel_token != NULL || !nm_fd_is_blocking(fd)) {
        return INT_MIN;
    }
    redirect_threshold_ms = nm_direct_poll_redirect_timeout_ms();
    if (timeout_ms > 0 && redirect_threshold_ms > 0U && (unsigned)timeout_ms < redirect_threshold_ms) {
        return INT_MIN;
    }
    if (timeout_ms > 0 && redirect_threshold_ms > 0U && (unsigned)timeout_ms >= redirect_threshold_ms) {
        // Long finite polls benefit from redirecting this shard's runnable work
        // while the task blocks in the kernel.
        g_nm_tls_opaque_redirect_hint += 1U;
        redirect_hint = true;
    }
    if (nm_enter_blocking() != 0) {
        if (redirect_hint && g_nm_tls_opaque_redirect_hint > 0U) {
            g_nm_tls_opaque_redirect_hint -= 1U;
        }
        return -1;
    }
    if (redirect_hint && g_nm_tls_opaque_redirect_hint > 0U) {
        g_nm_tls_opaque_redirect_hint -= 1U;
    }
    if (!g_nm_tls_task->opaque_uses_helper && !g_nm_tls_task->opaque_uses_redirect) {
        (void)nm_leave_blocking();
        return INT_MIN;
    }

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0) {
        saved_errno = errno;
    }
    if (nm_leave_blocking() != 0 && rc >= 0) {
        return -1;
    }
    if (revents != NULL) {
        *revents = pfd.revents;
    }
    if (rc < 0) {
        errno = saved_errno;
    }
    return rc;
}
