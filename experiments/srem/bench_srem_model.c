// Copyright 2026 Feralthedogg
// SPDX-License-Identifier: Apache-2.0

#include "srem_model.h"
#include "srem_platform.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum pair_order {
    PAIR_ORDER_ABBA = 0,
    PAIR_ORDER_BAAB = 1,
} pair_order_t;

typedef struct bench_options {
    srem_model_workload_t workload;
    srem_model_mode_t candidate;
    srem_model_mode_t baseline;
    size_t instances;
    size_t frame_bytes;
    unsigned tile_width;
    unsigned active_lanes;
    unsigned sites;
    unsigned divergence_eighths;
    unsigned threshold;
    unsigned producers;
    uint64_t seed;
    uint64_t min_mode_ns;
    pair_order_t order;
    uint64_t warmup_rounds;
    bool pin_owner;
    unsigned owner_cpu;
} bench_options_t;

typedef struct block_time {
    uint64_t wall_ns;
    uint64_t cpu_ns;
} block_time_t;

typedef enum option_id {
    OPTION_WORKLOAD = 0,
    OPTION_PAIR,
    OPTION_INSTANCES,
    OPTION_FRAME_BYTES,
    OPTION_TILE_WIDTH,
    OPTION_ACTIVE_LANES,
    OPTION_SITES,
    OPTION_DIVERGENCE_EIGHTHS,
    OPTION_THRESHOLD,
    OPTION_PRODUCERS,
    OPTION_SEED,
    OPTION_MIN_MODE_MS,
    OPTION_ORDER,
    OPTION_WARMUP_ROUNDS,
    OPTION_OWNER_CPU,
    OPTION_COUNT,
} option_id_t;

static int fail_message(const char *message) {
    fprintf(stderr, "[bench_srem_model] %s\n", message);
    return 2;
}

static bool mode_is_remote(srem_model_mode_t mode) {
    return mode == SREM_MODEL_REMOTE_WAKER_FRAME ||
           mode == SREM_MODEL_REMOTE_ADAPTIVE;
}

static bool mode_uses_tiles(srem_model_mode_t mode) {
    return mode == SREM_MODEL_TILE_SCALAR ||
           mode == SREM_MODEL_TILE_VECTOR ||
           mode == SREM_MODEL_ADAPTIVE ||
           mode == SREM_MODEL_REMOTE_ADAPTIVE;
}

static int parse_u64(const char *text, uint64_t *out) {
    uint64_t value = 0U;
    const unsigned char *cursor =
        (const unsigned char *)text;

    if (text == NULL || out == NULL || *text == '\0') {
        return EINVAL;
    }
    while (*cursor != '\0') {
        const unsigned digit =
            (unsigned)(*cursor - (unsigned char)'0');

        if (*cursor < (unsigned char)'0' ||
            *cursor > (unsigned char)'9' ||
            value >
                (UINT64_MAX - (uint64_t)digit) /
                    UINT64_C(10)) {
            return EINVAL;
        }
        value =
            value * UINT64_C(10) + (uint64_t)digit;
        cursor += 1;
    }
    *out = value;
    return 0;
}

static int option_id_from_name(const char *name,
                               option_id_t *out) {
    static const char *const NAMES[OPTION_COUNT] = {
        "--workload",
        "--pair",
        "--instances",
        "--frame-bytes",
        "--tile-width",
        "--active-lanes",
        "--sites",
        "--divergence-eighths",
        "--threshold",
        "--producers",
        "--seed",
        "--min-mode-ms",
        "--order",
        "--warmup-rounds",
        "--owner-cpu",
    };
    unsigned index;

    if (name == NULL || out == NULL) {
        return EINVAL;
    }
    for (index = 0U; index < OPTION_COUNT; ++index) {
        if (strcmp(name, NAMES[index]) == 0) {
            *out = (option_id_t)index;
            return 0;
        }
    }
    return EINVAL;
}

