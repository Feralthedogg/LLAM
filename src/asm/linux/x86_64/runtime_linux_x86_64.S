/**
 * @file src/asm/linux/x86_64/runtime_linux_x86_64.S
 * @brief Linux x86_64 futex and eventfd syscall helper assembly.
 *
 * @details
 * These tiny helpers remove function-call overhead from the hottest Linux wake
 * paths. They intentionally wrap only stable syscall/atomic sequences and leave
 * policy decisions in C.
 *
 * @copyright Copyright 2026 Feralthedogg
 *
 * @par License
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

    /*
     * Linux x86-64 fast paths:
     *   - atomically claim an eventfd wake bit,
     *   - issue FUTEX_WAIT_PRIVATE,
     *   - issue FUTEX_WAKE_PRIVATE.
     */
    .text

    .globl nm_eventfd_try_claim
    .type nm_eventfd_try_claim, @function
nm_eventfd_try_claim:
    .cfi_startproc
    xorl %eax, %eax
    movl $1, %edx
    lock cmpxchgl %edx, (%rdi)
    sete %al
    movzbl %al, %eax
    ret
    .cfi_endproc

    .globl nm_linux_futex_wait_private
    .type nm_linux_futex_wait_private, @function
nm_linux_futex_wait_private:
    /* syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, NULL, NULL, 0) */
    .cfi_startproc
    movl %esi, %edx
    movl $128, %esi
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    movl $202, %eax
    syscall
    ret
    .cfi_endproc

    .globl nm_linux_futex_wait_private_timeout
    .type nm_linux_futex_wait_private_timeout, @function
nm_linux_futex_wait_private_timeout:
    /* syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, timeout, NULL, 0) */
    .cfi_startproc
    movq %rdx, %r10
    movl %esi, %edx
    movl $128, %esi
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    movl $202, %eax
    syscall
    ret
    .cfi_endproc

    .globl nm_linux_futex_wake_private
    .type nm_linux_futex_wake_private, @function
nm_linux_futex_wake_private:
    /* syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0) */
    .cfi_startproc
    movl %esi, %edx
    movl $129, %esi
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    movl $202, %eax
    syscall
    ret
    .cfi_endproc

    .section .note.GNU-stack,"",@progbits
