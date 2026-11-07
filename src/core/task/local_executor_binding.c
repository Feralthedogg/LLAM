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

#include "local_executor_internal.h"

static llam_local_executor_t *g_local_executors;

bool llam_task_owns_native_thread(const llam_task_t *task) {
    return task->managed_execution_class == LLAM_MANAGED_EXECUTION_NATIVE_AFFINE && task->local_executor == NULL;
}

static void llam_local_executor_release_worker_locked(llam_local_executor_t *domain) {
    llam_worker_t *worker = domain->worker;
    if (worker == NULL) return;
    if (atomic_load_explicit(&worker->local_executor, memory_order_relaxed) != domain) abort();
    atomic_store_explicit(&worker->local_executor, NULL, memory_order_release);
    llam_managed_release_execution(domain->object.owner, worker,
                                   LLAM_MANAGED_EXECUTION_NATIVE_AFFINE, domain->generation);
    domain->worker = NULL;
    domain->native_joined = true;
    /* The published-list reference remains until destroy or deferred teardown. */
    if (llam_structured_drop_locked(&domain->object) != NULL) abort();
}

bool llam_local_executor_finish_locked(llam_local_executor_t *domain) {
    if (!domain->closed || domain->pending != 0U || domain->shutdown_deferred) return false;
    if (!domain->started && domain->worker != NULL)
        llam_local_executor_release_worker_locked(domain);
    if (!domain->native_joined || domain->notified) return false;
    if (llam_structured_retain_locked(&domain->object) != 0) abort();
    domain->notified = true;
    return true;
}

void llam_local_executor_notify(llam_local_executor_t *domain, bool notify) {
    if (!notify) return;
    if (llam_channel_close(domain->done) != 0) abort();
    llam_structured_release(&domain->object);
}

int llam_local_executor_publish(llam_local_executor_t *domain) {
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = llam_structured_publish_locked(&domain->object);
    if (result == 0) {
        domain->object.refs += 2U; /* List and native reservation. */
        domain->listed = true;
        domain->next = g_local_executors;
        g_local_executors = domain;
        atomic_store_explicit(&domain->worker->local_executor, domain, memory_order_release);
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
    return result;
}

void llam_local_executor_unlink_locked(llam_local_executor_t *domain) {
    if (!domain->listed) return;
    llam_local_executor_t **link = &g_local_executors;
    while (*link != domain) { if (*link == NULL) abort(); link = &(*link)->next; }
    *link = domain->next;
    domain->next = NULL;
    domain->listed = false;
    if (llam_structured_drop_locked(&domain->object) != NULL) abort();
}

int llam_local_executor_admit(llam_runtime_t *runtime, llam_local_executor_t *handle,
    llam_local_executor_t **out, llam_worker_t **worker, uint64_t *generation) {
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR, &object) != 0) return -1;
    llam_local_executor_t *domain = (llam_local_executor_t *)object;
    pthread_mutex_lock(&g_llam_structured_lock);
    int error = 0;
    if (object->owner != runtime) error = EXDEV;
    else if (domain->closed || domain->worker == NULL) error = EPIPE;
    /* Pair with the scheduler's SC closing publication and its locked pending
     * recheck. A member accepted before closing is observed by that recheck;
     * one arriving after it cannot strand work on a retiring native thread. */
    else if (atomic_load_explicit(&domain->worker->managed_slot_closing, memory_order_seq_cst)) error = EPIPE;
    else if (domain->pending == SIZE_MAX) error = EOVERFLOW;
    else if (llam_structured_retain_locked(object) != 0) error = errno;
    if (error == 0) {
        domain->pending += 1U; /* Reserve execution before runnable publication. */
        *worker = domain->worker;
        *generation = domain->generation;
        *out = domain;
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_structured_end(object);
    if (error != 0) { errno = error; return -1; }
    return 0;
}

void llam_local_executor_admission_release(llam_local_executor_t *domain, bool retired) {
    if (domain == NULL) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    if (!retired) {
        if (domain->pending == 0U) abort();
        domain->pending -= 1U;
    }
    bool notify = llam_local_executor_finish_locked(domain);
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_local_executor_notify(domain, notify);
    llam_structured_release(&domain->object);
}