static int assign_numeric_option(bench_options_t *options,
                                 option_id_t id,
                                 const char *value) {
    uint64_t parsed;

    if (parse_u64(value, &parsed) != 0) {
        return EINVAL;
    }
    switch (id) {
    case OPTION_INSTANCES:
        if (parsed == 0U || parsed > SIZE_MAX ||
            parsed > UINT32_MAX) {
            return EINVAL;
        }
        options->instances = (size_t)parsed;
        return 0;
    case OPTION_FRAME_BYTES:
        if (parsed != 64U && parsed != 128U &&
            parsed != 256U) {
            return EINVAL;
        }
        options->frame_bytes = (size_t)parsed;
        return 0;
    case OPTION_TILE_WIDTH:
        if (parsed != 8U && parsed != 16U &&
            parsed != 32U) {
            return EINVAL;
        }
        options->tile_width = (unsigned)parsed;
        return 0;
    case OPTION_ACTIVE_LANES:
        if (parsed == 0U ||
            parsed > SREM_MODEL_MAX_TILE_WIDTH) {
            return EINVAL;
        }
        options->active_lanes = (unsigned)parsed;
        return 0;
    case OPTION_SITES:
        if (parsed != 1U &&
            parsed != SREM_MODEL_MAX_SITES) {
            return EINVAL;
        }
        options->sites = (unsigned)parsed;
        return 0;
    case OPTION_DIVERGENCE_EIGHTHS:
        if (parsed != 0U && parsed != 1U &&
            parsed != 4U) {
            return EINVAL;
        }
        options->divergence_eighths =
            (unsigned)parsed;
        return 0;
    case OPTION_THRESHOLD:
        if (parsed == 0U ||
            parsed > SREM_MODEL_MAX_TILE_WIDTH) {
            return EINVAL;
        }
        options->threshold = (unsigned)parsed;
        return 0;
    case OPTION_PRODUCERS:
        if (parsed != 0U &&
            parsed != SREM_MODEL_REMOTE_PRODUCER_COUNT) {
            return EINVAL;
        }
        options->producers = (unsigned)parsed;
        return 0;
    case OPTION_SEED:
        if (parsed == 0U) {
            return EINVAL;
        }
        options->seed = parsed;
        return 0;
    case OPTION_MIN_MODE_MS:
        if (parsed == 0U ||
            parsed > UINT64_C(60000)) {
            return EINVAL;
        }
        options->min_mode_ns =
            parsed * UINT64_C(1000000);
        return 0;
    case OPTION_WARMUP_ROUNDS:
        if (parsed == 0U) {
            return EINVAL;
        }
        options->warmup_rounds = parsed;
        return 0;
    case OPTION_OWNER_CPU:
        if (parsed > UINT_MAX) {
            return EINVAL;
        }
        options->pin_owner = true;
        options->owner_cpu = (unsigned)parsed;
        return 0;
    case OPTION_WORKLOAD:
    case OPTION_PAIR:
    case OPTION_ORDER:
    case OPTION_COUNT:
    default:
        return EINVAL;
    }
}

static int assign_option(bench_options_t *options,
                         option_id_t id,
                         const char *value) {
    switch (id) {
    case OPTION_WORKLOAD:
        return srem_model_parse_workload(
            value, &options->workload);
    case OPTION_PAIR:
        if (srem_model_parse_mode(
                value, &options->candidate) != 0) {
            return EINVAL;
        }
        return srem_model_candidate_baseline(
            options->candidate, &options->baseline);
    case OPTION_ORDER:
        if (strcmp(value, "abba") == 0) {
            options->order = PAIR_ORDER_ABBA;
            return 0;
        }
        if (strcmp(value, "baab") == 0) {
            options->order = PAIR_ORDER_BAAB;
            return 0;
        }
        return EINVAL;
    case OPTION_OWNER_CPU:
        if (strcmp(value, "none") == 0) {
            options->pin_owner = false;
            options->owner_cpu = 0U;
            return 0;
        }
        return assign_numeric_option(options, id, value);
    case OPTION_INSTANCES:
    case OPTION_FRAME_BYTES:
    case OPTION_TILE_WIDTH:
    case OPTION_ACTIVE_LANES:
    case OPTION_SITES:
    case OPTION_DIVERGENCE_EIGHTHS:
    case OPTION_THRESHOLD:
    case OPTION_PRODUCERS:
    case OPTION_SEED:
    case OPTION_MIN_MODE_MS:
    case OPTION_WARMUP_ROUNDS:
        return assign_numeric_option(options, id, value);
    case OPTION_COUNT:
    default:
        return EINVAL;
    }
}

