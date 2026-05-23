/**
 * @file src/core/runtime_stack_sample.c
 * @brief Stack sampling helpers for diagnostics and safety checks.
 *
 * @details
 * Stack classes define the amount of usable fiber stack each task receives.
 * Sampling is disabled by default and can be enabled through diagnostic
 * environment variables. When enabled, the runtime records stack high-water
 * marks so users can choose smaller/larger stack classes with evidence instead
 * of guessing.
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

#if defined(_MSC_VER)
#include <intrin.h>
#endif

/**
 * @brief Return the usable stack size for a stack class.
 *
 * @param stack_class Requested stack class.
 * @return Usable stack bytes, excluding the guard page.
 */
size_t llam_stack_bytes(llam_stack_class_t stack_class) {
    switch (stack_class) {
    case LLAM_STACK_CLASS_LARGE:
        return 256U * 1024U;
    case LLAM_STACK_CLASS_HUGE:
        return 1024U * 1024U;
    case LLAM_STACK_CLASS_DEFAULT:
    default:
        return 64U * 1024U;
    }
}

/**
 * @brief Return the cooperative scheduler slice budget for a task class.
 *
 * @param task_class Task scheduling class.
 * @return Time slice budget in nanoseconds.
 */
uint64_t llam_slice_ns(llam_task_class_t task_class) {
    switch (task_class) {
    case LLAM_TASK_CLASS_LATENCY:
        return 50000ULL;
    case LLAM_TASK_CLASS_BATCH:
        return 1000000ULL;
    case LLAM_TASK_CLASS_DEFAULT:
    default:
        return 200000ULL;
    }
}

/**
 * @brief Read the current hardware stack pointer.
 *
 * @return Current stack pointer value as an integer.
 */
static uintptr_t llam_current_rsp(void) {
    uintptr_t rsp;

#if defined(_MSC_VER)
    rsp = (uintptr_t)_AddressOfReturnAddress();
#elif LLAM_ARCH_AARCH64
    __asm__ volatile("mov %0, sp" : "=r"(rsp));
#elif LLAM_ARCH_X86_64
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
#else
    // Portable fallback is approximate but still useful for diagnostics on
    // unsupported architectures.
    rsp = (uintptr_t)&rsp;
#endif
    return rsp;
}

/**
 * @brief Compute task stack usage from a sampled stack pointer.
 *
 * @param task Task whose stack bounds are known.
 * @param rsp  Sampled stack pointer.
 * @return Bytes used from the high end of the stack.
 */
static size_t llam_task_stack_used_from_rsp(const llam_task_t *task, uintptr_t rsp) {
    uintptr_t low;
    uintptr_t high;

    if (task == NULL || task->stack_base == NULL || task->stack_size == 0U) {
        return 0U;
    }

    low = (uintptr_t)task->stack_base;
    high = low + task->stack_size;
    if (rsp >= high) {
        return 0U;
    }
    if (rsp <= low) {
        return task->stack_size;
    }
    return (size_t)(high - rsp);
}

/**
 * @brief Check whether stack sampling is enabled for this process.
 *
 * @return true if sampling should record high-water data.
 */
static bool llam_stack_sampling_enabled(const llam_task_t *task) {
    const llam_runtime_t *rt = task != NULL ? task->owner_runtime : NULL;

    return rt != NULL && rt->stack_sampling_enabled != 0U;
}

/**
 * @brief Record a stack sample for a task from an explicit stack pointer.
 *
 * @param task Task to update.
 * @param rsp  Sampled stack pointer.
 */
void llam_task_sample_stack_rsp(llam_task_t *task, uintptr_t rsp) {
    size_t used;

    if (task == NULL || !llam_stack_sampling_enabled(task)) {
        return;
    }

    used = llam_task_stack_used_from_rsp(task, rsp);
    task->last_stack_used = used;
    if (used > task->stack_high_water) {
        task->stack_high_water = used;
    }
}

/**
 * @brief Sample the currently executing native stack for a task.
 *
 * @param task Task whose stack statistics should be updated.
 */
void llam_task_sample_live_stack(llam_task_t *task) {
    if (!llam_stack_sampling_enabled(task)) {
        return;
    }
    llam_task_sample_stack_rsp(task, llam_current_rsp());
}

/**
 * @brief Return a coarse sizing hint based on recorded stack high-water usage.
 *
 * @param task Task to inspect.
 * @return Static string describing the observed stack profile.
 */
const char *llam_stack_profile_hint(const llam_task_t *task) {
    size_t peak;

    if (task == NULL || task->stack_size == 0U) {
        return "unknown";
    }

    peak = task->stack_high_water;
    if (peak == 0U) {
        return "cold";
    }
    if (peak <= (64U * 1024U) / 2U) {
        return "default-ok";
    }
    if (peak <= (256U * 1024U) / 2U) {
        return "large-ok";
    }
    if (peak <= (1024U * 1024U) / 2U) {
        return "huge-ok";
    }
    return "near-limit";
}

/**
 * @brief Return the system page size.
 *
 * @return Cached page size from @c sysconf(_SC_PAGESIZE).
 */
long llam_page_size(void) {
    static atomic_long cached_page_size;
    long page_size;

    page_size = atomic_load_explicit(&cached_page_size, memory_order_acquire);
    if (page_size == 0) {
#if LLAM_PLATFORM_WINDOWS
        SYSTEM_INFO system_info;

        memset(&system_info, 0, sizeof(system_info));
        GetSystemInfo(&system_info);
        page_size = system_info.dwPageSize != 0U ? (long)system_info.dwPageSize : 4096L;
#else
        page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            page_size = 4096L;
        }
#endif
        /*
         * Multiple native workers may query this helper before the runtime is
         * fully warm.  Racing stores of the same page size are harmless only if
         * they are atomic; keep the cache lock-free but data-race free.
         */
        atomic_store_explicit(&cached_page_size, page_size, memory_order_release);
    }

    return page_size;
}
