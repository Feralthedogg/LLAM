/**
 * @file include/llam/io.h
 * @brief Public LLAM I/O declarations included by llam/runtime.h.
 *
 * @details
 * This header is split out to keep the canonical runtime header navigable while
 * preserving source compatibility: including <llam/runtime.h> still exposes the
 * complete public API, and including <llam/io.h> directly first includes the
 * canonical runtime header.
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

#ifndef LLAM_IO_H
#define LLAM_IO_H
#endif

#ifndef LLAM_RUNTIME_H
#include "llam/runtime.h"
#else
#ifndef LLAM_IO_DECLS_INCLUDED
#define LLAM_IO_DECLS_INCLUDED

/** @brief Portable immutable scatter/gather I/O slice used by write APIs. */
typedef struct llam_iovec {
    const void *iov_base; /**< Pointer to bytes to write. */
    size_t iov_len;      /**< Number of bytes in this slice. */
} llam_iovec_t;

/** @brief Portable mutable scatter/gather I/O slice used by read APIs. */
typedef struct llam_mut_iovec {
    void *iov_base; /**< Pointer to bytes to fill. */
    size_t iov_len; /**< Number of bytes in this slice. */
} llam_mut_iovec_t;

/** @brief Runtime-owned I/O buffer allocation flags. */
#define LLAM_IO_BUFFER_F_ZERO_FILL UINT32_C(1)

/** @brief Options for ::llam_io_buffer_alloc_ex. */
typedef struct llam_io_buffer_opts {
    size_t capacity;   /**< Minimum usable buffer capacity in bytes. */
    size_t alignment;  /**< Required power-of-two alignment, or 0 for default. */
    uint32_t flags;    /**< Bitwise OR of LLAM_IO_BUFFER_F_* flags. */
    uint32_t reserved0; /**< Reserved ABI padding; initialize to 0. */
} llam_io_buffer_opts_t;

/** @brief Current size to pass to ::llam_io_buffer_opts_init and ::llam_io_buffer_alloc_ex. */
#define LLAM_IO_BUFFER_OPTS_CURRENT_SIZE ((size_t)sizeof(llam_io_buffer_opts_t))

/* ============================================================================
 * Runtime I/O and owned buffers
 * ============================================================================
 */

/**
 * @brief Read from fd using the runtime I/O backend where possible.
 *
 * @details Managed tasks first try a direct nonblocking fast path, then the
 * platform backend, then a blocking helper so the scheduler worker is not
 * pinned. Calls outside a managed LLAM task delegate to the platform read
 * primitive directly and may block the calling OS thread.
 */
LLAM_API ssize_t llam_read(llam_fd_t fd, void *buf, size_t count);

/**
 * @brief Read from a generic platform handle.
 *
 * @details
 * On Windows this API submits overlapped @c ReadFile operations to the native
 * IOCP backend when the handle supports overlapped I/O. If the backend cannot
 * accept the handle, managed tasks fall back to the blocking-helper pool so the
 * scheduler worker is not pinned. On POSIX, ::llam_handle_t aliases
 * ::llam_fd_t and this function delegates to ::llam_read.
 *
 * This API is intended for pipe, device, and explicitly overlapped HANDLE
 * integrations. Sequential disk-file wrappers may prefer blocking offload
 * unless they manage file offsets explicitly.
 */
LLAM_API ssize_t llam_read_handle(llam_handle_t handle, void *buf, size_t count);

/**
 * @brief Wait for read readiness and read in one runtime operation.
 *
 * @details
 * Managed tasks first try an immediate nonblocking read. If the descriptor is
 * not ready, LLAM waits for @c POLLIN and then retries the read directly,
 * avoiding the duplicate safepoint/readiness checks of a separate
 * ::llam_poll_fd + ::llam_read pair. Calls outside a managed LLAM task delegate
 * to platform poll/read primitives and may block the calling OS thread.
 *
 * @param fd         File descriptor to read from.
 * @param buf        Destination buffer.
 * @param count      Maximum bytes to read.
 * @param timeout_ms Timeout in milliseconds; negative means infinite.
 *
 * @return Number of bytes read.
 * @return -1 with @c errno set to @c ETIMEDOUT if the readiness wait expires,
 *         or another error from the poll/read path.
 */
LLAM_API ssize_t llam_read_when_ready(llam_fd_t fd, void *buf, size_t count, int timeout_ms);

/**
 * @brief Write to fd using the runtime I/O backend where possible.
 *
 * @details Managed tasks first try a direct nonblocking fast path, then the
 * platform backend, then a blocking helper so the scheduler worker is not
 * pinned. Calls outside a managed LLAM task delegate to the platform write
 * primitive directly and may block the calling OS thread.
 */
LLAM_API ssize_t llam_write(llam_fd_t fd, const void *buf, size_t count);

