/**
 * @file src/core/runtime_channel_fast.c
 * @brief Buffered channel hot-path helpers.
 *
 * @details
 * The blocking send/recv state machine stays in runtime_channel.c. This file
 * owns small non-parking shortcuts that only touch buffered channel state and
 * fall back before any waiter wakeup, parking, or deadline handling is needed.
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

typedef enum llam_channel_fast_result {
    LLAM_CHANNEL_FAST_FALLBACK = 0,
    LLAM_CHANNEL_FAST_DONE = 1,
    LLAM_CHANNEL_FAST_ERROR = -1,
} llam_channel_fast_result_t;

int llam_channel_try_send_buffered_fast(llam_channel_t *handle, void *value) {
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    llam_runtime_t *current_owner;
#endif
    llam_channel_t *channel;

    if (g_llam_tls_task == NULL || g_llam_tls_shard == NULL) {
        return LLAM_CHANNEL_FAST_FALLBACK;
    }
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    current_owner = llam_runtime_tls_owner_fast();
    if (LLAM_UNLIKELY(current_owner == NULL)) {
        current_owner = llam_runtime_current_owner();
    }
#endif

    /*
     * Pure buffered sends are common in select-ready and single-slot ping loops.
     * Keeping the registry locked through the short probe protects the object
     * without taking an active-op pin, because no waiter wake escapes this call.
     */
    llam_channel_public_registry_lock();
    channel = llam_channel_resolve_public_handle_locked_unpinned(handle);
    if (LLAM_UNLIKELY(channel == NULL)) {
        llam_channel_public_registry_unlock();
        errno = EINVAL;
        return LLAM_CHANNEL_FAST_ERROR;
    }
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    if (LLAM_UNLIKELY(channel->owner_runtime == NULL || channel->owner_runtime != current_owner)) {
        llam_channel_public_registry_unlock();
        errno = channel->owner_runtime == NULL ? EINVAL : EXDEV;
        return LLAM_CHANNEL_FAST_ERROR;
    }
#else
    if (LLAM_UNLIKELY(channel->owner_runtime == NULL)) {
        llam_channel_public_registry_unlock();
        errno = EINVAL;
        return LLAM_CHANNEL_FAST_ERROR;
    }
#endif

    pthread_mutex_lock(&channel->lock);
    if (LLAM_UNLIKELY(channel->closed)) {
        pthread_mutex_unlock(&channel->lock);
        llam_channel_public_registry_unlock();
        errno = EPIPE;
        return LLAM_CHANNEL_FAST_ERROR;
    }
    if (LLAM_UNLIKELY(channel->recv_waiters.head != NULL || channel->count >= channel->capacity)) {
        pthread_mutex_unlock(&channel->lock);
        llam_channel_public_registry_unlock();
        return LLAM_CHANNEL_FAST_FALLBACK;
    }
    if (channel->capacity == 1U) {
        channel->buffer[0] = value;
    } else {
        channel->buffer[channel->tail] = value;
        channel->tail = (channel->tail + 1U) & channel->mask;
    }
    channel->count += 1U;
    pthread_mutex_unlock(&channel->lock);
    llam_channel_public_registry_unlock();
    llam_channel_hot_safepoint();
    return LLAM_CHANNEL_FAST_DONE;
}
