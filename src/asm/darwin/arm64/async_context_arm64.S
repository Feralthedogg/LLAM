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
    .p2align 2
    .globl _llam_darwin_async_restore_scheduler
    .private_extern _llam_darwin_async_restore_scheduler
_llam_darwin_async_restore_scheduler:
    /* Injected PC, not a C call. x0 names the live native M continuation.
     * Do not write to G's interrupted stack or restore an old G.ctx. */
    mov x9, x0
    ldr x10, [x9, #184]
    tbnz x10, #1, 1f
    ldp d8, d9, [x9, #104]
    ldp d10, d11, [x9, #120]
    ldp d12, d13, [x9, #136]
    ldp d14, d15, [x9, #152]
    ldr x10, [x9, #168]
    msr fpcr, x10
    ldr x10, [x9, #176]
    msr fpsr, x10
1:
    ldr x10, [x9, #0]
    ldp x19, x20, [x9, #8]
    ldp x21, x22, [x9, #24]
    ldp x23, x24, [x9, #40]
    ldp x25, x26, [x9, #56]
    ldp x27, x28, [x9, #72]
    ldp x29, x30, [x9, #88]
    mov sp, x10
    ret

    .globl _llam_darwin_async_save_gate
    .private_extern _llam_darwin_async_save_gate
_llam_darwin_async_save_gate:
    /* x0=M context, x1=bank phase. Publish GATE after every ABI save. */
    mov x9, sp
    str x9, [x0]
    stp x19, x20, [x0, #8]
    stp x21, x22, [x0, #24]
    stp x23, x24, [x0, #40]
    stp x25, x26, [x0, #56]
    stp x27, x28, [x0, #72]
    stp x29, x30, [x0, #88]
    ldr x10, [x0, #184]
    tbnz x10, #0, 2f
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mrs x9, fpcr
    str x9, [x0, #168]
    mrs x9, fpsr
    str x9, [x0, #176]
2:
    mov w9, #LLAM_ASYNC_BANK_GATE
    /* Release pairs with the controller's acquire before it reads M.ctx. */
    stlr w9, [x1]
    .globl _llam_darwin_async_gate_begin
    .private_extern _llam_darwin_async_gate_begin
_llam_darwin_async_gate_begin:
    yield
    b _llam_darwin_async_gate_begin
    .globl _llam_darwin_async_gate_end
    .private_extern _llam_darwin_async_gate_end
_llam_darwin_async_gate_end:
    brk #0
