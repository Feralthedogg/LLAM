/**
 * @file src/io/runtime_io_api_internal.h
 * @brief Internal declarations shared by public I/O APIs and backend submission paths.
 *
 * @details
 * Public I/O calls pass through a layered path: direct nonblocking attempts,
 * cooperative/backend parking, multishot watch setup, and finally blocking
 * fallbacks when the backend cannot handle an operation. This header exposes
 * only the private hooks needed across those layers.
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

#ifndef LLAM_RUNTIME_IO_API_INTERNAL_H
#define LLAM_RUNTIME_IO_API_INTERNAL_H

#include "runtime_internal.h"

#if defined(__APPLE__)
#include <sys/event.h>
#endif

/* Request ownership helpers used by public I/O entry points. */
llam_io_req_t *llam_api_io_req_acquire(llam_shard_t *shard);
void llam_api_io_req_release(llam_shard_t *shard, llam_io_req_t *req);

/* Direct syscall and poll probes used before parking a task. */
static inline ssize_t llam_platform_read_fd(llam_fd_t fd, void *buf, size_t count) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return llam_windows_socket_recv(fd, buf, count, 0);
#else
    return read(fd, buf, count);
#endif
}

static inline ssize_t llam_platform_write_fd(llam_fd_t fd, const void *buf, size_t count) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return llam_windows_socket_send(fd, buf, count, 0);
#else
    return write(fd, buf, count);
#endif
}

static inline ssize_t llam_platform_read_handle(llam_handle_t handle, void *buf, size_t count) {
    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        DWORD transferred = 0;

        if (LLAM_HANDLE_IS_INVALID(handle) || count > (size_t)ULONG_MAX) {
            errno = EINVAL;
            return -1;
        }
        if (!ReadFile((HANDLE)handle, buf, (DWORD)count, &transferred, NULL)) {
            errno = llam_windows_system_error_to_errno(GetLastError());
            return -1;
        }
        return (ssize_t)transferred;
    }
#else
    return read((llam_fd_t)handle, buf, count);
#endif
}

static inline ssize_t llam_platform_write_handle(llam_handle_t handle, const void *buf, size_t count) {
    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        DWORD transferred = 0;

        if (LLAM_HANDLE_IS_INVALID(handle) || count > (size_t)ULONG_MAX) {
            errno = EINVAL;
            return -1;
        }
        if (!WriteFile((HANDLE)handle, buf, (DWORD)count, &transferred, NULL)) {
            errno = llam_windows_system_error_to_errno(GetLastError());
            return -1;
        }
        return (ssize_t)transferred;
    }
#else
    return write((llam_fd_t)handle, buf, count);
#endif
}

static inline ssize_t llam_platform_recv_fd(llam_fd_t fd, void *buf, size_t count, int flags) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return llam_windows_socket_recv(fd, buf, count, flags);
#else
    return recv(fd, buf, count, flags);
#endif
}

static inline ssize_t llam_platform_send_fd(llam_fd_t fd, const void *buf, size_t count, int flags) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return llam_windows_socket_send(fd, buf, count, flags);
#else
    return send(fd, buf, count, flags);
#endif
}

static inline llam_fd_t llam_platform_accept_fd(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return llam_windows_socket_accept(fd, addr, addrlen);
#else
    return accept(fd, addr, addrlen);
#endif
}

static inline int llam_platform_connect_fd(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    return llam_windows_socket_connect(fd, addr, addrlen);
#else
    return connect(fd, addr, addrlen);
#endif
}

int llam_platform_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);
int llam_platform_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents);
int llam_platform_poll_now(llam_fd_t fd, short events, short *revents);
int llam_try_direct_rw(llam_fd_t fd,
                     void *buf,
                     size_t count,
                     bool is_write,
                     bool socket_recv,
                     int recv_flags,
                     ssize_t *result_out,
                     bool *socket_out);
int llam_try_direct_writev(llam_fd_t fd,
                           const llam_iovec_t *iov,
                           int iovcnt,
                           ssize_t *result_out,
                           bool *socket_out);
int llam_try_direct_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen, llam_fd_t *result_out);
bool llam_io_coop_yield_enabled(void);
bool llam_io_poll_coop_yield_enabled(void);
bool llam_io_poll_extra_yield_enabled(void);
bool llam_io_poll_pre_yield_enabled(void);
unsigned llam_io_poll_ready_yields(void);
bool llam_io_shard_has_local_work(void);
void llam_maybe_handoff_after_socket_write(llam_fd_t fd, size_t count, bool known_socket);
int llam_try_direct_blocking_rw(llam_fd_t fd,
                              void *buf,
                              size_t count,
                              bool is_write,
                              bool socket_recv,
                              int recv_flags,
                              ssize_t *result_out);
int llam_try_direct_blocking_rw_forced(llam_fd_t fd,
                                     void *buf,
                                     size_t count,
                                     bool is_write,
                                     bool socket_recv,
                                     int recv_flags,
                                     ssize_t *result_out);
int llam_socket_connect_error(llam_fd_t fd);
int llam_try_socket_pollin_now(llam_fd_t fd, short events, short *revents);
int llam_try_direct_blocking_poll(llam_fd_t fd, short events, int timeout_ms, short *revents);
#if LLAM_RUNTIME_BACKEND_WINDOWS
bool llam_windows_socket_nonblocking_cached(llam_fd_t fd);
bool llam_windows_iocp_poll_supported(llam_fd_t fd, short events);
#endif

/* Blocking fallback callbacks executed on the runtime blocking pool. */
void *llam_blocking_read_impl(void *arg);
void *llam_blocking_write_impl(void *arg);
void *llam_blocking_accept_impl(void *arg);
void *llam_blocking_connect_impl(void *arg);
void *llam_blocking_poll_impl(void *arg);
void *llam_blocking_handle_read_impl(void *arg);
void *llam_blocking_handle_write_impl(void *arg);
void *llam_blocking_handle_poll_impl(void *arg);
ssize_t llam_read_owned_impl(llam_fd_t fd,
                           size_t max_count,
                           int recv_flags,
                           bool socket_recv,
                           llam_io_buffer_t **out);

/* Cooperative I/O parking and backend issue paths. */
void llam_cleanup_io_wait_setup(llam_task_t *task, llam_io_req_t *req);
int llam_park_io_req(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns, llam_node_t *wake_node);
int llam_issue_multishot_poll(llam_io_req_t *req);
int llam_issue_multishot_accept(llam_io_req_t *req);
int llam_issue_multishot_recv(llam_io_req_t *req);
int llam_issue_io(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns);
bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target);

#endif
