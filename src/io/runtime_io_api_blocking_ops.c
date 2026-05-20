/**
 * @file src/io/runtime_io_api_blocking_ops.c
 * @brief Blocking fallback implementations for I/O operations when backend async paths are unsuitable.
 *
 * @details
 * These callbacks run on the runtime blocking-worker pool via
 * ::llam_call_blocking. They are used only after direct completion and async
 * backend submission cannot handle the descriptor or operation. The callbacks
 * keep descriptors non-blocking while they poll in short slices, allowing task
 * cancellation to be observed without pinning a scheduler worker.
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
 * @brief Run an owned-buffer blocking fallback through the unambiguous API.
 */
static int llam_call_blocking_io(llam_blocking_fn fn, llam_io_req_t *req) {
    void *ignored = NULL;

    return llam_call_blocking_result(fn, req, &ignored);
}

/**
 * @brief Check whether a blocking-worker I/O fallback should abort.
 *
 * Blocking fallback callbacks run after the submitting task has parked.  Task
 * cancellation is delivered through the task token, while runtime stop aborts
 * all parked waits even when no per-task token exists.  The callbacks poll in
 * short slices, so they must observe both signals themselves; otherwise a
 * running block job can keep the runtime alive forever after stop is requested.
 */
static bool llam_blocking_req_cancelled(const llam_io_req_t *req) {
    if (atomic_load_explicit(&g_llam_runtime.stop_requested, memory_order_acquire)) {
        return true;
    }
    return req != NULL &&
           req->task != NULL &&
           req->task->cancel_token != NULL &&
           llam_cancel_token_is_cancelled(req->task->cancel_token) > 0;
}

/**
 * @brief Mark a blocking fallback request as cooperatively cancelled.
 */
static void llam_blocking_req_set_cancelled(llam_io_req_t *req) {
    errno = ECANCELED;
    if (req != NULL) {
        req->result = -1;
        req->fd_result = LLAM_INVALID_FD;
    }
}

/**
 * @brief Restore temporary descriptor flags without clobbering operation errno.
 */
static void llam_restore_fd_flags_preserve_errno(llam_fd_t fd, bool restore_flags, int saved_flags) {
    int saved_errno;

    if (!restore_flags) {
        return;
    }
    saved_errno = errno;
    (void)fcntl(fd, F_SETFL, saved_flags);
    errno = saved_errno;
}

/**
 * @brief Blocking-worker fallback for read/recv-style operations.
 *
 * The descriptor is temporarily forced non-blocking when needed. EAGAIN is
 * handled by short poll slices so cancellation can interrupt the fallback
 * without waiting forever in a kernel blocking syscall.
 *
 * @param arg Pointer to an initialized ::llam_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *llam_blocking_read_impl(void *arg) {
    llam_io_req_t *req = arg;
    int saved_flags = 0;
    bool restore_flags = false;

    if (req == NULL) {
        return NULL;
    }

    saved_flags = fcntl(req->fd, F_GETFL, 0);
    if (saved_flags >= 0 && (saved_flags & O_NONBLOCK) == 0) {
        if (fcntl(req->fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            req->result = -1;
            return req;
        }
        restore_flags = true;
    }

    for (;;) {
        if (llam_blocking_req_cancelled(req)) {
            llam_blocking_req_set_cancelled(req);
            break;
        }
        req->result = req->use_recv_op ? llam_platform_recv_fd(req->fd, req->buf, req->count, req->recv_flags) :
                                         llam_platform_read_fd(req->fd, req->buf, req->count);
        if (req->result >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (llam_platform_poll_fd(req->fd, POLLIN, 10, NULL) < 0 && errno != EINTR) {
            req->result = -1;
            break;
        }
    }

    llam_restore_fd_flags_preserve_errno(req->fd, restore_flags, saved_flags);
    return req;
}

/**
 * @brief Blocking-worker fallback wrapper for recv-owned paths.
 *
 * @param arg Pointer to an initialized ::llam_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
static void *llam_blocking_recv_impl(void *arg) {
    return llam_blocking_read_impl(arg);
}

/**
 * @brief Allocate an owned buffer outside a managed runtime task.
 *
 * Detached buffers are used by public owned-read APIs when the caller is not
 * running inside the scheduler and therefore cannot use shard-local allocators
 * or provided-buffer recycling.
 *
 * @param min_capacity Minimum buffer capacity required by the caller.
 *
 * @return Allocated buffer, or @c NULL on allocation failure.
 */
