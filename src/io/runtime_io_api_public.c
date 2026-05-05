/**
 * @file src/io/runtime_io_api_public.c
 * @brief Public llam_* and nm_* I/O API entry points.
 *
 * @details
 * Public I/O calls prefer the cheapest completion path available for the
 * current context:
 *  - outside a managed runtime task, they call the platform syscall directly;
 *  - inside a task, they first try a non-blocking direct fast path;
 *  - if direct completion is not possible, they submit to the async backend;
 *  - when the backend cannot support the descriptor or operation, they fall
 *    back to ::nm_call_blocking so the scheduler worker is not pinned.
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
ssize_t nm_read(int fd, void *buf, size_t count) {
    nm_io_req_t *req;
    ssize_t result;

    nm_task_safepoint();

    if (g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        return read(fd, buf, count);
    }
    {
        ssize_t direct_result;
        int direct_rc = nm_try_direct_rw(fd, buf, count, false, false, 0, &direct_result, NULL);

        if (direct_rc > 0) {
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        if (nm_io_coop_yield_enabled() && nm_io_shard_has_local_work()) {
            nm_yield();
            direct_rc = nm_try_direct_rw(fd, buf, count, false, false, 0, &direct_result, NULL);
            if (direct_rc > 0) {
                return direct_result;
            }
            if (direct_rc < 0) {
                return -1;
            }
        }
        direct_rc = nm_try_direct_blocking_rw(fd, buf, count, false, false, 0, &direct_result);
        if (direct_rc > 0) {
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
    }

    req = nm_api_io_req_acquire(g_nm_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = NM_IO_KIND_READ;
    req->fd = fd;
    req->buf = buf;
    req->count = count;
    req->recv_watch = NULL;
    if (nm_issue_io(req, false, 0U) != 0) {
        if (!nm_io_capability_error(errno)) {
            nm_api_io_req_release(g_nm_tls_shard, req);
            return -1;
        }
        req->kind = NM_IO_KIND_READ;
        req->fd = fd;
        req->buf = buf;
        req->count = count;
        req->task = g_nm_tls_task;
        if (nm_call_blocking(nm_blocking_read_impl, req) == NULL) {
            int saved_errno = errno;

            nm_api_io_req_release(g_nm_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
        result = req->result;
        nm_api_io_req_release(g_nm_tls_shard, req);
        return result;
    }

    result = req->result;
    nm_api_io_req_release(g_nm_tls_shard, req);
    return result;
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
 * @see nm_io_buffer_release
 */
