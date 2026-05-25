/**
 * @file src/io/runtime_io_api_direct.c
 * @brief Direct I/O fast paths and fallback decision logic.
 *
 * @details
 * Direct paths are used before submitting to the async backend. They avoid
 * parking the current task when an operation can complete immediately, and they
 * return a tri-state result so the public API can distinguish completion,
 * retry-via-backend, and hard error.
 *
 * Request objects are also acquired here. The common case uses the current
 * task's embedded request to avoid allocator traffic; heap-backed requests are
 * used only when the embedded slot is already occupied.
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
#include <sys/uio.h>
#endif

/**
 * @brief Return poll-compatible readiness for the public invalid-fd sentinel.
 *
 * Native poll(2) treats negative descriptors as ignored entries, which is
 * useful for poll arrays but wrong for LLAM's single-descriptor public API.
 * Normalize the sentinel to POLLNVAL before any direct poll fast path can turn
 * it into a false timeout.
 */
static int llam_invalid_fd_poll_result(short *revents) {
    if (revents != NULL) {
        *revents = POLLNVAL;
    }
    errno = 0;
    return 1;
}

#if LLAM_RUNTIME_BACKEND_WINDOWS
/**
 * @brief Return whether the Windows nonblocking socket cache is enabled.
 *
 * LLAM cannot observe every user-side closesocket call, so handle-value reuse
 * can make a cache entry stale.  Keep the optimization opt-in; the safe default
 * repeats FIONBIO and therefore never trusts a recycled SOCKET value.
 */
static bool llam_windows_nonblock_cache_enabled(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_WINDOWS_NONBLOCK_CACHE");

        value = (env != NULL && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return value != 0;
}

/**
 * @brief Return the runtime that may own the direct-I/O socket cache.
 *
 * @details
 * Direct I/O is entered from managed LLAM tasks.  Use pointer identity rather
 * than shard ids here: shard ids are runtime-local and runtime A shard 0 must
 * never share cached socket state with runtime B shard 0.
 */
static llam_runtime_t *llam_windows_nonblock_cache_runtime(void) {
    if (g_llam_tls_task != NULL && g_llam_tls_task->owner_runtime != NULL) {
        return g_llam_tls_task->owner_runtime;
    }
    if (g_llam_tls_shard != NULL) {
        return g_llam_tls_shard->runtime;
    }
    return NULL;
}

/** @brief Return true when @p fd is remembered as already nonblocking. */
bool llam_windows_socket_nonblocking_cached(llam_fd_t fd) {
    llam_runtime_t *rt = llam_windows_nonblock_cache_runtime();
    uintptr_t key = (uintptr_t)fd + 1U;
    uintptr_t slot = ((uintptr_t)fd ^ ((uintptr_t)fd >> 7U)) & (LLAM_WINDOWS_NONBLOCK_CACHE_CAP - 1U);

    return rt != NULL && llam_windows_nonblock_cache_enabled() &&
           atomic_load_explicit(&rt->windows_nonblock_cache[slot], memory_order_acquire) == key;
}

void llam_windows_socket_nonblocking_forget(llam_runtime_t *rt, llam_fd_t fd) {
    uintptr_t key;
    uintptr_t slot;

    if (rt == NULL || !llam_windows_nonblock_cache_enabled()) {
        return;
    }
    key = (uintptr_t)fd + 1U;
    slot = ((uintptr_t)fd ^ ((uintptr_t)fd >> 7U)) & (LLAM_WINDOWS_NONBLOCK_CACHE_CAP - 1U);
    (void)atomic_compare_exchange_strong_explicit(&rt->windows_nonblock_cache[slot],
                                                  &key,
                                                  0U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire);
}

/**
 * @brief Ensure a Windows socket is nonblocking for direct managed I/O.
 *
 * @return 1 when nonblocking mode is active, 0 when @p fd is not a socket, or
 *         -1 for a hard socket error with errno set.
 */
static int llam_windows_ensure_socket_nonblocking(llam_fd_t fd) {
    llam_runtime_t *rt = llam_windows_nonblock_cache_runtime();
    uintptr_t key = (uintptr_t)fd + 1U;
    uintptr_t slot = ((uintptr_t)fd ^ ((uintptr_t)fd >> 7U)) & (LLAM_WINDOWS_NONBLOCK_CACHE_CAP - 1U);
    bool cache_enabled = (rt != NULL) && llam_windows_nonblock_cache_enabled();
    u_long nonblocking = 1UL;

    if (cache_enabled && llam_windows_socket_nonblocking_cached(fd)) {
        return 1;
    }
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
        int mapped = llam_windows_wsa_error_to_errno(WSAGetLastError());

        if (mapped == ENOTSOCK) {
            return 0;
        }
        errno = mapped;
        return -1;
    }
    if (cache_enabled) {
        atomic_store_explicit(&rt->windows_nonblock_cache[slot], key, memory_order_release);
    }
    return 1;
}
#endif