static int parse_options(int argc,
                         char **argv,
                         bench_options_t *out) {
    bool seen[OPTION_COUNT] = {false};
    bench_options_t options;
    int index;

    if (out == NULL || argc != 1 + 2 * OPTION_COUNT) {
        return EINVAL;
    }
    memset(&options, 0, sizeof(options));
    for (index = 1; index < argc; index += 2) {
        option_id_t id;

        if (option_id_from_name(argv[index], &id) != 0 ||
            seen[id] ||
            assign_option(
                &options, id, argv[index + 1]) != 0) {
            return EINVAL;
        }
        seen[id] = true;
    }
    for (index = 0; index < OPTION_COUNT; ++index) {
        if (!seen[index]) {
            return EINVAL;
        }
    }
    if (options.active_lanes > options.tile_width ||
        options.threshold > options.tile_width ||
        (mode_is_remote(options.candidate) &&
         options.producers !=
             SREM_MODEL_REMOTE_PRODUCER_COUNT) ||
        (!mode_is_remote(options.candidate) &&
         options.producers != 0U)) {
        return EINVAL;
    }
    *out = options;
    return 0;
}

static int add_duration(uint64_t *target,
                        uint64_t increment) {
    if (target == NULL ||
        increment > UINT64_MAX - *target) {
        return EOVERFLOW;
    }
    *target += increment;
    return 0;
}

static int run_rounds(srem_model_batch_t *batch,
                      uint64_t rounds,
                      srem_model_metrics_t *metrics) {
    uint64_t round;

    if (batch == NULL || rounds == 0U || metrics == NULL) {
        return EINVAL;
    }
    for (round = 0U; round < rounds; ++round) {
        const int error =
            srem_model_run_round(batch, metrics);

        if (error != 0) {
            return error;
        }
    }
    return 0;
}

static int measure_block(srem_model_batch_t *batch,
                         uint64_t rounds,
                         srem_model_metrics_t *metrics,
                         block_time_t *out) {
    const uint64_t wall_start =
        srem_platform_monotonic_ns();
    const uint64_t cpu_start =
        srem_platform_process_cpu_ns();
    uint64_t cpu_end;
    uint64_t wall_end;
    int error;

    if (out == NULL || wall_start == 0U ||
        cpu_start == 0U) {
        return EINVAL;
    }
    error = run_rounds(batch, rounds, metrics);
    if (error != 0) {
        return error;
    }
    cpu_end = srem_platform_process_cpu_ns();
    wall_end = srem_platform_monotonic_ns();
    if (cpu_end <= cpu_start || wall_end <= wall_start) {
        return ERANGE;
    }
    out->cpu_ns = cpu_end - cpu_start;
    out->wall_ns = wall_end - wall_start;
    return 0;
}

static int batches_match(
    const srem_model_batch_t *baseline,
    const srem_model_batch_t *candidate,
    uint64_t *out_baseline_checksum,
    uint64_t *out_candidate_checksum) {
    const uint64_t baseline_checksum =
        srem_model_checksum(baseline);
    const uint64_t candidate_checksum =
        srem_model_checksum(candidate);

    if (baseline_checksum == 0U ||
        candidate_checksum == 0U ||
        baseline_checksum != candidate_checksum ||
        !srem_model_batch_equal(baseline, candidate)) {
        return EPROTO;
    }
    if (out_baseline_checksum != NULL) {
        *out_baseline_checksum = baseline_checksum;
    }
    if (out_candidate_checksum != NULL) {
        *out_candidate_checksum = candidate_checksum;
    }
    return 0;
}

static int prepare_pair(
    srem_model_batch_t *baseline,
    srem_model_batch_t *candidate,
    uint64_t warmup_rounds) {
    srem_model_metrics_t baseline_warmup = {0};
    srem_model_metrics_t candidate_warmup = {0};
    int error;

    error = srem_model_batch_reset(baseline);
    if (error == 0) {
        error = srem_model_batch_reset(candidate);
    }
    if (error == 0) {
        error = run_rounds(
            baseline, warmup_rounds, &baseline_warmup);
    }
    if (error == 0) {
        error = run_rounds(
            candidate, warmup_rounds, &candidate_warmup);
    }
    if (error == 0) {
        error = batches_match(
            baseline, candidate, NULL, NULL);
    }
    if (error == 0) {
        error =
            srem_model_batch_begin_measurement(baseline);
    }
    if (error == 0) {
        error =
            srem_model_batch_begin_measurement(candidate);
    }
    return error;
}

