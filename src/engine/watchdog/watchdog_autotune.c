/**
 * @file src/engine/watchdog/watchdog_autotune.c
 * @brief Low-frequency auto-tune sampling and control loop.
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

typedef struct llam_autotune_sample {
    uint64_t yield_handoff_attempts;
    uint64_t yield_handoff_hits;
    uint64_t yield_handoff_fail_policy;
    uint64_t yield_handoff_fail_budget;
    uint64_t yield_handoff_fail_no_work;
    uint64_t yield_handoff_fail_push;
    uint64_t wake_handoff_attempts;
    uint64_t wake_handoff_hits;
    uint64_t wake_handoff_fail_policy;
    uint64_t wake_handoff_fail_budget;
    uint64_t wake_handoff_fail_race;
    uint64_t wake_latency_samples;
    uint64_t wake_latency_buckets[LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS];
    uint64_t idle_spin_hits;
    uint64_t idle_spin_fallbacks;
    uint64_t idle_spin_ns;
    uint64_t queue_overflows;
} llam_autotune_sample_t;

static void llam_autotune_collect_sample(llam_runtime_t *rt, llam_autotune_sample_t *sample) {
    unsigned i;

    memset(sample, 0, sizeof(*sample));
    if (rt == NULL || rt->shards == NULL) {
        return;
    }
    for (i = 0U; i < rt->active_shards; ++i) {
        llam_metrics_t *metrics = &rt->shards[i].metrics;

        sample->yield_handoff_attempts +=
            atomic_load_explicit(&metrics->yield_direct_attempts, memory_order_relaxed);
        sample->yield_handoff_hits +=
            atomic_load_explicit(&metrics->yield_direct_fast_hits, memory_order_relaxed) +
            atomic_load_explicit(&metrics->yield_direct_locked_hits, memory_order_relaxed);
        sample->yield_handoff_fail_policy +=
            atomic_load_explicit(&metrics->yield_direct_fail_policy, memory_order_relaxed);
        sample->yield_handoff_fail_budget +=
            atomic_load_explicit(&metrics->yield_direct_fail_budget, memory_order_relaxed);
        sample->yield_handoff_fail_no_work +=
            atomic_load_explicit(&metrics->yield_direct_fail_no_work, memory_order_relaxed);
        sample->yield_handoff_fail_push +=
            atomic_load_explicit(&metrics->yield_direct_fail_push, memory_order_relaxed);
        sample->wake_handoff_attempts +=
            atomic_load_explicit(&metrics->wake_handoff_attempts, memory_order_relaxed);
        sample->wake_handoff_hits +=
            atomic_load_explicit(&metrics->wake_handoff_hits, memory_order_relaxed);
        sample->wake_handoff_fail_policy +=
            atomic_load_explicit(&metrics->wake_handoff_fail_policy, memory_order_relaxed);
        sample->wake_handoff_fail_budget +=
            atomic_load_explicit(&metrics->wake_handoff_fail_budget, memory_order_relaxed);
        sample->wake_handoff_fail_race +=
            atomic_load_explicit(&metrics->wake_handoff_fail_race, memory_order_relaxed);
        sample->wake_latency_samples +=
            atomic_load_explicit(&metrics->autotune_wake_latency_samples, memory_order_relaxed);
        for (unsigned bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
            sample->wake_latency_buckets[bucket] +=
                atomic_load_explicit(&metrics->autotune_wake_latency_buckets[bucket], memory_order_relaxed);
        }
        sample->idle_spin_hits +=
            atomic_load_explicit(&metrics->idle_spin_hits, memory_order_relaxed);
        sample->idle_spin_fallbacks +=
            atomic_load_explicit(&metrics->idle_spin_fallbacks, memory_order_relaxed);
        sample->idle_spin_ns +=
            atomic_load_explicit(&metrics->idle_spin_ns, memory_order_relaxed);
        sample->queue_overflows +=
            atomic_load_explicit(&metrics->queue_overflows, memory_order_relaxed);
    }
}

static uint64_t llam_autotune_delta_u64(uint64_t current, uint64_t previous) {
    return current >= previous ? current - previous : 0U;
}

static uint64_t llam_autotune_wake_latency_bucket_upper_ns(unsigned bucket) {
    if (bucket >= 63U) {
        return UINT64_MAX;
    }
    return UINT64_C(1) << bucket;
}

static uint64_t llam_autotune_wake_latency_percentile_ns(const llam_autotune_sample_t *delta,
                                                         unsigned percentile) {
    uint64_t rank;
    uint64_t cumulative = 0U;

    if (delta == NULL || delta->wake_latency_samples == 0U || percentile == 0U) {
        return 0U;
    }
    if (percentile > 100U) {
        percentile = 100U;
    }
    rank = (delta->wake_latency_samples * percentile + 99U) / 100U;
    if (rank == 0U) {
        rank = 1U;
    }
    for (unsigned bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
        cumulative += delta->wake_latency_buckets[bucket];
        if (cumulative >= rank) {
            return llam_autotune_wake_latency_bucket_upper_ns(bucket);
        }
    }
    return llam_autotune_wake_latency_bucket_upper_ns(LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS - 1U);
}

static uint64_t llam_autotune_reason_mask(const llam_runtime_t *rt, const llam_autotune_sample_t *delta) {
    uint64_t mask = LLAM_AUTOTUNE_INTERNAL_REASON_NONE;

    if (delta->idle_spin_hits != 0U || delta->idle_spin_fallbacks != 0U || delta->idle_spin_ns != 0U) {
        mask |= LLAM_AUTOTUNE_INTERNAL_REASON_IDLE_SPIN;
    }
    if (delta->yield_handoff_attempts != 0U || delta->wake_handoff_attempts != 0U) {
        mask |= LLAM_AUTOTUNE_INTERNAL_REASON_HANDOFF;
    }
    if (delta->queue_overflows != 0U) {
        mask |= LLAM_AUTOTUNE_INTERNAL_REASON_QUEUE_OVERFLOW;
    }
    if (delta->wake_latency_samples != 0U) {
        mask |= LLAM_AUTOTUNE_INTERNAL_REASON_WAKE_LATENCY;
    }
    if (rt != NULL && rt->experimental_dynamic_shards != 0U) {
        mask |= LLAM_AUTOTUNE_INTERNAL_REASON_DYNAMIC_WORKERS;
    }
    return mask;
}

static unsigned llam_autotune_handoff_max_budget(const llam_runtime_t *rt) {
    unsigned baseline;

    if (rt == NULL || rt->direct_handoff_burst == 0U) {
        return 0U;
    }
    baseline = rt->direct_handoff_burst;
    if (baseline > UINT_MAX / 2U) {
        return UINT_MAX;
    }
    if (baseline < 8U) {
        return 8U;
    }
    return baseline * 2U;
}

/** @brief Check whether the current handoff policy has completed its hold. */
static bool llam_autotune_handoff_hold_elapsed(
    const llam_autotune_control_t *tune,
    uint64_t now_ns) {
    uint64_t last_change_ns;
    uint64_t min_hold_ns;

    if (tune == NULL) {
        return false;
    }
    last_change_ns = atomic_load_explicit(
        &tune->last_change_ns, memory_order_acquire);
    if (last_change_ns == 0U) {
        return true;
    }
    min_hold_ns = atomic_load_explicit(
        &tune->min_hold_ns, memory_order_acquire);
    return now_ns >= last_change_ns &&
           now_ns - last_change_ns >= min_hold_ns;
}

