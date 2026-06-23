/**
 * @file src/io/api/positional_util.c
 * @brief Shared validation helpers for positional vector I/O entry points.
 *
 * @details
 * The fd and generic-handle positional APIs both need identical iovec and
 * offset overflow checks. Keeping those checks here prevents the two public API
 * files from drifting when platform byte-count limits or errno contracts change.
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

#include <limits.h>

#if LLAM_PLATFORM_POSIX
#include <sys/uio.h>
#endif

#ifndef IOV_MAX
#define LLAM_POSITIONAL_IOV_MAX_FALLBACK 1024
#endif

int llam_positional_iovcnt_max(void) {
#ifdef IOV_MAX
    return IOV_MAX;
#else
    return LLAM_POSITIONAL_IOV_MAX_FALLBACK;
#endif
}

static size_t llam_positional_byte_count_max(void) {
#if LLAM_PLATFORM_DARWIN
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

static size_t llam_positional_async_count_max(void) {
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

int llam_positional_validate_async_rw_count(size_t count) {
    if (count > llam_positional_async_count_max()) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int llam_positional_const_iov_validate(const llam_iovec_t *iov, int iovcnt) {
    size_t total = 0U;
    size_t max_total = llam_positional_byte_count_max();

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

int llam_positional_mut_iov_validate(const llam_mut_iovec_t *iov, int iovcnt) {
    size_t total = 0U;
    size_t max_total = llam_positional_byte_count_max();

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

int llam_positional_offset_advance(uint64_t *offset, ssize_t amount) {
    if (amount < 0 || offset == NULL || (uint64_t)amount > UINT64_MAX - *offset) {
        errno = EOVERFLOW;
        return -1;
    }
    *offset += (uint64_t)amount;
    return 0;
}

int llam_positional_call_blocking_io(llam_blocking_fn fn, llam_io_req_t *req) {
    void *ignored = NULL;

    return llam_call_blocking_result(fn, req, &ignored);
}
