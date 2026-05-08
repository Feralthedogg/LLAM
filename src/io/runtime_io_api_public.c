/**
 * @file src/io/runtime_io_api_public.c
 * @brief Public llam_* and llam_* I/O API entry points.
 *
 * @details
 * Public I/O calls prefer the cheapest completion path available for the
 * current context:
 *  - outside a managed runtime task, they call the platform syscall directly;
 *  - inside a task, they first try a non-blocking direct fast path;
 *  - if direct completion is not possible, they submit to the async backend;
 *  - when the backend cannot support the descriptor or operation, they fall
 *    back to ::llam_call_blocking so the scheduler worker is not pinned.
 *
 * Owned-buffer reads follow the same policy while also handling provided-buffer
 * and multishot receive support when the backend exposes it.
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
 * @brief Run an I/O blocking fallback and ignore the callback payload.
 *
 * The request object carries the real operation result. Using the result-style
 * blocking API keeps a legitimate callback NULL distinct from submission
 * failure, even though I/O callbacks normally return the request pointer.
 */
static int llam_call_blocking_io(llam_blocking_fn fn, llam_io_req_t *req) {
    void *ignored = NULL;

    return llam_call_blocking_result(fn, req, &ignored);
}

/**
 * @brief Convert a relative timeout to the remaining poll timeout.
 *
 * @param timeout_ms Original relative timeout; negative means infinite.
 * @param deadline_ns Absolute deadline computed from @p timeout_ms.
 * @return Remaining timeout in milliseconds, or -1 for infinite.
 */
static int llam_remaining_timeout_ms(int timeout_ms, uint64_t deadline_ns) {
    uint64_t now_ns;
    uint64_t remaining_ns;
    uint64_t remaining_ms;

    if (timeout_ms < 0) {
        return -1;
    }
    now_ns = llam_now_ns();
    if (now_ns >= deadline_ns) {
        return 0;
    }
    remaining_ns = deadline_ns - now_ns;
    remaining_ms = (remaining_ns + 999999ULL) / 1000000ULL;
    if (remaining_ms > (uint64_t)INT_MAX) {
        return INT_MAX;
    }
    return (int)remaining_ms;
}

/**
 * @brief Check whether fused read should yield once before its first probe.
 *
 * Producer/consumer loops often call read-when-ready while the producer is
 * already runnable locally but has not written yet.  A short same-shard handoff
 * avoids an immediate EAGAIN read and lets the subsequent direct read complete.
 */
static bool llam_read_ready_initial_handoff_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_READ_READY_INITIAL_HANDOFF");

        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Check whether read-when-ready may use compensated blocking read.
 *
 * The fused API has stronger semantics than a readiness probe: it is allowed to
 * wait until bytes can be read.  For infinite waits, compensated blocking read
 * can avoid a poll backend round trip while another worker/helper keeps the
 * shard making progress.
 */
static bool llam_read_ready_direct_blocking_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_READ_READY_DIRECT_BLOCKING");

        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Read bytes from a descriptor without blocking the scheduler worker.
 *
 * Managed tasks attempt direct non-blocking completion before submitting an
 * async read. Unsupported async paths fall back to the blocking worker pool.
 * Calls outside the runtime delegate to @c read directly.
 *
 * @param fd    File descriptor to read from.
 * @param buf   Destination buffer.
 * @param count Maximum bytes to read.
 *
 * @return Number of bytes read, or -1 with @c errno set.
 */
