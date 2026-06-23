/**
 * @file src/core/sync/channel_cache.c
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
 * @brief Free a cached channel object and its retained one-slot buffer.
 *
 * Cached channels keep their mutex initialized while idle so acquisition can
 * reuse the object directly.  Draining a cache must therefore destroy the mutex
 * before releasing the backing storage.
 */
static void llam_channel_cache_free_entry(llam_channel_t *channel) {
    if (channel == NULL) {
        return;
    }
    pthread_mutex_destroy(&channel->lock);
    free(channel->buffer);
    free(channel);
}

/**
 * @brief Return the configured per-thread channel cache capacity.
 *
 * @return Capacity parsed from @c LLAM_CHANNEL_TLS_CACHE_CAP, capped to 1024.
 */
static unsigned llam_channel_tls_cache_cap(void) {
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_CHANNEL_TLS_CACHE_CAP");

        // Cache the parsed environment value so channel create/destroy stays
        // cheap after the first use on any thread.
        value = (int)LLAM_CHANNEL_TLS_CACHE_CAP_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long parsed;

            if (!llam_ascii_is_space((unsigned char)env[0]) && env[0] != '-' && env[0] != '+') {
                errno = 0;
                parsed = strtoul(env, &end, 10);
                if (errno == 0 && end != env && *end == '\0') {
                    if (parsed > 1024UL) {
                        parsed = 1024UL;
                    }
                    value = (int)parsed;
                }
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
    static atomic_int cached = -1;
    int value = atomic_load_explicit(&cached, memory_order_acquire);

    if (value < 0) {
        const char *env = llam_env_get("LLAM_CHANNEL_CACHE_CAP");

        value = (int)LLAM_CHANNEL_CACHE_CAP_DEFAULT;
        if (env != NULL && env[0] != '\0') {
            char *end = NULL;
            unsigned long parsed;

            if (!llam_ascii_is_space((unsigned char)env[0]) && env[0] != '-' && env[0] != '+') {
                errno = 0;
                parsed = strtoul(env, &end, 10);
                if (errno == 0 && end != env && *end == '\0') {
                    if (parsed > 4096UL) {
                        parsed = 4096UL;
                    }
                    value = (int)parsed;
                }
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
static void llam_channel_reset_for_reuse(llam_channel_t *channel, llam_runtime_t *owner_runtime) {
    if (channel == NULL) {
        return;
    }
    channel->owner_runtime = owner_runtime;
    channel->registry_next = NULL;
    llam_public_active_op_init(&channel->active_ops);
    if (channel->buffer != NULL && channel->ring_capacity > 0U) {
        // Keep the allocated buffer for reuse, but clear old payload slots.
        memset(channel->buffer, 0, channel->ring_capacity * sizeof(*channel->buffer));
    }
    channel->cache_next = NULL;
    channel->mask = channel->ring_capacity > 0U ? channel->ring_capacity - 1U : 0U;
    channel->head = 0U;
    channel->tail = 0U;
    channel->count = 0U;
    atomic_store_explicit(&channel->inflight_waiters, 0U, memory_order_release);
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

    if (g_llam_tls_shard == NULL && g_llam_tls_task == NULL) {
        return NULL;
    }
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
    llam_channel_reset_for_reuse(channel, llam_runtime_owner_for_new_object());
    if (llam_channel_register_live(channel) != 0) {
        llam_channel_cache_free_entry(channel);
        return NULL;
    }
    return llam_channel_public_handle(channel);
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
    if (g_llam_tls_shard == NULL && g_llam_tls_task == NULL) {
        return false;
    }

    cap = llam_channel_tls_cache_cap();
    if (cap == 0U || g_llam_tls_channel_cache_count >= cap) {
        return false;
    }

    /*
     * A cached channel is no longer a live public handle. Poison the owner so
     * an accidental second destroy through a consumed handle fails before the
     * same object can be inserted into the cache twice.
     */
    llam_channel_reset_for_reuse(channel, NULL);
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

    if (g_llam_tls_shard == NULL && g_llam_tls_task == NULL) {
        return NULL;
    }
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
        llam_channel_reset_for_reuse(channel, llam_runtime_owner_for_new_object());
        if (llam_channel_register_live(channel) != 0) {
            llam_channel_cache_free_entry(channel);
            return NULL;
        }
    }
    return llam_channel_public_handle(channel);
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
        channel->recv_waiters.depth != 0U ||
        atomic_load_explicit(&channel->inflight_waiters, memory_order_acquire) != 0U ||
        channel->send_waiters.head != NULL || channel->recv_waiters.head != NULL) {
        // Only a completely idle one-slot channel is safe to recycle here.
        return false;
    }
    if (g_llam_tls_shard == NULL && g_llam_tls_task == NULL) {
        // Outside managed scheduler threads there is no reliable lifecycle hook
        // to drain TLS cache entries. Preserve destroy-as-free semantics there.
        return false;
    }

    if (llam_channel_tls_cache_release(channel)) {
        return true;
    }

    cap = llam_channel_cache_cap();
    if (cap == 0U) {
        return false;
    }

    /*
     * Keep cached entries outside the public ownership domain. Acquisition
     * stamps the requesting runtime owner back onto the object before returning
     * it, so an idle cached channel never carries a stale runtime owner across
     * runtime boundaries.
     */
    llam_channel_reset_for_reuse(channel, NULL);
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

/**
 * @brief Release every capacity-one channel cached by the current OS thread.
 *
 * Worker threads own independent TLS caches.  Without an explicit drain before
 * thread exit, cached channel objects become unreachable even though the public
 * channel has already been destroyed.
 */
void llam_channel_tls_cache_drain(void) {
    llam_channel_t *channel = g_llam_tls_channel_cache_head;

    g_llam_tls_channel_cache_head = NULL;
    g_llam_tls_channel_cache_count = 0U;
    while (channel != NULL) {
        llam_channel_t *next = channel->cache_next;

        channel->cache_next = NULL;
        llam_channel_cache_free_entry(channel);
        channel = next;
    }
}

/**
 * @brief Release every channel retained by the process-wide fallback cache.
 *
 * Runtime shutdown calls this after workers have stopped so repeated
 * init/shutdown cycles do not keep stale cache storage indefinitely.
 */
void llam_channel_global_cache_drain(void) {
    llam_channel_t *channel;

    pthread_mutex_lock(&g_llam_channel_cache_lock);
    channel = g_llam_channel_cache_head;
    g_llam_channel_cache_head = NULL;
    g_llam_channel_cache_count = 0U;
    pthread_mutex_unlock(&g_llam_channel_cache_lock);

    while (channel != NULL) {
        llam_channel_t *next = channel->cache_next;

        channel->cache_next = NULL;
        llam_channel_cache_free_entry(channel);
        channel = next;
    }
}
