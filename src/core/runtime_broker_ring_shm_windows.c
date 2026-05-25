/**
 * @file src/core/runtime_broker_ring_shm_windows.c
 * @brief Windows broker ring file-mapping backend.
 *
 * @details
 * Windows ring authority is represented by owner-scoped file mappings or by
 * unnamed mapping HANDLEs duplicated over a broker control session. This file
 * keeps DACL construction and HANDLE lifetime separate from the common ring
 * helpers.
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

#include "runtime_internal.h"
#include "runtime_broker_ring.h"
#include "runtime_broker_windows_security.h"

#if LLAM_PLATFORM_WINDOWS

static int llam_broker_ring_windows_errno(DWORD error_code) {
    if (error_code == ERROR_ALREADY_EXISTS) {
        return EEXIST;
    }
    if (error_code == ERROR_FILE_NOT_FOUND) {
        return ENOENT;
    }
    return llam_windows_system_error_to_errno(error_code);
}

static int llam_broker_ring_map_windows_handle(const char *name,
                                               HANDLE handle,
                                               bool owner,
                                               llam_broker_ring_mapping_t *out_mapping) {
    void *addr;
    size_t bytes = sizeof(llam_broker_ring_t);

    addr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0U, 0U, bytes);
    if (addr == NULL) {
        errno = llam_broker_ring_windows_errno(GetLastError());
        return -1;
    }
    llam_broker_ring_mapping_reset(out_mapping);
    out_mapping->ring = (llam_broker_ring_t *)addr;
    out_mapping->bytes = bytes;
    out_mapping->fd = -1;
    out_mapping->mapping_handle = (llam_handle_t)handle;
    out_mapping->owner = owner;
    (void)snprintf(out_mapping->name, sizeof(out_mapping->name), "%s", name);
    return 0;
}

int llam_broker_ring_map_handle(llam_handle_t handle,
                                bool take_ownership,
                                llam_broker_ring_mapping_t *out_mapping) {
    HANDLE mapped_handle = (HANDLE)handle;

    if (LLAM_UNLIKELY(LLAM_HANDLE_IS_INVALID(handle) || out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (!take_ownership) {
        if (!DuplicateHandle(GetCurrentProcess(),
                             (HANDLE)handle,
                             GetCurrentProcess(),
                             &mapped_handle,
                             FILE_MAP_ALL_ACCESS,
                             FALSE,
                             0U)) {
            errno = llam_broker_ring_windows_errno(GetLastError());
            return -1;
        }
    }
    if (llam_broker_ring_map_windows_handle("", mapped_handle, false, out_mapping) != 0) {
        int saved_errno = errno;

        if (!take_ownership || mapped_handle != (HANDLE)handle) {
            CloseHandle(mapped_handle);
        }
        errno = saved_errno;
        return -1;
    }
    if (!llam_broker_ring_mapping_ring_valid(out_mapping->ring)) {
        llam_broker_ring_unmap(out_mapping);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int llam_broker_ring_create_shm(const char *name, llam_broker_ring_mapping_t *out_mapping) {
    HANDLE handle;
    DWORD size_high;
    DWORD size_low;
    size_t bytes = sizeof(llam_broker_ring_t);
    llam_broker_windows_security_t security;

    if (LLAM_UNLIKELY(!llam_broker_ring_name_valid(name) || out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }

    size_high = (DWORD)(((uint64_t)bytes >> 32U) & UINT64_C(0xffffffff));
    size_low = (DWORD)((uint64_t)bytes & UINT64_C(0xffffffff));
    if (llam_broker_windows_security_init(&security, FILE_MAP_ALL_ACCESS) != 0) {
        return -1;
    }
    handle = CreateFileMappingA(INVALID_HANDLE_VALUE, &security.attrs, PAGE_READWRITE, size_high, size_low, name);
    llam_broker_windows_security_cleanup(&security);
    if (handle == NULL) {
        errno = llam_broker_ring_windows_errno(GetLastError());
        return -1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        errno = EEXIST;
        return -1;
    }
    if (llam_broker_ring_map_windows_handle(name, handle, true, out_mapping) != 0) {
        int saved_errno = errno;

        CloseHandle(handle);
        errno = saved_errno;
        return -1;
    }
    if (llam_broker_ring_init(out_mapping->ring) != 0) {
        int saved_errno = errno;

        llam_broker_ring_unmap(out_mapping);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int llam_broker_ring_create_private_handle(llam_broker_ring_mapping_t *out_mapping) {
    HANDLE handle;
    DWORD size_high;
    DWORD size_low;
    size_t bytes = sizeof(llam_broker_ring_t);
    llam_broker_windows_security_t security;

    if (LLAM_UNLIKELY(out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }
    /*
     * An unnamed file mapping has no reusable object-manager name to guess or
     * open. The HANDLE itself is the rendezvous authority and should be passed
     * with DuplicateHandle over the authenticated broker control session.
     */
    size_high = (DWORD)(((uint64_t)bytes >> 32U) & UINT64_C(0xffffffff));
    size_low = (DWORD)((uint64_t)bytes & UINT64_C(0xffffffff));
    if (llam_broker_windows_security_init(&security, FILE_MAP_ALL_ACCESS) != 0) {
        return -1;
    }
    handle = CreateFileMappingA(INVALID_HANDLE_VALUE,
                                &security.attrs,
                                PAGE_READWRITE,
                                size_high,
                                size_low,
                                NULL);
    llam_broker_windows_security_cleanup(&security);
    if (handle == NULL) {
        errno = llam_broker_ring_windows_errno(GetLastError());
        return -1;
    }
    if (llam_broker_ring_map_windows_handle("", handle, true, out_mapping) != 0) {
        int saved_errno = errno;

        CloseHandle(handle);
        errno = saved_errno;
        return -1;
    }
    if (llam_broker_ring_init(out_mapping->ring) != 0) {
        int saved_errno = errno;

        llam_broker_ring_unmap(out_mapping);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

int llam_broker_ring_open_shm(const char *name, llam_broker_ring_mapping_t *out_mapping) {
    HANDLE handle;

    if (LLAM_UNLIKELY(!llam_broker_ring_name_valid(name) || out_mapping == NULL)) {
        errno = EINVAL;
        return -1;
    }

    handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (handle == NULL) {
        errno = llam_broker_ring_windows_errno(GetLastError());
        return -1;
    }
    if (llam_broker_ring_map_windows_handle(name, handle, false, out_mapping) != 0) {
        int saved_errno = errno;

        CloseHandle(handle);
        errno = saved_errno;
        return -1;
    }
    if (!llam_broker_ring_mapping_ring_valid(out_mapping->ring)) {
        llam_broker_ring_unmap(out_mapping);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void llam_broker_ring_unmap(llam_broker_ring_mapping_t *mapping) {
    if (mapping == NULL) {
        return;
    }
    if (mapping->ring != NULL) {
        (void)UnmapViewOfFile(mapping->ring);
    }
    if (!LLAM_HANDLE_IS_INVALID(mapping->mapping_handle)) {
        (void)CloseHandle((HANDLE)mapping->mapping_handle);
    }
    llam_broker_ring_mapping_reset(mapping);
}

#endif
