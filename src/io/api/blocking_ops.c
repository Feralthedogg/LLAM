/**
 * @file src/io/api/blocking_ops.c
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

#include "io/runtime_io_api_internal.h"

#if LLAM_PLATFORM_POSIX
#include <sys/stat.h>
#endif

/**
 * @brief Run an owned-buffer blocking fallback through the unambiguous API.
 */
static int llam_call_blocking_io(llam_blocking_fn fn, llam_io_req_t *req) {
    void *ignored = NULL;

    return llam_call_blocking_result(fn, req, &ignored);
}

/**
 * @brief Return the largest owned-read allocation request LLAM will accept.
 *
 * Owned reads allocate the destination before issuing the operation.  Reject
 * counts that the platform cannot report through @c ssize_t before allocation
 * so a hostile or malformed max_count cannot force a giant allocation attempt
 * before the descriptor error path has a chance to run.
 */
static size_t llam_owned_read_count_max(void) {
#if LLAM_PLATFORM_DARWIN
    return (size_t)INT_MAX;
#elif LLAM_RUNTIME_BACKEND_LINUX
    return (size_t)UINT_MAX;
#elif LLAM_RUNTIME_BACKEND_WINDOWS
    return (size_t)ULONG_MAX;
#else
#ifdef SSIZE_MAX
    return (size_t)SSIZE_MAX;
#else
    return SIZE_MAX >> 1U;
#endif
#endif
}

