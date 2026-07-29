/**
 * @file src/core/lifecycle/init.c
 * @brief Runtime initialization, option parsing, worker/node allocation, and environment policy setup.
 *
 * @details
 * Runtime initialization builds one runtime instance in a strict order:
 *  - parse caller options and environment overrides,
 *  - discover the allowed CPU set and optional SQPOLL reservation,
 *  - allocate shards, per-shard synchronization objects, stacks, and contexts,
 *  - allocate I/O nodes and start their watcher/worker threads when supported,
 *  - initialize runtime-local task, blocking, and overflow infrastructure,
 *  - install diagnostics/signal handling, and finally start the controller.
 *
 * Failure handling is intentionally centralized through ::llam_runtime_shutdown
 * once partially initialized runtime resources exist. Early failures before
 * those resources are published clean up their local allocations directly.
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
 * @brief Initialize per-shard diagnostic counters after runtime storage reset.
 *
 * Metrics are updated by scheduler, I/O, watchdog, and user-facing diagnostic
 * paths.  Keep them atomic so live stats/dumps are race-free, but initialize
 * every field explicitly because runtime startup byte-clears the object.
 *
 * @param metrics Metrics block embedded in a shard.
 */
static void llam_metrics_init(llam_metrics_t *metrics) {
    unsigned i;

    if (metrics == NULL) {
        return;
    }

    atomic_init(&metrics->ctx_switches, 0U);
    atomic_init(&metrics->yields, 0U);
    atomic_init(&metrics->parks, 0U);
    atomic_init(&metrics->wakes, 0U);
    atomic_init(&metrics->timeout_wakes, 0U);
    atomic_init(&metrics->cancel_wakes, 0U);
    atomic_init(&metrics->sleeps, 0U);
    atomic_init(&metrics->joins, 0U);
    atomic_init(&metrics->steals, 0U);
    atomic_init(&metrics->migrations, 0U);
    atomic_init(&metrics->blocking_calls, 0U);
    atomic_init(&metrics->blocking_completions, 0U);
    atomic_init(&metrics->io_submits, 0U);
    atomic_init(&metrics->io_completions, 0U);
    atomic_init(&metrics->io_completion_latency_ns, 0U);
    atomic_init(&metrics->io_completion_samples, 0U);
    atomic_init(&metrics->io_fallbacks, 0U);
    atomic_init(&metrics->hot_enqueues, 0U);
    atomic_init(&metrics->norm_enqueues, 0U);
    atomic_init(&metrics->inject_enqueues, 0U);
    atomic_init(&metrics->wake_latency_ns, 0U);
    atomic_init(&metrics->wake_samples, 0U);
    atomic_init(&metrics->autotune_wake_latency_samples, 0U);
    for (i = 0U; i < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++i) {
        atomic_init(&metrics->autotune_wake_latency_buckets[i], 0U);
    }
    atomic_init(&metrics->idle_polls, 0U);
    atomic_init(&metrics->idle_spin_loops, 0U);
    atomic_init(&metrics->idle_spin_hits, 0U);
    atomic_init(&metrics->idle_spin_fallbacks, 0U);
    atomic_init(&metrics->idle_spin_ns, 0U);
    atomic_init(&metrics->watchdog_hits, 0U);
    atomic_init(&metrics->long_no_safepoint, 0U);
    atomic_init(&metrics->preempt_requests, 0U);
    atomic_init(&metrics->preempt_yields, 0U);
    atomic_init(&metrics->preempt_suppressed, 0U);
    atomic_init(&metrics->preempt_signals, 0U);
    atomic_init(&metrics->yield_direct_attempts, 0U);
    atomic_init(&metrics->yield_direct_fast_hits, 0U);
    atomic_init(&metrics->yield_direct_locked_hits, 0U);
    atomic_init(&metrics->yield_direct_fail_context, 0U);
    atomic_init(&metrics->yield_direct_fail_policy, 0U);
    atomic_init(&metrics->yield_direct_fail_budget, 0U);
    atomic_init(&metrics->yield_direct_fail_no_work, 0U);
    atomic_init(&metrics->yield_direct_fail_self, 0U);
    atomic_init(&metrics->yield_direct_fail_push, 0U);
    atomic_init(&metrics->wake_handoff_attempts, 0U);
    atomic_init(&metrics->wake_handoff_hits, 0U);
    atomic_init(&metrics->wake_handoff_fail_context, 0U);
    atomic_init(&metrics->wake_handoff_fail_policy, 0U);
    atomic_init(&metrics->wake_handoff_fail_budget, 0U);
    atomic_init(&metrics->wake_handoff_fail_race, 0U);
    atomic_init(&metrics->opaque_compensations, 0U);
    atomic_init(&metrics->deadlock_suspicions, 0U);
    atomic_init(&metrics->queue_overflows, 0U);
    atomic_init(&metrics->slice_budget_ns, 0U);
    atomic_init(&metrics->max_run_ns, 0U);
    atomic_init(&metrics->slice_overruns, 0U);
    atomic_init(&metrics->total_run_ns, 0U);
    atomic_init(&metrics->opaque_block_ns, 0U);
    atomic_init(&metrics->opaque_block_samples, 0U);
    atomic_init(&metrics->opaque_block_max_ns, 0U);
    atomic_init(&metrics->opaque_enter_wait_ns, 0U);
    atomic_init(&metrics->opaque_enter_wait_samples, 0U);
    atomic_init(&metrics->opaque_enter_wait_max_ns, 0U);
    atomic_init(&metrics->opaque_leave_wait_ns, 0U);
    atomic_init(&metrics->opaque_leave_wait_samples, 0U);
    atomic_init(&metrics->opaque_leave_wait_max_ns, 0U);
    atomic_init(&metrics->opaque_redirect_activations, 0U);
    for (i = 0U; i <= LLAM_WAIT_TIMEOUT; ++i) {
        atomic_init(&metrics->wake_reason_hist[i], 0U);
    }
}

/**
 * @brief Tear down a registered partially-initialized runtime and preserve errno.
 *
 * @details
 * After ::llam_runtime_register_handle succeeds, initialization may have already
 * acquired process/backend resources such as Winsock on Windows.  Failure paths
 * from that point must use normal runtime shutdown instead of open-coding
 * unregister+memset, otherwise backend-specific cleanup can be skipped.  The
 * original initialization error is restored after cleanup because teardown may
 * call system APIs that touch @c errno.
 */
static int llam_runtime_init_fail_registered(llam_runtime_t *rt, int error_code) {
    if (error_code == 0) {
        error_code = EINVAL;
    }
    errno = error_code;
    llam_runtime_shutdown_rt(rt);
    errno = error_code;
    return -1;
}

/**
 * @brief Initialize a shard trace ring after calloc-backed shard allocation.
 *
 * Trace events are atomic because trace producers can be peer wake paths, I/O
 * completions, watchdogs, or the shard owner itself.  Initialize them
 * explicitly instead of relying on the representation of zeroed atomics.
 *
 * @param shard Shard whose diagnostic trace ring should be reset.
 */
static void llam_trace_ring_init(llam_shard_t *shard) {
    unsigned i;

    if (shard == NULL) {
        return;
    }
    atomic_init(&shard->trace_head, 0U);
    for (i = 0U; i < LLAM_TRACE_RING_CAP; ++i) {
        atomic_init(&shard->trace_ring[i].ts_ns, 0U);
        atomic_init(&shard->trace_ring[i].task_id, 0U);
        atomic_init(&shard->trace_ring[i].kind, 0U);
        atomic_init(&shard->trace_ring[i].from_state, 0U);
        atomic_init(&shard->trace_ring[i].to_state, 0U);
        atomic_init(&shard->trace_ring[i].reason, 0U);
        atomic_init(&shard->trace_ring[i].shard, 0U);
    }
}

