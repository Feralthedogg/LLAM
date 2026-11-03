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
    .globl win_llam_async_leaf
win_llam_async_leaf:
    /* Deliberately no stack or library access within the certified interval. */
    .globl win_llam_async_leaf_begin
win_llam_async_leaf_begin:
    movl g_win_llam_async_release(%rip), %eax
    testl %eax, %eax
    jnz win_llam_async_leaf_end
    pause
    jmp win_llam_async_leaf_begin
    .globl win_llam_async_leaf_end
win_llam_async_leaf_end:
    ret
