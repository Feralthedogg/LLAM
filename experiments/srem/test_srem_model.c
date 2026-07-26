// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fprintf(stderr,                                                   \
                    "%s:%d: check failed: %s\n",                              \
                    __FILE__,                                                 \
                    __LINE__,                                                 \
                    #expression);                                             \
            exit(EXIT_FAILURE);                                               \
        }                                                                     \
    } while (0)

static srem_model_config_t valid_config(void) {
    const srem_model_config_t config = {
        .workload = SREM_MODEL_HTTP_PIPELINE,
        .mode = SREM_MODEL_WAKER_FRAME,
        .instance_count = 257U,
        .frame_bytes = 128U,
        .tile_width = 16U,
        .active_lanes = 8U,
        .site_count = 8U,
        .divergence_eighths = 1U,
        .vector_threshold = 8U,
        .remote_producers = 0U,
        .seed = UINT64_C(0x53E4D13A9C721B0F),
    };
    return config;
}

static uint64_t expected_active(const srem_model_config_t *config) {
    uint64_t total = 0U;
    size_t begin = 0U;

    while (begin < config->instance_count) {
        const size_t remaining = config->instance_count - begin;
        const size_t lanes =
            remaining < config->tile_width ? remaining : config->tile_width;
        const size_t active =
            lanes < config->active_lanes ? lanes : config->active_lanes;
        total += active;
        begin += lanes;
    }
    return total;
}

static void test_names_and_parsers(void) {
    static const struct {
        srem_model_mode_t mode;
        const char *name;
    } MODES[] = {
        {SREM_MODEL_WAKER_FRAME, "waker_frame"},
        {SREM_MODEL_TILE_SCALAR, "tile_scalar"},
        {SREM_MODEL_TILE_VECTOR, "tile_vector"},
        {SREM_MODEL_ADAPTIVE, "adaptive_srem"},
        {SREM_MODEL_REMOTE_WAKER_FRAME, "remote_waker_frame"},
        {SREM_MODEL_REMOTE_ADAPTIVE, "remote_adaptive_srem"},
    };
    static const struct {
        srem_model_workload_t workload;
        const char *name;
    } WORKLOADS[] = {
        {SREM_MODEL_HTTP_PIPELINE, "srem_http_pipeline"},
        {SREM_MODEL_RPC_PIPELINE, "srem_rpc_pipeline"},
        {SREM_MODEL_DIVERGENT_CANCEL, "srem_divergent_cancel"},
        {SREM_MODEL_MIXED_FAIRNESS, "srem_mixed_fairness"},
    };
    size_t i;

    for (i = 0U; i < sizeof(MODES) / sizeof(MODES[0]); ++i) {
        srem_model_mode_t parsed = SREM_MODEL_WAKER_FRAME;
        CHECK(strcmp(srem_model_mode_name(MODES[i].mode),
                     MODES[i].name) == 0);
        CHECK(srem_model_parse_mode(MODES[i].name, &parsed) == 0);
        CHECK(parsed == MODES[i].mode);
    }
    for (i = 0U; i < sizeof(WORKLOADS) / sizeof(WORKLOADS[0]); ++i) {
        srem_model_workload_t parsed = SREM_MODEL_HTTP_PIPELINE;
        CHECK(strcmp(srem_model_workload_name(WORKLOADS[i].workload),
                     WORKLOADS[i].name) == 0);
        CHECK(srem_model_parse_workload(WORKLOADS[i].name, &parsed) == 0);
        CHECK(parsed == WORKLOADS[i].workload);
    }

    CHECK(srem_model_mode_name((srem_model_mode_t)-1) == NULL);
    CHECK(srem_model_workload_name((srem_model_workload_t)-1) == NULL);
    CHECK(srem_model_parse_mode(NULL, NULL) == EINVAL);
    CHECK(srem_model_parse_mode("WAKER_FRAME", NULL) == EINVAL);
    CHECK(srem_model_parse_workload(NULL, NULL) == EINVAL);
    CHECK(srem_model_parse_workload("unknown", NULL) == EINVAL);
}

