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

#ifndef LLAM_LOCAL_EXECUTOR_INTERNAL_H
#define LLAM_LOCAL_EXECUTOR_INTERNAL_H
#include "structured_internal.h"
#include "llam/executor.h"
#include "managed_scheduler.h"
#include "managed_catalog.h"

struct llam_local_executor {
    llam_structured_object_t object;
    struct llam_local_executor *next;
    llam_worker_t *worker;
    uint64_t generation;
    uint64_t incarnation;
    size_t pending;
    llam_channel_t *done;
    bool closed, started, native_joined, notified, draining, shutdown_deferred, listed;
};

typedef struct llam_local_executor_binding {
    llam_local_executor_t *domain;
    llam_worker_t *worker;
    uint64_t generation;
} llam_local_executor_binding_t;

int llam_local_executor_publish(llam_local_executor_t *domain);
bool llam_local_executor_finish_locked(llam_local_executor_t *domain);
void llam_local_executor_notify(llam_local_executor_t *domain, bool notify);
void llam_local_executor_unlink_locked(llam_local_executor_t *domain);
int llam_local_executor_admit(llam_runtime_t *runtime, llam_local_executor_t *handle,
    llam_local_executor_t **out, llam_worker_t **worker, uint64_t *generation);
void llam_local_executor_admission_release(llam_local_executor_t *domain, bool retired);
#endif
