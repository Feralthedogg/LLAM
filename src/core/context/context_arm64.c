/**
 * @file src/core/context/context_arm64.c
 * @brief arm64 context bootstrap helpers shared by Linux and Darwin assembly paths.
 *
 * @details
 * AArch64 context switching is implemented in assembly, but initial task context
 * construction is easier to keep in C. The assembly switch restores callee-saved
 * registers and branches through the saved link register, so this file prepares
 * a stack pointer, bootstrap link register, and task pointer register.
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

#include "runtime_internal.h"

#if LLAM_ARCH_AARCH64

/**
 * @brief Build an initial AArch64 task context.
 *
 * @param ctx        Context object to initialize.
 * @param stack_base Usable stack base.
 * @param stack_size Usable stack size.
 * @param task       Task pointer passed in callee-saved register x19.
 * @return 0 on success, -1 on invalid arguments.
 */
int llam_ctx_make_task(llam_ctx_t *ctx, void *stack_base, size_t stack_size, llam_task_t *task) {
    uintptr_t stack_top;

    if (ctx == NULL || stack_base == NULL || stack_size == 0U || task == NULL) {
        errno = EINVAL;
        return -1;
    }

    stack_top = (uintptr_t)stack_base + stack_size;
    stack_top &= ~(uintptr_t)0xFUL;

    // The AArch64 ABI requires 16-byte stack alignment. The assembly bootstrap
    // expects x19 to contain the task pointer and lr to enter llam_fiber_bootstrap.
    ctx->sp = (uint64_t)stack_top;
    ctx->x19 = (uint64_t)(uintptr_t)task;
    ctx->x20 = 0U;
    ctx->x21 = 0U;
    ctx->x22 = 0U;
    ctx->x23 = 0U;
    ctx->x24 = 0U;
    ctx->x25 = 0U;
    ctx->x26 = 0U;
    ctx->x27 = 0U;
    ctx->x28 = 0U;
    ctx->fp = 0U;
    ctx->lr = (uint64_t)(uintptr_t)llam_fiber_bootstrap;
    return 0;
}

#else

/**
 * @brief Stub for non-AArch64 builds that should not call this implementation.
 *
 * @return -1 with @c errno set to ENOSYS.
 */
int llam_ctx_make_task(llam_ctx_t *ctx, void *stack_base, size_t stack_size, llam_task_t *task) {
    (void)ctx;
    (void)stack_base;
    (void)stack_size;
    (void)task;
    errno = ENOSYS;
    return -1;
}

#endif
