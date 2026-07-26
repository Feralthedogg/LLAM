// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model_internal.h"
#include "srem_platform.h"

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

static void test_tile_scalar_layout_and_coalescing(void) {
    srem_model_config_t config = valid_config();
    srem_model_batch_t *batch = NULL;
    srem_model_metrics_t metrics = {0};
    size_t tile;
    unsigned site;

    config.mode = SREM_MODEL_TILE_SCALAR;
    config.instance_count = 37U;
    config.tile_width = 8U;
    config.active_lanes = 4U;
    config.vector_threshold = 4U;
    config.divergence_eighths = 0U;
    CHECK(srem_model_batch_create(&config, &batch) == 0);
    CHECK(batch->tile_count == 5U);
    CHECK(batch->tile_slot_count == 40U);
    CHECK(((uintptr_t)batch->tile_fields & 63U) == 0U);
    CHECK(((uintptr_t)batch->tile_effect_arguments & 63U) == 0U);
    CHECK(((uintptr_t)batch->tile_event_word0 & 63U) == 0U);
    for (tile = 0U; tile < batch->tile_count; ++tile) {
        const uint32_t expected_valid =
            tile + 1U == batch->tile_count ? UINT32_C(0x1F) :
                                            UINT32_C(0xFF);
        CHECK(batch->tiles[tile].valid_mask == expected_valid);
    }

    CHECK(srem_model_run_round(batch, &metrics) == 0);
    CHECK(metrics.completions == expected_active(&config));
    CHECK(metrics.claims == metrics.completions);
    CHECK(metrics.queue_pushes == batch->tile_count);
    CHECK(metrics.queue_pops == batch->tile_count);
    CHECK(metrics.tile_dispatches == batch->tile_count);
    CHECK(metrics.scalar_lanes == metrics.completions);
    CHECK(metrics.resume_calls == 0U);
    CHECK(metrics.vector_lanes == 0U);
    CHECK(metrics.vector_blocks == 0U);
    CHECK(metrics.hot_allocations == 0U);
    for (tile = 0U; tile < batch->tile_count; ++tile) {
        CHECK(batch->tiles[tile].queued == 0U);
        CHECK(batch->tiles[tile].running == 0U);
        CHECK(batch->tiles[tile].pending_mask == 0U);
        for (site = 0U; site < SREM_MODEL_MAX_SITES; ++site) {
            CHECK(batch->tiles[tile].ready_mask[site] == 0U);
        }
    }
    srem_model_batch_destroy(batch);
}

static void test_tile_ticket_identity_and_reuse(void) {
    srem_model_config_t config = valid_config();
    srem_model_batch_t *batch = NULL;
    srem_model_metrics_t metrics = {0};
    srem_model_ticket_t ticket;
    uint32_t old_generation;

    config.mode = SREM_MODEL_TILE_SCALAR;
    config.instance_count = 16U;
    config.tile_width = 8U;
    config.active_lanes = 8U;
    config.vector_threshold = 4U;
    config.divergence_eighths = 0U;
    CHECK(srem_model_batch_create(&config, &batch) == 0);

    CHECK(srem_model_make_ticket(batch, 0U, &ticket) == 0);
    CHECK(srem_model_tile_admit_ticket(batch, &ticket, &metrics) == 0);
    CHECK(metrics.claims == 1U);
    CHECK(metrics.queue_pushes == 1U);
    CHECK(batch->local_queue.tail - batch->local_queue.head == 1U);
    CHECK(srem_model_tile_admit_ticket(batch, &ticket, &metrics) == 0);
    CHECK(metrics.duplicate_tickets == 1U);
    CHECK(metrics.queue_pushes == 1U);

    CHECK(srem_model_make_ticket(batch, 1U, &ticket) == 0);
    CHECK(srem_model_tile_admit_ticket(batch, &ticket, &metrics) == 0);
    CHECK(metrics.claims == 2U);
    CHECK(metrics.queue_pushes == 1U);
    CHECK(batch->local_queue.tail - batch->local_queue.head == 1U);

    CHECK(srem_model_make_ticket(batch, 8U, &ticket) == 0);
    old_generation = ticket.generation;
    *srem_model_tile_generation_at(batch, 8U) =
        old_generation + 2U;
    CHECK(srem_model_tile_admit_ticket(batch, &ticket, &metrics) == 0);
    CHECK(metrics.stale_tickets == 1U);
    CHECK(batch->tiles[1].pending_mask == 0U);

    CHECK(srem_model_tile_drain(batch, &metrics) == 0);
    CHECK(batch->tiles[0].pending_mask == 0U);
    CHECK(batch->tiles[0].queued == 0U);
    CHECK(batch->tiles[0].running == 0U);
    CHECK(srem_model_batch_reset(batch) == 0);
    srem_model_batch_destroy(batch);
}

