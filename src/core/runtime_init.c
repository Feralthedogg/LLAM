/**
 * @file src/core/runtime_init.c
 * @brief Runtime initialization, option parsing, worker/node allocation, and environment policy setup.
 *
 * @details
 * Runtime initialization builds the process-wide scheduler state in a strict
 * order:
 *  - parse caller options and environment overrides,
 *  - discover the allowed CPU set and optional SQPOLL reservation,
 *  - allocate shards, per-shard synchronization objects, stacks, and contexts,
 *  - allocate I/O nodes and start their watcher/worker threads when supported,
 *  - initialize global task/blocking/overflow infrastructure,
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
 * @brief Read a boolean-like runtime environment variable.
 *
 * Any non-empty value other than @c "0" is treated as enabled. This matches the
 * rest of the runtime's environment policy and keeps feature toggles cheap to
 * parse during initialization.
 *
 * @param name          Environment variable name.
 * @param default_value Value returned when @p name is unset or empty.
 *
 * @return 0 or 1.
 */
static unsigned llam_runtime_env_flag(const char *name, unsigned default_value) {
    const char *env = llam_env_get(name);

    if (env == NULL || env[0] == '\0') {
        return default_value;
    }
    return strcmp(env, "0") != 0 ? 1U : 0U;
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
 * @brief Find a CPU id inside a discovered CPU list.
 *
 * @param cpus       CPU id array.
 * @param count      Number of entries in @p cpus.
 * @param target_cpu CPU id to locate.
 *
 * @return Zero-based index on success, or -1 when not found.
 */
static int llam_find_cpu_index(const unsigned *cpus, unsigned count, unsigned target_cpu) {
    unsigned i;

    if (cpus == NULL) {
        return -1;
    }
    for (i = 0; i < count; ++i) {
        if (cpus[i] == target_cpu) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief Reserve a CPU for io_uring SQPOLL when the runtime is configured for it.
 *
 * SQPOLL runs a kernel submission thread. Keeping scheduler workers off that CPU
 * prevents the kernel SQ thread and a user scheduler worker from fighting over
 * the same core. If the caller does not choose a CPU explicitly, the last
 * discovered allowed CPU is reserved.
 *
 * @param rt             Runtime being initialized.
 * @param cpus_inout     In/out pointer to the allowed CPU list. Replaced with a
 *                       filtered allocation when a CPU is reserved.
 * @param observed_inout In/out count for @p cpus_inout.
 *
 * @return 0 on success.
 * @return -1 with @c errno set on invalid arguments, allocation failure, or an
 *         explicit SQPOLL CPU outside the allowed CPU set.
 */
static int llam_runtime_reserve_sqpoll_cpu(llam_runtime_t *rt, unsigned **cpus_inout, unsigned *observed_inout) {
    unsigned *cpus;
    unsigned observed;
    unsigned reserved_cpu;
    int reserved_index;
    unsigned *filtered;
    unsigned src;
    unsigned dst = 0U;

    if (rt == NULL || cpus_inout == NULL || observed_inout == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (rt->experimental_sqpoll_requested == 0U || rt->experimental_shard_rings != 0U) {
        return 0;
    }

    cpus = *cpus_inout;
    observed = *observed_inout;
    if (observed <= 1U) {
        rt->sqpoll_cpu_reserved = 0U;
        return 0;
    }

    if (rt->sqpoll_cpu >= 0) {
        reserved_cpu = (unsigned)rt->sqpoll_cpu;
        reserved_index = llam_find_cpu_index(cpus, observed, reserved_cpu);
        if (reserved_index < 0) {
            errno = EINVAL;
            return -1;
        }
    } else {
        reserved_index = (int)(observed - 1U);
        reserved_cpu = cpus[reserved_index];
        rt->sqpoll_cpu = (int)reserved_cpu;
    }

    filtered = calloc(observed - 1U, sizeof(*filtered));
    if (filtered == NULL) {
        errno = ENOMEM;
        return -1;
    }

    for (src = 0; src < observed; ++src) {
        if ((int)src == reserved_index) {
            continue;
        }
        filtered[dst++] = cpus[src];
    }

    free(cpus);
    *cpus_inout = filtered;
    *observed_inout = observed - 1U;
    rt->sqpoll_cpu_reserved = 1U;
    return 0;
}

/**
 * @brief Initialize the process-wide LLAM runtime.
 *
 * The runtime may be initialized only once at a time. This function consumes
 * caller options, applies environment overrides, allocates scheduler shards and
 * I/O nodes, starts background workers, and installs process diagnostics. The
 * final @c initialized flag is published only after all required subsystems are
 * ready.
 *
 * @param opts Optional runtime options. Passing @c NULL uses deterministic
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
int llam_runtime_init_ex(const llam_runtime_opts_t *opts, size_t opts_size) {
    llam_runtime_t *rt = &g_llam_runtime;
    llam_runtime_opts_t opts_storage;
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
    size_t opts_copy_size;
    uint64_t experimental_flags;
    int rc;

    if (opts != NULL) {
        if (opts_size == 0U) {
            errno = EINVAL;
            return -1;
        }
        memset(&opts_storage, 0, sizeof(opts_storage));
        opts_storage.deterministic = 1U;
        opts_storage.sqpoll_cpu = -1;
        opts_storage.profile = LLAM_RUNTIME_PROFILE_BALANCED;
        opts_copy_size = opts_size < sizeof(opts_storage) ? opts_size : sizeof(opts_storage);
        memcpy(&opts_storage, opts, opts_copy_size);
        opts = &opts_storage;
        if (!llam_public_runtime_profile_valid(opts->profile)) {
            errno = EINVAL;
            return -1;
        }
    }

    if (rt->initialized) {
        errno = EBUSY;
        return -1;
    }

    memset(rt, 0, sizeof(*rt));
    g_llam_tls_shard = NULL;
    g_llam_tls_task = NULL;
    g_llam_tls_scheduler_ctx = NULL;
    rt->init_thread = pthread_self();
#if defined(__linux__)
    if (pthread_getaffinity_np(rt->init_thread, sizeof(rt->init_thread_affinity), &rt->init_thread_affinity) == 0) {
        rt->init_thread_affinity_valid = true;
    }
#endif

    observed = llam_count_allowed_cpus(&cpus);
    if (observed == 0U) {
        errno = ENODEV;
        return -1;
    }
    observed_total = observed;

    experimental_flags = opts != NULL ? opts->experimental_flags : 0U;
    rt->deterministic = opts != NULL ? (opts->deterministic != 0U) : 1U;
    rt->forced_yield_every = opts != NULL ? opts->forced_yield_every : 0U;
    rt->experimental_shard_rings =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS) != 0U ? 1U : 0U;
    rt->experimental_shard_rings_multishot =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_WORKER_RINGS_MULTISHOT) != 0U ? 1U : 0U;
    rt->experimental_dynamic_shards =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_DYNAMIC_WORKERS) != 0U ? 1U : 0U;
    rt->experimental_lockfree_normq =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ) != 0U ? 1U : 0U;
    rt->experimental_huge_alloc_requested =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_HUGE_ALLOC) != 0U ? 1U : 0U;
    rt->idle_spin_ns = opts != NULL ? opts->idle_spin_ns : 0U;
    rt->idle_spin_max_iters = opts != NULL ? opts->idle_spin_max_iters : 0U;
    rt->experimental_sqpoll_requested =
        (experimental_flags & LLAM_RUNTIME_EXPERIMENTAL_F_SQPOLL) != 0U ? 1U : 0U;
    rt->sqpoll_cpu = opts != NULL ? opts->sqpoll_cpu : -1;
    rt->profile =
        llam_runtime_profile_from_env(opts != NULL ? (llam_runtime_profile_t)opts->profile : LLAM_RUNTIME_PROFILE_BALANCED);
    light_safepoint_env = llam_env_get("LLAM_DIAG_LIGHT_SAFEPOINT");
    rt->trace_events_enabled = llam_runtime_env_flag("LLAM_TRACE_EVENTS", 0U);
    rt->wake_latency_metrics_enabled = llam_runtime_env_flag("LLAM_WAKE_LATENCY_METRICS", 0U);
    rt->run_timing_enabled =
        llam_runtime_env_flag("LLAM_RUN_TIMING", 0U) ||
        llam_runtime_env_flag("LLAM_STACK_SAMPLING", 0U) ||
        (light_safepoint_env != NULL &&
         light_safepoint_env[0] != '\0' &&
         strcmp(light_safepoint_env, "0") == 0) ||
        llam_runtime_env_flag("LLAM_STRICT_SAFEPOINT", 0U);
    if (light_safepoint_env != NULL && light_safepoint_env[0] != '\0') {
        rt->cheap_safepoint = strcmp(light_safepoint_env, "0") != 0 ? 1U : 0U;
    } else {
        rt->cheap_safepoint = llam_runtime_env_flag("LLAM_STRICT_SAFEPOINT", 0U) != 0U ? 0U : 1U;
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
#if defined(__APPLE__)
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
    rt->preempt_poll_period = llam_runtime_env_u32("LLAM_PREEMPT_POLL_PERIOD", rt->preempt_poll_period, 4096U);
    rt->spawn_fanout_wake_interval =
        llam_runtime_env_u32("LLAM_SPAWN_FANOUT_WAKE_INTERVAL", 0U, 65535U);
    rt->channel_local_handoff_enabled =
        llam_runtime_env_flag("LLAM_CHANNEL_LOCAL_HANDOFF",
                            rt->profile == LLAM_RUNTIME_PROFILE_DEBUG_SAFE ? 0U : 1U);
    if (rt->experimental_shard_rings != 0U) {
        rt->experimental_sqpoll_requested = 0U;
    }
    if (rt->deterministic != 0U || observed <= 1U) {
        rt->experimental_dynamic_shards = 0U;
    }
    if (llam_runtime_reserve_sqpoll_cpu(rt, &cpus, &observed) != 0) {
        free(cpus);
        memset(rt, 0, sizeof(*rt));
        return -1;
    }
    rt->observed_shards = observed_total;
    rt->active_shards = rt->deterministic != 0U ? 1U : observed;
    initial_online_shards = rt->active_shards;
    rt->dynamic_online_floor = rt->active_shards;
    if (rt->experimental_dynamic_shards != 0U) {
        unsigned floor_basis = (rt->sqpoll_cpu_reserved != 0U && rt->observed_shards > rt->active_shards)
                                   ? rt->observed_shards
                                   : rt->active_shards;

        if (floor_basis > 8U) {
            rt->dynamic_online_floor = llam_max_unsigned(4U, floor_basis / 2U);
        } else if (floor_basis > 4U) {
            rt->dynamic_online_floor = 4U;
        } else {
            rt->dynamic_online_floor = rt->active_shards;
        }
        if (rt->dynamic_online_floor > rt->active_shards) {
            rt->dynamic_online_floor = rt->active_shards;
        }
        initial_online_shards = rt->dynamic_online_floor;
    }
    atomic_store(&rt->online_shards, initial_online_shards);
    atomic_store(&rt->online_shards_min, initial_online_shards);
    atomic_store(&rt->online_shards_max, initial_online_shards);
    atomic_store(&rt->steal_pause_active, 0U);
    atomic_store(&rt->active_io_waiters, 0U);
    rt->allowed_cpus = cpus;
    (void)llam_detect_xsave_support(rt);
    altstack_size = LLAM_ALTSTACK_BYTES;
    if (altstack_size < (size_t)SIGSTKSZ) {
        altstack_size = (size_t)SIGSTKSZ;
    }

    rt->shards = calloc(rt->active_shards, sizeof(*rt->shards));
    if (rt->shards == NULL) {
        free(cpus);
        errno = ENOMEM;
        return -1;
    }

    locality_node_ids = calloc(rt->active_shards, sizeof(*locality_node_ids));
    if (locality_node_ids == NULL) {
        free(rt->shards);
        free(cpus);
        memset(rt, 0, sizeof(*rt));
        errno = ENOMEM;
        return -1;
    }
    if (rt->experimental_shard_rings != 0U) {
        io_node_ids = calloc(rt->active_shards, sizeof(*io_node_ids));
        if (io_node_ids == NULL) {
            free(locality_node_ids);
            free(rt->shards);
            free(cpus);
            memset(rt, 0, sizeof(*rt));
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
        atomic_init(&rt->shards[i].online, i < initial_online_shards ? 1U : 0U);
        atomic_init(&rt->shards[i].inflight_io_waiters, 0U);
        atomic_init(&rt->shards[i].merge_pause_requested, 0U);
        atomic_init(&rt->shards[i].merge_pause_ack, 0U);
        atomic_init(&rt->shards[i].steal_pause_ack, 0U);
        llam_cldeque_init(&rt->shards[i].norm_cldeque);
        atomic_init(&rt->shards[i].inject_depth, 0U);
        atomic_init(&rt->shards[i].norm_depth, 0U);
        atomic_init(&rt->shards[i].timer_count, 0U);
        atomic_init(&rt->shards[i].current, NULL);
        atomic_init(&rt->shards[i].opaque_helper_active_hint, 0U);
#if defined(__linux__)
        atomic_init(&rt->shards[i].opaque_helper_opaque_wait, 0U);
#endif
        rt->shards[i].opaque_redirect_target_id = UINT_MAX;
#if defined(__linux__)
        atomic_init(&rt->shards[i].opaque_wake_seq, 0U);
#endif
        if (io_node_ids != NULL) {
            io_node_ids[i] = kernel_node_id;
        }
        rt->shards[i].event_fd = llam_wake_handle_create();
        if (rt->shards[i].event_fd < 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown();
            return -1;
        }
        rc = pthread_mutex_init(&rt->shards[i].lock, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
        rc = pthread_mutex_init(&rt->shards[i].opaque_lock, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
        rc = pthread_cond_init(&rt->shards[i].opaque_cv, NULL);
        if (rc != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
        if (llam_opaque_wake_init(&rt->shards[i]) != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown();
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
            llam_runtime_shutdown();
            return -1;
        }
        rt->shards[i].signal_stack_size = altstack_size;
        rt->shards[i].previous_sigaltstack.ss_flags = SS_DISABLE;
        llam_allocator_init(&rt->shards[i].allocator);
        if (llam_ctx_init_fp_state(&rt->shards[i].scheduler_ctx) != 0 ||
            llam_ctx_init_fp_state(&rt->shards[i].opaque_scheduler_ctx) != 0) {
            free(io_node_ids);
            free(locality_node_ids);
            llam_runtime_shutdown();
            return -1;
        }
        atomic_store_explicit(&rt->shards[i].last_safepoint_ns, llam_now_ns(), memory_order_relaxed);
        atomic_store(&rt->shards[i].last_run_started_ns, 0U);
    }
    llam_runtime_prewarm_task_allocators(rt);

    rt->active_nodes = rt->experimental_shard_rings != 0U ? rt->active_shards : locality_nodes;

    rt->kernel_node_ids = calloc(rt->active_nodes, sizeof(*rt->kernel_node_ids));
    rt->nodes = calloc(rt->active_nodes, sizeof(*rt->nodes));
    if (rt->kernel_node_ids == NULL || rt->nodes == NULL) {
        free(io_node_ids);
        free(locality_node_ids);
        llam_runtime_shutdown();
        errno = ENOMEM;
        return -1;
    }

    if (rt->experimental_shard_rings != 0U) {
        memcpy(rt->kernel_node_ids, io_node_ids, rt->active_nodes * sizeof(*io_node_ids));
    } else {
        memcpy(rt->kernel_node_ids, locality_node_ids, rt->active_nodes * sizeof(*locality_node_ids));
    }
    free(io_node_ids);
    free(locality_node_ids);

    for (i = 0; i < rt->active_nodes; ++i) {
        rt->nodes[i].runtime = rt;
        rt->nodes[i].index = i;
        rt->nodes[i].kernel_node_id = rt->kernel_node_ids[i];
        rt->nodes[i].sqpoll_enabled = false;
        rt->nodes[i].sqpoll_cpu = UINT_MAX;
        rt->nodes[i].event_fd = llam_wake_handle_create();
        if (rt->nodes[i].event_fd < 0) {
            llam_runtime_shutdown();
            return -1;
        }
        rc = pthread_mutex_init(&rt->nodes[i].submit_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
        rc = pthread_mutex_init(&rt->nodes[i].watch_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
        rc = pthread_mutex_init(&rt->nodes[i].recv_buf_lock, NULL);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
        if (llam_node_init_ring(rt, &rt->nodes[i]) == 0) {
            rt->nodes[i].ring_ready = true;
            llam_probe_ring_support(&rt->nodes[i]);
            (void)llam_node_setup_recv_buf_ring(&rt->nodes[i]);
            if (rt->experimental_shard_rings != 0U && rt->experimental_shard_rings_multishot == 0U) {
                rt->nodes[i].supports_multishot_recv = false;
                rt->nodes[i].supports_multishot_accept = false;
                rt->nodes[i].supports_multishot_poll = false;
            }
            if (pthread_create(&rt->nodes[i].thread, NULL, llam_io_worker_main, &rt->nodes[i]) == 0) {
                rt->nodes[i].thread_started = true;
            } else {
                io_uring_queue_exit(&rt->nodes[i].ring);
                rt->nodes[i].ring_ready = false;
            }
        }
    }

    rc = pthread_mutex_init(&rt->task_list_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown();
        return -1;
    }
    rt->task_list_lock_initialized = true;
    rc = pthread_mutex_init(&rt->stack_cache_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown();
        return -1;
    }
    rt->stack_cache_lock_initialized = true;
    llam_runtime_prewarm_stack_cache(rt);

    rc = pthread_mutex_init(&rt->block_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown();
        return -1;
    }
    rt->block_lock_initialized = true;
    rc = pthread_cond_init(&rt->block_cv, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown();
        return -1;
    }
    rt->block_cv_initialized = true;
    atomic_store(&rt->block_job_free, NULL);
    atomic_store(&rt->block_wake_seq, 0U);

    rc = pthread_mutex_init(&rt->overflow_lock, NULL);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown();
        return -1;
    }
    rt->overflow_lock_initialized = true;
    atomic_store(&rt->overflow_depth, 0U);

    rt->block_worker_count = rt->active_shards < 4U ? rt->active_shards : 4U;
#if !defined(__linux__)
    rt->block_worker_count = rt->active_shards;
#endif
    if (rt->block_worker_count == 0U) {
        rt->block_worker_count = 1U;
    }
    rt->block_threads = calloc(rt->block_worker_count, sizeof(*rt->block_threads));
    if (rt->block_threads == NULL) {
        llam_runtime_shutdown();
        errno = ENOMEM;
        return -1;
    }

    for (i = 0; i < rt->block_worker_count; ++i) {
        rc = pthread_create(&rt->block_threads[i], NULL, llam_block_worker_main, rt);
        if (rc != 0) {
            errno = rc;
            llam_runtime_shutdown();
            return -1;
        }
    }

    if (llam_install_process_signal_handlers(rt) != 0) {
        llam_runtime_shutdown();
        return -1;
    }

    rc = pthread_create(&rt->ctrl_thread, NULL, llam_ctrl_worker_main, rt);
    if (rc != 0) {
        errno = rc;
        llam_runtime_shutdown();
        return -1;
    }
    rt->ctrl_thread_started = true;
    rt->initialized = true;
    return 0;
}

int llam_runtime_init(const llam_runtime_opts_t *opts) {
    return llam_runtime_init_ex(opts, opts != NULL ? sizeof(*opts) : 0U);
}
