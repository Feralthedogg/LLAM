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

#ifndef LLAM_ASYNC_PREEMPT_ABI_H
#define LLAM_ASYNC_PREEMPT_ABI_H

#define LLAM_ASYNC_PREEMPT_STATUS_OFFSET 0
#define LLAM_ASYNC_PREEMPT_NEXT_REQUEST_OFFSET 8
#define LLAM_ASYNC_PREEMPT_REQUEST_OFFSET 16
#define LLAM_ASYNC_PREEMPT_ACK_OFFSET 24
#define LLAM_ASYNC_PREEMPT_WORKER_OFFSET 32
#define LLAM_ASYNC_PREEMPT_TASK_OFFSET 40
#define LLAM_ASYNC_PREEMPT_PROCESSOR_OFFSET 48
#define LLAM_ASYNC_PREEMPT_WORKER_INCARNATION_OFFSET 56
#define LLAM_ASYNC_PREEMPT_WORKER_RUN_OFFSET 64
#define LLAM_ASYNC_PREEMPT_TASK_RUN_OFFSET 72
#define LLAM_ASYNC_PREEMPT_LEASE_EPOCH_OFFSET 80
#define LLAM_ASYNC_PREEMPT_P_GENERATION_OFFSET 88
#define LLAM_ASYNC_PREEMPT_XSTATE_AREA_OFFSET 96
#define LLAM_ASYNC_PREEMPT_XSTATE_CAPACITY_OFFSET 104
#define LLAM_ASYNC_PREEMPT_XSAVE_MASK_OFFSET 112
#define LLAM_ASYNC_PREEMPT_CAPTURED_XSTATE_SIZE_OFFSET 120
#define LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_OFFSET 128
#define LLAM_ASYNC_PREEMPT_TRAMPOLINE_STACK_OFFSET 136
#define LLAM_ASYNC_PREEMPT_CONTEXT_OFFSET 144
#define LLAM_ASYNC_PREEMPT_SCHEDULER_FXSTATE_OFFSET 328

#define LLAM_ASYNC_PREEMPT_GREG_R8_OFFSET 0
#define LLAM_ASYNC_PREEMPT_GREG_R9_OFFSET 8
#define LLAM_ASYNC_PREEMPT_GREG_R10_OFFSET 16
#define LLAM_ASYNC_PREEMPT_GREG_R11_OFFSET 24
#define LLAM_ASYNC_PREEMPT_GREG_R12_OFFSET 32
#define LLAM_ASYNC_PREEMPT_GREG_R13_OFFSET 40
#define LLAM_ASYNC_PREEMPT_GREG_R14_OFFSET 48
#define LLAM_ASYNC_PREEMPT_GREG_R15_OFFSET 56
#define LLAM_ASYNC_PREEMPT_GREG_RDI_OFFSET 64
#define LLAM_ASYNC_PREEMPT_GREG_RSI_OFFSET 72
#define LLAM_ASYNC_PREEMPT_GREG_RBP_OFFSET 80
#define LLAM_ASYNC_PREEMPT_GREG_RBX_OFFSET 88
#define LLAM_ASYNC_PREEMPT_GREG_RDX_OFFSET 96
#define LLAM_ASYNC_PREEMPT_GREG_RAX_OFFSET 104
#define LLAM_ASYNC_PREEMPT_GREG_RCX_OFFSET 112
#define LLAM_ASYNC_PREEMPT_GREG_RSP_OFFSET 120
#define LLAM_ASYNC_PREEMPT_GREG_RIP_OFFSET 128
#define LLAM_ASYNC_PREEMPT_GREG_RFLAGS_OFFSET 136

#define LLAM_ASYNC_PREEMPT_STATUS_IDLE 0
#define LLAM_ASYNC_PREEMPT_STATUS_WRITING 1
#define LLAM_ASYNC_PREEMPT_STATUS_REQUESTED 2
#define LLAM_ASYNC_PREEMPT_STATUS_CAPTURING 3
#define LLAM_ASYNC_PREEMPT_STATUS_CAPTURED 4
#define LLAM_ASYNC_PREEMPT_STATUS_LANDING 5

#define LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_LEGACY 1
#define LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_XSAVE 2
#define LLAM_ASYNC_PREEMPT_UC_FP_XSTATE 1U
#define LLAM_ASYNC_PREEMPT_UC_SIGCONTEXT_SS 2U
#define LLAM_ASYNC_PREEMPT_UC_STRICT_RESTORE_SS 4U

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#if defined(LLAM_ENABLE_TEST_HOOKS) && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT) && \
    defined(__linux__) && defined(__x86_64__)