ssize_t llam_read(llam_fd_t fd, void *buf, size_t count) {
    llam_io_req_t *req;
    ssize_t result;

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_read_fd(fd, buf, count);
    }
    {
        ssize_t direct_result;
        int direct_rc = llam_try_direct_rw(fd, buf, count, false, false, 0, &direct_result, NULL);

        if (direct_rc > 0) {
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        llam_task_safepoint();
        if (llam_io_coop_yield_enabled() && llam_io_shard_has_local_work()) {
            llam_yield();
            direct_rc = llam_try_direct_rw(fd, buf, count, false, false, 0, &direct_result, NULL);
            if (direct_rc > 0) {
                return direct_result;
            }
            if (direct_rc < 0) {
                return -1;
            }
        }
        direct_rc = llam_try_direct_blocking_rw(fd, buf, count, false, false, 0, &direct_result);
        if (direct_rc > 0) {
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_READ;
    req->fd = fd;
    req->buf = buf;
    req->count = count;
    req->recv_watch = NULL;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = LLAM_IO_KIND_READ;
        req->fd = fd;
        req->buf = buf;
        req->count = count;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_read_impl, req) != 0) {
            int saved_errno = errno;

            llam_api_io_req_release(g_llam_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
        result = req->result;
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }

    result = req->result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

/**
 * @brief Wait for read readiness and read without a separate public poll/read pair.
 *
 * This fused path is intended for event loops that always read immediately
 * after @c POLLIN.  It preserves ordinary read semantics while avoiding one
 * extra public-API safepoint and one redundant readiness probe after poll wake.
 */
ssize_t llam_read_when_ready(llam_fd_t fd, void *buf, size_t count, int timeout_ms) {
    uint64_t deadline_ns = 0U;
    int caller_errno = errno;

    llam_task_safepoint();

    if (count == 0U) {
        return 0;
    }
    if (timeout_ms >= 0) {
        deadline_ns = llam_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    }

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        for (;;) {
            short revents = 0;
            int poll_timeout = llam_remaining_timeout_ms(timeout_ms, deadline_ns);
            int poll_rc;
            ssize_t rc;

            poll_rc = llam_platform_poll_fd(fd, POLLIN, poll_timeout, &revents);
            if (poll_rc < 0) {
                return -1;
            }
            if (poll_rc == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            do {
                rc = llam_platform_read_fd(fd, buf, count);
            } while (rc < 0 && errno == EINTR);
            if (rc >= 0) {
                errno = caller_errno;
                return rc;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                return -1;
            }
            if (timeout_ms >= 0 && llam_remaining_timeout_ms(timeout_ms, deadline_ns) == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
        }
    }

    if (timeout_ms != 0 &&
        llam_read_ready_initial_handoff_enabled() &&
        llam_io_coop_yield_enabled() &&
        llam_io_poll_coop_yield_enabled()) {
        if (!llam_yield_to_local_runnable() && llam_io_shard_has_local_work()) {
            llam_yield();
        }
    }

    for (;;) {
        ssize_t direct_result;
        int direct_rc = llam_try_direct_rw(fd, buf, count, false, false, 0, &direct_result, NULL);

        if (direct_rc > 0) {
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        if (timeout_ms < 0 && llam_read_ready_direct_blocking_enabled()) {
            direct_rc = llam_try_direct_blocking_rw_forced(fd, buf, count, false, false, 0, &direct_result);
            if (direct_rc > 0) {
                return direct_result;
            }
            if (direct_rc < 0) {
                return -1;
            }
        }
        {
            short revents = 0;
            int poll_timeout = llam_remaining_timeout_ms(timeout_ms, deadline_ns);
            int poll_rc = llam_poll_fd(fd, POLLIN, poll_timeout, &revents);

            if (poll_rc < 0) {
                return -1;
            }
            if (poll_rc == 0) {
                errno = ETIMEDOUT;
                return -1;
            }
            if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                errno = EIO;
                return -1;
            }
        }
        if (timeout_ms >= 0 && llam_remaining_timeout_ms(timeout_ms, deadline_ns) == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
    }
}

/**
 * @brief Read into a runtime-owned buffer.
 *
 * @param fd        File descriptor to read from.
 * @param max_count Maximum bytes to read.
 * @param out       Receives the allocated buffer on success.
 *
 * @return Number of bytes read, or -1 with @c errno set.
 *
 * @see llam_io_buffer_release
 */
ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out) {
    return llam_read_owned_impl(fd, max_count, 0, false, out);
}

/**
 * @brief Receive socket data into a runtime-owned buffer.
 *
 * @param fd        Socket descriptor.
 * @param max_count Maximum bytes to receive.
 * @param flags     Flags passed to @c recv when the fallback/direct path is used.
 * @param out       Receives the allocated buffer on success.
 *
 * @return Number of bytes received, or -1 with @c errno set.
 *
 * @see llam_io_buffer_release
 */
ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out) {
    return llam_read_owned_impl(fd, max_count, flags, true, out);
}

/**
 * @brief Release an owned I/O buffer.
 *
 * Buffers backed by io_uring provided-buffer storage are recycled to their node
 * before the wrapper is returned to the runtime allocator.
 *
 * @param buffer Buffer returned by ::llam_read_owned or ::llam_recv_owned.
 */
void llam_io_buffer_release(llam_io_buffer_t *buffer) {
    if (buffer == NULL) {
        return;
    }

    if (buffer->detached_wrapper) {
        if (buffer->external_storage && buffer->data != NULL) {
            free(buffer->data);
        }
        free(buffer);
        return;
    }

    if (buffer->provided_storage && g_llam_runtime.initialized &&
        buffer->provided_node_index < g_llam_runtime.active_nodes) {
        (void)llam_node_recycle_recv_buffer(&g_llam_runtime.nodes[buffer->provided_node_index], buffer->provided_bid);
        buffer->provided_storage = false;
        buffer->provided_bid = 0U;
    }

    llam_io_buffer_allocator_free(buffer);
}

/**
 * @brief Return the data pointer for an owned I/O buffer.
 *
 * @param buffer Buffer object.
 *
 * @return Data pointer, or @c NULL for @c NULL input.
 */
void *llam_io_buffer_data(llam_io_buffer_t *buffer) {
    if (buffer == NULL) {
        return NULL;
    }
    return buffer->data;
}

/**
 * @brief Return the number of valid bytes in an owned I/O buffer.
 *
 * @param buffer Buffer object.
 *
 * @return Valid byte count, or 0 for @c NULL input.
 */
size_t llam_io_buffer_size(const llam_io_buffer_t *buffer) {
    return buffer != NULL ? buffer->size : 0U;
}

/**
 * @brief Return the total storage capacity of an owned I/O buffer.
 *
 * @param buffer Buffer object.
 *
 * @return Capacity in bytes, or 0 for @c NULL input.
 */
size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer) {
    return buffer != NULL ? buffer->capacity : 0U;
}

/**
 * @brief Write bytes to a descriptor without blocking the scheduler worker.
 *
 * Managed tasks attempt direct non-blocking completion before async submission.
 * Unsupported backend paths fall back to the blocking worker pool. Socket writes
 * may trigger a cooperative handoff after successful direct completion.
 *
 * @param fd    File descriptor to write to.
 * @param buf   Source buffer.
 * @param count Maximum bytes to write.
 *
 * @return Number of bytes written, or -1 with @c errno set.
 */
ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count) {
    llam_io_req_t *req;
    ssize_t result;

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_write_fd(fd, buf, count);
    }
    {
        ssize_t direct_result;
        bool direct_socket = false;
        int direct_rc = llam_try_direct_rw(fd, (void *)buf, count, true, false, 0, &direct_result, &direct_socket);

        if (direct_rc > 0) {
            llam_maybe_handoff_after_socket_write(fd, count, direct_socket);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        llam_task_safepoint();
        direct_rc = llam_try_direct_blocking_rw(fd, (void *)buf, count, true, false, 0, &direct_result);
        if (direct_rc > 0) {
            llam_maybe_handoff_after_socket_write(fd, count, false);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_WRITE;
    req->fd = fd;
    req->buf = (void *)buf;
    req->count = count;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = LLAM_IO_KIND_WRITE;
        req->fd = fd;
        req->buf = (void *)buf;
        req->count = count;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_write_impl, req) != 0) {
            int saved_errno = errno;

            llam_api_io_req_release(g_llam_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
        result = req->result;
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }

    result = req->result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

/**
 * @brief Accept a connection without blocking the scheduler worker.
 *
 * Managed tasks submit an async accept where possible and otherwise use the
 * blocking worker fallback. Non-runtime callers delegate to @c accept directly.
 *
 * @param fd      Listening socket.
 * @param addr    Optional peer address output.
 * @param addrlen Optional peer address length in/out.
 *
 * @return Accepted descriptor, or -1 with @c errno set.
 */
llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen) {
    llam_io_req_t *req;
    llam_fd_t result;
    bool allow_multishot = addr == NULL && addrlen == NULL;

#if defined(__linux__)
    allow_multishot = false;
#endif

    llam_task_safepoint();

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_accept_fd(fd, addr, addrlen);
    }
    {
        llam_fd_t direct_fd = LLAM_INVALID_FD;
        int direct_rc = llam_try_direct_accept(fd, addr, addrlen, &direct_fd);

        if (direct_rc > 0) {
            return direct_fd;
        }
        if (direct_rc < 0) {
            return LLAM_INVALID_FD;
        }
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return LLAM_INVALID_FD;
    }

    req->kind = LLAM_IO_KIND_ACCEPT;
    req->fd = fd;
    req->addr = addr;
    req->addrlen = addrlen;
    req->recv_watch = NULL;
    if (allow_multishot) {
        if (llam_issue_multishot_accept(req) == 0) {
            result = (llam_fd_t)req->result;
            llam_api_io_req_release(g_llam_tls_shard, req);
            return result;
        }
        if (!llam_io_capability_error(errno)) {
            result = (llam_fd_t)req->result;
            llam_api_io_req_release(g_llam_tls_shard, req);
            return result;
        }
        errno = 0;
    }
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return LLAM_INVALID_FD;
        }
        req->kind = LLAM_IO_KIND_ACCEPT;
        req->fd = fd;
        req->addr = addr;
        req->addrlen = addrlen;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_accept_impl, req) != 0) {
            int saved_errno = errno;

            llam_api_io_req_release(g_llam_tls_shard, req);
            errno = saved_errno;
            return LLAM_INVALID_FD;
        }
        result = LLAM_RUNTIME_BACKEND_WINDOWS ? req->fd_result : (llam_fd_t)req->result;
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }

    result = LLAM_RUNTIME_BACKEND_WINDOWS ? req->fd_result : (llam_fd_t)req->result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

