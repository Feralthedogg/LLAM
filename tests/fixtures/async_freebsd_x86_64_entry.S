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
    .p2align 4
    .globl llam_async_freebsd_fixture_entry
    .type llam_async_freebsd_fixture_entry,@function
llam_async_freebsd_fixture_entry:
    movq %r12, %rdi
    subq $8, %rsp
    call llam_async_freebsd_fixture_body
    ud2
    .size llam_async_freebsd_fixture_entry,.-llam_async_freebsd_fixture_entry
    .section .note.GNU-stack,"",@progbits
