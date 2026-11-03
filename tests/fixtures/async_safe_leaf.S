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

/* Linux x86-64 experimental leaf: saves its callee-saved probe before the range. */
    .text
    .globl llam_async_safe_leaf
    .globl llam_async_safe_leaf_begin
    .globl llam_async_safe_leaf_end
    .type llam_async_safe_leaf, @function
llam_async_safe_leaf:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq $0x10203040, %rax
    movq $0x20304050, %rbx
    movq $0x30405060, %rdx
    movq $0x40506070, %rsi
    movq $0x50607010, %rbp
    movq $0x60701020, %r8
    movq $0x70102030, %r9
    movq $0x11223344, %r10
    movq $0x55667788, %r11
    movq $0x12345678, %r12
    movq $0x23456701, %r13
    movq $0x34567012, %r14
    movq $0x45670123, %r15
    movdqu .Lllam_async_vector(%rip), %xmm0
    movdqu %xmm0, -64(%rsp)
    xorl %ecx, %ecx
    stc
llam_async_safe_leaf_begin:
.Lllam_async_leaf_loop:
    /* MOV/JRCXZ/PAUSE/JMP preserve CF and ZF across every no-poll iteration. */
    movl (%rdi), %ecx
    jrcxz .Lllam_async_leaf_wait
    jmp .Lllam_async_leaf_done
.Lllam_async_leaf_wait:
    pause
    jmp .Lllam_async_leaf_loop
.Lllam_async_leaf_done:
    pushfq
    popq %rcx
    andq $0x41, %rcx
    cmpq $0x41, %rcx
    jne .Lllam_async_leaf_register_failure
    cmpq $0x10203040, %rax
    jne .Lllam_async_leaf_register_failure
    cmpq $0x20304050, %rbx
    jne .Lllam_async_leaf_register_failure
    cmpq $0x30405060, %rdx
    jne .Lllam_async_leaf_register_failure
    cmpq $0x40506070, %rsi
    jne .Lllam_async_leaf_register_failure
    cmpq $0x50607010, %rbp
    jne .Lllam_async_leaf_register_failure
    cmpq $0x60701020, %r8
    jne .Lllam_async_leaf_register_failure
    cmpq $0x70102030, %r9
    jne .Lllam_async_leaf_register_failure
    cmpq $0x11223344, %r10
    jne .Lllam_async_leaf_register_failure
    cmpq $0x55667788, %r11
    jne .Lllam_async_leaf_register_failure
    cmpq $0x12345678, %r12
    jne .Lllam_async_leaf_register_failure
    cmpq $0x23456701, %r13
    jne .Lllam_async_leaf_register_failure
    cmpq $0x34567012, %r14
    jne .Lllam_async_leaf_register_failure
    cmpq $0x45670123, %r15
    jne .Lllam_async_leaf_register_failure
    movdqu -64(%rsp), %xmm1
    pcmpeqb .Lllam_async_vector(%rip), %xmm1
    pmovmskb %xmm1, %ecx
    cmpl $0xffff, %ecx
    jne .Lllam_async_leaf_register_failure
    pcmpeqb .Lllam_async_vector(%rip), %xmm0
    pmovmskb %xmm0, %ecx
    cmpl $0xffff, %ecx
    jne .Lllam_async_leaf_register_failure
    movl $1, %eax
    jmp .Lllam_async_leaf_return
.Lllam_async_leaf_register_failure:
    xorl %eax, %eax
.Lllam_async_leaf_return:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret
llam_async_safe_leaf_end:
    .size llam_async_safe_leaf, .-llam_async_safe_leaf

    .section .rodata
    .p2align 4
.Lllam_async_vector:
    .byte 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87
    .byte 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f

    .p2align 5
.Lllam_async_avx_vector:
    .byte 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87
    .byte 0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f
    .byte 0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87
    .byte 0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f

    .text
    .globl llam_async_avx_leaf
    .globl llam_async_avx_leaf_begin
    .globl llam_async_avx_leaf_end
    .type llam_async_avx_leaf, @function
llam_async_avx_leaf:
    vmovdqu .Lllam_async_avx_vector(%rip), %ymm0
llam_async_avx_leaf_begin:
.Lllam_async_avx_loop:
    cmpl $0, (%rdi)
    jne .Lllam_async_avx_done
    pause
    jmp .Lllam_async_avx_loop
.Lllam_async_avx_done:
    vextractf128 $1, %ymm0, %xmm1
    pcmpeqb .Lllam_async_avx_vector+16(%rip), %xmm1
    pmovmskb %xmm1, %ecx
    cmpl $0xffff, %ecx
    jne .Lllam_async_avx_failed
    pcmpeqb .Lllam_async_avx_vector(%rip), %xmm0
    pmovmskb %xmm0, %ecx
    cmpl $0xffff, %ecx
    jne .Lllam_async_avx_failed
    movl $1, %eax
    jmp .Lllam_async_avx_return
.Lllam_async_avx_failed:
    xorl %eax, %eax
.Lllam_async_avx_return:
    vzeroupper
    ret
llam_async_avx_leaf_end:
    .size llam_async_avx_leaf, .-llam_async_avx_leaf

    .text
    .globl llam_async_unsafe_leaf
    .type llam_async_unsafe_leaf, @function
llam_async_unsafe_leaf:
.Lllam_async_unsafe_loop:
    movl (%rdi), %eax
    testl %eax, %eax
    jne .Lllam_async_unsafe_done
    pause
    jmp .Lllam_async_unsafe_loop
.Lllam_async_unsafe_done:
    ret
    .size llam_async_unsafe_leaf, .-llam_async_unsafe_leaf
    .section .note.GNU-stack,"",@progbits