/**
 * @brief Connect a socket without blocking the scheduler worker.
 *
 * Managed tasks submit a one-shot connect request to the platform backend. If
 * the backend cannot initiate connect for this descriptor, the request falls
 * back to a blocking helper that drives a nonblocking connect with short poll
 * slices. Non-runtime callers delegate to @c connect directly.
 *
 * @param fd      Socket descriptor.
 * @param addr    Peer address. Must not be NULL.
 * @param addrlen Peer address length.
 *
 * @return 0 on connection, or -1 with @c errno set.
 */
int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen) {
    llam_io_req_t *req;
    int result;

    llam_task_safepoint();

    if (addr == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_connect_fd(fd, addr, addrlen);
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_CONNECT;
    req->fd = fd;
    req->addr = (struct sockaddr *)addr;
    req->addr_len = addrlen;
    req->recv_watch = NULL;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = LLAM_IO_KIND_CONNECT;
        req->fd = fd;
        req->addr = (struct sockaddr *)addr;
        req->addr_len = addrlen;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_connect_impl, req) != 0) {
            int saved_errno = errno;

            llam_api_io_req_release(g_llam_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
        result = (int)req->result;
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }

    result = (int)req->result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}

/**
 * @brief Poll a descriptor without unnecessarily parking the scheduler worker.
 *
 * The function first checks immediate readiness, optionally yields when local
 * work is waiting, then tries direct blocking poll heuristics before async
 * backend submission. Infinite polls may use multishot backend support.
 *
 * @param fd         File descriptor to poll.
 * @param events     Requested poll events.
 * @param timeout_ms Timeout in milliseconds; negative means infinite.
 * @param revents    Optional output for returned events.
 *
 * @return Positive ready count, 0 on timeout, or -1 with @c errno set.
 */
