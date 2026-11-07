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

#include "execution_controls.h"
#include "llam/executor.h"
#include "llam/managed.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "local SDK:%d: %s\n", __LINE__, #condition); _Exit(1); } } while (0)

void fixture_local_root(void *argument);
void execution_fixture_abort(void) { _Exit(125); }

static void *native_identity(void *argument) {
    (void)argument;
    return (void *)(uintptr_t)pthread_self();
}

static void native_cleanup(void *argument) {
    local_executor_native_child_t *child = argument;
    child->cleanup = (uint64_t)(uintptr_t)pthread_self();
    child->value += 1U;
}

static void *release_children(void *argument) {
    local_executor_native_work_t *work = argument;
    uint64_t limit = llam_now_ns() + UINT64_C(5000000000);
    while (!atomic_load_explicit(&work->draining, memory_order_acquire)) {
        REQUIRE(!atomic_load(&work->errors) && llam_now_ns() < limit);
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000L};
        nanosleep(&pause, NULL);
    }
    atomic_store_explicit(&work->release_children, 1U, memory_order_release);
    return NULL;
}

static void prepare(local_executor_native_work_t *work) {
    llam_runtime_opts_t runtime_options;
    llam_preempt_opts_t policy;
    llam_local_executor_opts_t executor_options;
    REQUIRE(llam_abi_version() >= ((2U << 16) | 6U));
    REQUIRE(llam_runtime_opts_init(&runtime_options, sizeof(runtime_options)) == 0);
    REQUIRE(llam_preempt_opts_init(&policy, sizeof(policy)) == 0);
    REQUIRE(llam_local_executor_opts_init(&executor_options, sizeof(executor_options)) == 0);
    runtime_options.worker_count = 1U;
    runtime_options.preempt_quantum_ns = UINT64_C(1000000);
    policy.native_affine_capacity = 1U;
    REQUIRE(llam_runtime_create_managed(&runtime_options, sizeof(runtime_options),
        &policy, sizeof(policy), &work->runtime) == 0);
    REQUIRE(llam_runtime_local_executor_create_ex(work->runtime, &executor_options,
        sizeof(executor_options), &work->executor) == 0);
    work->identity = native_identity;
    work->cleanup = native_cleanup;
    work->token = llam_runtime_cancel_token_create(work->runtime);
    REQUIRE(work->token != NULL && llam_task_local_key_create(&work->key) == 0);
}

int main(void) {
    local_executor_native_work_t work = {0};
    atomic_init(&work.go, 0U);
    atomic_init(&work.errors, 0U);
    atomic_init(&work.checked, 0U);
    atomic_init(&work.draining, 0U);
    atomic_init(&work.release_children, 0U);
    prepare(&work);
    llam_spawn_opts_t spawn;
    REQUIRE(llam_spawn_opts_init(&spawn, sizeof(spawn)) == 0);
    spawn.local_executor = work.executor;
    spawn.cancel_token = work.token;
    work.root = llam_runtime_spawn_ex(work.runtime, fixture_local_root, &work, &spawn, sizeof(spawn));
    REQUIRE(work.root != NULL);
    pthread_t release;
    REQUIRE(pthread_create(&release, NULL, release_children, &work) == 0);
    REQUIRE(llam_runtime_run_handle(work.runtime) == 0 && pthread_join(release, NULL) == 0);
    REQUIRE(llam_local_executor_drain(work.executor) == 0);
    REQUIRE(atomic_load(&work.checked) == 1U && atomic_load(&work.errors) == 0U);
    llam_runtime_stats_t stats;
    REQUIRE(llam_runtime_collect_stats_ex_handle(work.runtime, &stats, sizeof(stats)) == 0);
    REQUIRE(stats.preempt_yields != 0U);
    REQUIRE(llam_local_executor_destroy(work.executor) == 0 && llam_join(work.root) == 0);
    REQUIRE(llam_cancel_token_destroy(work.token) == 0 && llam_task_local_key_delete(work.key) == 0);
    llam_runtime_destroy(work.runtime);
    puts("installed SDK: async local executor, short regions and cancelled group drain passed");
    return 0;
}
