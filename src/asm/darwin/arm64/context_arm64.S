/**
 * @file src/asm/darwin/arm64/context_arm64.S
 * @brief Darwin arm64 fiber context switch assembly.
 *
 * @details
 * Darwin arm64 uses the same logical context layout as Linux arm64, with Mach-O
 * symbol naming. The offsets must match the AArch64 @c llam_ctx_t layout in
 * src/internal/llam_internal.h.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

    .text
    .align 2
    .globl _llam_ctx_switch
_llam_ctx_switch:
    /*
     * x0 = context to save
     * x1 = context to restore
     *
     * Save stack pointer, x19-x30, d8-d15, and FP control/status registers.
     */
    mov x9, sp
    str x9, [x0, #0]
    stp x19, x20, [x0, #8]
    stp x21, x22, [x0, #24]
    stp x23, x24, [x0, #40]
    stp x25, x26, [x0, #56]
    stp x27, x28, [x0, #72]
    stp x29, x30, [x0, #88]
    str d8, [x0, #104]
    str d9, [x0, #112]
    str d10, [x0, #120]
    str d11, [x0, #128]
    str d12, [x0, #136]
    str d13, [x0, #144]
    str d14, [x0, #152]
    str d15, [x0, #160]
    mrs x9, fpcr
    str x9, [x0, #168]
    mrs x9, fpsr
    str x9, [x0, #176]

    ldr x9, [x1, #0]
    ldp x19, x20, [x1, #8]
    ldp x21, x22, [x1, #24]
    ldp x23, x24, [x1, #40]
    ldp x25, x26, [x1, #56]
    ldp x27, x28, [x1, #72]
    ldp x29, x30, [x1, #88]
    ldr d8, [x1, #104]
    ldr d9, [x1, #112]
    ldr d10, [x1, #120]
    ldr d11, [x1, #128]
    ldr d12, [x1, #136]
    ldr d13, [x1, #144]
    ldr d14, [x1, #152]
    ldr d15, [x1, #160]
    ldr x10, [x1, #168]
    msr fpcr, x10
    ldr x10, [x1, #176]
    msr fpsr, x10
    mov sp, x9
    ret

    .globl _llam_fiber_bootstrap
_llam_fiber_bootstrap:
    /*
     * llam_ctx_make_task seeds x19 with llam_task_t*. Enter C bootstrap, then jump
     * to task-exit cleanup when the user function returns.
     */
    mov x0, x19
    bl _llam_task_bootstrap
    b _llam_task_exit_internal
