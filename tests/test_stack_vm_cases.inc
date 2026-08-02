// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

/**
 * @file tests/test_stack_vm_cases.inc
 * @brief Guarded stack VM transitions and cache failure-path tests.
 */

static int exercise_stack_vm_boundary(void) {
    const size_t stack_size = llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT);
    const size_t page_size = (size_t)llam_page_size();
    const size_t overflow_size = SIZE_MAX - (SIZE_MAX % page_size);
    void *mapping = NULL;
    void *stack_base = NULL;
    size_t mapping_size = 0U;
    uint64_t resident_bytes = UINT64_MAX;
    bool resident_valid = true;
    unsigned char *bytes;
    int rc = 1;

    errno = 0;
    if (llam_stack_vm_map(0U, &mapping, &mapping_size, &stack_base) != -1 ||
        errno != EINVAL || mapping != NULL || mapping_size != 0U ||
        stack_base != NULL) {
        return fail_msg("stack VM map accepted an empty stack");
    }
    errno = 0;
    if (llam_stack_vm_map(overflow_size,
                          &mapping,
                          &mapping_size,
                          &stack_base) != -1 ||
        errno != EOVERFLOW || mapping != NULL || mapping_size != 0U ||
        stack_base != NULL) {
        return fail_msg("stack VM map did not reject mapping-size overflow");
    }
    if (llam_stack_vm_map(stack_size,
                          &mapping,
                          &mapping_size,
                          &stack_base) != 0) {
        return fail_errno("stack VM map failed");
    }
    if (mapping == NULL || mapping == MAP_FAILED ||
        mapping_size != stack_size + page_size ||
        stack_base != (unsigned char *)mapping + page_size) {
        rc = fail_msg("stack VM map returned an invalid guarded layout");
        goto cleanup;
    }
    bytes = stack_base;
    memset(bytes, 0xA5, stack_size);

