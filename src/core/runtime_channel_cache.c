/**
 * @file src/core/runtime_channel_cache.c
 * @brief Channel wait-node cache helpers used to reduce allocation churn.
 *
 * @details
 * Capacity-one channels are common in handoff-heavy benchmarks and APIs. This
 * file keeps a small thread-local cache plus a process-wide fallback cache so
 * short-lived channels can be reused without repeatedly allocating the channel
 * object and its one-slot buffer.
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

/** Default process-wide cache size for reusable capacity-one channels. */
#define LLAM_CHANNEL_CACHE_CAP_DEFAULT 64U
/** Default per-thread cache size for reusable capacity-one channels. */
#define LLAM_CHANNEL_TLS_CACHE_CAP_DEFAULT 16U

/** Global fallback channel cache lock. */
static pthread_mutex_t g_llam_channel_cache_lock = PTHREAD_MUTEX_INITIALIZER;
/** Head of the process-wide channel cache. */
static llam_channel_t *g_llam_channel_cache_head;
/** Number of entries in the process-wide channel cache. */
static unsigned g_llam_channel_cache_count;
/** Thread-local channel cache head. */
static _Thread_local llam_channel_t *g_llam_tls_channel_cache_head;
/** Thread-local channel cache entry count. */
static _Thread_local unsigned g_llam_tls_channel_cache_count;

/**
 * @brief Return the configured per-thread channel cache capacity.
 *
 * @return Capacity parsed from @c LLAM_CHANNEL_TLS_CACHE_CAP, capped to 1024.
 */
static unsigned llam_channel_tls_cache_cap(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_CHANNEL_TLS_CACHE_CAP");

        // Cache the parsed environment value so channel create/destroy stays
        // cheap after the first use on any thread.
        value = (int)LLAM_CHANNEL_TLS_CACHE_CAP_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(env, &end, 10);

            if (end != env) {
                if (parsed > 1024UL) {
                    parsed = 1024UL;
                }
                value = (int)parsed;
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Return the configured process-wide channel cache capacity.
 *
 * @return Capacity parsed from @c LLAM_CHANNEL_CACHE_CAP, capped to 4096.
 */
static unsigned llam_channel_cache_cap(void) {
    static atomic_int cached = ATOMIC_VAR_INIT(-1);
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_CHANNEL_CACHE_CAP");

        value = (int)LLAM_CHANNEL_CACHE_CAP_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long parsed = strtoul(env, &end, 10);

            if (end != env) {
                if (parsed > 4096UL) {
                    parsed = 4096UL;
                }
                value = (int)parsed;
            }
        }
        atomic_store_explicit(&cached, value, memory_order_release);
    }
    return (unsigned)value;
}

/**
 * @brief Reset channel state before putting it into or taking it from a cache.
 *
 * @param channel Channel object to clear for reuse.
 */
static void llam_channel_reset_for_reuse(llam_channel_t *channel) {
    if (channel == NULL) {
        return;
    }
    if (channel->buffer != NULL && channel->capacity > 0U) {
        // Keep the allocated buffer for reuse, but clear stale pointer payloads.
        memset(channel->buffer, 0, channel->capacity * sizeof(*channel->buffer));
    }
    channel->cache_next = NULL;
    channel->head = 0U;
    channel->tail = 0U;
    channel->count = 0U;
    channel->closed = false;
    channel->send_waiters = (llam_wait_queue_t){0};
    channel->recv_waiters = (llam_wait_queue_t){0};
}

/**
 * @brief Try to acquire a channel from the thread-local cache.
 *
 * @return Reusable channel or NULL on miss/disabled cache.
 */
static llam_channel_t *llam_channel_tls_cache_acquire(void) {
    llam_channel_t *channel;

    if (llam_channel_tls_cache_cap() == 0U) {
        return NULL;
    }

    channel = g_llam_tls_channel_cache_head;
    if (channel == NULL) {
        return NULL;
    }

    g_llam_tls_channel_cache_head = channel->cache_next;
    if (g_llam_tls_channel_cache_count > 0U) {
        g_llam_tls_channel_cache_count -= 1U;
    }
    llam_channel_reset_for_reuse(channel);
    return channel;
}

/**
 * @brief Try to release a channel to the thread-local cache.
 *
 * @param channel Channel to cache.
 * @return true when cached, false when caller should try the global cache.
 */
static bool llam_channel_tls_cache_release(llam_channel_t *channel) {
    unsigned cap;

    if (channel == NULL) {
        return false;
    }

    cap = llam_channel_tls_cache_cap();
    if (cap == 0U || g_llam_tls_channel_cache_count >= cap) {
        return false;
    }

    llam_channel_reset_for_reuse(channel);
    channel->cache_next = g_llam_tls_channel_cache_head;
    g_llam_tls_channel_cache_head = channel;
    g_llam_tls_channel_cache_count += 1U;
    return true;
}

/**
 * @brief Acquire a reusable capacity-one channel from TLS or global cache.
 *
 * @return Reusable channel on cache hit, or NULL on miss.
 */
llam_channel_t *llam_channel_cache_acquire(void) {
    llam_channel_t *channel;

    channel = llam_channel_tls_cache_acquire();
    if (channel != NULL) {
        return channel;
    }

    if (llam_channel_cache_cap() == 0U) {
        return NULL;
    }

    // Global fallback allows reuse across threads without making every channel
    // destroy path pay a malloc/free cost.
    pthread_mutex_lock(&g_llam_channel_cache_lock);
    channel = g_llam_channel_cache_head;
    if (channel != NULL) {
        g_llam_channel_cache_head = channel->cache_next;
        g_llam_channel_cache_count -= 1U;
    }
    pthread_mutex_unlock(&g_llam_channel_cache_lock);

    if (channel != NULL) {
        llam_channel_reset_for_reuse(channel);
    }
    return channel;
}

/**
 * @brief Release a capacity-one idle channel into a reuse cache.
 *
 * @param channel Channel being destroyed.
 * @return true if the channel was cached, false if the caller must free it.
 */
bool llam_channel_cache_release(llam_channel_t *channel) {
    unsigned cap;

    if (channel == NULL || channel->capacity != 1U || channel->send_waiters.depth != 0U ||
        channel->recv_waiters.depth != 0U || channel->send_waiters.head != NULL || channel->recv_waiters.head != NULL) {
        // Only a completely idle one-slot channel is safe to recycle here.
        return false;
    }

    if (llam_channel_tls_cache_release(channel)) {
        return true;
    }

    cap = llam_channel_cache_cap();
    if (cap == 0U) {
        return false;
    }

    llam_channel_reset_for_reuse(channel);
    pthread_mutex_lock(&g_llam_channel_cache_lock);
    if (g_llam_channel_cache_count >= cap) {
        pthread_mutex_unlock(&g_llam_channel_cache_lock);
        return false;
    }
    channel->cache_next = g_llam_channel_cache_head;
    g_llam_channel_cache_head = channel;
    g_llam_channel_cache_count += 1U;
    pthread_mutex_unlock(&g_llam_channel_cache_lock);
    return true;
}