/**
 * @brief Acquire an I/O request object for the current operation.
 *
 * The current task's embedded request is preferred when available. Heap-backed
 * requests are allocated from the shard-local request allocator otherwise.
 *
 * @param shard Shard requesting the object; may be @c NULL for defensive calls.
 *
 * @return Request object on success, or @c NULL with @c errno set by the
 *         allocator.
 */
llam_io_req_t *llam_api_io_req_acquire(llam_shard_t *shard) {
    llam_task_t *task = g_llam_tls_task;
    llam_io_req_t *req;

    if (task != NULL && llam_task_active_io_req_load(task) == NULL) {
        req = &task->embedded_io_req;
        llam_io_req_reset(req, task->owner_runtime, shard != NULL ? shard->id : UINT_MAX, UINT_MAX);
        req->task = task;
        return req;
    }

    return llam_io_req_alloc(shard);
}

/**
 * @brief Release an I/O request acquired by ::llam_api_io_req_acquire.
 *
 * Embedded task requests are cleared in place. Heap-backed requests return to
 * the shard-local allocator.
 *
 * @param shard Shard owning the allocator.
 * @param req   Request to release; may be @c NULL.
 */
void llam_api_io_req_release(llam_shard_t *shard, llam_io_req_t *req) {
    if (req == NULL) {
        return;
    }
    if (req->alloc_owner_shard == UINT_MAX) {
        llam_io_req_reset(req, req->owner_runtime, UINT_MAX, UINT_MAX);
        return;
    }
    llam_io_req_free(shard, req);
}

/**
 * @brief Platform polling wrapper used by fallback paths.
 *
 * Kqueue platforms use a temporary kqueue so the rest of the runtime can use
 * poll-like semantics. Other platforms delegate to @c poll directly.
 *
 * @param fd         Descriptor to poll.
 * @param events     Requested events.
 * @param timeout_ms Timeout in milliseconds; negative means infinite.
 * @param revents    Optional returned events.
 *
 * @return Platform poll result.
 */
int llam_platform_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents) {
    if (LLAM_FD_IS_INVALID(fd)) {
        (void)events;
        (void)timeout_ms;
        return llam_invalid_fd_poll_result(revents);
    }
#if LLAM_RUNTIME_BACKEND_KQUEUE
    struct kevent changes[2];
    struct kevent fired[2];
    struct timespec ts;
    struct timespec *ts_ptr = NULL;
    int kq;
    int nchanges = 0;
    int rc;
    short out = 0;

    if (revents != NULL) {
        *revents = 0;
    }
    kq = kqueue();
    if (kq < 0) {
        return -1;
    }

    if ((events & (POLLIN | POLLPRI)) != 0) {
        EV_SET(&changes[nchanges++], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_ONESHOT | EV_CLEAR, 0U, 0, NULL);
    }
    if ((events & POLLOUT) != 0) {
        EV_SET(&changes[nchanges++], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_CLEAR, 0U, 0, NULL);
    }
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        ts_ptr = &ts;
    }

    rc = kevent(kq, changes, nchanges, fired, 2, ts_ptr);
    if (rc >= 0) {
        int i;

        for (i = 0; i < rc; ++i) {
            if (fired[i].flags & EV_ERROR) {
                int event_error = (int)fired[i].data;

                /*
                 * poll(2) reports per-descriptor failures through revents and
                 * still returns a positive descriptor count. kqueue
                 * reports a closed fd as EV_ERROR, and the temporary kqueue fd
                 * can even reuse the same integer value as the caller's stale
                 * descriptor. Keep the public wrapper poll-compatible by
                 * translating that registration failure into POLLNVAL instead
                 * of leaking the kqueue errno as a syscall failure.
                 */
                if (event_error == EBADF || event_error == EINVAL) {
                    out |= POLLNVAL;
                } else {
                    out |= POLLERR;
                }
                continue;
            }
            if (fired[i].filter == EVFILT_READ) {
                out |= POLLIN;
            } else if (fired[i].filter == EVFILT_WRITE) {
                out |= POLLOUT;
            }
            if ((fired[i].flags & EV_EOF) != 0) {
                out |= POLLHUP;
            }
        }
        if (revents != NULL) {
            *revents = out;
        }
        if (rc > 0) {
            /* poll returns the number of descriptors, not the number of events. */
            rc = 1;
        }
    }
    {
        int saved_errno = errno;

        close(kq);
        errno = saved_errno;
    }
    return rc;
