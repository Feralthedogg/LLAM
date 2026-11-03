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

#ifndef LLAM_ASYNC_CONTEXT_DARWIN_X86_64_H
#define LLAM_ASYNC_CONTEXT_DARWIN_X86_64_H

#if LLAM_PLATFORM_DARWIN && LLAM_ARCH_X86_64 && defined(LLAM_EXPERIMENTAL_ASYNC_PREEMPT)
#include <mach/mach.h>
#include <mach/i386/thread_status.h>
#include <signal.h>

/* The AVX flavor contains the complete legacy FP prefix and all YMM upper
 * halves. AVX512, CET and unknown enabled XSTATE have no qualified fallback. */
typedef struct llam_darwin_x86_async_machine {
    x86_thread_state64_t general;
    x86_avx_state64_t floating;
    thread_t origin_thread;
    thread_state_flavor_t floating_flavor;
    bool valid;
} llam_darwin_x86_async_machine_t;

typedef struct llam_darwin_x86_async_binding {
    thread_t thread; /* Owned send right until all bank/native operations drain. */
    thread_state_flavor_t floating_flavor;
    mach_msg_type_number_t floating_count;
    uint16_t code_selector;
    uint16_t fs_selector;
    uint16_t gs_selector;
    bool suspended; /* Exactly one suspend increment owned by this binding. */
    bool mutated;
    sigset_t previous_mask;
    bool mask_saved;
    bool restore_mask;
} llam_darwin_x86_async_binding_t;

int llam_darwin_x86_async_bind(llam_darwin_x86_async_binding_t *binding);
void llam_darwin_x86_async_close(llam_darwin_x86_async_binding_t *binding);
int llam_darwin_x86_async_suspend(llam_darwin_x86_async_binding_t *binding);
int llam_darwin_x86_async_read(llam_darwin_x86_async_binding_t *binding,
                                llam_darwin_x86_async_machine_t *out);
int llam_darwin_x86_async_redirect(llam_darwin_x86_async_binding_t *binding,
                                    const llam_darwin_x86_async_machine_t *captured,
                                    const llam_ctx_t *scheduler, uintptr_t begin, uintptr_t end,
                                    uintptr_t stack_low, uintptr_t stack_high);
int llam_darwin_x86_async_restore(llam_darwin_x86_async_binding_t *binding,
                                   const llam_darwin_x86_async_machine_t *captured);
void llam_darwin_x86_async_resume(llam_darwin_x86_async_binding_t *binding);
void llam_darwin_x86_async_restore_scheduler(void);
void llam_darwin_x86_async_save_gate(llam_ctx_t *scheduler, atomic_uint *phase);
extern const unsigned char llam_darwin_x86_async_gate_begin[];
extern const unsigned char llam_darwin_x86_async_gate_end[];
#endif

#endif /* LLAM_ASYNC_CONTEXT_DARWIN_X86_64_H */
