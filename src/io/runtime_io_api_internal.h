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
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

#ifndef LLAM_RUNTIME_IO_API_INTERNAL_H
#define LLAM_RUNTIME_IO_API_INTERNAL_H

#include "runtime_internal.h"

#if LLAM_RUNTIME_BACKEND_KQUEUE
#include <sys/event.h>
#endif

#include <limits.h>

/* Request ownership helpers used by public I/O entry points. */
llam_io_req_t *llam_api_io_req_acquire(llam_shard_t *shard);
void llam_api_io_req_release(llam_shard_t *shard, llam_io_req_t *req);
#if defined(LLAM_ENABLE_TEST_HOOKS)
bool llam_io_test_abort_published_io_setup(llam_io_req_t *req,
                                           llam_io_abort_reason_t reason,
                                           bool *wait_for_completion);
#endif

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
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        DWORD transferred = 0;

        if (buf == NULL && count != 0U) {
            errno = EINVAL;
            return -1;
        }
        if (LLAM_HANDLE_IS_INVALID(handle) || count > (size_t)ULONG_MAX) {
            errno = EINVAL;
            return -1;
        }
        if (!ReadFile((HANDLE)handle, buf, (DWORD)count, &transferred, NULL)) {
            DWORD error_code = GetLastError();

            if (error_code == ERROR_HANDLE_EOF) {
                return 0;
            }
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
        return (ssize_t)transferred;
    }
#else
    return read((llam_fd_t)handle, buf, count);
#endif
}