static llam_io_buffer_t *llam_io_buffer_alloc_detached(size_t min_capacity) {
    llam_io_buffer_t *buffer = calloc(1, sizeof(*buffer));

    if (buffer == NULL) {
        return NULL;
    }

    buffer->detached_wrapper = true;
    buffer->data = buffer->inline_data;
    buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
    if (min_capacity > LLAM_IO_BUFFER_INLINE_BYTES) {
        buffer->data = calloc(1, min_capacity);
        if (buffer->data == NULL) {
            free(buffer);
            return NULL;
        }
        buffer->capacity = min_capacity;
        buffer->external_storage = true;
    }
    return buffer;
}

/**
 * @brief Query whether a descriptor is a socket and optionally return its type.
 *
 * @param fd          Descriptor to inspect.
 * @param so_type_out Optional output for @c SO_TYPE.
 *
 * @return @c true when @p fd is a socket.
 */
static bool llam_fd_get_socket_type(llam_fd_t fd, int *so_type_out) {
    int so_type = 0;
    socklen_t so_type_len = sizeof(so_type);

    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &so_type_len) != 0) {
        return false;
    }
    if (so_type_out != NULL) {
        *so_type_out = so_type;
    }
    return true;
}

/**
 * @brief Shared implementation for owned read and owned recv APIs.
 *
 * The function selects between plain buffers, provided buffers, multishot recv,
 * normal async submission, and blocking-worker fallback. On success, ownership
 * of the buffer is transferred to the caller through @p out.
 *
 * @param fd         File descriptor or socket.
 * @param max_count  Maximum bytes to read.
 * @param recv_flags Flags used when the operation is socket-backed recv.
 * @param force_recv Require socket recv semantics.
 * @param out        Receives the owned buffer on success.
 *
 * @return Number of bytes read, or -1 with @c errno set.
 */
