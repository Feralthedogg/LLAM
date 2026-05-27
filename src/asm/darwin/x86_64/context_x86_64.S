/**
 * @file src/asm/darwin/x86_64/context_x86_64.S
 * @brief Darwin x86_64 fiber context switch and task bootstrap assembly.
 *
 * @details
 * Darwin x86_64 uses the System V register ABI with Mach-O symbol names. The
 * context layout matches the shared x86-64 @c llam_ctx_t definition in
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

    /*
     * Cooperative fiber switch:
     *   rdi = context to save
     *   rsi = context to restore
     *
     * Save/restore callee-saved GPRs and FP control state. Darwin keeps XSAVE
     * disabled in C today, but the null check keeps the layout compatible with
     * the Linux x86-64 path.
     */
    .text
    .globl _llam_ctx_switch
    .private_extern _llam_ctx_switch
    .p2align 4, 0x90
_llam_ctx_switch:
    .cfi_startproc
    .cfi_def_cfa %rsp, 8
    movq 64(%rdi), %rcx
    testq %rcx, %rcx
    jnz Lllam_ctx_save_xsave
    movq _g_llam_fp_control_context@GOTPCREL(%rip), %rax
    cmpl $0, (%rax)
    je Lllam_ctx_saved
Lllam_ctx_save_control:
    stmxcsr 56(%rdi)
    fnstcw 60(%rdi)
    jmp Lllam_ctx_saved
Lllam_ctx_save_xsave:
    movq _g_llam_xsave_mask_lo@GOTPCREL(%rip), %rax
    movl (%rax), %eax
    movq _g_llam_xsave_mask_hi@GOTPCREL(%rip), %rdx
    movl (%rdx), %edx
    xsave64 (%rcx)
    jmp Lllam_ctx_saved
Lllam_ctx_saved:
    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    movq 64(%rsi), %rcx
    testq %rcx, %rcx
    jnz Lllam_ctx_restore_xsave
    movq _g_llam_fp_control_context@GOTPCREL(%rip), %rax
    cmpl $0, (%rax)
    je Lllam_ctx_restored
Lllam_ctx_restore_control:
    ldmxcsr 56(%rsi)
    fldcw 60(%rsi)
    jmp Lllam_ctx_restored
Lllam_ctx_restore_xsave:
    movq _g_llam_xsave_mask_lo@GOTPCREL(%rip), %rax
    movl (%rax), %eax
    movq _g_llam_xsave_mask_hi@GOTPCREL(%rip), %rdx
    movl (%rdx), %edx
    xrstor64 (%rcx)
    jmp Lllam_ctx_restored
Lllam_ctx_restored:
    movq 0(%rsi), %rsp
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    ret
    .cfi_endproc

    .globl _llam_fiber_bootstrap
    .private_extern _llam_fiber_bootstrap
    .p2align 4, 0x90
_llam_fiber_bootstrap:
    /*
     * New task contexts enter here with r12 holding llam_task_t*. Align the stack
     * for the C ABI before calling llam_task_bootstrap().
     */
    .cfi_startproc
    .cfi_def_cfa %rsp, 8
    movq %r12, %rdi
    subq $8, %rsp
    .cfi_def_cfa_offset 16
    testq $0xF, %rsp
    jnz Lllam_fiber_alignment_violation
    call _llam_task_bootstrap
    addq $8, %rsp
    .cfi_def_cfa_offset 8
    ret

Lllam_fiber_alignment_violation:
    movq %rsp, %rdi
    andq $-16, %rsp
    call _llam_fiber_alignment_violation
    ud2
    .cfi_endproc
