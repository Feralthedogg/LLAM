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

static void llam_local_executor_free(llam_local_executor_t *domain) {
    if (domain->done != NULL) {
        if (llam_channel_close(domain->done) != 0 || llam_channel_destroy(domain->done) != 0) abort();
    }
    free(domain);
}

static void llam_local_executor_dispose(llam_structured_object_t *object) {
    llam_local_executor_free((llam_local_executor_t *)object);
}

int llam_local_executor_opts_init(llam_local_executor_opts_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    if (out == NULL || size < sizeof(*out)) { errno = EINVAL; return -1; }
    const llam_local_executor_opts_t defaults = {0};
    memcpy(out, &defaults, sizeof(defaults));
    return 0;
}

static int llam_local_executor_validate(const llam_local_executor_opts_t *opts, size_t size,
                                         llam_local_executor_t **out) {
    if (out == NULL) { errno = EINVAL; return -1; }
    if (opts == NULL) return 0;
    llam_local_executor_opts_t copy;
    if (llam_scope_copy_input(&copy, sizeof(copy), opts, size) != 0) return -1;
    if (copy.flags != 0U || copy.reserved0 != 0U) { errno = EINVAL; return -1; }
    return 0;
}

static llam_local_executor_t *llam_local_executor_prepare(llam_runtime_t *runtime) {
    if (!runtime->managed_policy_pools || runtime->managed_catalog == NULL ||
        runtime->portable_processor_count == 0U) { errno = ENOTSUP; return NULL; }
    llam_local_executor_t *domain = calloc(1U, sizeof(*domain));
    if (domain == NULL) return NULL;
    llam_structured_init(&domain->object, LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR,
                          runtime, llam_local_executor_dispose);
    domain->done = llam_channel_create_owned(1U, runtime);
    if (domain->done == NULL || llam_managed_reserve_execution(runtime, LLAM_MANAGED_EXECUTION_NATIVE_AFFINE,
            &domain->worker, &domain->generation) != 0) {
        int error = errno;
        llam_local_executor_free(domain);
        errno = error;
        return NULL;
    }
    return domain;
}

int llam_runtime_local_executor_create_ex(llam_runtime_t *handle,
    const llam_local_executor_opts_t *opts, size_t size, llam_local_executor_t **out) {
    LLAM_NATIVE_API_GUARD(-1);
    if (llam_local_executor_validate(opts, size, out) != 0) return -1;
    llam_runtime_t *runtime;
    if (llam_runtime_begin_object_creation(handle, &runtime) != 0) return -1;
    llam_local_executor_t *domain = llam_local_executor_prepare(runtime);
    int result = domain != NULL ? llam_local_executor_publish(domain) : -1;
    int error = errno;
    if (domain != NULL && result != 0) {
        llam_managed_release_execution(runtime, domain->worker, LLAM_MANAGED_EXECUTION_NATIVE_AFFINE,
                                         domain->generation);
        llam_local_executor_free(domain);
    }
    if (result == 0) *out = llam_structured_handle(&domain->object);
    llam_runtime_end_public_op(runtime);
    if (result != 0) errno = error;
    return result;
}

int llam_local_executor_close(llam_local_executor_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR, &object) != 0) return -1;
    llam_local_executor_t *domain = (llam_local_executor_t *)object;
    pthread_mutex_lock(&g_llam_structured_lock);
    domain->closed = true;
    bool notify = llam_local_executor_finish_locked(domain);
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_local_executor_notify(domain, notify);
    llam_structured_end(object);
    return 0;
}

static int llam_local_executor_claim_drain(llam_local_executor_t *domain) {
    if (g_llam_tls_task != NULL && g_llam_tls_task->local_executor == domain) { errno = EDEADLK; return -1; }
    if (domain->draining) { errno = EBUSY; return -1; }
    if (g_llam_tls_task == NULL && domain->pending != 0U &&
        !atomic_load_explicit(&domain->object.owner->exec_started, memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }
    domain->draining = true;
    domain->closed = true;
    return 0;
}

static int llam_local_executor_finish_drain(llam_local_executor_t *domain, int result, int error) {
    pthread_mutex_lock(&g_llam_structured_lock);
    if (domain->closed && domain->pending == 0U && domain->native_joined &&
        (!domain->shutdown_deferred || domain->notified)) result = 0;
    else if (result == 0) { result = -1; error = EIO; }
    domain->draining = false;
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (result != 0) errno = error;
    return result;
}

int llam_local_executor_drain(llam_local_executor_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR, &object) != 0) return -1;
    llam_local_executor_t *domain = (llam_local_executor_t *)object;
    llam_cleanup_wait_state_t mask = {0};
    int result = llam_borrowed_cleanup_enter(&mask);
    pthread_mutex_lock(&g_llam_structured_lock);
    if (result == 0) result = llam_local_executor_claim_drain(domain);
    bool claimed = result == 0;
    bool notify = claimed && llam_local_executor_finish_locked(domain);
    bool ready = domain->closed && domain->pending == 0U && domain->native_joined &&
        (!domain->shutdown_deferred || domain->notified);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (claimed) {
        llam_local_executor_notify(domain, notify);
        if (!ready) result = llam_borrowed_wait_event(object->owner, domain->done);
        result = llam_local_executor_finish_drain(domain, result, errno);
    }
    int error = errno;
    llam_borrowed_cleanup_leave(&mask);
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}

int llam_local_executor_destroy(llam_local_executor_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR, &object) != 0) return -1;
    llam_local_executor_t *domain = (llam_local_executor_t *)object;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = -1;
    if (!domain->closed || domain->pending != 0U || !domain->native_joined || (domain->shutdown_deferred && !domain->notified) || domain->draining ||
        object->public_ops != 1U) errno = EBUSY;
    else if (llam_structured_consume_locked(object) == 0) {
        llam_local_executor_unlink_locked(domain);
        result = 0;
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
    int error = errno;
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}
