// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lccf_model_internal.h"
#include "lccf_platform.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define LCCF_RACE_GENERATIONS 10000U
#define LCCF_RACE_WORKERS 3U

static int fail(const char *message) {
    fprintf(stderr, "[test_lccf_model] %s\n", message);
    return 1;
}

static lccf_model_config_t base_config(void) {
    lccf_model_config_t config;

    memset(&config, 0, sizeof(config));
    config.workload = LCCF_MODEL_COMPLETION_IO_PIPELINE;
    config.mode = LCCF_MODEL_WAKER_QUEUE;
    config.instance_count = 37U;
    config.frame_bytes = 64U;
    config.cell_bytes = 64U;
    config.site_count = 1U;
    config.direct_budget = 8U;
    config.chain_length = 1U;
    config.remote_producers = 0U;
    config.seed = UINT64_C(0x6C6363662D706830);
    return config;
}

static int test_names_and_parsers(void) {
    static const char *const mode_names[] = {
        "waker_queue",
        "causal_cell_queue",
        "fused_causal_cell",
        "budgeted_fused_chain",
        "remote_waker_queue",
        "remote_causal_cell",
        "recompute_queue",
        "shared_fact_queue",
        "recompute_fused",
        "shared_fact_fused",
        "mixed_recompute",
        "mixed_shared_fact",
    };
    static const char *const workload_names[] = {
        "completion_io_pipeline",
        "completion_rpc_state",
        "completion_timer_cancel",
        "completion_mixed_fairness",
    };
    size_t i;

    for (i = 0U; i < sizeof(mode_names) / sizeof(mode_names[0]); ++i) {
        lccf_model_mode_t parsed = LCCF_MODEL_WAKER_QUEUE;
        const lccf_model_mode_t expected = (lccf_model_mode_t)i;

        if (lccf_model_mode_name(expected) == NULL ||
            strcmp(lccf_model_mode_name(expected), mode_names[i]) != 0 ||
            lccf_model_parse_mode(mode_names[i], &parsed) != 0 ||
            parsed != expected) {
            return fail("mode name/parse contract");
        }
    }
    for (i = 0U; i < sizeof(workload_names) / sizeof(workload_names[0]); ++i) {
        lccf_model_workload_t parsed =
            LCCF_MODEL_COMPLETION_IO_PIPELINE;
        const lccf_model_workload_t expected =
            (lccf_model_workload_t)i;

        if (lccf_model_workload_name(expected) == NULL ||
            strcmp(lccf_model_workload_name(expected),
                   workload_names[i]) != 0 ||
            lccf_model_parse_workload(workload_names[i], &parsed) != 0 ||
            parsed != expected) {
            return fail("workload name/parse contract");
        }
    }
    if (lccf_model_mode_name((lccf_model_mode_t)99) != NULL ||
        lccf_model_workload_name((lccf_model_workload_t)99) != NULL ||
        lccf_model_parse_mode(NULL, NULL) != EINVAL ||
        lccf_model_parse_mode("waker_queue", NULL) != EINVAL ||
        lccf_model_parse_mode("unknown", &(lccf_model_mode_t){0}) !=
            EINVAL ||
        lccf_model_parse_workload(NULL, NULL) != EINVAL ||
        lccf_model_parse_workload("completion_io_pipeline", NULL) !=
            EINVAL ||
        lccf_model_parse_workload(
            "unknown", &(lccf_model_workload_t){0}) != EINVAL) {
        return fail("invalid parser contract");
    }
    return 0;
}

static int test_candidate_baseline_mapping(void) {
    static const struct {
        lccf_model_mode_t candidate;
        lccf_model_mode_t baseline;
    } cases[] = {
        {LCCF_MODEL_CAUSAL_CELL_QUEUE,
         LCCF_MODEL_WAKER_QUEUE},
        {LCCF_MODEL_FUSED_CAUSAL_CELL,
         LCCF_MODEL_WAKER_QUEUE},
        {LCCF_MODEL_BUDGETED_FUSED_CHAIN,
         LCCF_MODEL_WAKER_QUEUE},
        {LCCF_MODEL_REMOTE_CAUSAL_CELL,
         LCCF_MODEL_REMOTE_WAKER_QUEUE},
        {LCCF_MODEL_SHARED_FACT_QUEUE,
         LCCF_MODEL_RECOMPUTE_QUEUE},
        {LCCF_MODEL_SHARED_FACT_FUSED,
         LCCF_MODEL_RECOMPUTE_FUSED},
        {LCCF_MODEL_MIXED_SHARED_FACT,
         LCCF_MODEL_MIXED_RECOMPUTE},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        lccf_model_mode_t baseline =
            LCCF_MODEL_REMOTE_CAUSAL_CELL;

        if (lccf_model_candidate_baseline(
                cases[index].candidate, &baseline) != 0 ||
            baseline != cases[index].baseline) {
            return fail("candidate baseline mapping");
        }
    }
    if (lccf_model_candidate_baseline(
            LCCF_MODEL_WAKER_QUEUE,
            &(lccf_model_mode_t){0}) != EINVAL ||
        lccf_model_candidate_baseline(
            LCCF_MODEL_REMOTE_WAKER_QUEUE,
            &(lccf_model_mode_t){0}) != EINVAL ||
        lccf_model_candidate_baseline(
            LCCF_MODEL_RECOMPUTE_QUEUE,
            &(lccf_model_mode_t){0}) != EINVAL ||
        lccf_model_candidate_baseline(
            LCCF_MODEL_RECOMPUTE_FUSED,
            &(lccf_model_mode_t){0}) != EINVAL ||
        lccf_model_candidate_baseline(
            LCCF_MODEL_MIXED_RECOMPUTE,
            &(lccf_model_mode_t){0}) != EINVAL ||
        lccf_model_candidate_baseline(
            (lccf_model_mode_t)99,
            &(lccf_model_mode_t){0}) != EINVAL ||
        lccf_model_candidate_baseline(
            LCCF_MODEL_FUSED_CAUSAL_CELL,
            NULL) != EINVAL) {
        return fail("invalid candidate baseline mapping");
    }
    return 0;
}

static int expect_create_error(lccf_model_config_t config, int expected) {
    lccf_model_batch_t *batch =
        (lccf_model_batch_t *)(uintptr_t)1U;
    const int rc = lccf_model_batch_create(&config, &batch);

    if (rc != expected || batch != NULL) {
        fprintf(stderr,
                "[test_lccf_model] create error expected=%d actual=%d\n",
                expected,
                rc);
        lccf_model_batch_destroy(batch);
        return 1;
    }
    return 0;
}

