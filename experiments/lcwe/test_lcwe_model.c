// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Licensed under the LLAM Commercial Reciprocity License 1.0.
// See the LICENSE file distributed with this Software.

#include "lcwe_model.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int fail(const char *message) {
    fprintf(stderr, "[test_lcwe_model] %s\n", message);
    return 1;
}

static int test_names_and_parsers(void) {
    static const char *const mode_names[] = {
        "scalar",
        "cohort",
        "wave_pointers",
        "wave_capsule",
        "wave_aosoa",
    };
    static const char *const workload_names[] = {
        "exec_io_pipeline",
        "exec_rpc_state",
        "exec_event_fanout",
    };
    size_t i;

    for (i = 0U; i < sizeof(mode_names) / sizeof(mode_names[0]); ++i) {
        lcwe_model_mode_t parsed = LCWE_MODEL_SCALAR;
        const lcwe_model_mode_t expected = (lcwe_model_mode_t)i;

        if (lcwe_model_mode_name(expected) == NULL ||
            strcmp(lcwe_model_mode_name(expected), mode_names[i]) != 0 ||
            lcwe_model_parse_mode(mode_names[i], &parsed) != 0 ||
            parsed != expected) {
            return fail("mode name/parse contract");
        }
    }
    for (i = 0U; i < sizeof(workload_names) / sizeof(workload_names[0]); ++i) {
        lcwe_model_workload_t parsed = LCWE_MODEL_EXEC_IO_PIPELINE;
        const lcwe_model_workload_t expected = (lcwe_model_workload_t)i;

        if (lcwe_model_workload_name(expected) == NULL ||
            strcmp(lcwe_model_workload_name(expected), workload_names[i]) != 0 ||
            lcwe_model_parse_workload(workload_names[i], &parsed) != 0 ||
            parsed != expected) {
            return fail("workload name/parse contract");
        }
    }
    if (lcwe_model_mode_name((lcwe_model_mode_t)99) != NULL ||
        lcwe_model_workload_name((lcwe_model_workload_t)99) != NULL) {
        return fail("invalid enum name must be null");
    }
    if (lcwe_model_parse_mode(NULL, NULL) != EINVAL ||
        lcwe_model_parse_mode("scalar", NULL) != EINVAL ||
        lcwe_model_parse_mode("unknown", &(lcwe_model_mode_t){0}) != EINVAL ||
        lcwe_model_parse_workload(NULL, NULL) != EINVAL ||
        lcwe_model_parse_workload("exec_io_pipeline", NULL) != EINVAL ||
        lcwe_model_parse_workload("unknown",
                                  &(lcwe_model_workload_t){0}) != EINVAL) {
        return fail("invalid parser input");
    }
    return 0;
}

static int expect_create_error(lcwe_model_workload_t workload,
                               lcwe_model_mode_t mode,
                               size_t instance_count,
                               unsigned site_count,
                               int expected) {
    lcwe_model_batch_t *batch = (lcwe_model_batch_t *)(uintptr_t)1U;
    const int rc = lcwe_model_batch_create(workload,
                                           mode,
                                           instance_count,
                                           site_count,
                                           UINT64_C(1),
                                           &batch);

    if (rc != expected || batch != NULL) {
        return fail("create error contract");
    }
    return 0;
}

static int test_create_validation(void) {
    if (expect_create_error((lcwe_model_workload_t)99,
                            LCWE_MODEL_SCALAR,
                            1U,
                            1U,
                            EINVAL) != 0 ||
        expect_create_error(LCWE_MODEL_EXEC_IO_PIPELINE,
                            (lcwe_model_mode_t)99,
                            1U,
                            1U,
                            EINVAL) != 0 ||
        expect_create_error(LCWE_MODEL_EXEC_IO_PIPELINE,
                            LCWE_MODEL_SCALAR,
                            0U,
                            1U,
                            EINVAL) != 0 ||
        expect_create_error(LCWE_MODEL_EXEC_IO_PIPELINE,
                            LCWE_MODEL_SCALAR,
                            1U,
                            0U,
                            EINVAL) != 0 ||
        expect_create_error(LCWE_MODEL_EXEC_IO_PIPELINE,
                            LCWE_MODEL_SCALAR,
                            1U,
                            LCWE_MODEL_MAX_SITES + 1U,
                            EINVAL) != 0) {
        return 1;
    }

    if (lcwe_model_batch_create(LCWE_MODEL_EXEC_IO_PIPELINE,
                                LCWE_MODEL_SCALAR,
                                1U,
                                1U,
                                UINT64_C(1),
                                NULL) != EINVAL) {
        return fail("null create output");
    }
    return 0;
}

static int fail_differential(lcwe_model_workload_t workload,
                             lcwe_model_mode_t mode,
                             unsigned lane_width,
                             uint64_t seed,
                             size_t instance_count,
                             unsigned round,
                             const char *message) {
    fprintf(stderr,
            "[test_lcwe_model] %s workload=%s mode=%s lanes=%u "
            "seed=%016llx instances=%zu round=%u\n",
            message,
            lcwe_model_workload_name(workload),
            lcwe_model_mode_name(mode),
            lane_width,
            (unsigned long long)seed,
            instance_count,
            round);
    return 1;
}