struct llam_runtime;
struct llam_worker;
struct llam_async_preempt_state;
/* One-shot preparation failures; no hooks or failure state in production. */
enum {
    LLAM_ASYNC_TEST_FAIL_WORKER = 1,
    LLAM_ASYNC_TEST_FAIL_XSTATE,
    LLAM_ASYNC_TEST_FAIL_LANDING,
    LLAM_ASYNC_TEST_FAIL_RESTORE,
    LLAM_ASYNC_TEST_FAIL_TASK,
    LLAM_ASYNC_TEST_FAIL_BANK0,
    LLAM_ASYNC_TEST_FAIL_BANK1,
    LLAM_ASYNC_TEST_FAIL_BLOCK_JOB
};
void llam_async_preempt_test_fail_allocation(unsigned point);
void llam_async_preempt_test_fail_allocation_after(unsigned point, unsigned matches);
bool llam_async_preempt_test_frame_supported(const void *context);
bool llam_async_preempt_test_capture_xstate(struct llam_async_preempt_state *state,
                                            const void *context);
unsigned llam_async_preempt_test_stop_before_ready(unsigned state);
enum {
    LLAM_ASYNC_TEST_FAIL_THREAD_START = 1,
    LLAM_ASYNC_TEST_FAIL_WORKER_PROBE = 2,
    LLAM_ASYNC_TEST_HOLD_WORKER_PROBE = 3
};
void llam_async_preempt_test_fail_worker(unsigned kind, unsigned shard_id);
bool llam_async_preempt_test_take_worker_failure(unsigned kind, unsigned shard_id);
uint64_t llam_async_preempt_test_capsule_bytes(struct llam_runtime *runtime);
uint64_t llam_async_preempt_test_capsule_charge(struct llam_runtime *runtime);
unsigned llam_async_preempt_test_block_slots(struct llam_runtime *runtime);
bool llam_async_preempt_test_block_alloc_failure(struct llam_runtime *runtime);
unsigned llam_async_preempt_test_block_reserve_gate(unsigned state);
void llam_async_preempt_test_hold_normal_requests(bool hold);
bool llam_async_preempt_test_drain_guards(struct llam_worker *worker);
unsigned llam_async_preempt_test_prepare_gate(unsigned state);
#endif

typedef struct llam_async_preempt_context {
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rax;
    uint64_t rcx;
    uint64_t rsp;
    uint64_t rip;
    uint64_t rflags;
} llam_async_preempt_context_t;

typedef struct llam_async_preempt_state {
    atomic_uint status;
    uint32_t reserved0;
    atomic_uint_fast64_t next_request_generation;
    uint64_t request_generation;
    atomic_uint_fast64_t acknowledged_generation;
    struct llam_worker *worker;
    struct llam_task *task;
    struct llam_processor *processor;
    uint64_t worker_incarnation;
    uint64_t worker_run_generation;
    uint64_t task_run_generation;
    uint64_t lease_projection_epoch;
    uint64_t p_generation;
    void *xstate_area;
    size_t xstate_capacity;
    uint64_t xsave_mask;
    size_t captured_xstate_size;
    uint32_t xstate_format;
    uint32_t reserved1;
    uintptr_t trampoline_stack_top;
    llam_async_preempt_context_t context;
    void *trampoline_stack_mapping;
    size_t trampoline_stack_mapping_size;
    atomic_uint last_defer_reason;
    atomic_uint disabled;
    atomic_uint_fast64_t last_request_signal_ns;
    uint64_t request_started_ns;
    /* M-owned transient storage, never a suspended G continuation. */
    void *scheduler_fxstate;
    void *bridge_storage;
    void *captured_capsule;
    atomic_uintptr_t pending_restore;
    atomic_uint_fast64_t observed_restore_sequence;
    uint64_t next_restore_sequence;
    atomic_uint frame_probe_status;
    /* Origin service spans cooperative waits as well as outstanding capsules. */
    atomic_uint affine_live_g;
    atomic_uint ready_depth;
    /* Protected by this M's associated shard lock; never a stealable P queue. */
    struct llam_task *ready_head;
    struct llam_task *ready_tail;
    bool prefer_origin;
    bool drain_request;
    uint32_t request_owner_state;
    uint64_t request_run_generation;
    /* Immutable during one native-M run; no per-G protection-key policy. */
    uint32_t pkru_offset;
    uint32_t pkru_baseline;
} llam_async_preempt_state_t;

