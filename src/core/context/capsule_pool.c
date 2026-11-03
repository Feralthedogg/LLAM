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

#include "runtime_internal.h"
#include "capsule_pool.h"

typedef struct llam_capsule_pool_node {
    struct llam_capsule_pool_node *all_next;
    struct llam_capsule_pool_node *free_next;
    llam_capsule_pool_t *pool;
    bool in_use; /* pool->lock; not a replacement for native lifetime roots. */
    _Alignas(64) unsigned char object[];
} llam_capsule_pool_node_t;

static void llam_capsule_pool_free_node(llam_capsule_pool_node_t *node) {
#if LLAM_PLATFORM_WINDOWS
    _aligned_free(node);
#else
    free(node);
#endif
}

static void llam_capsule_pool_wake(llam_capsule_pool_t *pool) {
#if LLAM_PLATFORM_DARWIN
    (void)dispatch_semaphore_signal(pool->wake);
#elif LLAM_PLATFORM_WINDOWS
    if (!ReleaseSemaphore(pool->wake, 1L, NULL) && GetLastError() != ERROR_TOO_MANY_POSTS) abort();
#else
    if (sem_post(&pool->wake) != 0 && errno != EOVERFLOW) abort();
#endif
}

static void llam_capsule_pool_wait(llam_capsule_pool_t *pool) {
#if LLAM_PLATFORM_DARWIN
    if (dispatch_semaphore_wait(pool->wake, DISPATCH_TIME_FOREVER) != 0L) abort();
#elif LLAM_PLATFORM_WINDOWS
    if (WaitForSingleObject(pool->wake, INFINITE) != WAIT_OBJECT_0) abort();
#else
    int result;
    do { result = sem_wait(&pool->wake); } while (result != 0 && errno == EINTR);
    if (result != 0) abort();
#endif
}

/* build_lock serializes cold factories, never native capture/restore or G. */
static int llam_capsule_pool_refill(llam_capsule_pool_t *pool) {
    size_t made = 0U;
    int error = 0;
    pthread_mutex_lock(&pool->build_lock);
    while (made < 8U && !atomic_load_explicit(&pool->closing, memory_order_acquire)) {
        if (atomic_load_explicit(&pool->available, memory_order_acquire) >= pool->target_free ||
            atomic_load_explicit(&pool->prepared, memory_order_acquire) >= pool->maximum) break;
        void *storage = NULL;
        size_t bytes = sizeof(llam_capsule_pool_node_t) + pool->object_bytes;
#if LLAM_PLATFORM_WINDOWS
        storage = _aligned_malloc(bytes, 64U);
        if (storage == NULL) error = ENOMEM;
#else
        error = posix_memalign(&storage, 64U, bytes);
#endif
        if (error != 0) break;
        /* The entire object is touched in cold context before publication. */
        memset(storage, 0, bytes);
        llam_capsule_pool_node_t *node = storage;
        node->pool = pool;
        if (pool->prepare != NULL && pool->prepare(node->object, pool->data) != 0) {
            error = errno != 0 ? errno : ENOMEM;
            if (pool->destroy != NULL) pool->destroy(node->object, pool->data);
            llam_capsule_pool_free_node(node);
            break;
        }
        pthread_mutex_lock(&pool->lock);
        node->all_next = pool->all;
        node->free_next = pool->free;
        pool->all = node;
        pool->free = node;
        atomic_fetch_add_explicit(&pool->prepared, 1U, memory_order_relaxed);
        atomic_fetch_add_explicit(&pool->available, 1U, memory_order_release);
        pthread_mutex_unlock(&pool->lock);
        ++made;
    }
    atomic_store_explicit(&pool->refill_requested, false, memory_order_release);
    pthread_mutex_unlock(&pool->build_lock);
    if (made != 0U || error == 0) return 0;
    errno = error != 0 ? error : ENOMEM;
    return -1;
}

static void *llam_capsule_pool_cold_main(void *argument) {
    llam_capsule_pool_t *pool = argument;
    /* Allocation must remain schedulable when every managed CPU is busy.
     * This native cold worker owns no P and sleeps on a counted event at idle. */
    llam_tune_ctrl_thread(true);
    while (!atomic_load_explicit(&pool->closing, memory_order_acquire)) {
        if (atomic_load_explicit(&pool->refill_requested, memory_order_acquire)) {
            (void)llam_capsule_pool_refill(pool);
            continue;
        }
        /* A counted wake cannot be lost between the predicate and wait. An
         * idle pool causes no periodic wakeups or wall-clock deadline drift. */
        llam_capsule_pool_wait(pool);
    }
    return NULL;
}