void llam_local_executor_task_started(llam_task_t *task, llam_worker_t *worker) {
    llam_local_executor_t *domain = task->local_executor;
    if (domain == NULL) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    uint64_t incarnation = atomic_load_explicit(&worker->binding_incarnation, memory_order_acquire);
    if (domain->worker != worker || domain->generation != task->managed_execution_generation ||
        domain->native_joined || incarnation == 0U ||
        (domain->started && domain->incarnation != incarnation)) abort();
    domain->started = true;
    domain->incarnation = incarnation;
    pthread_mutex_unlock(&g_llam_structured_lock);
}

void llam_local_executor_task_retired(llam_task_t *task) {
    llam_local_executor_t *domain = task->local_executor;
    if (domain == NULL || task->local_executor_retired) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    if (domain->pending == 0U) abort();
    domain->pending -= 1U;
    task->local_executor_retired = true;
    bool notify = llam_local_executor_finish_locked(domain);
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_local_executor_notify(domain, notify);
}

bool llam_local_executor_worker_pending(const llam_worker_t *worker) {
    if (atomic_load_explicit(&worker->local_executor, memory_order_acquire) == NULL) return false;
    pthread_mutex_lock(&g_llam_structured_lock);
    const llam_local_executor_t *domain = atomic_load_explicit(&worker->local_executor, memory_order_relaxed);
    bool pending = domain != NULL && domain->pending != 0U;
    pthread_mutex_unlock(&g_llam_structured_lock);
    return pending;
}

bool llam_local_executor_worker_idle(const llam_worker_t *worker) {
    if (atomic_load_explicit(&worker->local_executor, memory_order_acquire) == NULL) return false;
    pthread_mutex_lock(&g_llam_structured_lock);
    const llam_local_executor_t *domain = atomic_load_explicit(&worker->local_executor, memory_order_relaxed);
    bool idle = domain != NULL && domain->pending == 0U;
    pthread_mutex_unlock(&g_llam_structured_lock);
    return idle;
}

bool llam_local_executor_worker_closing(llam_worker_t *worker) {
    if (atomic_load_explicit(&worker->local_executor, memory_order_acquire) == NULL) return false;
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_local_executor_t *domain = atomic_load_explicit(&worker->local_executor, memory_order_relaxed);
    if (domain != NULL && atomic_load_explicit(&worker->registry_runtime->stop_requested, memory_order_acquire))
        domain->closed = true;
    bool closing = domain != NULL && domain->closed && domain->pending == 0U;
    pthread_mutex_unlock(&g_llam_structured_lock);
    return closing;
}

void llam_local_executor_worker_joined(llam_worker_t *worker) {
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_local_executor_t *domain = atomic_load_explicit(&worker->local_executor, memory_order_relaxed);
    if (domain == NULL || (!domain->started && domain->pending != 0U)) {
        /* Aborted startup ran no user code. Keep its reservation for a retry. */
        pthread_mutex_unlock(&g_llam_structured_lock);
        return;
    }
    domain->closed = true;
    llam_local_executor_release_worker_locked(domain);
    bool notify = llam_local_executor_finish_locked(domain);
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_local_executor_notify(domain, notify);
}

void llam_local_executor_defer_shutdown(llam_runtime_t *runtime) {
    pthread_mutex_lock(&g_llam_structured_lock);
    for (llam_local_executor_t *domain = g_local_executors; domain != NULL; domain = domain->next) {
        if (domain->object.owner == runtime) domain->shutdown_deferred = true;
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
}

llam_local_executor_t *llam_local_executor_retire_runtime(llam_runtime_t *runtime) {
    llam_local_executor_t *retired = NULL;
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_local_executor_t **link = &g_local_executors;
    while (*link != NULL) {
        llam_local_executor_t *domain = *link;
        if (domain->object.owner != runtime) { link = &domain->next; continue; }
        if (domain->pending != 0U) abort();
        *link = domain->next;
        domain->next = retired;
        retired = domain; /* Transfer the list reference past lifecycle unlock. */
        domain->listed = false;
        domain->closed = true;
        llam_local_executor_release_worker_locked(domain);
        domain->native_joined = true;
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
    return retired;
}

void llam_local_executor_collect_retired(llam_local_executor_t *head) {
    while (head != NULL) {
        llam_local_executor_t *next = head->next;
        pthread_mutex_lock(&g_llam_structured_lock);
        head->next = NULL;
        head->shutdown_deferred = false;
        bool notify = llam_local_executor_finish_locked(head);
        pthread_mutex_unlock(&g_llam_structured_lock);
        llam_local_executor_notify(head, notify);
        llam_structured_release(&head->object);
        head = next;
    }
}
