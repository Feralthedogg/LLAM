/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */
#include "structured_internal.h"

int llam_task_set_outcome_ex(const llam_outcome_t *input, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_outcome_t outcome;
    if (llam_scope_copy_input(&outcome, sizeof(outcome), input, size) != 0) return -1;
    if (outcome.kind < LLAM_OUTCOME_OK || outcome.kind > LLAM_OUTCOME_CANCELLED ||
        outcome.runtime_error != 0 || (outcome.kind == LLAM_OUTCOME_CANCELLED && outcome.value != NULL)) {
        errno = EINVAL;
        return -1;
    }
    llam_task_t *task = g_llam_tls_task;
    if (task == NULL || task->scope_completion == NULL) { errno = ENOTSUP; return -1; }
    llam_future_t *future = task->scope_completion;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = -1;
    if (future->kind != 0U) errno = EBUSY;
    else if (outcome.kind == LLAM_OUTCOME_CANCELLED) result = 0;
    else {
        const llam_type_descriptor_t *type = outcome.kind == LLAM_OUTCOME_OK
                                               ? &future->contract.success : &future->contract.error;
        result = llam_value_take_locked(outcome.value, type, &future->result);
    }
    if (result == 0) future->kind = outcome.kind;
    pthread_mutex_unlock(&g_llam_structured_lock);
    return result;
}

void llam_scoped_task_entry_returned(llam_task_t *task) {
    if (task->scope_completion == NULL) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    task->scope_completion->entry_returned = true;
    pthread_mutex_unlock(&g_llam_structured_lock);
}

void llam_scoped_task_prepare_retire(llam_task_t *task) {
    llam_future_t *future = task->scope_completion;
    if (future == NULL) return;
    bool cancelled = llam_task_cancel_token_is_cancelled(task);
    int fatal = atomic_load_explicit(&task->owner_runtime->fatal_errno, memory_order_acquire);
    pthread_mutex_lock(&g_llam_structured_lock);
    if (!future->entry_returned || future->kind == 0U) {
        future->discarded_result = future->result;
        future->result = NULL;
        if (fatal != 0) {
            future->kind = LLAM_OUTCOME_RUNTIME_ERROR;
            future->runtime_error = fatal;
        } else if (cancelled) future->kind = LLAM_OUTCOME_CANCELLED;
        else if (future->entry_returned && llam_type_is_unit(&future->contract.success))
            future->kind = LLAM_OUTCOME_OK;
        else {
            future->kind = LLAM_OUTCOME_RUNTIME_ERROR;
            future->runtime_error = future->entry_returned ? EINVAL : ECANCELED;
        }
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
}

static void llam_scoped_release_inputs(llam_future_t *future) {
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_value_t *argument = future->argument;
    llam_value_t *discarded = future->discarded_result;
    future->argument = NULL;
    future->discarded_result = NULL;
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_structured_release(argument != NULL ? &argument->object : NULL);
    llam_structured_release(discarded != NULL ? &discarded->object : NULL);
}

static void llam_scope_account_completion_locked(llam_scope_t *scope, const llam_future_t *future) {
    if (scope->report.pending_tasks == 0U) abort();
    scope->report.pending_tasks -= 1U;
    switch (future->kind) {
    case LLAM_OUTCOME_OK: scope->report.succeeded += 1U; break;
    case LLAM_OUTCOME_APP_ERROR: scope->report.application_errors += 1U; break;
    case LLAM_OUTCOME_CANCELLED: scope->report.cancelled += 1U; break;
    case LLAM_OUTCOME_RUNTIME_ERROR:
        scope->report.runtime_errors += 1U;
        if (scope->report.runtime_error == 0) scope->report.runtime_error = future->runtime_error;
        break;
    default: abort();
    }
}

static void llam_scoped_publish_completion(llam_future_t *future) {
    llam_scope_t *scope = future->scope;
    bool notify;
    for (;;) {
        pthread_mutex_lock(&g_llam_structured_lock);
        if (!future->object.public_live && future->result != NULL) {
            llam_value_t *discarded = future->result;
            future->result = NULL;
            pthread_mutex_unlock(&g_llam_structured_lock);
            llam_structured_release(&discarded->object);
            continue;
        }
        /* Readiness and the final pending count share this publication lock.
         * If a consumer remains, its result ownership now survives scope drain. */
        future->ready = true;
        future->scope = NULL;
        llam_scope_account_completion_locked(scope, future);
        notify = llam_scope_finish_closing_locked(scope);
        pthread_mutex_unlock(&g_llam_structured_lock);
        break;
    }
    llam_structured_notify(future->object.owner, future->done);
    if (notify) llam_structured_notify(scope->object.owner, scope->done);
    llam_structured_release(&scope->object);
    llam_structured_release(&future->object);
}

void llam_scoped_task_retire(llam_task_t *task) {
    llam_future_t *future = task->scope_completion;
    if (future == NULL) return;
    task->scope_completion = NULL;
    if (atomic_load_explicit(&task->owner_runtime->shutdown_requested, memory_order_acquire)) {
        /* Teardown owns the lifecycle lock. Carry the independent completion
         * references past task/worker storage, then dispose outside that lock. */
        pthread_mutex_lock(&g_llam_structured_lock);
        future->retired_next = task->owner_runtime->scope_retirements;
        task->owner_runtime->scope_retirements = future;
        pthread_mutex_unlock(&g_llam_structured_lock);
        return;
    }
    llam_scoped_release_inputs(future);
    llam_scoped_publish_completion(future);
}

llam_future_t *llam_scoped_task_take_retired(llam_runtime_t *runtime) {
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_future_t *head = runtime->scope_retirements;
    runtime->scope_retirements = NULL;
    pthread_mutex_unlock(&g_llam_structured_lock);
    return head;
}

void llam_scoped_task_collect_retired(llam_future_t *head) {
    while (head != NULL) {
        llam_future_t *next = head->retired_next;
        head->retired_next = NULL;
        llam_scoped_release_inputs(head);
        llam_scoped_publish_completion(head);
        head = next;
    }
}
