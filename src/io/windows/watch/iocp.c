/**
 * @file src/io/windows/watch/iocp.c
 * @brief Low-level IOCP primitive wrappers for Windows 10/11.
 *
 * @details
 * The full Windows backend will bind sockets/files to this handle and use the
 * same completion shape for I/O completions and control packets. Non-Windows
 * builds provide ENOTSUP stubs so policy tests can run everywhere.
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

#include "runtime_windows_iocp.h"

#include "llam/platform.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#if LLAM_PLATFORM_WINDOWS
#include <windows.h>

static int llam_windows_error_to_errno(DWORD error_code) {
    switch (error_code) {
    case ERROR_SUCCESS:
        return 0;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE:
        return EINVAL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case WAIT_TIMEOUT:
        return ETIMEDOUT;
    case ERROR_OPERATION_ABORTED:
        return ECANCELED;
    default:
        return EIO;
    }
}
#endif

/** @brief Create a native IOCP handle using the selected policy concurrency. */
int llam_windows_iocp_create(const llam_windows_iocp_policy_t *policy, void **handle_out) {
#if LLAM_PLATFORM_WINDOWS
    DWORD concurrency;
    HANDLE handle;

    if (handle_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    *handle_out = NULL;
    concurrency = policy != NULL && policy->iocp_concurrency != 0U ? policy->iocp_concurrency : 0U;
    handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, concurrency);
    if (handle == NULL) {
        errno = llam_windows_error_to_errno(GetLastError());
        return -1;
    }
    *handle_out = handle;
    return 0;
#else
    (void)policy;
    if (handle_out != NULL) {
        *handle_out = NULL;
    }
    errno = ENOTSUP;
    return -1;
#endif
}

/** @brief Close a native IOCP handle. */
void llam_windows_iocp_close(void *handle) {
#if LLAM_PLATFORM_WINDOWS
    if (handle != NULL) {
        (void)CloseHandle((HANDLE)handle);
    }
#else
    (void)handle;
#endif
}

/** @brief Post a synthetic completion/control packet to an IOCP. */
int llam_windows_iocp_post(void *handle, uintptr_t key, uintptr_t overlapped, uint32_t bytes) {
#if LLAM_PLATFORM_WINDOWS
    if (handle == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!PostQueuedCompletionStatus((HANDLE)handle,
                                    (DWORD)bytes,
                                    (ULONG_PTR)key,
                                    (LPOVERLAPPED)overlapped)) {
        errno = llam_windows_error_to_errno(GetLastError());
        return -1;
    }
    return 0;
#else
    (void)handle;
    (void)key;
    (void)overlapped;
    (void)bytes;
    errno = ENOTSUP;
    return -1;
#endif
}

/** @brief Drain up to @p entry_count completions using GetQueuedCompletionStatusEx. */
int llam_windows_iocp_drain(void *handle,
                            llam_windows_iocp_completion_t *entries,
                            size_t entry_count,
                            uint32_t timeout_ms,
                            size_t *count_out) {
#if LLAM_PLATFORM_WINDOWS
    OVERLAPPED_ENTRY stack_entries[128];
    OVERLAPPED_ENTRY *native_entries = stack_entries;
    ULONG requested;
    ULONG removed = 0;
    size_t i;

    if (count_out != NULL) {
        *count_out = 0U;
    }
    if (handle == NULL || entries == NULL || entry_count == 0U || count_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    requested = entry_count > 128U ? 128U : (ULONG)entry_count;
    memset(native_entries, 0, requested * sizeof(*native_entries));
    if (!GetQueuedCompletionStatusEx((HANDLE)handle, native_entries, requested, &removed, timeout_ms, FALSE)) {
        DWORD error_code = GetLastError();

        if (error_code == WAIT_TIMEOUT) {
            *count_out = 0U;
            return 0;
        }
        errno = llam_windows_error_to_errno(error_code);
        return -1;
    }
    for (i = 0U; i < (size_t)removed; ++i) {
        entries[i].key = (uintptr_t)native_entries[i].lpCompletionKey;
        entries[i].overlapped = (uintptr_t)native_entries[i].lpOverlapped;
        entries[i].bytes = (uint32_t)native_entries[i].dwNumberOfBytesTransferred;
        entries[i].error_code = 0U;
    }
    *count_out = (size_t)removed;
    return 0;
#else
    (void)handle;
    (void)entries;
    (void)entry_count;
    (void)timeout_ms;
    if (count_out != NULL) {
        *count_out = 0U;
    }
    errno = ENOTSUP;
    return -1;
#endif
}