#else
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    rc = poll(&pfd, 1, timeout_ms);
    if (revents != NULL) {
        *revents = pfd.revents;
    }
    return rc;
#endif
}

int llam_platform_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents) {
    if (revents != NULL) {
        *revents = 0;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        DWORD wait_ms;
        DWORD rc;

        if (LLAM_HANDLE_IS_INVALID(handle)) {
            errno = EINVAL;
            return -1;
        }
        wait_ms = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
        rc = WaitForSingleObject((HANDLE)handle, wait_ms);
        if (rc == WAIT_OBJECT_0) {
            if (revents != NULL) {
                *revents = events;
            }
            return 1;
        }
        if (rc == WAIT_TIMEOUT) {
            return 0;
        }
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
#else
    return llam_platform_poll_fd((llam_fd_t)handle, events, timeout_ms, revents);
#endif
}

/**
 * @brief Poll a descriptor with a zero timeout, retrying interrupted syscalls.
 *
 * @param fd      Descriptor to poll.
 * @param events  Requested events.
 * @param revents Optional returned events.
 *
 * @return Platform poll result.
 */
int llam_platform_poll_now(llam_fd_t fd, short events, short *revents) {
    struct pollfd pfd;
    int rc;

    if (LLAM_FD_IS_INVALID(fd)) {
        (void)events;
        return llam_invalid_fd_poll_result(revents);
    }

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    do {
        rc = poll(&pfd, 1, 0);
    } while (rc < 0 && errno == EINTR);
    if (revents != NULL) {
        *revents = pfd.revents;
    }
    return rc;
}

/**
 * @brief Restore temporary nonblocking flags without clobbering syscall errno.
 */
static void llam_direct_restore_flags_preserve_errno(llam_fd_t fd, bool restore_flags, int saved_flags) {
#if !LLAM_RUNTIME_BACKEND_WINDOWS
    int saved_errno;

    if (!restore_flags) {
        return;
    }
    saved_errno = errno;
    (void)fcntl(fd, F_SETFL, saved_flags);
    errno = saved_errno;
#else
    (void)fd;
    (void)restore_flags;
    (void)saved_flags;
#endif
}

/**
 * @brief Try a read/write/recv operation as an immediate non-blocking syscall.
 *
 * Return values are tri-state:
 *  - positive means the operation completed and @p result_out is valid;
 *  - zero means the descriptor would block and the caller should use another
 *    path;
 *  - negative means a hard syscall/setup error with @c errno preserved.
 *
 * Socket descriptors use @c MSG_DONTWAIT when available to avoid changing file
 * status flags. Non-socket descriptors temporarily enable @c O_NONBLOCK and
 * restore the original flags before returning.
 *
 * @param fd            Descriptor to operate on.
 * @param buf           Source or destination buffer.
 * @param count         Maximum byte count.
 * @param write_op      Whether to write/send instead of read/recv.
 * @param recv_op       Whether the read side should use @c recv.
 * @param recv_flags    Flags passed to @c recv.
 * @param result_out    Optional syscall result output.
 * @param socket_op_out Optional flag set when the socket fast path was used.
 *
 * @return 1 for completed, 0 for would-block, or -1 for hard error.
 */
int llam_try_direct_rw(llam_fd_t fd,
                            void *buf,
                            size_t count,
                            bool write_op,
                            bool recv_op,
                            int recv_flags,
                            ssize_t *result_out,
                            bool *socket_op_out) {
    int saved_flags;
    bool restore_flags = false;
    ssize_t rc;

    if (result_out != NULL) {
        *result_out = -1;
    }
    if (socket_op_out != NULL) {
        *socket_op_out = false;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (count == 0U) {
        if (result_out != NULL) {
            *result_out = 0;
        }
        if (socket_op_out != NULL) {
            *socket_op_out = true;
        }
        return 1;
    }
    {
        int nonblock_rc = llam_windows_ensure_socket_nonblocking(fd);

        if (nonblock_rc < 0) {
            return -1;
        }
        if (nonblock_rc > 0) {
            for (;;) {
                rc = write_op ? llam_platform_send_fd(fd, buf, count, 0)
                              : llam_platform_recv_fd(fd, buf, count, recv_flags);
                if (rc >= 0) {
                    if (result_out != NULL) {
                        *result_out = rc;
                    }
                    if (socket_op_out != NULL) {
                        *socket_op_out = true;
                    }
                    return 1;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return 0;
                }
                return errno == ENOTSOCK ? 0 : -1;
            }
        }
    }
#endif
#if defined(MSG_DONTWAIT)
    for (;;) {
        if (write_op) {
            rc = llam_platform_send_fd(fd, buf, count, MSG_DONTWAIT);
        } else {
            rc = llam_platform_recv_fd(fd, buf, count, recv_flags | MSG_DONTWAIT);
        }
        if (rc >= 0) {
            if (result_out != NULL) {
                *result_out = rc;
            }
            if (socket_op_out != NULL) {
                *socket_op_out = true;
            }
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno != ENOTSOCK && errno != EOPNOTSUPP
#if defined(ENOTSUP)
            && errno != ENOTSUP
#endif
        ) {
            return -1;
        }
        break;
    }
#endif
    saved_flags = fcntl(fd, F_GETFL, 0);
    if (saved_flags < 0) {
        return -1;
    }
    if ((saved_flags & O_NONBLOCK) == 0) {
        if (fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            return -1;
        }
        restore_flags = true;
    }

    for (;;) {
        if (write_op) {
            rc = llam_platform_write_fd(fd, buf, count);
        } else if (recv_op) {
            rc = llam_platform_recv_fd(fd, buf, count, recv_flags);
        } else {
            rc = llam_platform_read_fd(fd, buf, count);
        }
        if (rc >= 0) {
            llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
            if (result_out != NULL) {
                *result_out = rc;
            }
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
            return 0;
        }
        llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
        return -1;
    }
}

/**
 * @brief Try a small scatter/gather write as one immediate non-blocking syscall.
 *
 * The public ::llam_writev path uses this before falling back to per-slice
 * cooperative writes. Keeping this direct helper small avoids heap allocation
 * in the hot chat/server broadcast case.
 */
int llam_try_direct_writev(llam_fd_t fd,
                           const llam_iovec_t *iov,
                           int iovcnt,
                           ssize_t *result_out,
                           bool *socket_out) {
#if LLAM_PLATFORM_POSIX
    struct iovec native_iov[16];
    int saved_flags;
    bool restore_flags = false;
    ssize_t rc;

    if (result_out != NULL) {
        *result_out = -1;
    }
    if (socket_out != NULL) {
        *socket_out = false;
    }
    if (iovcnt < 0 || iovcnt > (int)(sizeof(native_iov) / sizeof(native_iov[0]))) {
        return 0;
    }
    for (int i = 0; i < iovcnt; ++i) {
        native_iov[i].iov_base = (void *)iov[i].iov_base;
        native_iov[i].iov_len = iov[i].iov_len;
    }

#if defined(MSG_DONTWAIT)
    {
        struct msghdr msg;

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = native_iov;
        msg.msg_iovlen = (size_t)iovcnt;
        for (;;) {
            rc = sendmsg(fd, &msg, MSG_DONTWAIT);
            if (rc >= 0) {
                if (result_out != NULL) {
                    *result_out = rc;
                }
                if (socket_out != NULL) {
                    *socket_out = true;
                }
                return 1;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno != ENOTSOCK && errno != EOPNOTSUPP
#if defined(ENOTSUP)
                && errno != ENOTSUP
#endif
            ) {
                return -1;
            }
            break;
        }
    }
#endif

    saved_flags = fcntl(fd, F_GETFL, 0);
    if (saved_flags < 0) {
        return -1;
    }
    if ((saved_flags & O_NONBLOCK) == 0) {
        if (fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            return -1;
        }
        restore_flags = true;
    }

    for (;;) {
        rc = writev(fd, native_iov, iovcnt);
        if (rc >= 0) {
            llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
            if (result_out != NULL) {
                *result_out = rc;
            }
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
            return 0;
        }
        llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
        return -1;
    }
#else
    (void)fd;
    (void)iov;
    (void)iovcnt;
    if (result_out != NULL) {
        *result_out = -1;
    }
    if (socket_out != NULL) {
        *socket_out = false;
    }
    return 0;
#endif
}

/**
 * @brief Try accepting a pending connection without parking the current task.
 *
 * Serial accept/connect workloads often have a connection already queued in
 * the kernel backlog by the time the managed task calls ::llam_accept.  Taking
 * that descriptor directly avoids an I/O worker round trip and removes a
 * backend re-arm dependency from the hot accept path.
 *
 * @return 1 for an accepted descriptor, 0 for would-block, or -1 for hard
 *         accept/setup errors with @c errno preserved.
 */
int llam_try_direct_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen, llam_fd_t *result_out) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    int nonblock_rc;
#else
    int saved_flags;
    bool restore_flags = false;
#endif
    llam_fd_t accepted;

    if (result_out != NULL) {
        *result_out = LLAM_INVALID_FD;
    }

#if LLAM_RUNTIME_BACKEND_WINDOWS
    nonblock_rc = llam_windows_ensure_socket_nonblocking(fd);
    if (nonblock_rc < 0) {
        return -1;
    }
    if (nonblock_rc == 0) {
        return 0;
    }
#else
    saved_flags = fcntl(fd, F_GETFL, 0);
    if (saved_flags < 0) {
        return -1;
    }
    if ((saved_flags & O_NONBLOCK) == 0) {
        short revents = 0;
        int poll_rc = llam_platform_poll_now(fd, POLLIN, &revents);

        /*
         * Never enter accept(2) on a blocking listener unless the kernel has
         * already reported a queued connection.  Darwin stress found that the
         * previous "temporarily flip O_NONBLOCK then accept" fast path could
         * still strand a scheduler worker in __accept under heavy watcher churn.
         */
        if (poll_rc < 0) {
            return -1;
        }
        if (poll_rc == 0 || (revents & POLLIN) == 0) {
            if ((revents & POLLNVAL) != 0) {
                errno = EBADF;
                return -1;
            }
            return 0;
        }
        if (fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK) != 0) {
            return -1;
        }
        restore_flags = true;
    }
