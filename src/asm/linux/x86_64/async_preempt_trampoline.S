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
 * See LICENSES/Apache-2.0.txt.
 */

#include "async_preempt_abi.h"

    .text
    .globl llam_async_preempt_trampoline
    .hidden llam_async_preempt_trampoline
    .type llam_async_preempt_trampoline, @function
llam_async_preempt_trampoline:
    /*
     * Signal return enters on the M's landing stack. Restore the scheduler's
     * FP environment before the first C instruction. The user context is in
     * a G-owned capsule; this stack is never saved as a G continuation.
     */
    andq $-16, %rsp
    cld
    movq LLAM_ASYNC_PREEMPT_SCHEDULER_FXSTATE_OFFSET(%rdi), %rax
    fxrstor64 (%rax)
    call llam_async_preempt_trampoline_c
    ud2
    .size llam_async_preempt_trampoline, .-llam_async_preempt_trampoline

    .globl llam_async_preempt_resume_bridge
    .hidden llam_async_preempt_resume_bridge
    .type llam_async_preempt_resume_bridge, @function
llam_async_preempt_resume_bridge:
    /* Entered by the ordinary context switch, with r12 holding M state. */
    andq $-16, %rsp
    movq %r12, %rdi
    call llam_async_preempt_restore_c
    ud2
    .size llam_async_preempt_resume_bridge, .-llam_async_preempt_resume_bridge

    .globl llam_async_preempt_restore
    .hidden llam_async_preempt_restore
    .type llam_async_preempt_restore, @function
llam_async_preempt_restore:
    /*
     * Linux x86-64 rt_sigreturn locates its frame at RSP - sizeof(long).
     * It restores the signal mask and complete admitted machine state as one
     * kernel return. This is a private, architecture-qualified backend, not
     * a call to libc's nonfunctional sigreturn wrapper.
     */
    leaq 8(%rdi), %rsp
    movl $15, %eax
    syscall
    ud2
    .size llam_async_preempt_restore, .-llam_async_preempt_restore

    .section .note.GNU-stack,"",@progbits
