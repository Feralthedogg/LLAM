/**
 * @file src/core/broker/broker_descriptor.c
 * @brief Broker-owned descriptor and HANDLE capability data plane.
 *
 * @details
 * Descriptor authority is broker-owned. Clients may request reads and writes
 * with MAC-protected tokens, but every operation first validates the live slot
 * and duplicates the underlying descriptor/HANDLE while the broker table is
 * locked. The duplicate is then used outside the lock so close/reuse races
 * cannot retarget I/O at a different broker slot.
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
#include "runtime_broker.h"

#include <limits.h>
#include <string.h>
#if !LLAM_PLATFORM_WINDOWS
#include <fcntl.h>
#include <unistd.h>
#endif

static int llam_broker_descriptor_set_cloexec(llam_handle_t handle) {
#if LLAM_PLATFORM_WINDOWS
    if (LLAM_UNLIKELY(LLAM_HANDLE_IS_INVALID(handle))) {
        errno = EINVAL;
        return -1;
    }
    if (!SetHandleInformation((HANDLE)handle, HANDLE_FLAG_INHERIT, 0U)) {
        errno = llam_windows_system_error_to_errno(GetLastError());
        return -1;
    }
    return 0;
#else
    int fd = (int)handle;
    int flags;

    if (LLAM_UNLIKELY(fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return -1;
    }
    if ((flags & FD_CLOEXEC) != 0) {
        return 0;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#endif
}

static bool llam_broker_descriptor_handle_invalid(llam_handle_t handle) {
#if LLAM_PLATFORM_WINDOWS
    return LLAM_HANDLE_IS_INVALID(handle);
#else
    return handle < 0;
#endif
}

static void llam_broker_descriptor_close_owned_slot(llam_broker_descriptor_slot_t *slot) {
    if (slot == NULL || !slot->close_on_destroy) {
        return;
    }
#if LLAM_PLATFORM_WINDOWS
    if (!LLAM_HANDLE_IS_INVALID(slot->handle)) {
        (void)CloseHandle((HANDLE)slot->handle);
    }
#else
    if (slot->fd >= 0) {
        (void)close(slot->fd);
    }
#endif
}

static void llam_broker_descriptor_reset_slot(llam_broker_descriptor_slot_t *slot) {
    if (slot == NULL) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
#if LLAM_PLATFORM_WINDOWS
    slot->handle = LLAM_INVALID_HANDLE;
#else
    slot->fd = -1;
#endif
}

void llam_broker_clear_descriptors(llam_broker_t *broker) {
    size_t i;

    if (broker == NULL) {
        return;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

        /*
         * Ownership is represented by close_on_destroy plus a valid
         * descriptor/HANDLE. Destroy must still reclaim partially invalidated
         * slots whose active bit was cleared during an interrupted lifecycle.
         */
        llam_broker_descriptor_close_owned_slot(slot);
        llam_broker_descriptor_reset_slot(slot);
    }
}

static llam_broker_descriptor_slot_t *llam_broker_find_descriptor_unlocked(
    llam_broker_t *broker,
    const llam_capability_token_t *token,
    uint64_t required_rights) {
    size_t i;

    if (llam_broker_validate_token_family_unlocked(broker,
                                                   token,
                                                   LLAM_BROKER_CAP_FAMILY_DESCRIPTOR,
                                                   required_rights) != 0) {
        return NULL;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        llam_broker_descriptor_slot_t *slot = &broker->descriptors[i];

        if (slot->active &&
            slot->id == token->slot &&
            slot->generation == token->generation) {
            if (LLAM_UNLIKELY((slot->rights & required_rights) != required_rights)) {
                errno = EACCES;
                return NULL;
            }
            return slot;
        }
    }
    errno = EACCES;
    return NULL;
}

int llam_broker_register_fd(llam_broker_t *broker,
                            int fd,
                            uint64_t rights,
                            bool close_on_destroy,
                            llam_capability_token_t *out_token) {
#if LLAM_PLATFORM_WINDOWS
    (void)broker;
    (void)fd;
    (void)rights;
    (void)close_on_destroy;
    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    errno = ENOTSUP;
    return -1;
#else
    return llam_broker_register_handle(broker, (llam_handle_t)fd, rights, close_on_destroy, out_token);
#endif
}