/**
 * @brief Resolve the runtime profile from @c LLAM_RUNTIME_PROFILE.
 *
 * The profile controls default safety/performance policy, such as safepoint
 * frequency and preemption polling. Unknown profile names are ignored so callers
 * can safely provide a compiled fallback.
 *
 * @param fallback Profile to use when the environment variable is unset or
 *                 contains an unrecognized value.
 *
 * @return The selected runtime profile.
 */
static llam_runtime_profile_t llam_runtime_profile_from_env(llam_runtime_profile_t fallback) {
    const char *env = llam_env_get("LLAM_RUNTIME_PROFILE");

    if (env == NULL || env[0] == '\0') {
        return fallback;
    }
    if (strcmp(env, "release-fast") == 0 || strcmp(env, "fast") == 0 || strcmp(env, "release") == 0) {
        return LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    }
    if (strcmp(env, "debug-safe") == 0 || strcmp(env, "debug") == 0 || strcmp(env, "safe") == 0) {
        return LLAM_RUNTIME_PROFILE_DEBUG_SAFE;
    }
    if (strcmp(env, "io-latency") == 0 || strcmp(env, "latency-io") == 0 || strcmp(env, "poll") == 0) {
        return LLAM_RUNTIME_PROFILE_IO_LATENCY;
    }
    if (strcmp(env, "balanced") == 0 || strcmp(env, "default") == 0) {
        return LLAM_RUNTIME_PROFILE_BALANCED;
    }
    return fallback;
}

/** @brief Return true when @p profile is a supported public runtime profile. */
static bool llam_public_runtime_profile_valid(uint32_t profile) {
    return profile == (uint32_t)LLAM_RUNTIME_PROFILE_BALANCED ||
           profile == (uint32_t)LLAM_RUNTIME_PROFILE_RELEASE_FAST ||
           profile == (uint32_t)LLAM_RUNTIME_PROFILE_DEBUG_SAFE ||
           profile == (uint32_t)LLAM_RUNTIME_PROFILE_IO_LATENCY;
}

/**
 * @brief Return true when an ABI prefix contains a complete runtime option field.
 *
 * Runtime initialization accepts size-aware options from dynamic loaders and
 * FFI bindings.  Only whole fields are meaningful; partial fixed-width fields
 * must leave the initialized defaults intact.
 */
#define LLAM_RUNTIME_OPTS_PREFIX_HAS_FIELD(prefix_size, field) \
    ((prefix_size) >= offsetof(llam_runtime_opts_t, field) + sizeof(((llam_runtime_opts_t *)0)->field))

/**
 * @brief Read a boolean-like runtime environment variable.
 *
 * Conventional true/false tokens are accepted. Unknown text preserves the
 * supplied default so misspelled values do not silently enable experimental
 * paths.
 *
 * @param name          Environment variable name.
 * @param default_value Value returned when @p name is unset or empty.
 *
 * @return 0 or 1.
 */
static unsigned llam_runtime_env_flag(const char *name, unsigned default_value) {
    return llam_env_flag(name, default_value) != 0U ? 1U : 0U;
}

/**
 * @brief Read and clamp an unsigned runtime environment variable.
 *
 * Invalid input leaves the compiled default intact. Valid values greater than
 * @p max_value are clamped so diagnostics knobs cannot accidentally create
 * pathological polling or sampling intervals.
 *
 * @param name          Environment variable name.
 * @param default_value Value returned for missing or invalid input.
 * @param max_value     Inclusive upper bound for parsed values.
 *
 * @return Parsed and clamped value.
 */
static unsigned llam_runtime_env_u32(const char *name, unsigned default_value, unsigned max_value) {
    const char *env = llam_env_get(name);
    char *end = NULL;
    unsigned long parsed;

    if (env == NULL || env[0] == '\0') {
        return default_value;
    }
    if (llam_ascii_is_space((unsigned char)env[0]) || env[0] == '-' || env[0] == '+') {
        return default_value;
    }
    errno = 0;
    parsed = strtoul(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0') {
        return default_value;
    }
    if (parsed > (unsigned long)max_value) {
        parsed = (unsigned long)max_value;
    }
    return (unsigned)parsed;
}

/**
 * @brief Read and clamp an unsigned 64-bit runtime environment variable.
 */
static uint64_t llam_runtime_env_u64(const char *name, uint64_t default_value, uint64_t max_value) {
    const char *env = llam_env_get(name);
    char *end = NULL;
    unsigned long long parsed;

    if (env == NULL || env[0] == '\0') {
        return default_value;
    }
    if (llam_ascii_is_space((unsigned char)env[0]) || env[0] == '-' || env[0] == '+') {
        return default_value;
    }
    errno = 0;
    parsed = strtoull(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0') {
        return default_value;
    }
    if ((uint64_t)parsed > max_value) {
        parsed = (unsigned long long)max_value;
    }
    return (uint64_t)parsed;
}

/**
 * @brief Parse a present unsigned environment value and expose its authority.
 *
 * Missing, empty, signed, whitespace-prefixed, malformed, and overflowing
 * values are ignored so a malformed newer compatibility name can still fall
 * back to the historical name or profile default.
 */
static bool llam_runtime_env_u64_present(const char *name,
                                         uint64_t max_value,
                                         uint64_t *out_value) {
    const char *env = llam_env_get(name);
    char *end = NULL;
    unsigned long long parsed;

    if (out_value == NULL || env == NULL || env[0] == '\0' ||
        llam_ascii_is_space((unsigned char)env[0]) ||
        env[0] == '-' || env[0] == '+') {
        return false;
    }
    errno = 0;
    parsed = strtoull(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0') {
        return false;
    }
    if ((uint64_t)parsed > max_value) {
        parsed = (unsigned long long)max_value;
    }
    *out_value = (uint64_t)parsed;
    return true;
}

/** @brief Multiply a per-shard compatibility target and clamp its total. */
static uint64_t llam_runtime_prewarm_per_shard_total(uint64_t per_shard,
                                                     unsigned shard_count,
                                                     uint64_t max_total) {
    if (shard_count == 0U || per_shard == 0U) {
        return 0U;
    }
    if (per_shard > max_total / shard_count) {
        return max_total;
    }
    return per_shard * shard_count;
}

/** @brief Clamp a logical task target to the bytes its rounded slabs consume. */
static uint64_t llam_runtime_bound_task_prewarm_total(uint64_t requested,
                                                      unsigned shard_count,
                                                      uint64_t available_bytes,
                                                      uint64_t *storage_objects) {
    uint64_t low = 0U;
    uint64_t high = requested;
    uint64_t max_objects = available_bytes / sizeof(llam_task_t);

    while (low < high) {
        uint64_t delta = high - low;
        uint64_t candidate = low + delta / 2U + delta % 2U;
        uint64_t candidate_storage = 0U;

        if (llam_runtime_task_prewarm_storage_objects(candidate,
                                                      shard_count,
                                                      &candidate_storage) &&
            candidate_storage <= max_objects) {
            low = candidate;
        } else {
            high = candidate - 1U;
        }
    }
    if (!llam_runtime_task_prewarm_storage_objects(low,
                                                   shard_count,
                                                   storage_objects)) {
        *storage_objects = 0U;
        return 0U;
    }
    return low;
}

/**
 * @brief Resolve exact public and best-effort compatibility prewarm authority.
 *
 * The immutable resource plan retains only caller-supplied exact totals.
 * Effective profile/environment totals and their source are stored separately
 * on the runtime so diagnostics do not rewrite the planner's semantic input.
 */
