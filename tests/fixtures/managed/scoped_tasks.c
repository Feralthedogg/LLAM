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

#include "llam/scope.h"

static unsigned started;
static unsigned released;

/* No runtime call or safepoint in either rendezvous loop. A single worker must
 * capture one managed task to let the other pass the rendezvous. The finite
 * bound turns failure to preempt into a failed typed result instead of a hang. */
void sdk_scoped_value(void *argument) {
    (void)argument;
    __atomic_store_n(&started, 1U, __ATOMIC_RELEASE);
    for (uint64_t i = 0U; i != UINT64_C(500000000); ++i) {
        if (__atomic_load_n(&released, __ATOMIC_ACQUIRE)) break;
    }
    if (!__atomic_load_n(&released, __ATOMIC_ACQUIRE)) return;
    uint64_t number = 42U;
    const llam_type_descriptor_t type = LLAM_TYPE_U64_INIT;
    llam_value_t *value = NULL;
    if (llam_value_copy_ex(&type, sizeof(type), &number, &value) != 0) return;
    llam_outcome_t outcome = {LLAM_OUTCOME_OK, 0, value, 0U};
    if (llam_task_set_outcome_ex(&outcome, sizeof(outcome)) != 0)
        (void)llam_value_release(value);
}

void sdk_scoped_peer(void *argument) {
    (void)argument;
    for (uint64_t i = 0U; i != UINT64_C(500000000); ++i) {
        if (__atomic_load_n(&started, __ATOMIC_ACQUIRE)) break;
    }
    __atomic_store_n(&released, 1U, __ATOMIC_RELEASE);
}