static int llam_owned_read_validate_readable_fd_before_alloc(llam_fd_t fd) {
#if LLAM_PLATFORM_POSIX
    int saved_errno = errno;
    struct stat st;
    int flags;

    /*
     * Owned reads reserve caller-sized storage before issuing read(2).  A
     * descriptor can pass socket probing and still fail read(2) immediately
     * (for example write-only files or directories). Detect those native
     * descriptor errors before allocation so hostile sizes cannot hide them
     * behind ENOMEM.
     */
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if ((flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    errno = saved_errno;
#else
    (void)fd;
#endif
    return 0;
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
bool llam_blocking_req_cancelled(const llam_io_req_t *req) {
    llam_runtime_t *rt = req != NULL ? req->owner_runtime : NULL;
    llam_block_job_t *job = req != NULL && req->task != NULL ? llam_task_active_block_job_load(req->task) : NULL;

    if (rt != NULL && atomic_load_explicit(&rt->stop_requested, memory_order_acquire)) {
        return true;
    }
    if (job != NULL &&
        atomic_load_explicit(&job->state, memory_order_acquire) == LLAM_BLOCK_JOB_ABORTED) {
        /*
         * A cancel token can abort a running blocking job before the callback's
         * next token poll observes the token state.  Treat the job state as the
         * authoritative runtime-owned cancellation latch so blocking I/O
         * fallbacks cannot spin in short polls after the scheduler has already
         * detached the waiter.
         */
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
void llam_blocking_req_set_cancelled(llam_io_req_t *req) {
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

    if (LLAM_FD_IS_INVALID(fd)) {
        errno = EBADF;
        return false;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &so_type, &so_type_len) != 0) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
        /*
         * Winsock reports errors through WSAGetLastError(), not errno.  The
         * owned-buffer path relies on errno to decide whether it can safely
         * allocate before issuing the read, so normalize the probe failure at
         * the platform boundary.
         */
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
#endif
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
    llam_io_buffer_t *public_handle;
    llam_io_req_t *req;
    ssize_t result;
    int saved_errno = 0;
    int socket_probe_errno = 0;
    int socket_type = 0;
    bool is_socket;
    bool socket_recv;
    bool prefer_multishot = false;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (max_count > llam_owned_read_count_max()) {
        errno = EINVAL;
        return -1;
    }
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
    socket_probe_errno = errno;
    socket_recv = force_recv || is_socket;
    if (!is_socket && socket_probe_errno == EBADF) {
        /*
         * Owned reads allocate before issuing the operation. Preserve the
         * native invalid-descriptor error before allocation so hostile
         * max_count values cannot convert EBADF into ENOMEM.
         */
        errno = EBADF;
        return -1;
    }
    if (force_recv && !is_socket) {
        /*
         * recv-owned requires socket semantics, but an invalid descriptor must
         * still surface as EBADF just like recv(2).  Only a valid non-socket
         * descriptor is normalized to ENOTSOCK.
         */
        errno = socket_probe_errno == EBADF ? EBADF : ENOTSOCK;
        return -1;
    }
    if (!socket_recv && llam_owned_read_validate_readable_fd_before_alloc(fd) != 0) {
        return -1;
    }

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        buffer = llam_io_buffer_alloc_detached(max_count, 0U, 0U);
        if (buffer == NULL) {
            errno = ENOMEM;
            return -1;
        }
        result = socket_recv ? llam_platform_recv_fd(fd, buffer->data, max_count, recv_flags) :
                               llam_platform_read_fd(fd, buffer->data, max_count);
        if (result < 0) {
            saved_errno = errno;
            llam_io_buffer_release_raw(buffer);
            errno = saved_errno;
            return -1;
        }
        if (result == 0) {
            llam_io_buffer_release_raw(buffer);
            return 0;
        }
        buffer->size = (size_t)result;
        public_handle = llam_io_buffer_public_handle(buffer);
        if (public_handle == NULL) {
            /*
             * The buffer was already detached and populated, but publication is
             * still a separate registry step.  Do not report a successful read
             * with a NULL owned handle if registry state is inconsistent.
             */
            llam_io_buffer_release_raw(buffer);
            errno = ENOMEM;
            return -1;
        }
        *out = public_handle;
        return result;
    }

    /*
     * Public owned buffers are allowed to escape the task that produced them.
     * Keep their wrapper and payload detached from shard slabs so releasing an
     * otherwise valid buffer after runtime_shutdown() cannot read freed
     * allocator storage.
     */
    buffer = llam_io_buffer_alloc_detached(max_count, 0U, 0U);
    if (buffer == NULL) {
        errno = ENOMEM;
        return -1;
    }

    if (socket_recv && recv_flags == 0 && max_count <= LLAM_IO_BUFFER_INLINE_BYTES &&
        (socket_type == SOCK_DGRAM || socket_type == SOCK_SEQPACKET)) {
        llam_node_t *node = &g_llam_tls_shard->runtime->nodes[g_llam_tls_shard->io_node_index];

        prefer_multishot = !node->supports_provided_buffers && node->supports_multishot_recv;
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        saved_errno = errno != 0 ? errno : ENOMEM;
        llam_io_buffer_release_raw(buffer);
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
            req->owned_buffer->alignment = sizeof(void *);
            req->owned_buffer->aligned_storage = false;
        } else {
            goto read_owned_done;
        }
    } else if (prefer_multishot && !llam_io_capability_error(errno)) {
        saved_errno = errno;
        llam_api_io_req_release(g_llam_tls_shard, req);
        llam_io_buffer_release_raw(buffer);
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
            llam_io_buffer_release_raw(buffer);
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
            llam_io_buffer_release_raw(buffer);
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
        llam_io_buffer_release_raw(buffer);
        errno = saved_errno;
        return -1;
    }
    if (result == 0) {
        llam_api_io_req_release(g_llam_tls_shard, req);
        llam_io_buffer_release_raw(buffer);
        return 0;
    }

    buffer->size = (size_t)result;
    llam_api_io_req_release(g_llam_tls_shard, req);
    public_handle = llam_io_buffer_public_handle(buffer);
    if (public_handle == NULL) {
        llam_io_buffer_release_raw(buffer);
        errno = ENOMEM;
        return -1;
    }
    *out = public_handle;
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
 * @brief Prepare one cancellation-aware blocking poll slice.
 *
 * Blocking fallbacks poll in short slices so cancellation/runtime stop can be
 * observed without relying on backend-specific interrupt support.
 */
static bool llam_blocking_prepare_poll_slice(llam_io_req_t *req, uint64_t deadline_ns, int *slice_ms) {
    if (llam_blocking_req_cancelled(req)) {
        llam_blocking_req_set_cancelled(req);
        return false;
    }
    *slice_ms = 10;
    if (req->timeout_ms >= 0) {
        uint64_t now_ns = llam_now_ns();
        uint64_t remain_ns;

        if (now_ns >= deadline_ns) {
            req->poll_revents = 0;
            req->result = 0;
            return false;
        }
        remain_ns = deadline_ns - now_ns;
        *slice_ms = (int)(remain_ns / 1000000ULL);
        if (*slice_ms <= 0) {
            *slice_ms = 1;
        } else if (*slice_ms > 10) {
            *slice_ms = 10;
        }
    }
    return true;
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

        if (!llam_blocking_prepare_poll_slice(req, deadline_ns, &slice_ms)) {
            return req;
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

        if (!llam_blocking_prepare_poll_slice(req, deadline_ns, &slice_ms)) {
            return req;
        }

        req->result = llam_platform_poll_handle(req->handle,
                                                req->poll_events,
                                                req->timeout_ms < 0 ? 10 : slice_ms,
                                                &req->poll_revents);
        if (req->result > 0) {
            return req;
        }
        if (req->result == 0) {
            /*
             * A zero result means only the current cancellation slice expired.
             * Keep waiting until the absolute deadline above expires; otherwise
             * finite HANDLE polls can return after ~10 ms instead of the caller
             * supplied timeout.
             */
            continue;
        }
        if (errno != EINTR) {
            return req;
        }
    }
}