static inline ssize_t llam_platform_write_handle(llam_handle_t handle, const void *buf, size_t count) {
#if LLAM_RUNTIME_BACKEND_WINDOWS

    {
        DWORD transferred = 0;

        if (buf == NULL && count != 0U) {
            errno = EINVAL;
            return -1;
        }
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

#if LLAM_PLATFORM_POSIX
static inline int llam_offset_to_off_t(uint64_t offset, off_t *out) {
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (offset > (uint64_t)LLONG_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    *out = (off_t)offset;
    if ((uint64_t)*out != offset) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}
#endif

static inline ssize_t llam_platform_pread_fd(llam_fd_t fd, void *buf, size_t count, uint64_t offset) {
#if LLAM_PLATFORM_POSIX
    off_t native_offset;

    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_offset_to_off_t(offset, &native_offset) != 0) {
        return -1;
    }
    return pread(fd, buf, count, native_offset);
#else
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    errno = ENOTSUP;
    return -1;
#endif
}

static inline ssize_t llam_platform_pwrite_fd(llam_fd_t fd, const void *buf, size_t count, uint64_t offset) {
#if LLAM_PLATFORM_POSIX
    off_t native_offset;

    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_offset_to_off_t(offset, &native_offset) != 0) {
        return -1;
    }
    return pwrite(fd, buf, count, native_offset);
#else
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    errno = ENOTSUP;
    return -1;
#endif
}

#if LLAM_RUNTIME_BACKEND_WINDOWS
/* The documented NT file-information ABI, kept local to avoid SDK typedef clashes. */
typedef struct llam_windows_file_io_status {
    union { LONG status; PVOID pointer; } value;
    ULONG_PTR information;
} llam_windows_file_io_status_t;

static inline int llam_windows_query_file_ulong(HANDLE handle, ULONG info_class, ULONG *out) {
    typedef LONG(NTAPI *query_fn)(HANDLE, llam_windows_file_io_status_t *, PVOID, ULONG, ULONG);
    query_fn query = NULL;
    HMODULE module = GetModuleHandleA("ntdll.dll");
    FARPROC symbol = module != NULL ? GetProcAddress(module, "NtQueryInformationFile") : NULL;
    llam_windows_file_io_status_t status = {0};

    if (symbol == NULL || sizeof(query) != sizeof(symbol)) {
        errno = ENOTSUP;
        return -1;
    }
    memcpy(&query, &symbol, sizeof(query));
    if (query(handle, &status, out, (ULONG)sizeof(*out), info_class) < 0) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static inline int llam_windows_handle_is_synchronous(HANDLE handle, bool *out) {
    ULONG mode = 0U;

    if (llam_windows_query_file_ulong(handle, 16U, &mode) != 0) {
        return -1;
    }
    *out = (mode & 0x30U) != 0U; /* FILE_SYNCHRONOUS_IO_ALERT / NONALERT */
    return 0;
}

static inline HANDLE llam_windows_positional_handle(HANDLE handle, bool write_op, bool *reopened) {
    ULONG mode = 0U;
    ULONG access = 0U;
    DWORD needed = write_op ? FILE_WRITE_DATA : FILE_READ_DATA;
    DWORD flags = FILE_FLAG_OVERLAPPED;
    HANDLE result;

    *reopened = false;
    if (llam_windows_query_file_ulong(handle, 16U, &mode) != 0) {
        return INVALID_HANDLE_VALUE;
    }
    if ((mode & 0x30U) == 0U) {
        return handle;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        errno = ENOTSUP;
        return INVALID_HANDLE_VALUE;
    }
    /* Reopening must not acquire data access absent from the caller's handle. */
    if (llam_windows_query_file_ulong(handle, 8U, &access) != 0) {
        return INVALID_HANDLE_VALUE;
    }
    if ((access & needed) == 0U) {
        errno = EBADF;
        return INVALID_HANDLE_VALUE;
    }
    /* Preserve the source file object's buffering/durability mode on reopen. */
    if ((mode & 0x2U) != 0U) { /* FILE_WRITE_THROUGH */
        flags |= FILE_FLAG_WRITE_THROUGH;
    }
    if ((mode & 0x8U) != 0U) { /* FILE_NO_INTERMEDIATE_BUFFERING */
        flags |= FILE_FLAG_NO_BUFFERING;
    }
    result = ReOpenFile(handle, needed | SYNCHRONIZE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        flags);
    if (result == INVALID_HANDLE_VALUE) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return result;
    }
    *reopened = true;
    return result;
}

static inline void llam_windows_set_overlapped_offset(OVERLAPPED *overlapped, uint64_t offset) {
    if (overlapped != NULL) {
        overlapped->Offset = (DWORD)(offset & UINT64_C(0xffffffff));
        overlapped->OffsetHigh = (DWORD)(offset >> 32U);
    }
}
#endif

static inline ssize_t llam_platform_pread_handle(llam_handle_t handle, void *buf, size_t count, uint64_t offset) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        OVERLAPPED overlapped;
        DWORD transferred = 0;
        HANDLE io_handle, event;
        bool reopened;
        DWORD error_code = ERROR_SUCCESS;

        if (buf == NULL && count != 0U) {
            errno = EINVAL;
            return -1;
        }
        if (LLAM_HANDLE_IS_INVALID(handle) || count > (size_t)ULONG_MAX) {
            errno = EINVAL;
            return -1;
        }
        io_handle = llam_windows_positional_handle((HANDLE)handle, false, &reopened);
        if (io_handle == INVALID_HANDLE_VALUE) {
            return -1;
        }
        event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL) {
            error_code = GetLastError();
            if (reopened) {
                CloseHandle(io_handle);
            }
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
        memset(&overlapped, 0, sizeof(overlapped));
        /* The low event bit suppresses IOCP packets referencing stack storage. */
        overlapped.hEvent = (HANDLE)((uintptr_t)event | 1U);
        llam_windows_set_overlapped_offset(&overlapped, offset);
        if (!ReadFile(io_handle, buf, (DWORD)count, &transferred, &overlapped)) {
            error_code = GetLastError();
            if (error_code == ERROR_IO_PENDING) {
                error_code = GetOverlappedResult(io_handle, &overlapped, &transferred, TRUE)
                                 ? ERROR_SUCCESS : GetLastError();
            }
        }
        CloseHandle(event);
        if (reopened) {
            CloseHandle(io_handle);
        }
        if (error_code == ERROR_HANDLE_EOF) {
            return 0;
        }
        if (error_code != ERROR_SUCCESS) {
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
        return (ssize_t)transferred;
    }
#else
    return llam_platform_pread_fd((llam_fd_t)handle, buf, count, offset);
#endif
}