static int test_create_validation(void) {
    lccf_model_config_t config = base_config();
    lccf_model_batch_t *batch =
        (lccf_model_batch_t *)(uintptr_t)1U;

    if (lccf_model_batch_create(NULL, &batch) != EINVAL ||
        batch != NULL ||
        lccf_model_batch_create(&config, NULL) != EINVAL) {
        return fail("null create contract");
    }

#define EXPECT_INVALID(field, value)                                           \
    do {                                                                        \
        lccf_model_config_t invalid = config;                                   \
        invalid.field = (value);                                                \
        if (expect_create_error(invalid, EINVAL) != 0) {                        \
            return 1;                                                           \
        }                                                                       \
    } while (false)

    EXPECT_INVALID(workload, (lccf_model_workload_t)99);
    EXPECT_INVALID(mode, (lccf_model_mode_t)99);
    EXPECT_INVALID(instance_count, 0U);
    EXPECT_INVALID(frame_bytes, 65U);
    EXPECT_INVALID(cell_bytes, 65U);
    EXPECT_INVALID(site_count, 2U);
    EXPECT_INVALID(direct_budget, 0U);
    EXPECT_INVALID(direct_budget, LCCF_MODEL_MAX_DIRECT_BUDGET + 1U);
    EXPECT_INVALID(chain_length, 0U);
    EXPECT_INVALID(chain_length, LCCF_MODEL_MAX_CHAIN_LENGTH + 1U);
    EXPECT_INVALID(remote_producers, 3U);
#undef EXPECT_INVALID

    config.mode = LCCF_MODEL_REMOTE_WAKER_QUEUE;
    if (expect_create_error(config, EINVAL) != 0) {
        return 1;
    }
    config.remote_producers = 2U;
    if (lccf_model_batch_create(&config, &batch) != 0 ||
        batch == NULL) {
        return fail("remote create contract");
    }
    lccf_model_batch_destroy(batch);
    config = base_config();
    config.remote_producers = 2U;
    if (lccf_model_batch_create(&config, &batch) != 0 ||
        batch == NULL) {
        return fail("local producer-count compatibility");
    }
    lccf_model_batch_destroy(batch);
    batch = NULL;
    return 0;
}

static int metrics_equal(const lccf_model_metrics_t *lhs,
                         const lccf_model_metrics_t *rhs) {
    return memcmp(lhs, rhs, sizeof(*lhs)) == 0;
}

static int run_baseline_case(lccf_model_workload_t workload,
                             size_t frame_bytes,
                             size_t cell_bytes,
                             unsigned site_count,
                             size_t instance_count,
                             uint64_t seed) {
    lccf_model_config_t config = base_config();
    lccf_model_batch_t *lhs = NULL;
    lccf_model_batch_t *rhs = NULL;
    lccf_model_metrics_t lhs_metrics = {0};
    lccf_model_metrics_t rhs_metrics = {0};
    uint64_t initial_checksum;
    unsigned round;
    int rc = 1;

    config.workload = workload;
    config.frame_bytes = frame_bytes;
    config.cell_bytes = cell_bytes;
    config.site_count = site_count;
    config.instance_count = instance_count;
    config.seed = seed;

    if (lccf_model_batch_create(&config, &lhs) != 0 ||
        lccf_model_batch_create(&config, &rhs) != 0) {
        fail("baseline create");
        goto out;
    }
    initial_checksum = lccf_model_checksum(lhs);
    if (initial_checksum == 0U ||
        initial_checksum != lccf_model_checksum(rhs) ||
        !lccf_model_batch_equal(lhs, rhs)) {
        fail("baseline initial canonical state");
        goto out;
    }
    for (round = 0U; round < 19U; ++round) {
        if (lccf_model_run_round(lhs, &lhs_metrics) != 0 ||
            lccf_model_run_round(rhs, &rhs_metrics) != 0 ||
            !lccf_model_batch_equal(lhs, rhs) ||
            lccf_model_checksum(lhs) != lccf_model_checksum(rhs)) {
            fail("baseline deterministic round");
            goto out;
        }
    }
    if (!metrics_equal(&lhs_metrics, &rhs_metrics) ||
        lhs_metrics.completions !=
            (uint64_t)instance_count * UINT64_C(19) ||
        lhs_metrics.claims != lhs_metrics.completions ||
        lhs_metrics.resume_calls != lhs_metrics.completions ||
        lhs_metrics.queue_pushes != lhs_metrics.resume_calls ||
        lhs_metrics.queue_pops != lhs_metrics.resume_calls ||
        lhs_metrics.direct_calls != 0U ||
        lhs_metrics.stale_tickets != 0U ||
        lhs_metrics.remote_pushes != 0U ||
        lhs_metrics.hot_allocations != 0U) {
        fail("baseline metric invariants");
        goto out;
    }
    if (lccf_model_batch_reset(lhs) != 0 ||
        lccf_model_checksum(lhs) != initial_checksum ||
        !lccf_model_batch_equal(lhs, rhs)) {
        /*
         * rhs is intentionally reset only after proving that a mutated
         * batch does not compare equal to its initial state.
         */
        if (lccf_model_batch_reset(rhs) != 0 ||
            !lccf_model_batch_equal(lhs, rhs) ||
            lccf_model_checksum(rhs) != initial_checksum) {
            fail("baseline reset contract");
            goto out;
        }
    }
    rc = 0;

out:
    lccf_model_batch_destroy(rhs);
    lccf_model_batch_destroy(lhs);
    return rc;
}

