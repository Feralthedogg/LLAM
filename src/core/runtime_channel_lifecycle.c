/**
 * @file src/core/runtime_channel_lifecycle.c
 * @brief Channel public-handle registry, creation, and destruction.
 *
 * @details
 * Channel send/recv paths are kept in runtime_channel.c.  This translation
 * unit owns the slower lifecycle surface so stale-handle safety and object
 * cache reuse remain centralized without growing the channel hot-path file.
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

static pthread_mutex_t g_llam_channel_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_channel_t *g_llam_channel_registry;
static llam_public_slot_table_t g_llam_channel_public_slots;

static int llam_channel_reserve_public_slot_locked(llam_channel_t *channel, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family(&g_llam_channel_public_slots,
                                           channel,
                                           64U,
                                           LLAM_PUBLIC_HANDLE_FAMILY_CHANNEL,
                                           out_slot,
                                           &generation);
}

int llam_channel_register_live(llam_channel_t *channel) {
    size_t slot = 0U;

    if (channel == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_llam_channel_registry_lock);
    if (llam_channel_reserve_public_slot_locked(channel, &slot) != 0) {
        pthread_mutex_unlock(&g_llam_channel_registry_lock);
        return -1;
    }
    channel->public_handle_slot = slot;
    channel->public_handle_generation = llam_public_slot_generation(&g_llam_channel_public_slots, slot);
    channel->registry_next = g_llam_channel_registry;
    g_llam_channel_registry = channel;
    pthread_mutex_unlock(&g_llam_channel_registry_lock);
    return 0;
}

static void llam_channel_unregister_live_locked(llam_channel_t *channel) {
    llam_channel_t **link = &g_llam_channel_registry;

    if (channel->public_handle_slot < g_llam_channel_public_slots.count) {
        llam_public_slot_release(&g_llam_channel_public_slots,
                                 channel->public_handle_slot,
                                 channel,
                                 channel->public_handle_generation);
    }

    while (*link != NULL) {
        if (*link == channel) {
            *link = channel->registry_next;
            channel->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

llam_channel_t *llam_channel_resolve_public_handle(const llam_channel_t *handle) {
    llam_channel_t *channel = NULL;

    if (handle == NULL) {
        return NULL;
    }

    /*
     * Public channel handles are slot+generation values. Do not derive a raw
     * pointer from the incoming value; a stale consumed handle can name a
     * recycled public slot, but it cannot match the replacement generation.
     */
    pthread_mutex_lock(&g_llam_channel_registry_lock);
    channel = llam_public_slot_resolve_encoded(&g_llam_channel_public_slots,
                                               (uintptr_t)handle,
                                               LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                               NULL,
                                               NULL);
    if (channel != NULL) {
        llam_public_active_op_begin(&channel->active_ops);
    }
    pthread_mutex_unlock(&g_llam_channel_registry_lock);
    return channel;
}

void llam_channel_end_public_op(llam_channel_t *channel) {
    if (channel == NULL) {
        return;
    }
    llam_public_active_op_end(&channel->active_ops);
}

int llam_channel_resolve_public_handles_for_select(llam_select_op_t *ops,
                                                   size_t op_count,
                                                   llam_channel_t **out_channels) {
    size_t i;

    if (ops == NULL || out_channels == NULL || op_count == 0U) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_llam_channel_registry_lock);
    for (i = 0U; i < op_count; ++i) {
        llam_channel_t *channel = llam_public_slot_resolve_encoded(&g_llam_channel_public_slots,
                                                                   (uintptr_t)ops[i].channel,
                                                                   LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                                                   NULL,
                                                                   NULL);

        if (channel == NULL) {
            while (i > 0U) {
                --i;
                llam_public_active_op_end(&out_channels[i]->active_ops);
                out_channels[i] = NULL;
            }
            pthread_mutex_unlock(&g_llam_channel_registry_lock);
            errno = EINVAL;
            return -1;
        }
        if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
            while (i > 0U) {
                --i;
                llam_public_active_op_end(&out_channels[i]->active_ops);
                out_channels[i] = NULL;
            }
            pthread_mutex_unlock(&g_llam_channel_registry_lock);
            return -1;
        }
        llam_public_active_op_begin(&channel->active_ops);
        out_channels[i] = channel;
    }
    pthread_mutex_unlock(&g_llam_channel_registry_lock);
    return 0;
}

