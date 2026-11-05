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

#ifndef LLAM_IO_H
#define LLAM_IO_H
#endif

#ifndef LLAM_RUNTIME_H
#include "llam/runtime.h"
#else
#ifndef LLAM_IO_DECLS_INCLUDED
#define LLAM_IO_DECLS_INCLUDED

#if LLAM_PLATFORM_POSIX
#include <netdb.h>
#endif

/*
 * Keep the SDK headers self-contained under strict C modes where <netdb.h>
 * may hide addrinfo unless feature macros were defined before any system
 * header include.
 */
struct addrinfo;

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

/** @brief Portable file metadata returned by ::llam_stat_path_ex. */
typedef struct llam_file_stat {
    uint64_t size;       /**< File size in bytes when representable. */
    uint64_t mtime_ns;   /**< Last modification time as Unix epoch nanoseconds. */
    uint64_t atime_ns;   /**< Last access time as Unix epoch nanoseconds. */
    uint64_t ctime_ns;   /**< Metadata/change time as Unix epoch nanoseconds. */
    uint32_t mode;       /**< Platform mode/attribute bits. */
    uint32_t type;       /**< LLAM_FILE_TYPE_* value. */
    uint64_t reserved0;  /**< Reserved ABI padding; currently 0. */
} llam_file_stat_t;

/** @brief Current size to pass to ::llam_stat_path_ex. */
#define LLAM_FILE_STAT_CURRENT_SIZE ((size_t)sizeof(llam_file_stat_t))

/** @brief Regular file type. */
#define LLAM_FILE_TYPE_REGULAR UINT32_C(1)
/** @brief Directory file type. */
#define LLAM_FILE_TYPE_DIRECTORY UINT32_C(2)
/** @brief Symbolic link or reparse-point file type. */
#define LLAM_FILE_TYPE_SYMLINK UINT32_C(3)
/** @brief Other platform-specific file type. */
#define LLAM_FILE_TYPE_OTHER UINT32_C(4)

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
 * @brief Read/write/connect/poll with one absolute deadline and extra token.
 *
 * @details Managed operations use the earliest task/call deadline and observe task OR
 * group OR call cancellation. Cancellation never releases a buffer while a
 * backend/helper can still access it; a completed transfer can win the race.
 * Bounded read/write/connect need a managed task. POSIX bounded byte I/O accepts
 * sockets and nonblocking pipes/devices, rejecting regular/block files with
 * ENOTSUP rather than promising to interrupt a blocking filesystem call.
 * Windows HANDLE variants require native overlapped support when bounded.
 * Poll supports host threads as well. Its timeout result is 0, like poll_fd.
 * Existing read_when_ready and its relative millisecond timeout are unchanged.
 *
 * @param fd Descriptor or socket.
 * @param handle Platform handle.
 * @param buf Transfer buffer, in use until the call returns. May be NULL only when
 * @p count is 0.
 * @param count Maximum bytes to transfer.
 * @param addr Peer socket address. Must not be NULL.
 * @param addrlen Size of @p addr in bytes.
 * @param events Poll events, in the range of a @c short.
 * @param[out] revents Optional returned events, set to 0 first.
 * @param opts Optional wait policy; pass NULL for no additional limit.
 * @param opts_size Size of @p opts when non-NULL; must then be nonzero.
 * @return Bytes transferred for read and write, 0 for connect, and the ready count,
 * or 0 on timeout, for poll. Failure returns -1 with @c errno set.
 */
LLAM_API ssize_t llam_read_ex(llam_fd_t fd, void *buf, size_t count,
                              const llam_wait_opts_t *opts, size_t opts_size);
LLAM_API ssize_t llam_write_ex(llam_fd_t fd, const void *buf, size_t count,
                               const llam_wait_opts_t *opts, size_t opts_size);
LLAM_API int llam_connect_ex(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen,
                              const llam_wait_opts_t *opts, size_t opts_size);
LLAM_API int llam_poll_fd_ex(llam_fd_t fd, int events, short *revents,
                              const llam_wait_opts_t *opts, size_t opts_size);
LLAM_API ssize_t llam_read_handle_ex(llam_handle_t handle, void *buf, size_t count,
                                     const llam_wait_opts_t *opts, size_t opts_size);
LLAM_API ssize_t llam_write_handle_ex(llam_handle_t handle, const void *buf, size_t count,
                                      const llam_wait_opts_t *opts, size_t opts_size);

