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

    /*
     * Owned buffers can cross FFI boundaries and outlive the producing task, so
     * their public handle must not collide with task/sync registry handles.
     */
    return llam_public_slot_reserve_family_secret(&g_llam_io_buffer_public_slots,
                                                  buffer,
                                                  64U,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_IO_BUFFER,
                                                  buffer->owner_runtime != NULL
                                                      ? buffer->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

static bool llam_io_buffer_public_decode_handle_locked(const llam_io_buffer_t *handle,
                                                       bool allow_raw_internal,
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

    if (allow_raw_internal) {
        /*
         * Internal setup/error paths pass the real wrapper address before the
         * encoded public handle escapes. Check that live-list membership first:
         * a large long-running slot table can make a normal heap pointer decode
         * to an in-range slot number, and an encoded public handle is also a
         * pointer-sized value. Raw cleanup must therefore accept only the live
         * wrapper address and must never fall through to public-handle decode.
         */
        buffer = (llam_io_buffer_t *)handle;
        if (llam_io_buffer_public_is_live_locked(buffer)) {
            if (out_buffer != NULL) {
                *out_buffer = buffer;
            }
            return true;
        }
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

    return false;
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

static llam_io_buffer_t *llam_io_buffer_public_unregister_impl(llam_io_buffer_t *handle, bool allow_raw_internal) {
    llam_io_buffer_t *buffer = NULL;
    llam_io_buffer_t **link;
    struct timespec ts;

    if (handle == NULL) {
        return NULL;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (!llam_io_buffer_public_decode_handle_locked(handle, allow_raw_internal, &buffer)) {
        (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
        return NULL;
    }
    if (llam_public_active_op_count(&buffer->public_active_ops) >= (SIZE_MAX / 2U)) {
        /*
         * SIZE_MAX is the permanent busy sentinel used when public active-op
         * accounting detects corrupted or exhausted state. Release must fail
         * closed while the handle is still live; consuming the slot first would
         * strand the caller in the wait loop below and eventually recycle
         * storage that accessors still conceptually pin.
         */
        errno = EBUSY;
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
                 * Scalar accessors pin the wrapper while copying fields.  The
                 * data accessor returns a borrowed pointer under the registry
                 * lock and the public contract requires users to serialize raw
                 * pointer use against release. Release is a cold path, so wait
                 * outside the registry lock for pinned scalar accessors before
                 * the wrapper is recycled or freed.
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

llam_io_buffer_t *llam_io_buffer_public_unregister(llam_io_buffer_t *handle) {
    return llam_io_buffer_public_unregister_impl(handle, false);
}

llam_io_buffer_t *llam_io_buffer_public_unregister_raw(llam_io_buffer_t *handle) {
    return llam_io_buffer_public_unregister_impl(handle, true);
}

llam_io_buffer_t *llam_io_buffer_public_begin_op(const llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = NULL;

    if (buffer == NULL) {
        return NULL;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (llam_io_buffer_public_decode_handle_locked(buffer, false, &live)) {
        /*
         * Public accessors may race with release from another unmanaged thread.
         * Pin under the same registry lock used by unregister so release cannot
         * free the wrapper until the accessor has copied the requested field.
         */
        if (llam_public_active_op_try_begin(&live->public_active_ops) != 0) {
            live = NULL;
        }
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

static void llam_io_buffer_detach_provided_storage_locked(llam_io_buffer_t *buffer) {
    unsigned char *copy = NULL;
    size_t copy_size;

    if (buffer == NULL || !buffer->provided_storage || buffer->data == NULL) {
        return;
    }
    /*
     * Backend provided-buffer rings are runtime-owned.  A public owned-buffer
     * handle may outlive that runtime, so shutdown must sever the data pointer
     * before node teardown releases the ring storage.  Provided buffers are
     * normally LLAM_IO_BUFFER_INLINE_BYTES, but keep a heap fallback for future
     * larger backend groups.
     */
    copy_size = buffer->size;
    if (LLAM_UNLIKELY(copy_size > buffer->capacity)) {
        /*
         * Capacity is the backend slice boundary. If a corrupt request or
         * backend bug publishes size beyond that boundary, preserve only the
         * bytes that can belong to this public buffer rather than over-reading
         * runtime-owned ring storage during shutdown.
         */
        copy_size = buffer->capacity;
    }
    if (copy_size <= sizeof(buffer->inline_data)) {
        if (copy_size != 0U) {
            memcpy(buffer->inline_data, buffer->data, copy_size);
        }
        buffer->data = buffer->inline_data;
        buffer->size = copy_size;
        buffer->capacity = sizeof(buffer->inline_data);
        buffer->alignment = sizeof(void *);
        buffer->external_storage = false;
        buffer->aligned_storage = false;
    } else {
        copy = malloc(copy_size);
        if (copy == NULL) {
            /*
             * Do not leave a dangling backend pointer.  Preserve a valid empty
             * buffer rather than risking a post-shutdown UAF on data access.
             */
            buffer->data = buffer->inline_data;
            buffer->size = 0U;
            buffer->capacity = sizeof(buffer->inline_data);
            buffer->alignment = sizeof(void *);
            buffer->external_storage = false;
            buffer->aligned_storage = false;
        } else {
            memcpy(copy, buffer->data, copy_size);
            buffer->data = copy;
            buffer->size = copy_size;
            buffer->capacity = copy_size;
            buffer->alignment = sizeof(void *);
            buffer->external_storage = true;
            buffer->aligned_storage = false;
        }
    }
    buffer->provided_storage = false;
    buffer->provided_bid = 0U;
    buffer->provided_node_index = UINT_MAX;
}

void *llam_io_buffer_public_data(llam_io_buffer_t *buffer) {
    llam_io_buffer_t *live = NULL;
    void *data = NULL;

    if (buffer == NULL) {
        return NULL;
    }

    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    if (llam_io_buffer_public_decode_handle_locked(buffer, false, &live)) {
        /*
         * Public data pointers are documented as valid until release.  Provided
         * buffer rings are runtime-owned, so expose only wrapper-owned storage
         * to prevent a caller-held pointer from becoming dangling when the
         * producing runtime is destroyed before the buffer handle is released.
         */
        llam_io_buffer_detach_provided_storage_locked(live);
        data = live->data;
    }
    (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
    return data;
}

void llam_io_buffer_public_detach_runtime_storage(llam_runtime_t *rt) {
    llam_io_buffer_t *buffer;

    if (rt == NULL) {
        return;
    }
    /*
     * This is a shutdown-only cold path.  Holding the public-buffer registry
     * lock keeps release/accessor calls from observing a half-detached wrapper
     * while the runtime still owns the backing I/O ring storage.
     */
    (void)pthread_mutex_lock(&g_llam_io_buffer_public_registry_lock);
    for (buffer = g_llam_io_buffer_public_registry; buffer != NULL; buffer = buffer->public_registry_next) {
        if (buffer->owner_runtime == rt) {
            llam_io_buffer_detach_provided_storage_locked(buffer);
        }
    }
    (void)pthread_mutex_unlock(&g_llam_io_buffer_public_registry_lock);
}
