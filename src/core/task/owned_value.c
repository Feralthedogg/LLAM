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

static void llam_value_dispose(llam_structured_object_t *object) {
    llam_value_t *value = (llam_value_t *)object;
    int saved_errno = llam_thread_errno_load();
    /* These callbacks are explicitly nonblocking and thread-neutral. No registry
     * or runtime lock is held. Reentry is forbidden by the callback contract;
     * native guards also enforce it on registered native workers. */
    llam_managed_native_enter();
    if (value->spec.drop != NULL) value->spec.drop(value->spec.data, value->spec.drop_context);
    else free(value->spec.data);
    llam_managed_native_leave();
    free(value);
    llam_thread_errno_store(saved_errno);
}

int llam_value_create_ex(const llam_value_spec_t *spec, size_t size, llam_value_t **out) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_value_spec_t raw;
    if (out == NULL || llam_scope_copy_input(&raw, sizeof(raw), spec, size) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (llam_type_validate(&raw.type) != 0) return -1;
    if ((raw.type.size != 0U && raw.data == NULL) ||
        ((uintptr_t)raw.data & (raw.type.alignment - 1U)) != 0U) { errno = EINVAL; return -1; }
    llam_value_t *value = calloc(1U, sizeof(*value));
    if (value == NULL) { errno = ENOMEM; return -1; }
    llam_structured_init(&value->object, LLAM_PUBLIC_HANDLE_FAMILY_VALUE, NULL, llam_value_dispose);
    value->spec = raw;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = llam_structured_publish_locked(&value->object);
    if (result == 0) *out = llam_structured_handle(&value->object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (result != 0) free(value); /* Adoption did not commit; caller retains data. */
    return result;
}

static void llam_value_copy_drop(void *data, void *allocation) {
    (void)data;
    free(allocation);
}

int llam_value_copy_ex(const llam_type_descriptor_t *type, size_t size,
                       const void *data, llam_value_t **out) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_value_spec_t spec = {0};
    if (out == NULL || llam_scope_copy_input(&spec.type, sizeof(spec.type), type, size) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (llam_type_validate(&spec.type) != 0) return -1;
    size_t bytes = (size_t)spec.type.size, padding = spec.type.alignment - 1U;
    if ((bytes != 0U && data == NULL) || bytes > SIZE_MAX - padding) { errno = EINVAL; return -1; }
    void *allocation = bytes != 0U ? malloc(bytes + padding) : NULL;
    if (bytes != 0U && allocation == NULL) { errno = ENOMEM; return -1; }
    spec.data = allocation != NULL ? (void *)(((uintptr_t)allocation + padding) & ~(uintptr_t)padding) : NULL;
    spec.drop = llam_value_copy_drop;
    spec.drop_context = allocation;
    if (bytes != 0U) memcpy(spec.data, data, bytes);
    int result = llam_value_create_ex(&spec, sizeof(spec), out);
    if (result != 0) free(allocation);
    return result;
}

int llam_value_data_ex(llam_value_t *handle, const llam_type_descriptor_t *expected,
                       size_t size, void **out) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_type_descriptor_t type;
    if (out == NULL || llam_scope_copy_input(&type, sizeof(type), expected, size) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (llam_type_validate(&type) != 0) return -1;
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_VALUE, &object) != 0) return -1;
    llam_value_t *value = (llam_value_t *)object;
    int error = llam_type_equal(&value->spec.type, &type) ? 0 : EINVAL;
    if (error == 0) *out = value->spec.data;
    llam_structured_end(object);
    if (error != 0) { errno = error; return -1; }
    return 0;
}

int llam_value_release(llam_value_t *handle) {
    LLAM_NATIVE_API_GUARD(-1);
    llam_structured_object_t *object;
    if (llam_structured_begin(handle, LLAM_PUBLIC_HANDLE_FAMILY_VALUE, &object) != 0) return -1;
    pthread_mutex_lock(&g_llam_structured_lock);
    int result = llam_structured_consume_locked(object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    int error = errno;
    llam_structured_end(object);
    if (result != 0) errno = error;
    return result;
}

llam_value_t *llam_value_claim_locked(llam_value_t *handle) {
    llam_structured_object_t *object = llam_structured_resolve_locked(handle, LLAM_PUBLIC_HANDLE_FAMILY_VALUE);
    if (object == NULL) return NULL;
    if (object->public_ops != 0U) { errno = EBUSY; return NULL; }
    if (llam_structured_retain_locked(object) != 0) return NULL;
    object->claimed = true;
    return (llam_value_t *)object;
}

void llam_value_commit_claim(llam_value_t *value) {
    if (value == NULL) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    if (!value->object.claimed) abort();
    llam_structured_unpublish_locked(&value->object);
    llam_structured_object_t *dispose = llam_structured_drop_locked(&value->object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (dispose != NULL) dispose->dispose(dispose);
}

void llam_value_rollback_claim(llam_value_t *value) {
    if (value == NULL) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    if (!value->object.claimed || !value->object.public_live) abort();
    value->object.claimed = false;
    if (llam_structured_drop_locked(&value->object) != NULL) abort();
    pthread_mutex_unlock(&g_llam_structured_lock);
}

int llam_value_take_locked(llam_value_t *handle, const llam_type_descriptor_t *expected,
                           llam_value_t **out) {
    if (handle == NULL) {
        if (!llam_type_is_unit(expected)) { errno = EINVAL; return -1; }
        *out = NULL;
        return 0;
    }
    llam_structured_object_t *object = llam_structured_resolve_locked(handle, LLAM_PUBLIC_HANDLE_FAMILY_VALUE);
    if (object == NULL) return -1;
    llam_value_t *value = (llam_value_t *)object;
    if (!llam_type_equal(&value->spec.type, expected)) { errno = EINVAL; return -1; }
    if (object->public_ops != 0U) { errno = EBUSY; return -1; }
    /* Transfer the public ownership reference; do not release the value. */
    llam_structured_unpublish_locked(object);
    *out = value;
    return 0;
}