ssize_t llam_read_owned_impl(llam_fd_t fd,
                                  size_t max_count,
                                  int recv_flags,
                                  bool force_recv,
                                  llam_io_buffer_t **out) {
    llam_io_buffer_t *buffer;
    llam_io_req_t *req;
    ssize_t result;
    int saved_errno = 0;
    int socket_type = 0;
    bool is_socket;
    bool socket_recv;
    bool prefer_multishot = false;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (max_count == 0U) {
        /*
         * Match the public owned-buffer contract and ordinary read(2)
         * zero-count semantics: no descriptor inspection, no allocation, and
         * no owned wrapper is produced.
         */
        return 0;
    }
    llam_task_safepoint();

    errno = 0;
    is_socket = llam_fd_get_socket_type(fd, &socket_type);
    socket_recv = force_recv || is_socket;
    if (force_recv && !is_socket) {
        /*
         * recv-owned requires socket semantics, but an invalid descriptor must
         * still surface as EBADF just like recv(2).  Only a valid non-socket
         * descriptor is normalized to ENOTSOCK.
         */
        errno = errno == EBADF ? EBADF : ENOTSOCK;
        return -1;
    }

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        buffer = llam_io_buffer_alloc_detached(max_count);
        if (buffer == NULL) {
            errno = ENOMEM;
            return -1;
        }
        result = socket_recv ? llam_platform_recv_fd(fd, buffer->data, max_count, recv_flags) :
                               llam_platform_read_fd(fd, buffer->data, max_count);
        if (result < 0) {
            saved_errno = errno;
            llam_io_buffer_release(buffer);
            errno = saved_errno;
            return -1;
        }
        if (result == 0) {
            llam_io_buffer_release(buffer);
            return 0;
        }
        buffer->size = (size_t)result;
        *out = buffer;
        return result;
    }

    /*
     * Public owned buffers are allowed to escape the task that produced them.
     * Keep their wrapper and payload detached from shard slabs so releasing an
     * otherwise valid buffer after runtime_shutdown() cannot read freed
     * allocator storage.
     */
    buffer = llam_io_buffer_alloc_detached(max_count);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (socket_recv && recv_flags == 0 && max_count <= LLAM_IO_BUFFER_INLINE_BYTES &&
        (socket_type == SOCK_DGRAM || socket_type == SOCK_SEQPACKET)) {
        llam_node_t *node = &g_llam_runtime.nodes[g_llam_tls_shard->io_node_index];

        prefer_multishot = !node->supports_provided_buffers && node->supports_multishot_recv;
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        saved_errno = errno != 0 ? errno : ENOMEM;
        llam_io_buffer_release(buffer);
        errno = saved_errno;
        return -1;
    }

    req->kind = LLAM_IO_KIND_READ;
    req->fd = fd;
    req->count = max_count;
    req->recv_flags = recv_flags;
    req->owned_buffer = buffer;
    req->use_recv_op = socket_recv;
    req->use_provided_buffer = false;
    req->buf = prefer_multishot ? NULL : buffer->data;
    if (prefer_multishot && llam_issue_multishot_recv(req) == 0) {
        result = req->result;
        if (result < 0 && (req->error_code == 0 || llam_io_capability_error(req->error_code))) {
            /*
             * A shared multishot watch can be invalidated by a transient
             * readiness race before it reports a concrete errno. Treat that
             * incomplete completion as a backend miss and retry on the regular
             * one-shot path instead of surfacing -1/errno=0 to callers.
             */
            req->result = -1;
            req->error_code = 0;
            req->owned_buffer->provided_storage = false;
            req->owned_buffer->provided_bid = 0U;
            req->owned_buffer->data = req->owned_buffer->inline_data;
            req->owned_buffer->size = 0U;
            req->owned_buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
        } else {
            goto read_owned_done;
        }
    } else if (prefer_multishot && !llam_io_capability_error(errno)) {
        saved_errno = errno;
        llam_api_io_req_release(g_llam_tls_shard, req);
        llam_io_buffer_release(buffer);
        errno = saved_errno;
        return -1;
    }

    if (!req->use_provided_buffer) {
        req->buf = buffer->data;
    }
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            saved_errno = errno;
            llam_api_io_req_release(g_llam_tls_shard, req);
            llam_io_buffer_release(buffer);
            errno = saved_errno;
            return -1;
        }
        req->kind = LLAM_IO_KIND_READ;
        req->fd = fd;
        req->buf = buffer->data;
        req->count = max_count;
        req->recv_flags = recv_flags;
        req->use_recv_op = socket_recv;
        req->use_provided_buffer = false;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(socket_recv ? llam_blocking_recv_impl : llam_blocking_read_impl, req) != 0) {
            saved_errno = errno;
            llam_api_io_req_release(g_llam_tls_shard, req);
            llam_io_buffer_release(buffer);
            errno = saved_errno;
            return -1;
        }
        result = req->result;
    } else {
        result = req->result;
    }

read_owned_done:
    if (result < 0) {
        saved_errno = req->error_code != 0 ? req->error_code : errno;
        llam_api_io_req_release(g_llam_tls_shard, req);
        llam_io_buffer_release(buffer);
        errno = saved_errno;
        return -1;
    }
    if (result == 0) {
        llam_api_io_req_release(g_llam_tls_shard, req);
        llam_io_buffer_release(buffer);
        return 0;
    }

    buffer->size = (size_t)result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    *out = buffer;
    return result;
}

