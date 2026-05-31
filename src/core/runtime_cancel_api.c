/**
 * @file src/core/runtime_cancel_api.c
 * @brief Cancellation token allocation, cancellation, and query operations.
 *
 * @details
 * Cancellation tokens own a lock-protected waiter list of parked tasks. When a
 * token is cancelled, the list is detached under the token lock and each task is
 * resolved through the generic wait cancellation dispatcher. This keeps token
 * state independent from the particular wait primitive that parked the task.
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

static pthread_mutex_t g_llam_cancel_token_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_cancel_token_t *g_llam_cancel_token_registry;
static llam_public_slot_table_t g_llam_cancel_token_public_slots;

/*
 * Cancellation tokens are public heap objects. Public handles use a
 * slot+generation encoding so stale consumed handles cannot cancel or query a
 * replacement token that reuses the same slot or backing storage.
 */
static int llam_cancel_token_reserve_public_slot_locked(llam_cancel_token_t *token, size_t *out_slot) {
    uint32_t generation = 0U;

    return llam_public_slot_reserve_family_secret(&g_llam_cancel_token_public_slots,
                                                  token,
                                                  64U,
                                                  LLAM_PUBLIC_HANDLE_FAMILY_CANCEL_TOKEN,
                                                  token->owner_runtime != NULL
                                                      ? token->owner_runtime->public_handle_secret
                                                      : 0U,
                                                  out_slot,
                                                  &generation);
}

static int llam_cancel_token_register_live(llam_cancel_token_t *token) {
    size_t slot = 0U;

    pthread_mutex_lock(&g_llam_cancel_token_registry_lock);
    if (llam_cancel_token_reserve_public_slot_locked(token, &slot) != 0) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        return -1;
    }
    token->public_handle_slot = slot;
    token->public_handle_generation = llam_public_slot_generation(&g_llam_cancel_token_public_slots, slot);
    token->registry_next = g_llam_cancel_token_registry;
    g_llam_cancel_token_registry = token;
    pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
    return 0;
}

static int llam_cancel_token_begin_op_locked(llam_cancel_token_t *handle, llam_cancel_token_t **out_token) {
    llam_cancel_token_t *token = NULL;

    if (handle == NULL || out_token == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_llam_cancel_token_registry_lock);
    token = llam_public_slot_resolve_encoded(&g_llam_cancel_token_public_slots,
                                             (uintptr_t)handle,
                                             LLAM_CANCEL_TOKEN_PUBLIC_HANDLE_SHIFT,
                                             NULL,
                                             NULL);
    if (token == NULL) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner(token->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        return -1;
    }

    /*
     * Destroy removes the token from the live registry only while holding this
     * same registry lock.  Increment active_ops before taking token->lock so a
     * concurrent destroy sees a live public operation and returns EBUSY instead
     * of freeing the storage underneath it.
     */
    if (llam_public_active_op_try_begin(&token->active_ops) != 0) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        return -1;
    }
    pthread_mutex_lock(&token->lock);
    pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
    *out_token = token;
    return 0;
}

static int llam_cancel_token_begin_op_for_runtime_locked(llam_cancel_token_t *handle,
                                                         llam_runtime_t *owner_runtime,
                                                         llam_cancel_token_t **out_token) {
    llam_cancel_token_t *token = NULL;

    if (handle == NULL || owner_runtime == NULL || out_token == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_llam_cancel_token_registry_lock);
    token = llam_public_slot_resolve_encoded(&g_llam_cancel_token_public_slots,
                                             (uintptr_t)handle,
                                             LLAM_CANCEL_TOKEN_PUBLIC_HANDLE_SHIFT,
                                             NULL,
                                             NULL);
    if (token == NULL) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        errno = EINVAL;
        return -1;
    }
#if !LLAM_RUNTIME_DISABLE_OWNER_CHECKS
    if (token->owner_runtime != owner_runtime) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        errno = EXDEV;
        return -1;
    }
#else
    (void)owner_runtime;
