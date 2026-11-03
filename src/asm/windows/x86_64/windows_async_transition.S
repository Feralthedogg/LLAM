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

/*
 * Private experimental Windows x64 scheduler transitions, shared by the
 * production lifetime substrate and native qualification tests. Keep the
 * layout aligned with windows_context_x86_64.S; the ordinary hot switch is
 * intentionally untouched.
 */
    .set LLAM_WIN_CTX_SIMD_FLAGS_OFFSET, 260
    .set LLAM_WIN_CTX_SIMD_F_SKIP_SAVE, 1
    .set LLAM_WIN_CTX_SIMD_F_SKIP_RESTORE, 2

    .text
    .globl llam_windows_async_save_gate
llam_windows_async_save_gate:
    /* rcx=scheduler context, rdx=aligned atomic phase. Live native call. */
    movq %rdx, %r8
    testl $LLAM_WIN_CTX_SIMD_F_SKIP_SAVE, LLAM_WIN_CTX_SIMD_FLAGS_OFFSET(%rcx)
    jnz .Lprobe_saved_simd
    movq 248(%rcx), %r11
    testq %r11, %r11
    jnz .Lprobe_save_xsave
    cmpl $0, g_llam_fp_control_context(%rip)
    je .Lprobe_saved_fp
    stmxcsr 240(%rcx)
    fnstcw 244(%rcx)
    jmp .Lprobe_saved_fp
.Lprobe_save_xsave:
    movl g_llam_xsave_mask_lo(%rip), %eax
    movl g_llam_xsave_mask_hi(%rip), %edx
    xsave64 (%r11)
.Lprobe_saved_fp:
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
.Lprobe_saved_simd:
    movq %rsp, 0(%rcx)
    movq %rbx, 8(%rcx)
    movq %rbp, 16(%rcx)
    movq %rsi, 24(%rcx)
    movq %rdi, 32(%rcx)
    movq %r12, 40(%rcx)
    movq %r13, 48(%rcx)
    movq %r14, 56(%rcx)
    movq %r15, 64(%rcx)
    /* Aligned 32-bit store is atomic; x86 TSO publishes the saved frame. */
    movl $4, (%r8)
    .globl llam_windows_async_gate_spin_begin
llam_windows_async_gate_spin_begin:
    pause
    jmp llam_windows_async_gate_spin_begin
    .globl llam_windows_async_gate_spin_end
llam_windows_async_gate_spin_end:
    ud2

    .globl llam_windows_async_restore_scheduler
llam_windows_async_restore_scheduler:
    /* Injected RIP, not a call: no prologue or write on the interrupted G. */
    cld
    movq %rcx, %r10
    testl $LLAM_WIN_CTX_SIMD_F_SKIP_RESTORE, LLAM_WIN_CTX_SIMD_FLAGS_OFFSET(%r10)
    jnz .Lprobe_restored_simd
    cmpl $0, 256(%r10)
    jne .Lprobe_restore_valid_simd
    cmpl $0, g_llam_fp_control_context(%rip)
    je .Lprobe_restored_simd
    ldmxcsr 240(%r10)
    fldcw 244(%r10)
    jmp .Lprobe_restored_simd
.Lprobe_restore_valid_simd:
    movq 248(%r10), %r11
    testq %r11, %r11
    jnz .Lprobe_restore_xsave
    cmpl $0, g_llam_fp_control_context(%rip)
    je .Lprobe_restored_fp
    ldmxcsr 240(%r10)
    fldcw 244(%r10)
    jmp .Lprobe_restored_fp
.Lprobe_restore_xsave:
    movl g_llam_xsave_mask_lo(%rip), %eax
    movl g_llam_xsave_mask_hi(%rip), %edx
    xrstor64 (%r11)
.Lprobe_restored_fp:
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
.Lprobe_restored_simd:
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
    .globl llam_windows_async_restore_scheduler_end
llam_windows_async_restore_scheduler_end:
