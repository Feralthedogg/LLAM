// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "lccf_model_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    config.mode = LCCF_MODEL_FUSED_CAUSAL_CELL;
    if (expect_create_error(config, ENOTSUP) != 0) {
        return 1;
    }
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
            LCCF_MODEL_STATE_ARMED) {
        fail("stale generation must not claim new cell");
        goto out;
    }
    rc = 0;

out:
    lccf_model_batch_destroy(batch);
    return rc;
}

int main(void) {
    if (test_names_and_parsers() != 0 ||
        test_create_validation() != 0 ||
        test_baseline_matrix() != 0 ||
        test_baseline_continue_requeues() != 0 ||
        test_causal_differential_matrix() != 0 ||
        test_causal_layout_and_stale_generation() != 0) {
        return 1;
    }
    printf("[test_lccf_model] all checks passed\n");
    return 0;
}