_Static_assert(offsetof(llam_async_preempt_state_t, scheduler_fxstate) == LLAM_ASYNC_PREEMPT_SCHEDULER_FXSTATE_OFFSET,
               "async preempt scheduler FP state offset mismatch");

_Static_assert(offsetof(llam_async_preempt_state_t, status) == LLAM_ASYNC_PREEMPT_STATUS_OFFSET,
               "async preempt state status offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, next_request_generation) == LLAM_ASYNC_PREEMPT_NEXT_REQUEST_OFFSET,
               "async preempt request generation offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, request_generation) == LLAM_ASYNC_PREEMPT_REQUEST_OFFSET,
               "async preempt request offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, acknowledged_generation) == LLAM_ASYNC_PREEMPT_ACK_OFFSET,
               "async preempt ack offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, xstate_area) == LLAM_ASYNC_PREEMPT_XSTATE_AREA_OFFSET,
               "async preempt xstate pointer offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, xstate_capacity) == LLAM_ASYNC_PREEMPT_XSTATE_CAPACITY_OFFSET,
               "async preempt xstate capacity offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, xsave_mask) == LLAM_ASYNC_PREEMPT_XSAVE_MASK_OFFSET,
               "async preempt xsave mask offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, captured_xstate_size) == LLAM_ASYNC_PREEMPT_CAPTURED_XSTATE_SIZE_OFFSET,
               "async preempt xstate size offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, xstate_format) == LLAM_ASYNC_PREEMPT_XSTATE_FORMAT_OFFSET,
               "async preempt xstate format offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, trampoline_stack_top) == LLAM_ASYNC_PREEMPT_TRAMPOLINE_STACK_OFFSET,
               "async preempt trampoline stack offset mismatch");
_Static_assert(offsetof(llam_async_preempt_state_t, context) == LLAM_ASYNC_PREEMPT_CONTEXT_OFFSET,
               "async preempt register block offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r8) == LLAM_ASYNC_PREEMPT_GREG_R8_OFFSET,
               "async preempt r8 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r9) == LLAM_ASYNC_PREEMPT_GREG_R9_OFFSET,
               "async preempt r9 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r10) == LLAM_ASYNC_PREEMPT_GREG_R10_OFFSET,
               "async preempt r10 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r11) == LLAM_ASYNC_PREEMPT_GREG_R11_OFFSET,
               "async preempt r11 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r12) == LLAM_ASYNC_PREEMPT_GREG_R12_OFFSET,
               "async preempt r12 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r13) == LLAM_ASYNC_PREEMPT_GREG_R13_OFFSET,
               "async preempt r13 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r14) == LLAM_ASYNC_PREEMPT_GREG_R14_OFFSET,
               "async preempt r14 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, r15) == LLAM_ASYNC_PREEMPT_GREG_R15_OFFSET,
               "async preempt r15 offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rdi) == LLAM_ASYNC_PREEMPT_GREG_RDI_OFFSET,
               "async preempt rdi offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rsi) == LLAM_ASYNC_PREEMPT_GREG_RSI_OFFSET,
               "async preempt rsi offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rbp) == LLAM_ASYNC_PREEMPT_GREG_RBP_OFFSET,
               "async preempt rbp offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rbx) == LLAM_ASYNC_PREEMPT_GREG_RBX_OFFSET,
               "async preempt rbx offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rdx) == LLAM_ASYNC_PREEMPT_GREG_RDX_OFFSET,
               "async preempt rdx offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rax) == LLAM_ASYNC_PREEMPT_GREG_RAX_OFFSET,
               "async preempt rax offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rcx) == LLAM_ASYNC_PREEMPT_GREG_RCX_OFFSET,
               "async preempt rcx offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rsp) == LLAM_ASYNC_PREEMPT_GREG_RSP_OFFSET,
               "async preempt rsp offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rip) == LLAM_ASYNC_PREEMPT_GREG_RIP_OFFSET,
               "async preempt rip offset mismatch");
_Static_assert(offsetof(llam_async_preempt_context_t, rflags) == LLAM_ASYNC_PREEMPT_GREG_RFLAGS_OFFSET,
               "async preempt rflags offset mismatch");

#endif /* !__ASSEMBLER__ */

#endif /* LLAM_ASYNC_PREEMPT_ABI_H */