static int llam_runtime_resolve_prewarm_authority(llam_runtime_t *rt) {
    const uint64_t task_max =
        LLAM_RUNTIME_METADATA_BUDGET_BYTES / sizeof(llam_task_t);
    const uint64_t timer_max =
        LLAM_RUNTIME_METADATA_BUDGET_BYTES / sizeof(llam_timer_node_t);
    uint64_t task_total;
    uint64_t stack_total;
    uint64_t timer_total;
    uint64_t value;
    uint64_t estimated_metadata;
    uint64_t task_storage_objects = 0U;

    if (rt == NULL || rt->active_shards == 0U) {
        errno = EINVAL;
        return -1;
    }

    task_total = rt->resource_plan.task_prewarm_total;
    rt->task_prewarm_source = LLAM_RUNTIME_PREWARM_PUBLIC_EXACT;
    if (task_total == 0U) {
        if (llam_runtime_env_u64_present("LLAM_TASK_CACHE_PREWARM_TOTAL",
                                         task_max,
                                         &task_total)) {
            rt->task_prewarm_source = LLAM_RUNTIME_PREWARM_ENV_TOTAL;
        } else if (llam_runtime_env_u64_present("LLAM_TASK_CACHE_PREWARM",
                                                4096U,
                                                &value)) {
            task_total = llam_runtime_prewarm_per_shard_total(
                value,
                rt->active_shards,
                task_max);
            rt->task_prewarm_source = LLAM_RUNTIME_PREWARM_ENV_LEGACY;
        } else {
            task_total = llam_runtime_prewarm_per_shard_total(
                128U,
                rt->active_shards,
                task_max);
            rt->task_prewarm_source = LLAM_RUNTIME_PREWARM_DEFAULT;
        }
    }

    stack_total = rt->resource_plan.stack_prewarm_total;
    rt->stack_prewarm_source = LLAM_RUNTIME_PREWARM_PUBLIC_EXACT;
    if (stack_total == 0U) {
        if (llam_runtime_env_u64_present("LLAM_STACK_CACHE_PREWARM_TOTAL",
                                         LLAM_RUNTIME_MAX_STACK_PREWARM,
                                         &stack_total)) {
            rt->stack_prewarm_source = LLAM_RUNTIME_PREWARM_ENV_TOTAL;
        } else if (llam_runtime_env_u64_present("LLAM_STACK_CACHE_PREWARM",
                                                LLAM_RUNTIME_MAX_STACK_PREWARM,
                                                &stack_total)) {
            /*
             * Unlike the two historical metadata knobs, the old stack knob
             * was already runtime-total. Only its best-effort/source status is
             * deprecated.
             */
            rt->stack_prewarm_source = LLAM_RUNTIME_PREWARM_ENV_LEGACY;
        } else {
            stack_total =
                rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 256U : 128U;
            rt->stack_prewarm_source = LLAM_RUNTIME_PREWARM_DEFAULT;
        }
    }

    timer_total = rt->resource_plan.timer_prewarm_total;
    rt->timer_prewarm_source = LLAM_RUNTIME_PREWARM_PUBLIC_EXACT;
    if (timer_total == 0U) {
        if (llam_runtime_env_u64_present("LLAM_TIMER_HEAP_PREWARM_TOTAL",
                                         timer_max,
                                         &timer_total)) {
            rt->timer_prewarm_source = LLAM_RUNTIME_PREWARM_ENV_TOTAL;
        } else if (llam_runtime_env_u64_present("LLAM_TIMER_HEAP_PREWARM",
                                                1048576U,
                                                &value)) {
            timer_total = llam_runtime_prewarm_per_shard_total(
                value,
                rt->active_shards,
                timer_max);
            rt->timer_prewarm_source = LLAM_RUNTIME_PREWARM_ENV_LEGACY;
        } else {
            value = rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST
                        ? 1024U
                        : (rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE
                               ? 0U
                               : 512U);
            timer_total = llam_runtime_prewarm_per_shard_total(
                value,
                rt->active_shards,
                timer_max);
            rt->timer_prewarm_source = LLAM_RUNTIME_PREWARM_DEFAULT;
        }
    }

    /*
     * Exact public components are already included in the pure plan estimate.
     * Clamp only best-effort components to the remaining aggregate metadata
     * budget, in stable task-then-timer order.
     */
    estimated_metadata = rt->resource_plan.estimated_metadata_bytes;
    if (rt->task_prewarm_source != LLAM_RUNTIME_PREWARM_PUBLIC_EXACT) {
        uint64_t available =
            LLAM_RUNTIME_METADATA_BUDGET_BYTES - estimated_metadata;

        task_total = llam_runtime_bound_task_prewarm_total(
            task_total,
            rt->active_shards,
            available,
            &task_storage_objects);
        estimated_metadata += task_storage_objects * sizeof(llam_task_t);
    }
    if (rt->timer_prewarm_source != LLAM_RUNTIME_PREWARM_PUBLIC_EXACT) {
        uint64_t available =
            LLAM_RUNTIME_METADATA_BUDGET_BYTES - estimated_metadata;
        uint64_t bounded = available / sizeof(llam_timer_node_t);

        if (timer_total > bounded) {
            timer_total = bounded;
        }
        estimated_metadata += timer_total * sizeof(llam_timer_node_t);
    }

    rt->requested_task_prewarm_total = task_total;
    rt->requested_stack_prewarm_total = stack_total;
    rt->requested_timer_prewarm_total = timer_total;
    rt->achieved_task_prewarm_total = 0U;
    rt->achieved_stack_prewarm_total = 0U;
    rt->achieved_timer_prewarm_total = 0U;
    rt->estimated_metadata_bytes = estimated_metadata;
    rt->estimated_stack_mapping_bytes =
        stack_total * LLAM_RUNTIME_STACK_MAPPING_ESTIMATE_BYTES;
    return 0;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
/** Internal-only deterministic allocation budgets for exact rollback tests. */
static atomic_uint_fast64_t g_llam_test_prewarm_limits[LLAM_TEST_PREWARM_KIND_COUNT] = {
    UINT64_MAX,
    UINT64_MAX,
    UINT64_MAX,
};

void llam_runtime_test_set_prewarm_allocation_limit(
    llam_test_prewarm_kind_t kind,
    uint64_t successful_objects) {
    if ((unsigned)kind >= LLAM_TEST_PREWARM_KIND_COUNT) {
        return;
    }
    atomic_store_explicit(&g_llam_test_prewarm_limits[kind],
                          successful_objects,
                          memory_order_release);
}

void llam_runtime_test_reset_prewarm_allocation_limits(void) {
    for (unsigned kind = 0U; kind < LLAM_TEST_PREWARM_KIND_COUNT; ++kind) {
        atomic_store_explicit(&g_llam_test_prewarm_limits[kind],
                              UINT64_MAX,
                              memory_order_release);
    }
}

bool llam_runtime_test_prewarm_allocation_permitted(
    llam_test_prewarm_kind_t kind,
    uint64_t objects) {
    uint64_t remaining;

    if ((unsigned)kind >= LLAM_TEST_PREWARM_KIND_COUNT) {
        return true;
    }
    remaining = atomic_load_explicit(&g_llam_test_prewarm_limits[kind],
                                     memory_order_acquire);
    for (;;) {
        if (remaining == UINT64_MAX) {
            return true;
        }
        if (objects > remaining) {
            return false;
        }
        if (atomic_compare_exchange_weak_explicit(
                &g_llam_test_prewarm_limits[kind],
                &remaining,
                remaining - objects,
                memory_order_acq_rel,
                memory_order_acquire)) {
            return true;
        }
    }
}
#endif

/**
 * @brief Preallocate timer heap slots from one runtime-total authority.
 *
 * Each shard receives its quotient/remainder share in stable order. Legacy and
 * environment totals may complete partially; exact public requests instead
 * make initialization unwind through normal runtime teardown.
 */
int llam_runtime_prewarm_timer_heaps(llam_runtime_t *rt,
                                     uint64_t total,
                                     bool exact,
                                     uint64_t *achieved) {
    const uint64_t max_total =
        LLAM_RUNTIME_METADATA_BUDGET_BYTES / sizeof(llam_timer_node_t);
    uint64_t completed = 0U;
    int saved_errno = errno;

    if (achieved != NULL) {
        *achieved = 0U;
    }
    if (rt == NULL || rt->shards == NULL || rt->active_shards == 0U ||
        achieved == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (total > max_total) {
        errno = E2BIG;
        return -1;
    }

    for (unsigned shard_id = 0U; shard_id < rt->active_shards; ++shard_id) {
        uint64_t share =
            llam_runtime_prewarm_share(total, rt->active_shards, shard_id);
        llam_timer_node_t **heap;

        if (share == 0U) {
            continue;
        }
        if (share > SIZE_MAX / sizeof(*heap)) {
            errno = E2BIG;
            goto exhausted;
        }
#if defined(LLAM_ENABLE_TEST_HOOKS)
        if (!llam_runtime_test_prewarm_allocation_permitted(
                LLAM_TEST_PREWARM_TIMER,
                share)) {
            errno = ENOMEM;
            goto exhausted;
        }
#endif
        heap = calloc((size_t)share, sizeof(*heap));
        if (heap == NULL) {
            errno = ENOMEM;
            goto exhausted;
        }
        rt->shards[shard_id].timer_heap = heap;
        rt->shards[shard_id].timer_heap_cap = (size_t)share;
        completed += share;
    }

    *achieved = completed;
    errno = saved_errno;
    return 0;

exhausted:
    *achieved = completed;
    if (exact) {
        return -1;
    }
    errno = saved_errno;
    return 0;
}

/**
 * @brief Parse the automatic preemption policy from an environment override.
 */
static unsigned llam_runtime_preempt_mode_from_env(unsigned default_mode) {
    const char *env = llam_env_get("LLAM_PREEMPT_MODE");

    if (env == NULL || env[0] == '\0') {
        return default_mode;
    }
    if (llam_env_flag_value(env, 1U) == 0U || strcmp(env, "disabled") == 0) {
        return LLAM_PREEMPT_OFF;
    }
    if (strcmp(env, "1") == 0 || strcmp(env, "cooperative") == 0 || strcmp(env, "coop") == 0) {
        return LLAM_PREEMPT_COOPERATIVE;
    }
    if (strcmp(env, "3") == 0 || strcmp(env, "strict") == 0 || strcmp(env, "debug") == 0) {
        return LLAM_PREEMPT_STRICT;
    }
    if (strcmp(env, "2") == 0 || strcmp(env, "auto") == 0 || strcmp(env, "pressure") == 0) {
        return LLAM_PREEMPT_AUTO;
    }
    return default_mode;
}

/**
 * @brief Validate a public preemption mode value.
 */
static bool llam_public_preempt_mode_valid(uint32_t mode) {
    return mode == LLAM_PREEMPT_OFF ||
           mode == LLAM_PREEMPT_COOPERATIVE ||
           mode == LLAM_PREEMPT_AUTO ||
           mode == LLAM_PREEMPT_STRICT;
}

/** @brief Validate a public scheduler-thread CPU affinity policy. */
static bool llam_public_affinity_policy_valid(uint32_t policy) {
    return policy == LLAM_RUNTIME_AFFINITY_NONE ||
           policy == LLAM_RUNTIME_AFFINITY_PREFER ||
           policy == LLAM_RUNTIME_AFFINITY_REQUIRE;
}

/**
 * @brief Initialize one LLAM runtime instance.
 *
 * Each runtime instance may be initialized only once at a time. This function
 * consumes caller options, applies environment overrides, allocates scheduler
 * shards and I/O nodes, starts runtime-owned background workers, and references
 * process diagnostics shared by peer runtimes. The final @c initialized flag is
 * published only after all required subsystems are ready.
 *
 * @param opts Optional runtime options. Passing @c NULL uses non-deterministic
 *             balanced defaults with environment overrides still honored.
 * @param opts_size Size of the caller's option struct when @p opts is non-NULL.
 *
 * @return 0 on success.
 * @return -1 with @c errno set on invalid state, CPU discovery failure,
 *         allocation failure, pthread failure, I/O setup failure, or signal
 *         handler installation failure.
 *
 * @see llam_runtime_shutdown
 * @see llam_run
 */
static int llam_runtime_init_ex_rt_unlocked(llam_runtime_t *rt,
                                            const llam_runtime_opts_t *opts,
                                            size_t opts_size,
                                            bool heap_allocated) {
    llam_runtime_opts_t raw_opts;
    llam_runtime_opts_t opts_storage;
    llam_runtime_resource_plan_input_t resource_input;
    llam_runtime_resource_plan_t resource_plan;
    unsigned i;
    unsigned observed;
    unsigned observed_total;
    unsigned initial_online_shards;
    unsigned locality_nodes = 0U;
    unsigned *cpus = NULL;
    unsigned *locality_node_ids;
    unsigned *io_node_ids = NULL;
    size_t altstack_size;
    const char *light_safepoint_env;
    const char *spawn_fanout_env;
    const char *task_list_eager_env;
    unsigned cheap_safepoint_default;
    unsigned task_list_eager_default;
    unsigned *selected_cpus;
    size_t opts_copy_size;
    uint64_t experimental_flags;
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    unsigned windows_unsafe_skip_scheduler_simd;
    unsigned windows_unsafe_skip_task_simd;
#endif
#if LLAM_ARCH_AARCH64
    unsigned aarch64_unsafe_skip_scheduler_simd;
#endif
    int rc;

    if (rt == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * Shutdown release failures outlive their former runtime handle. A later
     * lifecycle opportunity retries one bounded process-quarantine batch.
     */
    llam_stack_cache_quarantine_retry();

    if (opts != NULL) {
        /*
         * Inbound options are ABI-prefix structs.  Copy only the caller's known
         * prefix into a full local object, then read each field behind a
         * prefix-size guard so old bindings can initialize a newer library.
         */
        if (opts_size == 0U) {
            errno = EINVAL;
            return -1;
        }
        memset(&raw_opts, 0, sizeof(raw_opts));
        opts_copy_size = opts_size < sizeof(raw_opts) ? opts_size : sizeof(raw_opts);
        memcpy(&raw_opts, opts, opts_copy_size);

        llam_runtime_opts_copy_prefix(&raw_opts, opts_size, &opts_storage);
        opts = &opts_storage;
        if (!llam_public_runtime_profile_valid(opts->profile)) {
            errno = EINVAL;
            return -1;
        }
        if (!llam_public_preempt_mode_valid(opts->preempt_mode)) {
            errno = EINVAL;
            return -1;
        }
        if (!llam_public_affinity_policy_valid(opts->affinity_policy)) {
            errno = EINVAL;
            return -1;
        }
    }

    if (atomic_load_explicit(&rt->initialized, memory_order_acquire)) {
        errno = EBUSY;
        return -1;
    }

    memset(rt, 0, sizeof(*rt));
    if (llam_runtime_register_handle(rt, heap_allocated) != 0) {
        return -1;
    }
    g_llam_tls_shard = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
#if LLAM_RUNTIME_BACKEND_WINDOWS
    {
        WSADATA wsa_data;
        int wsa_rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);

        if (wsa_rc != 0) {
            errno = llam_windows_wsa_error_to_errno(wsa_rc);
            llam_runtime_unregister_handle(rt);
            memset(rt, 0, sizeof(*rt));
            return -1;
        }
        rt->winsock_started = true;
    }
#endif

    errno = 0;
    observed = llam_count_allowed_cpus(&cpus);
    if (observed == 0U) {
        int saved_errno = errno != 0 ? errno : ENODEV;

        if (errno == 0) {
            errno = ENODEV;
        }
        return llam_runtime_init_fail_registered(rt, saved_errno);
    }
    observed_total = observed;
    memset(&resource_input, 0, sizeof(resource_input));
    resource_input.opts = opts;
    resource_input.opts_size = opts != NULL ? opts_size : 0U;
    resource_input.allowed_cpus = cpus;
    resource_input.allowed_cpu_count = observed;
    resource_input.affinity_supported = llam_runtime_affinity_supported();
#if LLAM_RUNTIME_BACKEND_LINUX
    resource_input.sqpoll_supported = true;
#else
    resource_input.sqpoll_supported = false;
#endif
    resource_input.page_size = (size_t)llam_page_size();
    if (llam_runtime_resource_plan_resolve(&resource_input, &resource_plan) != 0) {
        int saved_errno = errno;

        free(cpus);
        return llam_runtime_init_fail_registered(rt, saved_errno);
    }
    selected_cpus = calloc(resource_plan.selected_cpu_count, sizeof(*selected_cpus));
    if (selected_cpus == NULL) {
        free(cpus);
        return llam_runtime_init_fail_registered(rt, ENOMEM);
    }
    for (i = 0U; i < resource_plan.selected_cpu_count; ++i) {
        selected_cpus[i] = resource_plan.selected_cpus[i];
    }
    free(cpus);
    cpus = selected_cpus;
    rt->resource_plan = resource_plan;
    atomic_init(&rt->scheduler_threads_live, 0U);
    atomic_init(&rt->io_threads_live, 0U);
    atomic_init(&rt->controller_threads_live, 0U);
    atomic_init(&rt->opaque_helper_threads_live, 0U);
    atomic_init(&rt->host_threads_live, 0U);
    atomic_init(&rt->affinity_failures, 0U);
    atomic_init(&rt->stack_cache_account_writer, 0U);
    atomic_init(&rt->stack_cache_account_seq, 0U);
    atomic_init(&rt->stack_cache_cached_bytes, 0U);
    atomic_init(&rt->stack_cache_cached_mappings, 0U);
    atomic_init(&rt->stack_cache_committed_bytes, 0U);
    atomic_init(&rt->stack_cache_trim_requests, 0U);
    atomic_init(&rt->stack_cache_discarded_bytes, 0U);
    atomic_init(&rt->stack_cache_released_bytes, 0U);
    atomic_init(&rt->stack_cache_budget_rejections, 0U);
    atomic_init(&rt->stack_cache_secure_return_failures, 0U);
    atomic_init(&rt->stack_cache_resident_bytes, 0U);
    atomic_init(&rt->stack_cache_resident_sample_ns, 0U);
    atomic_init(&rt->stack_cache_last_idle_scan_ns, llam_now_ns());
    atomic_init(&rt->stack_cache_resident_valid, 0U);

    /*
     * From this point on, runtime policy is resolved once and stored on the
     * runtime object. Hot paths read these plain fields instead of re-checking
     * environment variables or caller option structs.
     */
    experimental_flags = opts != NULL ? opts->experimental_flags : 0U;
    rt->deterministic = opts != NULL ? (opts->deterministic != 0U) : 0U;
    rt->forced_yield_every = opts != NULL ? opts->forced_yield_every : 0U;
    rt->experimental_shard_rings =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS) != 0U ? 1U : 0U;
    rt->experimental_shard_rings_multishot =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT) != 0U ? 1U : 0U;
    rt->experimental_dynamic_shards =
        resource_plan.worker_min < resource_plan.worker_max ? 1U : 0U;
    rt->experimental_lockfree_normq =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ) != 0U ? 1U : 0U;
    rt->experimental_huge_alloc_requested =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC) != 0U ? 1U : 0U;
    rt->idle_spin_ns = opts != NULL ? opts->idle_spin_ns : 0U;
    rt->idle_spin_max_iters = opts != NULL ? opts->idle_spin_max_iters : 0U;
    rt->experimental_sqpoll_requested =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL) != 0U ? 1U : 0U;
    rt->sqpoll_cpu_reserved = resource_plan.sqpoll_reserved ? 1U : 0U;
    rt->sqpoll_cpu = resource_plan.sqpoll_cpu;
    rt->profile =
        llam_runtime_profile_from_env(opts != NULL ? (llam_runtime_profile_t)opts->profile : LLAM_RUNTIME_PROFILE_BALANCED);
    rt->preempt_mode = llam_runtime_preempt_mode_from_env(opts != NULL ? opts->preempt_mode : LLAM_PREEMPT_AUTO);
    rt->preempt_quantum_ns = opts != NULL ? opts->preempt_quantum_ns : 0U;
    rt->preempt_quantum_ns =
        llam_runtime_env_u64("LLAM_PREEMPT_QUANTUM_NS", rt->preempt_quantum_ns, 1000000000ULL);
    light_safepoint_env = llam_env_get("LLAM_DIAG_LIGHT_SAFEPOINT");
    rt->trace_events_enabled = llam_runtime_env_flag("LLAM_TRACE_EVENTS", 0U);
    rt->wake_latency_metrics_enabled = llam_runtime_env_flag("LLAM_WAKE_LATENCY_METRICS", 0U);
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
    windows_unsafe_skip_scheduler_simd = llam_runtime_env_flag("LLAM_WINDOWS_UNSAFE_SKIP_SCHEDULER_SIMD", 0U);
    windows_unsafe_skip_task_simd = llam_runtime_env_flag("LLAM_WINDOWS_UNSAFE_SKIP_TASK_SIMD", 0U);
    rt->windows_unsafe_skip_task_simd = windows_unsafe_skip_task_simd;
