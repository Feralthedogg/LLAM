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
nm_io_req_t *nm_api_io_req_acquire(nm_shard_t *shard) {
    nm_task_t *task = g_nm_tls_task;
    nm_io_req_t *req;

    if (task != NULL && task->active_io_req == NULL) {
        req = &task->embedded_io_req;
        memset(req, 0, sizeof(*req));
        req->task = task;
        req->owner_shard = shard != NULL ? shard->id : UINT_MAX;
        req->alloc_owner_shard = UINT_MAX;
        req->attached_node_index = UINT_MAX;
        atomic_store(&req->inflight_owner_shard, UINT_MAX);
        return req;
    }

    return nm_io_req_alloc(shard);
}

/**
 * @brief Release an I/O request acquired by ::nm_api_io_req_acquire.
 *
 * Embedded task requests are cleared in place. Heap-backed requests return to
 * the shard-local allocator.
 *
 * @param shard Shard owning the allocator.
 * @param req   Request to release; may be @c NULL.
 */
void nm_api_io_req_release(nm_shard_t *shard, nm_io_req_t *req) {
    if (req == NULL) {
        return;
    }
    if (req->alloc_owner_shard == UINT_MAX) {
        memset(req, 0, sizeof(*req));
        return;
    }
    nm_io_req_free(shard, req);
}

/**
 * @brief Platform polling wrapper used by fallback paths.
 *
 * Darwin uses a temporary kqueue so the rest of the runtime can use poll-like
 * semantics. Other platforms delegate to @c poll directly.
 *
 * @param fd         Descriptor to poll.
 * @param events     Requested events.
 * @param timeout_ms Timeout in milliseconds; negative means infinite.
 * @param revents    Optional returned events.
 *
 * @return Platform poll result.
 */
int nm_platform_poll_fd(int fd, short events, int timeout_ms, short *revents) {
#if defined(__APPLE__)
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
                out |= POLLERR;
                if (fired[i].data != 0) {
                    errno = (int)fired[i].data;
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
        if (rc > 0 && out == 0) {
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

/**
 * @brief Poll a descriptor with a zero timeout, retrying interrupted syscalls.
 *
 * @param fd      Descriptor to poll.
 * @param events  Requested events.
 * @param revents Optional returned events.
 *
 * @return Platform poll result.
 */
int nm_platform_poll_now(int fd, short events, short *revents) {
    struct pollfd pfd;
    int rc;

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
int nm_try_direct_rw(int fd,
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
#if defined(MSG_DONTWAIT)
    for (;;) {
        if (write_op) {
            rc = send(fd, buf, count, MSG_DONTWAIT);
        } else {
            rc = recv(fd, buf, count, recv_flags | MSG_DONTWAIT);
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
            rc = write(fd, buf, count);
        } else if (recv_op) {
            rc = recv(fd, buf, count, recv_flags);
        } else {
            rc = read(fd, buf, count);
        }
        if (rc >= 0) {
            if (restore_flags) {
                int saved_errno = errno;
                (void)fcntl(fd, F_SETFL, saved_flags);
                errno = saved_errno;
            }
            if (result_out != NULL) {
                *result_out = rc;
            }
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (restore_flags) {
                int saved_errno = errno;
                (void)fcntl(fd, F_SETFL, saved_flags);
                errno = saved_errno;
            }
            return 0;
        }
        {
            int saved_errno = errno;

            if (restore_flags) {
                (void)fcntl(fd, F_SETFL, saved_flags);
            }
            errno = saved_errno;
        }
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
int nm_socket_connect_error(int fd) {
    int error_code = 0;
    socklen_t error_len = sizeof(error_code);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error_code, &error_len) != 0) {
        return -1;
    }
    if (error_code != 0) {
        errno = error_code;
        return -1;
    }
    return 0;
}
