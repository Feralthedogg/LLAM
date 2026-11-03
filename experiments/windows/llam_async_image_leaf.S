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

    .text
    .globl win_llam_async_image_leaf
win_llam_async_image_leaf:
    .globl win_llam_async_image_safe_begin
win_llam_async_image_safe_begin:
    movl g_win_llam_async_image_release(%rip), %eax
    testl %eax, %eax
    jnz win_llam_async_image_safe_end
    pause
    jmp win_llam_async_image_safe_begin
    .globl win_llam_async_image_safe_end
win_llam_async_image_safe_end:
    ret

#if !defined(LLAM_PE_METADATA_ABSENT)
    /* LLPM v1 fixture in a PE-image-compatible seven-byte section name.
     * The signed displacements are relative to their own 32-bit fields. */
    .section .llampm,"dr"
    .p2align 3
    .globl win_llam_async_image_metadata_begin
win_llam_async_image_metadata_begin:
#if defined(LLAM_PE_METADATA_BAD_MAGIC)
    .long 0
#else
    .long 0x4d504c4c
#endif
#if defined(LLAM_PE_METADATA_BAD_VERSION)
    .short 2
#else
    .short 1
#endif
    .short 1
    .long 32
#if defined(LLAM_PE_METADATA_BAD_LENGTH) || defined(LLAM_PE_METADATA_OVERLAP)
    .long 64
#else
    .long 48
#endif
    .quad 0x202609270001
#if defined(LLAM_PE_METADATA_OVERLAP)
    .long 2
#else
    .long 1
#endif
    .long 4
#if defined(LLAM_PE_METADATA_BAD_REL)
    .long 0x7fffffff
#else
    .long win_llam_async_image_safe_begin - .
#endif
    .long win_llam_async_image_safe_end - .
    .long 1
    .long 0
#if defined(LLAM_PE_METADATA_OVERLAP)
    .long win_llam_async_image_safe_begin - .
    .long win_llam_async_image_safe_end - .
    .long 1
    .long 0
#endif
#endif