static void llam_autotune_publish_handoff_budget(llam_runtime_t *rt,
                                                 llam_autotune_control_t *tune,
                                                 unsigned budget,
                                                 uint64_t now_ns,
                                                 bool rollback,
                                                 bool guardrail) {
    unsigned current;

    current = llam_runtime_direct_handoff_budget(rt);
    if (budget == current) {
        return;
    }
    atomic_store_explicit(&rt->direct_handoff_budget, budget, memory_order_release);
    atomic_fetch_add_explicit(&tune->policy_epoch, 1U, memory_order_acq_rel);
    atomic_store_explicit(&tune->last_change_ns, now_ns, memory_order_release);
    if (rollback) {
        atomic_fetch_add_explicit(&tune->rollbacks, 1U, memory_order_acq_rel);
    } else {
        atomic_fetch_add_explicit(&tune->commits, 1U, memory_order_acq_rel);
    }
    if (guardrail) {
        atomic_fetch_add_explicit(&tune->guardrail_trips, 1U, memory_order_acq_rel);
    }
}

static void llam_autotune_probe_handoff_budget(llam_runtime_t *rt,
                                               llam_autotune_control_t *tune,
                                               unsigned previous_budget,
                                               unsigned probe_budget,
                                               uint64_t baseline_hit_ppm,
                                               uint64_t now_ns) {
    unsigned current;

    current = llam_runtime_direct_handoff_budget(rt);
    if (probe_budget == current) {
        return;
    }
    tune->handoff_probe_baseline_hit_ppm = baseline_hit_ppm;
    tune->handoff_probe_attempts = 0U;
    tune->handoff_probe_hits = 0U;
    tune->handoff_probe_previous_budget = previous_budget;
    tune->handoff_probe_budget = probe_budget;
    atomic_store_explicit(&rt->direct_handoff_budget, probe_budget, memory_order_release);
    atomic_fetch_add_explicit(&tune->policy_epoch, 1U, memory_order_acq_rel);
    atomic_store_explicit(&tune->last_change_ns, now_ns, memory_order_release);
    atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_PROBE, memory_order_release);
}

