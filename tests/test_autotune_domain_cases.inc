/**
 * @file tests/test_autotune_domain_cases.inc
 * @brief Auto-tune capability masks and minimum-hold regression checks.
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

static void init_autotune_domain_fixture(llam_runtime_t *rt,
                                         llam_shard_t *shard) {
    memset(rt, 0, sizeof(*rt));
    memset(shard, 0, sizeof(*shard));
    init_autotune_fake_handoff_metrics(&shard->metrics);
    rt->shards = shard;
    rt->active_shards = 1U;
    rt->direct_handoff_burst = 1U;
    atomic_init(&rt->direct_handoff_budget, 1U);
    shard->runtime = rt;
    shard->id = 0U;
}

static int test_autotune_domain_capabilities(void) {
    char *saved_autotune = test_dup_env_value("LLAM_AUTOTUNE");
    char *saved_domains = test_dup_env_value("LLAM_AUTOTUNE_DOMAINS");
    llam_runtime_t rt;
    llam_shard_t shard;
    const uint64_t recognized =
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_WORKERS |
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_IDLE |
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF |
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_PREEMPT |
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_IO;
    const uint64_t observable =
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_IDLE |
        LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF;
    int rc = 1;

    if (setenv("LLAM_AUTOTUNE", "on", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DOMAINS", "all", 1) != 0) {
        rc = test_fail_errno("setenv for autotune capability masks failed");
        goto cleanup;
    }
    init_autotune_domain_fixture(&rt, &shard);
    llam_autotune_init(&rt);
    if (atomic_load_explicit(&rt.autotune.supported_domains,
                             memory_order_acquire) != recognized ||
        atomic_load_explicit(&rt.autotune.active_domains,
                             memory_order_acquire) != recognized ||
        atomic_load_explicit(&rt.autotune.recognized_domains,
                             memory_order_acquire) != recognized ||
        atomic_load_explicit(&rt.autotune.observable_domains,
                             memory_order_acquire) != observable ||
        atomic_load_explicit(&rt.autotune.controllable_domains,
                             memory_order_acquire) !=
            LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF ||
        atomic_load_explicit(&rt.autotune.active_observation_domains,
                             memory_order_acquire) != observable ||
        atomic_load_explicit(&rt.autotune.active_control_domains,
                             memory_order_acquire) !=
            LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF) {
        rc = test_fail("autotune on-mode capability masks were misleading");
        goto cleanup;
    }

    if (setenv("LLAM_AUTOTUNE", "observe", 1) != 0) {
        rc = test_fail_errno("setenv for autotune observe masks failed");
        goto cleanup;
    }
    init_autotune_domain_fixture(&rt, &shard);
    llam_autotune_init(&rt);
    if (atomic_load_explicit(&rt.autotune.active_observation_domains,
                             memory_order_acquire) != observable ||
        atomic_load_explicit(&rt.autotune.active_control_domains,
                             memory_order_acquire) != 0U) {
        rc = test_fail("autotune observe mode exposed active control");
        goto cleanup;
    }

    if (setenv("LLAM_AUTOTUNE", "frozen", 1) != 0) {
        rc = test_fail_errno("setenv for autotune frozen masks failed");
        goto cleanup;
    }
    init_autotune_domain_fixture(&rt, &shard);
    llam_autotune_init(&rt);
    if (atomic_load_explicit(&rt.autotune.active_observation_domains,
                             memory_order_acquire) != 0U ||
        atomic_load_explicit(&rt.autotune.active_control_domains,
                             memory_order_acquire) != 0U) {
        rc = test_fail("autotune frozen mode exposed active work");
        goto cleanup;
    }
    rc = 0;

cleanup:
    test_restore_env_value("LLAM_AUTOTUNE", saved_autotune);
    test_restore_env_value("LLAM_AUTOTUNE_DOMAINS", saved_domains);
    return rc;
}

static int test_autotune_handoff_min_hold(void) {
    char *saved_autotune = test_dup_env_value("LLAM_AUTOTUNE");
    char *saved_domains = test_dup_env_value("LLAM_AUTOTUNE_DOMAINS");
    char *saved_interval =
        test_dup_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS");
    char *saved_hold = test_dup_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS");
    char *saved_wake = test_dup_env_value("LLAM_AUTOTUNE_WAKE_P99_NS");
    char *saved_period = test_dup_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD");
    llam_runtime_t rt;
    llam_shard_t shard;
    int rc = 1;

    if (setenv("LLAM_AUTOTUNE", "on", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DOMAINS", "handoff", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_DECISION_INTERVAL_NS", "1", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_MIN_HOLD_NS", "100", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_WAKE_P99_NS", "0", 1) != 0 ||
        setenv("LLAM_AUTOTUNE_SAMPLE_PERIOD", "1", 1) != 0) {
        rc = test_fail_errno("setenv for autotune hold test failed");
        goto cleanup;
    }
    init_autotune_domain_fixture(&rt, &shard);
    llam_autotune_init(&rt);
    llam_watchdog_autotune_tick(&rt, 1U);
    add_autotune_fake_yield_metrics(&shard, 64U, 32U, 32U);
    llam_watchdog_autotune_tick(&rt, 3U);
    if (llam_runtime_direct_handoff_budget(&rt) != 2U ||
        atomic_load_explicit(&rt.autotune.last_change_ns,
                             memory_order_acquire) != 3U) {
        rc = test_fail("autotune hold fixture did not open a probe");
        goto cleanup;
    }

    atomic_fetch_add_explicit(&shard.metrics.yield_direct_attempts,
                              1U,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&shard.metrics.yield_direct_fail_push,
                              1U,
                              memory_order_relaxed);
    llam_watchdog_autotune_tick(&rt, 50U);
    if (llam_runtime_direct_handoff_budget(&rt) != 2U ||
        atomic_load_explicit(&rt.autotune.guardrail_trips,
                             memory_order_acquire) != 0U) {
        rc = test_fail("autotune changed policy before min_hold_ns elapsed");
        goto cleanup;
    }

    llam_watchdog_autotune_tick(&rt, 103U);
    if (llam_runtime_direct_handoff_budget(&rt) != 1U ||
        atomic_load_explicit(&rt.autotune.rollbacks,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&rt.autotune.guardrail_trips,
                             memory_order_acquire) != 1U ||
        atomic_load_explicit(&rt.autotune.last_change_ns,
                             memory_order_acquire) != 103U) {
        rc = test_fail("autotune did not apply deferred guardrail after hold");
        goto cleanup;
    }
    rc = 0;

cleanup:
    test_restore_env_value("LLAM_AUTOTUNE", saved_autotune);
    test_restore_env_value("LLAM_AUTOTUNE_DOMAINS", saved_domains);
    test_restore_env_value("LLAM_AUTOTUNE_DECISION_INTERVAL_NS",
                           saved_interval);
    test_restore_env_value("LLAM_AUTOTUNE_MIN_HOLD_NS", saved_hold);
    test_restore_env_value("LLAM_AUTOTUNE_WAKE_P99_NS", saved_wake);
    test_restore_env_value("LLAM_AUTOTUNE_SAMPLE_PERIOD", saved_period);
    return rc;
}