#endif
#if LLAM_ARCH_AARCH64
    aarch64_unsafe_skip_scheduler_simd =
        llam_runtime_env_flag("LLAM_AARCH64_UNSAFE_SKIP_SCHEDULER_SIMD", 0U) ||
        llam_runtime_env_flag("LLAM_ARM64_UNSAFE_SKIP_SCHEDULER_SIMD", 0U);
#endif
    rt->stack_sampling_enabled =
        llam_runtime_env_flag("LLAM_STACK_SAMPLING", 0U) ||
        (light_safepoint_env != NULL &&
         light_safepoint_env[0] != '\0' &&
         llam_env_flag_value(light_safepoint_env, 1U) == 0U) ||
        llam_runtime_env_flag("LLAM_STRICT_SAFEPOINT", 0U);
    rt->run_timing_enabled =
        llam_runtime_env_flag("LLAM_RUN_TIMING", 0U) ||
        rt->stack_sampling_enabled != 0U;
    task_list_eager_default = rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ? 1U : 0U;
    task_list_eager_env = llam_env_get("LLAM_TASK_LIST_EAGER");
    if (task_list_eager_env != NULL && task_list_eager_env[0] != '\0') {
        rt->task_list_eager = llam_env_flag_value(task_list_eager_env, task_list_eager_default) != 0U ? 1U : 0U;
    } else {
        rt->task_list_eager = task_list_eager_default;
    }
    rt->direct_handoff_stats_enabled = llam_runtime_env_flag("LLAM_DIRECT_HANDOFF_STATS", 0U);
    rt->direct_handoff_stats_sample_mask = 0U;
    rt->autotune_wake_latency_sample_mask = 0U;
