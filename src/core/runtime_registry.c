/**
 * @file src/core/runtime_registry.c
 * @brief Process-local runtime handle registry and active-op pins.
 *
 * @details
 * Explicit runtime handles are raw pointers for source compatibility.  The
 * registry is the only place that validates those pointers before runtime state
 * is dereferenced, and active-op pins keep host-side operations from racing
 * concurrent runtime destruction.
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
static atomic_uint_fast64_t g_llam_next_runtime_id = 1U;

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

int llam_runtime_register_handle(llam_runtime_t *rt, bool heap_allocated) {
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
    atomic_store_explicit(&rt->destroy_claimed, false, memory_order_release);
    atomic_store_explicit(&rt->active_ops, 0U, memory_order_release);
    rt->registry_next = g_llam_runtime_registry;
    g_llam_runtime_registry = rt;
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    return 0;
}

int llam_runtime_claim_destroy_handle(llam_runtime_t *rt, bool *out_heap_allocated) {
    bool already_claimed;
    size_t active_ops;

    if (rt == NULL || out_heap_allocated == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    if (!llam_runtime_is_registered_locked(rt)) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EINVAL;
        return -1;
    }
    already_claimed = atomic_exchange_explicit(&rt->destroy_claimed, true, memory_order_acq_rel);
    if (already_claimed) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EINVAL;
        return -1;
    }
    *out_heap_allocated = rt->heap_allocated;
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    do {
        active_ops = atomic_load_explicit(&rt->active_ops, memory_order_acquire);
        if (active_ops != 0U) {
            llam_pause_cpu();
        }
    } while (active_ops != 0U);
    return 0;
}

void llam_runtime_unregister_handle(llam_runtime_t *rt) {
    llam_runtime_t **link;

    if (rt == NULL) {
        return;
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    for (link = &g_llam_runtime_registry; *link != NULL; link = &(*link)->registry_next) {
        if (*link == rt) {
            *link = rt->registry_next;
            rt->registry_next = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
}

void llam_runtime_retire_heap_handle(llam_runtime_t *rt) {
    if (rt == NULL) {
        return;
    }

    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    rt->registry_next = g_llam_runtime_retired_handles;
    g_llam_runtime_retired_handles = rt;
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
}

int llam_runtime_check_handle(const llam_runtime_t *runtime) {
    llam_runtime_t *registered;
    bool ok;

    if (runtime == NULL) {
        runtime = llam_runtime_default_storage();
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    registered = llam_runtime_find_registered_locked(runtime);
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
    if (runtime == NULL) {
        runtime = llam_runtime_default_storage();
    }

    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    registered = llam_runtime_find_registered_locked(runtime);
    if (registered == NULL ||
        atomic_load_explicit(&registered->destroy_claimed, memory_order_acquire)) {
        pthread_mutex_unlock(&g_llam_runtime_registry_lock);
        errno = EINVAL;
        return -1;
    }
    (void)atomic_fetch_add_explicit(&registered->active_ops, 1U, memory_order_acq_rel);
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    *out_runtime = registered;
    return 0;
}

void llam_runtime_end_public_op(llam_runtime_t *runtime) {
    size_t previous;

    if (runtime == NULL) {
        return;
    }
    previous = atomic_fetch_sub_explicit(&runtime->active_ops, 1U, memory_order_acq_rel);
    if (LLAM_UNLIKELY(previous == 0U)) {
        (void)atomic_fetch_add_explicit(&runtime->active_ops, 1U, memory_order_release);
    }
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

            if (next_capacity <= capacity || next_capacity > SIZE_MAX / sizeof(*items)) {
                rc = EOVERFLOW;
                break;
            }
            next_items = items == stack_items
                             ? malloc(next_capacity * sizeof(*items))
                             : realloc(items, next_capacity * sizeof(*items));
            if (next_items == NULL) {
                rc = ENOMEM;
                break;
            }
            if (items == stack_items) {
                memcpy(next_items, stack_items, count * sizeof(*next_items));
            }
            items = next_items;
            capacity = next_capacity;
        }
        (void)atomic_fetch_add_explicit(&iter->active_ops, 1U, memory_order_acq_rel);
        items[count++] = iter;
    }
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);

    for (size_t i = 0U; i < count; ++i) {
        fn(items[i], arg);
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