static int check_candidate_metrics(lcwe_model_mode_t mode,
                                   size_t instance_count,
                                   unsigned rounds,
                                   const lcwe_model_metrics_t *metrics) {
    const uint64_t expected =
        (uint64_t)instance_count * (uint64_t)rounds;

    if (metrics->admissions != expected || metrics->tickets != expected ||
        metrics->hot_allocations != 0U) {
        return fail("candidate ticket metric invariant");
    }
    if (mode == LCWE_MODEL_COHORT) {
        if (metrics->scalar_calls != expected || metrics->wave_calls != 0U ||
            metrics->pointer_lanes != 0U ||
            metrics->capsule_lanes != 0U ||
            metrics->aosoa_lanes != 0U) {
            return fail("cohort dispatch accounting");
        }
        return 0;
    }
    if (metrics->scalar_calls != 0U || metrics->wave_calls == 0U) {
        return fail("wave dispatch accounting");
    }
    if ((mode == LCWE_MODEL_WAVE_POINTERS &&
         (metrics->pointer_lanes != expected ||
          metrics->capsule_lanes != 0U ||
          metrics->aosoa_lanes != 0U)) ||
        (mode == LCWE_MODEL_WAVE_CAPSULE &&
         (metrics->pointer_lanes != 0U ||
          metrics->capsule_lanes != expected ||
          metrics->aosoa_lanes != 0U)) ||
        (mode == LCWE_MODEL_WAVE_AOSOA &&
         (metrics->pointer_lanes != 0U ||
          metrics->capsule_lanes != 0U ||
          metrics->aosoa_lanes != expected))) {
        return fail("wave lane accounting");
    }
    return 0;
}

static int run_differential_case(lcwe_model_workload_t workload,
                                 lcwe_model_mode_t mode,
                                 unsigned lane_width,
                                 uint64_t seed,
                                 size_t instance_count) {
    const unsigned site_count =
        mode == LCWE_MODEL_WAVE_AOSOA ? 1U : 8U;
    lcwe_model_batch_t *scalar = NULL;
    lcwe_model_batch_t *candidate = NULL;
    lcwe_model_metrics_t scalar_metrics = {0};
    lcwe_model_metrics_t candidate_metrics = {0};
    unsigned round;
    int rc = 1;

    if (lcwe_model_batch_create(workload,
                                LCWE_MODEL_SCALAR,
                                instance_count,
                                site_count,
                                seed,
                                &scalar) != 0 ||
        lcwe_model_batch_create(workload,
                                mode,
                                instance_count,
                                site_count,
                                seed,
                                &candidate) != 0) {
        fail_differential(workload,
                          mode,
                          lane_width,
                          seed,
                          instance_count,
                          0U,
                          "differential create");
        goto out;
    }

    for (round = 0U; round < 19U; ++round) {
        if (lcwe_model_run_round(scalar, 1U, &scalar_metrics) != 0 ||
            lcwe_model_run_round(candidate,
                                 lane_width,
                                 &candidate_metrics) != 0) {
            fail_differential(workload,
                              mode,
                              lane_width,
                              seed,
                              instance_count,
                              round,
                              "differential round");
            goto out;
        }
        if (!lcwe_model_batch_equal(scalar, candidate) ||
            lcwe_model_checksum(scalar) !=
                lcwe_model_checksum(candidate)) {
            fail_differential(workload,
                              mode,
                              lane_width,
                              seed,
                              instance_count,
                              round,
                              "differential state mismatch");
            goto out;
        }
    }
    if (check_candidate_metrics(mode,
                                instance_count,
                                19U,
                                &candidate_metrics) != 0) {
        goto out;
    }
    rc = 0;

out:
    lcwe_model_batch_destroy(candidate);
    lcwe_model_batch_destroy(scalar);
    return rc;
}

