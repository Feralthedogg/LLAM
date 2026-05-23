/**
 * @file src/io/runtime_io_api_owned.c
 * @brief Public owned I/O buffer wrappers and accessors.
 *
 * @details
 * Owned-buffer APIs are grouped away from descriptor read/write entry points so
 * their registry lifetime rules stay visible: public handles are consumed by
 * release and pinned by accessors before buffer storage is inspected.
 *
 * @copyright Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 */

#include "runtime_io_api_internal.h"

ssize_t llam_read_owned(llam_fd_t fd, size_t max_count, llam_io_buffer_t **out) {
    return llam_read_owned_impl(fd, max_count, 0, false, out);
}

ssize_t llam_recv_owned(llam_fd_t fd, size_t max_count, int flags, llam_io_buffer_t **out) {
    return llam_read_owned_impl(fd, max_count, flags, true, out);
}

void llam_io_buffer_release(llam_io_buffer_t *buffer) {
    llam_io_buffer_t *raw_buffer;

    if (buffer == NULL) {
        return;
    }

    raw_buffer = llam_io_buffer_public_unregister(buffer);
    if (raw_buffer == NULL) {
        return;
    }
    buffer = raw_buffer;

    if (buffer->detached_wrapper) {
        if (buffer->external_storage && buffer->data != NULL) {
            free(buffer->data);
        }
        free(buffer);
        return;
    }

    if (buffer->provided_storage &&
        buffer->owner_runtime != NULL &&
        atomic_load_explicit(&buffer->owner_runtime->initialized, memory_order_acquire) &&
        buffer->provided_node_index < buffer->owner_runtime->active_nodes) {
        (void)llam_node_recycle_recv_buffer(&buffer->owner_runtime->nodes[buffer->provided_node_index],
                                          buffer->provided_bid);
        buffer->provided_storage = false;
        buffer->provided_bid = 0U;
    }

    llam_io_buffer_allocator_free(buffer);
}

void *llam_io_buffer_data(llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = llam_io_buffer_public_begin_op(buffer);
    void *data;

    if (live == NULL) {
        return NULL;
    }
    data = live->data;
    llam_io_buffer_public_end_op(live);
    return data;
}

size_t llam_io_buffer_size(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = llam_io_buffer_public_begin_op(buffer);
    size_t size;

    if (live == NULL) {
        return 0U;
    }
    size = live->size;
    llam_io_buffer_public_end_op(live);
    return size;
}

size_t llam_io_buffer_capacity(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = llam_io_buffer_public_begin_op(buffer);
    size_t capacity;

    if (live == NULL) {
        return 0U;
    }
    capacity = live->capacity;
    llam_io_buffer_public_end_op(live);
    return capacity;
}
