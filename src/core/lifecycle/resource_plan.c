/**
 * @file src/core/lifecycle/resource_plan.c
 * @brief Pure runtime worker, CPU, blocking, and prewarm plan resolution.
 *
 * @details
 * This module validates the complete resource graph before initialization
 * allocates or publishes runtime-owned state. Caller-sized public options are
 * copied into a local full object and every appended field is read only when
 * its complete ABI prefix is present.
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

#include <errno.h>
#include <limits.h>
#include <string.h>

#define LLAM_RESOURCE_OPTS_HAS_FIELD(prefix_size, field) \
    ((prefix_size) >= offsetof(llam_runtime_opts_t, field) + sizeof(((llam_runtime_opts_t *)0)->field))
#define LLAM_COPY_RUNTIME_OPT(field) \
    do { \
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(opts_size, field)) { \
            opts_out->field = raw_opts->field; \
        } \
    } while (0)

void llam_runtime_opts_copy_prefix(const llam_runtime_opts_t *raw_opts,
                                   size_t opts_size,
                                   llam_runtime_opts_t *opts_out) {
    if (raw_opts == NULL || opts_out == NULL) {
        return;
    }
    memset(opts_out, 0, sizeof(*opts_out));
    opts_out->sqpoll_cpu = -1;
    opts_out->profile = LLAM_RUNTIME_PROFILE_BALANCED;
    opts_out->preempt_mode = LLAM_PREEMPT_AUTO;
    opts_out->affinity_policy = LLAM_RUNTIME_AFFINITY_NONE;
    opts_out->driver_mode = LLAM_RUNTIME_DRIVER_INTERNAL;
    LLAM_COPY_RUNTIME_OPT(deterministic);
    LLAM_COPY_RUNTIME_OPT(forced_yield_every);
    LLAM_COPY_RUNTIME_OPT(experimental_flags);
    LLAM_COPY_RUNTIME_OPT(idle_spin_ns);
    LLAM_COPY_RUNTIME_OPT(idle_spin_max_iters);
    LLAM_COPY_RUNTIME_OPT(sqpoll_cpu);
    LLAM_COPY_RUNTIME_OPT(profile);
    LLAM_COPY_RUNTIME_OPT(reserved0);
    LLAM_COPY_RUNTIME_OPT(preempt_mode);
    LLAM_COPY_RUNTIME_OPT(preempt_poll_period);
    LLAM_COPY_RUNTIME_OPT(preempt_quantum_ns);
    LLAM_COPY_RUNTIME_OPT(worker_min);
    LLAM_COPY_RUNTIME_OPT(worker_count);
    LLAM_COPY_RUNTIME_OPT(worker_max);
    LLAM_COPY_RUNTIME_OPT(blocking_min);
    LLAM_COPY_RUNTIME_OPT(blocking_max);
    LLAM_COPY_RUNTIME_OPT(affinity_policy);
    LLAM_COPY_RUNTIME_OPT(cpu_count);
    LLAM_COPY_RUNTIME_OPT(reserved1);
    LLAM_COPY_RUNTIME_OPT(cpu_ids);
    LLAM_COPY_RUNTIME_OPT(task_prewarm_total);
    LLAM_COPY_RUNTIME_OPT(stack_prewarm_total);
    LLAM_COPY_RUNTIME_OPT(timer_prewarm_total);
    LLAM_COPY_RUNTIME_OPT(stack_cache_budget_bytes);
    LLAM_COPY_RUNTIME_OPT(stack_cache_high_watermark_bytes);
    LLAM_COPY_RUNTIME_OPT(stack_cache_low_watermark_bytes);
    LLAM_COPY_RUNTIME_OPT(stack_cache_idle_ns);
    LLAM_COPY_RUNTIME_OPT(stack_cache_flags);
    LLAM_COPY_RUNTIME_OPT(reserved2);
    LLAM_COPY_RUNTIME_OPT(on_task_resume);
    LLAM_COPY_RUNTIME_OPT(on_task_suspend);
    LLAM_COPY_RUNTIME_OPT(switch_hook_context);
    LLAM_COPY_RUNTIME_OPT(driver_mode);
    LLAM_COPY_RUNTIME_OPT(reserved3);
}
#undef LLAM_COPY_RUNTIME_OPT

/** @brief Return false rather than wrapping an unsigned 64-bit addition. */
static bool add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (out == NULL || a > UINT64_MAX - b) {
        return false;
    }
    *out = a + b;
    return true;
}

