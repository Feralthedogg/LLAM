/**
 * @file src/io/api/handle_positional.c
 * @brief Generic platform-handle write and positional I/O entry points.
 *
 * @details
 * POSIX public handles are file descriptors and delegate directly to the fd
 * APIs. Windows handles need separate overlapped request kinds, so this file
 * owns the handle-specific managed-task and blocking-fallback path.
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
        if (llam_positional_call_blocking_io(llam_blocking_handle_write_impl, req) != 0) {
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

static ssize_t llam_positional_handle_rw(llam_handle_t handle, void *buf, size_t count, uint64_t offset, bool write_op) {
#if LLAM_PLATFORM_POSIX
    return write_op ? llam_pwrite((llam_fd_t)handle, buf, count, offset) :
                      llam_pread((llam_fd_t)handle, buf, count, offset);
#else
    llam_io_req_t *req;
    ssize_t result;

    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (count > (size_t)ULONG_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return write_op ? llam_platform_pwrite_handle(handle, buf, count, offset) :
                          llam_platform_pread_handle(handle, buf, count, offset);
    }
    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }
    req->kind = write_op ? LLAM_IO_KIND_HANDLE_PWRITE : LLAM_IO_KIND_HANDLE_PREAD;
    req->handle = handle;
    req->buf = buf;
    req->count = count;
    req->offset = offset;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = write_op ? LLAM_IO_KIND_HANDLE_PWRITE : LLAM_IO_KIND_HANDLE_PREAD;
        req->handle = handle;
        req->buf = buf;
        req->count = count;
        req->offset = offset;
        req->task = g_llam_tls_task;
        if (llam_positional_call_blocking_io(write_op ? llam_blocking_handle_pwrite_impl : llam_blocking_handle_pread_impl, req) != 0) {
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

ssize_t llam_pread_handle(llam_handle_t handle, void *buf, size_t count, uint64_t offset) {
    return llam_positional_handle_rw(handle, buf, count, offset, false);
}

ssize_t llam_pwrite_handle(llam_handle_t handle, const void *buf, size_t count, uint64_t offset) {
    return llam_positional_handle_rw(handle, (void *)buf, count, offset, true);
}

ssize_t llam_preadv_handle(llam_handle_t handle, const llam_mut_iovec_t *iov, int iovcnt, uint64_t offset) {
#if LLAM_PLATFORM_POSIX
    return llam_preadv((llam_fd_t)handle, iov, iovcnt, offset);
#else
    ssize_t total = 0;

    if (iovcnt < 0 || iovcnt > llam_positional_iovcnt_max() || (iovcnt > 0 && iov == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (iovcnt == 0) {
        return 0;
    }
    if (llam_positional_mut_iov_validate(iov, iovcnt) != 0) {
        return -1;
    }
    for (int i = 0; i < iovcnt; ++i) {
        char *bytes = (char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;

        while (remaining > 0U) {
            size_t before = remaining;
            ssize_t nread = llam_pread_handle(handle, bytes, remaining, offset);

            if (nread > 0) {
                total += nread;
                bytes += nread;
                remaining -= (size_t)nread;
                if (llam_positional_offset_advance(&offset, nread) != 0) {
                    return total > 0 ? total : -1;
                }
                if ((size_t)nread < before) {
                    return total;
                }
                continue;
            }
            if (nread == 0) {
                return total;
            }
            if (errno == EINTR) {
                continue;
            }
            return total > 0 ? total : -1;
        }
    }
    return total;
#endif
}

ssize_t llam_pwritev_handle(llam_handle_t handle, const llam_iovec_t *iov, int iovcnt, uint64_t offset) {
#if LLAM_PLATFORM_POSIX
    return llam_pwritev((llam_fd_t)handle, iov, iovcnt, offset);
#else
    ssize_t total = 0;

    if (iovcnt < 0 || iovcnt > llam_positional_iovcnt_max() || (iovcnt > 0 && iov == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (iovcnt == 0) {
        return 0;
    }
    if (llam_positional_const_iov_validate(iov, iovcnt) != 0) {
        return -1;
    }
    for (int i = 0; i < iovcnt; ++i) {
        const char *bytes = (const char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;

        while (remaining > 0U) {
            ssize_t written = llam_pwrite_handle(handle, bytes, remaining, offset);

            if (written > 0) {
                total += written;
                bytes += written;
                remaining -= (size_t)written;
                if (llam_positional_offset_advance(&offset, written) != 0) {
                    return total > 0 ? total : -1;
                }
                continue;
            }
            if (written < 0 && errno == EINTR) {
                continue;
            }
            return total > 0 ? total : -1;
        }
    }
    return total;
#endif
}