#endif

    /*
     * Spawn publishes the token into a task owned by owner_runtime.  Validate
     * that owner relationship directly instead of using the current TLS owner:
     * host threads can spawn explicit runtimes and managed tasks can target a
     * different runtime through the handle API.
     */
    if (llam_public_active_op_try_begin(&token->active_ops) != 0) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        return -1;
    }
    pthread_mutex_lock(&token->lock);
    pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
    *out_token = token;
    return 0;
}

static void llam_cancel_token_end_op(llam_cancel_token_t *token) {
    llam_public_active_op_end(token != NULL ? &token->active_ops : NULL);
}

static int llam_cancel_token_retain_locked(llam_cancel_token_t *raw_token, llam_cancel_token_t **out_token) {
    /* Saturated task refs are corrupt/exhausted; never wrap them to zero. */
    if (LLAM_UNLIKELY(raw_token->refcount == UINT_MAX)) {
        pthread_mutex_unlock(&raw_token->lock);
        llam_cancel_token_end_op(raw_token);
        errno = EBUSY;
        return -1;
    }
    raw_token->refcount += 1U;
    pthread_mutex_unlock(&raw_token->lock);
    llam_cancel_token_end_op(raw_token);
    if (out_token != NULL) {
        *out_token = raw_token;
    }
    return 0;
}

static void llam_cancel_token_unregister_live_locked(llam_cancel_token_t *token) {
    llam_cancel_token_t **link = &g_llam_cancel_token_registry;

    if (token->public_handle_slot < g_llam_cancel_token_public_slots.count) {
        llam_public_slot_release(&g_llam_cancel_token_public_slots,
                                 token->public_handle_slot,
                                 token,
                                 token->public_handle_generation);
    }

    while (*link != NULL) {
        if (*link == token) {
            *link = token->registry_next;
            token->registry_next = NULL;
            return;
        }
        link = &(*link)->registry_next;
    }
}

/** @brief Return true when @p task_class is a supported public task class. */
static bool llam_public_task_class_valid(uint32_t task_class) {
    return task_class == (uint32_t)LLAM_TASK_CLASS_LATENCY ||
           task_class == (uint32_t)LLAM_TASK_CLASS_DEFAULT ||
           task_class == (uint32_t)LLAM_TASK_CLASS_BATCH;
}

/**
 * @brief Set the scheduling class for the current task.
 *
 * Calls outside a managed task fail because there is no current task to mutate.
 *
 * @param task_class New class to store on the current task.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c ENOTSUP.
 */
int llam_task_set_class(uint32_t task_class) {
    if (!llam_public_task_class_valid(task_class)) {
        errno = EINVAL;
        return -1;
    }

    llam_task_safepoint();
    if (g_llam_tls_task == NULL) {
        errno = ENOTSUP;
        return -1;
    }
    atomic_store_explicit(&g_llam_tls_task->task_class, task_class, memory_order_release);
    atomic_store_explicit(&g_llam_tls_task->base_task_class, task_class, memory_order_release);
    return 0;
}

/**
 * @brief Return the flag word associated with a task handle.
 *
 * @param task Task to inspect.
 *
 * @return Task flags, or 0 for invalid, foreign, or @c NULL handles.
 */
uint32_t llam_task_flags(const llam_task_t *task) {
    uint32_t flags = 0U;
    int saved_errno = errno;

    task = llam_task_resolve_public_handle(task);
    if (task != NULL) {
        if (llam_runtime_check_object_owner(task->owner_runtime) == 0) {
            flags = (uint32_t)task->flags;
        } else {
            errno = saved_errno;
        }
        llam_task_end_public_op((llam_task_t *)task);
    }
    return flags;
}

/**
 * @brief Allocate a cancellation token.
 *
 * @return New token on success, or @c NULL with @c errno set on failure.
 */
