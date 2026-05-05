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

/**
 * @brief Return the usable stack size for a stack class.
 *
 * @param stack_class Requested stack class.
 * @return Usable stack bytes, excluding the guard page.
 */
size_t nm_stack_bytes(nm_stack_class_t stack_class) {
    switch (stack_class) {
    case NM_STACK_CLASS_LARGE:
        return 256U * 1024U;
    case NM_STACK_CLASS_HUGE:
        return 1024U * 1024U;
    case NM_STACK_CLASS_DEFAULT:
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
uint64_t nm_slice_ns(nm_task_class_t task_class) {
    switch (task_class) {
    case NM_TASK_CLASS_LATENCY:
        return 50000ULL;
    case NM_TASK_CLASS_BATCH:
        return 1000000ULL;
    case NM_TASK_CLASS_DEFAULT:
    default:
        return 200000ULL;
    }
}

/**
 * @brief Read the current hardware stack pointer.
 *
 * @return Current stack pointer value as an integer.
 */
static uintptr_t nm_current_rsp(void) {
    uintptr_t rsp;

#if defined(__aarch64__)
    __asm__ volatile("mov %0, sp" : "=r"(rsp));
#elif defined(__x86_64__)
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
static size_t nm_task_stack_used_from_rsp(const nm_task_t *task, uintptr_t rsp) {
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
static bool nm_stack_sampling_enabled(void) {
    static atomic_int cached_enabled = ATOMIC_VAR_INIT(-1);
    int enabled = atomic_load_explicit(&cached_enabled, memory_order_acquire);

    if (enabled < 0) {
        const char *sample_value = nm_env_get("LLAM_STACK_SAMPLING");
        const char *light_value = nm_env_get("LLAM_DIAG_LIGHT_SAFEPOINT");
        const char *strict_value = nm_env_get("LLAM_STRICT_SAFEPOINT");

        // Cache the environment decision after the first check so the sampling
        // path does not repeatedly parse strings.
        enabled = ((sample_value != NULL && sample_value[0] != '\0' && strcmp(sample_value, "0") != 0) ||
                   (light_value != NULL && light_value[0] != '\0' && strcmp(light_value, "0") == 0) ||
                   (strict_value != NULL && strict_value[0] != '\0' && strcmp(strict_value, "0") != 0))
                      ? 1
                      : 0;
        atomic_store_explicit(&cached_enabled, enabled, memory_order_release);
    }
    return enabled != 0;
}

/**
 * @brief Record a stack sample for a task from an explicit stack pointer.
 *
 * @param task Task to update.
 * @param rsp  Sampled stack pointer.
 */
void nm_task_sample_stack_rsp(nm_task_t *task, uintptr_t rsp) {
    size_t used;

    if (task == NULL || !nm_stack_sampling_enabled()) {
        return;
    }

    used = nm_task_stack_used_from_rsp(task, rsp);
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
void nm_task_sample_live_stack(nm_task_t *task) {
    if (!nm_stack_sampling_enabled()) {
        return;
    }
    nm_task_sample_stack_rsp(task, nm_current_rsp());
}

/**
 * @brief Return a coarse sizing hint based on recorded stack high-water usage.
 *
 * @param task Task to inspect.
 * @return Static string describing the observed stack profile.
 */
const char *nm_stack_profile_hint(const nm_task_t *task) {
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
long nm_page_size(void) {
    static long cached_page_size = 0;

    if (cached_page_size == 0) {
        cached_page_size = sysconf(_SC_PAGESIZE);
    }

    return cached_page_size;
}
