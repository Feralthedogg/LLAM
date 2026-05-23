/**
 * @file src/core/runtime_owner.c
 * @brief Runtime-owner diagnostics for the multi-runtime migration boundary.
 *
 * @details
 * Runtime-aware objects record the runtime that created them.  These helpers
 * provide a cheap owner check for sync, task, cancellation, and I/O objects so
 * explicit runtime handles cannot accidentally share wait queues, caches, or
 * backend completions across owner domains.  The legacy default-runtime path is
 * still intentionally fast because channel/select hot paths call this helper.
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
static atomic_uint_fast64_t g_llam_next_runtime_id = 1U;

llam_runtime_t *llam_runtime_default_storage(void) {
    return &g_llam_runtime;
}

static bool llam_runtime_is_registered_locked(const llam_runtime_t *runtime) {
    const llam_runtime_t *iter;

    for (iter = g_llam_runtime_registry; iter != NULL; iter = iter->registry_next) {
        if (iter == runtime) {
            return true;
        }
    }
    return false;
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
    rt->heap_allocated = heap_allocated;
    rt->registry_next = g_llam_runtime_registry;
    g_llam_runtime_registry = rt;
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
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

/**
 * @brief Validate a runtime handle accepted by public lifecycle APIs.
 *
 * True multi-runtime mode must never dereference an arbitrary caller pointer
 * before it has been observed in the live runtime registry. Keep this validation
 * centralized so unknown runtime handles fail consistently.
 */
int llam_runtime_check_handle(const llam_runtime_t *runtime) {
    bool ok;

    if (runtime == NULL) {
        runtime = llam_runtime_default_storage();
    }
    pthread_mutex_lock(&g_llam_runtime_registry_lock);
    ok = llam_runtime_is_registered_locked(runtime);
    pthread_mutex_unlock(&g_llam_runtime_registry_lock);
    if (LLAM_UNLIKELY(!ok)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/**
 * @brief Return the runtime associated with the current managed context.
 *
 * The shard is the most reliable owner cursor while a worker is running.  The
 * task field is a fallback for direct task-to-task handoff windows where the
 * task cursor has been installed before every helper has sampled the shard.
 */
llam_runtime_t *llam_runtime_current_owner(void) {
    if (LLAM_LIKELY(g_llam_tls_shard != NULL && g_llam_tls_shard->runtime != NULL)) {
        return g_llam_tls_shard->runtime;
    }
    if (LLAM_UNLIKELY(g_llam_tls_task != NULL && g_llam_tls_task->owner_runtime != NULL)) {
        return g_llam_tls_task->owner_runtime;
    }
    return llam_runtime_default_storage();
}

/**
 * @brief Return the owner to stamp onto a newly created runtime-aware object.
 */
llam_runtime_t *llam_runtime_owner_for_new_object(void) {
    return llam_runtime_current_owner();
}

/**
 * @brief Validate that an object belongs to the current runtime context.
 *
 * Managed tasks must use objects owned by their current runtime.  Host-thread
 * cleanup paths may touch any registered owner because they have no managed
 * runtime context; unknown non-NULL owners are treated as cross-runtime or
 * stale-owner misuse and fail with @c EXDEV without dereferencing the pointer.
 */
int llam_runtime_check_object_owner(const llam_runtime_t *owner_runtime) {
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    llam_runtime_t *current;

    if (LLAM_UNLIKELY(owner_runtime == NULL)) {
        errno = EINVAL;
        return -1;
    }
    current = llam_runtime_current_owner();
    if (LLAM_LIKELY(g_llam_tls_task != NULL || g_llam_tls_shard != NULL)) {
        if (LLAM_LIKELY(owner_runtime == current)) {
            return 0;
        }
        errno = EXDEV;
        return -1;
    }
    if (owner_runtime == llam_runtime_default_storage()) {
        return 0;
    }
    if (llam_runtime_check_handle(owner_runtime) == 0) {
        return 0;
    }
    /*
     * Object owner checks diagnose cross-runtime misuse.  A non-NULL owner that
     * is not in the live runtime registry must not be dereferenced, but it is
     * still an owner-domain mismatch from the caller's point of view.
     */
    errno = EXDEV;
    return -1;
#else
    if (LLAM_UNLIKELY(owner_runtime == NULL)) {
        errno = EINVAL;
        return -1;
    }
    (void)owner_runtime;
#endif
    return 0;
}

/**
 * @brief Require both managed-task context and matching object runtime owner.
 */
int llam_runtime_require_object_owner(const llam_runtime_t *owner_runtime) {
    if (llam_require_task_context() != 0) {
        return -1;
    }
    return llam_runtime_check_object_owner(owner_runtime);
}
