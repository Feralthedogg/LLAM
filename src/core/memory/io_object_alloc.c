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

/*
 * Request objects are aggressively recycled, including one embedded at a
 * stable address in every task.  Give every activation a process-unique epoch
 * so stale readers can distinguish two operations that used the same storage.
 * Saturation is terminal: wrapping to zero would reintroduce ABA.
 */
static atomic_uint_fast64_t g_llam_io_operation_generation = 0;

static uint64_t llam_io_req_next_operation_generation(llam_runtime_t *rt) {
    uint_fast64_t current = atomic_load_explicit(&g_llam_io_operation_generation,
                                                 memory_order_acquire);

    for (;;) {
        if (LLAM_UNLIKELY(current >= UINT64_MAX)) {
            if (rt != NULL) {
                llam_record_fatal(rt, EOVERFLOW);
            }
            errno = EOVERFLOW;
            return 0U;
        }
        if (atomic_compare_exchange_weak_explicit(&g_llam_io_operation_generation,
                                                  &current,
                                                  current + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return (uint64_t)(current + 1U);
        }
    }
}

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
    atomic_init(&req->owner_shard, owner_shard);
    req->alloc_owner_shard = alloc_owner_shard;
    atomic_init(&req->attached_node_index, UINT_MAX);
    req->fd_result = LLAM_INVALID_FD;
    atomic_init(&req->inflight_owner_shard, UINT_MAX);
    req->submit_ts_ns = 0U;
    req->deadline_ns = 0U;
    req->provided_bid = 0U;
    req->platform_data = NULL;
    atomic_init(&req->wait_mode, LLAM_IO_WAIT_MODE_NONE);
    atomic_init(&req->abort_reason, LLAM_IO_ABORT_NONE);
    atomic_init(&req->operation_generation, 0U);
    atomic_init(&req->cancel_queued, 0U);
    atomic_init(&req->cancel_submitted, 0U);
    atomic_init(&req->free_after_cancel, 0U);
    atomic_init(&req->backend_event_refs, 0U);
    atomic_init(&req->release_after_event, 0U);
    req->use_recv_op = false;
    req->use_provided_buffer = false;
    /* Publish an idle slot only after every reusable field is initialized. */
    atomic_init(&req->lifetime_refs, 0U);
}