static int test_baseline_matrix(void) {
    static const lccf_model_workload_t workloads[] = {
        LCCF_MODEL_COMPLETION_IO_PIPELINE,
        LCCF_MODEL_COMPLETION_RPC_STATE,
        LCCF_MODEL_COMPLETION_TIMER_CANCEL,
    };
    static const size_t frame_bytes[] = {64U, 128U, 256U};
    static const size_t cell_bytes[] = {64U, 96U, 128U};
    static const unsigned site_counts[] = {1U, 8U};
    static const size_t instance_counts[] = {37U, 257U};
    size_t wi;
    size_t fi;
    size_t ci;
    size_t si;
    size_t ii;

    for (wi = 0U; wi < sizeof(workloads) / sizeof(workloads[0]); ++wi) {
        for (fi = 0U;
             fi < sizeof(frame_bytes) / sizeof(frame_bytes[0]);
             ++fi) {
            for (ci = 0U;
                 ci < sizeof(cell_bytes) / sizeof(cell_bytes[0]);
                 ++ci) {
                for (si = 0U;
                     si < sizeof(site_counts) / sizeof(site_counts[0]);
                     ++si) {
                    for (ii = 0U;
                         ii <
                         sizeof(instance_counts) /
                             sizeof(instance_counts[0]);
                         ++ii) {
                        const uint64_t seed =
                            UINT64_C(0x9E3779B97F4A7C15) ^
                            (uint64_t)(wi * 101U + fi * 17U + ci * 5U +
                                       si * 3U + ii);

                        if (run_baseline_case(workloads[wi],
                                              frame_bytes[fi],
                                              cell_bytes[ci],
                                              site_counts[si],
                                              instance_counts[ii],
                                              seed) != 0) {
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static int test_baseline_continue_requeues(void) {
    lccf_model_config_t config = base_config();
    lccf_model_batch_t *batch = NULL;
    lccf_model_metrics_t metrics = {0};
    const uint64_t expected_callbacks =
        (uint64_t)config.instance_count * UINT64_C(18);
    int rc = 1;

    config.chain_length = 18U;
    if (lccf_model_batch_create(&config, &batch) != 0 ||
        lccf_model_run_round(batch, &metrics) != 0) {
        fail("baseline continue run");
        goto out;
    }
    if (metrics.completions != config.instance_count ||
        metrics.claims != config.instance_count ||
        metrics.resume_calls != expected_callbacks ||
        metrics.queue_pushes != expected_callbacks ||
        metrics.queue_pops != expected_callbacks ||
        metrics.hot_allocations != 0U) {
        fail("baseline continue requeue accounting");
        goto out;
    }
    rc = 0;

out:
    lccf_model_batch_destroy(batch);
    return rc;
}

static int run_causal_differential_case(
    lccf_model_workload_t workload,
    size_t frame_bytes,
    size_t cell_bytes,
    unsigned site_count,
    size_t instance_count,
    uint64_t seed) {
    lccf_model_config_t baseline_config = base_config();
    lccf_model_config_t causal_config = base_config();
    lccf_model_batch_t *baseline = NULL;
    lccf_model_batch_t *causal = NULL;
    lccf_model_metrics_t baseline_metrics = {0};
    lccf_model_metrics_t causal_metrics = {0};
    unsigned round;
    int rc = 1;

    baseline_config.workload = workload;
    baseline_config.frame_bytes = frame_bytes;
    baseline_config.cell_bytes = cell_bytes;
    baseline_config.site_count = site_count;
    baseline_config.instance_count = instance_count;
    baseline_config.seed = seed;
    causal_config = baseline_config;
    causal_config.mode = LCCF_MODEL_CAUSAL_CELL_QUEUE;

    if (lccf_model_batch_create(&baseline_config, &baseline) != 0 ||
        lccf_model_batch_create(&causal_config, &causal) != 0 ||
        !lccf_model_batch_equal(baseline, causal) ||
        lccf_model_checksum(baseline) !=
            lccf_model_checksum(causal)) {
        fail("causal differential create");
        goto out;
    }
    for (round = 0U; round < 19U; ++round) {
        size_t index;

        if (lccf_model_run_round(baseline, &baseline_metrics) != 0 ||
            lccf_model_run_round(causal, &causal_metrics) != 0 ||
            !lccf_model_batch_equal(baseline, causal) ||
            lccf_model_checksum(baseline) !=
                lccf_model_checksum(causal)) {
            fprintf(stderr,
                    "[test_lccf_model] causal differential round=%u "
                    "workload=%s frame=%zu cell=%zu sites=%u "
                    "instances=%zu seed=%016llx\n",
                    round,
                    lccf_model_workload_name(workload),
                    frame_bytes,
                    cell_bytes,
                    site_count,
                    instance_count,
                    (unsigned long long)seed);
            goto out;
        }
        for (index = 0U; index < instance_count; ++index) {
            const lccf_model_cell_hot_t *cell =
                lccf_model_cell_at(causal, index);
            const uint64_t word = atomic_load_explicit(
                &cell->state_generation,
                memory_order_acquire);
            const unsigned expected_refs =
                workload == LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
                    3U :
                    1U;

            if (lccf_model_unpack_state(word) !=
                    LCCF_MODEL_STATE_ARMED ||
                lccf_model_unpack_generation(word) !=
                    causal->instances[index].frame->generation ||
                atomic_load_explicit(&cell->backend_refs,
                                     memory_order_acquire) !=
                    expected_refs) {
                fail("causal next generation arm");
                goto out;
            }
        }
    }
    {
        const uint64_t completions =
            (uint64_t)instance_count * UINT64_C(19);
        const uint64_t expected_stale =
            workload == LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
                completions * UINT64_C(2) :
                0U;

        if (causal_metrics.completions != completions ||
            causal_metrics.claims != completions ||
            causal_metrics.stale_tickets != expected_stale ||
            causal_metrics.queue_pushes != completions ||
            causal_metrics.queue_pops != completions ||
            causal_metrics.resume_calls != completions ||
            causal_metrics.direct_calls != 0U ||
            causal_metrics.hot_allocations != 0U) {
            fail("causal queued metric invariants");
            goto out;
        }
    }
    rc = 0;

out:
    lccf_model_batch_destroy(causal);
    lccf_model_batch_destroy(baseline);
    return rc;
}

static int test_causal_differential_matrix(void) {
    static const lccf_model_workload_t workloads[] = {
        LCCF_MODEL_COMPLETION_IO_PIPELINE,
        LCCF_MODEL_COMPLETION_RPC_STATE,
        LCCF_MODEL_COMPLETION_TIMER_CANCEL,
    };
    static const size_t frame_bytes[] = {64U, 128U, 256U};
    static const size_t cell_bytes[] = {64U, 96U, 128U};
    static const unsigned site_counts[] = {1U, 8U};
    static const size_t instance_counts[] = {37U, 257U};
    static const uint64_t seeds[] = {
        UINT64_C(1),
        UINT64_C(0x0123456789ABCDEF),
        UINT64_C(0xFEDCBA9876543210),
    };
    size_t wi;
    size_t fi;
    size_t ci;
    size_t si;
    size_t ii;
    size_t seed_index;

    for (wi = 0U; wi < sizeof(workloads) / sizeof(workloads[0]); ++wi) {
        for (fi = 0U;
             fi < sizeof(frame_bytes) / sizeof(frame_bytes[0]);
             ++fi) {
            for (ci = 0U;
                 ci < sizeof(cell_bytes) / sizeof(cell_bytes[0]);
                 ++ci) {
                for (si = 0U;
                     si < sizeof(site_counts) / sizeof(site_counts[0]);
                     ++si) {
                    for (ii = 0U;
                         ii <
                         sizeof(instance_counts) /
                             sizeof(instance_counts[0]);
                         ++ii) {
                        for (seed_index = 0U;
                             seed_index <
                             sizeof(seeds) / sizeof(seeds[0]);
                             ++seed_index) {
                            if (run_causal_differential_case(
                                    workloads[wi],
                                    frame_bytes[fi],
                                    cell_bytes[ci],
                                    site_counts[si],
                                    instance_counts[ii],
                                    seeds[seed_index]) != 0) {
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static int test_causal_layout_and_stale_generation(void) {
    lccf_model_config_t config = base_config();
    lccf_model_batch_t *batch = NULL;
    lccf_model_metrics_t metrics = {0};
    lccf_model_ticket_t stale_ticket;
    lccf_model_cell_hot_t *cell;
    uint64_t expected;
    bool stale_won = true;
    int rc = 1;

    config.mode = LCCF_MODEL_CAUSAL_CELL_QUEUE;
    config.workload = LCCF_MODEL_COMPLETION_TIMER_CANCEL;
    config.instance_count = 37U;
    config.cell_bytes = 96U;
    config.site_count = 8U;
    if (lccf_model_batch_create(&config, &batch) != 0) {
        return fail("causal layout create");
    }
    if ((unsigned char *)lccf_model_cell_at(batch, 1U) -
            (unsigned char *)lccf_model_cell_at(batch, 0U) !=
        (ptrdiff_t)config.cell_bytes) {
        fail("causal cell stride");
        goto out;
    }
    stale_ticket = *lccf_model_ticket_at(batch, 0U, 0U);
    if (lccf_model_run_round(batch, &metrics) != 0) {
        fail("causal stale setup round");
        goto out;
    }
    cell = lccf_model_cell_at(batch, 0U);
    expected = lccf_model_pack_state(
        stale_ticket.generation,
        LCCF_MODEL_STATE_ARMED);
    if (atomic_compare_exchange_strong_explicit(
            &cell->state_generation,
            &expected,
            lccf_model_pack_state(stale_ticket.generation,
                                  LCCF_MODEL_STATE_CLAIMED),
            memory_order_acq_rel,
            memory_order_acquire) ||
        lccf_model_unpack_generation(expected) !=
            stale_ticket.generation + UINT64_C(1) ||
        lccf_model_unpack_state(expected) !=
            LCCF_MODEL_STATE_ARMED ||
        lccf_model_try_claim_ticket(
            &stale_ticket, &stale_won) != 0 ||
        stale_won ||
        atomic_load_explicit(
            &cell->backend_refs, memory_order_acquire) !=
            LCCF_MODEL_TICKETS_PER_INSTANCE) {
        fail("stale generation must not claim new cell");
        goto out;
    }
    rc = 0;

out:
    lccf_model_batch_destroy(batch);
    return rc;
}

static int run_fused_differential_case(
    lccf_model_workload_t workload,
    lccf_model_mode_t candidate_mode,
    size_t frame_bytes,
    size_t cell_bytes,
    unsigned site_count,
    size_t instance_count,
    unsigned chain_length,
    unsigned direct_budget,
    uint64_t seed,
    lccf_model_metrics_t *out_metrics,
    lccf_model_batch_t **out_candidate) {
    lccf_model_config_t baseline_config = base_config();
    lccf_model_config_t candidate_config;
    lccf_model_batch_t *baseline = NULL;
    lccf_model_batch_t *candidate = NULL;
    lccf_model_metrics_t baseline_metrics = {0};
    lccf_model_metrics_t candidate_metrics = {0};
    unsigned round;
    int rc = 1;

    if (out_candidate != NULL) {
        *out_candidate = NULL;
    }
    baseline_config.workload = workload;
    baseline_config.frame_bytes = frame_bytes;
    baseline_config.cell_bytes = cell_bytes;
    baseline_config.site_count = site_count;
    baseline_config.instance_count = instance_count;
    baseline_config.chain_length = chain_length;
    baseline_config.direct_budget = direct_budget;
    baseline_config.seed = seed;
    candidate_config = baseline_config;
    candidate_config.mode = candidate_mode;

    if (lccf_model_batch_create(&baseline_config, &baseline) != 0 ||
        lccf_model_batch_create(&candidate_config, &candidate) != 0) {
        fail("fused differential create");
        goto out;
    }
    for (round = 0U; round < 19U; ++round) {
        if (lccf_model_run_round(baseline, &baseline_metrics) != 0 ||
            lccf_model_run_round(candidate, &candidate_metrics) != 0 ||
            !lccf_model_batch_equal(baseline, candidate) ||
            lccf_model_checksum(baseline) !=
                lccf_model_checksum(candidate)) {
            fprintf(stderr,
                    "[test_lccf_model] fused differential round=%u "
                    "workload=%s mode=%s frame=%zu cell=%zu sites=%u "
                    "instances=%zu chain=%u budget=%u\n",
                    round,
                    lccf_model_workload_name(workload),
                    lccf_model_mode_name(candidate_mode),
                    frame_bytes,
                    cell_bytes,
                    site_count,
                    instance_count,
                    chain_length,
                    direct_budget);
            goto out;
        }
    }
    if (candidate_metrics.hot_allocations != 0U ||
        candidate_metrics.resume_calls !=
            (uint64_t)instance_count * chain_length *
                UINT64_C(19)) {
        fail("fused differential metric base");
        goto out;
    }
    if (out_metrics != NULL) {
        *out_metrics = candidate_metrics;
    }
    if (out_candidate != NULL) {
        *out_candidate = candidate;
        candidate = NULL;
    }
    rc = 0;

out:
    lccf_model_batch_destroy(candidate);
    lccf_model_batch_destroy(baseline);
    return rc;
}

static int test_fused_differential_matrix(void) {
    static const lccf_model_workload_t workloads[] = {
        LCCF_MODEL_COMPLETION_IO_PIPELINE,
        LCCF_MODEL_COMPLETION_RPC_STATE,
        LCCF_MODEL_COMPLETION_TIMER_CANCEL,
    };
    static const size_t frame_bytes[] = {64U, 128U, 256U};
    static const size_t cell_bytes[] = {64U, 96U, 128U};
    static const unsigned site_counts[] = {1U, 8U};
    size_t wi;
    size_t fi;
    size_t ci;
    size_t si;

    for (wi = 0U; wi < sizeof(workloads) / sizeof(workloads[0]); ++wi) {
        for (fi = 0U;
             fi < sizeof(frame_bytes) / sizeof(frame_bytes[0]);
             ++fi) {
            for (ci = 0U;
                 ci < sizeof(cell_bytes) / sizeof(cell_bytes[0]);
                 ++ci) {
                for (si = 0U;
                     si < sizeof(site_counts) / sizeof(site_counts[0]);
                     ++si) {
                    lccf_model_metrics_t metrics;
                    const size_t instance_count =
                        (fi + ci + si) % 2U == 0U ? 37U : 257U;

                    if (run_fused_differential_case(
                            workloads[wi],
                            LCCF_MODEL_FUSED_CAUSAL_CELL,
                            frame_bytes[fi],
                            cell_bytes[ci],
                            site_counts[si],
                            instance_count,
                            1U,
                            8U,
                            UINT64_C(0xD1B54A32D192ED03) ^
                                (uint64_t)(wi * 31U + fi * 7U +
                                           ci * 3U + si),
                            &metrics,
                            NULL) != 0) {
                        return 1;
                    }
                    if (metrics.queue_pushes != 0U ||
                        metrics.queue_pops != 0U ||
                        metrics.forced_escapes != 0U ||
                        metrics.direct_calls !=
                            metrics.resume_calls) {
                        return fail("unbounded fused accounting");
                    }
                }
            }
        }
    }
    return 0;
}

static int test_exact_causal_budgets(void) {
    static const struct {
        unsigned chain_length;
        unsigned direct_budget;
        uint64_t escapes_per_instance;
        uint64_t direct_per_instance;
    } cases[] = {
        {1U, 1U, UINT64_C(0), UINT64_C(1)},
        {9U, 8U, UINT64_C(1), UINT64_C(8)},
        {18U, 8U, UINT64_C(2), UINT64_C(16)},
    };
    size_t case_index;

    for (case_index = 0U;
         case_index < sizeof(cases) / sizeof(cases[0]);
         ++case_index) {
        lccf_model_metrics_t metrics;
        lccf_model_batch_t *candidate = NULL;
        const uint64_t instances = UINT64_C(37);
        const uint64_t rounds = UINT64_C(19);

        if (run_fused_differential_case(
                LCCF_MODEL_COMPLETION_IO_PIPELINE,
                LCCF_MODEL_BUDGETED_FUSED_CHAIN,
                128U,
                96U,
                8U,
                (size_t)instances,
                cases[case_index].chain_length,
                cases[case_index].direct_budget,
                UINT64_C(0xA0761D6478BD642F) + case_index,
                &metrics,
                &candidate) != 0) {
            return 1;
        }
        if (metrics.forced_escapes !=
                instances * rounds *
                    cases[case_index].escapes_per_instance ||
            metrics.queue_pushes != metrics.forced_escapes ||
            metrics.queue_pops != metrics.forced_escapes ||
            metrics.direct_calls !=
                instances * rounds *
                    cases[case_index].direct_per_instance ||
            candidate->maximum_callback_depth != 1U) {
            lccf_model_batch_destroy(candidate);
            return fail("exact causal budget accounting");
        }
        lccf_model_batch_destroy(candidate);
    }
    return 0;
}

static int test_mixed_fairness_accounting(void) {
    lccf_model_metrics_t metrics;
    lccf_model_batch_t *candidate = NULL;
    const uint64_t expected_samples =
        UINT64_C(257) * UINT64_C(18) * UINT64_C(19) /
        UINT64_C(32);
    int rc = 1;

    if (run_fused_differential_case(
            LCCF_MODEL_COMPLETION_MIXED_FAIRNESS,
            LCCF_MODEL_BUDGETED_FUSED_CHAIN,
            128U,
            96U,
            8U,
            257U,
            18U,
            8U,
            UINT64_C(0xE7037ED1A0B428DB),
            &metrics,
            &candidate) != 0) {
        return 1;
    }
    if (metrics.fairness_samples != expected_samples ||
        metrics.fairness_p99_ns == 0U ||
        candidate->fairness_services != metrics.fairness_samples ||
        candidate->fairness_due) {
        fail("mixed fairness service accounting");
        goto out;
    }
    rc = 0;

out:
    lccf_model_batch_destroy(candidate);
    return rc;
}

typedef struct platform_event_test_context {
    lccf_platform_event_t *pulse;
    lccf_platform_event_t *ready;
    lccf_platform_event_t *ack;
    _Atomic unsigned observed;
    _Atomic bool stop;
} platform_event_test_context_t;

static int wait_for_counter(lccf_platform_event_t *event,
                            const _Atomic unsigned *counter,
                            unsigned target) {
    while (atomic_load_explicit(counter, memory_order_acquire) <
           target) {
        const uint64_t epoch = lccf_platform_event_epoch(event);

        if (epoch == UINT64_MAX) {
            return EPROTO;
        }
        if (atomic_load_explicit(
                counter, memory_order_acquire) >= target) {
            break;
        }
        if (lccf_platform_event_wait(event, epoch) != 0) {
            return EPROTO;
        }
    }
    return 0;
}

static int platform_event_test_thread(void *opaque) {
    platform_event_test_context_t *context = opaque;
    uint64_t observed =
        lccf_platform_event_epoch(context->pulse);
    unsigned iteration;

    if (observed == UINT64_MAX ||
        lccf_platform_event_signal(context->ready) != 0) {
        return EPROTO;
    }
    for (iteration = 1U; iteration <= 10U; ++iteration) {
        uint64_t current;

        if (lccf_platform_event_wait(
                context->pulse, observed) != 0) {
            return EPROTO;
        }
        if (atomic_load_explicit(
                &context->stop, memory_order_acquire)) {
            return ECANCELED;
        }
        current = lccf_platform_event_epoch(context->pulse);
        if (current <= observed) {
            return EPROTO;
        }
        observed = current;
        atomic_store_explicit(
            &context->observed, iteration, memory_order_release);
        if (lccf_platform_event_signal(context->ack) != 0) {
            return EPROTO;
        }
    }
    return 0;
}

static int test_platform_contract(void) {
    platform_event_test_context_t context;
    lccf_platform_thread_t *thread = NULL;
    uint64_t ready_epoch;
    char description[256];
    unsigned iteration;
    int thread_result = EPROTO;
    int rc = 1;

    memset(&context, 0, sizeof(context));
    atomic_init(&context.observed, 0U);
    atomic_init(&context.stop, false);
    if (lccf_platform_event_create(&context.pulse) != 0 ||
        lccf_platform_event_create(&context.ready) != 0 ||
        lccf_platform_event_create(&context.ack) != 0) {
        fail("platform event create");
        goto out;
    }
    ready_epoch = lccf_platform_event_epoch(context.ready);
    if (ready_epoch == UINT64_MAX ||
        lccf_platform_thread_start(
            &thread, platform_event_test_thread, &context) != 0 ||
        lccf_platform_event_wait(
            context.ready, ready_epoch) != 0) {
        fail("platform thread ready");
        goto out;
    }
    for (iteration = 1U; iteration <= 10U; ++iteration) {
        if (lccf_platform_event_signal(context.pulse) != 0 ||
            wait_for_counter(
                context.ack, &context.observed, iteration) != 0) {
            fail("platform monotonic event epoch");
            goto out;
        }
    }
    if (lccf_platform_thread_join(thread, &thread_result) != 0 ||
        thread_result != 0) {
        thread = NULL;
        fail("platform thread join");
        goto out;
    }
    thread = NULL;
    if (lccf_platform_monotonic_ns() == 0U ||
        lccf_platform_process_cpu_ns() == 0U ||
        lccf_platform_describe(
            description, sizeof(description)) != 0 ||
        description[0] == '\0') {
        fail("platform clocks and description");
        goto out;
    }
    rc = 0;

out:
    if (thread != NULL) {
        atomic_store_explicit(
            &context.stop, true, memory_order_release);
        (void)lccf_platform_event_signal(context.pulse);
        (void)lccf_platform_thread_join(thread, NULL);
    }
    lccf_platform_event_destroy(context.ack);
    lccf_platform_event_destroy(context.ready);
    lccf_platform_event_destroy(context.pulse);
    return rc;
}

typedef struct ticket_race_context ticket_race_context_t;

typedef struct ticket_race_worker {
    ticket_race_context_t *race;
    unsigned index;
} ticket_race_worker_t;

struct ticket_race_context {
    lccf_model_batch_t *batch;
    lccf_platform_event_t *start;
    lccf_platform_event_t *ready;
    lccf_platform_event_t *done;
    _Atomic unsigned ready_workers;
    _Atomic unsigned completed_workers;
    _Atomic uint64_t wins;
    _Atomic uint64_t stale;
    _Atomic int error;
    _Atomic bool stop;
    ticket_race_worker_t workers[LCCF_RACE_WORKERS];
};

static void record_race_error(ticket_race_context_t *race, int error) {
    int expected = 0;

    (void)atomic_compare_exchange_strong_explicit(
        &race->error,
        &expected,
        error == 0 ? EPROTO : error,
        memory_order_acq_rel,
        memory_order_acquire);
}

static int ticket_race_thread(void *opaque) {
    ticket_race_worker_t *worker = opaque;
    ticket_race_context_t *race = worker->race;
    uint64_t observed =
        lccf_platform_event_epoch(race->start);
    unsigned generation;

    if (observed == UINT64_MAX) {
        return EPROTO;
    }
    atomic_fetch_add_explicit(
        &race->ready_workers, 1U, memory_order_acq_rel);
    if (lccf_platform_event_signal(race->ready) != 0) {
        return EPROTO;
    }
    for (generation = 0U;
         generation < LCCF_RACE_GENERATIONS;
         ++generation) {
        bool won = false;
        int claim_rc;

        if (lccf_platform_event_wait(
                race->start, observed) != 0) {
            record_race_error(race, EPROTO);
        } else {
            observed =
                lccf_platform_event_epoch(race->start);
            if (atomic_load_explicit(
                    &race->stop, memory_order_acquire)) {
                break;
            }
            claim_rc = lccf_model_try_claim_ticket(
                lccf_model_ticket_at(
                    race->batch, 0U, worker->index),
                &won);
            if (claim_rc != 0) {
                record_race_error(race, claim_rc);
            } else if (won) {
                atomic_fetch_add_explicit(
                    &race->wins, UINT64_C(1),
                    memory_order_relaxed);
            } else {
                atomic_fetch_add_explicit(
                    &race->stale, UINT64_C(1),
                    memory_order_relaxed);
            }
        }
        if (atomic_fetch_add_explicit(
                &race->completed_workers,
                1U,
                memory_order_acq_rel) +
                1U ==
            LCCF_RACE_WORKERS) {
            if (lccf_platform_event_signal(race->done) != 0) {
                record_race_error(race, EPROTO);
            }
        }
    }
    return atomic_load_explicit(
        &race->error, memory_order_acquire);
}

static int test_three_way_ticket_race(void) {
    lccf_model_config_t config = base_config();
    ticket_race_context_t race;
    lccf_platform_thread_t *threads[LCCF_RACE_WORKERS] = {0};
    lccf_model_metrics_t metrics = {0};
    unsigned generation;
    unsigned worker_index;
    unsigned started_workers = 0U;
    int rc = 1;

    memset(&race, 0, sizeof(race));
    atomic_init(&race.ready_workers, 0U);
    atomic_init(&race.completed_workers, 0U);
    atomic_init(&race.wins, UINT64_C(0));
    atomic_init(&race.stale, UINT64_C(0));
    atomic_init(&race.error, 0);
    atomic_init(&race.stop, false);
    config.mode = LCCF_MODEL_CAUSAL_CELL_QUEUE;
    config.workload = LCCF_MODEL_COMPLETION_TIMER_CANCEL;
    config.instance_count = 1U;
    config.site_count = 8U;
    if (lccf_model_batch_create(&config, &race.batch) != 0 ||
        lccf_platform_event_create(&race.start) != 0 ||
        lccf_platform_event_create(&race.ready) != 0 ||
        lccf_platform_event_create(&race.done) != 0) {
        fail("ticket race setup");
        goto out;
    }
    for (worker_index = 0U;
         worker_index < LCCF_RACE_WORKERS;
         ++worker_index) {
        race.workers[worker_index].race = &race;
        race.workers[worker_index].index = worker_index;
        if (lccf_platform_thread_start(
                &threads[worker_index],
                ticket_race_thread,
                &race.workers[worker_index]) != 0) {
            fail("ticket race thread start");
            goto out;
        }
        started_workers += 1U;
    }
    if (wait_for_counter(
            race.ready,
            &race.ready_workers,
            LCCF_RACE_WORKERS) != 0) {
        fail("ticket race workers ready");
        goto out;
    }

    for (generation = 0U;
         generation < LCCF_RACE_GENERATIONS;
         ++generation) {
        lccf_model_cell_hot_t *cell;
        uint64_t word;

        atomic_store_explicit(
            &race.completed_workers, 0U, memory_order_release);
        if (lccf_platform_event_signal(race.start) != 0 ||
            wait_for_counter(
                race.done,
                &race.completed_workers,
                LCCF_RACE_WORKERS) != 0 ||
            atomic_load_explicit(
                &race.error, memory_order_acquire) != 0) {
            fail("ticket race generation dispatch");
            goto out;
        }
        cell = lccf_model_cell_at(race.batch, 0U);
        word = atomic_load_explicit(
            &cell->state_generation, memory_order_acquire);
        if (lccf_model_unpack_state(word) !=
                LCCF_MODEL_STATE_CLAIMED ||
            atomic_load_explicit(
                &cell->backend_refs,
                memory_order_acquire) != 0U ||
            lccf_model_resume_claimed_cell(
                race.batch, 0U, &metrics) != 0) {
            fail("ticket race claim or resume");
            goto out;
        }
    }
    for (worker_index = 0U;
         worker_index < LCCF_RACE_WORKERS;
         ++worker_index) {
        int thread_result = EPROTO;

        if (lccf_platform_thread_join(
                threads[worker_index],
                &thread_result) != 0 ||
            thread_result != 0) {
            threads[worker_index] = NULL;
            fail("ticket race thread join");
            goto out;
        }
        threads[worker_index] = NULL;
    }
    if (atomic_load_explicit(
            &race.wins, memory_order_acquire) !=
            LCCF_RACE_GENERATIONS ||
        atomic_load_explicit(
            &race.stale, memory_order_acquire) !=
            UINT64_C(2) * LCCF_RACE_GENERATIONS ||
        metrics.resume_calls != LCCF_RACE_GENERATIONS ||
        metrics.direct_calls != LCCF_RACE_GENERATIONS ||
        metrics.hot_allocations != 0U) {
        fail("ticket race accounting");
        goto out;
    }
    rc = 0;

out:
    atomic_store_explicit(&race.stop, true, memory_order_release);
    if (race.start != NULL) {
        (void)lccf_platform_event_signal(race.start);
    }
    for (worker_index = 0U;
         worker_index < started_workers;
         ++worker_index) {
        if (threads[worker_index] != NULL) {
            (void)lccf_platform_thread_join(
                threads[worker_index], NULL);
        }
    }
    lccf_platform_event_destroy(race.done);
    lccf_platform_event_destroy(race.ready);
    lccf_platform_event_destroy(race.start);
    lccf_model_batch_destroy(race.batch);
    return rc;
}

static int run_remote_differential_case(
    lccf_model_workload_t workload,
    size_t instance_count,
    unsigned chain_length,
    uint64_t seed) {
    lccf_model_config_t waker_config = base_config();
    lccf_model_config_t cell_config;
    lccf_model_batch_t *waker = NULL;
    lccf_model_batch_t *cell = NULL;
    lccf_model_metrics_t waker_metrics = {0};
    lccf_model_metrics_t cell_metrics = {0};
    unsigned round;
    int rc = 1;

    waker_config.mode = LCCF_MODEL_REMOTE_WAKER_QUEUE;
    waker_config.workload = workload;
    waker_config.instance_count = instance_count;
    waker_config.frame_bytes = 128U;
    waker_config.cell_bytes = 96U;
    waker_config.site_count = 8U;
    waker_config.chain_length = chain_length;
    waker_config.remote_producers = 2U;
    waker_config.seed = seed;
    cell_config = waker_config;
    cell_config.mode = LCCF_MODEL_REMOTE_CAUSAL_CELL;

    if (lccf_model_batch_create(&waker_config, &waker) != 0 ||
        lccf_model_batch_create(&cell_config, &cell) != 0 ||
        !lccf_model_batch_equal(waker, cell)) {
        fail("remote differential create");
        goto out;
    }
    for (round = 0U; round < 19U; ++round) {
        if (lccf_model_run_round(waker, &waker_metrics) != 0 ||
            lccf_model_run_round(cell, &cell_metrics) != 0 ||
            !lccf_model_batch_equal(waker, cell) ||
            lccf_model_checksum(waker) !=
                lccf_model_checksum(cell)) {
            fprintf(stderr,
                    "[test_lccf_model] remote differential round=%u "
                    "workload=%s instances=%zu seed=%016llx\n",
                    round,
                    lccf_model_workload_name(workload),
                    instance_count,
                    (unsigned long long)seed);
            goto out;
        }
    }
    {
        const uint64_t completions =
            (uint64_t)instance_count * UINT64_C(19);
        const uint64_t callbacks =
            completions * (uint64_t)chain_length;
        const uint64_t expected_stale =
            workload == LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
                completions * UINT64_C(2) :
                0U;

        if (waker_metrics.completions != completions ||
            waker_metrics.claims != completions ||
            waker_metrics.remote_pushes != completions ||
            waker_metrics.queue_pushes != callbacks ||
            waker_metrics.queue_pops != callbacks ||
            waker_metrics.resume_calls != callbacks ||
            waker_metrics.stale_tickets != 0U ||
            waker_metrics.hot_allocations != 0U ||
            cell_metrics.completions != completions ||
            cell_metrics.claims != completions ||
            cell_metrics.remote_pushes != completions ||
            cell_metrics.queue_pushes != callbacks ||
            cell_metrics.queue_pops != callbacks ||
            cell_metrics.resume_calls != callbacks ||
            cell_metrics.stale_tickets != expected_stale ||
            cell_metrics.hot_allocations != 0U) {
            fail("remote metric invariants");
            goto out;
        }
    }
    rc = 0;

out:
    lccf_model_batch_destroy(cell);
    lccf_model_batch_destroy(waker);
    return rc;
}

static int test_remote_differential(void) {
    static const lccf_model_workload_t workloads[] = {
        LCCF_MODEL_COMPLETION_IO_PIPELINE,
        LCCF_MODEL_COMPLETION_RPC_STATE,
        LCCF_MODEL_COMPLETION_TIMER_CANCEL,
    };
    static const size_t instance_counts[] = {37U, 257U};
    static const uint64_t seeds[] = {
        UINT64_C(1),
        UINT64_C(0x0123456789ABCDEF),
        UINT64_C(0xFEDCBA9876543210),
    };
    size_t workload_index;
    size_t count_index;
    size_t seed_index;

    for (workload_index = 0U;
         workload_index <
         sizeof(workloads) / sizeof(workloads[0]);
         ++workload_index) {
        for (count_index = 0U;
             count_index <
             sizeof(instance_counts) / sizeof(instance_counts[0]);
             ++count_index) {
            for (seed_index = 0U;
                 seed_index < sizeof(seeds) / sizeof(seeds[0]);
                 ++seed_index) {
                if (run_remote_differential_case(
                        workloads[workload_index],
                        instance_counts[count_index],
                        seed_index == 2U ? 18U : 1U,
                        seeds[seed_index]) != 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int test_remote_destroy_after_error(void) {
    lccf_model_config_t config = base_config();
    lccf_model_batch_t *batch = NULL;
    lccf_model_metrics_t metrics = {0};

    config.mode = LCCF_MODEL_REMOTE_WAKER_QUEUE;
    config.remote_producers = 2U;
    if (lccf_model_batch_create(&config, &batch) != 0) {
        return fail("remote error setup");
    }
    batch->instances[1U].batch = NULL;
    if (lccf_model_run_round(batch, &metrics) != EPROTO) {
        lccf_model_batch_destroy(batch);
        return fail("remote injected error");
    }
    lccf_model_batch_destroy(batch);
    return 0;
}

static bool fact_pair_routing_metrics_equal(
    const lccf_model_metrics_t *baseline,
    const lccf_model_metrics_t *candidate) {
    return baseline->completions == candidate->completions &&
           baseline->claims == candidate->claims &&
           baseline->stale_tickets == candidate->stale_tickets &&
           baseline->queue_pushes == candidate->queue_pushes &&
           baseline->queue_pops == candidate->queue_pops &&
           baseline->resume_calls == candidate->resume_calls &&
           baseline->direct_calls == candidate->direct_calls &&
           baseline->forced_escapes == candidate->forced_escapes &&
           baseline->fairness_samples == candidate->fairness_samples &&
           baseline->hot_allocations == candidate->hot_allocations &&
           baseline->facts_attempted == candidate->facts_attempted &&
           baseline->facts_built == candidate->facts_built &&
           baseline->facts_build_failed ==
               candidate->facts_build_failed &&
           baseline->fact_module_pins == candidate->fact_module_pins &&
           baseline->fact_payload_pins == candidate->fact_payload_pins &&
           baseline->fact_stale_losers ==
               candidate->fact_stale_losers &&
           baseline->fact_guard_rechecks ==
               candidate->fact_guard_rechecks &&
           baseline->fact_queue_forwards ==
               candidate->fact_queue_forwards &&
           baseline->fact_generation_mismatches ==
               candidate->fact_generation_mismatches &&
           baseline->fact_reuse_delays == candidate->fact_reuse_delays &&
           baseline->fact_hot_bytes == candidate->fact_hot_bytes &&
           baseline->fact_sidecar_bytes ==
               candidate->fact_sidecar_bytes;
}

static int run_fact_differential_case(
    lccf_model_workload_t workload,
    lccf_model_mode_t baseline_mode,
    lccf_model_mode_t candidate_mode,
    size_t frame_bytes,
    size_t cell_bytes,
    unsigned site_count,
    uint64_t seed) {
    lccf_model_config_t baseline_config = base_config();
    lccf_model_config_t candidate_config;
    lccf_model_batch_t *baseline = NULL;
    lccf_model_batch_t *candidate = NULL;
    lccf_model_metrics_t baseline_metrics = {0};
    lccf_model_metrics_t candidate_metrics = {0};
    const uint64_t rounds = UINT64_C(5);
    const uint64_t instances = UINT64_C(17);
    const uint64_t chain = UINT64_C(5);
    const uint64_t completions = rounds * instances;
    const uint64_t callbacks = completions * chain;
    const uint64_t ticket_count =
        workload == LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
            UINT64_C(3) : UINT64_C(1);
    uint64_t baseline_expected_work;
    size_t index;
    uint64_t round;
    int rc = 1;

    baseline_config.workload = workload;
    baseline_config.mode = baseline_mode;
    baseline_config.instance_count = (size_t)instances;
    baseline_config.frame_bytes = frame_bytes;
    baseline_config.cell_bytes = cell_bytes;
    baseline_config.site_count = site_count;
    baseline_config.chain_length = (unsigned)chain;
    baseline_config.direct_budget = 8U;
    baseline_config.seed = seed;
    candidate_config = baseline_config;
    candidate_config.mode = candidate_mode;

    if (lccf_model_batch_create(&baseline_config, &baseline) != 0 ||
        lccf_model_batch_create(&candidate_config, &candidate) != 0 ||
        !lccf_model_batch_equal(baseline, candidate)) {
        fail("fact differential create");
        goto out;
    }
    for (round = 0U; round < rounds; ++round) {
        if (lccf_model_run_round(baseline, &baseline_metrics) != 0 ||
            lccf_model_run_round(candidate, &candidate_metrics) != 0 ||
            !lccf_model_batch_equal(baseline, candidate) ||
            lccf_model_checksum(baseline) !=
                lccf_model_checksum(candidate)) {
            fprintf(stderr,
                    "[test_lccf_model] fact differential round=%" PRIu64
                    " workload=%s pair=%s/%s frame=%zu cell=%zu sites=%u\n",
                    round, lccf_model_workload_name(workload),
                    lccf_model_mode_name(baseline_mode),
                    lccf_model_mode_name(candidate_mode), frame_bytes,
                    cell_bytes, site_count);
            goto out;
        }
    }
    for (index = 0U; index < (size_t)instances; ++index) {
        const lccf_model_instance_t *left = &baseline->instances[index];
        const lccf_model_instance_t *right = &candidate->instances[index];

        if (left->event_sequence_hash != right->event_sequence_hash ||
            left->command_sequence_hash != right->command_sequence_hash ||
            left->callback_sequence_count != callbacks / instances ||
            right->callback_sequence_count != callbacks / instances ||
            lccf_fact_module_unregister(left->fact_cell) != 0 ||
            lccf_fact_module_unregister(right->fact_cell) != 0) {
            fail("fact event/command sequence equivalence");
            goto out;
        }
    }
    if (!fact_pair_routing_metrics_equal(&baseline_metrics,
                                         &candidate_metrics) ||
        baseline_metrics.completions != completions ||
        baseline_metrics.resume_calls != callbacks ||
        baseline_metrics.facts_attempted != completions * ticket_count ||
        baseline_metrics.facts_built != completions ||
        baseline_metrics.fact_stale_losers !=
            completions * (ticket_count - UINT64_C(1)) ||
        baseline_metrics.fact_module_pins != completions ||
        baseline_metrics.facts_build_failed != 0U ||
        baseline_metrics.fact_generation_mismatches != 0U ||
        baseline_metrics.fact_reuse_delays != 0U ||
        baseline_metrics.fact_queue_forwards != 0U ||
        baseline_metrics.hot_allocations != 0U ||
        candidate_metrics.fact_normalizations != completions ||
        candidate_metrics.fact_site_lookups != completions) {
        fail("fact differential accounting");
        goto out;
    }
    if (baseline_mode == LCCF_MODEL_RECOMPUTE_QUEUE) {
        baseline_expected_work = callbacks + completions;
        if (baseline_metrics.direct_calls != 0U ||
            baseline_metrics.queue_pops != callbacks ||
            baseline_metrics.forced_escapes != 0U) {
            fail("fact queue routing");
            goto out;
        }
    } else if (baseline_mode == LCCF_MODEL_RECOMPUTE_FUSED) {
        baseline_expected_work = callbacks;
        if (baseline_metrics.direct_calls != callbacks ||
            baseline_metrics.queue_pops != 0U ||
            baseline_metrics.forced_escapes != 0U) {
            fail("fact fused routing");
            goto out;
        }
    } else {
        baseline_expected_work =
            callbacks + baseline_metrics.forced_escapes;
        if (baseline_metrics.forced_escapes == 0U ||
            baseline_metrics.forced_escapes >= completions ||
            baseline_metrics.queue_pops !=
                baseline_metrics.forced_escapes * chain ||
            baseline_metrics.direct_calls +
                    baseline_metrics.queue_pops !=
                callbacks) {
            fail("fact mixed routing");
            goto out;
        }
    }
    if (baseline_metrics.fact_normalizations != baseline_expected_work ||
        baseline_metrics.fact_site_lookups != baseline_expected_work ||
        baseline_metrics.fact_guard_rechecks != baseline_expected_work ||
        candidate_metrics.fact_guard_rechecks != baseline_expected_work) {
        fail("fact recompute/shared work separation");
        goto out;
    }
    rc = 0;

out:
    lccf_model_batch_destroy(candidate);
    lccf_model_batch_destroy(baseline);
    return rc;
}

static int test_fact_differential_matrix(void) {
    static const lccf_model_workload_t workloads[] = {
        LCCF_MODEL_COMPLETION_IO_PIPELINE,
        LCCF_MODEL_COMPLETION_RPC_STATE,
        LCCF_MODEL_COMPLETION_TIMER_CANCEL,
        LCCF_MODEL_COMPLETION_MIXED_FAIRNESS,
    };
    static const struct {
        lccf_model_mode_t baseline;
        lccf_model_mode_t candidate;
    } pairs[] = {
        {LCCF_MODEL_RECOMPUTE_QUEUE, LCCF_MODEL_SHARED_FACT_QUEUE},
        {LCCF_MODEL_RECOMPUTE_FUSED, LCCF_MODEL_SHARED_FACT_FUSED},
        {LCCF_MODEL_MIXED_RECOMPUTE, LCCF_MODEL_MIXED_SHARED_FACT},
    };
    static const size_t frames[] = {64U, 128U, 256U};
    static const size_t cells[] = {64U, 96U, 128U};
    static const unsigned sites[] = {1U, 8U};
    size_t workload_index;
    size_t pair_index;
    size_t layout_index;
    size_t site_index;

    for (workload_index = 0U;
         workload_index < sizeof(workloads) / sizeof(workloads[0]);
         ++workload_index) {
        for (pair_index = 0U;
             pair_index < sizeof(pairs) / sizeof(pairs[0]);
             ++pair_index) {
            for (layout_index = 0U;
                 layout_index < sizeof(cells) / sizeof(cells[0]);
                 ++layout_index) {
                for (site_index = 0U;
                     site_index < sizeof(sites) / sizeof(sites[0]);
                     ++site_index) {
                    if (run_fact_differential_case(
                            workloads[workload_index],
                            pairs[pair_index].baseline,
                            pairs[pair_index].candidate,
                            frames[(workload_index + layout_index) %
                                   (sizeof(frames) / sizeof(frames[0]))],
                            cells[layout_index], sites[site_index],
                            UINT64_C(0x6c6363662d636673) ^
                                (uint64_t)(workload_index * 101U +
                                           pair_index * 17U +
                                           layout_index * 5U + site_index)) !=
                        0) {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

int main(void) {
    if (test_names_and_parsers() != 0 ||
        test_candidate_baseline_mapping() != 0 ||
        test_create_validation() != 0 ||
        test_baseline_matrix() != 0 ||
        test_baseline_continue_requeues() != 0 ||
        test_causal_differential_matrix() != 0 ||
        test_causal_layout_and_stale_generation() != 0 ||
        test_fused_differential_matrix() != 0 ||
        test_exact_causal_budgets() != 0 ||
        test_mixed_fairness_accounting() != 0 ||
        test_platform_contract() != 0 ||
        test_three_way_ticket_race() != 0 ||
        test_remote_differential() != 0 ||
        test_remote_destroy_after_error() != 0 ||
        test_fact_differential_matrix() != 0) {
        return 1;
    }
    printf("[test_lccf_model] all checks passed\n");
    return 0;
}