llam_cancel_token_t *llam_cancel_token_create(void) {
    llam_cancel_token_t *token = calloc(1, sizeof(*token));
    int rc;

    if (token == NULL) {
        return NULL;
    }
    rc = pthread_mutex_init(&token->lock, NULL);
    if (rc != 0) {
        free(token);
        // pthread_mutex_init reports the error through its return value.
        errno = rc;
        return NULL;
    }
    token->owner_runtime = llam_runtime_owner_for_new_object();
    llam_public_active_op_init(&token->active_ops);
    if (llam_cancel_token_register_live(token) != 0) {
        pthread_mutex_destroy(&token->lock);
        free(token);
        return NULL;
    }
    return llam_cancel_token_public_handle(token);
}

/**
 * @brief Destroy a cancellation token.
 *
 * Tokens cannot be destroyed while tasks are still registered as waiters or
 * while external references are recorded through the token refcount.
 *
 * @param token Token to destroy.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL or @c EBUSY.
 */
int llam_cancel_token_destroy(llam_cancel_token_t *token) {
    uintptr_t handle = (uintptr_t)token;
    size_t slot;
    uint32_t generation;

    pthread_mutex_lock(&g_llam_cancel_token_registry_lock);
    token = llam_public_slot_resolve_encoded(&g_llam_cancel_token_public_slots,
                                             handle,
                                             LLAM_CANCEL_TOKEN_PUBLIC_HANDLE_SHIFT,
                                             &slot,
                                             &generation);
    if (token == NULL ||
        token->public_handle_slot != slot ||
        token->public_handle_generation != generation) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        errno = EINVAL;
        return -1;
    }
    if (llam_runtime_check_object_owner_for_cleanup(token->owner_runtime) != 0) {
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        return -1;
    }
    pthread_mutex_lock(&token->lock);
    if (token->waiters != NULL || token->refcount != 0U ||
        llam_public_active_op_count(&token->active_ops) != 0U) {
        pthread_mutex_unlock(&token->lock);
        pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
        errno = EBUSY;
        return -1;
    }
    llam_cancel_token_unregister_live_locked(token);
    /*
     * The token has been removed from the live handle table while the registry
     * lock is still held, so no new public operation can acquire token->lock.
     * Unlock before pthread_mutex_destroy: destroying a locked POSIX mutex is
     * undefined even when no other waiter exists.
     */
    pthread_mutex_unlock(&token->lock);
    pthread_mutex_destroy(&token->lock);
    free(token);
    pthread_mutex_unlock(&g_llam_cancel_token_registry_lock);
    return 0;
}

int llam_cancel_token_retain_task_ref(llam_cancel_token_t *token, llam_cancel_token_t **out_token) {
    llam_cancel_token_t *raw_token = NULL;

    if (out_token != NULL) {
        *out_token = NULL;
    }
    if (llam_cancel_token_begin_op_locked(token, &raw_token) != 0) {
        return -1;
    }
    return llam_cancel_token_retain_locked(raw_token, out_token);
}

int llam_cancel_token_retain_task_ref_for_runtime(llam_cancel_token_t *token,
                                                  llam_runtime_t *owner_runtime,
                                                  llam_cancel_token_t **out_token) {
    llam_cancel_token_t *raw_token = NULL;

    if (out_token != NULL) {
        *out_token = NULL;
    }
    if (llam_cancel_token_begin_op_for_runtime_locked(token, owner_runtime, &raw_token) != 0) {
        return -1;
    }
    return llam_cancel_token_retain_locked(raw_token, out_token);
}

void llam_cancel_token_release_task_ref(llam_cancel_token_t *token) {
    if (token == NULL) {
        return;
    }

    /*
     * The caller owns a task reference, so destroy cannot free the token until
     * this refcount is dropped.  A direct token lock is therefore sufficient and
     * avoids registry traffic on every task reclamation path.
     */
    pthread_mutex_lock(&token->lock);
    if (token->refcount > 0U) {
        token->refcount -= 1U;
    }
    pthread_mutex_unlock(&token->lock);
}

