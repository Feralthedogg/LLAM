/**
 * @file src/core/runtime_context_portable.c
 * @brief Portable ucontext-based fiber context fallback for unsupported assembly targets.
 *
 * @details
 * Linux/Darwin x86-64 and AArch64 builds use assembly context switching. Other
 * supported targets fall back to @c ucontext so the runtime remains buildable
 * and testable without architecture-specific assembly.
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

#if !(((LLAM_PLATFORM_LINUX || LLAM_PLATFORM_DARWIN || LLAM_PLATFORM_BSD) && LLAM_ARCH_X86_64) || \
      (LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64)) && \
    !LLAM_ARCH_AARCH64

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

/**
 * @brief ucontext trampoline that reconstructs the task pointer.
 *
 * @param lo Low 32 bits of the task pointer.
 * @param hi High 32 bits of the task pointer.
 */
static void llam_portable_fiber_entry(uint32_t lo, uint32_t hi) {
    uintptr_t packed = (uintptr_t)lo | ((uintptr_t)hi << 32U);
    llam_task_t *task = (llam_task_t *)(uintptr_t)packed;

    llam_task_bootstrap(task);
}

/**
 * @brief Build an initial ucontext for a runtime task.
 *
 * @param ctx        Context object to initialize.
 * @param stack_base Usable stack base.
 * @param stack_size Usable stack size.
 * @param task       Task passed to the bootstrap trampoline.
 * @return 0 on success, -1 on invalid arguments or @c getcontext failure.
 */
int llam_ctx_make_task(llam_ctx_t *ctx, void *stack_base, size_t stack_size, llam_task_t *task) {
    uintptr_t packed;

    if (ctx == NULL || stack_base == NULL || stack_size == 0U || task == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (getcontext(&ctx->uc) != 0) {
        return -1;
    }

    ctx->uc.uc_stack.ss_sp = stack_base;
    ctx->uc.uc_stack.ss_size = stack_size;
    ctx->uc.uc_stack.ss_flags = 0;
    ctx->uc.uc_link = NULL;

    packed = (uintptr_t)task;
    // makecontext has integer arguments only on many platforms, so split the
    // pointer into two 32-bit words and reconstruct it in the trampoline.
    makecontext(&ctx->uc,
                (void (*)(void))llam_portable_fiber_entry,
                2,
                (uint32_t)(packed & 0xffffffffU),
                (uint32_t)(packed >> 32U));
    return 0;
}

/**
 * @brief Switch between portable ucontext fiber contexts.
 *
 * @param from Context to save.
 * @param to   Context to restore.
 */
void llam_ctx_switch(llam_ctx_t *from, const llam_ctx_t *to) {
    if (from == NULL || to == NULL) {
        abort();
    }
    // ucontext does not guarantee preservation of the floating-point
    // environment on every platform, so keep a best-effort copy beside it.
    if (fegetenv(&from->fenv) == 0) {
        from->fenv_valid = 1;
    }
    if (to->fenv_valid) {
        (void)fesetenv((const fenv_t *)&to->fenv);
    }
    if (swapcontext(&from->uc, (ucontext_t *)&to->uc) != 0) {
        abort();
    }
}

/**
 * @brief Assembly-only bootstrap placeholder.
 *
 * Portable ucontext builds enter through ::llam_portable_fiber_entry instead.
 */
void llam_fiber_bootstrap(void) {
    abort();
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#endif
