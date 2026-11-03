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
    .globl win_llam_async_dual_leaf
win_llam_async_dual_leaf:
    movl $1, g_win_llam_async_dual_stage(%rip)
    .globl win_llam_async_dual_safe_begin
win_llam_async_dual_safe_begin:
    movl g_win_llam_async_dual_release(%rip), %eax
    testl $1, %eax
    jnz win_llam_async_dual_unsafe_begin
    pause
    jmp win_llam_async_dual_safe_begin
    .globl win_llam_async_dual_safe_end
win_llam_async_dual_safe_end:
    .globl win_llam_async_dual_unsafe_begin
win_llam_async_dual_unsafe_begin:
    movl $2, g_win_llam_async_dual_stage(%rip)
.Ldual_unsafe_spin:
    movl g_win_llam_async_dual_release(%rip), %eax
    testl $2, %eax
    jnz .Ldual_return
    pause
    jmp .Ldual_unsafe_spin
.Ldual_return:
    ret