static int calibrate_rounds(
    srem_model_batch_t *baseline,
    srem_model_batch_t *candidate,
    uint64_t warmup_rounds,
    uint64_t min_mode_ns,
    uint64_t *out_rounds) {
    const uint64_t minimum_block_ns =
        min_mode_ns / UINT64_C(2) +
        min_mode_ns % UINT64_C(2);
    uint64_t rounds = UINT64_C(1);

    if (baseline == NULL || candidate == NULL ||
        warmup_rounds == 0U || min_mode_ns == 0U ||
        out_rounds == NULL) {
        return EINVAL;
    }
    for (;;) {
        srem_model_metrics_t baseline_metrics = {0};
        srem_model_metrics_t candidate_metrics = {0};
        block_time_t baseline_time;
        block_time_t candidate_time;
        int error = prepare_pair(
            baseline, candidate, warmup_rounds);

        if (error == 0) {
            error = measure_block(
                baseline,
                rounds,
                &baseline_metrics,
                &baseline_time);
        }
        if (error == 0) {
            error = measure_block(
                candidate,
                rounds,
                &candidate_metrics,
                &candidate_time);
        }
        if (error == 0) {
            error = batches_match(
                baseline, candidate, NULL, NULL);
        }
        if (error != 0) {
            return error;
        }
        if (baseline_time.wall_ns >= minimum_block_ns &&
            candidate_time.wall_ns >= minimum_block_ns) {
            *out_rounds = rounds;
            return 0;
        }
        if (rounds > UINT64_MAX / UINT64_C(2)) {
            return EOVERFLOW;
        }
        rounds *= UINT64_C(2);
    }
}

static int expected_active_per_round(
    const bench_options_t *options,
    uint64_t *out) {
    const size_t full_tiles =
        options->instances / options->tile_width;
    const size_t remainder =
        options->instances % options->tile_width;
    uint64_t total;

    if (out == NULL ||
        full_tiles >
            UINT64_MAX / options->active_lanes) {
        return EOVERFLOW;
    }
    total =
        (uint64_t)full_tiles * options->active_lanes;
    if (remainder != 0U) {
        const size_t partial =
            remainder < options->active_lanes ?
                remainder :
                options->active_lanes;

        if ((uint64_t)partial > UINT64_MAX - total) {
            return EOVERFLOW;
        }
        total += (uint64_t)partial;
    }
    if (total == 0U) {
        return EPROTO;
    }
    *out = total;
    return 0;
}

static int verify_metrics(
    const bench_options_t *options,
    srem_model_mode_t mode,
    uint64_t rounds,
    const srem_model_metrics_t *metrics) {
    uint64_t active_per_round;
    uint64_t completions;
    uint64_t expected_remote;

    if (options == NULL || metrics == NULL ||
        expected_active_per_round(
            options, &active_per_round) != 0 ||
        rounds > UINT64_MAX / active_per_round) {
        return EOVERFLOW;
    }
    completions = rounds * active_per_round;
    expected_remote =
        mode_is_remote(mode) ? completions : 0U;
    if (metrics->completions != completions ||
        metrics->claims != completions ||
        metrics->stale_tickets != 0U ||
        metrics->duplicate_tickets != 0U ||
        metrics->queue_pushes != metrics->queue_pops ||
        metrics->remote_pushes != expected_remote ||
        metrics->hot_allocations != 0U) {
        return EPROTO;
    }
    if (options->workload ==
        SREM_MODEL_MIXED_FAIRNESS) {
        if (metrics->fairness_samples == 0U ||
            metrics->fairness_p99_gap == 0U) {
            return EPROTO;
        }
    } else if (metrics->fairness_samples != 0U ||
               metrics->fairness_p99_gap != 0U) {
        return EPROTO;
    }

    if (!mode_uses_tiles(mode)) {
        if (metrics->queue_pushes != completions ||
            metrics->resume_calls != completions ||
            metrics->tile_dispatches != 0U ||
            metrics->scalar_lanes != 0U ||
            metrics->vector_lanes != 0U ||
            metrics->vector_blocks != 0U) {
            return EPROTO;
        }
        return 0;
    }
    if (metrics->resume_calls != 0U ||
        metrics->tile_dispatches != metrics->queue_pops ||
        metrics->scalar_lanes >
            UINT64_MAX - metrics->vector_lanes ||
        metrics->scalar_lanes + metrics->vector_lanes !=
            completions ||
        (metrics->vector_lanes == 0U &&
         metrics->vector_blocks != 0U) ||
        (metrics->vector_lanes != 0U &&
         metrics->vector_blocks == 0U)) {
        return EPROTO;
    }
    if (mode == SREM_MODEL_TILE_SCALAR &&
        (metrics->vector_lanes != 0U ||
         metrics->vector_blocks != 0U)) {
        return EPROTO;
    }
    if (mode == SREM_MODEL_TILE_VECTOR &&
        metrics->scalar_lanes != 0U) {
        return EPROTO;
    }
    return 0;
}

