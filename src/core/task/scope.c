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

static void llam_scope_free(llam_scope_t *scope) {
    if (scope->cancel_token != NULL && llam_cancel_token_destroy(scope->cancel_token) != 0) abort();
    if (scope->done != NULL) {
        llam_structured_notify(scope->object.owner, scope->done);
        if (llam_channel_destroy(scope->done) != 0) abort();
    }
    free(scope);
}

void llam_scope_dispose(llam_structured_object_t *object) {
    llam_scope_free((llam_scope_t *)object);
}

static llam_scope_t *llam_scope_allocate(llam_runtime_t *owner) {
    llam_scope_t *scope = calloc(1U, sizeof(*scope));
    if (scope == NULL) { errno = ENOMEM; return NULL; }
    llam_structured_init(&scope->object, LLAM_PUBLIC_HANDLE_FAMILY_SCOPE, owner, llam_scope_dispose);
    scope->cancel_token = llam_cancel_token_create_owned(owner);
    if (scope->cancel_token != NULL) scope->done = llam_runtime_channel_create(owner, 1U);
    if (scope->cancel_token == NULL || scope->done == NULL) {
        int error = errno;
        llam_scope_free(scope);
        errno = error;
        return NULL;
    }
    return scope;
}

int llam_runtime_scope_create_ex(llam_runtime_t *runtime, const llam_scope_opts_t *opts,
                                 size_t size, llam_scope_t **out) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_scope_opts_t raw = {0};
    if (out == NULL || (opts != NULL && llam_scope_copy_input(&raw, sizeof(raw), opts, size) != 0)) {
        errno = EINVAL;
        return -1;
    }
    if (raw.flags != 0U || raw.reserved0 != 0U) { errno = EINVAL; return -1; }
    llam_runtime_t *owner;
    if (llam_runtime_begin_object_creation(runtime, &owner) != 0) return -1;
    llam_scope_t *scope = llam_scope_allocate(owner);
    int result = -1;
    if (scope != NULL) {
        pthread_mutex_lock(&g_llam_structured_lock);
        result = llam_structured_publish_locked(&scope->object);
        if (result == 0) *out = llam_structured_handle(&scope->object);
        pthread_mutex_unlock(&g_llam_structured_lock);
    }
    int error = errno;
    if (scope != NULL && result != 0) llam_scope_free(scope);
    llam_runtime_end_public_op(owner);
    if (result != 0) errno = error;
    return result;
}

bool llam_scope_finish_closing_locked(llam_scope_t *scope) {
    if (scope->report.state != LLAM_SCOPE_CLOSING || scope->report.pending_tasks != 0U ||
        scope->report.active_spawns != 0U) return false;
    scope->report.state = LLAM_SCOPE_DRAINED;
    scope->report.flags |= LLAM_SCOPE_REPORT_F_QUIESCENT;
    return true;
}

void llam_scope_copy_report_locked(const llam_scope_t *scope, llam_scope_report_t *out, size_t size) {
    if (out != NULL) memcpy(out, &scope->report, size < sizeof(*out) ? size : sizeof(*out));
}

int llam_scope_get_report_ex(llam_scope_t *handle, llam_scope_report_t *out, size_t size) {
    LLAM_NATIVE_API_GUARD(-1);
    if (out == NULL || size == 0U) { errno = EINVAL; return -1; }
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_SCOPE, &object) != 0) return -1;
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_scope_copy_report_locked((llam_scope_t *)object, out, size);
    pthread_mutex_unlock(&g_llam_structured_lock);
    llam_structured_end(object);
    return 0;
}

int llam_scope_request_cancel(llam_scope_t *scope) {
    pthread_mutex_lock(&g_llam_structured_lock);
    bool ready = scope->report.state == LLAM_SCOPE_DRAINED;
    pthread_mutex_unlock(&g_llam_structured_lock);
    int result = ready ? 0 : llam_cancel_token_cancel(scope->cancel_token);
    int error = errno;
    pthread_mutex_lock(&g_llam_structured_lock);
    if (scope->report.state == LLAM_SCOPE_DRAINED) result = 0;
    if (result == 0) scope->report.flags |= LLAM_SCOPE_REPORT_F_CANCEL_REQUESTED;
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (result != 0) errno = error;
    return result;
}

int llam_scope_cancel(llam_scope_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_SCOPE, &object) != 0) return -1;
    llam_scope_t *scope = (llam_scope_t *)object;
    pthread_mutex_lock(&g_llam_structured_lock);
    if (scope->report.state == LLAM_SCOPE_OPEN) scope->report.state = LLAM_SCOPE_CLOSING;
    bool notify = llam_scope_finish_closing_locked(scope);
    pthread_mutex_unlock(&g_llam_structured_lock);
    int result = llam_scope_request_cancel(scope);
    int error = errno;
    if (notify) llam_structured_notify(object->owner, scope->done);
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}

int llam_scope_destroy(llam_scope_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_SCOPE, &object) != 0) return -1;
    llam_scope_t *scope = (llam_scope_t *)object;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = -1;
    if (scope->report.state != LLAM_SCOPE_DRAINED || scope->draining) errno = EBUSY;
    else result = llam_structured_consume_locked(object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    int error = errno;
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}
