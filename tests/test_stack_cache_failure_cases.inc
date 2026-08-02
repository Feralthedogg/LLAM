/**
 * @file tests/test_stack_cache_failure_cases.inc
 * @brief Stack-cache trim failure ownership contracts.
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

static int test_trim_release_failure_preserves_ownership(void) {
    stack_cache_fixture_t fixture;
    llam_runtime_stats_t before;
    llam_runtime_stats_t failed;
    llam_runtime_stats_t retried;
    uint64_t released = UINT64_MAX;
    int rc = 1;

    if (fixture_init(&fixture,
                     4U,
                     4U,
                     1U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    if (fixture_stats(&fixture, &before) != 0 ||
        before.stack_cache_cached_mappings != 1U ||
        before.stack_cache_cached_bytes != fixture.mapping_bytes) {
        rc = fail_msg("release-failure fixture cache was not exact");
        goto cleanup;
    }

    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_RELEASE, EIO);
    errno = 0;
    if (llam_runtime_stack_cache_trim_ex(fixture.handle,
                                         0U,
                                         &released) != -1 ||
        errno != EIO || released != 0U ||
        fixture_stats(&fixture, &failed) != 0) {
        rc = fail_msg("trim did not report the injected VM release failure");
        goto cleanup;
    }
    if (failed.stack_cache_cached_mappings !=
            before.stack_cache_cached_mappings ||
        failed.stack_cache_cached_bytes != before.stack_cache_cached_bytes ||
        failed.stack_cache_committed_bytes !=
            before.stack_cache_committed_bytes ||
        failed.stack_cache_released_bytes !=
            before.stack_cache_released_bytes) {
        rc = fail_msg("failed trim abandoned retained mapping authority");
        goto cleanup;
    }

    llam_runtime_test_reset_stack_vm_hooks();
    released = UINT64_MAX;
    if (llam_runtime_stack_cache_trim_ex(fixture.handle,
                                         0U,
                                         &released) != 0 ||
        fixture_stats(&fixture, &retried) != 0 ||
        released != fixture.mapping_bytes ||
        retried.stack_cache_cached_mappings != 0U ||
        retried.stack_cache_cached_bytes != 0U ||
        retried.stack_cache_committed_bytes != 0U ||
        retried.stack_cache_released_bytes -
                failed.stack_cache_released_bytes !=
            fixture.mapping_bytes) {
        rc = fail_msg("trim did not retry and release retained ownership");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_test_reset_stack_vm_hooks();
    fixture_destroy(&fixture);
    return rc;
}

static int test_pop_failure_preserves_release_ownership(void) {
    stack_cache_fixture_t fixture;
    llam_runtime_stats_t before;
    llam_runtime_stats_t failed;
    llam_runtime_stats_t retried;
    void *mapping = NULL;
    void *stack_base = NULL;
    size_t mapping_size = 0U;
    uint64_t released = 0U;
    int rc = 1;

    if (fixture_init(&fixture,
                     4U,
                     4U,
                     1U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    fixture.runtime->resource_plan.stack_cache_flags |=
        LLAM_RUNTIME_STACK_CACHE_F_DISCARD_ON_RETURN;
    if (fixture_add_mapping(&fixture, NULL) != 0 ||
        fixture_stats(&fixture, &before) != 0 ||
        before.stack_cache_cached_mappings != 2U) {
        rc = fail_msg("pop-failure fixture cache was not exact");
        goto cleanup;
    }

    llam_runtime_test_reset_stack_vm_hooks();
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_REACTIVATE, EIO);
    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_RELEASE, EAGAIN);
    errno = 0;
    if (llam_stack_cache_pop_mapping(fixture.runtime,
                                     NULL,
                                     fixture.stack_size,
                                     &mapping,
                                     &mapping_size,
                                     &stack_base) ||
        mapping != NULL || mapping_size != 0U || stack_base != NULL ||
        fixture_stats(&fixture, &failed) != 0) {
        rc = fail_msg("pop accepted a mapping after reactivation failure");
        goto cleanup;
    }
    if (failed.stack_cache_cached_mappings !=
            before.stack_cache_cached_mappings ||
        failed.stack_cache_cached_bytes != before.stack_cache_cached_bytes ||
        failed.stack_cache_committed_bytes !=
            before.stack_cache_committed_bytes ||
        failed.stack_cache_released_bytes !=
            before.stack_cache_released_bytes ||
        failed.stack_cache_secure_return_failures !=
            before.stack_cache_secure_return_failures + 1U) {
        rc = fail_msg("failed pop abandoned retained mapping authority");
        goto cleanup;
    }

    llam_runtime_test_reset_stack_vm_hooks();
    if (llam_runtime_stack_cache_trim_ex(fixture.handle,
                                         0U,
                                         &released) != 0 ||
        fixture_stats(&fixture, &retried) != 0 ||
        released != 2U * fixture.mapping_bytes ||
        retried.stack_cache_cached_mappings != 0U ||
        retried.stack_cache_cached_bytes != 0U ||
        retried.stack_cache_committed_bytes != 0U) {
        rc = fail_msg("trim did not retry failed-pop release ownership");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_test_reset_stack_vm_hooks();
    fixture_destroy(&fixture);
    return rc;
}

static int test_uncached_release_failure_quarantines_mapping(void) {
    stack_cache_fixture_t fixture;
    void *mapping = NULL;
    void *stack_base = NULL;
    size_t mapping_size = 0U;
    uint64_t baseline_bytes = 0U;
    uint64_t baseline_mappings = 0U;
    uint64_t failed_bytes = 0U;
    uint64_t failed_mappings = 0U;
    uint64_t retried_bytes = 0U;
    uint64_t retried_mappings = 0U;
    int rc = 1;

    llam_runtime_test_reset_stack_vm_hooks();
    llam_stack_cache_quarantine_retry();
    llam_stack_cache_quarantine_snapshot(&baseline_bytes,
                                         &baseline_mappings);
    if (fixture_init(&fixture,
                     1U,
                     1U,
                     1U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    if (llam_stack_vm_map(fixture.stack_size,
                          &mapping,
                          &mapping_size,
                          &stack_base) != 0 ||
        mapping_size != fixture.mapping_bytes) {
        rc = fail_msg("uncached-failure mapping setup was not exact");
        goto cleanup;
    }

    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_RELEASE, EIO);
    if (llam_stack_cache_return_mapping(fixture.runtime,
                                        NULL,
                                        mapping,
                                        mapping_size,
                                        stack_base,
                                        fixture.stack_size)) {
        rc = fail_msg("full cache accepted an extra stack mapping");
        goto cleanup;
    }
    mapping = NULL;
    llam_stack_cache_quarantine_snapshot(&failed_bytes,
                                         &failed_mappings);
    if (failed_mappings != baseline_mappings + 1U ||
        failed_bytes != baseline_bytes + fixture.mapping_bytes) {
        rc = fail_msg(
            "uncached release failure lost process-owned mapping authority");
        goto cleanup;
    }

    llam_runtime_test_reset_stack_vm_hooks();
    llam_stack_cache_quarantine_retry();
    llam_stack_cache_quarantine_snapshot(&retried_bytes,
                                         &retried_mappings);
    if (retried_mappings != baseline_mappings ||
        retried_bytes != baseline_bytes) {
        rc = fail_msg(
            "uncached release quarantine did not retry VM release");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_test_reset_stack_vm_hooks();
    if (mapping != NULL) {
        (void)llam_stack_vm_release(mapping, mapping_size);
    }
    fixture_destroy(&fixture);
    llam_stack_cache_quarantine_retry();
    return rc;
}

static int test_shutdown_release_failure_quarantines_mapping(void) {
    stack_cache_fixture_t fixture;
    stack_cache_fixture_t retry_fixture;
    llam_runtime_stats_t before;
    uint64_t baseline_bytes = 0U;
    uint64_t baseline_mappings = 0U;
    uint64_t failed_bytes = 0U;
    uint64_t failed_mappings = 0U;
    uint64_t retried_bytes = 0U;
    uint64_t retried_mappings = 0U;
    int rc = 1;

    memset(&retry_fixture, 0, sizeof(retry_fixture));
    llam_runtime_test_reset_stack_vm_hooks();
    llam_stack_cache_quarantine_retry();
    llam_stack_cache_quarantine_snapshot(&baseline_bytes,
                                         &baseline_mappings);
    if (fixture_init(&fixture,
                     4U,
                     4U,
                     1U,
                     UINT64_C(3600000000000)) != 0) {
        return 1;
    }
    if (fixture_stats(&fixture, &before) != 0 ||
        before.stack_cache_cached_mappings != 1U ||
        before.stack_cache_cached_bytes != fixture.mapping_bytes) {
        rc = fail_msg("shutdown-failure fixture cache was not exact");
        goto cleanup;
    }

    llam_runtime_test_set_stack_vm_error(LLAM_TEST_STACK_VM_RELEASE, EIO);
    llam_runtime_destroy(fixture.handle);
    fixture.handle = NULL;
    fixture.runtime = NULL;
    llam_stack_cache_quarantine_snapshot(&failed_bytes,
                                         &failed_mappings);
    if (failed_mappings != baseline_mappings + 1U ||
        failed_bytes != baseline_bytes + fixture.mapping_bytes) {
        rc = fail_msg(
            "shutdown release failure lost process-owned mapping authority");
        goto cleanup;
    }

    llam_runtime_test_reset_stack_vm_hooks();
    if (fixture_init(&retry_fixture,
                     4U,
                     4U,
                     1U,
                     UINT64_C(3600000000000)) != 0) {
        rc = fail_msg(
            "later runtime initialization could not retry quarantine");
        goto cleanup;
    }
    llam_stack_cache_quarantine_snapshot(&retried_bytes,
                                         &retried_mappings);
    if (retried_mappings != baseline_mappings ||
        retried_bytes != baseline_bytes) {
        rc = fail_msg(
            "process-owned shutdown quarantine did not retry VM release");
        goto cleanup;
    }
    rc = 0;

cleanup:
    llam_runtime_test_reset_stack_vm_hooks();
    fixture_destroy(&fixture);
    fixture_destroy(&retry_fixture);
    llam_stack_cache_quarantine_retry();
    return rc;
}
