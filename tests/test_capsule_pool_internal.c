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
#include <stdio.h>

#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "capsule pool:%u: %s (errno=%d)\n", __LINE__, #expression, errno); \
    return -1; \
} } while (0)

typedef struct pool_object {
    void *payload;
    atomic_uint users;
    uint64_t cookie;
} pool_object_t;

typedef struct pool_case {
    llam_capsule_pool_t pool;
    pthread_t host;
    atomic_uint prepared;
    atomic_uint destroyed;
    atomic_uint cold_prepares;
    atomic_uint failures;
    bool fail_prepare;
} pool_case_t;

static int prepare_object(void *storage, void *argument) {
    pool_case_t *test = argument;
    pool_object_t *object = storage;
    object->payload = malloc(512U);
    if (object->payload == NULL) return -1;
    memset(object->payload, 0x5a, 512U);
    atomic_init(&object->users, 0U);
    object->cookie = UINT64_C(0x1122334455667788);
    atomic_fetch_add_explicit(&test->prepared, 1U, memory_order_relaxed);
    if (!pthread_equal(pthread_self(), test->host))
        atomic_fetch_add_explicit(&test->cold_prepares, 1U, memory_order_relaxed);
    if (test->fail_prepare) { errno = EIO; return -1; }
    return 0;
}

static void destroy_object(void *storage, void *argument) {
    pool_case_t *test = argument;
    pool_object_t *object = storage;
    free(object->payload);
    atomic_fetch_add_explicit(&test->destroyed, 1U, memory_order_relaxed);
}

static bool wait_available(llam_capsule_pool_t *pool) {
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) return false;
    do {
        if (atomic_load_explicit(&pool->available, memory_order_acquire) != 0U) return true;
        const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
        (void)nanosleep(&delay, NULL);
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return false;
    } while (now.tv_sec - start.tv_sec < 5);
    return false;
}

static int exhaustion_and_refill(void) {
    pool_case_t test = {.host = pthread_self()};
    CHECK(llam_capsule_pool_init(&test.pool, sizeof(pool_object_t), 512U, 1U, 1U,
                                  prepare_object, destroy_object, &test) == 0);
    size_t charge = test.pool.charge;
    CHECK(test.pool.maximum == 0U && llam_capsule_pool_take(&test.pool, false) == NULL && errno == ENOMEM);
    llam_capsule_pool_destroy(&test.pool);
    CHECK(llam_capsule_pool_init(&test.pool, sizeof(pool_object_t), 512U, 2U * charge, 1U,
                                  prepare_object, destroy_object, &test) == 0);
    CHECK(pthread_mutex_lock(&test.pool.lock) == 0);
    CHECK(llam_capsule_pool_take(&test.pool, false) == NULL && errno == EAGAIN);
    CHECK(pthread_mutex_unlock(&test.pool.lock) == 0);
    /* Holding the factory mutex proves a managed take cannot wait for it or
     * allocate a context itself. Only the dedicated cold thread may refill. */
    CHECK(pthread_mutex_lock(&test.pool.build_lock) == 0);
    pool_object_t *first = llam_capsule_pool_take(&test.pool, false);
    CHECK(first != NULL && ((uintptr_t)first & 63U) == 0U);
    CHECK(first->cookie == UINT64_C(0x1122334455667788));
    CHECK(llam_capsule_pool_take(&test.pool, false) == NULL && errno == EAGAIN);
    CHECK(atomic_load_explicit(&test.prepared, memory_order_acquire) == 1U);
    CHECK(pthread_mutex_unlock(&test.pool.build_lock) == 0 && wait_available(&test.pool));
    pool_object_t *second = llam_capsule_pool_take(&test.pool, false);
    CHECK(second != NULL && second != first);
    CHECK(atomic_load_explicit(&test.cold_prepares, memory_order_acquire) == 1U);
    CHECK(llam_capsule_pool_take(&test.pool, false) == NULL && errno == ENOMEM);
    llam_capsule_pool_put(&test.pool, second);
    pool_object_t *reused = llam_capsule_pool_take(&test.pool, false);
    CHECK(reused == second); /* The still-retained first object cannot recycle. */
    CHECK(atomic_load_explicit(&test.pool.prepared, memory_order_acquire) == 2U);
    llam_capsule_pool_put(&test.pool, reused);
    llam_capsule_pool_put(&test.pool, first);
    llam_capsule_pool_destroy(&test.pool);
    CHECK(atomic_load_explicit(&test.prepared, memory_order_acquire) == 2U &&
          atomic_load_explicit(&test.destroyed, memory_order_acquire) == 2U);
    return 0;
}

static int failed_preparation(void) {
    pool_case_t test = {.host = pthread_self(), .fail_prepare = true};
    CHECK(llam_capsule_pool_init(&test.pool, sizeof(pool_object_t), 512U, 4096U, 1U,
                                  prepare_object, destroy_object, &test) == -1 && errno == EIO);
    CHECK(!test.pool.initialized && atomic_load_explicit(&test.prepared, memory_order_acquire) == 1U &&
          atomic_load_explicit(&test.destroyed, memory_order_acquire) == 1U);
    return 0;
}

static void *reuse_worker(void *argument) {
    pool_case_t *test = argument;
    unsigned completed = 0U;
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) abort();
    while (completed < 500U) {
        pool_object_t *object = llam_capsule_pool_take(&test->pool, false);
        if (object == NULL) {
            if (errno != EAGAIN && errno != ENOMEM)
                atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed);
            const struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000L};
            (void)nanosleep(&delay, NULL);
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec - start.tv_sec >= 10) break;
            continue;
        }
        if (atomic_exchange_explicit(&object->users, 1U, memory_order_acq_rel) != 0U ||
            object->cookie != UINT64_C(0x1122334455667788) ||
            ((unsigned char *)object->payload)[511] != 0x5a)
            atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed);
        atomic_store_explicit(&object->users, 0U, memory_order_release);
        llam_capsule_pool_put(&test->pool, object);
        ++completed;
    }
    if (completed != 500U) atomic_fetch_add_explicit(&test->failures, 1U, memory_order_relaxed);
    return NULL;
}

static int concurrent_reuse(void) {
    pool_case_t test = {.host = pthread_self()};
    CHECK(llam_capsule_pool_init(&test.pool, sizeof(pool_object_t), 512U, 4096U, 2U,
                                  prepare_object, destroy_object, &test) == 0);
    pthread_t threads[8];
    for (unsigned i = 0U; i < 8U; ++i) CHECK(pthread_create(&threads[i], NULL, reuse_worker, &test) == 0);
    for (unsigned i = 0U; i < 8U; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(atomic_load_explicit(&test.failures, memory_order_acquire) == 0U &&
          atomic_load_explicit(&test.pool.in_use, memory_order_acquire) == 0U &&
          atomic_load_explicit(&test.pool.prepared, memory_order_acquire) <= test.pool.maximum);
    llam_capsule_pool_destroy(&test.pool);
    CHECK(atomic_load_explicit(&test.prepared, memory_order_acquire) ==
          atomic_load_explicit(&test.destroyed, memory_order_acquire));
    return 0;
}

int main(void) {
    if (exhaustion_and_refill() != 0 || failed_preparation() != 0 || concurrent_reuse() != 0) return 1;
    puts("capsule pool: bounded prepared admission, cold refill, retained object exclusion, failures and concurrent reuse passed");
    return 0;
}