/**
 * @brief Absolute-deadline forms; deadline 0 is already expired.
 *
 * @param fd Descriptor or socket.
 * @param buf Transfer buffer, in use until the call returns. May be NULL only when
 * @p count is 0.
 * @param count Maximum bytes to transfer.
 * @param addr Peer socket address. Must not be NULL.
 * @param addrlen Size of @p addr in bytes.
 * @param events Poll events, in the range of a @c short.
 * @param deadline_ns Absolute ::llam_now_ns deadline.
 * @param[out] revents Optional returned events, set to 0 first.
 * @return Bytes transferred for read and write, 0 for connect, and the ready count,
 * or 0 on timeout, for poll. Failure returns -1 with @c errno set.
 */
LLAM_API ssize_t llam_read_until(llam_fd_t fd, void *buf, size_t count, uint64_t deadline_ns);
LLAM_API ssize_t llam_write_until(llam_fd_t fd, const void *buf, size_t count, uint64_t deadline_ns);
LLAM_API int llam_connect_until(llam_fd_t fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t deadline_ns);
LLAM_API int llam_poll_fd_until(llam_fd_t fd, int events, uint64_t deadline_ns, short *revents);

/**
 * @brief Transfer exactly count bytes, returning confirmed progress on failure.
 *
 * @details EOF before a complete read is @c EPIPE, a zero-byte write is @c EIO.
 * Completed chunks remain counted after timeout/cancel/error. These helpers
 * cannot roll back bytes already transferred; do not resend the confirmed prefix.
 *
 * @param fd Descriptor or socket.
 * @param buf Transfer buffer, in use until the call returns. May be NULL only when
 * @p count is 0.
 * @param count Bytes to transfer, at most @c SIZE_MAX / 2.
 * @param opts Optional wait policy; pass NULL for no additional limit.
 * @param opts_size Size of @p opts when non-NULL; must then be nonzero.
 * @param[out] progress Receives the confirmed byte count, starting at 0. Must not be
 * NULL.
 * @return 0 after @p count bytes, -1 with @c errno set otherwise.
 */
LLAM_API int llam_read_exact_ex(llam_fd_t fd, void *buf, size_t count,
                                const llam_wait_opts_t *opts, size_t opts_size, size_t *progress);
LLAM_API int llam_write_all_ex(llam_fd_t fd, const void *buf, size_t count,
                               const llam_wait_opts_t *opts, size_t opts_size, size_t *progress);
LLAM_API int llam_read_exact(llam_fd_t fd, void *buf, size_t count, size_t *progress);
LLAM_API int llam_write_all(llam_fd_t fd, const void *buf, size_t count, size_t *progress);

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
 * @details Windows submits overlapped @c ReadFile with the supplied offset.
 * Synchronous file handles use an independent reopened handle and must allow
 * read sharing; managed tasks park while a blocking helper performs that I/O.
 * The original file position is preserved, and end-of-file returns zero. On
 * POSIX, ::llam_handle_t aliases ::llam_fd_t and this delegates to
 * ::llam_pread.
 */
LLAM_API ssize_t llam_pread_handle(llam_handle_t handle, void *buf, size_t count, uint64_t offset);

/**
 * @brief Positional write to a generic platform handle.
 *
 * The original file position is preserved. Synchronous Windows file handles
 * must allow write sharing so an independent handle can perform the operation.
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
 *
 * @param fd Descriptor or socket.
 * @param max_count Maximum bytes to read.
 * @param[out] out Receives the buffer or NULL. Must not be NULL.
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
 *
 * @param fd Socket, stream or datagram.
 * @param max_count Maximum bytes to receive.
 * @param flags @c recv flags, such as @c MSG_PEEK.
 * @param[out] out Receives the buffer or NULL. Must not be NULL.
 */
LLAM_API ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out);

/**
 * @brief Receive a datagram and optionally return the peer address.
 *
 * @details Managed tasks first try a nonblocking @c recvfrom, then park on
 * read readiness. Calls outside a managed task delegate to the platform
 * @c recvfrom and may block. @p src_addr and @p addrlen must either both be
 * NULL or both be non-NULL.
 *
 * @param fd Datagram socket.
 * @param buf Destination buffer, in use until the call returns.
 * @param count Maximum bytes to receive.
 * @param flags @c recvfrom flags.
 * @param[out] src_addr Optional peer address.
 * @param[in,out] addrlen Capacity of @p src_addr on entry, peer address size on
 * return.
 * @return Bytes received, or -1 with @c errno set.
 */
LLAM_API ssize_t llam_recvfrom(llam_fd_t fd,
                               void *buf,
                               size_t count,
                               int flags,
                               struct sockaddr *src_addr,
                               socklen_t *addrlen);