#if LLAM_RUNTIME_BACKEND_WINDOWS
    rt->direct_handoff_burst = llam_runtime_env_u32("LLAM_YIELD_DIRECT_HANDOFF_BURST", 64U, 65535U);
#elif LLAM_RUNTIME_BACKEND_LINUX || LLAM_RUNTIME_BACKEND_KQUEUE
    rt->direct_handoff_burst = llam_runtime_env_u32("LLAM_YIELD_DIRECT_HANDOFF_BURST", 64U, 65535U);
#else
    rt->direct_handoff_burst = llam_runtime_env_u32("LLAM_YIELD_DIRECT_HANDOFF_BURST", 0U, 65535U);
#endif
    atomic_init(&rt->direct_handoff_budget, rt->direct_handoff_burst);
#if defined(__APPLE__)
    /*
     * Darwin sleep fanout benefits from direct handoff while timers are armed,
     * but the bounded burst above still returns to the scheduler frequently
     * enough to fire near-deadline timers.
     */
    rt->direct_handoff_allow_timers =
        llam_runtime_env_flag("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS",
                            rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ? 0U : 1U);
#else
    rt->direct_handoff_allow_timers =
        llam_runtime_env_flag("LLAM_YIELD_DIRECT_HANDOFF_ALLOW_TIMERS", 0U);