static void llam_autotune_clear_handoff_probe(llam_autotune_control_t *tune) {
    if (tune == NULL) {
        return;
    }
    tune->handoff_probe_baseline_hit_ppm = 0U;
    tune->handoff_probe_attempts = 0U;
    tune->handoff_probe_hits = 0U;
    tune->handoff_probe_previous_budget = 0U;
    tune->handoff_probe_budget = 0U;
}

static uint64_t llam_autotune_handoff_hit_ppm(uint64_t hits, uint64_t attempts) {
    if (attempts == 0U) {
        return 0U;
    }
    if (hits > attempts) {
        hits = attempts;
    }
    return (hits * 1000000ULL) / attempts;
}

static bool llam_autotune_handoff_budget_failures_dominate(const llam_autotune_sample_t *delta,
                                                           uint64_t attempts,
                                                           uint64_t hits) {
    uint64_t budget_failures;
    uint64_t no_work_failures;
    uint64_t misses;

    if (delta == NULL) {
        return false;
    }
    budget_failures = delta->yield_handoff_fail_budget + delta->wake_handoff_fail_budget;
    if (budget_failures == 0U) {
        return false;
    }
    no_work_failures = delta->yield_handoff_fail_no_work;
    if (budget_failures < no_work_failures) {
        return false;
    }
    misses = attempts > hits ? attempts - hits : 0U;
    return budget_failures >= misses || budget_failures >= misses - budget_failures;
}

