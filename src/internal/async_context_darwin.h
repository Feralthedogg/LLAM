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

#ifndef LLAM_ASYNC_CONTEXT_DARWIN_H
#define LLAM_ASYNC_CONTEXT_DARWIN_H

#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_AARCH64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <mach/mach.h>
#include <mach/arm/thread_status.h>

/* Thread state is native and origin-M-only, never a portable serialized ABI. */
typedef struct llam_darwin_async_machine {
    arm_thread_state64_t general;
    arm_neon_state64_t neon;
    thread_t origin_thread; /* Identity only while caller retains that send right. */
    bool valid;
} llam_darwin_async_machine_t;

typedef struct llam_darwin_async_transaction {
    thread_t thread; /* Borrowed send right, held by the binding/caller. */
    bool suspended;  /* Exactly one suspend increment owned by this operation. */
    bool mutated;    /* Any committed flavor write makes uncertainty terminal. */
} llam_darwin_async_transaction_t;

/* tx must be zero-initialized or a completed transaction from this API. */
int llam_darwin_async_suspend(thread_t thread, llam_darwin_async_transaction_t *tx);
int llam_darwin_async_read(llam_darwin_async_transaction_t *tx,
                            llam_darwin_async_machine_t *out);
int llam_darwin_async_redirect(llam_darwin_async_transaction_t *tx,
                                const llam_darwin_async_machine_t *captured,
                                const llam_ctx_t *scheduler,
                                uintptr_t pc_begin, uintptr_t pc_end,
                                uintptr_t stack_low, uintptr_t stack_high);
int llam_darwin_async_write(llam_darwin_async_transaction_t *tx,
                             const llam_darwin_async_machine_t *captured);
void llam_darwin_async_resume(llam_darwin_async_transaction_t *tx);
uintptr_t llam_darwin_async_pc(const llam_darwin_async_machine_t *machine);
uintptr_t llam_darwin_async_sp(const llam_darwin_async_machine_t *machine);

void llam_darwin_async_restore_scheduler(void);
void llam_darwin_async_save_gate(llam_ctx_t *scheduler, atomic_uint *phase);
extern const unsigned char llam_darwin_async_gate_begin[];
extern const unsigned char llam_darwin_async_gate_end[];
#if defined(LLAM_ENABLE_TEST_HOOKS)
enum {
    LLAM_DARWIN_ASYNC_TEST_FAIL_READ = 1,
    LLAM_DARWIN_ASYNC_TEST_FAIL_NEON,
    LLAM_DARWIN_ASYNC_TEST_FAIL_GENERAL,
    LLAM_DARWIN_ASYNC_TEST_FAIL_RESUME,
    LLAM_DARWIN_ASYNC_TEST_FAIL_SME_QUERY,
    LLAM_DARWIN_ASYNC_TEST_FAIL_OBSERVE
};
void llam_darwin_async_test_fail(unsigned point);
unsigned llam_darwin_async_test_pending_failure(void);
#endif
#endif

#endif
