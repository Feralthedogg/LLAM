/**
 * @file src/core/runtime_io_object_alloc.c
 * @brief I/O request and watch-object allocation helpers.
 *
 * @details
 * I/O requests and buffer wrappers are cached per shard. The hot path reuses
 * objects from the owner shard without global malloc/free traffic. If an object
 * is released from a foreign shard, it is pushed onto the allocation owner's
 * remote-free list and later drained by the owner at a quiescent point.
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
 * @brief Allocate a cleared I/O request object from a shard cache.
 *
 * @param shard Preferred allocation-owner shard.
 * @return Request object on success, or NULL on allocation failure.
 */
llam_io_req_t *llam_io_req_alloc(llam_shard_t *shard) {
    llam_io_req_t *req = NULL;

    if (shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        if (g_llam_tls_shard == shard) {
            // Owner-shard request allocation is lock-free and dominates the I/O
            // submission path.
            req = shard->allocator.io_req_free;
            if (req != NULL) {
                shard->allocator.io_req_free = req->alloc_next;
                shard->allocator.io_req_reuses += 1U;
                memset(req, 0, sizeof(*req));
                req->owner_shard = shard->id;
                req->alloc_owner_shard = shard->id;
                req->attached_node_index = UINT_MAX;
                atomic_store(&req->inflight_owner_shard, UINT_MAX);
                return req;
            }
        } else {
            // Cold external path: serialize access to the owner-local free list.
            llam_allocator_lock(&shard->allocator);
            req = shard->allocator.io_req_free;
            if (req != NULL) {
                shard->allocator.io_req_free = req->alloc_next;
                shard->allocator.io_req_reuses += 1U;
                llam_allocator_unlock(&shard->allocator);
                memset(req, 0, sizeof(*req));
                req->owner_shard = shard->id;
                req->alloc_owner_shard = shard->id;
                req->attached_node_index = UINT_MAX;
                atomic_store(&req->inflight_owner_shard, UINT_MAX);
                return req;
            }
            llam_allocator_unlock(&shard->allocator);
        }
        // Grow lazily so startup does not reserve request objects for inactive
        // shards or workloads that never touch I/O.
        if (llam_allocator_grow_io_req_slab(shard) != 0) {
            return NULL;
        }
    }
}

/**
 * @brief Return an I/O request object to its allocation owner.
 *
 * @param shard Current shard hint (unused; ownership is embedded in @p req).
 * @param req   Request object to recycle.
 */
void llam_io_req_free(llam_shard_t *shard, llam_io_req_t *req) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_shard_t *owner;
    llam_io_req_t *head;

    (void)shard;
    if (req == NULL || req->alloc_owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[req->alloc_owner_shard];
    req->next = NULL;
    req->alloc_next = NULL;
    if (g_llam_tls_shard != NULL && g_llam_tls_shard->id == owner->id) {
        // Fast return when the completion path runs on the allocation owner.
        req->alloc_next = owner->allocator.io_req_free;
        owner->allocator.io_req_free = req;
        owner->allocator.io_req_frees += 1U;
        return;
    }

    // Cross-shard completion returns through the remote-free queue; the owner
    // folds it back into the local free list at the next quiescent point.
    do {
        head = atomic_load(&owner->allocator.io_req_remote_free);
        req->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->allocator.io_req_remote_free, &head, req));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
}

/**
 * @brief Allocate an I/O buffer wrapper with at least the requested capacity.
 *
 * @param shard        Preferred allocation-owner shard.
 * @param min_capacity Minimum usable data capacity in bytes.
 * @return Buffer wrapper on success, or NULL on allocation failure.
 */
llam_io_buffer_t *llam_io_buffer_alloc(llam_shard_t *shard, size_t min_capacity) {
    llam_io_buffer_t *buffer = NULL;

    if (shard == NULL) {
        errno = EINVAL;
        return NULL;
    }

    for (;;) {
        if (g_llam_tls_shard == shard) {
            // Inline buffers stay entirely inside the slab object; large payload
            // requests attach external storage only for that allocation.
            buffer = shard->allocator.io_buffer_free;
            if (buffer != NULL) {
                shard->allocator.io_buffer_free = buffer->alloc_next;
                shard->allocator.io_buffer_reuses += 1U;
                memset(buffer, 0, sizeof(*buffer));
                buffer->alloc_owner_shard = shard->id;
                buffer->data = buffer->inline_data;
                buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
                if (min_capacity > LLAM_IO_BUFFER_INLINE_BYTES) {
                    buffer->data = calloc(1, min_capacity);
                    if (buffer->data == NULL) {
                        llam_io_buffer_allocator_free(buffer);
                        return NULL;
                    }
                    buffer->capacity = min_capacity;
                    buffer->external_storage = true;
                }
                return buffer;
            }
        } else {
            // Foreign allocation is rare, but keeps public API calls legal from
            // threads that are not currently running a scheduler shard.
            llam_allocator_lock(&shard->allocator);
            buffer = shard->allocator.io_buffer_free;
            if (buffer != NULL) {
                shard->allocator.io_buffer_free = buffer->alloc_next;
                shard->allocator.io_buffer_reuses += 1U;
                llam_allocator_unlock(&shard->allocator);
                memset(buffer, 0, sizeof(*buffer));
                buffer->alloc_owner_shard = shard->id;
                buffer->data = buffer->inline_data;
                buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
                if (min_capacity > LLAM_IO_BUFFER_INLINE_BYTES) {
                    buffer->data = calloc(1, min_capacity);
                    if (buffer->data == NULL) {
                        llam_io_buffer_allocator_free(buffer);
                        return NULL;
                    }
                    buffer->capacity = min_capacity;
                    buffer->external_storage = true;
                }
                return buffer;
            }
            llam_allocator_unlock(&shard->allocator);
        }
        // The requested capacity is handled after a wrapper is obtained; slab
        // growth only creates wrapper objects with inline storage.
        if (llam_allocator_grow_io_buffer_slab(shard) != 0) {
            return NULL;
        }
    }
}

/**
 * @brief Return an I/O buffer wrapper to its allocation owner.
 *
 * @param buffer Buffer wrapper to recycle.
 */
void llam_io_buffer_allocator_free(llam_io_buffer_t *buffer) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_shard_t *owner;
    llam_io_buffer_t *head;

    if (buffer == NULL) {
        return;
    }

    if (buffer->external_storage && buffer->data != NULL) {
        // External payload storage is not part of the slab cache. Release it
        // before restoring the wrapper to its inline baseline state.
        free(buffer->data);
    }
    buffer->external_storage = false;
    buffer->detached_wrapper = false;
    buffer->data = buffer->inline_data;
    buffer->size = 0U;
    buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
    buffer->alloc_next = NULL;

    if (buffer->alloc_owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[buffer->alloc_owner_shard];
    if (g_llam_tls_shard != NULL && g_llam_tls_shard->id == owner->id) {
        // Owner-local recycle avoids allocator locking and malloc churn.
        buffer->alloc_next = owner->allocator.io_buffer_free;
        owner->allocator.io_buffer_free = buffer;
        owner->allocator.io_buffer_frees += 1U;
        return;
    }

    // Remote frees are batched for the owner shard just like task/request frees.
    do {
        head = atomic_load(&owner->allocator.io_buffer_remote_free);
        buffer->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->allocator.io_buffer_remote_free, &head, buffer));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
}