static void llam_autotune_apply_handoff(llam_runtime_t *rt,
                                        llam_autotune_control_t *tune,
                                        const llam_autotune_sample_t *delta,
                                        uint64_t now_ns) {
    uint64_t attempts;
    uint64_t hits;
    uint64_t hard_failures;
    uint64_t target_wake_p99_ns;
    uint64_t sampled_wake_p99_ns;
    uint64_t hit_ppm;
    unsigned current;
    unsigned max_budget;
    unsigned next_budget;
    unsigned phase;

    if (rt == NULL || tune == NULL || delta == NULL ||
        atomic_load_explicit(&tune->mode, memory_order_acquire) != LLAM_AUTOTUNE_INTERNAL_ON ||
        (atomic_load_explicit(&tune->active_control_domains,
                              memory_order_acquire) &
         LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF) == 0U) {
        return;
    }
    max_budget = llam_autotune_handoff_max_budget(rt);
    current = llam_runtime_direct_handoff_budget(rt);
    if (max_budget == 0U || current == 0U) {
        return;
    }

    attempts = delta->yield_handoff_attempts + delta->wake_handoff_attempts;
    hits = delta->yield_handoff_hits + delta->wake_handoff_hits;
    hit_ppm = llam_autotune_handoff_hit_ppm(hits, attempts);
    hard_failures = delta->yield_handoff_fail_push +
                    delta->wake_handoff_fail_race +
                    delta->queue_overflows;
    target_wake_p99_ns = atomic_load_explicit(&tune->target_wake_p99_ns, memory_order_acquire);
    sampled_wake_p99_ns = llam_autotune_wake_latency_percentile_ns(delta, 99U);
    if (target_wake_p99_ns != 0U &&
        delta->wake_latency_samples >= 32U &&
        sampled_wake_p99_ns > target_wake_p99_ns) {
        next_budget = current > 1U ? current / 2U : 1U;
        if (next_budget == 0U) {
            next_budget = 1U;
        }
        llam_autotune_publish_handoff_budget(rt, tune, next_budget, now_ns, true, true);
        llam_autotune_clear_handoff_probe(tune);
        atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_BACKOFF, memory_order_release);
        return;
    }
    if (hard_failures != 0U) {
        next_budget = current > 1U ? current / 2U : 1U;
        if (next_budget == 0U) {
            next_budget = 1U;
        }
        llam_autotune_publish_handoff_budget(rt, tune, next_budget, now_ns, true, true);
        llam_autotune_clear_handoff_probe(tune);
        atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_BACKOFF, memory_order_release);
        return;
    }

    phase = atomic_load_explicit(&tune->phase, memory_order_acquire);
    if (phase == LLAM_AUTOTUNE_INTERNAL_PHASE_PROBE) {
        uint64_t probe_hit_ppm;

        if (tune->handoff_probe_budget != current) {
            llam_autotune_clear_handoff_probe(tune);
            atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD, memory_order_release);
            return;
        }
        tune->handoff_probe_attempts += attempts;
        tune->handoff_probe_hits += hits;
        if (tune->handoff_probe_attempts < LLAM_AUTOTUNE_HANDOFF_PROBE_MIN_ATTEMPTS ||
            tune->handoff_probe_hits < LLAM_AUTOTUNE_HANDOFF_PROBE_MIN_HITS) {
            return;
        }
        probe_hit_ppm = llam_autotune_handoff_hit_ppm(tune->handoff_probe_hits,
                                                      tune->handoff_probe_attempts);
        if (probe_hit_ppm <
            tune->handoff_probe_baseline_hit_ppm + LLAM_AUTOTUNE_HANDOFF_PROBE_MIN_GAIN_PPM) {
            next_budget = tune->handoff_probe_previous_budget != 0U
                              ? tune->handoff_probe_previous_budget
                              : rt->direct_handoff_burst;
            llam_autotune_publish_handoff_budget(rt, tune, next_budget, now_ns, true, false);
            atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_BACKOFF, memory_order_release);
        } else {
            atomic_fetch_add_explicit(&tune->commits, 1U, memory_order_acq_rel);
            atomic_store_explicit(&tune->last_change_ns, now_ns, memory_order_release);
            atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD, memory_order_release);
        }
        llam_autotune_clear_handoff_probe(tune);
        return;
    }

    if (attempts < LLAM_AUTOTUNE_HANDOFF_PROBE_MIN_ATTEMPTS ||
        hits < LLAM_AUTOTUNE_HANDOFF_PROBE_MIN_HITS) {
        return;
    }
    if (hit_ppm >= LLAM_AUTOTUNE_HANDOFF_MIN_HIT_PPM) {
        return;
    }
    if (!llam_autotune_handoff_budget_failures_dominate(delta, attempts, hits)) {
        return;
    }
    if (current < max_budget) {
        next_budget = current < 8U ? current * 2U : current + llam_max_unsigned(1U, current / 4U);
        if (next_budget > max_budget) {
            next_budget = max_budget;
        }
        llam_autotune_probe_handoff_budget(rt, tune, current, next_budget, hit_ppm, now_ns);
    }
}

