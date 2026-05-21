/**
 * @file src/core/runtime_wait_timer_alloc.c
 * @brief Allocator helpers for wait timers and wait-related small objects.
 *
 * @details
 * Wait nodes and timer nodes are short-lived scheduler objects. They use the
 * same shard-local cache model as tasks and I/O objects: owner-shard reuse is
 * direct, foreign returns are batched through atomic remote-free lists.
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

/**
 * @brief Allocate a wait-node object from a shard cache.
 *
 * @param shard Preferred owner shard.
 * @return Cleared wait node on success, or NULL on allocation failure.
 */
llam_wait_node_t *llam_wait_node_alloc(llam_shard_t *shard) {
    llam_wait_node_t *node = NULL;

    if (shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        if (g_llam_tls_shard == shard) {
            // Most wait nodes are created by the scheduler shard that will own
            // them, so this path avoids allocator locking.
            node = shard->allocator.wait_free;
            if (node != NULL) {
                shard->allocator.wait_free = node->alloc_next;
                shard->allocator.wait_reuses += 1U;
                llam_wait_node_reset(node, shard->runtime, shard->id);
                return node;
            }
        } else {
            // Cold external path for API/bootstrap callers not bound to shard TLS.
            llam_allocator_lock(&shard->allocator);
            node = shard->allocator.wait_free;
            if (node != NULL) {
                shard->allocator.wait_free = node->alloc_next;
                shard->allocator.wait_reuses += 1U;
                llam_allocator_unlock(&shard->allocator);
                llam_wait_node_reset(node, shard->runtime, shard->id);
                return node;
            }
            llam_allocator_unlock(&shard->allocator);
        }
        // Empty free list: grow one slab and retry the normal pop path.
        if (llam_allocator_grow_wait_slab(shard) != 0) {
            return NULL;
        }
    }
}

/**
 * @brief Return a wait node to its allocation owner.
 *
 * @param shard Current shard hint (unused; ownership is stored in @p node).
 * @param node  Wait node to recycle.
 */
void llam_wait_node_free(llam_shard_t *shard, llam_wait_node_t *node) {
    llam_runtime_t *rt = node != NULL && node->owner_runtime != NULL ? node->owner_runtime : &g_llam_runtime;
    llam_shard_t *owner;
    llam_wait_node_t *head;

    (void)shard;
    if (node == NULL || node->owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[node->owner_shard];
    node->next = NULL;
    node->alloc_next = NULL;
    if (g_llam_tls_shard != NULL && g_llam_tls_shard->id == owner->id) {
        // Owner-local free goes straight back to the shard cache.
        node->alloc_next = owner->allocator.wait_free;
        owner->allocator.wait_free = node;
        owner->allocator.wait_frees += 1U;
        return;
    }

    // Foreign free: publish to the owner's remote list for quiescent drain.
    do {
        head = atomic_load(&owner->allocator.wait_remote_free);
        node->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->allocator.wait_remote_free, &head, node));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
}

/**
 * @brief Allocate a timer-node object from a shard cache.
 *
 * @param shard Preferred owner shard.
 * @return Cleared timer node on success, or NULL on allocation failure.
 */
llam_timer_node_t *llam_timer_node_alloc(llam_shard_t *shard) {
    llam_timer_node_t *node = NULL;

    if (shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        if (g_llam_tls_shard == shard) {
            // Timer allocation is on scheduler hot paths; keep same-shard reuse
            // lock-free.
            node = shard->allocator.timer_free;
            if (node != NULL) {
                shard->allocator.timer_free = node->alloc_next;
                shard->allocator.timer_reuses += 1U;
                memset(node, 0, sizeof(*node));
                node->owner_runtime = shard->runtime;
                node->owner_shard = shard->id;
                return node;
            }
        } else {
            // Cold path when a foreign caller needs a timer node from this shard.
            llam_allocator_lock(&shard->allocator);
            node = shard->allocator.timer_free;
            if (node != NULL) {
                shard->allocator.timer_free = node->alloc_next;
                shard->allocator.timer_reuses += 1U;
                llam_allocator_unlock(&shard->allocator);
                memset(node, 0, sizeof(*node));
                node->owner_runtime = shard->runtime;
                node->owner_shard = shard->id;
                return node;
            }
            llam_allocator_unlock(&shard->allocator);
        }
        // Grow lazily when the timer cache is exhausted.
        if (llam_allocator_grow_timer_slab(shard) != 0) {
            return NULL;
        }
    }
}

/**
 * @brief Return a timer node to its allocation owner.
 *
 * @param shard Current shard hint (unused; ownership is stored in @p node).
 * @param node  Timer node to recycle.
 */
void llam_timer_node_free(llam_shard_t *shard, llam_timer_node_t *node) {
    llam_runtime_t *rt = node != NULL && node->owner_runtime != NULL ? node->owner_runtime : &g_llam_runtime;
    llam_shard_t *owner;
    llam_timer_node_t *head;

    (void)shard;
    if (node == NULL || node->owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[node->owner_shard];
    node->next = NULL;
    node->alloc_next = NULL;
    if (g_llam_tls_shard != NULL && g_llam_tls_shard->id == owner->id) {
        // Common path: timer fires/cancels on its owner shard.
        node->alloc_next = owner->allocator.timer_free;
        owner->allocator.timer_free = node;
        owner->allocator.timer_frees += 1U;
        return;
    }

    // Cross-shard cancellation returns through the remote-free queue.
    do {
        head = atomic_load(&owner->allocator.timer_remote_free);
        node->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->allocator.timer_remote_free, &head, node));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
}
