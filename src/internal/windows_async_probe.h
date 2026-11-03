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

#ifndef LLAM_WINDOWS_ASYNC_PROBE_H
#define LLAM_WINDOWS_ASYNC_PROBE_H

#if !defined(LLAM_ENABLE_TEST_HOOKS) || !LLAM_PLATFORM_WINDOWS || !LLAM_ARCH_X86_64
#error "The Windows async scheduler probe is test-only and x64-only"
#endif

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum llam_windows_async_probe_phase {
    LLAM_WINDOWS_ASYNC_PROBE_IDLE = 0,
    LLAM_WINDOWS_ASYNC_PROBE_CAPTURED = 1,
    LLAM_WINDOWS_ASYNC_PROBE_ARRIVED = 2,
    LLAM_WINDOWS_ASYNC_PROBE_READY = 3,
    LLAM_WINDOWS_ASYNC_PROBE_GATE = 4,
    LLAM_WINDOWS_ASYNC_PROBE_RESTORED = 5,
    LLAM_WINDOWS_ASYNC_PROBE_FINISHED = 6,
    LLAM_WINDOWS_ASYNC_PROBE_FAILED = 7
};

typedef struct llam_windows_async_probe {
    atomic_uint phase; /* Assembly gate writes this field at byte offset zero. */
    llam_runtime_t *runtime;
    llam_task_t *task;
    llam_worker_t *worker;
    uint64_t capture_run_generation;
    uint64_t arrival_run_generation;
    uint64_t restore_run_generation;
    uintptr_t captured_rsp;
    int captured_errno;
    DWORD captured_last_error;
    int native_oracle_result;
    int (*native_oracle)(void);
    int scheduler_errno_before;
    unsigned scheduler_state_checks;
    atomic_uint reclaim_wait_seen;
    bool check_return_state;
    bool defer_requeue;
    struct llam_windows_async_probe *release_peer;
} llam_windows_async_probe_t;

_Static_assert(offsetof(llam_windows_async_probe_t, phase) == 0,
               "Windows async gate phase offset mismatch");
_Static_assert(sizeof(atomic_uint) == sizeof(uint32_t),
               "Windows async gate requires 32-bit atomic phase");

void llam_windows_async_probe_install(llam_windows_async_probe_t *probe);
void llam_windows_async_probe_install_pair(llam_windows_async_probe_t *first,
                                           llam_windows_async_probe_t *second);
void llam_windows_async_probe_arrival(llam_task_t *task);
void llam_windows_async_probe_record_scheduler_errno(llam_task_t *task,
                                                      int scheduler_errno);
bool llam_windows_async_probe_resume(llam_ctx_t *scheduler_ctx,
                                     llam_task_t *task);
void llam_windows_async_probe_scheduler_return(llam_shard_t *shard,
                                               llam_task_t *task);
uintptr_t llam_windows_async_probe_stack_rsp(llam_task_t *task);
void llam_windows_async_probe_note_reclaim_wait(llam_task_t *task);

void llam_windows_async_restore_scheduler(void);
void llam_windows_async_save_gate(llam_ctx_t *scheduler_ctx,
                                  atomic_uint *phase);
extern const unsigned char llam_windows_async_gate_spin_begin[];
extern const unsigned char llam_windows_async_gate_spin_end[];

#endif