#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_SCRUB, EIO);
    errno = 0;
    if (llam_stack_vm_secure_zero(stack_base, stack_size) != -1 ||
        errno != EIO ||
        llam_runtime_test_stack_vm_calls(LLAM_TEST_STACK_VM_SCRUB) != 1U ||
        bytes[0] != 0xA5U) {
        rc = fail_msg("stack VM scrub failure hook did not fail closed");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
#endif

    if (llam_stack_vm_secure_zero(stack_base, stack_size) != 0) {
        rc = fail_errno("stack VM secure zero failed");
        goto cleanup;
    }
    for (size_t index = 0U; index < stack_size; ++index) {
        if (bytes[index] != 0U) {
            rc = fail_msg("stack VM secure zero left caller data behind");
            goto cleanup;
        }
    }
    for (size_t offset = 0U; offset < stack_size; offset += page_size) {
        bytes[offset] = (unsigned char)(offset / page_size + 1U);
    }

#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_RESIDENT_SAMPLE,
                                         EIO);
    errno = 0;
    if (llam_stack_vm_sample_resident(stack_base,
                                      stack_size,
                                      &resident_bytes,
                                      &resident_valid) != -1 ||
        errno != EIO || resident_bytes != 0U || resident_valid ||
        llam_runtime_test_stack_vm_calls(
            LLAM_TEST_STACK_VM_RESIDENT_SAMPLE) != 1U) {
        rc = fail_msg("stack VM resident-sample failure hook was ambiguous");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
#endif

    if (llam_stack_vm_sample_resident(stack_base,
                                      stack_size,
                                      &resident_bytes,
                                      &resident_valid) != 0 ||
        (resident_valid &&
         (resident_bytes > stack_size ||
          resident_bytes % page_size != 0U)) ||
        (!resident_valid && resident_bytes != 0U)) {
        rc = fail_msg("stack VM resident sample violated its validity contract");
        goto cleanup;
    }

#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_DISCARD, EIO);
    errno = 0;
    if (llam_stack_vm_discard(stack_base, stack_size) != -1 ||
        errno != EIO ||
        llam_runtime_test_stack_vm_calls(LLAM_TEST_STACK_VM_DISCARD) != 1U) {
        rc = fail_msg("stack VM discard failure hook did not propagate");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
#endif

    if (llam_stack_vm_discard(stack_base, stack_size) != 0) {
        rc = fail_errno("stack VM discard failed");
        goto cleanup;
    }

#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_REACTIVATE, EIO);
    errno = 0;
    if (llam_stack_vm_reactivate(stack_base, stack_size) != -1 ||
        errno != EIO ||
        llam_runtime_test_stack_vm_calls(
            LLAM_TEST_STACK_VM_REACTIVATE) != 1U) {
        rc = fail_msg("stack VM reactivate failure hook did not propagate");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
#endif

    if (llam_stack_vm_reactivate(stack_base, stack_size) != 0) {
        rc = fail_errno("stack VM reactivate failed");
        goto cleanup;
    }
    memset(stack_base, 0x5A, stack_size);
    rc = 0;

cleanup:
#if defined(LLAM_ENABLE_TEST_HOOKS)
    llam_runtime_test_reset_stack_vm_hooks();
#endif
    if (mapping != NULL &&
        llam_stack_vm_release(mapping, mapping_size) != 0 &&
        rc == 0) {
        rc = fail_errno("stack VM release failed");
    }
    return rc;
}

#if defined(LLAM_ENABLE_TEST_HOOKS)
static void stack_cache_failure_noop(void *arg) {
    (void)arg;
}

static int init_stack_cache_failure_runtime(unsigned stack_cache_flags,
                                            llam_runtime_t **runtime_out) {
    llam_runtime_opts_t opts;

    if (runtime_out == NULL ||
        llam_runtime_opts_init(&opts, LLAM_RUNTIME_OPTS_CURRENT_SIZE) != 0) {
        errno = EINVAL;
        return -1;
    }
    *runtime_out = NULL;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.blocking_min = 0U;
    opts.blocking_max = 1U;
    opts.stack_prewarm_total = 1U;
    opts.stack_cache_flags = stack_cache_flags;
    return llam_runtime_create(&opts,
                               LLAM_RUNTIME_OPTS_CURRENT_SIZE,
                               runtime_out);
}

static int exercise_stack_cache_return_failure(
    unsigned stack_cache_flags,
    llam_test_stack_vm_operation_t failing_operation,
    const char *failure_name) {
    const uint64_t mapping_size =
        (uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT) +
        (uint64_t)llam_page_size();
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int rc = 1;

    llam_runtime_test_reset_stack_vm_hooks();
    if (init_stack_cache_failure_runtime(stack_cache_flags, &runtime) != 0) {
        fprintf(stderr,
                "test_runtime_shutdown_internal: %s runtime init failed: "
                "errno=%d (%s)\n",
                failure_name,
                errno,
                strerror(errno));
        goto cleanup;
    }
    /*
     * Creation prewarms one known mapping. Spawn transfers it out before the
     * failure hook is armed, so only the task's secure-return path can fail.
     */
    task = llam_runtime_spawn_ex(runtime,
                                 stack_cache_failure_noop,
                                 NULL,
                                 NULL,
                                 0U);
    if (task == NULL) {
        rc = fail_errno("stack-cache return-failure spawn failed");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_error(failing_operation, EIO);
    if (llam_runtime_run_handle(runtime) != 0 ||
        llam_join(task) != 0) {
        task = NULL;
        rc = fail_errno("stack-cache return-failure workload failed");
        goto cleanup;
    }
    task = NULL;
    if (llam_runtime_collect_stats_ex_handle(runtime,
                                             &stats,
                                             sizeof(stats)) != 0) {
        rc = fail_errno("stack-cache return-failure stats failed");
        goto cleanup;
    }
    if (llam_runtime_test_stack_vm_calls(failing_operation) != 1U ||
        stats.stack_cache_cached_bytes != 0U ||
        stats.stack_cache_cached_mappings != 0U ||
        stats.stack_cache_committed_bytes != 0U ||
        stats.stack_cache_secure_return_failures != 1U ||
        stats.stack_cache_released_bytes != mapping_size ||
        stats.stack_cache_budget_rejections != 0U) {
        fprintf(stderr,
                "test_runtime_shutdown_internal: %s did not fail closed: "
                "calls=%u cached=%llu/%llu committed=%llu "
                "secure_failures=%llu released=%llu rejections=%llu\n",
                failure_name,
                llam_runtime_test_stack_vm_calls(failing_operation),
                (unsigned long long)stats.stack_cache_cached_bytes,
                (unsigned long long)stats.stack_cache_cached_mappings,
                (unsigned long long)stats.stack_cache_committed_bytes,
                (unsigned long long)
                    stats.stack_cache_secure_return_failures,
                (unsigned long long)stats.stack_cache_released_bytes,
                (unsigned long long)stats.stack_cache_budget_rejections);
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_destroy(runtime);
    return rc;
}

static int exercise_stack_cache_reactivate_failure_falls_back(void) {
    const uint64_t mapping_size =
        (uint64_t)llam_stack_bytes(LLAM_STACK_CLASS_DEFAULT) +
        (uint64_t)llam_page_size();
    llam_runtime_stats_t stats;
    llam_runtime_t *runtime = NULL;
    llam_task_t *task = NULL;
    int rc = 1;

    llam_runtime_test_reset_stack_vm_hooks();
    if (init_stack_cache_failure_runtime(
            LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN,
            &runtime) != 0) {
        rc = fail_errno("stack-cache reactivate runtime init failed");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_REACTIVATE,
                                         EIO);
    task = llam_runtime_spawn_ex(runtime,
                                 stack_cache_failure_noop,
                                 NULL,
                                 NULL,
                                 0U);
    if (task == NULL ||
        llam_runtime_test_stack_vm_calls(
            LLAM_TEST_STACK_VM_REACTIVATE) != 1U) {
        rc = fail_msg("stack-cache reactivate failure did not reach fallback");
        goto cleanup;
    }
    /*
     * A failed recommit must release the retained reservation. The fresh-map
     * fallback remains task-owned until normal completion.
     */
    if (llam_runtime_collect_stats_ex_handle(runtime,
                                             &stats,
                                             sizeof(stats)) != 0 ||
        stats.stack_cache_cached_bytes != 0U ||
        stats.stack_cache_cached_mappings != 0U ||
        stats.stack_cache_committed_bytes != 0U ||
        stats.stack_cache_secure_return_failures != 1U ||
        stats.stack_cache_released_bytes != mapping_size) {
        rc = fail_msg("stack-cache reactivate failure stranded authority");
        goto cleanup;
    }
    llam_runtime_test_reset_stack_vm_hooks();
    if (llam_runtime_run_handle(runtime) != 0 ||
        llam_join(task) != 0) {
        task = NULL;
        rc = fail_errno("stack-cache reactivate fallback workload failed");
        goto cleanup;
    }
    task = NULL;
    if (llam_runtime_collect_stats_ex_handle(runtime,
                                             &stats,
                                             sizeof(stats)) != 0 ||
        stats.stack_cache_cached_bytes != mapping_size ||
        stats.stack_cache_cached_mappings != 1U ||
        stats.stack_cache_committed_bytes != 0U ||
        stats.stack_cache_secure_return_failures != 1U ||
        stats.stack_cache_released_bytes != mapping_size) {
        rc = fail_msg("stack-cache reactivate fallback did not recache safely");
        goto cleanup;
    }
    rc = 0;

cleanup:
    if (task != NULL) {
        (void)llam_detach(task);
    }
    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_destroy(runtime);
    return rc;
}
#endif

static int exercise_stack_cache_vm_cases(void) {
    if (exercise_stack_vm_boundary() != 0 ||
        llam_test_stack_cache_run() != 0) {
        return 1;
    }
#if defined(LLAM_ENABLE_TEST_HOOKS)
    if (exercise_stack_cache_return_failure(
            LLAM_RUNTIME_STACK_CACHE_F_SECURE_SCRUB,
            LLAM_TEST_STACK_VM_SCRUB,
            "stack scrub failure") != 0 ||
        exercise_stack_cache_return_failure(
            LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN,
            LLAM_TEST_STACK_VM_DISCARD,
            "stack discard failure") != 0 ||
        exercise_stack_cache_reactivate_failure_falls_back() != 0) {
        return 1;
    }
#endif
    return 0;
}
