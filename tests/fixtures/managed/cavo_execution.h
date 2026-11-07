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

#ifndef LLAM_CAVO_EXECUTION_FIXTURE_H
#define LLAM_CAVO_EXECUTION_FIXTURE_H
#include "llam/runtime.h"
#include "llam/executor.h"
#include <stdatomic.h>
#include <stdbool.h>
typedef struct cavo_execution_work {
    llam_runtime_t *runtime;
    llam_cancel_token_t *token;
    atomic_uint peer_ready, phase, probe_done, peer_done, errors, cancellation_seen;
    uint64_t iterations;
} cavo_execution_work_t;
typedef struct cavo_local_native_work cavo_local_native_work_t;
typedef struct cavo_local_native_child {
    cavo_local_native_work_t *work;
    uint64_t before, after, cleanup;
    unsigned value;
    atomic_uint ready;
} cavo_local_native_child_t;

struct cavo_local_native_work {
    llam_runtime_t *runtime;
    llam_local_executor_t *executor;
    llam_cancel_token_t *token;
    llam_task_local_key_t key;
    llam_blocking_fn identity;
    llam_task_cleanup_fn cleanup;
    llam_task_t *root;
    atomic_uint go, errors, checked, draining, release_children;
    uint64_t root_identity;
    unsigned shared_value;
};
#endif
