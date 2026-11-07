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

void llam_future_dispose(llam_structured_object_t *object) {
    llam_future_t *future = (llam_future_t *)object;
    if (future->done != NULL) {
        llam_structured_notify(object->owner, future->done);
        if (llam_channel_destroy(future->done) != 0) abort();
    }
    llam_structured_release(future->argument != NULL ? &future->argument->object : NULL);
    llam_structured_release(future->result != NULL ? &future->result->object : NULL);
    free(future);
}

llam_future_t *llam_future_prepare(llam_scope_t *scope, const llam_result_contract_t *contract) {
    llam_future_t *future = calloc(1U, sizeof(*future));
    if (future == NULL) { errno = ENOMEM; return NULL; }
    llam_structured_init(&future->object, LLAM_PUBLIC_HANDLE_FAMILY_FUTURE,
                          scope->object.owner, llam_future_dispose);
    future->scope = scope;
    future->contract = *contract;
    future->done = llam_runtime_channel_create(scope->object.owner, 1U);
    if (future->done == NULL) { free(future); return NULL; }
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = llam_structured_publish_locked(&future->object);
    if (result == 0) {
        /* The spawn call retains an operation pin until output publication. */
        future->object.refs += 1U;
        future->object.public_ops = 1U;
    }
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (result != 0) {
        int error = errno;
        llam_future_dispose(&future->object);
        errno = error;
        return NULL;
    }
    return future;
}

static bool llam_future_is_ready(llam_future_t *future) {
    pthread_mutex_lock(&g_llam_structured_lock);
    bool ready = future->ready;
    pthread_mutex_unlock(&g_llam_structured_lock);
    return ready;
}

int llam_future_wait_until(llam_future_t *handle, uint64_t deadline) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_FUTURE, &object) != 0) return -1;
    llam_future_t *future = (llam_future_t *)object;
    int result = 0;
    if (!llam_future_is_ready(future)) result = llam_structured_wait(object->owner, future->done, deadline);
    int error = errno;
    /* Completion publication wins a concurrent timeout or runtime stop. */
    if (llam_future_is_ready(future)) result = 0;
    else if (result == 0) { result = -1; error = EIO; }
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}

static int llam_future_take_locked(llam_future_t *future, const llam_result_contract_t *expected,
                                    llam_outcome_t *result) {
    if (!future->object.public_live) { errno = EINVAL; return -1; }
    if (future->object.public_ops != 1U || future->object.claimed) { errno = EBUSY; return -1; }
    if (!llam_contract_equal(&future->contract, expected)) { errno = EINVAL; return -1; }
    if (!future->ready) { errno = EAGAIN; return -1; }
    if (future->result != NULL && llam_structured_publish_locked(&future->result->object) != 0) return -1;
    result->kind = future->kind;
    result->runtime_error = future->runtime_error;
    result->task_id = future->task_id;
    result->value = future->result != NULL ? llam_structured_handle(&future->result->object) : NULL;
    future->result = NULL;
    /* Consumption is exclusive with every public operation. In particular a
     * rejected peer-runtime lookup cannot become the last disposer of this
     * future's runtime-owned notification channel. */
    if (llam_structured_consume_locked(&future->object) != 0) abort();
    return 0;
}

int llam_future_take_ex(llam_future_t *handle, const llam_result_contract_t *expected,
                        size_t expected_size, llam_outcome_t *out, size_t out_size) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_result_contract_t contract;
    if (out == NULL || out_size < sizeof(*out) ||
        llam_scope_copy_input(&contract, sizeof(contract), expected, expected_size) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (llam_contract_validate(&contract) != 0) return -1;
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_FUTURE, &object) != 0) return -1;
    llam_outcome_t result = {0};
    pthread_mutex_lock(&g_llam_structured_lock);
    int status = llam_future_take_locked((llam_future_t *)object, &contract, &result);
    pthread_mutex_unlock(&g_llam_structured_lock);
    int error = errno;
    if (status == 0) memcpy(out, &result, sizeof(result));
    llam_structured_end(object);
    if (status != 0) errno = error;
    return status;
}

int llam_future_release(llam_future_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_FUTURE, &object) != 0) return -1;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = llam_structured_consume_locked(object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    int error = errno;
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}
