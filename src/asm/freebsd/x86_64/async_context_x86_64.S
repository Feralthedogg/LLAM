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

#include "async_context_phase.h"

    .text
    .p2align 4
    .globl llam_freebsd_x86_async_restore_scheduler
    .hidden llam_freebsd_x86_async_restore_scheduler
    .type llam_freebsd_x86_async_restore_scheduler,@function
llam_freebsd_x86_async_restore_scheduler:
    /* Fresh sigreturn lands here without touching G's stack/red zone. */
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
    .size llam_freebsd_x86_async_restore_scheduler,.-llam_freebsd_x86_async_restore_scheduler

    .p2align 4
    .globl llam_freebsd_x86_async_save_gate
    .hidden llam_freebsd_x86_async_save_gate
    .type llam_freebsd_x86_async_save_gate,@function
llam_freebsd_x86_async_save_gate:
    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)
    stmxcsr 56(%rdi)
    fnstcw 60(%rdi)
    /* Aligned x86 store release-publishes the saved M context. */
    movl $LLAM_ASYNC_BANK_GATE, (%rsi)
    .globl llam_freebsd_x86_async_gate_begin
    .hidden llam_freebsd_x86_async_gate_begin
llam_freebsd_x86_async_gate_begin:
    pause
    jmp llam_freebsd_x86_async_gate_begin
    .globl llam_freebsd_x86_async_gate_end
    .hidden llam_freebsd_x86_async_gate_end
llam_freebsd_x86_async_gate_end:
    ud2
    .size llam_freebsd_x86_async_save_gate,.-llam_freebsd_x86_async_save_gate
    .section .note.GNU-stack,"",@progbits
