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

#ifndef LLAM_RUNTIME_TEST_MANAGED_FOREIGN_H
#define LLAM_RUNTIME_TEST_MANAGED_FOREIGN_H

#include "llam/runtime.h"

#if defined(LLAM_ENABLE_TEST_HOOKS)
enum {
    LLAM_MANAGED_FOREIGN_TEST_PENDING = 1,
    LLAM_MANAGED_FOREIGN_TEST_QUIESCENT
};
typedef struct llam_managed_foreign_test_gate {
    llam_runtime_t *runtime;
    void (*observe)(llam_task_t *task, unsigned point, void *data);
    void *data;
} llam_managed_foreign_test_gate_t;

/* Test publishes once before run and retains the gate through every G's exit.
 * Clearing the pointer closes new users; runtime/G join supplies reclamation. */
void llam_managed_foreign_test_set_gate(llam_managed_foreign_test_gate_t *gate);
#endif

#endif /* LLAM_RUNTIME_TEST_MANAGED_FOREIGN_H */