/**
 * @brief Blocking-worker fallback for write operations.
 *
 * Like the read fallback, this keeps the descriptor non-blocking and polls for
 * output readiness in short slices so cancellation remains observable.
 *
 * @param arg Pointer to an initialized ::llam_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *llam_blocking_write_impl(void *arg) {
    llam_io_req_t *req = arg;
    int saved_flags = 0;
    bool restore_flags = false;

    if (req == NULL) {
        return NULL;
    }

    saved_flags = fcntl(req->fd, F_GETFL, 0);
    if (saved_flags >= 0 && (saved_flags & O_NONBLOCK) == 0) {
        if (fcntl(req->fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            req->result = -1;
            return req;
        }
        restore_flags = true;
    }

    for (;;) {
        if (llam_blocking_req_cancelled(req)) {
            llam_blocking_req_set_cancelled(req);
            break;
        }
        req->result = llam_platform_write_fd(req->fd, req->buf, req->count);
        if (req->result >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (llam_platform_poll_fd(req->fd, POLLOUT, 10, NULL) < 0 && errno != EINTR) {
            req->result = -1;
            break;
        }
    }

    llam_restore_fd_flags_preserve_errno(req->fd, restore_flags, saved_flags);
    return req;
}

/**
 * @brief Blocking-worker fallback for generic handle reads.
 */