/**
 * @brief Send a datagram to @p dst_addr.
 *
 * @details Managed tasks first try a nonblocking @c sendto, then park on
 * write readiness. Calls outside a managed task delegate to the platform
 * @c sendto and may block.
 */
LLAM_API ssize_t llam_sendto(llam_fd_t fd,
                             const void *buf,
                             size_t count,
                             int flags,
                             const struct sockaddr *dst_addr,
                             socklen_t addrlen);

/**
 * @brief Receive a datagram into a runtime-owned buffer.
 *
 * @details On success with a positive byte count, @p out receives a non-NULL
 * owned buffer that must be released. A zero-length datagram returns 0 and
 * stores NULL in @p out, matching the existing owned-buffer EOF/zero rule.
 *
 * @param fd Datagram socket.
 * @param max_count Maximum bytes to receive.
 * @param flags @c recvfrom flags.
 * @param[out] src_addr Optional peer address.
 * @param[in,out] addrlen Capacity of @p src_addr on entry, peer address size on
 * return.
 * @param[out] out Receives the buffer or NULL. Must not be NULL.
 * @return Bytes received, or -1 with @c errno set.
 */
LLAM_API ssize_t llam_recvfrom_owned(llam_fd_t fd,
                                     size_t max_count,
                                     int flags,
                                     struct sockaddr *src_addr,
                                     socklen_t *addrlen,
                                     llam_io_buffer_t **out);

/**
 * @brief Initialize I/O buffer allocation options with ABI-safe defaults.
 *
 * @details LLAM writes only the prefix known to the loaded library. Caller-side
 * tail bytes from newer struct definitions are left untouched.
 */
LLAM_API int llam_io_buffer_opts_init(llam_io_buffer_opts_t *opts, size_t opts_size);

/**
 * @brief Allocate a runtime-owned buffer with explicit capacity/alignment options.
 *
 * @param opts Optional buffer options.
 * @param opts_size Size of @p opts when non-NULL; must then be nonzero.
 * @param[out] out Receives the buffer; set to NULL on failure. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_io_buffer_alloc_ex(const llam_io_buffer_opts_t *opts, size_t opts_size, llam_io_buffer_t **out);

/**
 * @brief Allocate a runtime-owned buffer with default alignment.
 *
 * @param capacity Minimum usable capacity in bytes.
 * @param[out] out Receives the buffer; set to NULL on failure. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_io_buffer_alloc(size_t capacity, llam_io_buffer_t **out);

/**
 * @brief Allocate a runtime-owned buffer with a power-of-two alignment.
 *
 * @param capacity Minimum usable capacity in bytes.
 * @param alignment Required power-of-two alignment in bytes.
 * @param[out] out Receives the buffer; set to NULL on failure. Must not be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_io_buffer_alloc_aligned(size_t capacity, size_t alignment, llam_io_buffer_t **out);

/**
 * @brief Return the alignment requested when the owned buffer was allocated.
 *
 * @details Invalid, stale, or consumed handles return 0 with @c errno set to
 * @c EINVAL. Saturated or corrupt lifecycle accounting returns 0 with @c errno
 * set to @c EBUSY.
 */
LLAM_API size_t llam_io_buffer_alignment(const llam_io_buffer_t *buffer);

/**
 * @brief Positional read into a newly allocated aligned owned buffer.
 *
 * @details On success with a positive byte count, @p out receives a buffer that
 * must be released with ::llam_io_buffer_release. EOF returns 0 and stores
 * NULL. Failure returns -1, stores NULL in @p out, and sets @c errno.
 *
 * @param fd Descriptor to read.
 * @param max_count Maximum bytes to read.
 * @param offset File offset in bytes.
 * @param alignment Required power-of-two alignment in bytes.
 * @param[out] out Receives the buffer or NULL. Must not be NULL.
 */
LLAM_API ssize_t llam_pread_owned_aligned(llam_fd_t fd,
                                          size_t max_count,
                                          uint64_t offset,
                                          size_t alignment,
                                          llam_io_buffer_t **out);

/**
 * @brief HANDLE variant of ::llam_pread_owned_aligned.
 *
 * @param handle Handle to read.
 * @param max_count Maximum bytes to read.
 * @param offset File offset in bytes.
 * @param alignment Required power-of-two alignment in bytes.
 * @param[out] out Receives the buffer or NULL. Must not be NULL.
 */
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
 * after another thread releases the buffer. If lifecycle accounting is
 * saturated or corrupt, this returns @c NULL with @c errno set to @c EBUSY.
 * Invalid, stale, or consumed handles return @c NULL with @c errno set to
 * @c EINVAL.
 */
