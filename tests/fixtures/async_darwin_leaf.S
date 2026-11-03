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

/* Reuse exactly the same register/arithmetic oracle for Linux AArch64. */
#if defined(__linux__)
#define _llam_async_darwin_leaf llam_async_darwin_leaf
#define _llam_async_darwin_leaf_begin llam_async_darwin_leaf_begin
#define _llam_async_darwin_leaf_end llam_async_darwin_leaf_end
#endif

    .text
    .p2align 2
    .globl _llam_async_darwin_leaf
_llam_async_darwin_leaf:
    sub sp, sp, #992
    str x18, [x1, #800]
    str x29, [x1, #808]
    str x30, [x1, #816]
    mov x9, sp
    str x9, [x1, #824]
    str x1, [sp, #800]
    add x16, sp, #808
    stp x19, x20, [x16, #0]
    stp x21, x22, [x16, #16]
    stp x23, x24, [x16, #32]
    stp x25, x26, [x16, #48]
    stp x27, x28, [x16, #64]
    stp x29, x30, [x16, #80]
    stp d8, d9, [x16, #96]
    stp d10, d11, [x16, #112]
    stp d12, d13, [x16, #128]
    stp d14, d15, [x16, #144]
    mrs x9, fpcr
    str x9, [sp, #968]
    mrs x9, fpsr
    str x9, [sp, #976]
    mov x9, #0x800000
    msr fpcr, x9
    mov x9, #0x8000000
    msr fpsr, x9
    mov x9, #0xa0000000
    msr nzcv, x9
    mov x1, #0
    mov x2, #20738
    mov x3, #20739
    mov x4, #20740
    mov x5, #20741
    mov x6, #20742
    mov x7, #20743
    mov x8, #20744
    mov x9, #20745
    mov x10, #20746
    mov x11, #20747
    mov x12, #20748
    mov x13, #20749
    mov x14, #20750
    mov x15, #20751
    mov x16, #20752
    mov x17, #20753
    mov x19, #20755
    mov x20, #20756
    mov x21, #20757
    mov x22, #20758
    mov x23, #20759
    mov x24, #20760
    mov x25, #20761
    mov x26, #20762
    mov x27, #20763
    mov x28, #20764
    movi v0.16b, #1
    movi v1.16b, #2
    movi v2.16b, #3
    movi v3.16b, #4
    movi v4.16b, #5
    movi v5.16b, #6
    movi v6.16b, #7
    movi v7.16b, #8
    movi v8.16b, #9
    movi v9.16b, #10
    movi v10.16b, #11
    movi v11.16b, #12
    movi v12.16b, #13
    movi v13.16b, #14
    movi v14.16b, #15
    movi v15.16b, #16
    movi v16.16b, #17
    movi v17.16b, #18
    movi v18.16b, #19
    movi v19.16b, #20
    movi v20.16b, #21
    movi v21.16b, #22
    movi v22.16b, #23
    movi v23.16b, #24
    movi v24.16b, #25
    movi v25.16b, #26
    movi v26.16b, #27
    movi v27.16b, #28
    movi v28.16b, #29
    movi v29.16b, #30
    movi v30.16b, #31
    movi v31.16b, #32

    .globl _llam_async_darwin_leaf_begin
_llam_async_darwin_leaf_begin:
    /* Finite no-call/no-yield arithmetic. No flag-setting loop instructions. */
    add x1, x1, x0
    sub x0, x0, #1
    cbnz x0, _llam_async_darwin_leaf_begin
    .globl _llam_async_darwin_leaf_end
_llam_async_darwin_leaf_end:
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mov x9, sp
    str x9, [sp, #248]
    mrs x9, fpcr
    str x9, [sp, #256]
    mrs x9, fpsr
    str x9, [sp, #264]
    mrs x9, nzcv
    str x9, [sp, #272]
    stp q0, q1, [sp, #288]
    stp q2, q3, [sp, #320]
    stp q4, q5, [sp, #352]
    stp q6, q7, [sp, #384]
    stp q8, q9, [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]
    mov x9, sp
    ldr x12, [sp, #800]
    mov x13, #50
Lsnapshot_copy:
    ldp x10, x11, [x9], #16
    stp x10, x11, [x12], #16
    sub x13, x13, #1
    cbnz x13, Lsnapshot_copy
    ldr x9, [sp, #968]
    msr fpcr, x9
    ldr x9, [sp, #976]
    msr fpsr, x9
    add x16, sp, #808
    ldp d8, d9, [x16, #96]
    ldp d10, d11, [x16, #112]
    ldp d12, d13, [x16, #128]
    ldp d14, d15, [x16, #144]
    ldp x19, x20, [x16, #0]
    ldp x21, x22, [x16, #16]
    ldp x23, x24, [x16, #32]
    ldp x25, x26, [x16, #48]
    ldp x27, x28, [x16, #64]
    ldp x29, x30, [x16, #80]
    mov x0, x1
    add sp, sp, #992
    ret

#if defined(__linux__)
    .section .note.GNU-stack,"",%progbits
#endif