static int test_non_scalar_modes(void) {
    static const uint64_t seeds[] = {
        UINT64_C(1),
        UINT64_C(0x6c6c616d77617665),
        UINT64_MAX - UINT64_C(17),
    };
    static const unsigned lane_widths[] = {1U, 2U, 4U, 8U, 16U, 32U};
    static const size_t instance_counts[] = {37U, 257U};
    unsigned workload;
    unsigned mode;
    size_t seed_index;
    size_t width_index;
    size_t count_index;

    if (expect_create_error(LCWE_MODEL_EXEC_IO_PIPELINE,
                            LCWE_MODEL_WAVE_AOSOA,
                            37U,
                            2U,
                            EINVAL) != 0) {
        return 1;
    }

    for (workload = (unsigned)LCWE_MODEL_EXEC_IO_PIPELINE;
         workload <= (unsigned)LCWE_MODEL_EXEC_EVENT_FANOUT;
         ++workload) {
        for (mode = (unsigned)LCWE_MODEL_COHORT;
             mode <= (unsigned)LCWE_MODEL_WAVE_AOSOA;
             ++mode) {
            for (seed_index = 0U;
                 seed_index < sizeof(seeds) / sizeof(seeds[0]);
                 ++seed_index) {
                for (width_index = 0U;
                     width_index <
                     sizeof(lane_widths) / sizeof(lane_widths[0]);
                     ++width_index) {
                    for (count_index = 0U;
                         count_index <
                         sizeof(instance_counts) /
                             sizeof(instance_counts[0]);
                         ++count_index) {
                        if (run_differential_case(
                                (lcwe_model_workload_t)workload,
                                (lcwe_model_mode_t)mode,
                                lane_widths[width_index],
                                seeds[seed_index],
                                instance_counts[count_index]) != 0) {
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

static int test_scalar_workloads(void) {
    unsigned workload;

    for (workload = (unsigned)LCWE_MODEL_EXEC_IO_PIPELINE;
         workload <= (unsigned)LCWE_MODEL_EXEC_EVENT_FANOUT;
         ++workload) {
        lcwe_model_batch_t *batch = NULL;
        lcwe_model_metrics_t metrics = {0};
        unsigned round;

        if (lcwe_model_batch_create((lcwe_model_workload_t)workload,
                                    LCWE_MODEL_SCALAR,
                                    37U,
                                    3U,
                                    UINT64_C(0x6c6c616d77617665),
                                    &batch) != 0 ||
            batch == NULL) {
            return fail("scalar create");
        }

        for (round = 0U; round < 11U; ++round) {
            if (lcwe_model_run_round(batch, 1U, &metrics) != 0) {
                lcwe_model_batch_destroy(batch);
                return fail("scalar round");
            }
        }

        if (metrics.admissions != UINT64_C(37) * 11U ||
            metrics.tickets != UINT64_C(37) * 11U ||
            metrics.scalar_calls != metrics.tickets ||
            metrics.wave_calls != 0U ||
            metrics.pointer_lanes != 0U ||
            metrics.capsule_lanes != 0U ||
            metrics.aosoa_lanes != 0U ||
            metrics.hot_allocations != 0U) {
            lcwe_model_batch_destroy(batch);
            return fail("scalar metric invariant");
        }
        if (lcwe_model_checksum(batch) == 0U) {
            lcwe_model_batch_destroy(batch);
            return fail("scalar checksum must be nonzero");
        }
        lcwe_model_batch_destroy(batch);
    }
    return 0;
}

static int test_determinism_and_round_validation(void) {
    lcwe_model_batch_t *first = NULL;
    lcwe_model_batch_t *second = NULL;
    lcwe_model_batch_t *different = NULL;
    lcwe_model_metrics_t first_metrics = {0};
    lcwe_model_metrics_t second_metrics = {0};
    lcwe_model_metrics_t untouched = {0};
    int rc = 1;

    if (lcwe_model_batch_create(LCWE_MODEL_EXEC_RPC_STATE,
                                LCWE_MODEL_SCALAR,
                                19U,
                                3U,
                                UINT64_C(7),
                                &first) != 0 ||
        lcwe_model_batch_create(LCWE_MODEL_EXEC_RPC_STATE,
                                LCWE_MODEL_SCALAR,
                                19U,
                                3U,
                                UINT64_C(7),
                                &second) != 0 ||
        lcwe_model_batch_create(LCWE_MODEL_EXEC_RPC_STATE,
                                LCWE_MODEL_SCALAR,
                                19U,
                                3U,
                                UINT64_C(8),
                                &different) != 0) {
        fail("determinism create");
        goto out;
    }
    if (!lcwe_model_batch_equal(first, second) ||
        lcwe_model_batch_equal(first, different) ||
        lcwe_model_batch_equal(NULL, second) ||
        lcwe_model_batch_equal(first, NULL)) {
        fail("initial deterministic equality");
        goto out;
    }
    if (lcwe_model_run_round(first, 1U, &first_metrics) != 0 ||
        lcwe_model_run_round(second, 1U, &second_metrics) != 0 ||
        !lcwe_model_batch_equal(first, second) ||
        lcwe_model_checksum(first) != lcwe_model_checksum(second)) {
        fail("round deterministic equality");
        goto out;
    }
    if (lcwe_model_run_round(NULL, 1U, &untouched) != EINVAL ||
        lcwe_model_run_round(first, 1U, NULL) != EINVAL ||
        lcwe_model_run_round(first, 0U, &untouched) != EINVAL ||
        lcwe_model_run_round(first, 3U, &untouched) != EINVAL ||
        lcwe_model_run_round(first, 33U, &untouched) != EINVAL ||
        memcmp(&untouched, &(lcwe_model_metrics_t){0}, sizeof(untouched)) != 0) {
        fail("round validation");
        goto out;
    }
    rc = 0;

out:
    lcwe_model_batch_destroy(different);
    lcwe_model_batch_destroy(second);
    lcwe_model_batch_destroy(first);
    lcwe_model_batch_destroy(NULL);
    return rc;
}

int main(void) {
    if (test_names_and_parsers() != 0 ||
        test_create_validation() != 0 ||
        test_scalar_workloads() != 0 ||
        test_determinism_and_round_validation() != 0 ||
        test_non_scalar_modes() != 0) {
        return 1;
    }
    puts("[test_lcwe_model] all checks passed");
    return 0;
}
