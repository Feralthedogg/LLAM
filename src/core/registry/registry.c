/**
 * @file src/core/registry/registry.c
 * @brief Process-local runtime handle registry and active-op pins.
 *
 * @details
 * Explicit runtime handles are encoded slot+generation tokens.  The registry
 * resolves those tokens before runtime state is dereferenced, and active-op
 * pins keep host-side operations from racing concurrent runtime destruction.
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

static pthread_mutex_t g_llam_runtime_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_runtime_t *g_llam_runtime_registry;
static llam_runtime_t *g_llam_runtime_retired_handles;
static llam_public_slot_table_t g_llam_runtime_public_slots;
static atomic_uint_fast64_t g_llam_next_runtime_id = 1U;

#if defined(LLAM_ENABLE_TEST_HOOKS)
static atomic_bool g_llam_runtime_test_live_iter_snapshot_alloc_failure;

void llam_runtime_test_force_live_iter_snapshot_alloc_failure(bool enabled) {
    atomic_store_explicit(&g_llam_runtime_test_live_iter_snapshot_alloc_failure,
                          enabled,
                          memory_order_release);
}

static bool llam_runtime_live_iter_snapshot_alloc_should_fail(void) {
    return atomic_load_explicit(&g_llam_runtime_test_live_iter_snapshot_alloc_failure,
                                memory_order_acquire);
}
#else
static bool llam_runtime_live_iter_snapshot_alloc_should_fail(void) {
    return false;
}
#endif

llam_runtime_t *llam_runtime_default_storage(void) {
    return &g_llam_runtime;
}

static llam_runtime_t *llam_runtime_find_registered_locked(const llam_runtime_t *runtime) {
    for (llam_runtime_t *iter = g_llam_runtime_registry; iter != NULL; iter = iter->registry_next) {
        if (iter == runtime) {
            return iter;
        }
    }
    return NULL;
}

static bool llam_runtime_is_registered_locked(const llam_runtime_t *runtime) {
    return llam_runtime_find_registered_locked(runtime) != NULL;
}

static bool llam_runtime_remove_retired_locked(llam_runtime_t *runtime) {
    llam_runtime_t **link;

    for (link = &g_llam_runtime_retired_handles;
         *link != NULL;
         link = &(*link)->registry_next) {
        if (*link == runtime) {
            *link = runtime->registry_next;
            runtime->registry_next = NULL;
            return true;
        }
    }
    return false;
}

static bool llam_runtime_is_retired_locked(
    const llam_runtime_t *runtime) {
    const llam_runtime_t *current;

    for (current = g_llam_runtime_retired_handles;
         current != NULL;
         current = current->registry_next) {
        if (current == runtime) {
            return true;
        }
    }
    return false;
}

static bool llam_runtime_unregister_handle_locked(
    llam_runtime_t *runtime) {
    llam_runtime_t **link;

    for (link = &g_llam_runtime_registry;
         *link != NULL;
         link = &(*link)->registry_next) {
        if (*link == runtime) {
            *link = runtime->registry_next;
            runtime->registry_next = NULL;
            if (runtime->public_handle_slot != SIZE_MAX &&
                runtime->public_handle_generation != 0U) {
                llam_public_slot_release(
                    &g_llam_runtime_public_slots,
                    runtime->public_handle_slot,
                    runtime,
                    runtime->public_handle_generation);
                runtime->public_handle_slot = SIZE_MAX;
                runtime->public_handle_generation = 0U;
            }
            return true;
        }
    }
    return false;
}

static llam_runtime_t *llam_runtime_resolve_handle_locked(
    const llam_runtime_t *handle) {
    llam_runtime_t *runtime;

    runtime = llam_runtime_find_registered_locked(handle);
    if (runtime != NULL) {
        return runtime;
    }
    runtime = llam_public_slot_resolve_encoded(
        &g_llam_runtime_public_slots,
        (uintptr_t)handle,
        LLAM_RUNTIME_PUBLIC_HANDLE_SHIFT,
        NULL,
        NULL);
    if (runtime == NULL ||
        llam_runtime_find_registered_locked(runtime) != runtime) {
        return NULL;
    }
    return runtime;
}

llam_runtime_t *llam_runtime_public_handle(llam_runtime_t *runtime) {
    uintptr_t encoded;

    if (runtime == NULL ||
        runtime == llam_runtime_default_storage()) {
        return runtime;
    }
    encoded = llam_public_slot_encode_handle(
        runtime->public_handle_slot,
        runtime->public_handle_generation,
        LLAM_RUNTIME_PUBLIC_HANDLE_SHIFT);
    return encoded != 0U ? (llam_runtime_t *)encoded : NULL;
}

int llam_runtime_reset_storage_for_init(llam_runtime_t *runtime) {
    size_t public_owner_refs = 0U;
    int rc = 0;

    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    if (llam_runtime_is_registered_locked(runtime) ||
        llam_runtime_is_retired_locked(runtime)) {
        /* A live or retired heap runtime is not an initialization target. */
        rc = EBUSY;
    } else if (runtime == llam_runtime_default_storage() &&
               runtime->public_owner_refs != 0U &&
               atomic_load_explicit(&runtime->destroy_claimed,
                                    memory_order_acquire)) {
        /*
         * Surviving cleanup objects from the previous default-runtime
         * incarnation keep its stable raw storage from aliasing a new
         * scheduler.
         */
        rc = EBUSY;
    } else {
        if (runtime == llam_runtime_default_storage()) {
            public_owner_refs = runtime->public_owner_refs;
        }
        /*
         * Public owner acquire/release also holds the registry lock, so the
         * preserved count cannot race this storage reset.
         */
        memset(runtime, 0, sizeof(*runtime));
        runtime->public_handle_slot = SIZE_MAX;
        runtime->public_owner_refs = public_owner_refs;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

int llam_runtime_public_owner_acquire(llam_runtime_t *runtime) {
    bool default_preinit;
    int rc = 0;

    if (runtime == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    /*
     * Once every object from the previous default-runtime incarnation has
     * drained, the stable default storage may start a fresh pre-init owner
     * epoch. Objects created in that epoch belong to the next initialization;
     * a surviving prior-incarnation object keeps destroy_claimed set and
     * therefore still prevents stale-owner aliasing.
     */
    if (runtime == llam_runtime_default_storage() &&
        !llam_runtime_is_registered_locked(runtime) &&
        runtime->runtime_id == 0U &&
        runtime->public_owner_refs == 0U &&
        atomic_load_explicit(&runtime->destroy_claimed,
                             memory_order_acquire)) {
        atomic_store_explicit(&runtime->destroy_claimed,
                              false,
                              memory_order_release);
    }
    default_preinit =
        runtime == llam_runtime_default_storage() &&
        !llam_runtime_is_registered_locked(runtime) &&
        runtime->runtime_id == 0U &&
        !atomic_load_explicit(&runtime->destroy_claimed,
                              memory_order_acquire);
    if ((!llam_runtime_is_registered_locked(runtime) && !default_preinit) ||
        atomic_load_explicit(&runtime->destroy_claimed,
                             memory_order_acquire)) {
        rc = EINVAL;
    } else if (runtime->public_owner_refs == SIZE_MAX) {
        rc = EOVERFLOW;
    } else {
        runtime->public_owner_refs += 1U;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}

void llam_runtime_public_owner_release(llam_runtime_t *runtime) {
    bool free_runtime = false;
    bool known_runtime;

    if (runtime == NULL) {
        return;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    known_runtime =
        runtime == llam_runtime_default_storage() ||
        llam_runtime_is_registered_locked(runtime) ||
        llam_runtime_remove_retired_locked(runtime);
    if (!known_runtime) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        return;
    }
    if (runtime->public_owner_refs == 0U) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        abort();
    }
    runtime->public_owner_refs -= 1U;
    if (runtime->public_owner_refs == 0U &&
        runtime->retired_storage &&
        runtime->heap_allocated) {
        runtime->retired_storage = false;
        free_runtime = true;
    } else if (runtime->retired_storage &&
               runtime->heap_allocated) {
        runtime->registry_next = g_llam_runtime_retired_handles;
        g_llam_runtime_retired_handles = runtime;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    if (free_runtime) {
        llam_aligned_free(runtime);
    }
}

uint64_t llam_runtime_public_owner_secret(
    const llam_runtime_t *runtime) {
    uint64_t secret = 0U;

    if (runtime == NULL) {
        return 0U;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    if (runtime == llam_runtime_default_storage() ||
        llam_runtime_is_registered_locked(runtime) ||
        llam_runtime_is_retired_locked(runtime)) {
        secret = runtime->public_handle_secret;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    return secret;
}

int llam_runtime_register_handle(llam_runtime_t *rt, bool heap_allocated) {
    size_t public_slot = SIZE_MAX;
    uint32_t public_generation = 0U;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    if (llam_runtime_is_registered_locked(rt)) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EBUSY;
        return -1;
    }
    rt->runtime_id = atomic_fetch_add_explicit(&g_llam_next_runtime_id, 1U, memory_order_relaxed);
    if (rt->runtime_id == 0U) {
        rt->runtime_id = atomic_fetch_add_explicit(&g_llam_next_runtime_id, 1U, memory_order_relaxed);
        if (rt->runtime_id == 0U) {
            pthread_mutex_unlock(&g_llam_runtime_registry_lock);
            errno = EOVERFLOW;
            return -1;
        }
    }
    {
        uint64_t secret = 0U;

        if (!llam_public_slot_entropy_from_os(&secret)) {
            secret = 0U;
        }
        secret ^= llam_public_slot_fallback_secret(rt, &rt->runtime_id, rt->runtime_id);
        rt->public_handle_secret = llam_public_slot_mix64(secret ^ rt->runtime_id);
        if (rt->public_handle_secret == 0U) {
            rt->public_handle_secret = UINT64_C(0xd6e8feb86659fd93);
        }
    }
    rt->heap_allocated = heap_allocated;
    rt->retired_storage = false;
    atomic_store_explicit(&rt->destroy_claimed, false, memory_order_release);
    atomic_store_explicit(&rt->active_ops, 0U, memory_order_release);
    rt->public_handle_slot = SIZE_MAX;
    rt->public_handle_generation = 0U;
    if (heap_allocated &&
        llam_public_slot_reserve_family_secret(
            &g_llam_runtime_public_slots,
            rt,
            64U,
            LLAM_PUBLIC_HANDLE_FAMILY_RUNTIME,
            rt->public_handle_secret,
            &public_slot,
            &public_generation) != 0) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        return -1;
    }
    rt->public_handle_slot = public_slot;
    rt->public_handle_generation = public_generation;
    rt->registry_next = g_llam_runtime_registry;
    g_llam_runtime_registry = rt;
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    return 0;
}

int llam_runtime_claim_destroy_handle(llam_runtime_t *handle,
                                      llam_runtime_t **out_runtime,
                                      bool *out_heap_allocated) {
    llam_runtime_t *rt;
    bool already_claimed;
    bool heap_allocated;
    size_t active_ops;

    if (handle == NULL ||
        out_runtime == NULL ||
        out_heap_allocated == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_runtime = NULL;
    *out_heap_allocated = false;
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    rt = llam_runtime_resolve_handle_locked(handle);
    if (rt == NULL) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EINVAL;
        return -1;
    }
    active_ops = atomic_load_explicit(&rt->active_ops, memory_order_acquire);
    if (LLAM_UNLIKELY(llam_public_active_op_is_saturated(active_ops))) {
        /*
         * Reserved/sentinel counts are corrupted or exhausted active-op state.
         * Destroy must fail closed before claiming the handle; otherwise it
         * would spin forever waiting for the permanent sentinel to become zero.
         */
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EBUSY;
        return -1;
    }
    already_claimed = atomic_exchange_explicit(&rt->destroy_claimed, true, memory_order_acq_rel);
    if (already_claimed) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EINVAL;
        return -1;
    }
    heap_allocated = rt->heap_allocated;
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    /*
     * Existing host-side public operations may be parked in a no-deadline
     * runtime-owned wait while holding active_ops. Publish stop before waiting
     * for the counter to drain so those operations can observe cancellation and
     * release their lifecycle pin.
     */
    llam_request_stop(rt);
    do {
        active_ops = atomic_load_explicit(&rt->active_ops, memory_order_acquire);
        if (LLAM_UNLIKELY(llam_public_active_op_is_saturated(active_ops))) {
            /*
             * No new public op can start after destroy_claimed is published, so
             * sentinel space here means the active-op counter was corrupted while
             * destroy was waiting. Release the claim and fail closed instead of
             * burning a host thread forever.
             */
            atomic_store_explicit(&rt->destroy_claimed, false, memory_order_release);
            errno = EBUSY;
            return -1;
        }
        if (active_ops != 0U) {
            llam_pause_cpu();
        }
    } while (active_ops != 0U);
    *out_runtime = rt;
    *out_heap_allocated = heap_allocated;
    return 0;
}

void llam_runtime_unregister_handle(llam_runtime_t *rt) {
    if (rt == NULL) {
        return;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    (void)llam_runtime_unregister_handle_locked(rt);
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
}

void llam_runtime_finalize_handle(
    llam_runtime_t *rt,
    bool retire_heap_storage) {
    bool free_runtime = false;
    bool heap_allocated;
    size_t public_owner_refs;
    uint64_t public_handle_secret;

    if (rt == NULL) {
        return;
    }

    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    if (!llam_runtime_unregister_handle_locked(rt)) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        return;
    }
    heap_allocated = rt->heap_allocated;
    public_owner_refs = rt->public_owner_refs;
    public_handle_secret = rt->public_handle_secret;
    /*
     * Object releases also serialize on the registry lock. Removing the live
     * token, preserving the owner count, clearing scheduler/backend state, and
     * publishing retired storage in one critical section leaves no
     * unregistered/unretired gap in which a cleanup decrement can be lost.
     */
    memset(rt, 0, sizeof(*rt));
    rt->public_handle_slot = SIZE_MAX;
    rt->public_owner_refs = public_owner_refs;
    rt->public_handle_secret = public_handle_secret;
    rt->heap_allocated = heap_allocated;
    atomic_init(&rt->destroy_claimed, true);
    if (retire_heap_storage && heap_allocated &&
        public_owner_refs != 0U) {
        rt->retired_storage = true;
        rt->registry_next = g_llam_runtime_retired_handles;
        g_llam_runtime_retired_handles = rt;
    } else if (retire_heap_storage && heap_allocated) {
        free_runtime = true;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    if (free_runtime) {
        llam_aligned_free(rt);
    }
}

int llam_runtime_check_handle(const llam_runtime_t *runtime) {
    llam_runtime_t *registered;
    bool ok;

    if (runtime == NULL) {
        runtime = llam_runtime_default_storage();
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    registered = llam_runtime_resolve_handle_locked(runtime);
    ok = registered != NULL;
    if (ok && atomic_load_explicit(&registered->destroy_claimed, memory_order_acquire)) {
        ok = false;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    if (LLAM_UNLIKELY(!ok)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int llam_runtime_begin_public_op(llam_runtime_t *runtime, llam_runtime_t **out_runtime) {
    llam_runtime_t *registered;

    if (out_runtime == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_runtime = NULL;
    if (runtime == NULL) {
        runtime = llam_runtime_default_storage();
    }

    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    registered = llam_runtime_resolve_handle_locked(runtime);
    if (registered == NULL ||
        atomic_load_explicit(&registered->destroy_claimed, memory_order_acquire)) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (LLAM_UNLIKELY(llam_public_active_op_try_begin(&registered->active_ops) != 0)) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    *out_runtime = registered;
    return 0;
}

void llam_runtime_end_public_op(llam_runtime_t *runtime) {
    if (runtime == NULL) {
        return;
    }
    llam_public_active_op_end(&runtime->active_ops);
}

static llam_runtime_t **llam_runtime_live_iter_snapshot_grow(llam_runtime_t **items,
                                                             llam_runtime_t **stack_items,
                                                             size_t count,
                                                             size_t next_capacity) {
    llam_runtime_t **next_items;

    if (llam_runtime_live_iter_snapshot_alloc_should_fail()) {
        errno = ENOMEM;
        return NULL;
    }
    next_items = items == stack_items
                     ? malloc(next_capacity * sizeof(*items))
                     : realloc(items, next_capacity * sizeof(*items));
    if (next_items != NULL && items == stack_items) {
        memcpy(next_items, stack_items, count * sizeof(*next_items));
    }
    return next_items;
}

int llam_runtime_for_each_live(llam_runtime_live_iter_fn fn, void *arg) {
    enum { LLAM_RUNTIME_STACK_SNAPSHOT = 16 };
    llam_runtime_t *stack_items[LLAM_RUNTIME_STACK_SNAPSHOT];
    llam_runtime_t **items = stack_items;
    size_t count = 0U;
    size_t capacity = LLAM_RUNTIME_STACK_SNAPSHOT;
    int rc = 0;

    if (fn == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    for (llam_runtime_t *iter = g_llam_runtime_registry; iter != NULL; iter = iter->registry_next) {
        llam_runtime_t **next_items;

        if (atomic_load_explicit(&iter->destroy_claimed, memory_order_acquire)) {
            continue;
        }
        if (count == capacity) {
            size_t next_capacity = capacity * 2U;

            /*
             * Grow the snapshot before pinning this runtime. If the cold OOM
             * path fails after a successful pin, the runtime destroy gate can
             * stay permanently busy even though no callback will receive it.
             */
            if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*items)) {
                rc = EOVERFLOW;
                break;
            }
            next_items = llam_runtime_live_iter_snapshot_grow(items,
                                                              stack_items,
                                                              count,
                                                              next_capacity);
            if (next_items == NULL) {
                rc = errno != 0 ? errno : ENOMEM;
                break;
            }
            items = next_items;
            capacity = next_capacity;
        }
        if (LLAM_UNLIKELY(llam_public_active_op_try_begin(&iter->active_ops) != 0)) {
            rc = errno;
            break;
        }
        items[count++] = iter;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);

    for (size_t i = 0U; i < count; ++i) {
        /*
         * Snapshot construction must be all-or-nothing.  If allocation fails
         * while collecting live runtimes, applying the callback to the prefix
         * would create partial side effects even though the API reports failure.
         * Release acquired pins only; the caller can retry with a complete
         * snapshot.
         */
        if (rc == 0) {
            fn(items[i], arg);
        }
        llam_runtime_end_public_op(items[i]);
    }
    if (items != stack_items) {
        free(items);
    }
    if (rc != 0) {
        errno = rc;
        return -1;
    }
    return 0;
}
