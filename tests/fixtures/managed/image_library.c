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

#include "cpu_workload.h"

typedef unsigned long long u64;
static unsigned *unload_counter;

void managed_image_observe_unload(unsigned *counter) { unload_counter = counter; }

u64 managed_image_leaf(u64 value) { return value * 7U + 3U; }

void managed_image_task(void *argument) {
    volatile u64 *values = argument;
    u64 value = values[0];
    for (u64 i = 0U; i < 10000U; ++i) value = (value * 17U) ^ (value >> 5U);
    values[1] = value;
}

__attribute__((destructor)) static void managed_image_unloaded(void) {
    if (unload_counter != (void *)0) ++*unload_counter;
}

__attribute__((noinline)) static u64 managed_cpu_linear(u64 value) {
    volatile u64 local[2];
    local[0] = value;
    local[1] = 7U;
    return local[0] * 3U + local[1];
}

void managed_image_cpu_task(void *argument) {
    llam_managed_cpu_workload_t *work = argument;
    volatile u64 lanes[8];
    for (unsigned i = 0U; i < 8U; ++i) lanes[i] = 0U;
    __atomic_store_n(&work->started, 1U, __ATOMIC_RELEASE);
    for (u64 i = 0U; i < work->iterations; ++i) {
        unsigned slot = (unsigned)(i & 7U);
        lanes[slot] += managed_cpu_linear(work->seed + i);
    }
    u64 result = 0U;
    for (unsigned i = 0U; i < 8U; ++i) result += lanes[i];
    work->result = result;
    __atomic_store_n(&work->finished, 1U, __ATOMIC_RELEASE);
}

void managed_image_cpu_observer(void *argument) {
    llam_managed_cpu_observation_t *observation = argument;
    while (!__atomic_load_n(&observation->first->started, __ATOMIC_ACQUIRE) ||
           !__atomic_load_n(&observation->second->started, __ATOMIC_ACQUIRE)) {
        /* The observer is ordinary no-yield managed code too. Waiting for G
         * startup must not retain the only P needed by an affine peer M. */
    }
    observation->both_started = 1U;
    observation->first_unfinished =
        !__atomic_load_n(&observation->first->finished, __ATOMIC_ACQUIRE);
    observation->both_unfinished = observation->first_unfinished &&
        !__atomic_load_n(&observation->second->finished, __ATOMIC_ACQUIRE);
}

void managed_image_keepalive(void *argument) {
    llam_managed_cpu_workload_t *work = argument;
    __atomic_store_n(&work->started, 1U, __ATOMIC_RELEASE);
    u64 value = work->seed;
    while (!__atomic_load_n(&work->finished, __ATOMIC_ACQUIRE))
        value = managed_cpu_linear(value);
    work->result = value;
}

#if defined(LLAM_MANAGED_AFFINE_FIXTURE)
static _Thread_local volatile u64 managed_image_thread_sum;
static _Thread_local unsigned managed_image_thread_cookie;

void managed_image_affine_task(void *argument) {
    llam_managed_cpu_workload_t *work = argument;
    if (managed_image_thread_cookie != 0U) {
        work->result = ~(u64)0;
        __atomic_store_n(&work->finished, 1U, __ATOMIC_RELEASE);
        return;
    }
    managed_image_thread_cookie = 1U;
    managed_image_thread_sum = 0U;
    __atomic_store_n(&work->started, 1U, __ATOMIC_RELEASE);
    for (u64 i = 0U; i < work->iterations; ++i)
        managed_image_thread_sum += managed_cpu_linear(work->seed + i);
    work->result = managed_image_thread_sum;
    __atomic_store_n(&work->finished, 1U, __ATOMIC_RELEASE);
}

/* Scalar declarations deliberately mirror the public ABI. Compiler contract
 * tests independently verify their shapes against the SDK headers. */
extern int llam_task_local_key_create(unsigned *key);
extern int llam_task_local_set_owned(unsigned key, void *value, void (*cleanup)(void *));
extern int llam_cancel_token_cancel(void *token);
extern int llam_task_set_result(void *value, int application_error);

static void managed_image_cleanup(void *argument) {
    llam_managed_cleanup_workload_t *work = argument;
    work->same_thread = work->tls_address == (void *)&managed_image_thread_cookie &&
                        managed_image_thread_cookie == 47U;
    work->cleaned += 1U;
}

void managed_image_cleanup_task(void *argument) {
    llam_managed_cleanup_workload_t *work = argument;
    managed_image_thread_cookie = 47U;
    work->tls_address = (void *)&managed_image_thread_cookie;
    if (llam_task_local_key_create(&work->key) != 0 ||
        llam_task_local_set_owned(work->key, work, managed_image_cleanup) != 0) {
        work->error = 1U;
        return;
    }
    u64 value = 3U;
    for (u64 i = 0U; i < 60000000U; ++i) value = managed_cpu_linear(value);
    work->result = value;
    if (llam_cancel_token_cancel(work->cancel_token) != 0 ||
        llam_task_set_result(work, 17) != 0) work->error = 2U;
}

#endif /* LLAM_MANAGED_AFFINE_FIXTURE */