/**
 * @brief Scatter/gather write using the runtime I/O path where possible.
 *
 * @details Calls outside a managed LLAM task use the platform vector-write
 * primitive where available. Managed POSIX tasks first try a nonblocking
 * scatter/gather syscall and park cooperatively for writable readiness before
 * falling back to per-slice ::llam_write. A partial return may occur after any
 * slice boundary or partial slice write.
 *
 * @param fd File descriptor to write to.
 * @param iov Array of buffers to write.
 * @param iovcnt Number of buffers in @p iov.
 * @return Number of bytes written, or -1 with @c errno set.
 */
LLAM_API ssize_t llam_writev(llam_fd_t fd, const llam_iovec_t *iov, int iovcnt);

/**
 * @brief Positional read from a POSIX-style descriptor.
 *
 * @details The file offset is not changed. On Windows, ::llam_fd_t is a SOCKET
 * and positional file I/O is exposed through ::llam_pread_handle instead, so
 * this function fails with @c ENOTSUP.
 */
LLAM_API ssize_t llam_pread(llam_fd_t fd, void *buf, size_t count, uint64_t offset);

/**
 * @brief Positional write to a POSIX-style descriptor without changing file offset.
 */
LLAM_API ssize_t llam_pwrite(llam_fd_t fd, const void *buf, size_t count, uint64_t offset);

/**
 * @brief Scatter/gather positional read from a POSIX-style descriptor.
 */
LLAM_API ssize_t llam_preadv(llam_fd_t fd, const llam_mut_iovec_t *iov, int iovcnt, uint64_t offset);

/**
 * @brief Scatter/gather positional write to a POSIX-style descriptor.
 */
LLAM_API ssize_t llam_pwritev(llam_fd_t fd, const llam_iovec_t *iov, int iovcnt, uint64_t offset);

/**
 * @brief Write to a generic platform handle.
 *
 * @details
 * On Windows this API submits overlapped @c WriteFile operations to the native
 * IOCP backend when the handle supports overlapped I/O. If the backend cannot
 * accept the handle, managed tasks fall back to the blocking-helper pool. On
 * POSIX, ::llam_handle_t aliases ::llam_fd_t and this function delegates to
 * ::llam_write.
 */
LLAM_API ssize_t llam_write_handle(llam_handle_t handle, const void *buf, size_t count);

/**
 * @brief Positional read from a generic platform handle.
 *
 * @details Windows submits overlapped @c ReadFile with the supplied offset. On
 * POSIX, ::llam_handle_t aliases ::llam_fd_t and this delegates to
 * ::llam_pread.
 */
LLAM_API ssize_t llam_pread_handle(llam_handle_t handle, void *buf, size_t count, uint64_t offset);

/**
 * @brief Positional write to a generic platform handle.
 */
LLAM_API ssize_t llam_pwrite_handle(llam_handle_t handle, const void *buf, size_t count, uint64_t offset);

/**
 * @brief Scatter/gather positional read from a generic platform handle.
 */
LLAM_API ssize_t llam_preadv_handle(llam_handle_t handle, const llam_mut_iovec_t *iov, int iovcnt, uint64_t offset);

/**
 * @brief Scatter/gather positional write to a generic platform handle.
 */
LLAM_API ssize_t llam_pwritev_handle(llam_handle_t handle, const llam_iovec_t *iov, int iovcnt, uint64_t offset);

/**
 * @brief Close an fd/socket after invalidating LLAM-local descriptor state.
 * @details Prefer this for descriptors used with LLAM I/O; raw close remains
 * legal but gives diagnostics less context around descriptor reuse.
 * @return 0 on close, -1 with @c errno set. @c LLAM_INVALID_FD fails with @c EBADF.
 */
LLAM_API int llam_close(llam_fd_t fd);

/**
 * @brief Close a generic platform handle after invalidating LLAM-local handle state.
 * @details Windows calls @c CloseHandle. POSIX delegates to ::llam_close.
 */
LLAM_API int llam_close_handle(llam_handle_t handle);

/**
 * @brief Read into a runtime-owned buffer.
 *
 * @details
 * On success with a positive byte count, @p out receives a non-NULL buffer that
 * must be released with ::llam_io_buffer_release. The returned buffer remains
 * valid until release, even if the runtime is shut down first. On EOF or
 * zero-byte read, the function returns @c 0 and stores @c NULL in @p out. On
 * failure, the function returns @c -1, stores @c NULL in @p out, and sets
 * @c errno.
 */
LLAM_API ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out);

/**
 * @brief Receive into a runtime-owned buffer with recv flags.
 *
 * @details
 * On success with a positive byte count, @p out receives a non-NULL buffer that
 * must be released with ::llam_io_buffer_release. The returned buffer remains
 * valid until release, even if the runtime is shut down first. On EOF or
 * zero-byte receive, the function returns @c 0 and stores @c NULL in @p out. On
 * failure, the function returns @c -1, stores @c NULL in @p out, and sets
 * @c errno.
 */
LLAM_API ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out);

/**
 * @brief Initialize I/O buffer allocation options with ABI-safe defaults.
 *
 * @details LLAM writes only the prefix known to the loaded library. Caller-side
 * tail bytes from newer struct definitions are left untouched.
 */
