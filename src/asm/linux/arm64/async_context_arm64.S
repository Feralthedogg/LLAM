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
    .p2align 2
    .globl llam_linux_arm64_async_restore_scheduler
    .hidden llam_linux_arm64_async_restore_scheduler
    .type llam_linux_arm64_async_restore_scheduler, %function
llam_linux_arm64_async_restore_scheduler:
    /* Injected native PC, x0=M context. No use of the interrupted G stack. */
    mov x9, x0
    ldp d8, d9, [x9, #104]
    ldp d10, d11, [x9, #120]
    ldp d12, d13, [x9, #136]
    ldp d14, d15, [x9, #152]
    ldr x10, [x9, #168]
    msr fpcr, x10
    ldr x10, [x9, #176]
    msr fpsr, x10
    ldr x10, [x9, #0]
    ldp x19, x20, [x9, #8]
    ldp x21, x22, [x9, #24]
    ldp x23, x24, [x9, #40]
    ldp x25, x26, [x9, #56]
    ldp x27, x28, [x9, #72]
    ldp x29, x30, [x9, #88]
    mov sp, x10
    ret
    .size llam_linux_arm64_async_restore_scheduler, .-llam_linux_arm64_async_restore_scheduler

    .globl llam_linux_arm64_async_save_gate
    .hidden llam_linux_arm64_async_save_gate
    .type llam_linux_arm64_async_save_gate, %function
llam_linux_arm64_async_save_gate:
    /* Fresh M continuation. x0=M context, x1=atomic phase. */
    mov x9, sp
    str x9, [x0]
    stp x19, x20, [x0, #8]
    stp x21, x22, [x0, #24]
    stp x23, x24, [x0, #40]
    stp x25, x26, [x0, #56]
    stp x27, x28, [x0, #72]
    stp x29, x30, [x0, #88]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mrs x9, fpcr
    str x9, [x0, #168]
    mrs x9, fpsr
    str x9, [x0, #176]
    mov w9, #LLAM_ASYNC_BANK_GATE
    /* Controller acquire sees the COMPLETE saved M continuation. */
    stlr w9, [x1]
    .globl llam_linux_arm64_async_gate_begin
    .hidden llam_linux_arm64_async_gate_begin
llam_linux_arm64_async_gate_begin:
    yield
    b llam_linux_arm64_async_gate_begin
    .globl llam_linux_arm64_async_gate_end
    .hidden llam_linux_arm64_async_gate_end
llam_linux_arm64_async_gate_end:
    brk #0
    .size llam_linux_arm64_async_save_gate, .-llam_linux_arm64_async_save_gate

    .section .note.GNU-stack,"",%progbits
