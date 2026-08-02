// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file tests/test_runtime_stack_cache_authority.inc
 * @brief Runtime-wide retained-stack byte-authority tests.
 */

typedef struct stack_cache_test_snapshot {
    uint64_t cached_bytes;
    uint64_t committed_bytes;
    uint64_t mappings;
    unsigned class_mask;
    bool invalid;
} stack_cache_test_snapshot_t;

static unsigned stack_cache_test_snapshot_list(
    llam_stack_cache_entry_t *entry,
    llam_runtime_t *runtime,
    stack_cache_test_snapshot_t *snapshot) {
    unsigned count = 0U;

    while (entry != NULL) {
        size_t expected_stack_size;

        if (entry->stack_class > (uint32_t)LLAM_STACK_CLASS_HUGE) {
            snapshot->invalid = true;
            break;
        }
        expected_stack_size =
            llam_stack_bytes((llam_stack_class_t)entry->stack_class);
        if (entry->owner_runtime != runtime ||
            entry->mapping == NULL ||
            entry->mapping_size !=
                expected_stack_size + (size_t)llam_page_size() ||
            entry->stack_base !=
                (unsigned char *)entry->mapping + llam_page_size() ||
            entry->stack_size != expected_stack_size ||
            entry->last_return_ns == 0U ||
            (entry->state != LLAM_STACK_CACHE_ENTRY_READY &&
             entry->state != LLAM_STACK_CACHE_ENTRY_DISCARDED) ||
            (entry->state == LLAM_STACK_CACHE_ENTRY_READY &&
             entry->committed_bytes != entry->stack_size) ||
            (entry->state == LLAM_STACK_CACHE_ENTRY_DISCARDED &&
             entry->committed_bytes != 0U)) {
            snapshot->invalid = true;
            break;
        }
        snapshot->cached_bytes += entry->mapping_size;
        snapshot->committed_bytes += entry->committed_bytes;
        snapshot->mappings += 1U;
        snapshot->class_mask |= 1U << entry->stack_class;
        count += 1U;
        entry = entry->next;
    }
    return count;
}

static stack_cache_test_snapshot_t stack_cache_test_snapshot_runtime(
    llam_runtime_t *runtime) {
    stack_cache_test_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    for (unsigned shard_id = 0U;
         shard_id < runtime->active_shards;
         ++shard_id) {
        llam_shard_t *shard = &runtime->shards[shard_id];
        unsigned default_count;
        unsigned large_count;
        unsigned huge_count;

        pthread_mutex_lock(&shard->stack_cache_lock);
        default_count = stack_cache_test_snapshot_list(
            shard->stack_cache_default, runtime, &snapshot);
        large_count = stack_cache_test_snapshot_list(
            shard->stack_cache_large, runtime, &snapshot);
        huge_count = stack_cache_test_snapshot_list(
            shard->stack_cache_huge, runtime, &snapshot);
        if (default_count != shard->stack_cache_default_count ||
            large_count != shard->stack_cache_large_count ||
            huge_count != shard->stack_cache_huge_count) {
            snapshot.invalid = true;
        }
        pthread_mutex_unlock(&shard->stack_cache_lock);
    }

    pthread_mutex_lock(&runtime->stack_cache_lock);
    {
        unsigned default_count = stack_cache_test_snapshot_list(
            runtime->stack_cache_default, runtime, &snapshot);
        unsigned large_count = stack_cache_test_snapshot_list(
            runtime->stack_cache_large, runtime, &snapshot);
        unsigned huge_count = stack_cache_test_snapshot_list(
            runtime->stack_cache_huge, runtime, &snapshot);

        if (default_count != runtime->stack_cache_default_count ||
            large_count != runtime->stack_cache_large_count ||
            huge_count != runtime->stack_cache_huge_count) {
            snapshot.invalid = true;
        }
    }
    pthread_mutex_unlock(&runtime->stack_cache_lock);
    return snapshot;
}

static void stack_cache_test_noop(void *arg) {
    (void)arg;
}