#endif
    cheap_safepoint_default = llam_runtime_env_flag("LLAM_STRICT_SAFEPOINT", 0U) != 0U ? 0U : 1U;
    if (light_safepoint_env != NULL && light_safepoint_env[0] != '\0') {
        rt->cheap_safepoint = llam_env_flag_value(light_safepoint_env, cheap_safepoint_default) != 0U ? 1U : 0U;
    } else {
        rt->cheap_safepoint = cheap_safepoint_default;
    }
    rt->safepoint_clock_period = rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 128U : 32U;
    if (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY) {
        rt->safepoint_clock_period = 64U;
    }
    if (rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE) {
        rt->safepoint_clock_period = 1U;
    }
    rt->safepoint_clock_period =
        llam_runtime_env_u32("LLAM_SAFEPOINT_CLOCK_PERIOD", rt->safepoint_clock_period, 4096U);
#if LLAM_RUNTIME_BACKEND_WINDOWS
    rt->preempt_poll_period = rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY ? 16U : 32U;
#elif defined(__APPLE__)
    rt->preempt_poll_period = rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 16U : 8U;
    if (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY) {
        rt->preempt_poll_period = 16U;
    }
#else
    rt->preempt_poll_period = rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 4U : 1U;
    if (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY) {
        rt->preempt_poll_period = 4U;
    }
#endif
    if (rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE) {
        rt->preempt_poll_period = 1U;
    }
    if (opts != NULL && opts->preempt_poll_period != 0U) {
        rt->preempt_poll_period = opts->preempt_poll_period > 4096U ? 4096U : opts->preempt_poll_period;
    }
    rt->preempt_poll_period = llam_runtime_env_u32("LLAM_PREEMPT_POLL_PERIOD", rt->preempt_poll_period, 4096U);
    if (rt->preempt_mode == LLAM_PREEMPT_OFF) {
        rt->preempt_poll_period = 0U;
    } else if (rt->preempt_mode == LLAM_PREEMPT_STRICT) {
        rt->preempt_poll_period = 1U;
        rt->safepoint_clock_period = 1U;
    }
    rt->wake_handoff_enabled =
        llam_runtime_env_flag("LLAM_WAKE_HANDOFF",
                            rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ? 0U : 1U);
    rt->channel_local_handoff_enabled =
        llam_runtime_env_flag("LLAM_CHANNEL_LOCAL_HANDOFF", rt->wake_handoff_enabled);
    rt->channel_safepoint_interval =
        rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE
            ? 1U
            : (rt->profile == LLAM_RUNTIME_PROFILE_RELEASE_FAST ? 64U : 32U);
    if (rt->profile == LLAM_RUNTIME_PROFILE_IO_LATENCY) {
        rt->channel_safepoint_interval = 32U;
    }
    rt->channel_safepoint_interval =
        llam_runtime_env_u32("LLAM_CHANNEL_SAFEPOINT_INTERVAL", rt->channel_safepoint_interval, 65536U);
    if (rt->channel_safepoint_interval == 0U) {
        rt->channel_safepoint_interval = 1U;
    }
    /*
     * Some experimental policies are mutually exclusive because they assume a
     * specific ownership model for workers or kernel submission threads.  Resolve
     * those conflicts during init so backends see a single coherent policy.
     */
    if (rt->experimental_shard_rings != 0U) {
        rt->experimental_sqpoll_requested = 0U;
    }
    rt->observed_shards = observed_total;
    rt->active_shards = resource_plan.worker_max;
    if (llam_runtime_resolve_prewarm_authority(rt) != 0) {
        int saved_errno = errno;

        free(cpus);
        return llam_runtime_init_fail_registered(rt, saved_errno);
    }
#if LLAM_RUNTIME_BACKEND_WINDOWS
    rt->direct_handoff_live_limit =
        llam_runtime_env_u32("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT",
                            rt->active_shards <= 2U ? 0U : 16U,
                            1048576U);
#else
    rt->direct_handoff_live_limit =
        llam_runtime_env_u32("LLAM_YIELD_DIRECT_HANDOFF_LIVE_LIMIT", 0U, 1048576U);
#endif
#if LLAM_RUNTIME_BACKEND_WINDOWS
    /* Windows needs earlier kicks to avoid lock-free fanout starvation. */
    rt->spawn_fanout_wake_interval =
        rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ? 0U : 128U;
    rt->spawn_fanout_adaptive = llam_runtime_env_flag("LLAM_SPAWN_FANOUT_ADAPTIVE", 0U);
#else
    rt->spawn_fanout_wake_interval = 0U;
    rt->spawn_fanout_adaptive = llam_runtime_env_flag("LLAM_SPAWN_FANOUT_ADAPTIVE", 0U);
