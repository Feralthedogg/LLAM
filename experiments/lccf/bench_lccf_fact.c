/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
 */

#include "lccf_model.h"
#include "lccf_platform.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LCCF_FACT_SAMPLE_VERSION 2U

typedef enum option_id {
    OPTION_WORKLOAD = 0,
    OPTION_MODE,
    OPTION_INSTANCES,
    OPTION_FRAME_BYTES,
    OPTION_CELL_BYTES,
    OPTION_SITES,
    OPTION_CHAIN,
    OPTION_ROUNDS,
    OPTION_WARMUP_ROUNDS,
    OPTION_SEED,
    OPTION_COUNT
} option_id_t;

typedef struct bench_options {
    lccf_model_workload_t workload;
    lccf_model_mode_t mode;
    size_t instances;
    size_t frame_bytes;
    size_t cell_bytes;
    unsigned sites;
    unsigned chain;
    uint64_t rounds;
    uint64_t warmup_rounds;
    uint64_t seed;
} bench_options_t;

static int fail_message(const char *message) {
    fprintf(stderr, "[bench_lccf_fact] %s\n", message);
    return 2;
}

static bool fact_mode(lccf_model_mode_t mode) {
    switch (mode) {
    case LCCF_MODEL_RECOMPUTE_QUEUE:
    case LCCF_MODEL_SHARED_EVENT_QUEUE:
    case LCCF_MODEL_SHARED_FACT_QUEUE:
    case LCCF_MODEL_RECOMPUTE_FUSED:
    case LCCF_MODEL_SHARED_EVENT_FUSED:
    case LCCF_MODEL_SHARED_FACT_FUSED:
    case LCCF_MODEL_MIXED_RECOMPUTE:
    case LCCF_MODEL_MIXED_SHARED_EVENT:
    case LCCF_MODEL_MIXED_SHARED_FACT:
        return true;
    default:
        return false;
    }
}

static bool shared_event_mode(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_SHARED_EVENT_QUEUE ||
           mode == LCCF_MODEL_SHARED_EVENT_FUSED ||
           mode == LCCF_MODEL_MIXED_SHARED_EVENT;
}

static bool shared_fact_mode(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_SHARED_FACT_QUEUE ||
           mode == LCCF_MODEL_SHARED_FACT_FUSED ||
           mode == LCCF_MODEL_MIXED_SHARED_FACT;
}

static bool queue_mode(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_RECOMPUTE_QUEUE ||
           mode == LCCF_MODEL_SHARED_EVENT_QUEUE ||
           mode == LCCF_MODEL_SHARED_FACT_QUEUE;
}

static bool mixed_mode(lccf_model_mode_t mode) {
    return mode == LCCF_MODEL_MIXED_RECOMPUTE ||
           mode == LCCF_MODEL_MIXED_SHARED_EVENT ||
           mode == LCCF_MODEL_MIXED_SHARED_FACT;
}

static int parse_u64(const char *text, uint64_t *out) {
    const unsigned char *cursor = (const unsigned char *)text;
    uint64_t value = 0U;

    if (text == NULL || out == NULL || *text == '\0') {
        return EINVAL;
    }
    while (*cursor != '\0') {
        const unsigned digit = (unsigned)(*cursor - (unsigned char)'0');

        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9' ||
            value > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10)) {
            return EINVAL;
        }
        value = value * UINT64_C(10) + (uint64_t)digit;
        ++cursor;
    }
    *out = value;
    return 0;
}

static int option_from_name(const char *name, option_id_t *out) {
    static const char *const names[OPTION_COUNT] = {
        "--workload",
        "--mode",
        "--instances",
        "--frame-bytes",
        "--cell-bytes",
        "--sites",
        "--chain",
        "--rounds",
        "--warmup-rounds",
        "--seed",
    };
    unsigned index;

    if (name == NULL || out == NULL) {
        return EINVAL;
    }
    for (index = 0U; index < OPTION_COUNT; ++index) {
        if (strcmp(name, names[index]) == 0) {
            *out = (option_id_t)index;
            return 0;
        }
    }
    return EINVAL;
}

