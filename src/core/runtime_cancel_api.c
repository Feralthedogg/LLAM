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

/**
 * @brief Set the scheduling class for the current task.
 *
 * Calls outside a managed task are no-ops.
 *
 * @param task_class New class to store on the current task.
 */
void nm_task_set_class(nm_task_class_t task_class) {
    nm_task_safepoint();
    if (g_nm_tls_task != NULL) {
        g_nm_tls_task->task_class = task_class;
    }
}

/**
 * @brief Return the flag word associated with a task handle.
 *
 * @param task Task to inspect.
 *
 * @return Task flags, or 0 for @c NULL.
 */
unsigned nm_task_flags(const nm_task_t *task) {
    return task != NULL ? task->flags : 0U;
}

/**
 * @brief Allocate a cancellation token.
 *
 * @return New token on success, or @c NULL with @c errno set on failure.
 */
nm_cancel_token_t *nm_cancel_token_create(void) {
    nm_cancel_token_t *token = calloc(1, sizeof(*token));

    if (token == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&token->lock, NULL) != 0) {
        free(token);
        errno = ENOMEM;
        return NULL;
    }
    return token;
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
int nm_cancel_token_destroy(nm_cancel_token_t *token) {
    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&token->lock);
    if (token->waiters != NULL || token->refcount != 0U) {
        pthread_mutex_unlock(&token->lock);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&token->lock);
    pthread_mutex_destroy(&token->lock);
    free(token);
    return 0;
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
int nm_cancel_token_cancel(nm_cancel_token_t *token) {
    nm_task_t *waiters;

    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&token->lock);
    if (token->cancelled) {
        pthread_mutex_unlock(&token->lock);
        return 0;
    }
    token->cancelled = true;
    waiters = token->waiters;
    token->waiters = NULL;
    if (waiters != NULL) {
        waiters->cancel_prev = NULL;
    }
    pthread_mutex_unlock(&token->lock);

    while (waiters != NULL) {
        nm_task_t *next = waiters->cancel_next;

        waiters->cancel_prev = NULL;
        waiters->cancel_next = NULL;
        waiters->cancel_registered = false;
        nm_cancel_task_wait(waiters);
        waiters = next;
    }

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
int nm_cancel_token_is_cancelled(const nm_cancel_token_t *token) {
    int cancelled;
    nm_cancel_token_t *mutable_token;

    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    mutable_token = (nm_cancel_token_t *)token;
    pthread_mutex_lock(&mutable_token->lock);
    cancelled = mutable_token->cancelled ? 1 : 0;
    pthread_mutex_unlock(&mutable_token->lock);
    return cancelled;
}