/**
 * @brief Check whether a task's retained cancellation token is already set.
 *
 * @details
 * Task spawn retains @c task->cancel_token for the task lifetime, so internal
 * wait setup can inspect the raw token under its lock without going through the
 * public handle registry. This is used immediately before parking paths publish
 * a wait: if cancellation won just before registration, the wait should fail
 * inline with @c ECANCELED instead of depending on a backend wake that may never
 * be armed.
 */
bool llam_task_cancel_token_is_cancelled(const llam_task_t *task) {
    llam_cancel_token_t *token;
    bool cancelled;

    if (task == NULL || task->cancel_token == NULL) {
        return false;
    }

    token = task->cancel_token;
    pthread_mutex_lock(&token->lock);
    cancelled = token->cancelled;
    pthread_mutex_unlock(&token->lock);
    return cancelled;
}

/**
 * @brief Cancel a token and wake all currently registered waiters.
 *
 * Waiters are detached while holding the token lock, then cancelled after the
 * lock is released. The per-wait cancellation dispatcher removes each task from
 * its owning wait structure before reinjecting it.
 *
 * @param token Token to cancel.
 *
 * @return 0 on success, or -1 with @c errno set to @c EINVAL.
 */
int llam_cancel_token_cancel(llam_cancel_token_t *token) {
    llam_task_t *waiters;
    llam_task_t *iter;
    llam_cancel_token_t *raw_token = NULL;
    llam_runtime_t *pinned_runtime = NULL;

    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (llam_cancel_token_begin_op_locked(token, &raw_token) != 0) {
        return -1;
    }
    token = raw_token;
    if (token->cancelled) {
        pthread_mutex_unlock(&token->lock);
        llam_cancel_token_end_op(token);
        return 0;
    }
    if (token->waiters != NULL &&
        llam_runtime_begin_live_object_owner_op(token->owner_runtime, &pinned_runtime, ENOTSUP) != 0) {
        pthread_mutex_unlock(&token->lock);
        llam_cancel_token_end_op(token);
        return -1;
    }
    token->cancelled = true;
    waiters = token->waiters;
    token->waiters = NULL;
    for (iter = waiters; iter != NULL; iter = iter->cancel_next) {
        /*
         * Mark the entire detached chain unregistered while still holding the
         * token lock.  Otherwise a concurrently completing wait can call
         * unregister on a node that is no longer in token->waiters and corrupt
         * either token->waiters or this detached cancellation chain.
         *
         * The chain is processed after releasing the token lock. Hold a short
         * scan reference so a naturally completing detached task cannot be
         * reclaimed while this cancel call still owns its raw list pointer.
         */
        atomic_fetch_add_explicit(&iter->scan_refs, 1U, memory_order_acq_rel);
        iter->cancel_prev = NULL;
        iter->cancel_registered = false;
    }
    pthread_mutex_unlock(&token->lock);

    while (waiters != NULL) {
        llam_task_t *next = waiters->cancel_next;

        waiters->cancel_next = NULL;
        llam_cancel_task_wait(waiters);
        atomic_fetch_sub_explicit(&waiters->scan_refs, 1U, memory_order_acq_rel);
        waiters = next;
    }

    llam_runtime_end_public_op(pinned_runtime);
    llam_cancel_token_end_op(token);
    return 0;
}

/**
 * @brief Query cancellation state for a token.
 *
 * @param token Token to inspect.
 *
 * @return 1 if cancelled, 0 if not cancelled, or -1 with @c errno set to
 *         @c EINVAL for @c NULL input.
 */
int llam_cancel_token_is_cancelled(const llam_cancel_token_t *token) {
    int cancelled;
    llam_cancel_token_t *mutable_token;

    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (llam_cancel_token_begin_op_locked((llam_cancel_token_t *)token, &mutable_token) != 0) {
        return -1;
    }
    cancelled = mutable_token->cancelled ? 1 : 0;
    pthread_mutex_unlock(&mutable_token->lock);
    llam_cancel_token_end_op(mutable_token);
    return cancelled;
}