static int assign_numeric(bench_options_t *options,
                          option_id_t id,
                          const char *text) {
    uint64_t value;

    if (parse_u64(text, &value) != 0) {
        return EINVAL;
    }
    switch (id) {
    case OPTION_INSTANCES:
        if (value == 0U || value > SIZE_MAX || value > UINT32_MAX) {
            return EINVAL;
        }
        options->instances = (size_t)value;
        return 0;
    case OPTION_FRAME_BYTES:
        if (value != 64U && value != 128U && value != 256U) {
            return EINVAL;
        }
        options->frame_bytes = (size_t)value;
        return 0;
    case OPTION_CELL_BYTES:
        if (value != 64U && value != 96U && value != 128U) {
            return EINVAL;
        }
        options->cell_bytes = (size_t)value;
        return 0;
    case OPTION_SITES:
        if (value != 1U && value != LCCF_MODEL_MAX_SITES) {
            return EINVAL;
        }
        options->sites = (unsigned)value;
        return 0;
    case OPTION_CHAIN:
        if (value == 0U || value > LCCF_MODEL_MAX_CHAIN_LENGTH) {
            return EINVAL;
        }
        options->chain = (unsigned)value;
        return 0;
    case OPTION_ROUNDS:
        if (value == 0U) {
            return EINVAL;
        }
        options->rounds = value;
        return 0;
    case OPTION_WARMUP_ROUNDS:
        options->warmup_rounds = value;
        return 0;
    case OPTION_SEED:
        options->seed = value;
        return 0;
    case OPTION_WORKLOAD:
    case OPTION_MODE:
    case OPTION_COUNT:
    default:
        return EINVAL;
    }
}

static int assign_option(bench_options_t *options,
                         option_id_t id,
                         const char *text) {
    if (id == OPTION_WORKLOAD) {
        return lccf_model_parse_workload(text, &options->workload);
    }
    if (id == OPTION_MODE) {
        if (lccf_model_parse_mode(text, &options->mode) != 0 ||
            !fact_mode(options->mode)) {
            return EINVAL;
        }
        return 0;
    }
    return assign_numeric(options, id, text);
}

static int parse_options(int argc, char **argv, bench_options_t *out) {
    bool seen[OPTION_COUNT] = {false};
    bench_options_t options;
    int index;

    if (out == NULL || argc != 1 + 2 * OPTION_COUNT) {
        return EINVAL;
    }
    memset(&options, 0, sizeof(options));
    for (index = 1; index < argc; index += 2) {
        option_id_t id;

        if (option_from_name(argv[index], &id) != 0 || seen[id] ||
            assign_option(&options, id, argv[index + 1]) != 0) {
            return EINVAL;
        }
        seen[id] = true;
    }
    for (index = 0; index < OPTION_COUNT; ++index) {
        if (!seen[index]) {
            return EINVAL;
        }
    }
    if (options.instances > SIZE_MAX / sizeof(uint64_t) ||
        options.rounds > SIZE_MAX / sizeof(uint64_t) ||
        options.rounds > UINT64_MAX / (uint64_t)options.instances ||
        options.rounds * (uint64_t)options.instances >
            UINT64_MAX / (uint64_t)options.chain) {
        return EOVERFLOW;
    }
    *out = options;
    return 0;
}

static int compare_u64(const void *left, const void *right) {
    const uint64_t lhs = *(const uint64_t *)left;
    const uint64_t rhs = *(const uint64_t *)right;

    return lhs < rhs ? -1 : (lhs > rhs ? 1 : 0);
}

static size_t percentile_index(size_t count, unsigned percentile) {
    const size_t rank =
        (count * (size_t)percentile + 99U) / 100U;

    return rank == 0U ? 0U : rank - 1U;
}