static int run_measured_pair(
    const bench_options_t *options,
    srem_model_batch_t *baseline,
    srem_model_batch_t *candidate,
    uint64_t rounds_per_block,
    srem_model_metrics_t *baseline_metrics,
    srem_model_metrics_t *candidate_metrics,
    block_time_t *baseline_total,
    block_time_t *candidate_total,
    uint64_t *out_baseline_checksum,
    uint64_t *out_candidate_checksum) {
    static const bool ABBA_CANDIDATE[4] = {
        false, true, true, false,
    };
    static const bool BAAB_CANDIDATE[4] = {
        true, false, false, true,
    };
    const bool *sequence =
        options->order == PAIR_ORDER_ABBA ?
            ABBA_CANDIDATE :
            BAAB_CANDIDATE;
    unsigned baseline_blocks = 0U;
    unsigned candidate_blocks = 0U;
    unsigned block;
    int error;

    memset(baseline_metrics, 0, sizeof(*baseline_metrics));
    memset(candidate_metrics, 0, sizeof(*candidate_metrics));
    memset(baseline_total, 0, sizeof(*baseline_total));
    memset(candidate_total, 0, sizeof(*candidate_total));
    error = prepare_pair(
        baseline, candidate, options->warmup_rounds);
    if (error != 0) {
        return error;
    }

    for (block = 0U; block < 4U; ++block) {
        const bool is_candidate = sequence[block];
        srem_model_batch_t *batch =
            is_candidate ? candidate : baseline;
        srem_model_metrics_t *metrics =
            is_candidate ?
                candidate_metrics :
                baseline_metrics;
        block_time_t *total =
            is_candidate ?
                candidate_total :
                baseline_total;
        block_time_t measured;

        error = measure_block(
            batch,
            rounds_per_block,
            metrics,
            &measured);
        if (error != 0 ||
            add_duration(
                &total->wall_ns, measured.wall_ns) != 0 ||
            add_duration(
                &total->cpu_ns, measured.cpu_ns) != 0) {
            return error != 0 ? error : EOVERFLOW;
        }
        if (is_candidate) {
            candidate_blocks += 1U;
        } else {
            baseline_blocks += 1U;
        }
        if (baseline_blocks == candidate_blocks) {
            error = batches_match(
                baseline,
                candidate,
                out_baseline_checksum,
                out_candidate_checksum);
            if (error != 0) {
                return error;
            }
        }
    }
    return baseline_blocks == 2U &&
                   candidate_blocks == 2U ?
               0 :
               EPROTO;
}

static int describe_affinity(const bench_options_t *options,
                             char *buffer,
                             size_t buffer_size) {
    int written;

    if (options->pin_owner) {
        const int affinity_error =
            srem_platform_pin_current_thread(
                options->owner_cpu);

        if (affinity_error == 0) {
            written = snprintf(
                buffer,
                buffer_size,
                "cpu%u",
                options->owner_cpu);
        } else {
            written = snprintf(
                buffer,
                buffer_size,
                "failed%u_%d",
                options->owner_cpu,
                affinity_error);
        }
    } else {
        written = snprintf(buffer, buffer_size, "none");
    }
    return written < 0 || (size_t)written >= buffer_size ?
               ENOSPC :
               0;
}

static int compiler_description(char *buffer,
                                size_t buffer_size) {
    int written;

#if defined(__clang__)
    written = snprintf(
        buffer,
        buffer_size,
        "clang-%d.%d.%d",
        __clang_major__,
        __clang_minor__,
        __clang_patchlevel__);
#elif defined(_MSC_VER)
    written = snprintf(
        buffer, buffer_size, "msvc-%d", _MSC_VER);
#elif defined(__GNUC__)
    written = snprintf(
        buffer,
        buffer_size,
        "gcc-%d.%d.%d",
        __GNUC__,
        __GNUC_MINOR__,
        __GNUC_PATCHLEVEL__);
#else
    written = snprintf(buffer, buffer_size, "unknown");
#endif
    return written < 0 || (size_t)written >= buffer_size ?
               ENOSPC :
               0;
}