int llam_broker_register_handle(llam_broker_t *broker,
                                llam_handle_t handle,
                                uint64_t rights,
                                bool close_on_destroy,
                                llam_capability_token_t *out_token) {
    llam_broker_descriptor_slot_t *slot = NULL;
    size_t i;

    if (out_token != NULL) {
        memset(out_token, 0, sizeof(*out_token));
    }
    if (LLAM_UNLIKELY(broker == NULL ||
                      llam_broker_descriptor_handle_invalid(handle) ||
                      rights == 0U ||
                      out_token == NULL)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_validate_object_rights(LLAM_BROKER_CAP_FAMILY_DESCRIPTOR, rights) != 0) {
        return -1;
    }
    if (llam_broker_descriptor_set_cloexec(handle) != 0) {
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (LLAM_UNLIKELY(!broker->initialized || broker->runtime == NULL)) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < LLAM_BROKER_DESCRIPTOR_SLOTS; ++i) {
        if (!broker->descriptors[i].active) {
            slot = &broker->descriptors[i];
            break;
        }
    }
    if (slot == NULL) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        errno = ENOSPC;
        return -1;
    }
    if (llam_broker_validate_next_object_id(broker->next_descriptor_id) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    /*
     * Free-list selection is based on active=false. If a previous internal
     * lifecycle was interrupted after clearing active, the slot can still own
     * descriptor authority. Reclaim it before overwriting the fd/HANDLE field.
     */
    llam_broker_descriptor_close_owned_slot(slot);
    llam_broker_descriptor_reset_slot(slot);

#if LLAM_PLATFORM_WINDOWS
    slot->handle = handle;
#else
    slot->fd = handle;
#endif
    slot->id = broker->next_descriptor_id++;
    slot->generation = 1U;
    slot->rights = rights;
    slot->close_on_destroy = close_on_destroy;
    slot->active = true;
    if (llam_broker_issue_object_cap_unlocked(broker,
                                              LLAM_BROKER_CAP_FAMILY_DESCRIPTOR,
                                              slot->id,
                                              slot->generation,
                                              rights,
                                              out_token) != 0) {
        memset(slot, 0, sizeof(*slot));
#if LLAM_PLATFORM_WINDOWS
        slot->handle = LLAM_INVALID_HANDLE;
#else
        slot->fd = -1;
#endif
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_unlock(broker);
    llam_broker_end_op(broker);
    return 0;
}

static int llam_broker_duplicate_descriptor_unlocked(llam_broker_t *broker,
                                                     const llam_capability_token_t *token,
                                                     uint64_t required_rights,
                                                     llam_handle_t *out_handle) {
    llam_broker_descriptor_slot_t *slot;

    if (LLAM_UNLIKELY(out_handle == NULL)) {
        errno = EINVAL;
        return -1;
    }
    *out_handle = LLAM_INVALID_HANDLE;
    slot = llam_broker_find_descriptor_unlocked(broker, token, required_rights);
    if (slot == NULL) {
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    {
        HANDLE duplicate = NULL;

        if (!DuplicateHandle(GetCurrentProcess(),
                             (HANDLE)slot->handle,
                             GetCurrentProcess(),
                             &duplicate,
                             0U,
                             FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            errno = llam_windows_system_error_to_errno(GetLastError());
            return -1;
        }
        *out_handle = (llam_handle_t)duplicate;
    }
#else
    {
        int duplicate = llam_broker_dup_cloexec_fd(slot->fd);

        if (duplicate < 0) {
            return -1;
        }
        *out_handle = (llam_handle_t)duplicate;
    }
#endif
    return 0;
}

ssize_t llam_broker_read_fd(llam_broker_t *broker,
                            const llam_capability_token_t *token,
                            void *out_data,
                            size_t length) {
    if (LLAM_UNLIKELY(out_data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    (void)broker;
    (void)token;
    (void)out_data;
    (void)length;
    errno = ENOTSUP;
    return llam_broker_fail_clear_output_ssize(out_data, length, errno);
#else
    return llam_broker_read_handle(broker, token, out_data, length);
#endif
}

ssize_t llam_broker_write_fd(llam_broker_t *broker,
                             const llam_capability_token_t *token,
                             const void *data,
                             size_t length) {
    if (LLAM_UNLIKELY(data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
#if LLAM_PLATFORM_WINDOWS
    (void)broker;
    (void)token;
    (void)data;
    (void)length;
    errno = ENOTSUP;
    return -1;
#else
    return llam_broker_write_handle(broker, token, data, length);
#endif
}

ssize_t llam_broker_read_handle(llam_broker_t *broker,
                                const llam_capability_token_t *token,
                                void *out_data,
                                size_t length) {
    llam_handle_t handle;
    ssize_t result;

    if (LLAM_UNLIKELY(out_data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return llam_broker_fail_clear_output_ssize(out_data, length, errno);
    }
    if (llam_broker_lock(broker) != 0) {
        int saved_errno = errno;

        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
    }
    if (llam_broker_duplicate_descriptor_unlocked(broker, token, LLAM_CAP_RIGHT_READ, &handle) != 0) {
        int saved_errno = errno;

        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
    }
    llam_broker_unlock(broker);
#if LLAM_PLATFORM_WINDOWS
    {
        DWORD transferred = 0U;

        if (LLAM_UNLIKELY(length > (size_t)ULONG_MAX)) {
            (void)CloseHandle((HANDLE)handle);
            llam_broker_end_op(broker);
            return llam_broker_fail_clear_output_ssize(out_data, length, EINVAL);
        }
        if (!ReadFile((HANDLE)handle, out_data, (DWORD)length, &transferred, NULL)) {
            int saved_errno = llam_windows_system_error_to_errno(GetLastError());

            (void)CloseHandle((HANDLE)handle);
            llam_broker_end_op(broker);
            return llam_broker_fail_clear_output_ssize(out_data, length, saved_errno);
        }
        (void)CloseHandle((HANDLE)handle);
        llam_broker_end_op(broker);
        return llam_broker_finish_read_clear_tail(out_data, length, (ssize_t)transferred);
    }
#else
    result = read((int)handle, out_data, length);
    {
        int saved_errno = errno;

        (void)close((int)handle);
        errno = saved_errno;
    }
    llam_broker_end_op(broker);
    return llam_broker_finish_read_clear_tail(out_data, length, result);
#endif
}

ssize_t llam_broker_write_handle(llam_broker_t *broker,
                                 const llam_capability_token_t *token,
                                 const void *data,
                                 size_t length) {
    llam_handle_t handle;
    ssize_t result;

    if (LLAM_UNLIKELY(data == NULL || length == 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (llam_broker_begin_op(broker) != 0) {
        return -1;
    }
    if (llam_broker_lock(broker) != 0) {
        llam_broker_end_op(broker);
        return -1;
    }
    if (llam_broker_duplicate_descriptor_unlocked(broker, token, LLAM_CAP_RIGHT_WRITE, &handle) != 0) {
        llam_broker_unlock(broker);
        llam_broker_end_op(broker);
        return -1;
    }
    llam_broker_unlock(broker);
#if LLAM_PLATFORM_WINDOWS
    {
        DWORD transferred = 0U;

        if (LLAM_UNLIKELY(length > (size_t)ULONG_MAX)) {
            (void)CloseHandle((HANDLE)handle);
            llam_broker_end_op(broker);
            errno = EINVAL;
            return -1;
        }
        if (!WriteFile((HANDLE)handle, data, (DWORD)length, &transferred, NULL)) {
            int saved_errno = llam_windows_system_error_to_errno(GetLastError());

            (void)CloseHandle((HANDLE)handle);
            llam_broker_end_op(broker);
            errno = saved_errno;
            return -1;
        }
        (void)CloseHandle((HANDLE)handle);
        llam_broker_end_op(broker);
        return (ssize_t)transferred;
    }
#else
    result = write((int)handle, data, length);
    {
        int saved_errno = errno;

        (void)close((int)handle);
        errno = saved_errno;
    }
    llam_broker_end_op(broker);
    return result;
#endif
}
