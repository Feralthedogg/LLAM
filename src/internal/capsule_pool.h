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
 * See LICENSES/Apache-2.0.txt.
 */

#ifndef LLAM_CAPSULE_POOL_H
#define LLAM_CAPSULE_POOL_H

#include "runtime_platform.h"
#if LLAM_PLATFORM_WINDOWS
#include "runtime_windows_compat.h"
#else
#include <pthread.h>
#endif
#if LLAM_PLATFORM_DARWIN
#include <dispatch/dispatch.h>
#elif !LLAM_PLATFORM_WINDOWS
#include <semaphore.h>
#endif
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*llam_capsule_prepare_fn)(void *object, void *data);
typedef void (*llam_capsule_destroy_fn)(void *object, void *data);
struct llam_capsule_pool_node;

typedef struct llam_capsule_pool {
    pthread_mutex_t lock;
    pthread_mutex_t build_lock;
#if LLAM_PLATFORM_DARWIN
    dispatch_semaphore_t wake;
#elif LLAM_PLATFORM_WINDOWS
    HANDLE wake;
#else
    sem_t wake;
#endif
    pthread_t cold_thread;
    struct llam_capsule_pool_node *all;
    struct llam_capsule_pool_node *free;
    size_t object_bytes;
    size_t charge;
    size_t maximum;
    size_t target_free;
    atomic_size_t prepared;
    atomic_size_t available;
    atomic_size_t in_use;
    atomic_bool refill_requested;
    atomic_bool closing;
    llam_capsule_prepare_fn prepare;
    llam_capsule_destroy_fn destroy;
    void *data;
    bool initialized;
    bool cold_started;
} llam_capsule_pool_t;

/* Initialization/refill allocate only in host or dedicated cold context.
 * charge includes aligned pool metadata and the backend's nested payloads.
 * A false cold_allowed take only trylocks and consumes a prepared object. */
int llam_capsule_pool_init(llam_capsule_pool_t *pool, size_t object_bytes,
                            size_t nested_bytes, uint64_t budget, size_t initial,
                            llam_capsule_prepare_fn prepare,
                            llam_capsule_destroy_fn destroy, void *data);
void *llam_capsule_pool_take(llam_capsule_pool_t *pool, bool cold_allowed);
/* Backend must first prove its exact native readers/banks/operations drained. */
void llam_capsule_pool_put(llam_capsule_pool_t *pool, void *object);
void llam_capsule_pool_destroy(llam_capsule_pool_t *pool);

#endif /* LLAM_CAPSULE_POOL_H */