LLAM_API void *llam_io_buffer_data(llam_io_buffer_t *buffer);

/**
 * @brief Return the number of valid bytes in a runtime-owned I/O buffer.
 *
 * @details Invalid, stale, or consumed handles return 0 with @c errno set to
 * @c EINVAL. Saturated or corrupt lifecycle accounting returns 0 with @c errno
 * set to @c EBUSY.
 */
LLAM_API size_t llam_io_buffer_size(const llam_io_buffer_t *buffer);

/**
 * @brief Return total capacity of a runtime-owned I/O buffer.
 *
 * @details Invalid, stale, or consumed handles return 0 with @c errno set to
 * @c EINVAL. Saturated or corrupt lifecycle accounting returns 0 with @c errno
 * set to @c EBUSY.
 */
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
 * @param fd Listening socket.
 * @param[out] addr Optional peer address.
 * @param[in,out] addrlen Capacity of @p addr on entry, peer address size on return.
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
 *
 * @param fd Descriptor or socket.
 * @param events Poll events.
 * @param timeout_ms Relative timeout in milliseconds, as described above.
 * @param[out] revents Optional returned events.
 * @return Ready count, 0 on timeout, or -1 with @c errno set.
 */
LLAM_API int llam_poll_fd(llam_fd_t fd, short events, int timeout_ms, short *revents);

/**
 * @brief Wait for a generic platform handle.
 * @details Windows HANDLE waits use the blocking-helper pool in managed tasks;
 * this is not socket readiness and does not replace ::llam_poll_fd. POSIX
 * delegates to ::llam_poll_fd.
 *
 * @param handle Handle to wait on.
 * @param events Poll events.
 * @param timeout_ms Relative timeout in milliseconds, as for ::llam_poll_fd.
 * @param[out] revents Optional returned events.
 * @return Ready count, 0 on timeout, or -1 with @c errno set.
 */
LLAM_API int llam_poll_handle(llam_handle_t handle, short events, int timeout_ms, short *revents);

/**
 * @brief Resolve addresses through the runtime blocking path.
 *
 * @details LLAM submission/cancellation failures return -1 with @c errno set.
 * Resolver failures return 0, store the getaddrinfo @c EAI_* status in
 * @p gai_error, and store NULL in @p out. Successful results must be freed with
 * ::llam_freeaddrinfo_result.
 *
 * @param node Host name or address; may be NULL when @p service is set.
 * @param service Service name or port; may be NULL when @p node is set.
 * @param hints Optional lookup hints.
 * @param[out] out Receives the result list, set to NULL first. Must not be NULL.
 * @param[out] gai_error Receives the @c EAI_* status, set to 0 first. Must not be
 * NULL.
 * @return 0 when the lookup ran, -1 with @c errno set otherwise.
 */
LLAM_API int llam_getaddrinfo_result(const char *node,
                                     const char *service,
                                     const struct addrinfo *hints,
                                     struct addrinfo **out,
                                     int *gai_error);

/** @brief Free a result returned by ::llam_getaddrinfo_result. */
LLAM_API void llam_freeaddrinfo_result(struct addrinfo *result);

/**
 * @brief Open a filesystem path through the runtime blocking path.
 *
 * @details POSIX callers pass normal @c open flags and mode bits. Windows maps
 * the common @c O_RDONLY, @c O_WRONLY, @c O_RDWR, @c O_CREAT, @c O_EXCL, and
 * @c O_TRUNC flags to @c CreateFileA. On success, @p out receives a
 * ::llam_handle_t that must be closed with ::llam_close_handle.
 *
 * @param path Path to open. Must not be NULL.
 * @param flags Open flags.
 * @param mode Mode bits for a created file.
 * @param[out] out Receives the handle, set to ::LLAM_INVALID_HANDLE first. Must not
 * be NULL.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_open_async(const char *path, int flags, uint32_t mode, llam_handle_t *out);

/**
 * @brief Stat a filesystem path through the runtime blocking path.
 *
 * @param path Path to query. Must not be NULL.
 * @param[out] out Receives the metadata; its prefix is cleared first. Must not be
 * NULL.
 * @param out_size Size of the caller's ::llam_file_stat_t; must be nonzero.
 * @return 0 on success, -1 with @c errno set.
 */
LLAM_API int llam_stat_path_ex(const char *path, llam_file_stat_t *out, size_t out_size);


#endif /* LLAM_IO_DECLS_INCLUDED */
#endif /* LLAM_RUNTIME_H */