LLAM_API int llam_io_buffer_opts_init(llam_io_buffer_opts_t *opts, size_t opts_size);

/** @brief Allocate a runtime-owned buffer with explicit capacity/alignment options. */
LLAM_API int llam_io_buffer_alloc_ex(const llam_io_buffer_opts_t *opts, size_t opts_size, llam_io_buffer_t **out);

/** @brief Allocate a runtime-owned buffer with default alignment. */
LLAM_API int llam_io_buffer_alloc(size_t capacity, llam_io_buffer_t **out);

/** @brief Allocate a runtime-owned buffer with a power-of-two alignment. */
LLAM_API int llam_io_buffer_alloc_aligned(size_t capacity, size_t alignment, llam_io_buffer_t **out);

/** @brief Return the alignment requested when the owned buffer was allocated. */
LLAM_API size_t llam_io_buffer_alignment(const llam_io_buffer_t *buffer);

/**
 * @brief Positional read into a newly allocated aligned owned buffer.
 *
 * @details On success with a positive byte count, @p out receives a buffer that
 * must be released with ::llam_io_buffer_release. EOF returns 0 and stores
 * NULL. Failure returns -1, stores NULL in @p out, and sets @c errno.
 */
LLAM_API ssize_t llam_pread_owned_aligned(llam_fd_t fd,
                                          size_t max_count,
                                          uint64_t offset,
                                          size_t alignment,
                                          llam_io_buffer_t **out);

/** @brief HANDLE variant of ::llam_pread_owned_aligned. */
LLAM_API ssize_t llam_pread_handle_owned_aligned(llam_handle_t handle,
                                                 size_t max_count,
                                                 uint64_t offset,
                                                 size_t alignment,
                                                 llam_io_buffer_t **out);

/**
 * @brief Release a runtime-owned I/O buffer.
 *
 * @details The buffer handle is consumed. Duplicate or stale releases are
 * ignored, but any data pointer previously returned by ::llam_io_buffer_data is
 * invalid once release begins. Callers that share a buffer between threads must
 * serialize release against data-pointer use.
 */
LLAM_API void llam_io_buffer_release(llam_io_buffer_t *buffer);

/**
 * @brief Return the data pointer for a runtime-owned I/O buffer.
 *
 * @details The returned pointer is a borrowed view into @p buffer. It remains
 * valid only while the buffer remains unreleased; LLAM protects the accessor
 * itself from stale public handles, but it cannot make a raw C pointer safe
 * after another thread releases the buffer.
 */
LLAM_API void *llam_io_buffer_data(llam_io_buffer_t *buffer);

/** @brief Return the number of valid bytes in a runtime-owned I/O buffer. */
LLAM_API size_t llam_io_buffer_size(const llam_io_buffer_t *buffer);

/** @brief Return total capacity of a runtime-owned I/O buffer. */
LLAM_API size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer);

/**
 * @brief Accept a connection from a listener fd using the runtime I/O backend where possible.
 *
 * @details Managed tasks submit to the platform backend where possible and
 * otherwise use a blocking helper so the scheduler worker is not pinned. Calls
 * outside a managed LLAM task delegate to the platform accept primitive directly
 * and may block the calling OS thread. @p addr and @p addrlen must either both
 * be NULL or both be non-NULL; mismatched peer-address outputs fail with
 * @c EINVAL before any connection is consumed.
 *
 * @return Accepted descriptor on success, or @c LLAM_INVALID_FD on failure with
 * @c errno set.
 */
LLAM_API llam_fd_t llam_accept(llam_fd_t fd, struct sockaddr *addr, socklen_t *addrlen);

/**
 * @brief Connect a socket without blocking the scheduler worker.
 *
 * Managed tasks submit the connection attempt to the runtime backend where
 * possible and otherwise use the blocking-worker fallback. Calls outside a
 * managed task delegate to @c connect directly.
 *
 * @param fd      Socket descriptor.
 * @param addr    Peer socket address. Must not be NULL.
 * @param addrlen Size of @p addr in bytes.
 * @return 0 on connection, -1 with @c errno set on failure.
 */
LLAM_API int llam_connect(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen);

/**
 * @brief Wait for fd readiness.
 *
 * @details
 * Calls from managed LLAM tasks park cooperatively. Calls outside a managed
 * task delegate to the platform poll/select backend and may block the calling
 * OS thread. @p timeout_ms < 0 waits indefinitely, @p timeout_ms == 0 performs
 * a non-blocking readiness check, and positive values bound the wait in
 * milliseconds.
 */
LLAM_API int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);

/**
 * @brief Wait for a generic platform handle.
 * @details Windows HANDLE waits use the blocking-helper pool in managed tasks;
 * this is not socket readiness and does not replace ::llam_poll_fd. POSIX
 * delegates to ::llam_poll_fd.
 */
LLAM_API int llam_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents);


#endif /* LLAM_IO_DECLS_INCLUDED */
#endif /* LLAM_RUNTIME_H */