void llam_watchdog_autotune_tick(llam_runtime_t *rt, uint64_t now_ns) {
    llam_autotune_control_t *tune;
    llam_autotune_sample_t sample;
    llam_autotune_sample_t delta;
    uint64_t next_ns;
    uint64_t interval_ns;
    unsigned mode;
    unsigned phase;

    if (rt == NULL) {
        return;
    }
    tune = &rt->autotune;
    mode = atomic_load_explicit(&tune->mode, memory_order_acquire);
    if (mode == LLAM_AUTOTUNE_INTERNAL_OFF ||
        mode == LLAM_AUTOTUNE_INTERNAL_FROZEN ||
        atomic_load_explicit(&tune->active_observation_domains,
                             memory_order_acquire) == 0U) {
        return;
    }

    next_ns = atomic_load_explicit(&tune->next_decision_ns, memory_order_acquire);
    if (next_ns != 0U && now_ns < next_ns) {
        return;
    }
    if ((atomic_load_explicit(&tune->active_control_domains,
                              memory_order_acquire) &
         LLAM_AUTOTUNE_INTERNAL_DOMAIN_HANDOFF) != 0U &&
        !llam_autotune_handoff_hold_elapsed(tune, now_ns)) {
        return;
    }

    llam_autotune_collect_sample(rt, &sample);
    interval_ns = atomic_load_explicit(&tune->decision_interval_ns, memory_order_relaxed);
    if (interval_ns == 0U) {
        interval_ns = LLAM_AUTOTUNE_DEFAULT_DECISION_INTERVAL_NS;
    }
    atomic_store_explicit(&tune->next_decision_ns, now_ns + interval_ns, memory_order_release);
    atomic_store_explicit(&tune->last_decision_ns, now_ns, memory_order_release);

    if (!tune->has_previous_sample) {
        tune->previous_yield_handoff_attempts = sample.yield_handoff_attempts;
        tune->previous_yield_handoff_hits = sample.yield_handoff_hits;
        tune->previous_yield_handoff_fail_policy = sample.yield_handoff_fail_policy;
        tune->previous_yield_handoff_fail_budget = sample.yield_handoff_fail_budget;
        tune->previous_yield_handoff_fail_no_work = sample.yield_handoff_fail_no_work;
        tune->previous_yield_handoff_fail_push = sample.yield_handoff_fail_push;
        tune->previous_wake_handoff_attempts = sample.wake_handoff_attempts;
        tune->previous_wake_handoff_hits = sample.wake_handoff_hits;
        tune->previous_wake_handoff_fail_policy = sample.wake_handoff_fail_policy;
        tune->previous_wake_handoff_fail_budget = sample.wake_handoff_fail_budget;
        tune->previous_wake_handoff_fail_race = sample.wake_handoff_fail_race;
        tune->previous_wake_latency_samples = sample.wake_latency_samples;
        for (unsigned bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
            tune->previous_wake_latency_buckets[bucket] = sample.wake_latency_buckets[bucket];
        }
        tune->previous_idle_spin_hits = sample.idle_spin_hits;
        tune->previous_idle_spin_fallbacks = sample.idle_spin_fallbacks;
        tune->previous_idle_spin_ns = sample.idle_spin_ns;
        tune->previous_queue_overflows = sample.queue_overflows;
        tune->has_previous_sample = true;
        if (mode != LLAM_AUTOTUNE_INTERNAL_OFF) {
            atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD, memory_order_release);
        }
        return;
    }

    delta.yield_handoff_attempts =
        llam_autotune_delta_u64(sample.yield_handoff_attempts, tune->previous_yield_handoff_attempts);
    delta.yield_handoff_hits =
        llam_autotune_delta_u64(sample.yield_handoff_hits, tune->previous_yield_handoff_hits);
    delta.yield_handoff_fail_policy =
        llam_autotune_delta_u64(sample.yield_handoff_fail_policy, tune->previous_yield_handoff_fail_policy);
    delta.yield_handoff_fail_budget =
        llam_autotune_delta_u64(sample.yield_handoff_fail_budget, tune->previous_yield_handoff_fail_budget);
    delta.yield_handoff_fail_no_work =
        llam_autotune_delta_u64(sample.yield_handoff_fail_no_work, tune->previous_yield_handoff_fail_no_work);
    delta.yield_handoff_fail_push =
        llam_autotune_delta_u64(sample.yield_handoff_fail_push, tune->previous_yield_handoff_fail_push);
    delta.wake_handoff_attempts =
        llam_autotune_delta_u64(sample.wake_handoff_attempts, tune->previous_wake_handoff_attempts);
    delta.wake_handoff_hits =
        llam_autotune_delta_u64(sample.wake_handoff_hits, tune->previous_wake_handoff_hits);
    delta.wake_handoff_fail_policy =
        llam_autotune_delta_u64(sample.wake_handoff_fail_policy, tune->previous_wake_handoff_fail_policy);
    delta.wake_handoff_fail_budget =
        llam_autotune_delta_u64(sample.wake_handoff_fail_budget, tune->previous_wake_handoff_fail_budget);
    delta.wake_handoff_fail_race =
        llam_autotune_delta_u64(sample.wake_handoff_fail_race, tune->previous_wake_handoff_fail_race);
    delta.wake_latency_samples =
        llam_autotune_delta_u64(sample.wake_latency_samples, tune->previous_wake_latency_samples);
    for (unsigned bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
        delta.wake_latency_buckets[bucket] =
            llam_autotune_delta_u64(sample.wake_latency_buckets[bucket],
                                    tune->previous_wake_latency_buckets[bucket]);
    }
    delta.idle_spin_hits =
        llam_autotune_delta_u64(sample.idle_spin_hits, tune->previous_idle_spin_hits);
    delta.idle_spin_fallbacks =
        llam_autotune_delta_u64(sample.idle_spin_fallbacks, tune->previous_idle_spin_fallbacks);
    delta.idle_spin_ns =
        llam_autotune_delta_u64(sample.idle_spin_ns, tune->previous_idle_spin_ns);
    delta.queue_overflows =
        llam_autotune_delta_u64(sample.queue_overflows, tune->previous_queue_overflows);

    tune->previous_yield_handoff_attempts = sample.yield_handoff_attempts;
    tune->previous_yield_handoff_hits = sample.yield_handoff_hits;
    tune->previous_yield_handoff_fail_policy = sample.yield_handoff_fail_policy;
    tune->previous_yield_handoff_fail_budget = sample.yield_handoff_fail_budget;
    tune->previous_yield_handoff_fail_no_work = sample.yield_handoff_fail_no_work;
    tune->previous_yield_handoff_fail_push = sample.yield_handoff_fail_push;
    tune->previous_wake_handoff_attempts = sample.wake_handoff_attempts;
    tune->previous_wake_handoff_hits = sample.wake_handoff_hits;
    tune->previous_wake_handoff_fail_policy = sample.wake_handoff_fail_policy;
    tune->previous_wake_handoff_fail_budget = sample.wake_handoff_fail_budget;
    tune->previous_wake_handoff_fail_race = sample.wake_handoff_fail_race;
    tune->previous_wake_latency_samples = sample.wake_latency_samples;
    for (unsigned bucket = 0U; bucket < LLAM_AUTOTUNE_WAKE_LATENCY_BUCKETS; ++bucket) {
        tune->previous_wake_latency_buckets[bucket] = sample.wake_latency_buckets[bucket];
    }
    tune->previous_idle_spin_hits = sample.idle_spin_hits;
    tune->previous_idle_spin_fallbacks = sample.idle_spin_fallbacks;
    tune->previous_idle_spin_ns = sample.idle_spin_ns;
    tune->previous_queue_overflows = sample.queue_overflows;

    atomic_store_explicit(&tune->sampled_yield_handoff_attempts, delta.yield_handoff_attempts, memory_order_release);
    atomic_store_explicit(&tune->sampled_yield_handoff_hits, delta.yield_handoff_hits, memory_order_release);
    atomic_store_explicit(&tune->sampled_yield_handoff_fail_policy, delta.yield_handoff_fail_policy, memory_order_release);
    atomic_store_explicit(&tune->sampled_yield_handoff_fail_no_work, delta.yield_handoff_fail_no_work, memory_order_release);
    atomic_store_explicit(&tune->sampled_yield_handoff_fail_push, delta.yield_handoff_fail_push, memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_handoff_attempts, delta.wake_handoff_attempts, memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_handoff_hits, delta.wake_handoff_hits, memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_handoff_fail_policy, delta.wake_handoff_fail_policy, memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_handoff_fail_race, delta.wake_handoff_fail_race, memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_latency_samples, delta.wake_latency_samples, memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_latency_p50_ns,
                          llam_autotune_wake_latency_percentile_ns(&delta, 50U),
                          memory_order_release);
    atomic_store_explicit(&tune->sampled_wake_latency_p99_ns,
                          llam_autotune_wake_latency_percentile_ns(&delta, 99U),
                          memory_order_release);
    atomic_store_explicit(&tune->sampled_idle_spin_hits, delta.idle_spin_hits, memory_order_release);
    atomic_store_explicit(&tune->sampled_idle_spin_fallbacks, delta.idle_spin_fallbacks, memory_order_release);
    atomic_store_explicit(&tune->sampled_idle_spin_ns, delta.idle_spin_ns, memory_order_release);
    atomic_store_explicit(&tune->sampled_queue_overflows, delta.queue_overflows, memory_order_release);
    atomic_store_explicit(&tune->last_reason_mask, llam_autotune_reason_mask(rt, &delta), memory_order_release);
    atomic_store_explicit(&tune->target_online_workers, llam_runtime_online_shards(rt), memory_order_release);
    atomic_fetch_add_explicit(&tune->decisions, 1U, memory_order_acq_rel);

    phase = atomic_load_explicit(&tune->phase, memory_order_acquire);
    if (phase == LLAM_AUTOTUNE_INTERNAL_PHASE_WARMUP ||
        phase == LLAM_AUTOTUNE_INTERNAL_PHASE_EVALUATE ||
        phase == LLAM_AUTOTUNE_INTERNAL_PHASE_BACKOFF) {
        atomic_store_explicit(&tune->phase, LLAM_AUTOTUNE_INTERNAL_PHASE_HOLD, memory_order_release);
    }
    llam_autotune_apply_handoff(rt, tune, &delta, now_ns);
}
