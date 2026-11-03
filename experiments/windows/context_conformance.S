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

    .text
    .globl win_probe_leaf
    .seh_proc win_probe_leaf
win_probe_leaf:
    /* Windows nonvolatile state is saved with matching x64 unwind metadata. */
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    subq $168, %rsp
    .seh_stackalloc 168
    movdqa %xmm6, 0(%rsp)
    .seh_savexmm %xmm6, 0
    movdqa %xmm7, 16(%rsp)
    .seh_savexmm %xmm7, 16
    movdqa %xmm8, 32(%rsp)
    .seh_savexmm %xmm8, 32
    movdqa %xmm9, 48(%rsp)
    .seh_savexmm %xmm9, 48
    movdqa %xmm10, 64(%rsp)
    .seh_savexmm %xmm10, 64
    movdqa %xmm11, 80(%rsp)
    .seh_savexmm %xmm11, 80
    movdqa %xmm12, 96(%rsp)
    .seh_savexmm %xmm12, 96
    movdqa %xmm13, 112(%rsp)
    .seh_savexmm %xmm13, 112
    movdqa %xmm14, 128(%rsp)
    .seh_savexmm %xmm14, 128
    movdqa %xmm15, 144(%rsp)
    .seh_savexmm %xmm15, 144
    .seh_endprologue

    /* RCX=release, RDX=observation. The safe range begins after setup. */
    stmxcsr 40(%rdx)
    movl $0x3f80, 44(%rdx)
    ldmxcsr 44(%rdx)
    fnstcw 432(%rdx)
    movw $0x0b7f, 434(%rdx)
    fldcw 434(%rdx)
    movabsq $0x1122334455667788, %rax
    movq %rax, %xmm0
    punpcklqdq %xmm0, %xmm0
    movabsq $0x8877665544332211, %r8
    movq %r8, %xmm1
    punpcklqdq %xmm1, %xmm1
    vinsertf128 $1, %xmm1, %ymm0, %ymm0

    movabsq $0x0badc0de00000006, %r8
    movq %r8, %xmm6
    punpcklqdq %xmm6, %xmm6
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm6, %ymm6
    incq %r8
    movq %r8, %xmm7
    punpcklqdq %xmm7, %xmm7
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm7, %ymm7
    incq %r8
    movq %r8, %xmm8
    punpcklqdq %xmm8, %xmm8
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm8, %ymm8
    incq %r8
    movq %r8, %xmm9
    punpcklqdq %xmm9, %xmm9
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm9, %ymm9
    incq %r8
    movq %r8, %xmm10
    punpcklqdq %xmm10, %xmm10
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm10, %ymm10
    incq %r8
    movq %r8, %xmm11
    punpcklqdq %xmm11, %xmm11
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm11, %ymm11
    incq %r8
    movq %r8, %xmm12
    punpcklqdq %xmm12, %xmm12
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm12, %ymm12
    incq %r8
    movq %r8, %xmm13
    punpcklqdq %xmm13, %xmm13
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm13, %ymm13
    incq %r8
    movq %r8, %xmm14
    punpcklqdq %xmm14, %xmm14
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm14, %ymm14
    incq %r8
    movq %r8, %xmm15
    punpcklqdq %xmm15, %xmm15
    movq %r8, %r9
    notq %r9
    movq %r9, %xmm2
    punpcklqdq %xmm2, %xmm2
    vinsertf128 $1, %xmm2, %ymm15, %ymm15

    movabsq $0xabc0000000000001, %rbx
    movabsq $0xabc0000000000002, %rbp
    movabsq $0xabc0000000000003, %rsi
    movabsq $0xabc0000000000004, %rdi
    movabsq $0xabc0000000000005, %r12
    movabsq $0xabc0000000000006, %r13
    movabsq $0xabc0000000000007, %r14
    movabsq $0xabc0000000000008, %r15

    .globl win_probe_leaf_begin
win_probe_leaf_begin:
.Lwin_probe_spin:
    pause
    cmpl $0, (%rcx)
    je .Lwin_probe_spin
    .globl win_probe_leaf_end
win_probe_leaf_end:
    movq %rax, 0(%rdx)
    vmovdqu %ymm0, 8(%rdx)
    stmxcsr 44(%rdx)
    fnstcw 434(%rdx)
    movq %rbx, 48(%rdx)
    movq %rbp, 56(%rdx)
    movq %rsi, 64(%rdx)
    movq %rdi, 72(%rdx)
    movq %r12, 80(%rdx)
    movq %r13, 88(%rdx)
    movq %r14, 96(%rdx)
    movq %r15, 104(%rdx)
    vmovdqu %ymm6, 112(%rdx)
    vmovdqu %ymm7, 144(%rdx)
    vmovdqu %ymm8, 176(%rdx)
    vmovdqu %ymm9, 208(%rdx)
    vmovdqu %ymm10, 240(%rdx)
    vmovdqu %ymm11, 272(%rdx)
    vmovdqu %ymm12, 304(%rdx)
    vmovdqu %ymm13, 336(%rdx)
    vmovdqu %ymm14, 368(%rdx)
    vmovdqu %ymm15, 400(%rdx)
    ldmxcsr 40(%rdx)
    fldcw 432(%rdx)
    vzeroupper
    movdqa 0(%rsp), %xmm6
    movdqa 16(%rsp), %xmm7
    movdqa 32(%rsp), %xmm8
    movdqa 48(%rsp), %xmm9
    movdqa 64(%rsp), %xmm10
    movdqa 80(%rsp), %xmm11
    movdqa 96(%rsp), %xmm12
    movdqa 112(%rsp), %xmm13
    movdqa 128(%rsp), %xmm14
    movdqa 144(%rsp), %xmm15
    addq $168, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    ret
    .seh_endproc

    .globl win_restore_gate
    .seh_proc win_restore_gate
