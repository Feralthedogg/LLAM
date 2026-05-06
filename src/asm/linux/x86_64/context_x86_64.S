/**
 * @file src/asm/linux/x86_64/context_x86_64.S
 * @brief Linux x86_64 fiber context switch and task bootstrap assembly.
 *
 * @details
 * This file implements the hot context-switch ABI for Linux x86-64. The C
 * runtime owns task/stack allocation; assembly saves/restores callee-saved
 * registers and optional floating-point state described by @c nm_ctx_t.
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
     * Callee-saved GPRs plus optional FP control/xstate live in nm_ctx_t. The
     * layout offsets here must match src/internal/nm_internal.h.
     */
    .text
    .globl nm_ctx_switch
    .type nm_ctx_switch, @function
nm_ctx_switch:
    .cfi_startproc
    .cfi_def_cfa %rsp, 8
    movq 64(%rdi), %rcx
    testq %rcx, %rcx
    jnz .Lnm_ctx_save_xsave
#ifdef __PIC__
    movq g_nm_fp_control_context@GOTPCREL(%rip), %rax
    cmpl $0, (%rax)
#else
    cmpl $0, g_nm_fp_control_context(%rip)
#endif
    je .Lnm_ctx_saved
.Lnm_ctx_save_control:
    stmxcsr 56(%rdi)
    fnstcw 60(%rdi)
    jmp .Lnm_ctx_saved
.Lnm_ctx_save_xsave:
#ifdef __PIC__
    movq g_nm_xsave_mask_lo@GOTPCREL(%rip), %rax
    movl (%rax), %eax
    movq g_nm_xsave_mask_hi@GOTPCREL(%rip), %rdx
    movl (%rdx), %edx
#else
    movl g_nm_xsave_mask_lo(%rip), %eax
    movl g_nm_xsave_mask_hi(%rip), %edx
#endif
    xsave64 (%rcx)
    jmp .Lnm_ctx_saved
.Lnm_ctx_saved:
    movq %rsp, 0(%rdi)
    movq %rbx, 8(%rdi)
    movq %rbp, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %r13, 32(%rdi)
    movq %r14, 40(%rdi)
    movq %r15, 48(%rdi)

    movq 64(%rsi), %rcx
    testq %rcx, %rcx
    jnz .Lnm_ctx_restore_xsave
#ifdef __PIC__
    movq g_nm_fp_control_context@GOTPCREL(%rip), %rax
    cmpl $0, (%rax)
#else
    cmpl $0, g_nm_fp_control_context(%rip)
#endif
    je .Lnm_ctx_restored
.Lnm_ctx_restore_control:
    ldmxcsr 56(%rsi)
    fldcw 60(%rsi)
    jmp .Lnm_ctx_restored
.Lnm_ctx_restore_xsave:
#ifdef __PIC__
    movq g_nm_xsave_mask_lo@GOTPCREL(%rip), %rax
    movl (%rax), %eax
    movq g_nm_xsave_mask_hi@GOTPCREL(%rip), %rdx
    movl (%rdx), %edx
#else
    movl g_nm_xsave_mask_lo(%rip), %eax
    movl g_nm_xsave_mask_hi(%rip), %edx
#endif
    xrstor64 (%rcx)
    jmp .Lnm_ctx_restored
.Lnm_ctx_restored:
    movq 0(%rsi), %rsp
    movq 8(%rsi), %rbx
    movq 16(%rsi), %rbp
    movq 24(%rsi), %r12
    movq 32(%rsi), %r13
    movq 40(%rsi), %r14
    movq 48(%rsi), %r15
    ret
    .cfi_endproc

    .globl nm_fiber_bootstrap
    .type nm_fiber_bootstrap, @function
nm_fiber_bootstrap:
    /*
     * New task contexts enter here with r12 holding nm_task_t*. Align the stack
     * for the C ABI before calling nm_task_bootstrap().
     */
    .cfi_startproc
    .cfi_def_cfa %rsp, 8
    movq %r12, %rdi
    subq $8, %rsp
    .cfi_def_cfa_offset 16
    testq $0xF, %rsp
    jnz .Lnm_fiber_alignment_violation
    call nm_task_bootstrap
    addq $8, %rsp
    .cfi_def_cfa_offset 8
    ret

.Lnm_fiber_alignment_violation:
    movq %rsp, %rdi
    andq $-16, %rsp
    call nm_fiber_alignment_violation
    ud2
    .cfi_endproc

    .section .note.GNU-stack,"",@progbits
