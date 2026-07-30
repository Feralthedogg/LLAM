// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Feralthedogg

static int test_optional_shard_storage_contract(void) {
    char *saved_trace = test_dup_env_value("LLAM_TRACE_EVENTS");
    llam_runtime_opts_t opts;
    int rc = 1;

    memset(&opts, 0, sizeof(opts));
    opts.deterministic = 1U;
    opts.worker_min = 1U;
    opts.worker_count = 1U;
    opts.worker_max = 1U;
    opts.profile = LLAM_RUNTIME_PROFILE_RELEASE_FAST;

    if (setenv("LLAM_TRACE_EVENTS", "0", 1) != 0) {
        rc = test_fail_errno("setenv for disabled optional shard storage failed");
        goto cleanup_env;
    }
    if (llam_runtime_init(&opts) != 0) {
        rc = test_fail_errno("runtime init for disabled optional shard storage failed");
        goto cleanup_env;
    }
    if (g_llam_runtime.norm_cldeques != NULL ||
        g_llam_runtime.trace_events != NULL ||
        g_llam_runtime.shards[0].norm_cldeque != NULL ||
        g_llam_runtime.shards[0].trace_ring != NULL) {
        llam_runtime_shutdown();
        rc = test_fail("disabled optional shard storage was allocated");
        goto cleanup_env;
    }
    llam_runtime_shutdown();

    opts.experimental_flags = LLAM_RUNTIME_EXPERIMENTAL_F_LOCKFREE_NORMQ;
    if (setenv("LLAM_TRACE_EVENTS", "1", 1) != 0) {
        rc = test_fail_errno("setenv for enabled optional shard storage failed");
        goto cleanup_env;
    }
    if (llam_runtime_init(&opts) != 0) {
        rc = test_fail_errno("runtime init for enabled optional shard storage failed");
        goto cleanup_env;
    }
    if (g_llam_runtime.norm_cldeques == NULL ||
        g_llam_runtime.trace_events == NULL ||
        g_llam_runtime.shards[0].norm_cldeque !=
            &g_llam_runtime.norm_cldeques[0] ||
        g_llam_runtime.shards[0].trace_ring !=
            &g_llam_runtime.trace_events[0] ||
        atomic_load_explicit(
            &g_llam_runtime.shards[0].trace_head,
            memory_order_acquire) != 0U) {
        llam_runtime_shutdown();
        rc = test_fail("enabled optional shard storage was not wired");
        goto cleanup_env;
    }
    llam_runtime_shutdown();
    rc = 0;

cleanup_env:
    test_restore_env_value("LLAM_TRACE_EVENTS", saved_trace);
    return rc;
}
