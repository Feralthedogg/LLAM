/**
 * @file src/engine/watchdog/watchdog_autotune_config.c
 * @brief Auto-tune environment, domain, and initialization contract.
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

#include "engine/runtime_watchdog_internal.h"

static bool llam_autotune_token_eq(const char *token, size_t len, const char *expected) {
    size_t expected_len = strlen(expected);

    return len == expected_len && memcmp(token, expected, len) == 0;
}

static uint64_t llam_autotune_env_u64(const char *name, uint64_t default_value, uint64_t max_value) {
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

static unsigned llam_autotune_env_sample_mask(void) {
    uint64_t period;
    unsigned rounded = 1U;

    period = llam_autotune_env_u64("LLAM_AUTOTUNE_SAMPLE_PERIOD",
                                   LLAM_AUTOTUNE_DEFAULT_SAMPLE_PERIOD,
                                   1024U);
    if (period <= 1U) {
        return 0U;
    }
    while (rounded < period && rounded < 1024U) {
        rounded <<= 1U;
    }
    return rounded - 1U;
}

static unsigned llam_autotune_mode_from_env(void) {
    const char *env = llam_env_get("LLAM_AUTOTUNE");
    unsigned flag_value;

    if (env == NULL || env[0] == '\0') {
        return LLAM_AUTOTUNE_INTERNAL_OFF;
    }
    flag_value = llam_env_flag_value(env, 2U);
    if (strcmp(env, "observe") == 0 ||
        strcmp(env, "dry-run") == 0 ||
        strcmp(env, "dryrun") == 0) {
        return LLAM_AUTOTUNE_INTERNAL_OBSERVE;
    }
    if (strcmp(env, "on") == 0 ||
        strcmp(env, "auto") == 0 ||
        strcmp(env, "adaptive") == 0 ||
        flag_value == 1U) {
        return LLAM_AUTOTUNE_INTERNAL_ON;
    }
    if (strcmp(env, "frozen") == 0 ||
        strcmp(env, "freeze") == 0) {
        return LLAM_AUTOTUNE_INTERNAL_FROZEN;
    }
    if (flag_value == 0U ||
        strcmp(env, "off") == 0 ||
        strcmp(env, "disabled") == 0) {
        return LLAM_AUTOTUNE_INTERNAL_OFF;
    }
    return LLAM_AUTOTUNE_INTERNAL_OFF;
}

static uint64_t llam_autotune_recognized_domains(void) {
    return LLAM_AUTOTUNE_INTERNAL_DOMAIN_WORKERS |
           LLAM_AUTOTUNE_INTERNAL_DOMAIN_IDLE |
           LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF |
           LLAM_AUTOTUNE_INTERNAL_DOMAIN_PREEMPT |
           LLAM_AUTOTUNE_INTERNAL_DOMAIN_IO;
}

static uint64_t llam_autotune_observable_domains(void) {
    return LLAM_AUTOTUNE_INTERNAL_DOMAIN_IDLE |
           LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF;
}

static uint64_t llam_autotune_controllable_domains(void) {
    return LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF;
}

static uint64_t llam_autotune_default_domains(const llam_runtime_t *rt, uint64_t supported) {
    uint64_t domains = LLAM_AUTOTUNE_INTERNAL_DOMAIN_IDLE |
                       LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF;

    if (rt != NULL && rt->experimental_dynamic_shards != 0U && rt->active_shards > 1U) {
        domains |= LLAM_AUTOTUNE_INTERNAL_DOMAIN_WORKERS;
    }
    return domains & supported;
}

static uint64_t llam_autotune_domains_from_env(uint64_t default_domains, uint64_t supported_domains) {
    const char *env = llam_env_get("LLAM_AUTOTUNE_DOMAINS");
    uint64_t domains = 0U;

    if (env == NULL || env[0] == '\0') {
        return default_domains & supported_domains;
    }
    while (*env != '\0') {
        const char *start;
        size_t len;

        while (*env == ',' || llam_ascii_is_space((unsigned char)*env)) {
            ++env;
        }
        start = env;
        while (*env != '\0' && *env != ',' && !llam_ascii_is_space((unsigned char)*env)) {
            ++env;
        }
        len = (size_t)(env - start);
        if (len == 0U) {
            continue;
        }
        if (llam_autotune_token_eq(start, len, "all")) {
            domains |= supported_domains;
        } else if (llam_autotune_token_eq(start, len, "workers")) {
            domains |= LLAM_AUTOTUNE_INTERNAL_DOMAIN_WORKERS;
        } else if (llam_autotune_token_eq(start, len, "idle")) {
            domains |= LLAM_AUTOTUNE_INTERNAL_DOMAIN_IDLE;
        } else if (llam_autotune_token_eq(start, len, "handoff")) {
            domains |= LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF;
        } else if (llam_autotune_token_eq(start, len, "preempt")) {
            domains |= LLAM_AUTOTUNE_INTERNAL_DOMAIN_PREEMPT;
        } else if (llam_autotune_token_eq(start, len, "io")) {
            domains |= LLAM_AUTOTUNE_INTERNAL_DOMAIN_IO;
        }
    }
    return domains & supported_domains;
}

void llam_autotune_init(llam_runtime_t *rt) {
    llam_autotune_control_t *tune;
    uint64_t recognized;
    uint64_t observable;
    uint64_t controllable;
    uint64_t active_observation;
    uint64_t active_control;
    uint64_t supported;
    uint64_t active;
    uint64_t decision_interval_ns;
    uint64_t min_hold_ns;
    uint64_t target_wake_p99_ns;
    unsigned sample_mask;
    unsigned mode;
    unsigned phase;

    if (rt == NULL) {
        return;
    }
    tune = &rt->autotune;
    recognized = llam_autotune_recognized_domains();
    observable = llam_autotune_observable_domains();
    controllable = llam_autotune_controllable_domains();
    /* Legacy supported/active fields retain recognized/configured semantics. */
    supported = recognized;
    active = llam_autotune_domains_from_env(llam_autotune_default_domains(rt, supported), supported);
    mode = llam_autotune_mode_from_env();
    phase = mode == LLAM_AUTOTUNE_INTERNAL_OFF
                ? LLAM_AUTOTUNE_INTERNAL_PHASE_OFF
                : (mode == LLAM_AUTOTUNE_INTERNAL_FROZEN
                       ? LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD
                       : LLAM_AUTOTUNE_INTERNAL_PHASE_WARMUP);

    if (rt->deterministic != 0U && mode > LLAM_AUTOTUNE_INTERNAL_OBSERVE) {
        mode = LLAM_AUTOTUNE_INTERNAL_OBSERVE;
        phase = LLAM_AUTOTUNE_INTERNAL_PHASE_SUSPENDED;
    }
    active_observation =
        (mode == LLAM_AUTOTUNE_INTERNAL_OBSERVE ||
         mode == LLAM_AUTOTUNE_INTERNAL_ON)
            ? active & observable
            : 0U;
    active_control =
        mode == LLAM_AUTOTUNE_INTERNAL_ON ? active & controllable : 0U;
    sample_mask = llam_autotune_env_sample_mask();
    target_wake_p99_ns = llam_autotune_env_u64("LLAM_AUTOTUNE_WAKE_P99_NS", 0U, UINT64_MAX);
    if ((active_observation & LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF) != 0U) {
        if (rt->direct_handoff_stats_enabled == 0U) {
            rt->direct_handoff_stats_sample_mask = sample_mask;
        }
        rt->direct_handoff_stats_enabled = 1U;
        rt->autotune_wake_latency_enabled =
            (mode == LLAM_AUTOTUNE_INTERNAL_OBSERVE || target_wake_p99_ns != 0U) ? 1U : 0U;
        rt->autotune_wake_latency_sample_mask =
            rt->autotune_wake_latency_enabled != 0U ? sample_mask : 0U;
    } else {
        rt->autotune_wake_latency_enabled = 0U;
        rt->autotune_wake_latency_sample_mask = 0U;
    }
    decision_interval_ns = llam_autotune_env_u64("LLAM_AUTOTUNE_DECISION_INTERVAL_NS",
                                                 LLAM_AUTOTUNE_DEFAULT_DECISION_INTERVAL_NS,
                                                 10ULL * 1000ULL * 1000ULL * 1000ULL);
    if (decision_interval_ns == 0U) {
        decision_interval_ns = LLAM_AUTOTUNE_DEFAULT_DECISION_INTERVAL_NS;
    }
    min_hold_ns = llam_autotune_env_u64("LLAM_AUTOTUNE_MIN_HOLD_NS",
                                        LLAM_AUTOTUNE_DEFAULT_MIN_HOLD_NS,
                                        60ULL * 1000ULL * 1000ULL * 1000ULL);
    if (min_hold_ns == 0U) {
        min_hold_ns = LLAM_AUTOTUNE_DEFAULT_MIN_HOLD_NS;
    }

    atomic_init(&tune->mode, mode);
    atomic_init(&tune->phase, phase);
    atomic_init(&tune->recognized_domains, recognized);
    atomic_init(&tune->observable_domains, observable);
    atomic_init(&tune->controllable_domains, controllable);
    atomic_init(&tune->active_observation_domains, active_observation);
    atomic_init(&tune->active_control_domains, active_control);
    atomic_init(&tune->supported_domains, supported);
    atomic_init(&tune->active_domains, active);
    atomic_init(&tune->suspended_domains,
                phase == LLAM_AUTOTUNE_INTERNAL_PHASE_SUSPENDED ? active : 0U);
    atomic_init(&tune->policy_epoch, 0U);
    atomic_init(&tune->decisions, 0U);
    atomic_init(&tune->commits, 0U);
    atomic_init(&tune->rollbacks, 0U);
    atomic_init(&tune->guardrail_trips, 0U);
    atomic_init(&tune->decision_interval_ns, decision_interval_ns);
    atomic_init(&tune->min_hold_ns, min_hold_ns);
    atomic_init(&tune->target_wake_p99_ns, target_wake_p99_ns);
    atomic_init(&tune->next_decision_ns, 0U);
    atomic_init(&tune->last_decision_ns, 0U);
    atomic_init(&tune->last_change_ns, 0U);
    atomic_init(&tune->last_reason_mask, LLAM_AUTOTUNE_INTERNAL_REASON_NONE);
    atomic_init(&tune->target_online_workers, llam_runtime_online_shards(rt));
    atomic_init(&tune->sampled_yield_handoff_attempts, 0U);
    atomic_init(&tune->sampled_yield_handoff_hits, 0U);
    atomic_init(&tune->sampled_yield_handoff_fail_policy, 0U);
    atomic_init(&tune->sampled_yield_handoff_fail_no_work, 0U);
    atomic_init(&tune->sampled_yield_handoff_fail_push, 0U);
    atomic_init(&tune->sampled_wake_handoff_attempts, 0U);
    atomic_init(&tune->sampled_wake_handoff_hits, 0U);
    atomic_init(&tune->sampled_wake_handoff_fail_policy, 0U);
    atomic_init(&tune->sampled_wake_handoff_fail_race, 0U);
    atomic_init(&tune->sampled_wake_latency_samples, 0U);
    atomic_init(&tune->sampled_wake_latency_p50_ns, 0U);
    atomic_init(&tune->sampled_wake_latency_p99_ns, 0U);
    atomic_init(&tune->sampled_idle_spin_hits, 0U);
    atomic_init(&tune->sampled_idle_spin_fallbacks, 0U);
    atomic_init(&tune->sampled_idle_spin_ns, 0U);
    atomic_init(&tune->sampled_queue_overflows, 0U);
    tune->previous_yield_handoff_attempts = 0U;
    tune->previous_yield_handoff_hits = 0U;
    tune->previous_yield_handoff_fail_policy = 0U;
    tune->previous_yield_handoff_fail_budget = 0U;
    tune->previous_yield_handoff_fail_no_work = 0U;
    tune->previous_yield_handoff_fail_push = 0U;
    tune->previous_wake_handoff_attempts = 0U;
    tune->previous_wake_handoff_hits = 0U;
    tune->previous_wake_handoff_fail_policy = 0U;
    tune->previous_wake_handoff_fail_budget = 0U;
    tune->previous_wake_handoff_fail_race = 0U;
    tune->previous_wake_latency_samples = 0U;
    for (unsigned bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
        tune->previous_wake_latency_buckets[bucket] = 0U;
    }
    tune->previous_idle_spin_hits = 0U;
    tune->previous_idle_spin_fallbacks = 0U;
    tune->previous_idle_spin_ns = 0U;
    tune->previous_queue_overflows = 0U;
    tune->handoff_probe_baseline_hit_ppm = 0U;
    tune->handoff_probe_attempts = 0U;
    tune->handoff_probe_hits = 0U;
    tune->handoff_probe_previous_budget = 0U;
    tune->handoff_probe_budget = 0U;
    tune->has_previous_sample = false;
}
