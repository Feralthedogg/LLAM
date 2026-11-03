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

#ifndef LLAM_RUNTIME_TEST_WATCHDOG_H
#define LLAM_RUNTIME_TEST_WATCHDOG_H

#include "llam_internal.h"

#if defined(LLAM_ENABLE_TEST_HOOKS)
enum {
    LLAM_WATCHDOG_TEST_NO_PAUSE = 0,
    LLAM_WATCHDOG_TEST_DEQUEUE,
    LLAM_WATCHDOG_TEST_RETURN
};
enum {
    LLAM_WATCHDOG_TEST_FATAL = 1,
    LLAM_WATCHDOG_TEST_ACTIVITY,
    LLAM_WATCHDOG_TEST_INCONCLUSIVE
};
typedef struct llam_watchdog_test_record {
    unsigned decision;
    unsigned streak;
    unsigned live;
    unsigned queues;
    unsigned active;
    unsigned listed;
    unsigned states[LLAM_TASK_STATE_DEAD + 1U];
    unsigned parked_mutex;
    unsigned parked_join;
    unsigned resolver_busy;
    unsigned truncated;
    uint64_t progress;
    uint64_t selected_id;
    unsigned selected_state;
    unsigned selected_listed;
    unsigned selected_seen;
    unsigned capture_ops;
    unsigned restore_ops;
    unsigned pending_phase;
    unsigned ready;
    unsigned mailbox;
    uint64_t lifetime_debt; /* Diagnostic only, NEVER execution progress. */
} llam_watchdog_test_record_t;

typedef struct llam_watchdog_test_gate {
    llam_runtime_t *runtime; /* Test retains this runtime until controller join. */
    unsigned point;
    atomic_uint reached; /* 0 unclaimed,1 copying,2 immutable M observation. */
    atomic_uint release;
    atomic_uint decision_ready;
    uint64_t selected_id;
    unsigned selected_state;
    unsigned selected_listed;
    llam_watchdog_test_record_t record;
} llam_watchdog_test_gate_t;

/* Test owns the gate through runtime destroy/controller join. Unregistering
 * stops new users but is NOT itself a last-reader/stack-lifetime proof. */
void llam_watchdog_test_set_gate(llam_watchdog_test_gate_t *gate);
void llam_watchdog_test_scheduler_point(llam_shard_t *shard, llam_task_t *task, unsigned point);
#endif

#endif