int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents) {
    llam_io_req_t *req;
    int result;
    unsigned ready_yields = 0U;
    unsigned ready_yield_limit;

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_poll_fd(fd, events, timeout_ms, revents);
    }
    if (timeout_ms != 0 &&
        llam_io_poll_pre_yield_enabled() &&
        llam_io_coop_yield_enabled() &&
        llam_io_poll_coop_yield_enabled() &&
        llam_io_shard_has_local_work()) {
        if (!llam_yield_to_local_runnable()) {
            llam_yield();
        }
    }
    result = llam_try_socket_pollin_now(fd, events, revents);
    if (result == INT_MIN) {
        result = llam_platform_poll_now(fd, events, revents);
    }
    if (result != 0 || timeout_ms == 0) {
        return result;
    }
    llam_task_safepoint();
    ready_yield_limit = llam_io_poll_ready_yields();
    if (!llam_io_poll_extra_yield_enabled() && ready_yield_limit > 1U) {
        ready_yield_limit = 1U;
    }
    while (ready_yields < ready_yield_limit && timeout_ms != 0 && llam_io_coop_yield_enabled() &&
           llam_io_poll_coop_yield_enabled()) {
        ready_yields += 1U;
        if (!llam_yield_to_local_runnable()) {
            if (!llam_io_shard_has_local_work()) {
                break;
            }
            llam_yield();
        }
        result = llam_try_socket_pollin_now(fd, events, revents);
        if (result == INT_MIN) {
            result = llam_platform_poll_now(fd, events, revents);
        }
        if (result != 0 || timeout_ms == 0) {
            return result;
        }
    }
    result = llam_try_direct_blocking_poll(fd, events, timeout_ms, revents);
    if (result != INT_MIN) {
        return result;
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_POLL;
    req->fd = fd;
    req->poll_events = events;
    req->timeout_ms = timeout_ms;
    req->recv_watch = NULL;

    if (timeout_ms < 0 && llam_issue_multishot_poll(req) == 0) {
        result = (int)req->result;
        if (revents != NULL) {
            *revents = req->poll_revents;
        }
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }
    if (timeout_ms < 0 && !llam_io_capability_error(errno)) {
        result = (int)req->result;
        if (revents != NULL) {
            *revents = req->poll_revents;
        }
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }

    if (timeout_ms == 0) {
        llam_api_io_req_release(g_llam_tls_shard, req);
        return llam_platform_poll_fd(fd, events, 0, revents);
    }

    if (llam_issue_io(req, timeout_ms >= 0, timeout_ms >= 0 ? llam_now_ns() + (uint64_t)timeout_ms * 1000000ULL : 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = LLAM_IO_KIND_POLL;
        req->fd = fd;
        req->poll_events = events;
        req->timeout_ms = timeout_ms;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_poll_impl, req) != 0) {
            int saved_errno = errno;

            llam_api_io_req_release(g_llam_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
    }
    result = (int)req->result;
    if (revents != NULL) {
        *revents = req->poll_revents;
    }
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
}
