/**
 * @file src/io/runtime_io_api_positional.c
 * @brief Public positional I/O entry points and vector positional fallbacks.
 *
 * @details
 * This file owns pread/pwrite style APIs so the direct read/write/poll public
 * entry points stay small. Positional operations preserve the caller's current
 * file offset and route managed tasks through the same backend/blocking-helper
 * policy as the rest of the public I/O API.
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

static ssize_t llam_positional_fd_rw(llam_fd_t fd, void *buf, size_t count, uint64_t offset, bool write_op) {
#if LLAM_PLATFORM_POSIX
    llam_io_req_t *req;
    ssize_t result;

    if (buf == NULL && count != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (llam_positional_validate_async_rw_count(count) != 0) {
        return -1;
    }
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        return write_op ? llam_platform_pwrite_fd(fd, buf, count, offset) :
                          llam_platform_pread_fd(fd, buf, count, offset);
    }

    req = llam_api_io_req_acquire(g_llam_tls_shard);
    if (req == NULL) {
        errno = ENOMEM;
        return -1;
    }
    req->kind = write_op ? LLAM_IO_KIND_PWRITE : LLAM_IO_KIND_PREAD;
    req->fd = fd;
    req->buf = buf;
    req->count = count;
    req->offset = offset;
    if (llam_issue_io(req, false, 0U) != 0) {
        if (!llam_io_capability_error(errno)) {
            llam_api_io_req_release(g_llam_tls_shard, req);
            return -1;
        }
        req->kind = write_op ? LLAM_IO_KIND_PWRITE : LLAM_IO_KIND_PREAD;
        req->fd = fd;
        req->buf = buf;
        req->count = count;
        req->offset = offset;
        req->task = g_llam_tls_task;
        if (llam_positional_call_blocking_io(write_op ? llam_blocking_pwrite_impl : llam_blocking_pread_impl, req) != 0) {
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
#else
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    (void)write_op;
    errno = ENOTSUP;
    return -1;
#endif
}

ssize_t llam_pread(llam_fd_t fd, void *buf, size_t count, uint64_t offset) {
    return llam_positional_fd_rw(fd, buf, count, offset, false);
}

ssize_t llam_pwrite(llam_fd_t fd, const void *buf, size_t count, uint64_t offset) {
    return llam_positional_fd_rw(fd, (void *)buf, count, offset, true);
}

ssize_t llam_preadv(llam_fd_t fd, const llam_mut_iovec_t *iov, int iovcnt, uint64_t offset) {
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
#if LLAM_PLATFORM_POSIX
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        struct iovec stack_iov[16];
        struct iovec *native_iov = stack_iov;
        off_t native_offset;
        ssize_t result;

        if (llam_offset_to_off_t(offset, &native_offset) != 0) {
            return -1;
        }
        if (iovcnt > (int)(sizeof(stack_iov) / sizeof(stack_iov[0]))) {
            native_iov = malloc((size_t)iovcnt * sizeof(*native_iov));
            if (native_iov == NULL) {
                errno = ENOMEM;
                return -1;
            }
        }
        for (int i = 0; i < iovcnt; ++i) {
            native_iov[i].iov_base = iov[i].iov_base;
            native_iov[i].iov_len = iov[i].iov_len;
        }
        result = preadv(fd, native_iov, iovcnt, native_offset);
        if (native_iov != stack_iov) {
            free(native_iov);
        }
        return result;
    }
#endif
    for (int i = 0; i < iovcnt; ++i) {
        char *bytes = (char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;

        while (remaining > 0U) {
            size_t before = remaining;
            ssize_t nread = llam_pread(fd, bytes, remaining, offset);

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
}

ssize_t llam_pwritev(llam_fd_t fd, const llam_iovec_t *iov, int iovcnt, uint64_t offset) {
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
#if LLAM_PLATFORM_POSIX
    if (g_llam_tls_shard == NULL || g_llam_tls_task == NULL) {
        struct iovec stack_iov[16];
        struct iovec *native_iov = stack_iov;
        off_t native_offset;
        ssize_t result;

        if (llam_offset_to_off_t(offset, &native_offset) != 0) {
            return -1;
        }
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
        result = pwritev(fd, native_iov, iovcnt, native_offset);
        if (native_iov != stack_iov) {
            free(native_iov);
        }
        return result;
    }
#endif
    for (int i = 0; i < iovcnt; ++i) {
        const char *bytes = (const char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;

        while (remaining > 0U) {
            ssize_t written = llam_pwrite(fd, bytes, remaining, offset);

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
}