static int run_rounds(lccf_model_batch_t *batch,
                      uint64_t rounds,
                      lccf_model_metrics_t *metrics) {
    uint64_t round;

    for (round = 0U; round < rounds; ++round) {
        const int rc = lccf_model_run_round(batch, metrics);

        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static int verify_metrics(const bench_options_t *options,
                          const lccf_model_metrics_t *metrics) {
    const uint64_t completions =
        options->rounds * (uint64_t)options->instances;
    const uint64_t callbacks =
        completions * (uint64_t)options->chain;
    const uint64_t attempts =
        completions *
        (options->workload == LCCF_MODEL_COMPLETION_TIMER_CANCEL ?
             UINT64_C(3) : UINT64_C(1));
    const uint64_t recompute_work =
        callbacks +
        (queue_mode(options->mode)
             ? completions
             : (mixed_mode(options->mode)
                    ? metrics->forced_escapes
                    : 0U));
    const uint64_t expected_normalizations =
        shared_event_mode(options->mode) ||
                shared_fact_mode(options->mode)
            ? completions
            : recompute_work;
    const bool site_work_valid =
        shared_fact_mode(options->mode)
            ? metrics->fact_site_lookups >= completions &&
                  metrics->fact_site_lookups <= callbacks
            : metrics->fact_site_lookups == recompute_work;
    const uint64_t expected_sidecar_bytes =
        shared_fact_mode(options->mode)
            ? UINT64_C(64)
            : (shared_event_mode(options->mode) ? UINT64_C(48)
                                                : UINT64_C(0));

    if (metrics->completions != completions ||
        metrics->claims != completions ||
        metrics->resume_calls != callbacks ||
        metrics->queue_pushes != metrics->queue_pops ||
        metrics->direct_calls + metrics->queue_pops != callbacks ||
        metrics->facts_attempted != attempts ||
        metrics->facts_built != completions ||
        metrics->facts_build_failed != 0U ||
        metrics->fact_stale_losers != metrics->stale_tickets ||
        metrics->fact_module_pins != completions ||
        metrics->fact_payload_pins != completions ||
        metrics->fact_normalizations != expected_normalizations ||
        !site_work_valid ||
        metrics->fact_guard_rechecks != recompute_work ||
        metrics->fact_generation_mismatches != 0U ||
        metrics->fact_reuse_delays != 0U ||
        metrics->fact_queue_forwards != 0U ||
        metrics->hot_allocations != 0U ||
        metrics->fact_hot_bytes != UINT64_C(64) ||
        metrics->fact_sidecar_bytes != expected_sidecar_bytes ||
        metrics->fact_overflow_pushes != metrics->fact_overflow_pops) {
        return EPROTO;
    }
    return 0;
}

static int emit_sample(const bench_options_t *options,
                       const lccf_model_metrics_t *metrics,
                       uint64_t wall_ns,
                       uint64_t cpu_ns,
                       uint64_t p50_ns,
                       uint64_t p99_ns,
                       uint64_t checksum,
                       bool references_balanced) {
    const uint64_t callbacks =
        metrics->completions * (uint64_t)options->chain;
    const int written = printf(
        "LCCF_FACT_SAMPLE {"
        "\"version\":%u,"
        "\"workload\":\"%s\","
        "\"mode\":\"%s\","
        "\"instances\":%zu,"
        "\"frame_bytes\":%zu,"
        "\"cell_bytes\":%zu,"
        "\"sites\":%u,"
        "\"chain\":%u,"
        "\"rounds\":%" PRIu64 ","
        "\"operations\":%" PRIu64 ","
        "\"callbacks\":%" PRIu64 ","
        "\"wall_ns\":%" PRIu64 ","
        "\"cpu_ns\":%" PRIu64 ","
        "\"p50_ns\":%" PRIu64 ","
        "\"p99_ns\":%" PRIu64 ","
        "\"instructions_supported\":false,"
        "\"instructions\":0,"
        "\"checksum\":\"%016" PRIx64 "\","
        "\"completions\":%" PRIu64 ","
        "\"claims\":%" PRIu64 ","
        "\"stale_tickets\":%" PRIu64 ","
        "\"queue_pushes\":%" PRIu64 ","
        "\"queue_pops\":%" PRIu64 ","
        "\"resume_calls\":%" PRIu64 ","
        "\"direct_calls\":%" PRIu64 ","
        "\"forced_escapes\":%" PRIu64 ","
        "\"hot_allocations\":%" PRIu64 ","
        "\"facts_attempted\":%" PRIu64 ","
        "\"facts_built\":%" PRIu64 ","
        "\"facts_build_failed\":%" PRIu64 ","
        "\"fact_normalizations\":%" PRIu64 ","
        "\"fact_site_lookups\":%" PRIu64 ","
        "\"fact_module_pins\":%" PRIu64 ","
        "\"fact_payload_pins\":%" PRIu64 ","
        "\"fact_stale_losers\":%" PRIu64 ","
        "\"fact_guard_rechecks\":%" PRIu64 ","
        "\"fact_queue_forwards\":%" PRIu64 ","
        "\"fact_generation_mismatches\":%" PRIu64 ","
        "\"fact_reuse_delays\":%" PRIu64 ","
        "\"fact_hot_bytes\":%" PRIu64 ","
        "\"fact_sidecar_bytes\":%" PRIu64 ","
        "\"fact_overflow_pushes\":%" PRIu64 ","
        "\"fact_overflow_pops\":%" PRIu64 ","
        "\"refs_balanced\":%s}\n",
        LCCF_FACT_SAMPLE_VERSION,
        lccf_model_workload_name(options->workload),
        lccf_model_mode_name(options->mode),
        options->instances, options->frame_bytes, options->cell_bytes,
        options->sites, options->chain, options->rounds,
        callbacks, callbacks, wall_ns, cpu_ns, p50_ns, p99_ns,
        checksum, metrics->completions, metrics->claims,
        metrics->stale_tickets, metrics->queue_pushes,
        metrics->queue_pops, metrics->resume_calls,
        metrics->direct_calls, metrics->forced_escapes,
        metrics->hot_allocations, metrics->facts_attempted,
        metrics->facts_built, metrics->facts_build_failed,
        metrics->fact_normalizations, metrics->fact_site_lookups,
        metrics->fact_module_pins, metrics->fact_payload_pins,
        metrics->fact_stale_losers, metrics->fact_guard_rechecks,
        metrics->fact_queue_forwards,
        metrics->fact_generation_mismatches,
        metrics->fact_reuse_delays, metrics->fact_hot_bytes,
        metrics->fact_sidecar_bytes,
        metrics->fact_overflow_pushes,
        metrics->fact_overflow_pops,
        references_balanced ? "true" : "false");

    return written < 0 ? EIO : 0;
}

static int run_benchmark(const bench_options_t *options) {
    lccf_model_config_t config;
    lccf_model_batch_t *batch = NULL;
    lccf_model_metrics_t warmup_metrics = {0};
    lccf_model_metrics_t metrics = {0};
    uint64_t *latencies = NULL;
    uint64_t wall_start;
    uint64_t wall_end;
    uint64_t cpu_start;
    uint64_t cpu_end;
    uint64_t checksum;
    uint64_t round;
    int rc;
    int result = 2;

    memset(&config, 0, sizeof(config));
    config.workload = options->workload;
    config.mode = options->mode;
    config.instance_count = options->instances;
    config.frame_bytes = options->frame_bytes;
    config.cell_bytes = options->cell_bytes;
    config.site_count = options->sites;
    config.direct_budget = 8U;
    config.chain_length = options->chain;
    config.seed = options->seed;

    rc = lccf_model_batch_create(&config, &batch);
    if (rc != 0) {
        fail_message("batch creation failed");
        goto out;
    }
    if (options->warmup_rounds != 0U) {
        rc = run_rounds(batch, options->warmup_rounds, &warmup_metrics);
        if (rc != 0 || lccf_model_batch_reset(batch) != 0) {
            fail_message("warmup or reset failed");
            goto out;
        }
    }
    latencies = calloc((size_t)options->rounds, sizeof(*latencies));
    if (latencies == NULL) {
        fail_message("latency allocation failed");
        goto out;
    }
    wall_start = lccf_platform_monotonic_ns();
    cpu_start = lccf_platform_process_cpu_ns();
    if (wall_start == 0U || cpu_start == 0U) {
        fail_message("clock initialization failed");
        goto out;
    }
    for (round = 0U; round < options->rounds; ++round) {
        const uint64_t start = lccf_platform_monotonic_ns();
        uint64_t end;

        if (start == 0U || lccf_model_run_round(batch, &metrics) != 0) {
            fail_message("measured round failed");
            goto out;
        }
        end = lccf_platform_monotonic_ns();
        if (end < start) {
            fail_message("monotonic clock moved backwards");
            goto out;
        }
        latencies[round] = end == start ? UINT64_C(1) : end - start;
    }
    cpu_end = lccf_platform_process_cpu_ns();
    wall_end = lccf_platform_monotonic_ns();
    if (cpu_end <= cpu_start || wall_end <= wall_start ||
        verify_metrics(options, &metrics) != 0 ||
        !lccf_model_fact_references_balanced(batch)) {
        fail_message("measured integrity gate failed");
        goto out;
    }
    checksum = lccf_model_checksum(batch);
    if (checksum == 0U) {
        fail_message("checksum failed");
        goto out;
    }
    qsort(latencies, (size_t)options->rounds,
          sizeof(*latencies), compare_u64);
    rc = emit_sample(
        options, &metrics, wall_end - wall_start, cpu_end - cpu_start,
        latencies[percentile_index((size_t)options->rounds, 50U)],
        latencies[percentile_index((size_t)options->rounds, 99U)],
        checksum, true);
    if (rc != 0) {
        fail_message("sample output failed");
        goto out;
    }
    result = 0;

out:
    free(latencies);
    lccf_model_batch_destroy(batch);
    return result;
}

int main(int argc, char **argv) {
    bench_options_t options;

    if (parse_options(argc, argv, &options) != 0) {
        return fail_message("invalid command line");
    }
    return run_benchmark(&options);
}