void *llam_blocking_handle_read_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    do {
        req->result = llam_platform_read_handle(req->handle, req->buf, req->count);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for generic handle writes.
 */
void *llam_blocking_handle_write_impl(void *arg) {
    llam_io_req_t *req = arg;

    if (req == NULL) {
        return NULL;
    }
    do {
        req->result = llam_platform_write_handle(req->handle, req->buf, req->count);
    } while (req->result < 0 && errno == EINTR);
    return req;
}

/**
 * @brief Blocking-worker fallback for accept operations.
 *
 * @param arg Pointer to an initialized ::llam_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *llam_blocking_accept_impl(void *arg) {
    llam_io_req_t *req = arg;
    int saved_flags = 0;
    bool restore_flags = false;

    if (req == NULL) {
        return NULL;
    }

    saved_flags = fcntl(req->fd, F_GETFL, 0);
    if (saved_flags >= 0 && (saved_flags & O_NONBLOCK) == 0 &&
        fcntl(req->fd, F_SETFL, saved_flags | O_NONBLOCK) == 0) {
        restore_flags = true;
    }

    for (;;) {
        if (llam_blocking_req_cancelled(req)) {
            llam_blocking_req_set_cancelled(req);
            break;
        }
        {
            llam_fd_t accepted = llam_platform_accept_fd(req->fd, req->addr, req->addrlen);

            req->fd_result = accepted;
            req->result = LLAM_FD_IS_INVALID(accepted) ? -1 : (ssize_t)accepted;
        }
        if (req->result >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (llam_platform_poll_fd(req->fd, POLLIN, 10, NULL) < 0 && errno != EINTR) {
            req->result = -1;
            break;
        }
    }

    llam_restore_fd_flags_preserve_errno(req->fd, restore_flags, saved_flags);
    return req;
}

/**
 * @brief Blocking-worker fallback for connect operations.
 *
 * The helper initiates a nonblocking connect and waits for writable readiness in
 * short slices. After readiness, @c SO_ERROR is the authoritative connection
 * result.
 *
 * @param arg Pointer to an initialized ::llam_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *llam_blocking_connect_impl(void *arg) {
    llam_io_req_t *req = arg;
    int saved_flags = 0;
    bool restore_flags = false;
    bool wait_ready = false;

    if (req == NULL || req->addr == NULL) {
        return NULL;
    }

    saved_flags = fcntl(req->fd, F_GETFL, 0);
    if (saved_flags >= 0 && (saved_flags & O_NONBLOCK) == 0) {
        if (fcntl(req->fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            req->result = -1;
            return req;
        }
        restore_flags = true;
    }

    for (;;) {
        if (llam_blocking_req_cancelled(req)) {
            llam_blocking_req_set_cancelled(req);
            break;
        }
        if (!wait_ready) {
            if (llam_platform_connect_fd(req->fd, req->addr, req->addr_len) == 0) {
                req->result = 0;
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EISCONN) {
                req->result = 0;
                break;
            }
            if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
                req->result = -1;
                break;
            }
            wait_ready = true;
        }
        {
            int poll_rc = llam_platform_poll_fd(req->fd, POLLOUT, 10, NULL);

            if (poll_rc < 0 && errno != EINTR) {
                req->result = -1;
                break;
            }
            if (poll_rc < 0) {
                continue;
            }
            if (poll_rc == 0) {
                continue;
            }
        }
        if (llam_socket_connect_error(req->fd) == 0) {
            req->result = 0;
        } else {
            req->result = -1;
        }
        break;
    }

    llam_restore_fd_flags_preserve_errno(req->fd, restore_flags, saved_flags);
    return req;
}

/**
 * @brief Blocking-worker fallback for poll operations.
 *
 * Polling is sliced into at most 10 ms intervals so finite timeouts remain
 * accurate enough while cancellation is still checked frequently.
 *
 * @param arg Pointer to an initialized ::llam_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *llam_blocking_poll_impl(void *arg) {
    llam_io_req_t *req = arg;
    uint64_t deadline_ns;

    if (req == NULL) {
        return NULL;
    }
    deadline_ns = req->timeout_ms >= 0 ? llam_now_ns() + (uint64_t)req->timeout_ms * 1000000ULL : 0U;

    for (;;) {
        int slice_ms = 10;

        if (llam_blocking_req_cancelled(req)) {
            llam_blocking_req_set_cancelled(req);
            return req;
        }
        if (req->timeout_ms >= 0) {
            uint64_t now_ns = llam_now_ns();
            uint64_t remain_ns;

            if (now_ns >= deadline_ns) {
                req->poll_revents = 0;
                req->result = 0;
                return req;
            }
            remain_ns = deadline_ns - now_ns;
            slice_ms = (int)(remain_ns / 1000000ULL);
            if (slice_ms <= 0) {
                slice_ms = 1;
            } else if (slice_ms > 10) {
                slice_ms = 10;
            }
        }

        req->result = llam_platform_poll_fd(req->fd, req->poll_events, req->timeout_ms < 0 ? 10 : slice_ms, &req->poll_revents);
        if (req->result > 0) {
            return req;
        }
        if (req->result < 0 && errno != EINTR) {
            return req;
        }
        if (req->result < 0) {
            continue;
        }
    }
    return req;
}

/**
 * @brief Blocking-worker fallback for generic waitable-handle polling.
 */
void *llam_blocking_handle_poll_impl(void *arg) {
    llam_io_req_t *req = arg;
    uint64_t deadline_ns;

    if (req == NULL) {
        return NULL;
    }
    deadline_ns = req->timeout_ms >= 0 ? llam_now_ns() + (uint64_t)req->timeout_ms * 1000000ULL : 0U;

    for (;;) {
        int slice_ms = 10;

        if (llam_blocking_req_cancelled(req)) {
            llam_blocking_req_set_cancelled(req);
            return req;
        }
        if (req->timeout_ms >= 0) {
            uint64_t now_ns = llam_now_ns();
            uint64_t remain_ns;

            if (now_ns >= deadline_ns) {
                req->poll_revents = 0;
                req->result = 0;
                return req;
            }
            remain_ns = deadline_ns - now_ns;
            slice_ms = (int)(remain_ns / 1000000ULL);
            if (slice_ms <= 0) {
                slice_ms = 1;
            } else if (slice_ms > 10) {
                slice_ms = 10;
            }
        }

        req->result = llam_platform_poll_handle(req->handle,
                                                req->poll_events,
                                                req->timeout_ms < 0 ? 10 : slice_ms,
                                                &req->poll_revents);
        if (req->result >= 0) {
            if (req->result > 0 || req->timeout_ms >= 0) {
                return req;
            }
            continue;
        }
        if (errno != EINTR) {
            return req;
        }
    }
}
