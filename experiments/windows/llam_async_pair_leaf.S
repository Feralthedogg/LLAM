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
    .globl win_llam_async_pair_leaf
    .seh_proc win_llam_async_pair_leaf
win_llam_async_pair_leaf:
    pushq %rbx
    .seh_pushreg %rbx
    pushq %r12
    .seh_pushreg %r12
    subq $40, %rsp
    .seh_stackalloc 40
    movdqa %xmm6, 0(%rsp)
    .seh_savexmm %xmm6, 0
    .seh_endprologue

    /* RCX points at one G's stable, 16-byte-aligned observation record. */
    movq %rcx, %r12
    stmxcsr 104(%r12)
    fnstcw 102(%r12)
    ldmxcsr 108(%r12)
    fldcw 112(%r12)
    movq 8(%r12), %rbx
    movq %rbx, %rax
    movq %rbx, %xmm0
    punpcklqdq %xmm0, %xmm0
    movq %rbx, %rdx
    notq %rdx
    movq %rdx, %xmm1
    punpcklqdq %xmm1, %xmm1
    vinsertf128 $1, %xmm1, %ymm0, %ymm0
    vmovdqu %ymm0, %ymm6

    .globl win_llam_async_pair_leaf_begin
win_llam_async_pair_leaf_begin:
    movl 0(%r12), %edx
    testl %edx, %edx
    jnz win_llam_async_pair_leaf_end
    pause
    jmp win_llam_async_pair_leaf_begin
    .globl win_llam_async_pair_leaf_end
win_llam_async_pair_leaf_end:
    /* Record before returning to C; ordinary calls would erase volatile state. */
    movq %rax, 16(%r12)
    movq %rbx, 24(%r12)
    vmovdqu %ymm0, 32(%r12)
    vmovdqu %ymm6, 64(%r12)
    stmxcsr 96(%r12)
    fnstcw 100(%r12)
    ldmxcsr 104(%r12)
    fldcw 102(%r12)
    vzeroupper
    movdqa 0(%rsp), %xmm6
    addq $40, %rsp
    popq %r12
    popq %rbx
    ret
    .seh_endproc