static inline ssize_t llam_platform_pwrite_handle(llam_handle_t handle, const void *buf, size_t count, uint64_t offset) {
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        OVERLAPPED overlapped;
        DWORD transferred = 0;
        HANDLE io_handle, event;
        bool reopened;
        DWORD error_code = ERROR_SUCCESS;

        if (buf == NULL && count != 0U) {
            errno = EINVAL;
            return -1;
        }
        if (LLAM_HANDLE_IS_INVALID(handle) || count > (size_t)ULONG_MAX) {
            errno = EINVAL;
            return -1;
        }
        io_handle = llam_windows_positional_handle((HANDLE)handle, true, &reopened);
        if (io_handle == INVALID_HANDLE_VALUE) {
            return -1;
        }
        event = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (event == NULL) {
            error_code = GetLastError();
            if (reopened) {
                CloseHandle(io_handle);
            }
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
        memset(&overlapped, 0, sizeof(overlapped));
        overlapped.hEvent = (HANDLE)((uintptr_t)event | 1U);
        llam_windows_set_overlapped_offset(&overlapped, offset);
        if (!WriteFile(io_handle, buf, (DWORD)count, &transferred, &overlapped)) {
            error_code = GetLastError();
            if (error_code == ERROR_IO_PENDING) {
                error_code = GetOverlappedResult(io_handle, &overlapped, &transferred, TRUE)
                                 ? ERROR_SUCCESS : GetLastError();
            }
        }
        CloseHandle(event);
        if (reopened) {
            CloseHandle(io_handle);
        }
        if (error_code != ERROR_SUCCESS) {
            errno = llam_windows_system_error_to_errno(error_code);
            return -1;
        }
        return (ssize_t)transferred;
    }
#else
    return llam_platform_pwrite_fd((llam_fd_t)handle, buf, count, offset);
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

static inline int llam_platform_close_fd(llam_fd_t fd) {
    if (LLAM_FD_IS_INVALID(fd)) {
        errno = EBADF;
        return -1;
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    if (closesocket(fd) != 0) {
        errno = llam_windows_wsa_error_to_errno(WSAGetLastError());
        return -1;
    }
    return 0;
#else
    return close(fd);
#endif
}

int llam_platform_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);
int llam_platform_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents);
int llam_platform_poll_now(llam_fd_t fd, short events, short *revents);

llam_io_buffer_t *llam_io_buffer_alloc_detached(size_t min_capacity, size_t alignment, uint32_t flags);
int llam_io_scope_error(const llam_task_t *task);
int llam_io_buffer_public_register(llam_io_buffer_t *buffer);
llam_io_buffer_t *llam_io_buffer_public_handle(llam_io_buffer_t *buffer);
llam_io_buffer_t *llam_io_buffer_public_unregister(llam_io_buffer_t *buffer);
llam_io_buffer_t *llam_io_buffer_public_unregister_raw(llam_io_buffer_t *buffer);
llam_io_buffer_t *llam_io_buffer_public_begin_op(const llam_io_buffer_t *buffer);
void llam_io_buffer_public_end_op(const llam_io_buffer_t *buffer);
void *llam_io_buffer_public_data(llam_io_buffer_t *buffer);
void llam_io_buffer_release_raw(llam_io_buffer_t *buffer);
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
void llam_windows_socket_nonblocking_forget(llam_runtime_t *rt, llam_fd_t fd);
void llam_windows_forget_fd_assoc(llam_runtime_t *rt, llam_fd_t fd);
bool llam_windows_iocp_poll_supported(llam_fd_t fd, short events);
#endif

/* Blocking fallback callbacks executed on the runtime blocking pool. */
bool llam_blocking_req_cancelled(const llam_io_req_t *req);
void llam_blocking_req_set_cancelled(llam_io_req_t *req);
void *llam_blocking_read_impl(void *arg);
void *llam_blocking_write_impl(void *arg);
void *llam_blocking_accept_impl(void *arg);
void *llam_blocking_connect_impl(void *arg);
void *llam_blocking_poll_impl(void *arg);
void *llam_blocking_handle_read_impl(void *arg);
void *llam_blocking_handle_write_impl(void *arg);
void *llam_blocking_pread_impl(void *arg);
void *llam_blocking_pwrite_impl(void *arg);
void *llam_blocking_handle_pread_impl(void *arg);
void *llam_blocking_handle_pwrite_impl(void *arg);
void *llam_blocking_handle_poll_impl(void *arg);
ssize_t llam_read_owned_impl(llam_fd_t fd,
                           size_t max_count,
                           int recv_flags,
                           bool socket_recv,
                           llam_io_buffer_t **out);

/* Shared positional I/O validation and fallback helpers. */
int llam_positional_iovcnt_max(void);
int llam_positional_validate_async_rw_count(size_t count);
int llam_positional_const_iov_validate(const llam_iovec_t *iov, int iovcnt);
int llam_positional_mut_iov_validate(const llam_mut_iovec_t *iov, int iovcnt);
int llam_positional_offset_advance(uint64_t *offset, ssize_t amount);
int llam_positional_call_blocking_io(llam_blocking_fn fn, llam_io_req_t *req);

/* Cooperative I/O parking and backend issue paths. */
void llam_cleanup_io_wait_setup(llam_task_t *task, llam_io_req_t *req);
int llam_park_io_req(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns, llam_node_t *wake_node);
int llam_issue_multishot_poll(llam_io_req_t *req);
int llam_issue_multishot_accept(llam_io_req_t *req);
int llam_issue_multishot_recv(llam_io_req_t *req);
int llam_issue_io(llam_io_req_t *req, bool has_deadline, uint64_t deadline_ns);
bool llam_drop_node_control_locked(llam_node_t *node, llam_io_control_kind_t kind, const void *target);

#endif