static void test_waker_and_tile_modes_are_differential(void) {
    static const size_t INSTANCES[] = {37U, 257U};
    static const size_t FRAME_BYTES[] = {64U, 128U, 256U};
    static const unsigned WIDTHS[] = {8U, 16U, 32U};
    static const unsigned ACTIVE_DIVISORS[] = {1U, 2U, 8U};
    static const unsigned SITES[] = {1U, 8U};
    static const unsigned DIVERGENCE[] = {0U, 1U, 4U};
    static const srem_model_mode_t CANDIDATES[] = {
        SREM_MODEL_TILE_SCALAR,
        SREM_MODEL_TILE_VECTOR,
        SREM_MODEL_ADAPTIVE,
    };
    size_t workload;
    size_t instance_case;
    size_t frame_case;
    size_t width_case;
    size_t active_case;
    size_t site_case;
    size_t divergence_case;
    size_t candidate_case;

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
                    for (active_case = 0U;
                         active_case <
                         sizeof(ACTIVE_DIVISORS) /
                             sizeof(ACTIVE_DIVISORS[0]);
                         ++active_case) {
                        for (site_case = 0U;
                             site_case < sizeof(SITES) / sizeof(SITES[0]);
                             ++site_case) {
                            for (divergence_case = 0U;
                                 divergence_case <
                                 sizeof(DIVERGENCE) /
                                     sizeof(DIVERGENCE[0]);
                                 ++divergence_case) {
                                for (candidate_case = 0U;
                                     candidate_case <
                                     sizeof(CANDIDATES) /
                                         sizeof(CANDIDATES[0]);
                                     ++candidate_case) {
                                    srem_model_config_t baseline_config =
                                        valid_config();
                                    srem_model_config_t tile_config;
                                    srem_model_batch_t *baseline = NULL;
                                    srem_model_batch_t *tile = NULL;
                                    srem_model_metrics_t baseline_metrics = {
                                        0};
                                    srem_model_metrics_t tile_metrics = {0};
                                    unsigned round;

                                    baseline_config.workload =
                                        (srem_model_workload_t)workload;
                                    baseline_config.instance_count =
                                        INSTANCES[instance_case];
                                    baseline_config.frame_bytes =
                                        FRAME_BYTES[frame_case];
                                    baseline_config.tile_width =
                                        WIDTHS[width_case];
                                    baseline_config.active_lanes =
                                        baseline_config.tile_width /
                                        ACTIVE_DIVISORS[active_case];
                                    baseline_config.site_count =
                                        SITES[site_case];
                                    baseline_config.divergence_eighths =
                                        DIVERGENCE[divergence_case];
                                    baseline_config.vector_threshold =
                                        baseline_config.tile_width / 2U;
                                    tile_config = baseline_config;
                                    tile_config.mode =
                                        CANDIDATES[candidate_case];

                                    CHECK(srem_model_batch_create(
                                              &baseline_config,
                                              &baseline) == 0);
                                    CHECK(srem_model_batch_create(
                                              &tile_config,
                                              &tile) == 0);
                                    CHECK(srem_model_batch_equal(baseline,
                                                                 tile));
                                    for (round = 0U; round < 19U; ++round) {
                                        CHECK(srem_model_run_round(
                                                  baseline,
                                                  &baseline_metrics) == 0);
                                        CHECK(srem_model_run_round(
                                                  tile,
                                                  &tile_metrics) == 0);
                                        CHECK(srem_model_batch_equal(
                                            baseline,
                                            tile));
                                        CHECK(srem_model_checksum(baseline) ==
                                              srem_model_checksum(tile));
                                    }
                                    if (tile_config.mode ==
                                        SREM_MODEL_TILE_SCALAR) {
                                        CHECK(tile_metrics.scalar_lanes ==
                                              tile_metrics.completions);
                                    } else if (tile_config.mode ==
                                               SREM_MODEL_TILE_VECTOR) {
                                        CHECK(tile_metrics.vector_lanes ==
                                              tile_metrics.completions);
                                        CHECK(tile_metrics.vector_blocks >
                                              0U);
                                    } else {
                                        CHECK(
                                            tile_metrics.scalar_lanes +
                                                tile_metrics.vector_lanes ==
                                            tile_metrics.completions);
                                    }
                                    CHECK(tile_metrics.queue_pushes <=
                                          baseline_metrics.queue_pushes);
                                    srem_model_batch_destroy(tile);
                                    srem_model_batch_destroy(baseline);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

static void test_vector_and_adaptive_crossover(void) {
    srem_model_config_t config = valid_config();
    srem_model_batch_t *batch = NULL;
    srem_model_metrics_t metrics = {0};

    config.mode = SREM_MODEL_TILE_VECTOR;
    config.instance_count = 32U;
    config.tile_width = 8U;
    config.active_lanes = 1U;
    config.vector_threshold = 4U;
    config.divergence_eighths = 0U;
    CHECK(srem_model_batch_create(&config, &batch) == 0);
    CHECK(srem_model_run_round(batch, &metrics) == 0);
    CHECK(metrics.vector_lanes == metrics.completions);
    CHECK(metrics.scalar_lanes == 0U);
    CHECK(metrics.vector_blocks == batch->tile_count);
    srem_model_batch_destroy(batch);

    memset(&metrics, 0, sizeof(metrics));
    config.mode = SREM_MODEL_ADAPTIVE;
    CHECK(srem_model_batch_create(&config, &batch) == 0);
    CHECK(srem_model_run_round(batch, &metrics) == 0);
    CHECK(metrics.scalar_lanes == metrics.completions);
    CHECK(metrics.vector_lanes == 0U);
    CHECK(metrics.vector_blocks == 0U);
    srem_model_batch_destroy(batch);

    memset(&metrics, 0, sizeof(metrics));
    config.active_lanes = 8U;
    CHECK(srem_model_batch_create(&config, &batch) == 0);
    CHECK(srem_model_run_round(batch, &metrics) == 0);
    CHECK(metrics.vector_lanes == metrics.completions);
    CHECK(metrics.scalar_lanes == 0U);
    CHECK(metrics.vector_blocks == batch->tile_count);
    srem_model_batch_destroy(batch);
}

typedef struct platform_smoke_context {
    srem_platform_event_t *event;
} platform_smoke_context_t;

static int platform_smoke_thread(void *opaque) {
    platform_smoke_context_t *context = opaque;

    return srem_platform_event_signal(context->event) == 0 ? 73 : EIO;
}

static void test_platform_contract(void) {
    platform_smoke_context_t context = {0};
    srem_platform_thread_t *thread = NULL;
    uint64_t observed;
    int result = 0;
    char description[256];

    CHECK(srem_platform_event_create(&context.event) == 0);
    observed = srem_platform_event_epoch(context.event);
    CHECK(observed != UINT64_MAX);
    CHECK(srem_platform_thread_start(
              &thread, platform_smoke_thread, &context) == 0);
    CHECK(srem_platform_event_wait(context.event, observed) == 0);
    CHECK(srem_platform_thread_join(thread, &result) == 0);
    CHECK(result == 73);
    CHECK(srem_platform_monotonic_ns() != 0U);
    CHECK(srem_platform_process_cpu_ns() != 0U);
    CHECK(srem_platform_describe(
              description, sizeof(description)) == 0);
    CHECK(description[0] != '\0');
    srem_platform_event_destroy(context.event);
}

static void check_remote_quiescent(const srem_model_batch_t *batch) {
    size_t tile;
    unsigned site;

    CHECK(batch->remote_team.worker_count ==
          SREM_MODEL_REMOTE_PRODUCER_COUNT);
    CHECK(atomic_load_explicit(
              &batch->remote_queue.enqueue_position,
              memory_order_acquire) ==
          batch->remote_queue.dequeue_position);
    for (tile = 0U; tile < batch->tile_count; ++tile) {
        CHECK(atomic_load_explicit(
                  &batch->remote_pending_masks[tile],
                  memory_order_acquire) == 0U);
        CHECK(atomic_load_explicit(
                  &batch->remote_published[tile],
                  memory_order_acquire) == 0U);
        for (site = 0U; site < SREM_MODEL_MAX_SITES; ++site) {
            CHECK(atomic_load_explicit(
                      &batch->remote_ready_masks[
                          tile * SREM_MODEL_MAX_SITES + site],
                      memory_order_acquire) == 0U);
        }
    }
}

static void run_remote_differential_case(
    srem_model_workload_t workload,
    size_t instance_count,
    unsigned tile_width,
    unsigned active_lanes,
    unsigned site_count,
    unsigned divergence_eighths) {
    srem_model_config_t baseline_config = valid_config();
    srem_model_config_t candidate_config;
    srem_model_batch_t *baseline = NULL;
    srem_model_batch_t *candidate = NULL;
    srem_model_metrics_t baseline_metrics = {0};
    srem_model_metrics_t candidate_metrics = {0};
    srem_platform_thread_t *baseline_threads[
        SREM_MODEL_REMOTE_PRODUCER_COUNT];
    srem_platform_thread_t *candidate_threads[
        SREM_MODEL_REMOTE_PRODUCER_COUNT];
    const unsigned rounds = 17U;
    const uint64_t completions =
        expected_active(&(srem_model_config_t){
            .instance_count = instance_count,
            .tile_width = tile_width,
            .active_lanes = active_lanes,
        }) * rounds;
    unsigned worker;
    unsigned round;

    baseline_config.workload = workload;
    baseline_config.mode = SREM_MODEL_REMOTE_WAKER_FRAME;
    baseline_config.instance_count = instance_count;
    baseline_config.tile_width = tile_width;
    baseline_config.active_lanes = active_lanes;
    baseline_config.site_count = site_count;
    baseline_config.divergence_eighths = divergence_eighths;
    baseline_config.vector_threshold = tile_width / 2U;
    baseline_config.remote_producers =
        SREM_MODEL_REMOTE_PRODUCER_COUNT;
    candidate_config = baseline_config;
    candidate_config.mode = SREM_MODEL_REMOTE_ADAPTIVE;

    CHECK(srem_model_batch_create(&baseline_config, &baseline) == 0);
    CHECK(srem_model_batch_create(&candidate_config, &candidate) == 0);
    CHECK(srem_model_batch_equal(baseline, candidate));
    for (worker = 0U;
         worker < SREM_MODEL_REMOTE_PRODUCER_COUNT;
         ++worker) {
        baseline_threads[worker] =
            baseline->remote_team.workers[worker].thread;
        candidate_threads[worker] =
            candidate->remote_team.workers[worker].thread;
        CHECK(baseline_threads[worker] != NULL);
        CHECK(candidate_threads[worker] != NULL);
    }

    for (round = 0U; round < rounds; ++round) {
        CHECK(srem_model_run_round(baseline, &baseline_metrics) == 0);
        CHECK(srem_model_run_round(candidate, &candidate_metrics) == 0);
        CHECK(srem_model_batch_equal(baseline, candidate));
        CHECK(srem_model_checksum(baseline) ==
              srem_model_checksum(candidate));
        check_remote_quiescent(baseline);
        check_remote_quiescent(candidate);
        for (worker = 0U;
             worker < SREM_MODEL_REMOTE_PRODUCER_COUNT;
             ++worker) {
            CHECK(baseline->remote_team.workers[worker].thread ==
                  baseline_threads[worker]);
            CHECK(candidate->remote_team.workers[worker].thread ==
                  candidate_threads[worker]);
        }
    }

    CHECK(baseline_metrics.completions == completions);
    CHECK(candidate_metrics.completions == completions);
    CHECK(baseline_metrics.claims == completions);
    CHECK(candidate_metrics.claims == completions);
    CHECK(baseline_metrics.remote_pushes == completions);
    CHECK(candidate_metrics.remote_pushes == completions);
    CHECK(baseline_metrics.queue_pushes == completions);
    CHECK(baseline_metrics.queue_pops == completions);
    CHECK(baseline_metrics.resume_calls == completions);
    CHECK(candidate_metrics.queue_pushes <= completions);
    CHECK(candidate_metrics.queue_pops ==
          candidate_metrics.tile_dispatches);
    CHECK(candidate_metrics.scalar_lanes +
              candidate_metrics.vector_lanes ==
          completions);
    CHECK(baseline_metrics.stale_tickets == 0U);
    CHECK(candidate_metrics.stale_tickets == 0U);
    CHECK(baseline_metrics.duplicate_tickets == 0U);
    CHECK(candidate_metrics.duplicate_tickets == 0U);
    CHECK(baseline_metrics.hot_allocations == 0U);
    CHECK(candidate_metrics.hot_allocations == 0U);

    CHECK(srem_model_batch_reset(baseline) == 0);
    CHECK(srem_model_batch_reset(candidate) == 0);
    CHECK(srem_model_batch_equal(baseline, candidate));
    CHECK(srem_model_run_round(baseline, &baseline_metrics) == 0);
    CHECK(srem_model_run_round(candidate, &candidate_metrics) == 0);
    CHECK(srem_model_batch_equal(baseline, candidate));
    srem_model_batch_destroy(candidate);
    srem_model_batch_destroy(baseline);
}

static void test_remote_differential_and_team_reuse(void) {
    static const size_t INSTANCES[] = {37U, 257U};
    static const unsigned WIDTHS[] = {8U, 32U};
    static const unsigned SITES[] = {1U, 8U};
    size_t workload;
    size_t instance_case;
    size_t width_case;
    size_t site_case;

    for (workload = SREM_MODEL_HTTP_PIPELINE;
         workload <= SREM_MODEL_MIXED_FAIRNESS;
         ++workload) {
        for (instance_case = 0U;
             instance_case < sizeof(INSTANCES) / sizeof(INSTANCES[0]);
             ++instance_case) {
            for (width_case = 0U;
                 width_case < sizeof(WIDTHS) / sizeof(WIDTHS[0]);
                 ++width_case) {
                for (site_case = 0U;
                     site_case < sizeof(SITES) / sizeof(SITES[0]);
                     ++site_case) {
                    run_remote_differential_case(
                        (srem_model_workload_t)workload,
                        INSTANCES[instance_case],
                        WIDTHS[width_case],
                        1U,
                        SITES[site_case],
                        4U);
                    run_remote_differential_case(
                        (srem_model_workload_t)workload,
                        INSTANCES[instance_case],
                        WIDTHS[width_case],
                        WIDTHS[width_case],
                        SITES[site_case],
                        1U);
                }
            }
        }
    }
}

static void test_remote_tile_publication_coalesces(void) {
    srem_model_config_t config = valid_config();
    srem_model_batch_t *batch = NULL;
    srem_model_metrics_t metrics = {0};
    const unsigned rounds = 23U;
    unsigned round;

    config.mode = SREM_MODEL_REMOTE_ADAPTIVE;
    config.instance_count = 257U;
    config.tile_width = 32U;
    config.active_lanes = 32U;
    config.site_count = 1U;
    config.divergence_eighths = 0U;
    config.vector_threshold = 16U;
    config.remote_producers = SREM_MODEL_REMOTE_PRODUCER_COUNT;
    CHECK(srem_model_batch_create(&config, &batch) == 0);
    for (round = 0U; round < rounds; ++round) {
        CHECK(srem_model_run_round(batch, &metrics) == 0);
        check_remote_quiescent(batch);
    }
    CHECK(metrics.completions == expected_active(&config) * rounds);
    CHECK(metrics.queue_pushes == batch->tile_count * rounds);
    CHECK(metrics.queue_pops == batch->tile_count * rounds);
    CHECK(metrics.vector_lanes + metrics.scalar_lanes ==
          metrics.completions);
    CHECK(metrics.vector_blocks ==
          (batch->tile_count - 1U) * rounds);
    srem_model_batch_destroy(batch);
}

static void test_mixed_fairness_service_gap(void) {
    srem_model_config_t baseline_config = valid_config();
    srem_model_config_t candidate_config;
    srem_model_batch_t *baseline = NULL;
    srem_model_batch_t *candidate = NULL;
    srem_model_metrics_t baseline_metrics = {0};
    srem_model_metrics_t candidate_metrics = {0};
    srem_model_metrics_t baseline_warmup = {0};
    srem_model_metrics_t candidate_warmup = {0};
    unsigned round;

    baseline_config.workload = SREM_MODEL_MIXED_FAIRNESS;
    baseline_config.instance_count = 1024U;
    baseline_config.tile_width = 16U;
    baseline_config.active_lanes = 8U;
    baseline_config.site_count = 8U;
    baseline_config.divergence_eighths = 1U;
    baseline_config.vector_threshold = 8U;
    candidate_config = baseline_config;
    candidate_config.mode = SREM_MODEL_ADAPTIVE;
    CHECK(srem_model_batch_begin_measurement(NULL) == EINVAL);
    CHECK(srem_model_batch_create(&baseline_config, &baseline) == 0);
    CHECK(srem_model_batch_create(&candidate_config, &candidate) == 0);
    for (round = 0U; round < 3U; ++round) {
        CHECK(srem_model_run_round(
                  baseline, &baseline_warmup) == 0);
        CHECK(srem_model_run_round(
                  candidate, &candidate_warmup) == 0);
    }
    CHECK(baseline_warmup.fairness_samples != 0U);
    CHECK(srem_model_batch_equal(baseline, candidate));
    CHECK(srem_model_batch_begin_measurement(baseline) == 0);
    CHECK(srem_model_batch_begin_measurement(candidate) == 0);
    for (round = 0U; round < 31U; ++round) {
        CHECK(srem_model_run_round(baseline, &baseline_metrics) == 0);
        CHECK(srem_model_run_round(candidate, &candidate_metrics) == 0);
        CHECK(srem_model_batch_equal(baseline, candidate));
    }
    CHECK(baseline_metrics.fairness_samples != 0U);
    CHECK(candidate_metrics.fairness_samples ==
          baseline_metrics.fairness_samples);
    if (candidate_metrics.fairness_p99_gap >
        (baseline_metrics.fairness_p99_gap * 110U + 99U) / 100U) {
        fprintf(stderr,
                "[test_srem_model] fairness baseline=%" PRIu64
                " candidate=%" PRIu64 "\n",
                baseline_metrics.fairness_p99_gap,
                candidate_metrics.fairness_p99_gap);
    }
    CHECK(candidate_metrics.fairness_p99_gap <=
          (baseline_metrics.fairness_p99_gap * 110U + 99U) / 100U);
    srem_model_batch_destroy(candidate);
    srem_model_batch_destroy(baseline);
}

int main(void) {
    test_names_and_parsers();
    test_candidate_baselines();
    test_config_validation();
    test_waker_baseline_matrix();
    test_determinism_and_equality();
    test_tile_scalar_layout_and_coalescing();
    test_tile_ticket_identity_and_reuse();
    test_waker_and_tile_modes_are_differential();
    test_vector_and_adaptive_crossover();
    test_platform_contract();
    test_remote_differential_and_team_reuse();
    test_remote_tile_publication_coalesces();
    test_mixed_fairness_service_gap();
    puts("[test_srem_model] remote and fairness checks passed");
    return 0;
}
