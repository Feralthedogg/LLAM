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

pthread_mutex_t g_llam_structured_lock = PTHREAD_MUTEX_INITIALIZER;
static llam_public_slot_table_t g_llam_structured_slots[4];

static llam_public_slot_table_t *llam_structured_table(uint32_t family) {
    if (family < LLAM_PUBLIC_HANDLE_FAMILY_SCOPE || family > LLAM_PUBLIC_HANDLE_FAMILY_LOCAL_EXECUTOR) return NULL;
    return &g_llam_structured_slots[family - LLAM_PUBLIC_HANDLE_FAMILY_SCOPE];
}

void llam_structured_init(llam_structured_object_t *object, uint32_t family,
                          llam_runtime_t *owner, void (*dispose)(llam_structured_object_t *)) {
    object->owner = owner;
    object->family = family;
    object->refs = 1U;
    object->dispose = dispose;
}

int llam_structured_publish_locked(llam_structured_object_t *object) {
    llam_public_slot_table_t *table = llam_structured_table(object->family);
    uint64_t secret = object->owner != NULL ? object->owner->public_handle_secret : 0U;
    if (table == NULL || object->public_live) { errno = EINVAL; return -1; }
    if (llam_public_slot_reserve_family_secret(table, object, 64U, object->family,
                                               secret, &object->slot, &object->generation) != 0) return -1;
    object->public_live = true;
    return 0;
}

void *llam_structured_handle(const llam_structured_object_t *object) {
    return (void *)llam_public_slot_encode_handle(object->slot, object->generation, 32U);
}

llam_structured_object_t *llam_structured_resolve_locked(const void *handle, uint32_t family) {
    llam_public_slot_table_t *table = llam_structured_table(family);
    if (table == NULL) { errno = EINVAL; return NULL; }
    llam_structured_object_t *object = llam_public_slot_resolve_encoded(table, (uintptr_t)handle,
                                                                       32U, NULL, NULL);
    if (object == NULL || !object->public_live || object->family != family) {
        errno = EINVAL;
        return NULL;
    }
    if (object->claimed) { errno = EBUSY; return NULL; }
    return object;
}

int llam_structured_retain_locked(llam_structured_object_t *object) {
    if (object->refs == SIZE_MAX) { errno = EBUSY; return -1; }
    object->refs += 1U;
    return 0;
}

llam_structured_object_t *llam_structured_drop_locked(llam_structured_object_t *object) {
    if (object->refs == 0U) abort();
    object->refs -= 1U;
    return object->refs == 0U ? object : NULL;
}

void llam_structured_unpublish_locked(llam_structured_object_t *object) {
    if (!object->public_live) abort();
    llam_public_slot_release(llam_structured_table(object->family), object->slot,
                              object, object->generation);
    object->public_live = false;
    object->claimed = false;
}

int llam_structured_begin(const void *handle, uint32_t family, llam_structured_object_t **out) {
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_structured_object_t *object = llam_structured_resolve_locked(handle, family);
    if (object != NULL && llam_structured_retain_locked(object) == 0) object->public_ops += 1U;
    else object = NULL;
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (object == NULL) return -1;
    if (object->owner != NULL && llam_runtime_check_object_owner_for_cleanup(object->owner) != 0) {
        int error = errno;
        llam_structured_end(object);
        errno = error;
        return -1;
    }
    *out = object;
    return 0;
}

void llam_structured_end(llam_structured_object_t *object) {
    pthread_mutex_lock(&g_llam_structured_lock);
    if (object->public_ops == 0U) abort();
    object->public_ops -= 1U;
    llam_structured_object_t *dispose = llam_structured_drop_locked(object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (dispose != NULL) dispose->dispose(dispose);
}

void llam_structured_release(llam_structured_object_t *object) {
    if (object == NULL) return;
    pthread_mutex_lock(&g_llam_structured_lock);
    llam_structured_object_t *dispose = llam_structured_drop_locked(object);
    pthread_mutex_unlock(&g_llam_structured_lock);
    if (dispose != NULL) dispose->dispose(dispose);
}

int llam_structured_consume_locked(llam_structured_object_t *object) {
    if (!object->public_live) { errno = EINVAL; return -1; }
    if (object->claimed || object->public_ops != 1U) { errno = EBUSY; return -1; }
    llam_structured_unpublish_locked(object);
    /* The current public operation retains storage until end(). */
    if (llam_structured_drop_locked(object) != NULL) abort();
    return 0;
}
