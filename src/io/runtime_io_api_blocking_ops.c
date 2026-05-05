/**
 * @file src/io/runtime_io_api_blocking_ops.c
 * @brief Blocking fallback implementations for I/O operations when backend async paths are unsuitable.
 *
 * @details
 * These callbacks run on the runtime blocking-worker pool via
 * ::nm_call_blocking. They are used only after direct completion and async
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
 * @brief Blocking-worker fallback for read/recv-style operations.
 *
 * The descriptor is temporarily forced non-blocking when needed. EAGAIN is
 * handled by short poll slices so cancellation can interrupt the fallback
 * without waiting forever in a kernel blocking syscall.
 *
 * @param arg Pointer to an initialized ::nm_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *nm_blocking_read_impl(void *arg) {
    nm_io_req_t *req = arg;
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
        req->result = req->use_recv_op ? recv(req->fd, req->buf, req->count, req->recv_flags) :
                                         read(req->fd, req->buf, req->count);
        if (req->result >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (req->task != NULL && req->task->cancel_token != NULL &&
            nm_cancel_token_is_cancelled(req->task->cancel_token) > 0) {
            errno = ECANCELED;
            req->result = -1;
            break;
        }
        if (nm_platform_poll_fd(req->fd, POLLIN, 10, NULL) < 0 && errno != EINTR) {
            req->result = -1;
            break;
        }
    }

    if (restore_flags) {
        (void)fcntl(req->fd, F_SETFL, saved_flags);
    }
    return req;
}

/**
 * @brief Blocking-worker fallback wrapper for recv-owned paths.
 *
 * @param arg Pointer to an initialized ::nm_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
static void *nm_blocking_recv_impl(void *arg) {
    return nm_blocking_read_impl(arg);
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
static nm_io_buffer_t *nm_io_buffer_alloc_detached(size_t min_capacity) {
    nm_io_buffer_t *buffer = calloc(1, sizeof(*buffer));

    if (buffer == NULL) {
        return NULL;
    }

    buffer->detached_wrapper = true;
    buffer->data = buffer->inline_data;
    buffer->capacity = NM_IO_BUFFER_INLINE_BYTES;
    if (min_capacity > NM_IO_BUFFER_INLINE_BYTES) {
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
static bool nm_fd_get_socket_type(int fd, int *so_type_out) {
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
ssize_t nm_read_owned_impl(int fd,
                                  size_t max_count,
                                  int recv_flags,
                                  bool force_recv,
                                  nm_io_buffer_t **out) {
    nm_io_buffer_t *buffer;
    nm_io_req_t *req;
    ssize_t result;
    int saved_errno = 0;
    int socket_type = 0;
    bool is_socket = nm_fd_get_socket_type(fd, &socket_type);
    bool socket_recv = force_recv || is_socket;
    bool prefer_provided = false;
    bool prefer_multishot = false;

    if (out == NULL || max_count == 0U) {
        errno = EINVAL;
        return -1;
    }

    *out = NULL;
    nm_task_safepoint();

    if (force_recv && !is_socket) {
        errno = ENOTSOCK;
        return -1;
    }

    if (g_nm_tls_shard == NULL || g_nm_tls_task == NULL) {
        buffer = nm_io_buffer_alloc_detached(max_count);
        if (buffer == NULL) {
            errno = ENOMEM;
            return -1;
        }
        result = socket_recv ? recv(fd, buffer->data, max_count, recv_flags) : read(fd, buffer->data, max_count);
        if (result < 0) {
            saved_errno = errno;
            nm_io_buffer_release(buffer);
            errno = saved_errno;
            return -1;
        }
        buffer->size = (size_t)result;
        *out = buffer;
        return result;
    }

    buffer = nm_io_buffer_alloc(g_nm_tls_shard, max_count);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (socket_recv && recv_flags == 0 && max_count <= NM_IO_BUFFER_INLINE_BYTES &&
        (socket_type == SOCK_DGRAM || socket_type == SOCK_SEQPACKET)) {
        nm_node_t *node = &g_nm_runtime.nodes[g_nm_tls_shard->io_node_index];

        prefer_provided = node->supports_provided_buffers || node->supports_multishot_recv;
        prefer_multishot = !node->supports_provided_buffers && node->supports_multishot_recv;
    }

    req = nm_api_io_req_acquire(g_nm_tls_shard);
    if (req == NULL) {
        saved_errno = errno != 0 ? errno : ENOMEM;
        nm_io_buffer_release(buffer);
        errno = saved_errno;
        return -1;
    }

    req->kind = NM_IO_KIND_READ;
    req->fd = fd;
    req->buf = prefer_provided ? NULL : buffer->data;
    req->count = max_count;
    req->recv_flags = recv_flags;
    req->owned_buffer = buffer;
    req->use_recv_op = socket_recv;
    req->use_provided_buffer = prefer_provided && g_nm_runtime.nodes[g_nm_tls_shard->io_node_index].supports_provided_buffers;
    req->buf = (req->use_provided_buffer || prefer_multishot) ? NULL : buffer->data;
    if (prefer_multishot && nm_issue_multishot_recv(req) == 0) {
        result = req->result;
        if (result < 0 && nm_io_capability_error(req->error_code)) {
            req->result = -1;
            req->error_code = 0;
            req->owned_buffer->provided_storage = false;
            req->owned_buffer->provided_bid = 0U;
            req->owned_buffer->data = req->owned_buffer->inline_data;
            req->owned_buffer->size = 0U;
            req->owned_buffer->capacity = NM_IO_BUFFER_INLINE_BYTES;
        } else {
            goto read_owned_done;
        }
    } else if (prefer_multishot && !nm_io_capability_error(errno)) {
        saved_errno = errno;
        nm_api_io_req_release(g_nm_tls_shard, req);
        nm_io_buffer_release(buffer);
        errno = saved_errno;
        return -1;
    }

    if (!req->use_provided_buffer) {
        req->buf = buffer->data;
    }
    if (nm_issue_io(req, false, 0U) != 0) {
        if (!nm_io_capability_error(errno)) {
            saved_errno = errno;
            nm_api_io_req_release(g_nm_tls_shard, req);
            nm_io_buffer_release(buffer);
            errno = saved_errno;
            return -1;
        }
        req->kind = NM_IO_KIND_READ;
        req->fd = fd;
        req->buf = buffer->data;
        req->count = max_count;
        req->recv_flags = recv_flags;
        req->use_recv_op = socket_recv;
        req->use_provided_buffer = false;
        req->task = g_nm_tls_task;
        if (nm_call_blocking(socket_recv ? nm_blocking_recv_impl : nm_blocking_read_impl, req) == NULL) {
            saved_errno = errno;
            nm_api_io_req_release(g_nm_tls_shard, req);
            nm_io_buffer_release(buffer);
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
        nm_api_io_req_release(g_nm_tls_shard, req);
        nm_io_buffer_release(buffer);
        errno = saved_errno;
        return -1;
    }

    buffer->size = (size_t)result;
    nm_api_io_req_release(g_nm_tls_shard, req);
    *out = buffer;
    return result;
}

/**
 * @brief Blocking-worker fallback for write operations.
 *
 * Like the read fallback, this keeps the descriptor non-blocking and polls for
 * output readiness in short slices so cancellation remains observable.
 *
 * @param arg Pointer to an initialized ::nm_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *nm_blocking_write_impl(void *arg) {
    nm_io_req_t *req = arg;
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
        req->result = write(req->fd, req->buf, req->count);
        if (req->result >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (req->task != NULL && req->task->cancel_token != NULL &&
            nm_cancel_token_is_cancelled(req->task->cancel_token) > 0) {
            errno = ECANCELED;
            req->result = -1;
            break;
        }
        if (nm_platform_poll_fd(req->fd, POLLOUT, 10, NULL) < 0 && errno != EINTR) {
            req->result = -1;
            break;
        }
    }

    if (restore_flags) {
        (void)fcntl(req->fd, F_SETFL, saved_flags);
    }
    return req;
}

/**
 * @brief Blocking-worker fallback for accept operations.
 *
 * @param arg Pointer to an initialized ::nm_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *nm_blocking_accept_impl(void *arg) {
    nm_io_req_t *req = arg;
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
        req->result = accept(req->fd, req->addr, req->addrlen);
        if (req->result >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            break;
        }
        if (req->task != NULL && req->task->cancel_token != NULL &&
            nm_cancel_token_is_cancelled(req->task->cancel_token) > 0) {
            errno = ECANCELED;
            req->result = -1;
            break;
        }
        if (nm_platform_poll_fd(req->fd, POLLIN, 10, NULL) < 0 && errno != EINTR) {
            req->result = -1;
            break;
        }
    }

    if (restore_flags) {
        (void)fcntl(req->fd, F_SETFL, saved_flags);
    }
    return req;
}

/**
 * @brief Blocking-worker fallback for poll operations.
 *
 * Polling is sliced into at most 10 ms intervals so finite timeouts remain
 * accurate enough while cancellation is still checked frequently.
 *
 * @param arg Pointer to an initialized ::nm_io_req_t.
 *
 * @return @p arg on completion, or @c NULL for invalid input.
 */
void *nm_blocking_poll_impl(void *arg) {
    nm_io_req_t *req = arg;
    uint64_t deadline_ns;

    if (req == NULL) {
        return NULL;
    }
    deadline_ns = req->timeout_ms >= 0 ? nm_now_ns() + (uint64_t)req->timeout_ms * 1000000ULL : 0U;

    for (;;) {
        int slice_ms = 10;

        if (req->task != NULL && req->task->cancel_token != NULL &&
            nm_cancel_token_is_cancelled(req->task->cancel_token) > 0) {
            errno = ECANCELED;
            req->result = -1;
            return req;
        }
        if (req->timeout_ms >= 0) {
            uint64_t now_ns = nm_now_ns();
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

        req->result = nm_platform_poll_fd(req->fd, req->poll_events, req->timeout_ms < 0 ? 10 : slice_ms, &req->poll_revents);
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