bool llam_io_req_lifetime_activate(llam_io_req_t *req) {
    unsigned expected = 0U;
    uint64_t operation_generation;

    if (req == NULL || req->owner_runtime == NULL) {
        errno = EINVAL;
        return false;
    }
    operation_generation = llam_io_req_next_operation_generation(req->owner_runtime);
    if (operation_generation == 0U) {
        return false;
    }
    if (!atomic_compare_exchange_strong_explicit(&req->lifetime_refs,
                                                 &expected,
                                                 1U,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        llam_record_fatal(req->owner_runtime, expected == UINT_MAX ? EOVERFLOW : EBUSY);
        errno = expected == UINT_MAX ? EOVERFLOW : EBUSY;
        return false;
    }
    atomic_store_explicit(&req->operation_generation,
                          operation_generation,
                          memory_order_release);
    return true;
}

bool llam_io_req_lifetime_try_acquire(llam_io_req_t *req) {
    unsigned refs;

    if (req == NULL || req->owner_runtime == NULL) {
        errno = EINVAL;
        return false;
    }
    refs = atomic_load_explicit(&req->lifetime_refs, memory_order_acquire);
    for (;;) {
        if (refs == 0U || refs == UINT_MAX) {
            /* UINT_MAX is the embedded-request reset publication sentinel. */
            errno = EBUSY;
            return false;
        }
        if (refs == UINT_MAX - 1U) {
            /* Caller did not acquire ownership; latch without owner-lock recursion. */
            llam_record_fatal_deferred(req->owner_runtime, EOVERFLOW);
            errno = EOVERFLOW;
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&req->lifetime_refs,
                                                  &refs,
                                                  refs + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

static void llam_io_req_publish(llam_io_req_t *req) {
    llam_runtime_t *rt = req != NULL ? req->owner_runtime : NULL;
    llam_shard_t *owner;
    llam_io_req_t *head;

    if (req == NULL || rt == NULL) {
        return;
    }
    if (req->alloc_owner_shard == UINT_MAX) {
        llam_io_req_reset(req, rt, UINT_MAX, UINT_MAX);
        return;
    }
    if (req->alloc_owner_shard >= rt->active_shards) {
        llam_record_fatal(rt, EINVAL);
        return;
    }

    owner = &rt->shards[req->alloc_owner_shard];
    req->next = NULL;
    req->alloc_next = NULL;
    if (g_llam_tls_shard == owner) {
        req->alloc_next = owner->allocator.io_req_free;
        owner->allocator.io_req_free = req;
        owner->allocator.io_req_frees += 1U;
        return;
    }

    do {
        head = atomic_load_explicit(&owner->allocator.io_req_remote_free, memory_order_acquire);
        req->alloc_next = head;
    } while (!atomic_compare_exchange_weak_explicit(&owner->allocator.io_req_remote_free,
                                                     &head,
                                                     req,
                                                     memory_order_release,
                                                     memory_order_acquire));
    atomic_store_explicit(&owner->allocator.remote_free_pending, 1U, memory_order_release);
}

bool llam_io_req_lifetime_release(llam_io_req_t *req) {
    unsigned refs;

    if (req == NULL || req->owner_runtime == NULL) {
        errno = EINVAL;
        return false;
    }
    refs = atomic_load_explicit(&req->lifetime_refs, memory_order_acquire);
    for (;;) {
        if (refs == 0U) {
            /* Double release makes subsequent storage ownership unprovable. */
            abort();
        }
        if (refs == UINT_MAX) {
            /* UINT_MAX is reset publication, never a releasable owner. */
            abort();
        }
        if (atomic_compare_exchange_weak_explicit(&req->lifetime_refs,
                                                  &refs,
                                                  refs == 1U && req->alloc_owner_shard == UINT_MAX
                                                      ? UINT_MAX
                                                      : refs - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            if (refs == 1U) {
                llam_io_req_publish(req);
            }
            return true;
        }
    }
}

bool llam_io_req_backend_event_pin(llam_io_req_t *req) {
    llam_runtime_t *rt = req != NULL ? req->owner_runtime : NULL;
    llam_task_t *task_ref = NULL;
    bool holds_task_ref = false;
    unsigned refs;

    if (req == NULL || rt == NULL) {
        errno = EINVAL;
        return false;
    }
    if (req->alloc_owner_shard == UINT_MAX) {
        task_ref = req->task;
        if (task_ref == NULL || task_ref->owner_runtime != rt || req != &task_ref->embedded_io_req) {
            llam_record_fatal(rt, EINVAL);
            errno = EINVAL;
            return false;
        }
        if (!llam_task_scan_ref_try_acquire(rt, task_ref)) {
            return false;
        }
        holds_task_ref = true;
    }
    if (!llam_io_req_lifetime_try_acquire(req)) {
        if (holds_task_ref) {
            (void)llam_task_scan_ref_release(rt, task_ref);
        }
        return false;
    }
    refs = atomic_load_explicit(&req->backend_event_refs, memory_order_acquire);
    for (;;) {
        if (refs == UINT_MAX) {
            llam_record_fatal(req->owner_runtime, EOVERFLOW);
            (void)llam_io_req_lifetime_release(req);
            if (holds_task_ref) {
                (void)llam_task_scan_ref_release(rt, task_ref);
            }
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(&req->backend_event_refs,
                                                  &refs,
                                                  refs + 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            return true;
        }
    }
}

void llam_io_req_backend_event_unpin(llam_io_req_t *req) {
    llam_runtime_t *rt;
    llam_task_t *task_ref = NULL;
    bool holds_task_ref = false;
    unsigned refs;

    if (req == NULL) {
        return;
    }
    rt = req->owner_runtime;
    if (rt == NULL) {
        return;
    }
    if (req->alloc_owner_shard == UINT_MAX) {
        task_ref = req->task;
        if (task_ref == NULL || task_ref->owner_runtime != rt || req != &task_ref->embedded_io_req) {
            llam_record_fatal(rt, EINVAL);
            return;
        }
        holds_task_ref = true;
    }
    refs = atomic_load_explicit(&req->backend_event_refs, memory_order_acquire);
    while (refs != 0U) {
        if (atomic_compare_exchange_weak_explicit(&req->backend_event_refs,
                                                  &refs,
                                                  refs - 1U,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            (void)llam_io_req_lifetime_release(req);
            if (holds_task_ref) {
                (void)llam_task_scan_ref_release(rt, task_ref);
            }
            return;
        }
    }
    llam_record_fatal(rt, EINVAL);
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
                if (!llam_io_req_lifetime_activate(req)) {
                    return NULL;
                }
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
                if (!llam_io_req_lifetime_activate(req)) {
                    return NULL;
                }
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
    (void)shard;
    (void)llam_io_req_lifetime_release(req);
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
