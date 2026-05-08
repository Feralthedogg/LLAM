/**
 * @file src/asm/windows/x86_64/context_x86_64.S
 * @brief Windows x86-64 fiber context switch assembly.
 *
 * @details
 * This path uses the Windows x64 calling convention and preserves all
 * callee-saved integer registers plus XMM6-XMM15. The C runtime still owns task
 * stack allocation, so Windows does not use the OS Fiber allocator here.
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

    .set LLAM_WIN_CTX_SIMD_FLAGS_OFFSET, 260
    .set LLAM_WIN_CTX_SIMD_F_SKIP_SAVE, 1
    .set LLAM_WIN_CTX_SIMD_F_SKIP_RESTORE, 2

    /*
     * Cooperative fiber switch, Windows x64 ABI:
     *   rcx = context to save
     *   rdx = context to restore
     */
    .text
    .globl llam_ctx_switch
llam_ctx_switch:
    movq %rdx, %r10

    testl $LLAM_WIN_CTX_SIMD_F_SKIP_SAVE, LLAM_WIN_CTX_SIMD_FLAGS_OFFSET(%rcx)
    jnz .Lllam_win_ctx_saved_simd
    movq 248(%rcx), %r11
    testq %r11, %r11
    jnz .Lllam_win_ctx_save_xsave
    cmpl $0, g_llam_fp_control_context(%rip)
    je .Lllam_win_ctx_saved_fp
.Lllam_win_ctx_save_control:
    stmxcsr 240(%rcx)
    fnstcw 244(%rcx)
    jmp .Lllam_win_ctx_saved_fp
.Lllam_win_ctx_save_xsave:
    movl g_llam_xsave_mask_lo(%rip), %eax
    movl g_llam_xsave_mask_hi(%rip), %edx
    xsave64 (%r11)
.Lllam_win_ctx_saved_fp:
    movdqa %xmm6, 80(%rcx)
    movdqa %xmm7, 96(%rcx)
    movdqa %xmm8, 112(%rcx)
    movdqa %xmm9, 128(%rcx)
    movdqa %xmm10, 144(%rcx)
    movdqa %xmm11, 160(%rcx)
    movdqa %xmm12, 176(%rcx)
    movdqa %xmm13, 192(%rcx)
    movdqa %xmm14, 208(%rcx)
    movdqa %xmm15, 224(%rcx)
    movl $1, 256(%rcx)

.Lllam_win_ctx_saved_simd:
    movq %rsp, 0(%rcx)
    movq %rbx, 8(%rcx)
    movq %rbp, 16(%rcx)
    movq %rsi, 24(%rcx)
    movq %rdi, 32(%rcx)
    movq %r12, 40(%rcx)
    movq %r13, 48(%rcx)
    movq %r14, 56(%rcx)
    movq %r15, 64(%rcx)

    testl $LLAM_WIN_CTX_SIMD_F_SKIP_RESTORE, LLAM_WIN_CTX_SIMD_FLAGS_OFFSET(%r10)
    jnz .Lllam_win_ctx_restored_simd
    cmpl $0, 256(%r10)
    je .Lllam_win_ctx_restored_simd
    movq 248(%r10), %r11
    testq %r11, %r11
    jnz .Lllam_win_ctx_restore_xsave
    cmpl $0, g_llam_fp_control_context(%rip)
    je .Lllam_win_ctx_restored_fp
.Lllam_win_ctx_restore_control:
    ldmxcsr 240(%r10)
    fldcw 244(%r10)
    jmp .Lllam_win_ctx_restored_fp
.Lllam_win_ctx_restore_xsave:
    movl g_llam_xsave_mask_lo(%rip), %eax
    movl g_llam_xsave_mask_hi(%rip), %edx
    xrstor64 (%r11)
.Lllam_win_ctx_restored_fp:
    movdqa 80(%r10), %xmm6
    movdqa 96(%r10), %xmm7
    movdqa 112(%r10), %xmm8
    movdqa 128(%r10), %xmm9
    movdqa 144(%r10), %xmm10
    movdqa 160(%r10), %xmm11
    movdqa 176(%r10), %xmm12
    movdqa 192(%r10), %xmm13
    movdqa 208(%r10), %xmm14
    movdqa 224(%r10), %xmm15

.Lllam_win_ctx_restored_simd:
    movq 0(%r10), %rsp
    movq 8(%r10), %rbx
    movq 16(%r10), %rbp
    movq 24(%r10), %rsi
    movq 32(%r10), %rdi
    movq 40(%r10), %r12
    movq 48(%r10), %r13
    movq 56(%r10), %r14
    movq 64(%r10), %r15
    ret

    .globl llam_fiber_bootstrap
llam_fiber_bootstrap:
    /*
     * New task contexts enter here with r12 holding llam_task_t*. The Windows
     * x64 ABI requires 32 bytes of shadow space and 16-byte stack alignment
     * before calls.
     */
    movq %r12, %rcx
    subq $40, %rsp
    testq $0xF, %rsp
    jnz .Lllam_win_fiber_alignment_violation
    call llam_task_bootstrap
    addq $40, %rsp
    ret

.Lllam_win_fiber_alignment_violation:
    movq %rsp, %rcx
    andq $-16, %rsp
    subq $32, %rsp
    call llam_fiber_alignment_violation
    ud2