#endif

    for (;;) {
        accepted = llam_platform_accept_fd(fd, addr, addrlen);
        if (!LLAM_FD_IS_INVALID(accepted)) {
#if !LLAM_RUNTIME_BACKEND_WINDOWS
            llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
#endif
            if (result_out != NULL) {
                *result_out = accepted;
            }
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
#if !LLAM_RUNTIME_BACKEND_WINDOWS
            llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
#endif
            return 0;
        }
#if !LLAM_RUNTIME_BACKEND_WINDOWS
        llam_direct_restore_flags_preserve_errno(fd, restore_flags, saved_flags);
#endif
        return -1;
    }
}

/**
 * @brief Resolve the completion status for a nonblocking socket connect.
 *
 * Writable readiness after @c EINPROGRESS only means the connection attempt has
 * finished. The real result is reported through @c SO_ERROR.
 *
 * @param fd Socket descriptor whose connection status should be checked.
 * @return 0 when connected, -1 with @c errno set to the connection error.
 */
int llam_socket_connect_error(llam_fd_t fd) {
    int error_code = 0;
    socklen_t error_len = sizeof(error_code);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_code, &error_len) != 0) {
        return -1;
    }
    if (error_code != 0) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
        errno = llam_windows_wsa_error_to_errno(error_code);
#else
        errno = error_code;
#endif
        return -1;
    }
    return 0;
}