static void sanitize_token(char *text) {
    unsigned char *cursor = (unsigned char *)text;

    while (*cursor != '\0') {
        if (isspace(*cursor)) {
            *cursor = (unsigned char)'_';
        }
        cursor += 1;
    }
}

static int run_benchmark(const bench_options_t *options) {
    srem_model_config_t baseline_config;
    srem_model_config_t candidate_config;
    srem_model_batch_t *baseline = NULL;
    srem_model_batch_t *candidate = NULL;
    srem_model_metrics_t baseline_metrics;
    srem_model_metrics_t candidate_metrics;
    block_time_t baseline_time;
    block_time_t candidate_time;
    uint64_t rounds_per_block;
    uint64_t measured_rounds;
    uint64_t active_per_round;
    uint64_t ops_per_mode;
    uint64_t baseline_checksum = 0U;
    uint64_t candidate_checksum = 0U;
    double wall_speedup;
    double cpu_ratio;
    char affinity[64];
    char compiler[64];
    char host[256];
    int error;
    int result = 2;

    memset(&baseline_config, 0, sizeof(baseline_config));
    baseline_config.workload = options->workload;
    baseline_config.mode = options->baseline;
    baseline_config.instance_count = options->instances;
    baseline_config.frame_bytes = options->frame_bytes;
    baseline_config.tile_width = options->tile_width;
    baseline_config.active_lanes = options->active_lanes;
    baseline_config.site_count = options->sites;
    baseline_config.divergence_eighths =
        options->divergence_eighths;
    baseline_config.vector_threshold = options->threshold;
    baseline_config.remote_producers = options->producers;
    baseline_config.seed = options->seed;
    candidate_config = baseline_config;
    candidate_config.mode = options->candidate;

    error = srem_model_batch_create(
        &baseline_config, &baseline);
    if (error == 0) {
        error = srem_model_batch_create(
            &candidate_config, &candidate);
    }
    if (error != 0) {
        fail_message("batch creation failed");
        goto out;
    }
    if (describe_affinity(
            options, affinity, sizeof(affinity)) != 0 ||
        compiler_description(
            compiler, sizeof(compiler)) != 0 ||
        srem_platform_describe(host, sizeof(host)) != 0) {
        fail_message("environment description failed");
        goto out;
    }
    sanitize_token(host);

    error = calibrate_rounds(
        baseline,
        candidate,
        options->warmup_rounds,
        options->min_mode_ns,
        &rounds_per_block);
    if (error != 0) {
        fail_message("paired calibration failed");
        goto out;
    }
    for (;;) {
        error = run_measured_pair(
            options,
            baseline,
            candidate,
            rounds_per_block,
            &baseline_metrics,
            &candidate_metrics,
            &baseline_time,
            &candidate_time,
            &baseline_checksum,
            &candidate_checksum);
        if (error != 0) {
            fail_message("paired measurement failed");
            goto out;
        }
        if (baseline_time.wall_ns >=
                options->min_mode_ns &&
            candidate_time.wall_ns >=
                options->min_mode_ns) {
            break;
        }
        if (rounds_per_block >
            UINT64_MAX / UINT64_C(2)) {
            fail_message(
                "measured duration calibration overflow");
            goto out;
        }
        rounds_per_block *= UINT64_C(2);
    }
    if (rounds_per_block >
        UINT64_MAX / UINT64_C(2)) {
        fail_message("measured round count overflow");
        goto out;
    }
    measured_rounds =
        rounds_per_block * UINT64_C(2);
    if (expected_active_per_round(
            options, &active_per_round) != 0 ||
        measured_rounds >
            UINT64_MAX / active_per_round) {
        fail_message("operation count overflow");
        goto out;
    }
    ops_per_mode = measured_rounds * active_per_round;
    if (baseline_time.cpu_ns == 0U ||
        candidate_time.cpu_ns == 0U ||
        verify_metrics(
            options,
            options->baseline,
            measured_rounds,
            &baseline_metrics) != 0 ||
        verify_metrics(
            options,
            options->candidate,
            measured_rounds,
            &candidate_metrics) != 0 ||
        baseline_metrics.fairness_samples !=
            candidate_metrics.fairness_samples ||
        baseline_checksum == 0U ||
        baseline_checksum != candidate_checksum) {
        fail_message("measured integrity gate failed");
        goto out;
    }
    wall_speedup =
        (double)baseline_time.wall_ns /
        (double)candidate_time.wall_ns;
    cpu_ratio =
        (double)candidate_time.cpu_ns /
        (double)baseline_time.cpu_ns;
    if (!isfinite(wall_speedup) ||
        !isfinite(cpu_ratio) ||
        wall_speedup <= 0.0 || cpu_ratio <= 0.0) {
        fail_message("non-finite paired ratio");
        goto out;
    }

    printf(
        "SREM_PAIR version=1 workload=%s candidate=%s baseline=%s "
        "instances=%zu frame_bytes=%zu tile_width=%u "
        "active_lanes=%u sites=%u divergence_eighths=%u "
        "threshold=%u producers=%u seed=%" PRIu64 " "
        "min_mode_ns=%" PRIu64 " warmup_rounds=%" PRIu64 " "
        "rounds_per_block=%" PRIu64 " ops_per_mode=%" PRIu64 " "
        "baseline_wall_ns=%" PRIu64 " candidate_wall_ns=%" PRIu64 " "
        "baseline_cpu_ns=%" PRIu64 " candidate_cpu_ns=%" PRIu64 " "
        "wall_speedup=%.12f cpu_ratio=%.12f "
        "baseline_checksum=%016" PRIx64 " "
        "candidate_checksum=%016" PRIx64 " "
        "baseline_completions=%" PRIu64 " "
        "candidate_completions=%" PRIu64 " "
        "baseline_queue_pushes=%" PRIu64 " "
        "candidate_queue_pushes=%" PRIu64 " "
        "baseline_queue_pops=%" PRIu64 " "
        "candidate_queue_pops=%" PRIu64 " "
        "candidate_tile_dispatches=%" PRIu64 " "
        "candidate_scalar_lanes=%" PRIu64 " "
        "candidate_vector_lanes=%" PRIu64 " "
        "candidate_vector_blocks=%" PRIu64 " "
        "baseline_remote_pushes=%" PRIu64 " "
        "candidate_remote_pushes=%" PRIu64 " "
        "baseline_fair_samples=%" PRIu64 " "
        "candidate_fair_samples=%" PRIu64 " "
        "baseline_fair_p99_gap=%" PRIu64 " "
        "candidate_fair_p99_gap=%" PRIu64 " "
        "candidate_forced_escapes=%" PRIu64 " "
        "hot_allocations=%" PRIu64 " order=%s affinity=%s "
        "clock=monotonic+process_cpu compiler=%s host=%s\n",
        srem_model_workload_name(options->workload),
        srem_model_mode_name(options->candidate),
        srem_model_mode_name(options->baseline),
        options->instances,
        options->frame_bytes,
        options->tile_width,
        options->active_lanes,
        options->sites,
        options->divergence_eighths,
        options->threshold,
        options->producers,
        options->seed,
        options->min_mode_ns,
        options->warmup_rounds,
        rounds_per_block,
        ops_per_mode,
        baseline_time.wall_ns,
        candidate_time.wall_ns,
        baseline_time.cpu_ns,
        candidate_time.cpu_ns,
        wall_speedup,
        cpu_ratio,
        baseline_checksum,
        candidate_checksum,
        baseline_metrics.completions,
        candidate_metrics.completions,
        baseline_metrics.queue_pushes,
        candidate_metrics.queue_pushes,
        baseline_metrics.queue_pops,
        candidate_metrics.queue_pops,
        candidate_metrics.tile_dispatches,
        candidate_metrics.scalar_lanes,
        candidate_metrics.vector_lanes,
        candidate_metrics.vector_blocks,
        baseline_metrics.remote_pushes,
        candidate_metrics.remote_pushes,
        baseline_metrics.fairness_samples,
        candidate_metrics.fairness_samples,
        baseline_metrics.fairness_p99_gap,
        candidate_metrics.fairness_p99_gap,
        candidate_metrics.forced_escapes,
        baseline_metrics.hot_allocations +
            candidate_metrics.hot_allocations,
        options->order == PAIR_ORDER_ABBA ?
            "ABBA" :
            "BAAB",
        affinity,
        compiler,
        host);
    result = 0;

out:
    srem_model_batch_destroy(candidate);
    srem_model_batch_destroy(baseline);
    return result;
}

int main(int argc, char **argv) {
    bench_options_t options;

    if (parse_options(argc, argv, &options) != 0) {
        return fail_message("invalid command line");
    }
    return run_benchmark(&options);
}
