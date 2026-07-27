/**
 * @file src/io/linux/watch/linux_segment_resources.c
 * @brief Sparse fixed-file and fixed-buffer leases for native segments.
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

#include "io/linux/runtime_io_watch_linux_internal.h"

static int llam_linux_native_files_update(
    llam_node_t *node,
    unsigned offset,
    const int *files,
    unsigned count) {
    if (node->native_files_update_override != NULL) {
        return node->native_files_update_override(
            node,
            offset,
            files,
            count,
            node->native_resource_update_override_arg);
    }
    return io_uring_register_files_update(
        &node->ring, offset, files, count);
}

static int llam_linux_native_buffers_update(
    llam_node_t *node,
    unsigned offset,
    const struct iovec *buffers,
    unsigned count) {
    if (node->native_buffers_update_override != NULL) {
        return node->native_buffers_update_override(
            node,
            offset,
            buffers,
            count,
            node->native_resource_update_override_arg);
    }
    return io_uring_register_buffers_update_tag(
        &node->ring,
        offset,
        buffers,
        NULL,
        count);
}

static int llam_linux_native_update_error(int result) {
    if (result == 1) {
        return 0;
    }
    return result < 0 ? -result : EIO;
}

static bool llam_linux_native_reserve_slots(
    uint64_t bitmap,
    unsigned count,
    unsigned *slots,
    uint64_t *reserved_out) {
    uint64_t reserved = 0U;
    unsigned i;

    for (i = 0U; i < count; i += 1U) {
        uint64_t available = ~(bitmap | reserved);
        unsigned slot;

        if (available == 0U) {
            return false;
        }
        slot = (unsigned)__builtin_ctzll(available);
        slots[i] = slot;
        reserved |= UINT64_C(1) << slot;
    }
    *reserved_out = reserved;
    return true;
}

static void llam_linux_native_resource_cleanup_failed(
    llam_node_t *node) {
    node->supports_native_fixed_files = false;
    node->supports_native_fixed_buffers = false;
    if (node->runtime != NULL) {
        llam_record_fatal_deferred(node->runtime, EIO);
    }
}

int llam_linux_native_resources_setup(llam_node_t *node) {
    int files_result;
    int buffers_result;

    if (node == NULL ||
        !node->native_resource_lock_initialized) {
        errno = EINVAL;
        return -1;
    }
    node->supports_native_fixed_files = false;
    node->supports_native_fixed_buffers = false;
    node->native_fixed_files_registered = false;
    node->native_fixed_buffers_registered = false;
    node->native_fixed_file_bitmap = 0U;
    node->native_fixed_buffer_bitmap = 0U;
    if (!node->ring_ready) {
        return 0;
    }

    files_result = io_uring_register_files_sparse(
        &node->ring,
        LLAM_LINUX_NATIVE_FIXED_FILE_SLOTS);
    if (files_result < 0) {
        return 0;
    }
    node->native_fixed_files_registered = true;
    buffers_result = io_uring_register_buffers_sparse(
        &node->ring,
        LLAM_LINUX_NATIVE_FIXED_BUFFER_SLOTS);
    if (buffers_result < 0) {
        int unregister_result =
            io_uring_unregister_files(&node->ring);

        if (unregister_result < 0 &&
            unregister_result != -ENXIO &&
            unregister_result != -EINVAL) {
            if (node->runtime != NULL) {
                llam_record_fatal_deferred(
                    node->runtime, -unregister_result);
            }
        } else {
            node->native_fixed_files_registered = false;
        }
        return 0;
    }
    node->native_fixed_buffers_registered = true;
    node->supports_native_fixed_files = true;
    node->supports_native_fixed_buffers = true;
    return 0;
}

int llam_linux_native_resources_attach(
    llam_node_t *node,
    const int *fds,
    unsigned fd_count,
    const struct iovec *buffers,
    unsigned buffer_count,
    llam_linux_native_resource_lease_t *lease) {
    unsigned file_slots[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    unsigned
        buffer_slots[LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS];
    uint64_t reserved_files;
    uint64_t reserved_buffers;
    unsigned updated_files = 0U;
    unsigned updated_buffers = 0U;
    unsigned i;
    int error = 0;
    bool cleanup_failed = false;

    if (node == NULL ||
        fds == NULL ||
        buffers == NULL ||
        lease == NULL ||
        fd_count == 0U ||
        buffer_count == 0U ||
        fd_count >
            LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS ||
        buffer_count >
            LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS ||
        lease->attached ||
        !node->native_resource_lock_initialized) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < fd_count; i += 1U) {
        if (fds[i] < 0) {
            errno = EINVAL;
            return -1;
        }
    }
    for (i = 0U; i < buffer_count; i += 1U) {
        if (buffers[i].iov_base == NULL ||
            buffers[i].iov_len == 0U) {
            errno = EINVAL;
            return -1;
        }
    }
    pthread_mutex_lock(&node->native_resource_lock);
    if (!node->supports_native_fixed_files ||
        !node->supports_native_fixed_buffers) {
        error = ENOTSUP;
        goto fail_without_reservation;
    }
    if (!llam_linux_native_reserve_slots(
            node->native_fixed_file_bitmap,
            fd_count,
            file_slots,
            &reserved_files) ||
        !llam_linux_native_reserve_slots(
            node->native_fixed_buffer_bitmap,
            buffer_count,
            buffer_slots,
            &reserved_buffers)) {
        error = ENOSPC;
        goto fail_without_reservation;
    }
    node->native_fixed_file_bitmap |= reserved_files;
    node->native_fixed_buffer_bitmap |= reserved_buffers;

    for (i = 0U; i < fd_count; i += 1U) {
        int result = llam_linux_native_files_update(
            node, file_slots[i], &fds[i], 1U);

        error = llam_linux_native_update_error(result);
        if (error != 0) {
            goto rollback;
        }
        updated_files += 1U;
    }
    for (i = 0U; i < buffer_count; i += 1U) {
        int result = llam_linux_native_buffers_update(
            node,
            buffer_slots[i],
            &buffers[i],
            1U);

        error = llam_linux_native_update_error(result);
        if (error != 0) {
            goto rollback;
        }
        updated_buffers += 1U;
    }

    memset(lease, 0, sizeof(*lease));
    memcpy(
        lease->file_slots,
        file_slots,
        fd_count * sizeof(file_slots[0]));
    memcpy(
        lease->buffer_slots,
        buffer_slots,
        buffer_count * sizeof(buffer_slots[0]));
    lease->file_count = fd_count;
    lease->buffer_count = buffer_count;
    lease->node_index = node->index;
    lease->attached = true;
    pthread_mutex_unlock(&node->native_resource_lock);
    errno = 0;
    return 0;

rollback:
    while (updated_buffers > 0U) {
        struct iovec empty = {NULL, 0U};
        int result;

        updated_buffers -= 1U;
        result = llam_linux_native_buffers_update(
            node,
            buffer_slots[updated_buffers],
            &empty,
            1U);
        if (llam_linux_native_update_error(result) != 0) {
            cleanup_failed = true;
        }
    }
    while (updated_files > 0U) {
        int empty = -1;
        int result;

        updated_files -= 1U;
        result = llam_linux_native_files_update(
            node,
            file_slots[updated_files],
            &empty,
            1U);
        if (llam_linux_native_update_error(result) != 0) {
            cleanup_failed = true;
        }
    }
    node->native_fixed_file_bitmap &= ~reserved_files;
    node->native_fixed_buffer_bitmap &= ~reserved_buffers;
    memset(lease, 0, sizeof(*lease));
    if (cleanup_failed) {
        llam_linux_native_resource_cleanup_failed(node);
    }
fail_without_reservation:
    pthread_mutex_unlock(&node->native_resource_lock);
    errno = error;
    return -1;
}

int llam_linux_native_resources_detach(
    llam_node_t *node,
    llam_linux_native_resource_lease_t *lease) {
    unsigned i;
    int error = 0;

    if (node == NULL ||
        lease == NULL ||
        !lease->attached ||
        lease->node_index != node->index ||
        lease->file_count == 0U ||
        lease->buffer_count == 0U ||
        lease->file_count >
            LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS ||
        lease->buffer_count >
            LLAM_LINUX_NATIVE_SEGMENT_MAX_OPS ||
        !node->native_resource_lock_initialized) {
        errno = EINVAL;
        return -1;
    }
    for (i = 0U; i < lease->file_count; i += 1U) {
        if (lease->file_slots[i] >=
            LLAM_LINUX_NATIVE_FIXED_FILE_SLOTS) {
            errno = EINVAL;
            return -1;
        }
    }
    for (i = 0U; i < lease->buffer_count; i += 1U) {
        if (lease->buffer_slots[i] >=
            LLAM_LINUX_NATIVE_FIXED_BUFFER_SLOTS) {
            errno = EINVAL;
            return -1;
        }
    }
    pthread_mutex_lock(&node->native_resource_lock);
    for (i = 0U; i < lease->file_count; i += 1U) {
        if ((node->native_fixed_file_bitmap &
             (UINT64_C(1) << lease->file_slots[i])) ==
            0U) {
            error = EINVAL;
            goto fail;
        }
    }
    for (i = 0U; i < lease->buffer_count; i += 1U) {
        if ((node->native_fixed_buffer_bitmap &
             (UINT64_C(1) << lease->buffer_slots[i])) ==
            0U) {
            error = EINVAL;
            goto fail;
        }
    }
    for (i = 0U; i < lease->buffer_count; i += 1U) {
        struct iovec empty = {NULL, 0U};
        int result = llam_linux_native_buffers_update(
            node,
            lease->buffer_slots[i],
            &empty,
            1U);

        error = llam_linux_native_update_error(result);
        if (error != 0) {
            goto fail;
        }
    }
    for (i = 0U; i < lease->file_count; i += 1U) {
        int empty = -1;
        int result = llam_linux_native_files_update(
            node,
            lease->file_slots[i],
            &empty,
            1U);

        error = llam_linux_native_update_error(result);
        if (error != 0) {
            goto fail;
        }
    }
    for (i = 0U; i < lease->file_count; i += 1U) {
        node->native_fixed_file_bitmap &=
            ~(UINT64_C(1) << lease->file_slots[i]);
    }
    for (i = 0U; i < lease->buffer_count; i += 1U) {
        node->native_fixed_buffer_bitmap &=
            ~(UINT64_C(1) << lease->buffer_slots[i]);
    }
    memset(lease, 0, sizeof(*lease));
    pthread_mutex_unlock(&node->native_resource_lock);
    errno = 0;
    return 0;

fail:
    pthread_mutex_unlock(&node->native_resource_lock);
    errno = error;
    return -1;
}

void llam_linux_native_resources_before_ring_exit(
    llam_node_t *node) {
    int buffers_result = 0;
    int files_result = 0;

    if (node == NULL ||
        !node->native_resource_lock_initialized) {
        return;
    }
    pthread_mutex_lock(&node->native_resource_lock);
    if (node->native_fixed_buffers_registered) {
        buffers_result =
            io_uring_unregister_buffers(&node->ring);
    }
    if (node->native_fixed_files_registered) {
        files_result =
            io_uring_unregister_files(&node->ring);
    }
    node->supports_native_fixed_buffers = false;
    node->supports_native_fixed_files = false;
    if (buffers_result >= 0 ||
        buffers_result == -ENXIO ||
        buffers_result == -EINVAL) {
        node->native_fixed_buffers_registered = false;
    }
    if (files_result >= 0 ||
        files_result == -ENXIO ||
        files_result == -EINVAL) {
        node->native_fixed_files_registered = false;
    }
    pthread_mutex_unlock(&node->native_resource_lock);

    if (node->runtime != NULL) {
        if (buffers_result < 0 &&
            buffers_result != -ENXIO &&
            buffers_result != -EINVAL) {
            llam_record_fatal_deferred(
                node->runtime, -buffers_result);
        }
        if (files_result < 0 &&
            files_result != -ENXIO &&
            files_result != -EINVAL) {
            llam_record_fatal_deferred(
                node->runtime, -files_result);
        }
    }
}

void llam_linux_native_resources_after_ring_exit(
    llam_node_t *node) {
    if (node == NULL ||
        !node->native_resource_lock_initialized) {
        return;
    }
    pthread_mutex_lock(&node->native_resource_lock);
    node->supports_native_fixed_buffers = false;
    node->supports_native_fixed_files = false;
    node->native_fixed_buffers_registered = false;
    node->native_fixed_files_registered = false;
    node->native_fixed_file_bitmap = 0U;
    node->native_fixed_buffer_bitmap = 0U;
    pthread_mutex_unlock(&node->native_resource_lock);
    pthread_mutex_destroy(&node->native_resource_lock);
    node->native_resource_lock_initialized = false;
}