static int test_stack_cache_runtime_byte_authority(void) {
    enum {
        STACK_CACHE_INITIAL_TASKS = 3,
        STACK_CACHE_BURST_TASKS = 96,
    };
    char *saved_prewarm_total =
        test_dup_env_value("LLAM_STACK_CACHE_PREWARM_TOTAL");
    llam_runtime_opts_t opts;
    llam_runtime_stats_t stats;
    llam_spawn_opts_t spawn_opts;
    llam_task_t *tasks[STACK_CACHE_BURST_TASKS];
    stack_cache_test_snapshot_t snapshot;
    unsigned *cpus = NULL;
    unsigned worker_count;
    size_t page_size = (size_t)llam_page_size();
    uint64_t default_mapping =
        (uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT) + page_size;
    uint64_t large_mapping =
        (uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_LARGE) + page_size;
    uint64_t huge_mapping =
        (uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_HUGE) + page_size;
    uint64_t budget = default_mapping + large_mapping + huge_mapping;
    bool initialized = false;
    int rc = 1;

    memset(tasks, 0, sizeof(tasks));
    if (setenv("LLAM_STACK_CACHE_PREWARM_TOTAL", "0", 1) != 0) {
        rc = test_fail_errno("disabling stack prewarm for byte authority failed");
        goto cleanup;
    }
    worker_count = llam_count_allowed_cpus(&cpus);
    free(cpus);
    cpus = NULL;
    if (worker_count > 4U) {
        worker_count = 4U;
    }
    if (worker_count == 0U) {
        rc = test_fail("stack-cache byte authority observed no CPUs");
        goto cleanup;
    }
    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("stack-cache byte authority opts init failed");
        goto cleanup;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = worker_count;
    opts.worker_count = worker_count;
    opts.worker_max = worker_count;
    opts.blocking_min = 1U;
    opts.blocking_max = 1U;
    opts.stack_cache_budget_bytes = budget;
    opts.stack_cache_high_watermark_bytes = budget;
    opts.stack_cache_low_watermark_bytes = page_size;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("stack-cache byte authority runtime init failed");
        goto cleanup;
    }
    initialized = true;
    if (llam_spawn_opts_init(&spawn_opts, sizeof(spawn_opts)) != 0) {
        rc = test_fail_errno("stack-cache byte authority spawn opts init failed");
        goto cleanup;
    }

    for (unsigned index = 0U; index < STACK_CACHE_INITIAL_TASKS; ++index) {
        spawn_opts.stack_class = index;
        tasks[index] = llam_spawn_ex(stack_cache_test_noop,
                                     NULL,
                                     &spawn_opts,
                                     sizeof(spawn_opts));
        if (tasks[index] == NULL) {
            rc = test_fail_errno("mixed-class stack-cache spawn failed");
            goto cleanup;
        }
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("mixed-class stack-cache run failed");
        goto cleanup;
    }
    for (unsigned index = 0U; index < STACK_CACHE_INITIAL_TASKS; ++index) {
        if (llam_join(tasks[index]) != 0) {
            rc = test_fail_errno("mixed-class stack-cache join failed");
            goto cleanup;
        }
        tasks[index] = NULL;
    }
    if (llam_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("mixed-class stack-cache stats failed");
        goto cleanup;
    }
    snapshot = stack_cache_test_snapshot_runtime(&g_llam_runtime);
    if (snapshot.invalid ||
        snapshot.class_mask !=
            ((1U << LLAM_STACK_CLASS_DEFAULT) |
             (1U << LLAM_STACK_CLASS_LARGE) |
             (1U << LLAM_STACK_CLASS_HUGE)) ||
        snapshot.cached_bytes != budget ||
        snapshot.committed_bytes !=
            budget - UINT64_C(3) * page_size ||
        snapshot.mappings != STACK_CACHE_INITIAL_TASKS ||
        stats.stack_cache_cached_bytes != snapshot.cached_bytes ||
        stats.stack_cache_committed_bytes != snapshot.committed_bytes ||
        stats.stack_cache_cached_mappings != snapshot.mappings) {
        rc = test_fail("mixed-class stack-cache authority was not exact");
        goto cleanup;
    }

    for (unsigned index = 0U; index < STACK_CACHE_BURST_TASKS; ++index) {
        spawn_opts.stack_class = index % 3U;
        tasks[index] = llam_spawn_ex(stack_cache_test_noop,
                                     NULL,
                                     &spawn_opts,
                                     sizeof(spawn_opts));
        if (tasks[index] == NULL) {
            rc = test_fail_errno("concurrent stack-cache burst spawn failed");
            goto cleanup;
        }
    }
    if (llam_run() != 0) {
        rc = test_fail_errno("concurrent stack-cache burst run failed");
        goto cleanup;
    }
    for (unsigned index = 0U; index < STACK_CACHE_BURST_TASKS; ++index) {
        if (llam_join(tasks[index]) != 0) {
            rc = test_fail_errno("concurrent stack-cache burst join failed");
            goto cleanup;
        }
        tasks[index] = NULL;
    }
    if (llam_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("concurrent stack-cache burst stats failed");
        goto cleanup;
    }
    snapshot = stack_cache_test_snapshot_runtime(&g_llam_runtime);
    if (snapshot.invalid || snapshot.cached_bytes > budget ||
        stats.stack_cache_cached_bytes != snapshot.cached_bytes ||
        stats.stack_cache_committed_bytes != snapshot.committed_bytes ||
        stats.stack_cache_cached_mappings != snapshot.mappings ||
        stats.stack_cache_budget_rejections == 0U ||
        stats.stack_cache_released_bytes == 0U) {
        rc = test_fail("concurrent stack-cache byte authority diverged from lists");
        goto cleanup;
    }
    llam_runtime_shutdown();
    initialized = false;

    if (llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("disabled stack-cache opts init failed");
        goto cleanup;
    }
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 1U;
    opts.blocking_max = 1U;
    opts.stack_cache_flags = LLAM_RUNTIME_STACK_CACHE_F_DISABLED;
    if (llam_runtime_init_ex(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        rc = test_fail_errno("disabled stack-cache runtime init failed");
        goto cleanup;
    }
    initialized = true;
    tasks[0] = llam_spawn(stack_cache_test_noop, NULL, NULL);
    if (tasks[0] == NULL || llam_run() != 0 || llam_join(tasks[0]) != 0 ||
        llam_runtime_collect_stats_ex(&stats, sizeof(stats)) != 0) {
        rc = test_fail_errno("disabled stack-cache lifecycle failed");
        goto cleanup;
    }
    tasks[0] = NULL;
    if (stats.stack_cache_cached_bytes != 0U ||
        stats.stack_cache_committed_bytes != 0U ||
        stats.stack_cache_cached_mappings != 0U ||
        stats.stack_cache_budget_rejections != 0U) {
        rc = test_fail("disabled stack-cache retained runtime mappings");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (initialized) {
        llam_runtime_shutdown();
    }
    free(cpus);
    test_restore_env_value("LLAM_STACK_CACHE_PREWARM_TOTAL",
                           saved_prewarm_total);
    return rc;
}