ssize_t nm_read_owned(int fd, size_t max_count, nm_io_buffer_t **out) {
    return nm_read_owned_impl(fd, max_count, 0, false, out);
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
 * @see nm_io_buffer_release
 */
ssize_t nm_recv_owned(int fd, size_t max_count, int flags, nm_io_buffer_t **out) {
    return nm_read_owned_impl(fd, max_count, flags, true, out);
}

/**
 * @brief Release an owned I/O buffer.
 *
 * Buffers backed by io_uring provided-buffer storage are recycled to their node
 * before the wrapper is returned to the runtime allocator.
 *
 * @param buffer Buffer returned by ::nm_read_owned or ::nm_recv_owned.
 */
void nm_io_buffer_release(nm_io_buffer_t *buffer) {
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

    if (buffer->provided_storage && g_nm_runtime.initialized &&
        buffer->provided_node_index < g_nm_runtime.active_nodes) {
        (void)nm_node_recycle_recv_buffer(&g_nm_runtime.nodes[buffer->provided_node_index], buffer->provided_bid);
        buffer->provided_storage = false;
        buffer->provided_bid = 0U;
    }

    nm_io_buffer_allocator_free(buffer);
}

/**
 * @brief Return the data pointer for an owned I/O buffer.
 *
 * @param buffer Buffer object.
 *
 * @return Data pointer, or @c NULL for @c NULL input.
 */
void *nm_io_buffer_data(nm_io_buffer_t *buffer) {
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
size_t nm_io_buffer_size(const nm_io_buffer_t *buffer) {
    return buffer != NULL ? buffer->size : 0U;
}

/**
 * @brief Return the total storage capacity of an owned I/O buffer.
 *
 * @param buffer Buffer object.
 *
 * @return Capacity in bytes, or 0 for @c NULL input.
 */
size_t nm_io_buffer_capacity(const nm_io_buffer_t *buffer) {
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
ssize_t nm_write(int fd, const void *buf, size_t count) {
    nm_io_req_t *req;
    ssize_t result;

    nm_task_safepoint();

    if (g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        return write(fd, buf, count);
    }
    {
        ssize_t direct_result;
        bool direct_socket = false;
        int direct_rc = nm_try_direct_rw(fd, (void *)buf, count, true, false, 0, &direct_result, &direct_socket);

        if (direct_rc > 0) {
            nm_maybe_handoff_after_socket_write(fd, count, direct_socket);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        direct_rc = nm_try_direct_blocking_rw(fd, (void *)buf, count, true, false, 0, &direct_result);
        if (direct_rc > 0) {
            nm_maybe_handoff_after_socket_write(fd, count, false);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
    }

    req = nm_api_io_req_acquire(g_nm_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = NM_IO_KIND_WRITE;
    req->fd = fd;
    req->buf = (void *)buf;
    req->count = count;
    if (nm_issue_io(req, false, 0U) != 0) {
        if (!nm_io_capability_error(errno)) {
            nm_api_io_req_release(g_nm_tls_shard, req);
            return -1;
        }
        req->kind = NM_IO_KIND_WRITE;
        req->fd = fd;
        req->buf = (void *)buf;
        req->count = count;
        req->task = g_nm_tls_task;
        if (nm_call_blocking(nm_blocking_write_impl, req) == NULL) {
            int saved_errno = errno;

            nm_api_io_req_release(g_nm_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
        result = req->result;
        nm_api_io_req_release(g_nm_tls_shard, req);
        return result;
    }

    result = req->result;
    nm_api_io_req_release(g_nm_tls_shard, req);
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
int nm_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    nm_io_req_t *req;
    int result;
    bool allow_multishot = addr == NULL && addrlen == NULL;

#if defined(__linux__)
    allow_multishot = false;
#endif

    nm_task_safepoint();

    if (g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        return accept(fd, addr, addrlen);
    }

    req = nm_api_io_req_acquire(g_nm_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = NM_IO_KIND_ACCEPT;
    req->fd = fd;
    req->addr = addr;
    req->addrlen = addrlen;
    req->recv_watch = NULL;
    if (allow_multishot) {
        if (nm_issue_multishot_accept(req) == 0) {
            result = (int)req->result;
            nm_api_io_req_release(g_nm_tls_shard, req);
            return result;
        }
        if (!nm_io_capability_error(errno)) {
            result = (int)req->result;
            nm_api_io_req_release(g_nm_tls_shard, req);
            return result;
        }
        errno = 0;
    }
    if (nm_issue_io(req, false, 0U) != 0) {
        if (!nm_io_capability_error(errno)) {
            nm_api_io_req_release(g_nm_tls_shard, req);
            return -1;
        }
        req->kind = NM_IO_KIND_ACCEPT;
        req->fd = fd;
        req->addr = addr;
        req->addrlen = addrlen;
        req->task = g_nm_tls_task;
        if (nm_call_blocking(nm_blocking_accept_impl, req) == NULL) {
            int saved_errno = errno;

            nm_api_io_req_release(g_nm_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
        result = (int)req->result;
        nm_api_io_req_release(g_nm_tls_shard, req);
        return result;
    }

    result = (int)req->result;
    nm_api_io_req_release(g_nm_tls_shard, req);
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
int nm_poll_fd(int fd, short events, int timeout_ms, short *revents) {
    nm_io_req_t *req;
    int result;
    bool yielded_for_local_work = false;

    nm_task_safepoint();

    if (g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        return nm_platform_poll_fd(fd, events, timeout_ms, revents);
    }
    if (timeout_ms > 0 && nm_io_coop_yield_enabled() && nm_io_shard_has_local_work()) {
        if (nm_io_poll_coop_yield_enabled()) {
            yielded_for_local_work = true;
            nm_yield();
        }
    }
    result = nm_try_socket_pollin_now(fd, events, revents);
    if (result == INT_MIN) {
        result = nm_platform_poll_now(fd, events, revents);
    }
    if (result != 0 || timeout_ms == 0) {
        return result;
    }
    if ((!yielded_for_local_work || nm_io_poll_extra_yield_enabled()) && nm_io_coop_yield_enabled() &&
        nm_io_poll_coop_yield_enabled() && nm_io_shard_has_local_work()) {
        nm_yield();
        result = nm_try_socket_pollin_now(fd, events, revents);
        if (result == INT_MIN) {
            result = nm_platform_poll_now(fd, events, revents);
        }
        if (result != 0 || timeout_ms == 0) {
            return result;
        }
    }
    result = nm_try_direct_blocking_poll(fd, events, timeout_ms, revents);
    if (result != INT_MIN) {
        return result;
    }

    req = nm_api_io_req_acquire(g_nm_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = NM_IO_KIND_POLL;
    req->fd = fd;
    req->poll_events = events;
    req->timeout_ms = timeout_ms;
    req->recv_watch = NULL;

    if (timeout_ms < 0 && nm_issue_multishot_poll(req) == 0) {
        result = (int)req->result;
        if (revents != NULL) {
            *revents = req->poll_revents;
        }
        nm_api_io_req_release(g_nm_tls_shard, req);
        return result;
    }
    if (timeout_ms < 0 && !nm_io_capability_error(errno)) {
        result = (int)req->result;
        if (revents != NULL) {
            *revents = req->poll_revents;
        }
        nm_api_io_req_release(g_nm_tls_shard, req);
        return result;
    }

    if (timeout_ms == 0) {
        nm_api_io_req_release(g_nm_tls_shard, req);
        return nm_platform_poll_fd(fd, events, 0, revents);
    }

    if (nm_issue_io(req, timeout_ms >= 0, timeout_ms >= 0 ? nm_now_ns() + (uint64_t)timeout_ms * 1000000ULL : 0U) != 0) {
        if (!nm_io_capability_error(errno)) {
            nm_api_io_req_release(g_nm_tls_shard, req);
            return -1;
        }
        req->kind = NM_IO_KIND_POLL;
        req->fd = fd;
        req->poll_events = events;
        req->timeout_ms = timeout_ms;
        req->task = g_nm_tls_task;
        if (nm_call_blocking(nm_blocking_poll_impl, req) == NULL) {
            int saved_errno = errno;

            nm_api_io_req_release(g_nm_tls_shard, req);
            errno = saved_errno;
            return -1;
        }
    }
    result = (int)req->result;
    if (revents != NULL) {
        *revents = req->poll_revents;
    }
    nm_api_io_req_release(g_nm_tls_shard, req);
    return result;
}
