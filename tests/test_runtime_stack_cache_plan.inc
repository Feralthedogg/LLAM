/**
 * @file tests/test_runtime_stack_cache_plan.inc
 * @brief Stack-cache resource-plan and initialization diagnostics tests.
 */

static int test_stack_cache_resource_plan(const unsigned *cpus) {
    {
        llam_runtime_opts_t opts = {
            .sqpoll_cpu = -1,
            .worker_count = 1U,
            .blocking_max = 1U,
            .stack_cache_budget_bytes = UINT64_C(64) * 1024U * 1024U,
            .stack_cache_high_watermark_bytes = UINT64_C(48) * 1024U * 1024U,
            .stack_cache_low_watermark_bytes = UINT64_C(32) * 1024U * 1024U,
            .stack_cache_idle_ns = UINT64_C(123456789),
            .stack_cache_flags =
                LLAM_RUNTIME_STACK_CACHE_F_SECURE_SCRUB |
                LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN,
        };
        llam_runtime_resource_plan_input_t input = {
            .opts = &opts,
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .page_size = 4096U,
        };
        llam_runtime_resource_plan_t plan;

        if (llam_runtime_resource_plan_resolve(&input, &plan) != 0 ||
            plan.stack_cache_budget_bytes != opts.stack_cache_budget_bytes ||
            plan.stack_cache_high_watermark_bytes !=
                opts.stack_cache_high_watermark_bytes ||
            plan.stack_cache_low_watermark_bytes !=
                opts.stack_cache_low_watermark_bytes ||
            plan.stack_cache_idle_ns != opts.stack_cache_idle_ns ||
            plan.stack_cache_flags != opts.stack_cache_flags) {
            return test_fail_errno(
                "custom stack-cache byte policy did not resolve exactly");
        }
    }
    {
        llam_runtime_opts_t opts = {
            .sqpoll_cpu = -1,
            .worker_count = 1U,
            .blocking_max = 1U,
            .stack_cache_flags = LLAM_RUNTIME_STACK_CACHE_F_DISABLED,
        };
        llam_runtime_resource_plan_input_t input = {
            .opts = &opts,
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .page_size = 4096U,
        };
        llam_runtime_resource_plan_t plan;

        if (llam_runtime_resource_plan_resolve(&input, &plan) != 0 ||
            plan.stack_cache_budget_bytes != 0U ||
            plan.stack_cache_high_watermark_bytes != 0U ||
            plan.stack_cache_low_watermark_bytes != 0U ||
            plan.stack_cache_flags != LLAM_RUNTIME_STACK_CACHE_F_DISABLED) {
            return test_fail_errno(
                "disabled stack-cache policy retained a byte budget");
        }
    }
    {
        uint64_t exact_mapping_bytes =
            (uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT) +
            UINT64_C(4096);
        llam_runtime_opts_t opts = {
            .sqpoll_cpu = -1,
            .worker_count = 1U,
            .blocking_max = 1U,
            .stack_prewarm_total = 1U,
            .stack_cache_budget_bytes = exact_mapping_bytes,
            .stack_cache_high_watermark_bytes = exact_mapping_bytes,
            .stack_cache_low_watermark_bytes = exact_mapping_bytes,
        };
        llam_runtime_resource_plan_input_t input = {
            .opts = &opts,
            .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
            .allowed_cpus = cpus,
            .allowed_cpu_count = 1U,
            .affinity_supported = true,
            .sqpoll_supported = true,
            .page_size = 4096U,
        };
        llam_runtime_resource_plan_t plan;

        if (llam_runtime_resource_plan_resolve(&input, &plan) != 0 ||
            plan.stack_cache_budget_bytes != exact_mapping_bytes ||
            plan.stack_cache_high_watermark_bytes != exact_mapping_bytes ||
            plan.stack_prewarm_total != 1U) {
            return test_fail_errno(
                "exact-fit stack prewarm was rejected by its estimate");
        }
    }
    {
        typedef struct stack_cache_invalid_case {
            const char *name;
            uint64_t budget;
            uint64_t high;
            uint64_t low;
            uint64_t prewarm;
            uint32_t flags;
            int expected_errno;
        } stack_cache_invalid_case_t;
        static const stack_cache_invalid_case_t invalid_cases[] = {
            {
                "low above high",
                UINT64_C(64) * 1024U * 1024U,
                UINT64_C(32) * 1024U * 1024U,
                UINT64_C(48) * 1024U * 1024U,
                0U,
                0U,
                EINVAL,
            },
            {
                "high above budget",
                UINT64_C(64) * 1024U * 1024U,
                UINT64_C(80) * 1024U * 1024U,
                UINT64_C(32) * 1024U * 1024U,
                0U,
                0U,
                EINVAL,
            },
            {
                "unaligned budget",
                UINT64_C(64) * 1024U * 1024U + 1U,
                UINT64_C(48) * 1024U * 1024U,
                UINT64_C(32) * 1024U * 1024U,
                0U,
                0U,
                EINVAL,
            },
            {
                "unknown flags",
                UINT64_C(64) * 1024U * 1024U,
                UINT64_C(48) * 1024U * 1024U,
                UINT64_C(32) * 1024U * 1024U,
                0U,
                UINT32_C(0x80000000),
                EINVAL,
            },
            {
                "exact prewarm above budget",
                UINT64_C(64) * 1024U,
                UINT64_C(64) * 1024U,
                UINT64_C(64) * 1024U,
                1U,
                0U,
                E2BIG,
            },
            {
                "exact prewarm above high watermark",
                UINT64_C(256) * 1024U,
                UINT64_C(64) * 1024U,
                UINT64_C(64) * 1024U,
                1U,
                0U,
                E2BIG,
            },
            {
                "disabled exact prewarm",
                0U,
                0U,
                0U,
                1U,
                LLAM_RUNTIME_STACK_CACHE_F_DISABLED,
                EINVAL,
            },
        };

        for (size_t case_index = 0U;
             case_index < sizeof(invalid_cases) / sizeof(invalid_cases[0]);
             ++case_index) {
            const stack_cache_invalid_case_t *test_case =
                &invalid_cases[case_index];
            llam_runtime_opts_t opts = {
                .sqpoll_cpu = -1,
                .worker_count = 1U,
                .blocking_max = 1U,
                .stack_prewarm_total = test_case->prewarm,
                .stack_cache_budget_bytes = test_case->budget,
                .stack_cache_high_watermark_bytes = test_case->high,
                .stack_cache_low_watermark_bytes = test_case->low,
                .stack_cache_flags = test_case->flags,
            };
            llam_runtime_resource_plan_input_t input = {
                .opts = &opts,
                .opts_size = LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                .allowed_cpus = cpus,
                .allowed_cpu_count = 1U,
                .affinity_supported = true,
                .sqpoll_supported = true,
                .page_size = 4096U,
            };
            llam_runtime_resource_plan_t plan;

            errno = 0;
            if (llam_runtime_resource_plan_resolve(&input, &plan) != -1 ||
                errno != test_case->expected_errno) {
                fprintf(stderr,
                        "[test_runtime_core] stack-cache plan case '%s' "
                        "returned errno=%d, expected=%d\n",
                        test_case->name,
                        errno,
                        test_case->expected_errno);
                return 1;
            }
        }
    }
    return 0;
}