void llam_channel_end_public_select_ops(llam_channel_t **channels, size_t op_count) {
    size_t i;

    if (channels == NULL) {
        return;
    }
    for (i = 0U; i < op_count; ++i) {
        if (channels[i] != NULL) {
            llam_public_active_op_end(&channels[i]->active_ops);
            channels[i] = NULL;
        }
    }
}

static size_t llam_channel_round_capacity(size_t capacity) {
    size_t rounded = 1U;

    if (capacity == 0U) {
        return 0U;
    }
    if (capacity > (SIZE_MAX / 2U) + 1U) {
        return 0U;
    }
    while (rounded < capacity) {
        if (rounded > SIZE_MAX / 2U) {
            return 0U;
        }
        rounded <<= 1U;
    }
    return rounded;
}

llam_channel_t *llam_channel_create(size_t capacity) {
    llam_channel_t *channel;
    size_t ring_capacity;
    int rc;

    if (capacity == 0U) {
        errno = EINVAL;
        return NULL;
    }
    ring_capacity = llam_channel_round_capacity(capacity);
    if (ring_capacity == 0U) {
        errno = ENOMEM;
        return NULL;
    }

    if (capacity == 1U) {
        channel = llam_channel_cache_acquire();
        if (channel != NULL) {
            return channel;
        }
    }

    channel = calloc(1, sizeof(*channel));
    if (channel == NULL) {
        return NULL;
    }

    channel->owner_runtime = llam_runtime_owner_for_new_object();
    llam_public_active_op_init(&channel->active_ops);
    channel->buffer = calloc(ring_capacity, sizeof(*channel->buffer));
    if (channel->buffer == NULL) {
        free(channel);
        return NULL;
    }
    channel->capacity = capacity;
    channel->ring_capacity = ring_capacity;
    channel->mask = ring_capacity - 1U;
    rc = pthread_mutex_init(&channel->lock, NULL);
    if (rc != 0) {
        free(channel->buffer);
        free(channel);
        errno = rc;
        return NULL;
    }
    if (llam_channel_register_live(channel) != 0) {
        pthread_mutex_destroy(&channel->lock);
        free(channel->buffer);
        free(channel);
        return NULL;
    }

    return llam_channel_public_handle(channel);
}

int llam_channel_destroy(llam_channel_t *channel) {
    uintptr_t handle = (uintptr_t)channel;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_channel_registry_lock);
    channel = llam_public_slot_resolve_encoded(&g_llam_channel_public_slots,
                                               handle,
                                               LLAM_SYNC_PUBLIC_HANDLE_SHIFT,
                                               &slot,
                                               &generation);
    if (channel == NULL ||
        channel->public_handle_slot != slot ||
        channel->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_channel_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(channel->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_channel_registry_lock);
        return -1;
    }

    pthread_mutex_lock(&channel->lock);
    if (channel->count != 0U ||
        llam_public_active_op_count(&channel->active_ops) != 0U ||
        atomic_load_explicit(&channel->inflight_waiters, memory_order_acquire) != 0U ||
        channel->send_waiters.head != NULL ||
        channel->send_waiters.depth != 0U ||
        channel->recv_waiters.head != NULL ||
        channel->recv_waiters.depth != 0U) {
        pthread_mutex_unlock(&channel->lock);
        pthread_mutex_unlock(&g_llam_channel_registry_lock);
        errno = EBUSY;
        return -1;
    }
    llam_channel_unregister_live_locked(channel);
    pthread_mutex_unlock(&channel->lock);
    pthread_mutex_unlock(&g_llam_channel_registry_lock);

    if (llam_channel_cache_release(channel)) {
        return 0;
    }

    pthread_mutex_destroy(&channel->lock);
    free(channel->buffer);
    free(channel);
    return 0;
}