/** @brief Return false rather than wrapping an unsigned 64-bit multiplication. */
static bool mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (out == NULL || (a != 0U && b > UINT64_MAX / a)) {
        return false;
    }
    *out = a * b;
    return true;
}

bool llam_runtime_task_prewarm_storage_objects(uint64_t logical_total,
                                               unsigned worker_count,
                                               uint64_t *out_objects) {
    uint64_t physical_total = 0U;

    if (worker_count == 0U || out_objects == NULL) {
        return false;
    }
    for (unsigned index = 0U; index < worker_count; ++index) {
        uint64_t share =
            logical_total / worker_count +
            (index < logical_total % worker_count ? 1U : 0U);
        uint64_t slabs;
        uint64_t objects;

        if (share == 0U) {
            continue;
        }
        if (share > UINT64_MAX - (LLAM_TASK_SLAB_COUNT - 1U)) {
            return false;
        }
        slabs =
            (share + LLAM_TASK_SLAB_COUNT - 1U) / LLAM_TASK_SLAB_COUNT;
        if (!mul_u64(slabs, LLAM_TASK_SLAB_COUNT, &objects) ||
            !add_u64(physical_total, objects, &physical_total)) {
            return false;
        }
    }
    *out_objects = physical_total;
    return true;
}

/** @brief Return whether one CPU is present in the process-allowed set. */
static bool cpu_is_allowed(unsigned cpu, const unsigned *allowed, unsigned count) {
    unsigned i;

    if (allowed == NULL) {
        return false;
    }
    for (i = 0U; i < count; ++i) {
        if (allowed[i] == cpu) {
            return true;
        }
    }
    return false;
}

/** @brief Return whether one CPU is already present in an ordered selection. */
static bool cpu_is_duplicate(unsigned cpu, const unsigned *selected, unsigned count) {
    unsigned i;

    if (selected == NULL) {
        return false;
    }
    for (i = 0U; i < count; ++i) {
        if (selected[i] == cpu) {
            return true;
        }
    }
    return false;
}

/** @brief Return one CPU from either a public explicit list or discovery. */
static unsigned source_cpu_at(const uint32_t *explicit_cpus,
                              const unsigned *allowed_cpus,
                              unsigned index) {
    return explicit_cpus != NULL ? (unsigned)explicit_cpus[index] : allowed_cpus[index];
}

/** @brief Resolve the legacy platform-specific blocking-worker default. */
static unsigned legacy_blocking_count(unsigned worker_count) {
    unsigned count;

#if defined(__linux__)
    count = worker_count < 4U ? worker_count : 4U;
    return count == 0U ? 1U : count;
#else
    count = worker_count;
    if (count < 2U) {
        count = 2U;
    }
    return count;
#endif
}

/** @brief Resolve the legacy dynamic-worker floor for an automatic plan. */
static unsigned legacy_dynamic_worker_floor(unsigned worker_max) {
    if (worker_max > 8U) {
        unsigned half = worker_max / 2U;

        return half > 4U ? half : 4U;
    }
    if (worker_max > 4U) {
        return 4U;
    }
    return worker_max;
}

/** @brief Add one checked component to the plan's metadata estimate. */
static bool add_metadata_component(uint64_t count,
                                   uint64_t width,
                                   uint64_t *metadata_bytes) {
    uint64_t component;
    uint64_t total;

    if (!mul_u64(count, width, &component) ||
        !add_u64(*metadata_bytes, component, &total)) {
        return false;
    }
    *metadata_bytes = total;
    return true;
}

