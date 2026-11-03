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

#include "async_context_phase.h"

    .text
    .p2align 4
    .globl _llam_darwin_x86_async_restore_scheduler
    .private_extern _llam_darwin_x86_async_restore_scheduler
_llam_darwin_x86_async_restore_scheduler:
    /* Injected PC; rdi names M.ctx. No push/call/store uses G's red zone. */
    movq %rdi, %r11
    ldmxcsr 56(%r11)
    fnclex
    fldcw 60(%r11)
    cld
    movq 8(%r11), %rbx
    movq 16(%r11), %rbp
    movq 24(%r11), %r12
    movq 32(%r11), %r13
    movq 40(%r11), %r14
    movq 48(%r11), %r15
    movq 0(%r11), %rsp
    ret

    .p2align 4
    .globl _llam_darwin_x86_async_save_gate
    .private_extern _llam_darwin_x86_async_save_gate
_llam_darwin_x86_async_save_gate:
    /* Ordinary call: rdi=M.ctx, rsi=atomic bank phase. Save the return slot. */
    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    stmxcsr 56(%rdi)
    fnstcw 60(%rdi)
    /* Aligned x86 stores publish prior context stores with release ordering. */
    movl $LLAM_ASYNC_BANK_GATE, (%rsi)
    .globl _llam_darwin_x86_async_gate_begin
    .private_extern _llam_darwin_x86_async_gate_begin
_llam_darwin_x86_async_gate_begin:
    pause
    jmp _llam_darwin_x86_async_gate_begin
    .globl _llam_darwin_x86_async_gate_end
    .private_extern _llam_darwin_x86_async_gate_end
_llam_darwin_x86_async_gate_end:
    ud2
