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

#ifndef LLAM_MANAGED_CATALOG_H
#define LLAM_MANAGED_CATALOG_H

#include "llam/managed.h"
typedef struct llam_llpm2_range llam_llpm2_range_t;
typedef struct llam_llpm2 llam_llpm2_t;

/* Included after runtime types. The runtime owns the catalog. A task admission
 * retains the entire catalog, including images that may occur in return frames.
 * Native readers retain it before publication and release only after draining. */
typedef struct llam_managed_admission {
    llam_managed_catalog_t *catalog;
    llam_task_fn entry;
    uint32_t execution_class;
    uint32_t spawn_flags;
    llam_worker_t *execution_worker;
    uint64_t execution_generation;
} llam_managed_admission_t;

int llam_managed_task_admit(llam_runtime_t *runtime, llam_task_fn entry, uint32_t spawn_flags,
                             llam_managed_admission_t *out);
bool llam_managed_catalog_retain(llam_managed_catalog_t *catalog);
void llam_managed_catalog_release(llam_managed_catalog_t *catalog);
/* Release task/admission references through these typed helpers so failed
 * legacy admission cannot leave a permanent cooperative fallback decision. */
void llam_managed_admission_release(llam_managed_admission_t *admission);
void llam_managed_task_release(llam_task_t *task);
/* Lifecycle hooks execute outside LLAM locks. destroy follows final G/native
 * drain and worker join; a failed EBUSY never releases an image pin. */
int llam_managed_catalog_run_begin(llam_runtime_t *runtime);
/* The final barrier commits policy before dispatch. adapter_ready means every
 * participating M completed its native probe; available_isa is their common
 * supported ISA intersection. REQUIRED failure never opens dispatch. */
int llam_managed_catalog_run_commit(llam_runtime_t *runtime, bool adapter_ready,
                                     uint64_t available_isa);
int llam_managed_catalog_async_commit(llam_runtime_t *runtime, uint64_t available_isa);
uint32_t llam_managed_catalog_policy(const llam_runtime_t *runtime);
uint64_t llam_managed_catalog_budget(const llam_runtime_t *runtime);
uint64_t llam_managed_catalog_required_isa(const llam_runtime_t *runtime);
bool llam_managed_catalog_async_enabled(const llam_runtime_t *runtime);
bool llam_managed_catalog_has_legacy_references(const llam_runtime_t *runtime);
void llam_managed_catalog_run_end(llam_runtime_t *runtime);
/* detach runs under the lifecycle gate without loader calls. NULL/errno=0
 * means absent; NULL/errno=EBUSY preserves the catalog and runtime storage.
 * retire runs after releasing every LLAM lock and never dereferences runtime. */
llam_managed_catalog_t *llam_managed_catalog_detach(llam_runtime_t *runtime);
void llam_managed_catalog_retire(llam_managed_catalog_t *catalog);
int llam_managed_catalog_destroy(llam_runtime_t *runtime);
/* Signal-safe binary search. Caller already owns a retained catalog or a run
 * hold; the returned range is borrowed for exactly that protected lifetime. */
const llam_llpm2_range_t *llam_managed_catalog_find_safe(
    const llam_managed_catalog_t *catalog, uintptr_t pc);
uint64_t llam_managed_task_type_id(uint16_t architecture, uint16_t native_abi);

#if defined(LLAM_ENABLE_TEST_HOOKS)
int llam_managed_catalog_test_register(llam_runtime_t *runtime, const llam_llpm2_t *record,
                                        llam_module_t **out);
#endif

#endif /* LLAM_MANAGED_CATALOG_H */
