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

#include <limits.h>

#if LLAM_PLATFORM_POSIX
#include <sys/uio.h>
#endif

#ifndef IOV_MAX
#define LLAM_WRITEV_IOV_MAX_FALLBACK 1024
#endif

/**
 * @brief Return the maximum public iovec count accepted by ::llam_writev.
 *
 * POSIX writev rejects counts above IOV_MAX with EINVAL. LLAM validates that
 * limit before touching user iovec memory so an invalid count cannot make the
 * wrapper scan beyond the caller-provided array.
 */
static int llam_writev_iovcnt_max(void) {
#ifdef IOV_MAX
    return IOV_MAX;
#else
    return LLAM_WRITEV_IOV_MAX_FALLBACK;
#endif
}

/**
 * @brief Return the largest aggregate byte count accepted before writev fallback.
 */
static size_t llam_writev_byte_count_max(void) {
#if LLAM_PLATFORM_DARWIN
    /*
     * Darwin rejects write/writev counts above INT_MAX with EINVAL. Mirror that
     * limit before the cooperative fallback writes any earlier slice.
     */
    return (size_t)INT_MAX;
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

/**
 * @brief Return the largest byte count supported by the async rw backend.
 *
 * Direct syscalls may accept platform-specific larger counts, but once LLAM
 * has to park the task and submit to a backend, the request count must fit the
 * backend ABI. Linux io_uring read/write SQEs store length in a 32-bit field;
 * validating before submission prevents silent truncation into a zero/short op.
 */
static size_t llam_async_rw_count_max(void) {
#if LLAM_RUNTIME_BACKEND_LINUX
    return (size_t)UINT_MAX;
#elif LLAM_PLATFORM_DARWIN
    return (size_t)INT_MAX;
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

/**
 * @brief Reject counts that cannot be represented by the parked I/O backend.
 */
static int llam_validate_async_rw_count(size_t count) {
    if (count > llam_async_rw_count_max()) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/**
 * @brief Validate scatter/gather slices before per-slice fallback writes.
 *
 * Native writev rejects an iovec whose total byte length cannot be represented
 * by ssize_t. The managed fallback writes each slice separately, so it must
 * perform aggregate validation before that fallback or an invalid tail slice
 * could be reported as a partial successful write after earlier slices were
 * already submitted.
 *
 * Native descriptors such as /dev/null may legally accept non-NULL byte counts
 * from a NULL base because the kernel never consumes user bytes. Keep that
 * platform-specific behavior on native writev paths; only the fallback rejects
 * NULL non-empty slices because it cannot preserve native all-or-nothing
 * validation while writing one slice at a time.
 */
static int llam_writev_validate_fallback_iov(const llam_iovec_t *iov, int iovcnt) {
    size_t total = 0U;
    size_t max_total = llam_writev_byte_count_max();

    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_base == NULL && iov[i].iov_len != 0U) {
            errno = EINVAL;
            return -1;
        }
        if (iov[i].iov_len > max_total - total) {
            errno = EINVAL;
            return -1;
        }
        total += iov[i].iov_len;
    }
    return 0;
}

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
 * @brief Return whether not-ready accept should use the blocking-helper path.
 *
 * Darwin one-shot accept readiness is correct for many workloads, but hosted
 * CI can expose a narrow registration/re-arm race in serial connect/accept
 * tests with peer-address output. The helper path is only used for accept
 * calls that cannot use the multishot accept-watch path. It keeps the listener
 * nonblocking and polls in short slices, so it does not pin a scheduler worker
 * and still observes task cancellation.
 */
static bool llam_accept_direct_blocking_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_ACCEPT_DIRECT_BLOCKING");

        if (env != NULL && env[0] != '\0') {
            value = strcmp(env, "0") != 0 ? 1 : 0;
        } else {
#if defined(__APPLE__)
            value = 1;
#else
            value = 0;
#endif
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Extract an accepted descriptor while preserving the public errno contract.
 *
 * Accept watch completions should always carry either a valid descriptor or a
 * positive errno.  Defensive normalization here prevents a backend wakeup bug
 * from surfacing as LLAM_INVALID_FD with errno left at zero.
 */
static llam_fd_t llam_accept_req_result(const llam_io_req_t *req) {
    llam_fd_t result;

    if (req == NULL) {
        errno = EINVAL;
        return LLAM_INVALID_FD;
    }
    result = LLAM_RUNTIME_BACKEND_WINDOWS ? req->fd_result : (llam_fd_t)req->result;
    if (LLAM_FD_IS_INVALID(result) && errno == 0) {
        errno = req->error_code != 0 ? req->error_code : EIO;
    }
    return result;
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
    if (llam_validate_async_rw_count(count) != 0) {
        return -1;
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
 * @brief Read bytes from a generic platform handle.
 */
ssize_t llam_read_handle(llam_handle_t handle, void *buf, size_t count) {
#if LLAM_PLATFORM_POSIX
    /*
     * Public POSIX handles are file descriptors. Route through llam_read so
     * managed and unmanaged callers get the same syscall-level edge semantics
     * and scheduler behavior as the fd API promised by the public header.
     */
    return llam_read((llam_fd_t)handle, buf, count);
#else
    llam_io_req_t *req;
    ssize_t result;

    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_read_handle(handle, buf, count);
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_HANDLE_READ;
    req->handle = handle;
    req->buf = buf;
    req->count = count;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = LLAM_IO_KIND_HANDLE_READ;
        req->handle = handle;
        req->buf = buf;
        req->count = count;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_handle_read_impl, req) != 0) {
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
#endif
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
            llam_maybe_handoff_after_socket_write(fd, (size_t)direct_result, direct_socket);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        llam_task_safepoint();
        direct_rc = llam_try_direct_blocking_rw(fd, (void *)buf, count, true, false, 0, &direct_result);
        if (direct_rc > 0) {
            llam_maybe_handoff_after_socket_write(fd, (size_t)direct_result, false);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
    }
    if (llam_validate_async_rw_count(count) != 0) {
        return -1;
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
 * @brief Scatter/gather write wrapper.
 */
ssize_t llam_writev(llam_fd_t fd, const llam_iovec_t *iov, int iovcnt) {
    ssize_t total = 0;

    if (iovcnt < 0) {
        errno = EINVAL;
        return -1;
    }
    if (iovcnt == 0) {
        return 0;
    }
    if (iovcnt > llam_writev_iovcnt_max()) {
        errno = EINVAL;
        return -1;
    }
    if (iov == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
#if LLAM_PLATFORM_POSIX
        struct iovec stack_iov[16];
        struct iovec *native_iov = stack_iov;
        ssize_t result;

        if (iovcnt > (int)(sizeof(stack_iov) / sizeof(stack_iov[0]))) {
            native_iov = malloc((size_t)iovcnt * sizeof(*native_iov));
            if (native_iov == NULL) {
                errno = ENOMEM;
                return -1;
            }
        }
        for (int i = 0; i < iovcnt; ++i) {
            native_iov[i].iov_base = (void *)iov[i].iov_base;
            native_iov[i].iov_len = iov[i].iov_len;
        }
        result = writev(fd, native_iov, iovcnt);
        if (native_iov != stack_iov) {
            free(native_iov);
        }
        return result;
#else
        /* Windows has WSASend, but this public fd API also covers non-sockets.
         * Keep the portable fallback exact and cooperative-compatible. */
#endif
    }

#if LLAM_PLATFORM_POSIX
    if (iovcnt <= 16) {
        ssize_t direct_result;
        bool direct_socket = false;
        int direct_rc = llam_try_direct_writev(fd, iov, iovcnt, &direct_result, &direct_socket);

        if (direct_rc > 0) {
            llam_maybe_handoff_after_socket_write(fd, (size_t)direct_result, direct_socket);
            return direct_result;
        }
        if (direct_rc < 0) {
            return -1;
        }
        llam_task_safepoint();
        if (llam_poll_fd(fd, POLLOUT, -1, NULL) > 0) {
            direct_rc = llam_try_direct_writev(fd, iov, iovcnt, &direct_result, &direct_socket);
            if (direct_rc > 0) {
                llam_maybe_handoff_after_socket_write(fd, (size_t)direct_result, direct_socket);
                return direct_result;
            }
            if (direct_rc < 0) {
                return -1;
            }
        } else if (errno != EINTR) {
            return -1;
        }
    }
#endif

    if (llam_writev_validate_fallback_iov(iov, iovcnt) != 0) {
        return -1;
    }

    for (int i = 0; i < iovcnt; ++i) {
        const char *bytes = (const char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;

        while (remaining > 0U) {
            ssize_t written = llam_write(fd, bytes, remaining);

            if (written > 0) {
                total += written;
                bytes += written;
                remaining -= (size_t)written;
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (total > 0) {
                return total;
            }
            return -1;
        }
    }
    return total;
}

/**
 * @brief Write bytes to a generic platform handle.
 */
ssize_t llam_write_handle(llam_handle_t handle, const void *buf, size_t count) {
#if LLAM_PLATFORM_POSIX
    /*
     * Public POSIX handles alias file descriptors. Keep the handle wrapper a
     * thin fd-API delegate so device-specific native behavior is not filtered
     * by the generic Windows HANDLE validation rules.
     */
    return llam_write((llam_fd_t)handle, buf, count);
#else
    llam_io_req_t *req;
    ssize_t result;

    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return llam_platform_write_handle(handle, buf, count);
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_HANDLE_WRITE;
    req->handle = handle;
    req->buf = (void *)buf;
    req->count = count;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = LLAM_IO_KIND_HANDLE_WRITE;
        req->handle = handle;
        req->buf = (void *)buf;
        req->count = count;
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_handle_write_impl, req) != 0) {
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
#endif
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

    if ((addr == NULL) != (addrlen == NULL)) {
        errno = EINVAL;
        return LLAM_INVALID_FD;
    }

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
    if (!allow_multishot && llam_accept_direct_blocking_enabled()) {
        req->task = g_llam_tls_task;
        if (llam_call_blocking_io(llam_blocking_accept_impl, req) != 0) {
            int saved_errno = errno;

            llam_api_io_req_release(g_llam_tls_shard, req);
            errno = saved_errno;
            return LLAM_INVALID_FD;
        }
        result = llam_accept_req_result(req);
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }
    if (allow_multishot) {
        if (llam_issue_multishot_accept(req) == 0) {
            result = llam_accept_req_result(req);
            llam_api_io_req_release(g_llam_tls_shard, req);
            return result;
        }
        if (!llam_io_capability_error(errno)) {
            result = llam_accept_req_result(req);
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
        result = llam_accept_req_result(req);
        llam_api_io_req_release(g_llam_tls_shard, req);
        return result;
    }

    result = llam_accept_req_result(req);
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

/**
 * @brief Poll a generic platform handle without pinning a scheduler worker.
 */
int llam_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents) {
#if LLAM_PLATFORM_POSIX
    /*
     * POSIX handles are file descriptors in the public ABI.  Delegate to the
     * fd poll path so infinite waits, runtime-stop cancellation, backend aborts,
     * and direct readiness probes keep the same semantics as llam_poll_fd.
     */
    return llam_poll_fd((llam_fd_t)handle, events, timeout_ms, revents);
#else
    llam_io_req_t *req;
    int result;

    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL || timeout_ms == 0) {
        return llam_platform_poll_handle(handle, events, timeout_ms, revents);
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }

    req->kind = LLAM_IO_KIND_POLL;
    req->handle = handle;
    req->poll_events = events;
    req->timeout_ms = timeout_ms;
    req->task = g_llam_tls_task;
    if (llam_call_blocking_io(llam_blocking_handle_poll_impl, req) != 0) {
        int saved_errno = errno;

        llam_api_io_req_release(g_llam_tls_shard, req);
        errno = saved_errno;
        return -1;
    }
    result = (int)req->result;
    if (revents != NULL) {
        *revents = req->poll_revents;
    }
    llam_api_io_req_release(g_llam_tls_shard, req);
    return result;
#endif
}
