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

#if LLAM_PLATFORM_POSIX
#include <sys/ioctl.h>
#endif

/** @brief Runtime that owns the currently executing managed task, if any. */
static llam_runtime_t *llam_direct_current_runtime(void) {
    if (g_llam_tls_task != NULL && g_llam_tls_task->owner_runtime != NULL) {
        return g_llam_tls_task->owner_runtime;
    }
    return g_llam_tls_shard != NULL ? g_llam_tls_shard->runtime : NULL;
}

/** @brief Return true when @c LLAM_DIRECT_BLOCKING_IO enables direct blocking read/write fallback. */
static bool llam_direct_blocking_io_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_DIRECT_BLOCKING_IO");

        // Cache env parsing once; this predicate is on I/O hot paths.
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/** @brief Return true when direct blocking poll should be attempted. */
static bool llam_direct_blocking_poll_enabled(int timeout_ms) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_DIRECT_BLOCKING_POLL");

        value = (int)llam_env_flag_value(env, 2U);
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    if (value == 2) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
        (void)timeout_ms;
        /*
         * WSAPoll on Windows AF_UNIX can stall a scheduler worker when used as
         * a direct blocking fallback.  Keep the automatic policy on the
         * backend/offload path; explicit LLAM_DIRECT_BLOCKING_POLL=1 remains
         * available for local experiments.
         */
        return false;
#elif LLAM_RUNTIME_BACKEND_LINUX
        llam_runtime_t *rt;
        llam_shard_t *shard = g_llam_tls_shard;
        llam_node_t *node;

        // Auto mode uses direct blocking poll for finite waits, and for
        // infinite waits only when the backend poll path is unavailable.
        if (timeout_ms > 0) {
            return true;
        }
        rt = llam_direct_current_runtime();
        if (shard == NULL || rt == NULL || shard->io_node_index >= rt->active_nodes) {
            return false;
        }
        node = &rt->nodes[shard->io_node_index];
        return timeout_ms < 0 && (!node->ring_ready || !node->supports_poll);
#elif LLAM_RUNTIME_BACKEND_KQUEUE
        const llam_runtime_t *rt = llam_direct_current_runtime();

        // Kqueue platforms keep infinite waits on the backend so one parked
        // task cannot pin a scheduler worker. Finite waits are safe to redirect
        // through compensated direct poll in the latency-oriented profiles.
        return rt != NULL && timeout_ms > 0 &&
               (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY ||
                rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST);
#else
        (void)timeout_ms;
        return false;
#endif
    }
    (void)timeout_ms;
    return value != 0;
}