int llam_runtime_resource_plan_resolve(const llam_runtime_resource_plan_input_t *input,
                                       llam_runtime_resource_plan_t *out) {
    llam_runtime_opts_t raw_opts;
    llam_runtime_resource_plan_t plan;
    const uint32_t *explicit_cpus = NULL;
    const unsigned *source_allowed;
    unsigned source_count;
    unsigned copied_count = 0U;
    unsigned source_index;
    unsigned affinity_policy = LLAM_RUNTIME_AFFINITY_NONE;
    unsigned worker_min = 0U;
    unsigned worker_count = 0U;
    unsigned worker_max = 0U;
    unsigned blocking_min = 0U;
    unsigned blocking_max = 0U;
    unsigned stack_cache_flags = 0U;
    unsigned driver_mode = LLAM_RUNTIME_DRIVER_INTERNAL;
    uint64_t experimental_flags = 0U;
    uint64_t stack_cache_budget_bytes =
        LLAM_RUNTIME_STACK_CACHE_DEFAULT_BUDGET_BYTES;
    uint64_t stack_cache_high_watermark_bytes =
        LLAM_RUNTIME_STACK_CACHE_DEFAULT_HIGH_WATERMARK_BYTES;
    uint64_t stack_cache_low_watermark_bytes =
        LLAM_RUNTIME_STACK_CACHE_DEFAULT_LOW_WATERMARK_BYTES;
    uint64_t stack_cache_idle_ns =
        LLAM_RUNTIME_STACK_CACHE_DEFAULT_IDLE_NS;
    bool deterministic = false;
    bool worker_min_present = false;
    bool worker_count_present = false;
    bool worker_max_present = false;
    bool blocking_min_present = false;
    bool blocking_max_present = false;
    bool cpu_pair_present = false;
    bool sqpoll_requested;
    unsigned sqpoll_reserve_index = UINT_MAX;
    int requested_sqpoll_cpu = -1;
    int reserved_cpu = -1;
    uint64_t metadata_bytes = 0U;
    uint64_t exact_stack_cache_bytes;
    uint64_t default_stack_mapping_bytes;
    uint64_t stack_mapping_bytes;
    uint64_t task_storage_objects;
    size_t copy_size;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (input == NULL || out == NULL ||
        input->allowed_cpus == NULL || input->allowed_cpu_count == 0U ||
        input->page_size == 0U) {
        errno = EINVAL;
        return -1;
    }
    if ((input->opts == NULL && input->opts_size != 0U) ||
        (input->opts != NULL && input->opts_size == 0U)) {
        errno = EINVAL;
        return -1;
    }

    memset(&plan, 0, sizeof(plan));
    memset(&raw_opts, 0, sizeof(raw_opts));
    raw_opts.sqpoll_cpu = -1;
    if (input->opts != NULL) {
        copy_size = input->opts_size < sizeof(raw_opts) ? input->opts_size : sizeof(raw_opts);
        memcpy(&raw_opts, input->opts, copy_size);
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, deterministic)) {
            deterministic = raw_opts.deterministic != 0U;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, experimental_flags)) {
            experimental_flags = raw_opts.experimental_flags;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, driver_mode)) {
            driver_mode = raw_opts.driver_mode;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, sqpoll_cpu)) {
            requested_sqpoll_cpu = raw_opts.sqpoll_cpu;
        }
        worker_min_present = LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, worker_min);
        worker_count_present = LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, worker_count);
        worker_max_present = LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, worker_max);
        blocking_min_present = LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, blocking_min);
        blocking_max_present = LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, blocking_max);
        if (worker_min_present) {
            worker_min = raw_opts.worker_min;
        }
        if (worker_count_present) {
            worker_count = raw_opts.worker_count;
        }
        if (worker_max_present) {
            worker_max = raw_opts.worker_max;
        }
        if (blocking_min_present) {
            blocking_min = raw_opts.blocking_min;
        }
        if (blocking_max_present) {
            blocking_max = raw_opts.blocking_max;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, affinity_policy)) {
            affinity_policy = raw_opts.affinity_policy;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size,
                                         stack_cache_budget_bytes) &&
            raw_opts.stack_cache_budget_bytes != 0U) {
            stack_cache_budget_bytes = raw_opts.stack_cache_budget_bytes;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size,
                                         stack_cache_high_watermark_bytes) &&
            raw_opts.stack_cache_high_watermark_bytes != 0U) {
            stack_cache_high_watermark_bytes =
                raw_opts.stack_cache_high_watermark_bytes;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size,
                                         stack_cache_low_watermark_bytes) &&
            raw_opts.stack_cache_low_watermark_bytes != 0U) {
            stack_cache_low_watermark_bytes =
                raw_opts.stack_cache_low_watermark_bytes;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size,
                                         stack_cache_idle_ns) &&
            raw_opts.stack_cache_idle_ns != 0U) {
            stack_cache_idle_ns = raw_opts.stack_cache_idle_ns;
        }
        if (LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size,
                                         stack_cache_flags)) {
            stack_cache_flags = raw_opts.stack_cache_flags;
        }
        cpu_pair_present =
            LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, cpu_count) &&
            LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, cpu_ids);
        if (cpu_pair_present && raw_opts.cpu_count != 0U) {
            if (raw_opts.cpu_ids == NULL) {
                errno = EINVAL;
                return -1;
            }
            if (raw_opts.cpu_count > LLAM_RUNTIME_MAX_WORKERS) {
                errno = E2BIG;
                return -1;
            }
            explicit_cpus = raw_opts.cpu_ids;
            source_count = raw_opts.cpu_count;
        } else {
            source_count = input->allowed_cpu_count;
        }
    } else {
        source_count = input->allowed_cpu_count;
    }

    if (affinity_policy != LLAM_RUNTIME_AFFINITY_NONE &&
        affinity_policy != LLAM_RUNTIME_AFFINITY_PREFER &&
        affinity_policy != LLAM_RUNTIME_AFFINITY_REQUIRE) {
        errno = EINVAL;
        return -1;
    }
    if (driver_mode != LLAM_RUNTIME_DRIVER_INTERNAL &&
        driver_mode != LLAM_RUNTIME_DRIVER_EXTERNAL) {
        errno = EINVAL;
        return -1;
    }
    if (affinity_policy == LLAM_RUNTIME_AFFINITY_REQUIRE &&
        !input->affinity_supported) {
        errno = ENOTSUP;
        return -1;
    }
    if ((stack_cache_flags &
         ~(LLAM_RUNTIME_STACK_CACHE_F_SECURE_SCRUB |
           LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN |
           LLAM_RUNTIME_STACK_CACHE_F_DISABLED)) != 0U) {
        errno = EINVAL;
        return -1;
    }
    if ((stack_cache_flags & LLAM_RUNTIME_STACK_CACHE_F_DISABLED) != 0U) {
        if (input->opts != NULL &&
            ((LLAM_RESOURCE_OPTS_HAS_FIELD(
                  input->opts_size, stack_cache_budget_bytes) &&
              raw_opts.stack_cache_budget_bytes != 0U) ||
             (LLAM_RESOURCE_OPTS_HAS_FIELD(
                  input->opts_size, stack_cache_high_watermark_bytes) &&
              raw_opts.stack_cache_high_watermark_bytes != 0U) ||
             (LLAM_RESOURCE_OPTS_HAS_FIELD(
                  input->opts_size, stack_cache_low_watermark_bytes) &&
              raw_opts.stack_cache_low_watermark_bytes != 0U))) {
            errno = EINVAL;
            return -1;
        }
        stack_cache_budget_bytes = 0U;
        stack_cache_high_watermark_bytes = 0U;
        stack_cache_low_watermark_bytes = 0U;
    } else {
        uint64_t page_size = (uint64_t)input->page_size;

        if (stack_cache_budget_bytes % page_size != 0U ||
            stack_cache_high_watermark_bytes % page_size != 0U ||
            stack_cache_low_watermark_bytes % page_size != 0U ||
            stack_cache_low_watermark_bytes >
                stack_cache_high_watermark_bytes ||
            stack_cache_high_watermark_bytes >
                stack_cache_budget_bytes) {
            errno = EINVAL;
            return -1;
        }
    }

    source_allowed = input->allowed_cpus;
    for (source_index = 0U; source_index < source_count; ++source_index) {
        unsigned cpu = source_cpu_at(explicit_cpus, source_allowed, source_index);
        unsigned earlier;

        if (!cpu_is_allowed(cpu, input->allowed_cpus, input->allowed_cpu_count)) {
            errno = EINVAL;
            return -1;
        }
        for (earlier = 0U; earlier < source_index; ++earlier) {
            if (source_cpu_at(explicit_cpus, source_allowed, earlier) == cpu) {
                errno = EINVAL;
                return -1;
            }
        }
    }

    sqpoll_requested =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL) != 0U &&
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS) == 0U;
    if (driver_mode == LLAM_RUNTIME_DRIVER_EXTERNAL &&
        ((experimental_flags &
          LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS) != 0U ||
         (experimental_flags &
          LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL) != 0U)) {
        errno = EINVAL;
        return -1;
    }
    if (sqpoll_requested && requested_sqpoll_cpu < -1) {
        errno = EINVAL;
        return -1;
    }
    if (sqpoll_requested && requested_sqpoll_cpu >= 0) {
        for (source_index = 0U; source_index < source_count; ++source_index) {
            if (source_cpu_at(explicit_cpus, source_allowed, source_index) ==
                (unsigned)requested_sqpoll_cpu) {
                sqpoll_reserve_index = source_index;
                break;
            }
        }
        if (sqpoll_reserve_index == UINT_MAX) {
            errno = EINVAL;
            return -1;
        }
    }
    if (sqpoll_requested && !input->sqpoll_supported) {
        errno = ENOTSUP;
        return -1;
    }
    if (sqpoll_requested && source_count > 1U) {
        if (sqpoll_reserve_index == UINT_MAX) {
            sqpoll_reserve_index = source_count - 1U;
        }
        reserved_cpu = (int)source_cpu_at(explicit_cpus, source_allowed, sqpoll_reserve_index);
        plan.sqpoll_reserved = true;
    } else if (sqpoll_requested && requested_sqpoll_cpu >= 0) {
        if (!cpu_is_allowed((unsigned)requested_sqpoll_cpu,
                            input->allowed_cpus,
                            input->allowed_cpu_count)) {
            errno = EINVAL;
            return -1;
        }
        reserved_cpu = requested_sqpoll_cpu;
    }

    for (source_index = 0U;
         source_index < source_count && copied_count < LLAM_RUNTIME_MAX_WORKERS;
         ++source_index) {
        unsigned cpu = source_cpu_at(explicit_cpus, source_allowed, source_index);

        if (plan.sqpoll_reserved && (int)cpu == reserved_cpu) {
            continue;
        }
        if (cpu_is_duplicate(cpu, plan.selected_cpus, copied_count)) {
            errno = EINVAL;
            return -1;
        }
        plan.selected_cpus[copied_count++] = cpu;
    }
    if (copied_count == 0U) {
        errno = EINVAL;
        return -1;
    }

    if (driver_mode == LLAM_RUNTIME_DRIVER_EXTERNAL) {
        if (worker_min > 1U || worker_count > 1U || worker_max > 1U) {
            errno = EINVAL;
            return -1;
        }
        worker_min = 1U;
        worker_count = 1U;
        worker_max = 1U;
    } else if (worker_min == 0U && worker_count == 0U && worker_max == 0U) {
        worker_max = copied_count;
        if (deterministic) {
            worker_min = 1U;
            worker_count = 1U;
            worker_max = 1U;
        } else if ((experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS) != 0U) {
            worker_min = legacy_dynamic_worker_floor(worker_max);
            worker_count = worker_min;
        } else {
            worker_min = worker_max;
            worker_count = worker_max;
        }
    } else if (worker_count != 0U && worker_min == 0U && worker_max == 0U) {
        worker_min = worker_count;
        worker_max = worker_count;
    } else if (!worker_min_present || !worker_count_present || !worker_max_present ||
               worker_min == 0U || worker_count == 0U || worker_max == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (worker_min > LLAM_RUNTIME_MAX_WORKERS ||
        worker_count > LLAM_RUNTIME_MAX_WORKERS ||
        worker_max > LLAM_RUNTIME_MAX_WORKERS) {
        errno = E2BIG;
        return -1;
    }
    if (worker_min > worker_count || worker_count > worker_max ||
        worker_max > copied_count) {
        errno = EINVAL;
        return -1;
    }
    if (deterministic &&
        (worker_min != 1U || worker_count != 1U || worker_max != 1U)) {
        errno = EINVAL;
        return -1;
    }
    plan.selected_cpu_count = worker_max;

    if (blocking_min == 0U && blocking_max == 0U) {
        blocking_min = legacy_blocking_count(worker_count);
        blocking_max = blocking_min;
    } else if (!blocking_min_present || !blocking_max_present ||
               blocking_max == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (blocking_min > LLAM_RUNTIME_MAX_BLOCKING_WORKERS ||
        blocking_max > LLAM_RUNTIME_MAX_BLOCKING_WORKERS) {
        errno = E2BIG;
        return -1;
    }
    if (blocking_min > blocking_max) {
        errno = EINVAL;
        return -1;
    }

    plan.driver_mode = driver_mode;
    plan.worker_min = worker_min;
    plan.worker_count = worker_count;
    plan.worker_max = worker_max;
    plan.blocking_min = blocking_min;
    plan.blocking_max = blocking_max;
    plan.affinity_policy = affinity_policy;
    plan.stack_cache_budget_bytes = stack_cache_budget_bytes;
    plan.stack_cache_high_watermark_bytes =
        stack_cache_high_watermark_bytes;
    plan.stack_cache_low_watermark_bytes =
        stack_cache_low_watermark_bytes;
    plan.stack_cache_idle_ns = stack_cache_idle_ns;
    plan.stack_cache_flags = stack_cache_flags;
    plan.sqpoll_cpu = reserved_cpu;
    if (!sqpoll_requested) {
        plan.sqpoll_cpu = -1;
    }
    if (input->opts != NULL &&
        LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, task_prewarm_total)) {
        plan.task_prewarm_total = raw_opts.task_prewarm_total;
    }
    if (input->opts != NULL &&
        LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, stack_prewarm_total)) {
        plan.stack_prewarm_total = raw_opts.stack_prewarm_total;
    }
    if (input->opts != NULL &&
        LLAM_RESOURCE_OPTS_HAS_FIELD(input->opts_size, timer_prewarm_total)) {
        plan.timer_prewarm_total = raw_opts.timer_prewarm_total;
    }

    if (!llam_runtime_task_prewarm_storage_objects(plan.task_prewarm_total,
                                                   plan.worker_max,
                                                   &task_storage_objects) ||
        !add_metadata_component(plan.worker_max, sizeof(llam_shard_t), &metadata_bytes) ||
        !add_metadata_component(plan.blocking_max, sizeof(pthread_t), &metadata_bytes) ||
        !add_metadata_component(task_storage_objects, sizeof(llam_task_t), &metadata_bytes) ||
        !add_metadata_component(plan.timer_prewarm_total, sizeof(llam_timer_node_t), &metadata_bytes) ||
        !add_u64((uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT),
                 (uint64_t)input->page_size,
                 &default_stack_mapping_bytes) ||
        !mul_u64(plan.stack_prewarm_total,
                 default_stack_mapping_bytes,
                 &exact_stack_cache_bytes) ||
        !mul_u64(plan.stack_prewarm_total,
                 LLAM_RUNTIME_STACK_MAPPING_ESTIMATE_BYTES,
                 &stack_mapping_bytes)) {
        errno = EOVERFLOW;
        return -1;
    }
    if (plan.stack_prewarm_total > LLAM_RUNTIME_MAX_STACK_PREWARM ||
        metadata_bytes > LLAM_RUNTIME_METADATA_BUDGET_BYTES) {
        errno = E2BIG;
        return -1;
    }
    if ((plan.stack_cache_flags & LLAM_RUNTIME_STACK_CACHE_F_DISABLED) != 0U &&
        plan.stack_prewarm_total != 0U) {
        errno = EINVAL;
        return -1;
    }
    if (exact_stack_cache_bytes > plan.stack_cache_budget_bytes ||
        exact_stack_cache_bytes >
            plan.stack_cache_high_watermark_bytes) {
        errno = E2BIG;
        return -1;
    }
    plan.estimated_metadata_bytes = metadata_bytes;
    plan.estimated_stack_mapping_bytes = stack_mapping_bytes;

    *out = plan;
    return 0;
}
