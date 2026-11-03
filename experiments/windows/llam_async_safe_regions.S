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

/* Four deterministic no-call regions; only [safe_begin,safe_end) is certified. */
    .text
    .globl win_llam_async_safe_regions
win_llam_async_safe_regions:
    movl $1, g_win_llam_safe_region_stage(%rip)
    .globl win_llam_async_unsafe_before_begin
win_llam_async_unsafe_before_begin:
    movl g_win_llam_safe_region_release(%rip), %eax
    testl $1, %eax
    jnz win_llam_async_safe_begin
    pause
    jmp win_llam_async_unsafe_before_begin

    .globl win_llam_async_safe_begin
win_llam_async_safe_begin:
    movl $2, g_win_llam_safe_region_stage(%rip)
    .globl win_llam_async_safe_spin
win_llam_async_safe_spin:
    movl g_win_llam_safe_region_release(%rip), %eax
    testl $2, %eax
    jnz win_llam_async_safe_end
    pause
    jmp win_llam_async_safe_spin

    .globl win_llam_async_safe_end
win_llam_async_safe_end:
    movl $3, g_win_llam_safe_region_stage(%rip)
    .globl win_llam_async_unsafe_after_spin
win_llam_async_unsafe_after_spin:
    movl g_win_llam_safe_region_release(%rip), %eax
    testl $4, %eax
    jnz .Lsafe_regions_return
    pause
    jmp win_llam_async_unsafe_after_spin
.Lsafe_regions_return:
    ret
