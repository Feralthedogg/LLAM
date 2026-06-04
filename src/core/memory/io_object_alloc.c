/**
 * @file src/core/memory/io_object_alloc.c
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
 * @brief Reinitialize an I/O request object for reuse.
 *
 * @details
 * Requests are recycled aggressively and contain several atomic handshake
 * fields shared by task, cancellation, timeout, backend completion, and rehome
 * paths.  Reset fields explicitly so recycling never byte-writes over an
 * already initialized atomic object.
 *
 * @param req               Request object to reset.
 * @param owner_runtime     Runtime that owns the request object.
 * @param owner_shard       Logical owner shard to publish.
 * @param alloc_owner_shard Allocation-owner shard for cache return.
 */
void llam_io_req_reset(llam_io_req_t *req, llam_runtime_t *owner_runtime, unsigned owner_shard, unsigned alloc_owner_shard) {
    if (req == NULL) {
        return;
    }

    req->owner_runtime = owner_runtime;
    req->kind = LLAM_IO_KIND_READ;
    req->fd = LLAM_INVALID_FD;
    req->handle = LLAM_INVALID_HANDLE;
    req->buf = NULL;
    req->count = 0U;
    req->offset = 0U;
    req->addr = NULL;
    req->addrlen = NULL;
    req->addr_len = 0U;
    req->result = 0;
    req->error_code = 0;
    req->poll_events = 0;
    req->poll_revents = 0;
    req->timeout_ms = 0;
    req->recv_flags = 0;
    req->task = NULL;
    req->next = NULL;
    req->alloc_next = NULL;
    req->poll_watch = NULL;
    req->accept_watch = NULL;
    req->recv_watch = NULL;
    req->owned_buffer = NULL;
    req->owner_shard = owner_shard;
    req->alloc_owner_shard = alloc_owner_shard;
    req->attached_node_index = UINT_MAX;
    req->fd_result = LLAM_INVALID_FD;
    atomic_init(&req->inflight_owner_shard, UINT_MAX);
    req->submit_ts_ns = 0U;
    req->deadline_ns = 0U;
    req->provided_bid = 0U;
    req->platform_data = NULL;
    atomic_init(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req->cancel_queued, 0U);
    req->use_recv_op = false;
    req->use_provided_buffer = false;
}

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
                llam_io_req_reset(req, shard->runtime, shard->id, shard->id);
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
                llam_io_req_reset(req, shard->runtime, shard->id, shard->id);
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
    llam_runtime_t *rt = req != NULL ? req->owner_runtime : NULL;
    llam_shard_t *owner;
    llam_io_req_t *head;

    (void)shard;
    if (req == NULL || rt == NULL || req->alloc_owner_shard >= rt->active_shards) {
        return;
    }

    owner = &rt->shards[req->alloc_owner_shard];
    req->next = NULL;
    req->alloc_next = NULL;
    if (g_llam_tls_shard == owner) {
        // Fast return when the completion path runs on the allocation owner.
        // Shard ids are only runtime-local, so pointer identity is the safe
        // concurrent-runtime ownership check.
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
llam_io_buffer_t *llam_io_buffer_allocator_alloc(llam_shard_t *shard, size_t min_capacity) {
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
                buffer->owner_runtime = shard->runtime;
                buffer->alloc_owner_shard = shard->id;
                buffer->data = buffer->inline_data;
                buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
                buffer->alignment = sizeof(void *);
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
                buffer->owner_runtime = shard->runtime;
                buffer->alloc_owner_shard = shard->id;
                buffer->data = buffer->inline_data;
                buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
                buffer->alignment = sizeof(void *);
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
    llam_runtime_t *rt = buffer != NULL ? buffer->owner_runtime : NULL;
    llam_runtime_t *pinned_rt = NULL;
    llam_shard_t *owner;
    llam_io_buffer_t *head;

    if (buffer == NULL) {
        return;
    }

    if (buffer->external_storage && buffer->data != NULL) {
        // External payload storage is not part of the slab cache. Release it
        // before restoring the wrapper to its inline baseline state.
#if LLAM_RUNTIME_BACKEND_WINDOWS
        if (buffer->aligned_storage) {
            _aligned_free(buffer->data);
        } else
#endif
        free(buffer->data);
    }
    buffer->external_storage = false;
    buffer->aligned_storage = false;
    buffer->detached_wrapper = false;
    buffer->data = buffer->inline_data;
    buffer->size = 0U;
    buffer->capacity = LLAM_IO_BUFFER_INLINE_BYTES;
    buffer->alignment = sizeof(void *);
    buffer->alloc_next = NULL;

    if (rt == NULL || llam_runtime_begin_public_op(rt, &pinned_rt) != 0) {
        return;
    }

    /*
     * Public owned buffers can be released by host threads while their runtime is
     * being destroyed.  Pin the runtime before consulting shard arrays; otherwise
     * release could race backend/cache teardown after the buffer leaves the public
     * registry.
     */
    if (buffer->alloc_owner_shard >= pinned_rt->active_shards) {
        llam_runtime_end_public_op(pinned_rt);
        return;
    }

    owner = &pinned_rt->shards[buffer->alloc_owner_shard];
    if (g_llam_tls_shard == owner) {
        // Owner-local recycle avoids allocator locking and malloc churn.  A
        // different runtime may have the same shard id and must use remote-free.
        buffer->alloc_next = owner->allocator.io_buffer_free;
        owner->allocator.io_buffer_free = buffer;
        owner->allocator.io_buffer_frees += 1U;
        llam_runtime_end_public_op(pinned_rt);
        return;
    }

    // Remote frees are batched for the owner shard just like task/request frees.
    do {
        head = atomic_load(&owner->allocator.io_buffer_remote_free);
        buffer->alloc_next = head;
    } while (!atomic_compare_exchange_weak(&owner->allocator.io_buffer_remote_free, &head, buffer));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
    llam_runtime_end_public_op(pinned_rt);
}