int llam_capsule_pool_init(llam_capsule_pool_t *pool, size_t object_bytes,
                            size_t nested_bytes, uint64_t budget, size_t initial,
                            llam_capsule_prepare_fn prepare,
                            llam_capsule_destroy_fn destroy, void *data) {
    if (pool == NULL || object_bytes == 0U || budget > SIZE_MAX ||
        object_bytes > SIZE_MAX - sizeof(llam_capsule_pool_node_t) ||
        nested_bytes > SIZE_MAX - sizeof(llam_capsule_pool_node_t) - object_bytes) {
        errno = EINVAL;
        return -1;
    }
    memset(pool, 0, sizeof(*pool));
    int error = pthread_mutex_init(&pool->lock, NULL);
    if (error != 0) { errno = error; return -1; }
    error = pthread_mutex_init(&pool->build_lock, NULL);
    if (error != 0) { pthread_mutex_destroy(&pool->lock); errno = error; return -1; }
#if LLAM_PLATFORM_DARWIN
    pool->wake = dispatch_semaphore_create(0L);
    error = pool->wake != NULL ? 0 : ENOMEM;
#elif LLAM_PLATFORM_WINDOWS
    pool->wake = CreateSemaphoreW(NULL, 0L, LONG_MAX, NULL);
    error = pool->wake != NULL ? 0 : ENOMEM;
#else
    error = sem_init(&pool->wake, 0, 0U) == 0 ? 0 : errno;
#endif
    if (error != 0) {
        pthread_mutex_destroy(&pool->build_lock);
        pthread_mutex_destroy(&pool->lock);
        errno = error;
        return -1;
    }
    pool->object_bytes = object_bytes;
    pool->charge = sizeof(llam_capsule_pool_node_t) + object_bytes + nested_bytes;
    pool->maximum = (size_t)budget / pool->charge;
    pool->target_free = initial != 0U ? initial : 1U;
    if (pool->target_free > pool->maximum) pool->target_free = pool->maximum;
    pool->prepare = prepare;
    pool->destroy = destroy;
    pool->data = data;
    atomic_init(&pool->prepared, 0U);
    atomic_init(&pool->available, 0U);
    atomic_init(&pool->in_use, 0U);
    atomic_init(&pool->refill_requested, false);
    atomic_init(&pool->closing, false);
    pool->initialized = true;
    while (atomic_load_explicit(&pool->available, memory_order_acquire) < pool->target_free) {
        if (llam_capsule_pool_refill(pool) != 0) goto failed;
    }
    if (pool->maximum != 0U) {
        error = pthread_create(&pool->cold_thread, NULL, llam_capsule_pool_cold_main, pool);
        if (error != 0) { errno = error; goto failed; }
        pool->cold_started = true;
    }
    return 0;
failed:
    error = errno;
    llam_capsule_pool_destroy(pool);
    errno = error;
    return -1;
}

void *llam_capsule_pool_take(llam_capsule_pool_t *pool, bool cold_allowed) {
    if (pool == NULL || !pool->initialized ||
        atomic_load_explicit(&pool->closing, memory_order_acquire)) { errno = EINVAL; return NULL; }
    for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
        int error = cold_allowed ? pthread_mutex_lock(&pool->lock) : pthread_mutex_trylock(&pool->lock);
        if (error != 0) { errno = error == EBUSY ? EAGAIN : error; return NULL; }
        llam_capsule_pool_node_t *node = pool->free;
        if (node != NULL) {
            if (node->in_use || node->pool != pool) abort();
            pool->free = node->free_next;
            node->free_next = NULL;
            node->in_use = true;
            atomic_fetch_sub_explicit(&pool->available, 1U, memory_order_relaxed);
            atomic_fetch_add_explicit(&pool->in_use, 1U, memory_order_relaxed);
        }
        pthread_mutex_unlock(&pool->lock);
        if (node != NULL) {
            if (atomic_load_explicit(&pool->available, memory_order_acquire) < pool->target_free &&
                atomic_load_explicit(&pool->prepared, memory_order_acquire) < pool->maximum) {
                if (!atomic_exchange_explicit(&pool->refill_requested, true, memory_order_acq_rel))
                    llam_capsule_pool_wake(pool);
            }
            return node->object;
        }
        if (atomic_load_explicit(&pool->prepared, memory_order_acquire) >= pool->maximum) {
            errno = ENOMEM;
            return NULL;
        }
        if (!atomic_exchange_explicit(&pool->refill_requested, true, memory_order_acq_rel))
            llam_capsule_pool_wake(pool);
        if (!cold_allowed) { errno = EAGAIN; return NULL; }
        if (llam_capsule_pool_refill(pool) != 0) return NULL;
    }
    errno = EAGAIN;
    return NULL;
}

void llam_capsule_pool_put(llam_capsule_pool_t *pool, void *object) {
    if (pool == NULL || object == NULL || !pool->initialized) abort();
    llam_capsule_pool_node_t *node = (void *)((unsigned char *)object -
                                             offsetof(llam_capsule_pool_node_t, object));
    pthread_mutex_lock(&pool->lock);
    if (node->pool != pool || !node->in_use ||
        atomic_load_explicit(&pool->in_use, memory_order_relaxed) == 0U) abort();
    node->in_use = false;
    node->free_next = pool->free;
    pool->free = node;
    atomic_fetch_sub_explicit(&pool->in_use, 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&pool->available, 1U, memory_order_release);
    pthread_mutex_unlock(&pool->lock);
}

void llam_capsule_pool_destroy(llam_capsule_pool_t *pool) {
    if (pool == NULL || !pool->initialized) return;
    atomic_store_explicit(&pool->closing, true, memory_order_release);
    llam_capsule_pool_wake(pool);
    if (pool->cold_started && pthread_join(pool->cold_thread, NULL) != 0) abort();
    pthread_mutex_lock(&pool->build_lock);
    if (atomic_load_explicit(&pool->in_use, memory_order_acquire) != 0U) abort();
    llam_capsule_pool_node_t *node = pool->all;
    while (node != NULL) {
        llam_capsule_pool_node_t *next = node->all_next;
        if (pool->destroy != NULL) pool->destroy(node->object, pool->data);
        llam_capsule_pool_free_node(node);
        node = next;
    }
    pthread_mutex_unlock(&pool->build_lock);
#if LLAM_PLATFORM_DARWIN
    dispatch_release(pool->wake);
#elif LLAM_PLATFORM_WINDOWS
    if (!CloseHandle(pool->wake)) abort();
#else
    if (sem_destroy(&pool->wake) != 0) abort();
#endif
    pthread_mutex_destroy(&pool->build_lock);
    pthread_mutex_destroy(&pool->lock);
    pool->initialized = false;
}