win_restore_gate:
    /* RCX points to the readiness flag; all tested state is clobbered first. */
    pushq %rbx
    .seh_pushreg %rbx
    pushq %rbp
    .seh_pushreg %rbp
    pushq %rsi
    .seh_pushreg %rsi
    pushq %rdi
    .seh_pushreg %rdi
    pushq %r12
    .seh_pushreg %r12
    pushq %r13
    .seh_pushreg %r13
    pushq %r14
    .seh_pushreg %r14
    pushq %r15
    .seh_pushreg %r15
    subq $168, %rsp
    .seh_stackalloc 168
    movdqa %xmm6, 0(%rsp)
    .seh_savexmm %xmm6, 0
    movdqa %xmm7, 16(%rsp)
    .seh_savexmm %xmm7, 16
    movdqa %xmm8, 32(%rsp)
    .seh_savexmm %xmm8, 32
    movdqa %xmm9, 48(%rsp)
    .seh_savexmm %xmm9, 48
    movdqa %xmm10, 64(%rsp)
    .seh_savexmm %xmm10, 64
    movdqa %xmm11, 80(%rsp)
    .seh_savexmm %xmm11, 80
    movdqa %xmm12, 96(%rsp)
    .seh_savexmm %xmm12, 96
    movdqa %xmm13, 112(%rsp)
    .seh_savexmm %xmm13, 112
    movdqa %xmm14, 128(%rsp)
    .seh_savexmm %xmm14, 128
    movdqa %xmm15, 144(%rsp)
    .seh_savexmm %xmm15, 144
    .seh_endprologue

    movq %rcx, %r11
    vxorps %ymm0, %ymm0, %ymm0
    vxorps %ymm6, %ymm6, %ymm6
    vxorps %ymm7, %ymm7, %ymm7
    vxorps %ymm8, %ymm8, %ymm8
    vxorps %ymm9, %ymm9, %ymm9
    vxorps %ymm10, %ymm10, %ymm10
    vxorps %ymm11, %ymm11, %ymm11
    vxorps %ymm12, %ymm12, %ymm12
    vxorps %ymm13, %ymm13, %ymm13
    vxorps %ymm14, %ymm14, %ymm14
    vxorps %ymm15, %ymm15, %ymm15
    movl $0x1f80, 160(%rsp)
    ldmxcsr 160(%rsp)
    fninit
    xorq %rbx, %rbx
    xorq %rbp, %rbp
    xorq %rsi, %rsi
    xorq %rdi, %rdi
    xorq %r12, %r12
    xorq %r13, %r13
    xorq %r14, %r14
    xorq %r15, %r15
    xorq %rcx, %rcx
    xorq %rdx, %rdx
    xorq %r8, %r8
    xorq %r9, %r9
    xorq %r10, %r10
    movl $1, %eax
    xchgl %eax, (%r11)
    .globl win_restore_gate_begin
win_restore_gate_begin:
.Lwin_restore_spin:
    pause
    jmp .Lwin_restore_spin
    .globl win_restore_gate_end
win_restore_gate_end:
    .seh_endproc

    /* Test-only transition to a private G stack; no unwind crosses it. */
    .globl win_enter_private_g
win_enter_private_g:
    /* RCX=G stack top, RDX=release, R8=observation, R9=native RSP cell. */
    pushq %rbx
    movq %r9, %rbx
    movq %rsp, (%rbx)
    movq %rcx, %rsp
    movq %rdx, %rcx
    movq %r8, %rdx
    subq $32, %rsp
    call win_probe_leaf
    addq $32, %rsp
    movq (%rbx), %rsp
    popq %rbx
    ret

    /* Static landing with Windows x64 entry alignment and unwind metadata. */
    .globl win_landing_entry
    .seh_proc win_landing_entry
win_landing_entry:
    subq $40, %rsp
    .seh_stackalloc 40
    .seh_endprologue
    cld
    ldmxcsr win_landing_mxcsr(%rip)
    fldcw win_landing_x87cw(%rip)
    call win_landing_c
    ud2
    .globl win_landing_entry_end
win_landing_entry_end:
    .seh_endproc

    .section .rdata
    .p2align 2
win_landing_mxcsr:
    .long 0x1f80
win_landing_x87cw:
    .short 0x037f
