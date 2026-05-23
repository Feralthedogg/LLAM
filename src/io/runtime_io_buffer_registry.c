/**
 * @file src/io/runtime_io_buffer_registry.c
 * @brief Public owned-buffer lifetime registry.
 *
 * @details
 * Owned buffers returned by ::llam_read_owned and ::llam_recv_owned can escape
 * the task and runtime that produced them.  The registry is intentionally a
 * cold-path guard for public handle consumption: release/accessor calls can
 * reject stale encoded handles without reading a wrapper that has already been
 * freed.
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

static pthread_mutex_t g_llam_io_buffer_public_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_io_buffer_t *g_llam_io_buffer_public_registry;

#if UINTPTR_MAX <= UINT32_MAX
#error "LLAM owned-buffer public handles require uintptr_t wider than 32 bits"
#define LLAM_IO_BUFFER_PUBLIC_HANDLE_SHIFT 0U
#else
#define LLAM_IO_BUFFER_PUBLIC_HANDLE_SHIFT 32U
#endif

static llam_public_slot_table_t g_llam_io_buffer_public_slots;

static uintptr_t llam_io_buffer_public_encode_handle(size_t slot, uint32_t generation) {
    return llam_public_slot_encode_handle(slot, generation, LLAM_IO_BUFFER_PUBLIC_HANDLE_SHIFT);
}

static bool llam_io_buffer_public_is_live_locked(const llam_io_buffer_t *buffer) {
    const llam_io_buffer_t *current;

    for (current = g_llam_io_buffer_public_registry; current != NULL; current = current->public_registry_next) {
        if (current == buffer) {
            return true;
        }
    }
    return false;
}

static int llam_io_buffer_public_reserve_slot_locked(llam_io_buffer_t *buffer, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve(&g_llam_io_buffer_public_slots, buffer, 64U, out_slot, &generation);
}

static bool llam_io_buffer_public_decode_handle_locked(const llam_io_buffer_t *handle,
                                                      llam_io_buffer_t **out_buffer) {
    llam_io_buffer_t *buffer = NULL;
    size_t decoded_slot = 0U;
    uint32_t decoded_generation = 0U;

    if (out_buffer != NULL) {
        *out_buffer = NULL;
    }
    if (handle == NULL) {
        return false;
    }

    buffer = llam_public_slot_resolve_encoded(&g_llam_io_buffer_public_slots,
                                              (uintptr_t)handle,
                                              LLAM_IO_BUFFER_PUBLIC_HANDLE_SHIFT,
                                              NULL,
                                              NULL);
    if (buffer != NULL) {
        if (out_buffer != NULL) {
            *out_buffer = buffer;
        }
        return true;
    }
    if (llam_public_slot_decode_handle((uintptr_t)handle,
                                       LLAM_IO_BUFFER_PUBLIC_HANDLE_SHIFT,
                                       &decoded_slot,
                                       &decoded_generation)) {
        /*
         * If the decoded slot exists, the caller passed a stale or forged
         * public handle and must not fall through to raw-pointer lookup.  A
         * decoded slot outside the table is treated as an internal raw wrapper:
         * heap pointers often have nonzero high bits, while real public handles
         * always name an existing slot family entry.
         */
        if (decoded_slot < g_llam_io_buffer_public_slots.count) {
            return false;
        }
    }
    (void)decoded_slot;
    (void)decoded_generation;

    /*
     * Internal setup/error paths may still pass the raw wrapper before it has
     * escaped through the public ABI.  Public callers receive slot handles, so
     * this fallback does not reintroduce the stale-release ABA for user-owned
     * handles.
     */
    buffer = (llam_io_buffer_t *)handle;
    if (!llam_io_buffer_public_is_live_locked(buffer)) {
        return false;
    }
    if (out_buffer != NULL) {
        *out_buffer = buffer;
    }
    return true;
}

int llam_io_buffer_public_register(llam_io_buffer_t *buffer) {
    size_t slot = 0U;

    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (llam_io_buffer_public_reserve_slot_locked(buffer, &slot) != 0) {
        (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
        return -1;
    }
    buffer->public_handle_slot = slot;
    buffer->public_handle_generation = llam_public_slot_generation(&g_llam_io_buffer_public_slots, slot);
    llam_public_active_op_init(&buffer->public_active_ops);
    buffer->public_registry_next = g_llam_io_buffer_public_registry;
    g_llam_io_buffer_public_registry = buffer;
    (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
    return 0;
}

llam_io_buffer_t *llam_io_buffer_public_handle(llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = NULL;
    uintptr_t encoded = 0U;

    if (buffer == NULL) {
        return NULL;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (llam_io_buffer_public_is_live_locked(buffer) &&
        buffer->public_handle_slot < g_llam_io_buffer_public_slots.count &&
        llam_public_slot_resolve(&g_llam_io_buffer_public_slots,
                                 buffer->public_handle_slot,
                                 buffer->public_handle_generation) == buffer) {
        live = buffer;
        encoded = llam_io_buffer_public_encode_handle(buffer->public_handle_slot, buffer->public_handle_generation);
    }
    (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
    (void)live;
    return encoded != 0U ? (llam_io_buffer_t *)encoded : NULL;
}

llam_io_buffer_t *llam_io_buffer_public_unregister(llam_io_buffer_t *handle) {
    llam_io_buffer_t *buffer = NULL;
    llam_io_buffer_t **link;
    struct timespec ts;

    if (handle == NULL) {
        return NULL;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (!llam_io_buffer_public_decode_handle_locked(handle, &buffer)) {
        (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
        return NULL;
    }
    if (buffer->public_handle_slot < g_llam_io_buffer_public_slots.count) {
        llam_public_slot_release(&g_llam_io_buffer_public_slots,
                                 buffer->public_handle_slot,
                                 buffer,
                                 buffer->public_handle_generation);
    }
    for (link = &g_llam_io_buffer_public_registry; *link != NULL; link = &(*link)->public_registry_next) {
        if (*link == buffer) {
            *link = buffer->public_registry_next;
            buffer->public_registry_next = NULL;
            while (llam_public_active_op_count(&buffer->public_active_ops) != 0U) {
                /*
                 * Accessors pin the wrapper only while copying scalar fields.
                 * Release is a cold path, so wait outside the registry lock to
                 * let in-flight accessors drop their pins before the wrapper is
                 * recycled or freed.
                 */
                (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
                ts.tv_sec = 0;
                ts.tv_nsec = 1000000L;
                (void)nanosleep(&ts, NULL);
                (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
            }
            (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
            return buffer;
        }
    }
    (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
    return NULL;
}

llam_io_buffer_t *llam_io_buffer_public_begin_op(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = NULL;

    if (buffer == NULL) {
        return NULL;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (llam_io_buffer_public_decode_handle_locked(buffer, &live)) {
        /*
         * Public accessors may race with release from another unmanaged thread.
         * Pin under the same registry lock used by unregister so release cannot
         * free the wrapper until the accessor has copied the requested field.
         */
        llam_public_active_op_begin(&live->public_active_ops);
    }
    (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
    return live;
}

void llam_io_buffer_public_end_op(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = (llam_io_buffer_t *)buffer;

    if (live == NULL) {
        return;
    }

    llam_public_active_op_end(&live->public_active_ops);
}