static int assert_fixed_runtime_resource_stats(unsigned worker_count) {
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    int rc = 1;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        return test_fail_errno("fixed resource opts init failed");
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = worker_count;
    opts.worker_count = worker_count;
    opts.worker_max = worker_count;
    opts.blocking_min = 1U;
    opts.blocking_max = 1U;
    if (llam_runtime_create(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE, &runtime) !=
        0) {
        return test_fail_errno("fixed resource runtime create failed");
    }
    if (llam_runtime_collect_stats_ex_handle(runtime, &stats, sizeof(stats)) !=
        0) {
        rc = test_fail_errno("fixed resource stats collection failed");
        goto cleanup;
    }
    if (stats.configured_worker_min != worker_count ||
        stats.configured_worker_count != worker_count ||
        stats.configured_worker_max != worker_count ||
        stats.active_workers != worker_count ||
        stats.online_workers != worker_count ||
        stats.selected_cpu_count != worker_count ||
        stats.configured_blocking_min != 1U ||
        stats.configured_blocking_max != 1U ||
        stats.stack_cache_budget_bytes !=
            LLAM_RUNTIME_STACK_CACHE_DEFAULT_BUDGET_BYTES ||
        stats.stack_cache_high_watermark_bytes !=
            LLAM_RUNTIME_STACK_CACHE_DEFAULT_HIGH_WATERMARK_BYTES ||
        stats.stack_cache_low_watermark_bytes !=
            LLAM_RUNTIME_STACK_CACHE_DEFAULT_LOW_WATERMARK_BYTES ||
        stats.stack_cache_idle_ns != LLAM_RUNTIME_STACK_CACHE_DEFAULT_IDLE_NS ||
        stats.stack_cache_flags != 0U ||
        stats.estimated_metadata_bytes == 0U) {
        fprintf(stderr,
                "[test_runtime_core] fixed %u-worker plan resolved as "
                "configured=%u/%u/%u active=%u online=%u cpus=%u "
                "block=%u/%u cache=%llu/%llu/%llu idle=%llu flags=%u "
                "metadata=%llu\n",
                worker_count,
                stats.configured_worker_min,
                stats.configured_worker_count,
                stats.configured_worker_max,
                stats.active_workers,
                stats.online_workers,
                stats.selected_cpu_count,
                stats.configured_blocking_min,
                stats.configured_blocking_max,
                (unsigned long long)stats.stack_cache_budget_bytes,
                (unsigned long long)stats.stack_cache_high_watermark_bytes,
                (unsigned long long)stats.stack_cache_low_watermark_bytes,
                (unsigned long long)stats.stack_cache_idle_ns,
                stats.stack_cache_flags,
                (unsigned long long)stats.estimated_metadata_bytes);
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_destroy(runtime);
    return rc;
}