/** @brief Return direct-poll opaque redirect threshold in milliseconds; 0 disables it. */
static unsigned llam_direct_poll_redirect_timeout_ms(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_POLL_REDIRECT_TIMEOUT_MS");

#if defined(__linux__)
        value = 1000;
#else
        value = 0;
#endif
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            long parsed;

            if (llam_ascii_is_space((unsigned char)env[0])) {
                atomic_store_explicit(&cached, value, memory_order_release);
                return (unsigned)value;
            }
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

/** @brief Return true when small blocking socket writes may yield after completion. */
static bool llam_write_handoff_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_WRITE_HANDOFF");

#if LLAM_RUNTIME_BACKEND_KQUEUE
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#elif LLAM_RUNTIME_BACKEND_LINUX
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#elif LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/** @brief Return true when direct I/O operations should cooperatively yield. */
bool llam_io_coop_yield_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_COOP_YIELD");

#if LLAM_RUNTIME_BACKEND_KQUEUE
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#elif LLAM_RUNTIME_BACKEND_LINUX
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#elif LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
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
bool llam_io_poll_coop_yield_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_POLL_COOP_YIELD");

#if LLAM_RUNTIME_BACKEND_KQUEUE
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#elif LLAM_RUNTIME_BACKEND_LINUX
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#elif LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
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
bool llam_io_poll_extra_yield_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_POLL_EXTRA_YIELD");

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether poll may hand off before its first readiness probe.
 *
 * This avoids a guaranteed not-ready syscall in producer/consumer patterns
 * where the producer is already runnable on the same shard.
 */
bool llam_io_poll_pre_yield_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_POLL_PRE_YIELD");

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Return short cooperative yield attempts before poll parks in the kernel.
 *
 * @return Number of ready-yield probes to attempt.
 */
unsigned llam_io_poll_ready_yields(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_POLL_READY_YIELDS");

#if LLAM_RUNTIME_BACKEND_WINDOWS
        value = 2;
#elif LLAM_RUNTIME_BACKEND_KQUEUE
        /*
         * Kqueue socketpair producer/consumer patterns often become ready
         * after one scheduler handoff but before the kqueue backend is worth
         * arming.  Two short probes keep poll_wake on the direct path without
         * changing long-wait semantics; LLAM_IO_POLL_READY_YIELDS remains the
         * escape hatch for latency-vs-throughput tuning.
         */
        value = 2;
#else
        value = 1;
#endif
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            long parsed;

            if (llam_ascii_is_space((unsigned char)env[0])) {
                atomic_store_explicit(&cached, value, memory_order_release);
                return (unsigned)value;
            }
            errno = 0;
            parsed = strtol(env, &end, 10);
            if (errno == 0 && end != env && *end == '\0') {
                if (parsed < 0) {
                    parsed = 0;
                } else if (parsed > 8L) {
                    parsed = 8L;
                }
                value = (int)parsed;
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Check whether socket poll-readiness should use MSG_PEEK.
 *
 * @return true if enabled.
 */
static bool llam_poll_socket_peek_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_POLL_SOCKET_PEEK");

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
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
static bool llam_fd_is_blocking_socket(llam_fd_t fd) {
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
static bool llam_fd_is_blocking(llam_fd_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    return flags >= 0 && (flags & O_NONBLOCK) == 0;
}

/**
 * @brief Check whether the current shard has local runnable work.
 *
 * @return true if local queues are non-empty.
 */
bool llam_io_shard_has_local_work(void) {
    bool has_local_work;
    llam_shard_t *shard = g_llam_tls_shard;

    if (shard == NULL) {
        return false;
    }
    if (llam_norm_queue_depth(shard) > 0U) {
        return true;
    }
    // Recheck under shard lock to include hot/inject queues and avoid missing a
    // just-enqueued task when deciding whether to hand off after a write.
    pthread_mutex_lock(&shard->lock);
    has_local_work = llam_norm_queue_depth(shard) > 0U || shard->hot_q.depth > 0U || shard->inject_q.depth > 0U;
    pthread_mutex_unlock(&shard->lock);
    return has_local_work;
}

/**
 * @brief Check whether write handoff requires queued local work.
 *
 * @return true if handoff should be skipped when the shard is otherwise idle.
 */
static bool llam_write_handoff_requires_work(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_WRITE_HANDOFF_REQUIRE_WORK");

        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether write handoff may switch directly to local work.
 *
 * @return true when direct task-to-task handoff is enabled.
 */
static bool llam_write_direct_local_handoff_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_WRITE_DIRECT_LOCAL_HANDOFF");

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_WINDOWS
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
#endif
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Return the recent-yield suppression window for write handoff.
 *
 * @return Nanosecond window; 0 disables suppression.
 */
static uint64_t llam_write_handoff_recent_yield_ns(void) {
    static atomic_ullong cached = UINT64_MAX;
    uint64_t value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value == UINT64_MAX) {
        const char *env = llam_env_get("LLAM_IO_WRITE_HANDOFF_RECENT_YIELD_NS");

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_WINDOWS
        value = 1000000ULL;
#else
        value = 0ULL;
#endif
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long long parsed;

            if (!llam_ascii_is_space((unsigned char)env[0]) && env[0] != '-' && env[0] != '+') {
                errno = 0;
                parsed = strtoull(env, &end, 10);
                if (errno == 0 && end != env && *end == '\0') {
                    value = (uint64_t)parsed;
                }
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
static bool llam_write_handoff_check_fd_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_IO_WRITE_HANDOFF_CHECK_FD");

#if LLAM_RUNTIME_BACKEND_KQUEUE || LLAM_RUNTIME_BACKEND_LINUX
        value = llam_env_flag_value(env, 0U) != 0U ? 1 : 0;
#else
        value = llam_env_flag_value(env, 1U) != 0U ? 1 : 0;
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
void llam_maybe_handoff_after_socket_write(llam_fd_t fd, size_t count, bool known_socket) {
    uint64_t recent_yield_ns;
    bool direct_local_handoff;

    if (!llam_write_handoff_enabled() || count > 256U || g_llam_tls_task == NULL || g_llam_tls_shard == NULL) {
        return;
    }
    recent_yield_ns = llam_write_handoff_recent_yield_ns();
    if (recent_yield_ns > 0U && g_llam_tls_task->last_yield_ns > 0U) {
        // llam_yield uses UINT64_MAX to mark the first post-yield write without a
        // clock read.
        if (g_llam_tls_task->last_yield_ns == UINT64_MAX) {
            g_llam_tls_task->last_yield_ns = 0U;
            return;
        } else {
            uint64_t now_ns = llam_now_ns();

            if (now_ns >= g_llam_tls_task->last_yield_ns && now_ns - g_llam_tls_task->last_yield_ns <= recent_yield_ns) {
                return;
            }
        }
    }
    direct_local_handoff = llam_write_direct_local_handoff_enabled();
    if (direct_local_handoff) {
        g_llam_tls_io_handoff_yield += 1U;
        if (llam_yield_to_local_runnable()) {
            g_llam_tls_io_handoff_yield -= 1U;
            return;
        }
        g_llam_tls_io_handoff_yield -= 1U;
    }
    if (llam_write_handoff_requires_work() && !llam_io_shard_has_local_work()) {
        return;
    }
    if (known_socket && !llam_write_handoff_check_fd_enabled()) {
        g_llam_tls_io_handoff_yield += 1U;
        llam_yield();
        g_llam_tls_io_handoff_yield -= 1U;
        return;
    }
    if (known_socket) {
        if (!llam_fd_is_blocking(fd)) {
            return;
        }
    } else if (!llam_fd_is_blocking_socket(fd)) {
        return;
    }
    g_llam_tls_io_handoff_yield += 1U;
    llam_yield();
    g_llam_tls_io_handoff_yield -= 1U;
}

/**
 * @brief Try a blocking read/write/recv directly inside opaque-blocking mode.
 *
 * @return 1 if handled, 0 if caller should use normal path, -1 on error.
 */
static int llam_try_direct_blocking_rw_impl(llam_fd_t fd,
                                            void *buf,
                                            size_t count,
                                            bool write_op,
                                            bool recv_op,
                                            int recv_flags,
                                            ssize_t *result_out,
                                            bool force_enabled) {
    struct pollfd pfd;
    ssize_t direct_result;
    llam_runtime_t *rt;
    int saved_errno = 0;

    if (result_out != NULL) {
        *result_out = -1;
    }
    if ((!force_enabled && !llam_direct_blocking_io_enabled()) ||
        g_llam_tls_task == NULL || g_llam_tls_shard == NULL ||
        g_llam_tls_task->cancel_token != NULL || !llam_fd_is_blocking_socket(fd)) {
        return 0;
    }
    if (llam_enter_blocking() != 0) {
        return -1;
    }
    rt = llam_direct_current_runtime();
    if (!g_llam_tls_task->opaque_uses_helper && !g_llam_tls_task->opaque_uses_redirect) {
        // Runtime policy refused helper/redirect compensation, so do not block
        // the scheduler thread directly.
        (void)llam_leave_blocking();
        return 0;
    }

    pfd.fd = fd;
    pfd.events = write_op ? POLLOUT : POLLIN;
    pfd.revents = 0;
    /*
     * This path exists only as an opt-in latency shortcut, but it still runs on a
     * scheduler thread.  Do not block forever inside read/write/recv/send: wait
     * for readiness in short slices, observe runtime stop, then perform the actual
     * transfer through the non-blocking direct helper.
     */
    for (;;) {
        int poll_rc;
        int direct_rc;

        if (rt != NULL && atomic_load_explicit(&rt->stop_requested, memory_order_acquire)) {
            saved_errno = ECANCELED;
            break;
        }
        pfd.revents = 0;
        do {
            poll_rc = poll(&pfd, 1, 10);
        } while (poll_rc < 0 && errno == EINTR);
        if (poll_rc < 0) {
            saved_errno = errno;
            break;
        }
        if (poll_rc == 0) {
            continue;
        }
        direct_rc = llam_try_direct_rw(fd, buf, count, write_op, recv_op, recv_flags, &direct_result, NULL);
        if (direct_rc > 0) {
            if (result_out != NULL) {
                *result_out = direct_result;
            }
            if (llam_leave_blocking() != 0) {
                return -1;
            }
            return 1;
        }
        if (direct_rc < 0) {
            saved_errno = errno;
            break;
        }
    }

    if (llam_leave_blocking() != 0) {
        return -1;
    }
    errno = saved_errno;
    return -1;
}

int llam_try_direct_blocking_rw(llam_fd_t fd,
                                     void *buf,
                                     size_t count,
                                     bool write_op,
                                     bool recv_op,
                                     int recv_flags,
                                     ssize_t *result_out) {
    return llam_try_direct_blocking_rw_impl(fd, buf, count, write_op, recv_op, recv_flags, result_out, false);
}

int llam_try_direct_blocking_rw_forced(llam_fd_t fd,
                                            void *buf,
                                            size_t count,
                                            bool write_op,
                                            bool recv_op,
                                            int recv_flags,
                                            ssize_t *result_out) {
    return llam_try_direct_blocking_rw_impl(fd, buf, count, write_op, recv_op, recv_flags, result_out, true);
}

/**
 * @brief Try to answer a POLLIN readiness check without a blocking poll.
 *
 * @return 1 ready, 0 not ready, or INT_MIN when unsupported/unhandled.
 */
int llam_try_socket_pollin_now(llam_fd_t fd, short events, short *revents) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    u_long available = 0UL;
    int saved_errno = errno;

    if (!llam_poll_socket_peek_enabled() || (events & POLLIN) == 0 || (events & POLLOUT) != 0) {
        return INT_MIN;
    }
    if (ioctlsocket(fd, FIONREAD, &available) != 0) {
        errno = saved_errno;
        return INT_MIN;
    }
    if (available > 0UL) {
        if (revents != NULL) {
            *revents = POLLIN;
        }
        errno = saved_errno;
        return 1;
    }
    if (revents != NULL) {
        *revents = 0;
    }
    errno = saved_errno;
    return 0;
#elif defined(MSG_PEEK) && defined(MSG_DONTWAIT)
    char byte;
    ssize_t rc;
    int saved_errno = errno;

    if (!llam_poll_socket_peek_enabled() || (events & POLLIN) == 0 || (events & POLLOUT) != 0) {
        return INT_MIN;
    }
#if defined(FIONREAD)
    {
        int available = 0;

        /*
         * Socketpair poll_wake is dominated by tiny readable probes.  FIONREAD
         * answers the hot "bytes are queued" case without copying even one byte
         * through MSG_PEEK; EOF and unsupported descriptors still fall through
         * to the exact peek/poll behavior below.
         */
        if (ioctl(fd, FIONREAD, &available) == 0 && available > 0) {
            if (revents != NULL) {
                *revents = POLLIN;
            }
            errno = saved_errno;
            return 1;
        }
        errno = saved_errno;
    }
#endif
    for (;;) {
        rc = llam_platform_recv_fd(fd, &byte, 1U, MSG_PEEK | MSG_DONTWAIT);
        if (rc > 0) {
            if (revents != NULL) {
                *revents = POLLIN;
            }
            errno = saved_errno;
            return 1;
        }
        if (rc == 0) {
            errno = saved_errno;
            return INT_MIN;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (revents != NULL) {
                *revents = 0;
            }
            errno = saved_errno;
            return 0;
        }
        errno = saved_errno;
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
int llam_try_direct_blocking_poll(llam_fd_t fd, short events, int timeout_ms, short *revents) {
    struct pollfd pfd;
    int rc;
    int saved_errno = 0;
    uint64_t deadline_ns = 0U;
    llam_runtime_t *rt;
    unsigned redirect_threshold_ms;
    bool redirect_hint = false;

    if (!llam_direct_blocking_poll_enabled(timeout_ms) || timeout_ms == 0 || g_llam_tls_task == NULL || g_llam_tls_shard == NULL ||
        g_llam_tls_task->cancel_token != NULL || !llam_fd_is_blocking(fd)) {
        return INT_MIN;
    }
    redirect_threshold_ms = llam_direct_poll_redirect_timeout_ms();
    if (timeout_ms > 0 && redirect_threshold_ms > 0U && (unsigned)timeout_ms < redirect_threshold_ms) {
        return INT_MIN;
    }
    if (timeout_ms > 0 && redirect_threshold_ms > 0U && (unsigned)timeout_ms >= redirect_threshold_ms) {
        // Long finite polls benefit from redirecting this shard's runnable work
        // while the task blocks in the kernel.
        g_llam_tls_opaque_redirect_hint += 1U;
        redirect_hint = true;
    }
    if (llam_enter_blocking() != 0) {
        if (redirect_hint && g_llam_tls_opaque_redirect_hint > 0U) {
            g_llam_tls_opaque_redirect_hint -= 1U;
        }
        return -1;
    }
    rt = llam_direct_current_runtime();
    if (redirect_hint && g_llam_tls_opaque_redirect_hint > 0U) {
        g_llam_tls_opaque_redirect_hint -= 1U;
    }
    if (!g_llam_tls_task->opaque_uses_helper && !g_llam_tls_task->opaque_uses_redirect) {
        (void)llam_leave_blocking();
        return INT_MIN;
    }

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    if (timeout_ms > 0) {
        deadline_ns = llam_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    }
    /*
     * Direct blocking poll runs on the scheduler thread under opaque-blocking
     * compensation.  Never pass an unbounded or long timeout directly to poll(2):
     * runtime stop cannot interrupt that syscall, so slice waits and observe the
     * stop flag between slices.
     */
    for (;;) {
        int slice_ms = 10;

        if (rt != NULL && atomic_load_explicit(&rt->stop_requested, memory_order_acquire)) {
            rc = -1;
            saved_errno = ECANCELED;
            break;
        }
        if (timeout_ms > 0) {
            uint64_t now_ns = llam_now_ns();
            uint64_t remain_ns;

            if (now_ns >= deadline_ns) {
                rc = 0;
                break;
            }
            remain_ns = deadline_ns - now_ns;
            slice_ms = (int)(remain_ns / 1000000ULL);
            if (slice_ms <= 0) {
                slice_ms = 1;
            } else if (slice_ms > 10) {
                slice_ms = 10;
            }
        } else if (timeout_ms < 0) {
            slice_ms = 10;
        }
        pfd.revents = 0;
        rc = poll(&pfd, 1, slice_ms);
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        if (rc != 0 || timeout_ms == 0) {
            break;
        }
    }
    if (rc < 0) {
        if (saved_errno == 0) {
            saved_errno = errno;
        }
    }
    if (llam_leave_blocking() != 0 && rc >= 0) {
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