static void test_candidate_baselines(void) {
    srem_model_mode_t baseline = SREM_MODEL_TILE_VECTOR;

    CHECK(srem_model_candidate_baseline(SREM_MODEL_TILE_SCALAR,
                                        &baseline) == 0);
    CHECK(baseline == SREM_MODEL_WAKER_FRAME);
    CHECK(srem_model_candidate_baseline(SREM_MODEL_TILE_VECTOR,
                                        &baseline) == 0);
    CHECK(baseline == SREM_MODEL_WAKER_FRAME);
    CHECK(srem_model_candidate_baseline(SREM_MODEL_ADAPTIVE,
                                        &baseline) == 0);
    CHECK(baseline == SREM_MODEL_WAKER_FRAME);
    CHECK(srem_model_candidate_baseline(SREM_MODEL_REMOTE_ADAPTIVE,
                                        &baseline) == 0);
    CHECK(baseline == SREM_MODEL_REMOTE_WAKER_FRAME);
    CHECK(srem_model_candidate_baseline(SREM_MODEL_WAKER_FRAME,
                                        &baseline) == EINVAL);
    CHECK(srem_model_candidate_baseline(SREM_MODEL_TILE_SCALAR,
                                        NULL) == EINVAL);
}

static void expect_bad_config(srem_model_config_t config) {
    srem_model_batch_t *batch = (srem_model_batch_t *)(uintptr_t)1U;
    CHECK(srem_model_batch_create(&config, &batch) == EINVAL);
    CHECK(batch == NULL);
}

static void test_config_validation(void) {
    srem_model_config_t config = valid_config();
    srem_model_batch_t *batch = NULL;

    CHECK(srem_model_batch_create(NULL, &batch) == EINVAL);
    CHECK(srem_model_batch_create(&config, NULL) == EINVAL);

    config.workload = (srem_model_workload_t)-1;
    expect_bad_config(config);
    config = valid_config();
    config.mode = (srem_model_mode_t)-1;
    expect_bad_config(config);
    config = valid_config();
    config.instance_count = 0U;
    expect_bad_config(config);
    config = valid_config();
    config.frame_bytes = 96U;
    expect_bad_config(config);
    config = valid_config();
    config.tile_width = 4U;
    expect_bad_config(config);
    config = valid_config();
    config.tile_width = 64U;
    expect_bad_config(config);
    config = valid_config();
    config.active_lanes = 0U;
    expect_bad_config(config);
    config = valid_config();
    config.active_lanes = 17U;
    expect_bad_config(config);
    config = valid_config();
    config.site_count = 2U;
    expect_bad_config(config);
    config = valid_config();
    config.divergence_eighths = 2U;
    expect_bad_config(config);
    config = valid_config();
    config.vector_threshold = 0U;
    expect_bad_config(config);
    config = valid_config();
    config.vector_threshold = 17U;
    expect_bad_config(config);
    config = valid_config();
    config.remote_producers = 1U;
    expect_bad_config(config);

    config = valid_config();
    config.mode = SREM_MODEL_REMOTE_WAKER_FRAME;
    config.remote_producers = 0U;
    expect_bad_config(config);
    config.remote_producers = SREM_MODEL_REMOTE_PRODUCER_COUNT;
    CHECK(srem_model_batch_create(&config, &batch) == 0);
    srem_model_batch_destroy(batch);

    config = valid_config();
    config.remote_producers = SREM_MODEL_REMOTE_PRODUCER_COUNT;
    expect_bad_config(config);
}