#endif
    spawn_fanout_env = llam_env_get("LLAM_SPAWN_FANOUT_WAKE_INTERVAL");
    rt->spawn_fanout_wake_interval_forced =
        spawn_fanout_env != NULL && spawn_fanout_env[0] != '\0' ? 1U : 0U;
    rt->spawn_fanout_wake_interval =
        llam_runtime_env_u32("LLAM_SPAWN_FANOUT_WAKE_INTERVAL", rt->spawn_fanout_wake_interval, 65535U);
    initial_online_shards = resource_plan.worker_count;
    rt->dynamic_online_floor = resource_plan.worker_min;
    atomic_store(&rt->online_shards, initial_online_shards);
    atomic_store(&rt->online_shards_min, initial_online_shards);
    atomic_store(&rt->online_shards_max, initial_online_shards);
    atomic_store(&rt->steal_pause_active, 0U);
    atomic_store(&rt->live_tasks, 0U);
    atomic_store(&rt->live_task_shards, 0U);
    atomic_store(&rt->active_io_waiters, 0U);
    atomic_store(&rt->deferred_fatal_pending, 0U);
    atomic_store_explicit(&rt->next_spawn_shard, 0U, memory_order_relaxed);
    rt->allowed_cpus = cpus;
    (void)llam_detect_xsave_support(rt);
    altstack_size = LLAM_ALTSTACK_BYTES;
    if (altstack_size < (size_t)SIGSTKSZ) {
        altstack_size = (size_t)SIGSTKSZ;
    }

    rt->shards = calloc(rt->active_shards, sizeof(*rt->shards));
    if (rt->shards == NULL) {
        /*
         * rt->allowed_cpus was already published for partial-init cleanup.
         * Let shutdown consume it exactly once; freeing @c cpus directly here
         * leaves a dangling runtime pointer and makes caller cleanup double-free.
         */
        llam_runtime_shutdown_rt(rt);
        errno = ENOMEM;
        return -1;
    }
    /*
     * Shutdown always walks the full published shard array. Prime every wake
     * descriptor before any later allocation can fail; otherwise untouched
     * calloc-backed entries look like fd 0 and partial-init cleanup can close
     * the caller's stdin.
     */
    for (i = 0; i < rt->active_shards; ++i) {
        rt->shards[i].event_fd = -1;
    }

    locality_node_ids = calloc(rt->active_shards, sizeof(*locality_node_ids));
    if (locality_node_ids == NULL) {
        /*
         * The runtime is already registered and owns rt->allowed_cpus/shards.
         * Reuse normal partial-init teardown so the live handle registry cannot
         * retain a pointer to zeroed storage.
         */
        llam_runtime_shutdown_rt(rt);
        errno = ENOMEM;
        return -1;
    }
    if (rt->experimental_shard_rings != 0U) {
        io_node_ids = calloc(rt->active_shards, sizeof(*io_node_ids));
        if (io_node_ids == NULL) {
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            errno = ENOMEM;
            return -1;
        }
    }

    for (i = 0; i < rt->active_shards; ++i) {
        unsigned kernel_node_id = llam_detect_cpu_node(rt->allowed_cpus[i]);
        unsigned node_index = llam_find_or_add_node_id(locality_node_ids, &locality_nodes, rt->active_shards, kernel_node_id);

        rt->shards[i].runtime = rt;
        rt->shards[i].id = i;
        rt->shards[i].cpu_id = rt->allowed_cpus[i];
        rt->shards[i].node_index = node_index;
        rt->shards[i].io_node_index = rt->experimental_shard_rings != 0U ? i : node_index;
        llam_metrics_init(&rt->shards[i].metrics);
        llam_trace_ring_init(&rt->shards[i]);
        atomic_init(&rt->shards[i].online, i < initial_online_shards ? 1U : 0U);
        atomic_init(&rt->shards[i].inflight_io_waiters, 0U);
        atomic_init(&rt->shards[i].merge_pause_requested, 0U);
        atomic_init(&rt->shards[i].merge_pause_ack, 0U);
        atomic_init(&rt->shards[i].steal_pause_ack, 0U);
        atomic_init(&rt->shards[i].live_tasks, 0U);
        llam_cldeque_init(&rt->shards[i].norm_cldeque);
        atomic_init(&rt->shards[i].inject_depth, 0U);
        atomic_init(&rt->shards[i].norm_depth, 0U);
        atomic_init(&rt->shards[i].timer_count, 0U);
        atomic_init(&rt->shards[i].timer_callbacks_active, 0U);
        atomic_init(&rt->shards[i].current, NULL);
        atomic_init(&rt->shards[i].opaque_helper_active_hint, 0U);
#if defined(__linux__)
        atomic_init(&rt->shards[i].opaque_helper_opaque_wait, 0U);
#endif
        rt->shards[i].opaque_redirect_target_id = UINT_MAX;
#if LLAM_RUNTIME_BACKEND_LINUX || LLAM_PLATFORM_WINDOWS || LLAM_RUNTIME_BACKEND_KQUEUE
        atomic_init(&rt->shards[i].opaque_wake_seq, 0U);
#endif
        if (io_node_ids != NULL) {
            io_node_ids[i] = kernel_node_id;
        }
        rt->shards[i].event_fd = llam_wake_handle_create();
        if (rt->shards[i].event_fd < 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rc = pthread_mutex_init(&rt->shards[i].lock, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->shards[i].lock_initialized = true;
        rc = pthread_mutex_init(&rt->shards[i].stack_cache_lock, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->shards[i].stack_cache_lock_initialized = true;
        rc = pthread_mutex_init(&rt->shards[i].opaque_lock, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->shards[i].opaque_lock_initialized = true;
        rc = pthread_cond_init(&rt->shards[i].opaque_cv, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->shards[i].opaque_cv_initialized = true;
        if (llam_opaque_wake_init(&rt->shards[i]) != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->shards[i].signal_stack = mmap(NULL,
                                          altstack_size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS,
                                          -1,
                                          0);
        if (rt->shards[i].signal_stack == MAP_FAILED) {
            rt->shards[i].signal_stack = NULL;
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->shards[i].signal_stack_size = altstack_size;
        rt->shards[i].previous_sigaltstack.ss_flags = SS_DISABLE;
        if (llam_allocator_init(&rt->shards[i].allocator) != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        if (llam_ctx_init_fp_state(&rt->shards[i].scheduler_ctx, rt) != 0 ||
            llam_ctx_init_fp_state(&rt->shards[i].opaque_scheduler_ctx, rt) != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
#if LLAM_PLATFORM_WINDOWS && LLAM_ARCH_X86_64
        if (windows_unsafe_skip_scheduler_simd != 0U) {
            /*
             * Opt-in ceiling mode: task contexts still preserve XMM6-XMM15, but
             * scheduler contexts skip their own SIMD save/restore. This is not
             * the default because Windows x64 treats XMM6-XMM15 as callee-saved.
             */
            rt->shards[i].scheduler_ctx.simd_flags =
                LLAM_CTX_SIMD_F_SKIP_SAVE | LLAM_CTX_SIMD_F_SKIP_RESTORE;
            rt->shards[i].opaque_scheduler_ctx.simd_flags =
                LLAM_CTX_SIMD_F_SKIP_SAVE | LLAM_CTX_SIMD_F_SKIP_RESTORE;
        }
#endif
#if LLAM_ARCH_AARCH64
        if (aarch64_unsafe_skip_scheduler_simd != 0U) {
            /*
             * Opt-in ceiling mode: task contexts still preserve d8-d15 and
             * FPCR/FPSR, but scheduler contexts skip their own SIMD path.
             */
            rt->shards[i].scheduler_ctx.simd_flags =
                LLAM_CTX_SIMD_F_SKIP_SAVE | LLAM_CTX_SIMD_F_SKIP_RESTORE;
            rt->shards[i].opaque_scheduler_ctx.simd_flags =
                LLAM_CTX_SIMD_F_SKIP_SAVE | LLAM_CTX_SIMD_F_SKIP_RESTORE;
        }
#endif
        atomic_store_explicit(&rt->shards[i].last_safepoint_ns, llam_now_ns(), memory_order_relaxed);
        atomic_store(&rt->shards[i].last_run_started_ns, 0U);
    }
    if (llam_runtime_prewarm_timer_heaps(
            rt,
            rt->requested_timer_prewarm_total,
            rt->timer_prewarm_source == LLAM_RUNTIME_PREWARM_PUBLIC_EXACT,
            &rt->achieved_timer_prewarm_total) != 0 ||
        llam_runtime_prewarm_task_allocators(
            rt,
            rt->requested_task_prewarm_total,
            rt->task_prewarm_source == LLAM_RUNTIME_PREWARM_PUBLIC_EXACT,
            &rt->achieved_task_prewarm_total) != 0) {
        int saved_errno = errno != 0 ? errno : ENOMEM;

        free(io_node_ids);
        free(locality_node_ids);
        llam_runtime_shutdown_rt(rt);
        errno = saved_errno;
        return -1;
    }

    rt->active_nodes = rt->experimental_shard_rings != 0U ? rt->active_shards : locality_nodes;
    llam_autotune_init(rt);

    rt->kernel_node_ids = calloc(rt->active_nodes, sizeof(*rt->kernel_node_ids));
    if (rt->kernel_node_ids == NULL) {
        free(io_node_ids);
        free(locality_node_ids);
        llam_runtime_shutdown_rt(rt);
        errno = ENOMEM;
        return -1;
    }
    rt->nodes = calloc(rt->active_nodes, sizeof(*rt->nodes));
    if (rt->nodes == NULL) {
        free(io_node_ids);
        free(locality_node_ids);
        llam_runtime_shutdown_rt(rt);
        errno = ENOMEM;
        return -1;
    }
    /*
     * As with shards, node shutdown scans the entire published node array even
     * when init fails mid-loop. Preload invalid wake descriptors for untouched
     * nodes so cleanup cannot close fd 0.
     */
    for (i = 0; i < rt->active_nodes; ++i) {
        rt->nodes[i].event_fd = -1;
    }

    {
        const unsigned *node_ids =
            rt->experimental_shard_rings != 0U ? io_node_ids : locality_node_ids;

        if (node_ids == NULL) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown_rt(rt);
            errno = ENOMEM;
            return -1;
        }
        memcpy(rt->kernel_node_ids, node_ids, rt->active_nodes * sizeof(*node_ids));
    }
    free(io_node_ids);
    free(locality_node_ids);

    for (i = 0; i < rt->active_nodes; ++i) {
        rt->nodes[i].runtime = rt;
        rt->nodes[i].index = i;
        rt->nodes[i].kernel_node_id = rt->kernel_node_ids[i];
        rt->nodes[i].sqpoll_enabled = false;
        rt->nodes[i].sqpoll_cpu = UINT_MAX;
        atomic_init(&rt->nodes[i].submit_batches, 0U);
        atomic_init(&rt->nodes[i].submit_entries, 0U);
        atomic_init(&rt->nodes[i].submit_calls, 0U);
        atomic_init(&rt->nodes[i].submit_syscalls, 0U);
        atomic_init(&rt->nodes[i].windows_cancel_controls, 0U);
        atomic_init(&rt->nodes[i].windows_cancel_success, 0U);
        atomic_init(&rt->nodes[i].windows_cancel_failures, 0U);
        atomic_init(&rt->nodes[i].windows_cancel_not_found, 0U);
        atomic_init(&rt->nodes[i].max_submit_batch, 0U);
        atomic_init(&rt->nodes[i].last_cq_depth, 0U);
        atomic_init(&rt->nodes[i].max_cq_depth, 0U);
        atomic_init(&rt->nodes[i].unsupported_ops, 0U);
        atomic_init(&rt->nodes[i].provided_buf_acquires, 0U);
        atomic_init(&rt->nodes[i].provided_buf_returns, 0U);
        rt->nodes[i].event_fd = llam_wake_handle_create();
        if (rt->nodes[i].event_fd < 0) {
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rc = pthread_mutex_init(&rt->nodes[i].submit_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->nodes[i].submit_lock_initialized = true;
        rc = pthread_mutex_init(&rt->nodes[i].windows_assoc_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->nodes[i].windows_assoc_lock_initialized = true;
        rc = pthread_mutex_init(
            &rt->nodes[i].windows_op_pool_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->nodes[i].windows_op_pool_lock_initialized = true;
        rc = pthread_mutex_init(&rt->nodes[i].watch_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->nodes[i].watch_lock_initialized = true;
        rc = pthread_mutex_init(&rt->nodes[i].recv_buf_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->nodes[i].recv_buf_lock_initialized = true;
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
        rc = pthread_mutex_init(
            &rt->nodes[i].native_resource_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown_rt(rt);
            return -1;
        }
        rt->nodes[i].native_resource_lock_initialized = true;
#endif
        if (llam_node_init_ring(rt, &rt->nodes[i]) == 0) {
            rt->nodes[i].ring_ready = true;
            llam_probe_ring_support(&rt->nodes[i]);
            (void)llam_node_setup_recv_buf_ring(&rt->nodes[i]);
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
            (void)llam_linux_native_resources_setup(
                &rt->nodes[i]);
#endif
            if (rt->experimental_shard_rings != 0U && rt->experimental_shard_rings_multishot == 0U) {
                rt->nodes[i].supports_multishot_recv = false;
                rt->nodes[i].supports_multishot_accept = false;
                rt->nodes[i].supports_multishot_poll = false;
            }
            if (pthread_create(&rt->nodes[i].thread, NULL, llam_io_worker_main, &rt->nodes[i]) == 0) {
                rt->nodes[i].thread_started = true;
            } else {
#if LLAM_RUNTIME_BACKEND_LINUX && LLAM_BUILD_RESEARCH
                llam_linux_native_resources_before_ring_exit(
                    &rt->nodes[i]);
#endif
                io_uring_queue_exit(&rt->nodes[i].ring);
                rt->nodes[i].ring_ready = false;
#if LLAM_RUNTIME_BACKEND_LINUX
                rt->nodes[i].linux_ring_features = 0U;
#if LLAM_BUILD_RESEARCH
                llam_linux_native_resources_after_ring_exit(
                    &rt->nodes[i]);
#endif
#endif
            }
        }
    }

    rc = pthread_mutex_init(&rt->task_list_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->task_list_lock_initialized = true;
    rc = pthread_mutex_init(&rt->stack_cache_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->stack_cache_lock_initialized = true;
    rc = pthread_mutex_init(&rt->stack_cache_trim_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->stack_cache_trim_lock_initialized = true;
    if (llam_runtime_prewarm_stack_cache(
            rt,
            rt->requested_stack_prewarm_total,
            rt->stack_prewarm_source == LLAM_RUNTIME_PREWARM_PUBLIC_EXACT,
            &rt->achieved_stack_prewarm_total) != 0) {
        int saved_errno = errno != 0 ? errno : ENOMEM;

        llam_runtime_shutdown_rt(rt);
        errno = saved_errno;
        return -1;
    }

    rc = pthread_mutex_init(&rt->block_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->block_lock_initialized = true;
    rc = pthread_cond_init(&rt->block_cv, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->block_cv_initialized = true;
    atomic_store(&rt->block_job_free, NULL);
    atomic_store(&rt->block_wake_seq, 0U);

    rc = pthread_mutex_init(&rt->overflow_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->overflow_lock_initialized = true;
    atomic_store(&rt->overflow_depth, 0U);

    rt->block_worker_count = resource_plan.blocking_max;
    atomic_init(&rt->block_threads_started, 0U);
    atomic_init(&rt->block_threads_entered, 0U);
    atomic_init(&rt->block_threads_exited, 0U);
    atomic_init(&rt->block_threads_live, 0U);
    atomic_init(&rt->block_thread_create_failures, 0U);
    rt->block_threads = calloc(rt->block_worker_count, sizeof(*rt->block_threads));
    if (rt->block_threads == NULL) {
        llam_runtime_shutdown_rt(rt);
        errno = ENOMEM;
        return -1;
    }

    if (llam_block_pool_start_min(rt) != 0) {
        int saved_errno = errno;

        llam_runtime_shutdown_rt(rt);
        errno = saved_errno;
        return -1;
    }

    if (llam_install_process_signal_handlers(rt) != 0) {
        llam_runtime_shutdown_rt(rt);
        return -1;
    }

    rc = pthread_create(&rt->ctrl_thread, NULL, llam_ctrl_worker_main, rt);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown_rt(rt);
        return -1;
    }
    rt->ctrl_thread_started = true;
    atomic_store_explicit(&rt->initialized, true, memory_order_release);
    return 0;
}

int llam_runtime_init_rt(llam_runtime_t *rt,
                         const llam_runtime_opts_t *opts,
                         size_t opts_size,
                         bool heap_allocated) {
    llam_shard_t *saved_tls_shard = g_llam_tls_shard;
    llam_task_t *saved_tls_task = g_llam_tls_task;
    llam_ctx_t *saved_tls_scheduler_ctx = g_llam_tls_scheduler_ctx;
    int rc;

    /*
     * initialized is only published after every subsystem is ready.  The
     * process-default wrapper preserves the historical non-reentrant EBUSY
     * contract, while heap-backed explicit runtimes wait for the short global
     * construction gate so two independent handles can both be created safely.
     */
    if (heap_allocated) {
        llam_runtime_lifecycle_lock();
    } else if (llam_runtime_lifecycle_trylock() != 0) {
        return -1;
    }
    rc = llam_runtime_init_ex_rt_unlocked(rt, opts, opts_size, heap_allocated);
    /*
     * The init body clears TLS while constructing a runtime so partial-init
     * cleanup can use host-side teardown paths.  If an embedder creates an
     * explicit runtime from an existing LLAM task, restore that task context
     * before returning; otherwise a successful create corrupts the caller's
     * managed execution frame.
     */
    if (saved_tls_task != NULL || saved_tls_scheduler_ctx != NULL) {
        g_llam_tls_shard = saved_tls_shard;
        g_llam_tls_task = saved_tls_task;
        g_llam_tls_scheduler_ctx = saved_tls_scheduler_ctx;
    }
    llam_runtime_lifecycle_unlock();
    return rc;
}

int llam_runtime_init_ex(const llam_runtime_opts_t *opts, size_t opts_size) {
    return llam_runtime_init_rt(llam_runtime_default_storage(), opts, opts_size, false);
}

int llam_runtime_init(const llam_runtime_opts_t *opts) {
    return llam_runtime_init_ex(opts, opts != NULL ? LLAM_RUNTIME_OPTS_V2_2_SIZE : 0U);
}