static void test_waker_baseline_matrix(void) {
    static const size_t INSTANCES[] = {37U, 257U};
    static const size_t FRAME_BYTES[] = {64U, 128U, 256U};
    static const unsigned WIDTHS[] = {8U, 16U, 32U};
    static const unsigned SITES[] = {1U, 8U};
    size_t workload;
    size_t instance_case;
    size_t frame_case;
    size_t width_case;
    size_t site_case;

    for (workload = SREM_MODEL_HTTP_PIPELINE;
         workload <= SREM_MODEL_MIXED_FAIRNESS;
         ++workload) {
        for (instance_case = 0U;
             instance_case < sizeof(INSTANCES) / sizeof(INSTANCES[0]);
             ++instance_case) {
            for (frame_case = 0U;
                 frame_case < sizeof(FRAME_BYTES) / sizeof(FRAME_BYTES[0]);
                 ++frame_case) {
                for (width_case = 0U;
                     width_case < sizeof(WIDTHS) / sizeof(WIDTHS[0]);
                     ++width_case) {
                    for (site_case = 0U;
                         site_case < sizeof(SITES) / sizeof(SITES[0]);
                         ++site_case) {
                        srem_model_config_t config = valid_config();
                        srem_model_batch_t *batch = NULL;
                        srem_model_metrics_t metrics = {0};
                        uint64_t initial_checksum;
                        uint64_t changed_checksum;
                        unsigned round;

                        config.workload =
                            (srem_model_workload_t)workload;
                        config.instance_count = INSTANCES[instance_case];
                        config.frame_bytes = FRAME_BYTES[frame_case];
                        config.tile_width = WIDTHS[width_case];
                        config.active_lanes = config.tile_width / 2U;
                        config.site_count = SITES[site_case];
                        config.vector_threshold = config.active_lanes;
                        CHECK(srem_model_batch_create(&config, &batch) == 0);
                        initial_checksum = srem_model_checksum(batch);

                        for (round = 0U; round < 19U; ++round) {
                            CHECK(srem_model_run_round(batch, &metrics) == 0);
                        }
                        CHECK(metrics.completions ==
                              expected_active(&config) * 19U);
                        CHECK(metrics.claims == metrics.completions);
                        CHECK(metrics.queue_pushes == metrics.completions);
                        CHECK(metrics.queue_pops == metrics.completions);
                        CHECK(metrics.resume_calls == metrics.completions);
                        CHECK(metrics.tile_dispatches == 0U);
                        CHECK(metrics.scalar_lanes == 0U);
                        CHECK(metrics.vector_lanes == 0U);
                        CHECK(metrics.vector_blocks == 0U);
                        CHECK(metrics.hot_allocations == 0U);
                        changed_checksum = srem_model_checksum(batch);
                        CHECK(changed_checksum != initial_checksum);
                        CHECK(srem_model_batch_reset(batch) == 0);
                        CHECK(srem_model_checksum(batch) == initial_checksum);
                        srem_model_batch_destroy(batch);
                    }
                }
            }
        }
    }
}

static void test_determinism_and_equality(void) {
    srem_model_config_t config = valid_config();
    srem_model_batch_t *left = NULL;
    srem_model_batch_t *right = NULL;
    srem_model_metrics_t left_metrics = {0};
    srem_model_metrics_t right_metrics = {0};
    unsigned round;

    CHECK(srem_model_batch_create(&config, &left) == 0);
    CHECK(srem_model_batch_create(&config, &right) == 0);
    CHECK(srem_model_batch_equal(left, right));

    for (round = 0U; round < 23U; ++round) {
        CHECK(srem_model_run_round(left, &left_metrics) == 0);
        CHECK(srem_model_run_round(right, &right_metrics) == 0);
        CHECK(srem_model_batch_equal(left, right));
        CHECK(srem_model_checksum(left) == srem_model_checksum(right));
    }

    srem_model_batch_destroy(right);
    srem_model_batch_destroy(left);
}

static void test_unimplemented_modes_are_explicit(void) {
    static const srem_model_mode_t MODES[] = {
        SREM_MODEL_TILE_SCALAR,
        SREM_MODEL_TILE_VECTOR,
        SREM_MODEL_ADAPTIVE,
        SREM_MODEL_REMOTE_WAKER_FRAME,
        SREM_MODEL_REMOTE_ADAPTIVE,
    };
    size_t i;

    for (i = 0U; i < sizeof(MODES) / sizeof(MODES[0]); ++i) {
        srem_model_config_t config = valid_config();
        srem_model_batch_t *batch = NULL;
        srem_model_metrics_t metrics = {0};

        config.mode = MODES[i];
        if (MODES[i] == SREM_MODEL_REMOTE_WAKER_FRAME ||
            MODES[i] == SREM_MODEL_REMOTE_ADAPTIVE) {
            config.remote_producers = SREM_MODEL_REMOTE_PRODUCER_COUNT;
        }
        CHECK(srem_model_batch_create(&config, &batch) == 0);
        CHECK(srem_model_run_round(batch, &metrics) == ENOTSUP);
        srem_model_batch_destroy(batch);
    }
}

int main(void) {
    test_names_and_parsers();
    test_candidate_baselines();
    test_config_validation();
    test_waker_baseline_matrix();
    test_determinism_and_equality();
    test_unimplemented_modes_are_explicit();
    puts("[test_srem_model] baseline checks passed");
    return 0;
}
