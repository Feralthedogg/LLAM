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

#define LCCF_REPR_SCHEMA_VERSION 1U
#define LCCF_REPR_CONTRAST_COUNT 3U

typedef enum lccf_repr_route {
    LCCF_REPR_ROUTE_QUEUE = 0,
    LCCF_REPR_ROUTE_FUSED,
    LCCF_REPR_ROUTE_MIXED,
    LCCF_REPR_ROUTE_COUNT
} lccf_repr_route_t;

typedef enum option_id {
    OPTION_WORKLOAD = 0,
    OPTION_ROUTE,
    OPTION_PROCESS_ID,
    OPTION_INSTANCES,
    OPTION_FRAME_BYTES,
    OPTION_SITES,
    OPTION_CHAIN,
    OPTION_BLOCKS,
    OPTION_MINIMUM_WINDOW_NS,
    OPTION_WARMUP_ROUNDS,
    OPTION_SEED,
    OPTION_COUNT
} option_id_t;

typedef struct bench_options {
    lccf_model_workload_t workload;
    lccf_repr_route_t route;
    uint64_t process_id;
    size_t instances;
    size_t frame_bytes;
    unsigned sites;
    unsigned chain;
    unsigned blocks;
    uint64_t minimum_window_ns;
    uint64_t warmup_rounds;
    uint64_t seed;
} bench_options_t;

typedef struct window_sample {
    lccf_model_metrics_t metrics;
    uint64_t wall_ns;
    uint64_t cpu_ns;
    uint64_t p50_ns;
    uint64_t p99_ns;
    uint64_t checksum;
    bool references_balanced;
} window_sample_t;

typedef struct contrast_runner {
    const char *name;
    const char *left_representation;
    const char *right_representation;
    lccf_model_mode_t left_mode;
    lccf_model_mode_t right_mode;
    lccf_model_batch_t *left_batch;
    lccf_model_batch_t *right_batch;
    uint64_t *latencies;
    uint64_t rounds;
} contrast_runner_t;

static int fail_message(const char *message) {
    fprintf(stderr, "[bench_lccf_repr] %s\n", message);
    return 2;
}

static const char *route_name(lccf_repr_route_t route) {
    switch (route) {
    case LCCF_REPR_ROUTE_QUEUE:
        return "queue";
    case LCCF_REPR_ROUTE_FUSED:
        return "fused";
    case LCCF_REPR_ROUTE_MIXED:
        return "mixed";
    case LCCF_REPR_ROUTE_COUNT:
    default:
        return NULL;
    }
}

static int parse_route(const char *text, lccf_repr_route_t *out) {
    lccf_repr_route_t route;

    if (text == NULL || out == NULL) {
        return EINVAL;
    }
    for (route = LCCF_REPR_ROUTE_QUEUE;
         route < LCCF_REPR_ROUTE_COUNT;
         route = (lccf_repr_route_t)(route + 1)) {
        if (strcmp(text, route_name(route)) == 0) {
            *out = route;
            return 0;
        }
    }
    return EINVAL;
}

static lccf_model_mode_t mode_for(
    lccf_repr_route_t route,
    const char *representation) {
    if (representation == NULL) {
        return LCCF_MODEL_MODE_COUNT;
    }
    if (strcmp(representation, "A") == 0) {
        switch (route) {
        case LCCF_REPR_ROUTE_QUEUE:
            return LCCF_MODEL_RECOMPUTE_QUEUE;
        case LCCF_REPR_ROUTE_FUSED:
            return LCCF_MODEL_RECOMPUTE_FUSED;
        case LCCF_REPR_ROUTE_MIXED:
            return LCCF_MODEL_MIXED_RECOMPUTE;
        case LCCF_REPR_ROUTE_COUNT:
        default:
            return LCCF_MODEL_MODE_COUNT;
        }
    }
    if (strcmp(representation, "B") == 0) {
        switch (route) {
        case LCCF_REPR_ROUTE_QUEUE:
            return LCCF_MODEL_SHARED_EVENT_QUEUE;
        case LCCF_REPR_ROUTE_FUSED:
            return LCCF_MODEL_SHARED_EVENT_FUSED;
        case LCCF_REPR_ROUTE_MIXED:
            return LCCF_MODEL_MIXED_SHARED_EVENT;
        case LCCF_REPR_ROUTE_COUNT:
        default:
            return LCCF_MODEL_MODE_COUNT;
        }
    }
    if (strcmp(representation, "C") == 0) {
        switch (route) {
        case LCCF_REPR_ROUTE_QUEUE:
            return LCCF_MODEL_SHARED_FACT_QUEUE;
        case LCCF_REPR_ROUTE_FUSED:
            return LCCF_MODEL_SHARED_FACT_FUSED;
        case LCCF_REPR_ROUTE_MIXED:
            return LCCF_MODEL_MIXED_SHARED_FACT;
        case LCCF_REPR_ROUTE_COUNT:
        default:
            return LCCF_MODEL_MODE_COUNT;
        }
    }
    return LCCF_MODEL_MODE_COUNT;
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
        "--route",
        "--process-id",
        "--instances",
        "--frame-bytes",
        "--sites",
        "--chain",
        "--blocks",
        "--minimum-window-ns",
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
    case OPTION_PROCESS_ID:
        options->process_id = value;
        return 0;
    case OPTION_INSTANCES:
        if (value == 0U || value > SIZE_MAX || value > UINT32_MAX ||
            value == SIZE_MAX) {
            return EINVAL;
        }
        options->instances = (size_t)value;
        return 0;
    case OPTION_FRAME_BYTES:
        if (value != 64U && value != 256U) {
            return EINVAL;
        }
        options->frame_bytes = (size_t)value;
        return 0;
    case OPTION_SITES:
        if (value != LCCF_MODEL_MAX_SITES) {
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
    case OPTION_BLOCKS:
        if (value == 0U || value > UINT32_MAX) {
            return EINVAL;
        }
        options->blocks = (unsigned)value;
        return 0;
    case OPTION_MINIMUM_WINDOW_NS:
        if (value == 0U) {
            return EINVAL;
        }
        options->minimum_window_ns = value;
        return 0;
    case OPTION_WARMUP_ROUNDS:
        options->warmup_rounds = value;
        return 0;
    case OPTION_SEED:
        options->seed = value;
        return 0;
    case OPTION_WORKLOAD:
    case OPTION_ROUTE:
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
    if (id == OPTION_ROUTE) {
        return parse_route(text, &options->route);
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
    if (options.instances > SIZE_MAX / options.chain ||
        options.instances * options.chain > UINT64_MAX) {
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

static int capture_window(lccf_model_batch_t *batch,
                          uint64_t rounds,
                          uint64_t *latencies,
                          window_sample_t *out) {
    uint64_t wall_start;
    uint64_t wall_end;
    uint64_t cpu_start;
    uint64_t cpu_end;
    uint64_t round;

    if (batch == NULL || rounds == 0U || rounds > SIZE_MAX ||
        latencies == NULL || out == NULL) {
        return EINVAL;
    }
    memset(out, 0, sizeof(*out));
    wall_start = lccf_platform_monotonic_ns();
    cpu_start = lccf_platform_process_cpu_ns();
    if (wall_start == 0U || cpu_start == 0U) {
        return EIO;
    }
    for (round = 0U; round < rounds; ++round) {
        const uint64_t start = lccf_platform_monotonic_ns();
        uint64_t end;
        int rc;

        if (start == 0U) {
            return EIO;
        }
        rc = lccf_model_run_round(batch, &out->metrics);
        if (rc != 0) {
            return rc;
        }
        end = lccf_platform_monotonic_ns();
        if (end < start) {
            return EIO;
        }
        latencies[round] = end == start ? UINT64_C(1) : end - start;
    }
    cpu_end = lccf_platform_process_cpu_ns();
    wall_end = lccf_platform_monotonic_ns();
    if (cpu_end <= cpu_start || wall_end <= wall_start) {
        return EIO;
    }
    out->wall_ns = wall_end - wall_start;
    out->cpu_ns = cpu_end - cpu_start;
    qsort(latencies, (size_t)rounds, sizeof(*latencies), compare_u64);
    out->p50_ns = latencies[percentile_index((size_t)rounds, 50U)];
    out->p99_ns = latencies[percentile_index((size_t)rounds, 99U)];
    out->checksum = lccf_model_checksum(batch);
    out->references_balanced = lccf_model_fact_references_balanced(batch);
    return out->checksum == 0U ? EPROTO : 0;
}

static uint64_t expected_sidecar_bytes(const char *representation) {
    if (strcmp(representation, "B") == 0) {
        return UINT64_C(48);
    }
    if (strcmp(representation, "C") == 0) {
        return UINT64_C(64);
    }
    return UINT64_C(0);
}

static int validate_window(const bench_options_t *options,
                           const char *representation,
                           uint64_t rounds,
                           const window_sample_t *sample,
                           bool require_minimum) {
    const lccf_model_metrics_t *metrics;
    uint64_t completions;
    uint64_t callbacks;
    uint64_t attempts;
    uint64_t stale;
    uint64_t materializations;
    uint64_t normalizations;
    uint64_t lookups;
    uint64_t fairness_samples;

    if (options == NULL || representation == NULL || sample == NULL ||
        rounds == 0U || rounds > UINT64_MAX / options->instances) {
        return EINVAL;
    }
    completions = rounds * (uint64_t)options->instances;
    if (completions > UINT64_MAX / options->chain) {
        return EOVERFLOW;
    }
    callbacks = completions * (uint64_t)options->chain;
    fairness_samples =
        options->workload == LCCF_MODEL_COMPLETION_MIXED_FAIRNESS ?
            callbacks / UINT64_C(32) :
            UINT64_C(0);
    attempts = completions *
        (options->workload == LCCF_MODEL_COMPLETION_TIMER_CANCEL
             ? UINT64_C(3)
             : UINT64_C(1));
    stale = attempts - completions;
    metrics = &sample->metrics;
    materializations = callbacks;
    if (options->route == LCCF_REPR_ROUTE_QUEUE) {
        materializations += completions;
    } else if (options->route == LCCF_REPR_ROUTE_MIXED) {
        materializations += metrics->forced_escapes;
    }
    if (strcmp(representation, "A") == 0) {
        normalizations = materializations;
        lookups = materializations;
    } else if (strcmp(representation, "B") == 0) {
        normalizations = completions;
        lookups = materializations;
    } else if (strcmp(representation, "C") == 0) {
        normalizations = completions;
        lookups = completions +
                  metrics->fact_changed_site_materializations;
    } else {
        return EINVAL;
    }
    if ((require_minimum &&
         sample->wall_ns < options->minimum_window_ns) ||
        sample->wall_ns == 0U || sample->cpu_ns == 0U ||
        sample->p50_ns == 0U || sample->p99_ns == 0U ||
        sample->p50_ns > sample->p99_ns ||
        sample->p99_ns > sample->wall_ns ||
        !sample->references_balanced ||
        metrics->completions != completions ||
        metrics->claims != completions ||
        metrics->resume_calls != callbacks ||
        metrics->facts_attempted != attempts ||
        metrics->facts_built != completions ||
        metrics->stale_tickets != stale ||
        metrics->fact_stale_losers != stale ||
        metrics->fairness_samples != fairness_samples ||
        (fairness_samples == 0U && metrics->fairness_p99_ns != 0U) ||
        (fairness_samples != 0U && metrics->fairness_p99_ns == 0U) ||
        metrics->facts_build_failed != 0U ||
        metrics->fact_module_pins != completions ||
        metrics->fact_payload_pins != completions ||
        metrics->fact_normalizations != normalizations ||
        metrics->fact_site_lookups != lookups ||
        metrics->fact_changed_site_materializations >
            callbacks - completions ||
        metrics->fact_guard_rechecks != materializations ||
        metrics->fact_queue_forwards != 0U ||
        metrics->fact_generation_mismatches != 0U ||
        metrics->fact_reuse_delays != 0U ||
        metrics->hot_allocations != 0U ||
        metrics->fact_hot_bytes != UINT64_C(64) ||
        metrics->fact_sidecar_bytes !=
            expected_sidecar_bytes(representation) ||
        metrics->queue_pushes != metrics->queue_pops ||
        metrics->fact_overflow_pushes !=
            metrics->fact_overflow_pops ||
        metrics->direct_calls + metrics->queue_pops != callbacks) {
        return EPROTO;
    }
    if (options->route == LCCF_REPR_ROUTE_QUEUE &&
        (metrics->direct_calls != 0U ||
         metrics->forced_escapes != 0U)) {
        return EPROTO;
    }
    if (options->route == LCCF_REPR_ROUTE_FUSED &&
        (metrics->direct_calls != callbacks ||
         metrics->queue_pops != 0U ||
         metrics->forced_escapes != 0U)) {
        return EPROTO;
    }
    if (options->route == LCCF_REPR_ROUTE_MIXED &&
        (metrics->forced_escapes == 0U ||
         metrics->queue_pops !=
             metrics->forced_escapes * options->chain)) {
        return EPROTO;
    }
    return 0;
}

static bool common_metrics_equal(const lccf_model_metrics_t *left,
                                 const lccf_model_metrics_t *right) {
    return left->completions == right->completions &&
           left->claims == right->claims &&
           left->stale_tickets == right->stale_tickets &&
           left->queue_pushes == right->queue_pushes &&
           left->queue_pops == right->queue_pops &&
           left->resume_calls == right->resume_calls &&
           left->direct_calls == right->direct_calls &&
           left->forced_escapes == right->forced_escapes &&
           left->remote_pushes == right->remote_pushes &&
           left->fairness_samples == right->fairness_samples &&
           left->hot_allocations == right->hot_allocations &&
           left->facts_attempted == right->facts_attempted &&
           left->facts_built == right->facts_built &&
           left->facts_build_failed == right->facts_build_failed &&
           left->fact_changed_site_materializations ==
               right->fact_changed_site_materializations &&
           left->fact_module_pins == right->fact_module_pins &&
           left->fact_payload_pins == right->fact_payload_pins &&
           left->fact_stale_losers == right->fact_stale_losers &&
           left->fact_guard_rechecks == right->fact_guard_rechecks &&
           left->fact_queue_forwards == right->fact_queue_forwards &&
           left->fact_generation_mismatches ==
               right->fact_generation_mismatches &&
           left->fact_reuse_delays == right->fact_reuse_delays &&
           left->fact_hot_bytes == right->fact_hot_bytes &&
           left->fact_overflow_pushes == right->fact_overflow_pushes &&
           left->fact_overflow_pops == right->fact_overflow_pops;
}

static int validate_pair(const bench_options_t *options,
                         const contrast_runner_t *contrast,
                         const window_sample_t *left,
                         const window_sample_t *right,
                         bool require_minimum) {
    int rc;

    rc = validate_window(options, contrast->left_representation,
                         contrast->rounds, left, require_minimum);
    if (rc == 0) {
        rc = validate_window(options, contrast->right_representation,
                             contrast->rounds, right, require_minimum);
    }
    if (rc != 0 || left->checksum != right->checksum ||
        !common_metrics_equal(&left->metrics, &right->metrics) ||
        !lccf_model_batch_equal(contrast->left_batch,
                                contrast->right_batch)) {
        return rc != 0 ? rc : EPROTO;
    }
    return 0;
}

static int create_contrast(const bench_options_t *options,
                           contrast_runner_t *contrast) {
    lccf_model_config_t config;
    int rc;

    memset(&config, 0, sizeof(config));
    config.workload = options->workload;
    config.instance_count = options->instances;
    config.frame_bytes = options->frame_bytes;
    config.cell_bytes = 64U;
    config.site_count = options->sites;
    config.direct_budget = 8U;
    config.chain_length = options->chain;
    config.seed = options->seed;
    config.mode = contrast->left_mode;
    rc = lccf_model_batch_create(&config, &contrast->left_batch);
    if (rc != 0) {
        return rc;
    }
    config.mode = contrast->right_mode;
    rc = lccf_model_batch_create(&config, &contrast->right_batch);
    return rc;
}

static int warmup_contrast(const bench_options_t *options,
                           contrast_runner_t *contrast) {
    lccf_model_metrics_t left_metrics = {0};
    lccf_model_metrics_t right_metrics = {0};
    int rc = 0;

    if (options->warmup_rounds != 0U) {
        rc = run_rounds(contrast->left_batch,
                        options->warmup_rounds, &left_metrics);
        if (rc == 0) {
            rc = run_rounds(contrast->right_batch,
                            options->warmup_rounds, &right_metrics);
        }
        if (rc == 0 &&
            (!common_metrics_equal(&left_metrics, &right_metrics) ||
             !lccf_model_batch_equal(contrast->left_batch,
                                     contrast->right_batch))) {
            rc = EPROTO;
        }
    }
    if (rc == 0 &&
        (lccf_model_batch_reset(contrast->left_batch) != 0 ||
         lccf_model_batch_reset(contrast->right_batch) != 0)) {
        rc = EPROTO;
    }
    return rc;
}

static int resize_latencies(contrast_runner_t *contrast,
                            uint64_t rounds) {
    uint64_t *resized;

    if (rounds == 0U || rounds > SIZE_MAX ||
        rounds > SIZE_MAX / sizeof(*contrast->latencies)) {
        return EOVERFLOW;
    }
    resized = realloc(
        contrast->latencies,
        (size_t)rounds * sizeof(*contrast->latencies));
    if (resized == NULL) {
        return ENOMEM;
    }
    contrast->latencies = resized;
    contrast->rounds = rounds;
    return 0;
}

static int calibrate_contrast(const bench_options_t *options,
                              contrast_runner_t *contrast) {
    uint64_t rounds = UINT64_C(1);

    for (;;) {
        window_sample_t left;
        window_sample_t right;
        int rc = resize_latencies(contrast, rounds);

        if (rc == 0) {
            rc = capture_window(contrast->left_batch, rounds,
                                contrast->latencies, &left);
        }
        if (rc == 0) {
            rc = capture_window(contrast->right_batch, rounds,
                                contrast->latencies, &right);
        }
        if (rc == 0) {
            rc = validate_pair(options, contrast, &left, &right, false);
        }
        if (rc != 0) {
            return rc;
        }
        if (lccf_model_batch_reset(contrast->left_batch) != 0 ||
            lccf_model_batch_reset(contrast->right_batch) != 0) {
            return EPROTO;
        }
        if (left.wall_ns >= options->minimum_window_ns &&
            right.wall_ns >= options->minimum_window_ns) {
            return 0;
        }
        if (rounds > UINT64_MAX / UINT64_C(2)) {
            return EOVERFLOW;
        }
        rounds *= UINT64_C(2);
    }
}

static uint64_t splitmix64_next(uint64_t *state) {
    uint64_t value;

    *state += UINT64_C(0x9E3779B97F4A7C15);
    value = *state;
    value = (value ^ (value >> 30U)) *
            UINT64_C(0xBF58476D1CE4E5B9);
    value = (value ^ (value >> 27U)) *
            UINT64_C(0x94D049BB133111EB);
    return value ^ (value >> 31U);
}

static bool choose_abba(uint64_t *state,
                        unsigned abba_count,
                        unsigned baab_count) {
    bool abba = (splitmix64_next(state) & UINT64_C(1)) == 0U;
    const unsigned next_abba = abba_count + (abba ? 1U : 0U);
    const unsigned next_baab = baab_count + (abba ? 0U : 1U);
    const unsigned difference = next_abba > next_baab
                                    ? next_abba - next_baab
                                    : next_baab - next_abba;

    if (difference > 1U) {
        abba = !abba;
    }
    return abba;
}

static bool queue_balanced(const window_sample_t *sample) {
    return sample->metrics.queue_pushes == sample->metrics.queue_pops;
}

static bool overflow_balanced(const window_sample_t *sample) {
    return sample->metrics.fact_overflow_pushes ==
           sample->metrics.fact_overflow_pops;
}

static bool module_balanced(const window_sample_t *sample) {
    return sample->references_balanced &&
           sample->metrics.fact_module_pins ==
               sample->metrics.completions;
}

static bool payload_balanced(const window_sample_t *sample) {
    return sample->references_balanced &&
           sample->metrics.fact_payload_pins ==
               sample->metrics.completions;
}

static int emit_side(const char *representation,
                     const window_sample_t *sample) {
    const lccf_model_metrics_t *metrics = &sample->metrics;
    const int written = printf(
        "{\"representation\":\"%s\"," 
        "\"wall_ns\":%" PRIu64 ","
        "\"cpu_ns\":%" PRIu64 ","
        "\"p50_ns\":%" PRIu64 ","
        "\"p99_ns\":%" PRIu64 ","
        "\"checksum\":\"%016" PRIx64 "\"," 
        "\"operations\":%" PRIu64 ","
        "\"callbacks\":%" PRIu64 ","
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
        "\"fact_changed_site_materializations\":%" PRIu64 ","
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
        "\"refs_balanced\":%s,"
        "\"queue_balanced\":%s,"
        "\"overflow_balanced\":%s,"
        "\"module_balanced\":%s,"
        "\"payload_balanced\":%s,"
        "\"backend_balanced\":%s,"
        "\"tickets_balanced\":%s}",
        representation,
        sample->wall_ns,
        sample->cpu_ns,
        sample->p50_ns,
        sample->p99_ns,
        sample->checksum,
        metrics->resume_calls,
        metrics->resume_calls,
        metrics->completions,
        metrics->claims,
        metrics->stale_tickets,
        metrics->queue_pushes,
        metrics->queue_pops,
        metrics->resume_calls,
        metrics->direct_calls,
        metrics->forced_escapes,
        metrics->hot_allocations,
        metrics->facts_attempted,
        metrics->facts_built,
        metrics->facts_build_failed,
        metrics->fact_normalizations,
        metrics->fact_site_lookups,
        metrics->fact_changed_site_materializations,
        metrics->fact_module_pins,
        metrics->fact_payload_pins,
        metrics->fact_stale_losers,
        metrics->fact_guard_rechecks,
        metrics->fact_queue_forwards,
        metrics->fact_generation_mismatches,
        metrics->fact_reuse_delays,
        metrics->fact_hot_bytes,
        metrics->fact_sidecar_bytes,
        metrics->fact_overflow_pushes,
        metrics->fact_overflow_pops,
        sample->references_balanced ? "true" : "false",
        queue_balanced(sample) ? "true" : "false",
        overflow_balanced(sample) ? "true" : "false",
        module_balanced(sample) ? "true" : "false",
        payload_balanced(sample) ? "true" : "false",
        sample->references_balanced ? "true" : "false",
        sample->references_balanced ? "true" : "false");

    return written < 0 ? EIO : 0;
}

static int emit_pair(const bench_options_t *options,
                     const contrast_runner_t *contrast,
                     unsigned block,
                     unsigned pair,
                     const char *order,
                     const window_sample_t *left,
                     const window_sample_t *right) {
    int written;

    written = printf(
        "{\"schema_version\":%u,"
        "\"process_id\":%" PRIu64 ","
        "\"cell\":{\"workload\":\"%s\"," 
        "\"route\":\"%s\","
        "\"frame_bytes\":%zu,"
        "\"instances\":%zu,"
        "\"sites\":%u,"
        "\"chain\":%u},"
        "\"contrast\":\"%s\","
        "\"block\":%u,"
        "\"pair\":%u,"
        "\"order\":\"%s\","
        "\"minimum_window_ns\":%" PRIu64 ","
        "\"rounds\":%" PRIu64 ","
        "\"seed\":%" PRIu64 ","
        "\"left\":",
        LCCF_REPR_SCHEMA_VERSION,
        options->process_id,
        lccf_model_workload_name(options->workload),
        route_name(options->route),
        options->frame_bytes,
        options->instances,
        options->sites,
        options->chain,
        contrast->name,
        block,
        pair,
        order,
        options->minimum_window_ns,
        contrast->rounds,
        options->seed);
    if (written < 0 ||
        emit_side(contrast->left_representation, left) != 0 ||
        printf(",\"right\":") < 0 ||
        emit_side(contrast->right_representation, right) != 0 ||
        printf("}\n") < 0) {
        return EIO;
    }
    return 0;
}

static int capture_pair(const bench_options_t *options,
                        contrast_runner_t *contrast,
                        bool left_first,
                        unsigned block,
                        unsigned pair,
                        const char *order) {
    window_sample_t left;
    window_sample_t right;
    int rc;

    if (left_first) {
        rc = capture_window(contrast->left_batch, contrast->rounds,
                            contrast->latencies, &left);
        if (rc == 0) {
            rc = capture_window(contrast->right_batch, contrast->rounds,
                                contrast->latencies, &right);
        }
    } else {
        rc = capture_window(contrast->right_batch, contrast->rounds,
                            contrast->latencies, &right);
        if (rc == 0) {
            rc = capture_window(contrast->left_batch, contrast->rounds,
                                contrast->latencies, &left);
        }
    }
    if (rc == 0) {
        rc = validate_pair(options, contrast, &left, &right, true);
    }
    if (rc == 0) {
        rc = emit_pair(options, contrast, block, pair, order,
                       &left, &right);
    }
    return rc;
}

static int run_contrast(const bench_options_t *options,
                        contrast_runner_t *contrast,
                        unsigned contrast_index) {
    uint64_t order_state =
        options->seed ^
        ((uint64_t)(contrast_index + 1U) *
         UINT64_C(0xD1B54A32D192ED03));
    unsigned abba_count = 0U;
    unsigned baab_count = 0U;
    unsigned block;

    for (block = 0U; block < options->blocks; ++block) {
        const bool abba = choose_abba(
            &order_state, abba_count, baab_count);
        const char *order = abba ? "ABBA" : "BAAB";
        int rc;

        if (abba) {
            abba_count += 1U;
            rc = capture_pair(options, contrast, true,
                              block, 0U, order);
            if (rc == 0) {
                rc = capture_pair(options, contrast, false,
                                  block, 1U, order);
            }
        } else {
            baab_count += 1U;
            rc = capture_pair(options, contrast, false,
                              block, 0U, order);
            if (rc == 0) {
                rc = capture_pair(options, contrast, true,
                                  block, 1U, order);
            }
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

static void destroy_contrast(contrast_runner_t *contrast) {
    if (contrast == NULL) {
        return;
    }
    free(contrast->latencies);
    lccf_model_batch_destroy(contrast->right_batch);
    lccf_model_batch_destroy(contrast->left_batch);
    contrast->latencies = NULL;
    contrast->right_batch = NULL;
    contrast->left_batch = NULL;
}

static int run_benchmark(const bench_options_t *options) {
    contrast_runner_t contrasts[LCCF_REPR_CONTRAST_COUNT] = {
        {
            "A/B", "A", "B",
            LCCF_MODEL_MODE_COUNT, LCCF_MODEL_MODE_COUNT,
            NULL, NULL, NULL, 0U,
        },
        {
            "A/C", "A", "C",
            LCCF_MODEL_MODE_COUNT, LCCF_MODEL_MODE_COUNT,
            NULL, NULL, NULL, 0U,
        },
        {
            "B/C", "B", "C",
            LCCF_MODEL_MODE_COUNT, LCCF_MODEL_MODE_COUNT,
            NULL, NULL, NULL, 0U,
        },
    };
    unsigned index;
    int rc = 0;

    for (index = 0U; index < LCCF_REPR_CONTRAST_COUNT; ++index) {
        contrasts[index].left_mode = mode_for(
            options->route, contrasts[index].left_representation);
        contrasts[index].right_mode = mode_for(
            options->route, contrasts[index].right_representation);
        if (contrasts[index].left_mode == LCCF_MODEL_MODE_COUNT ||
            contrasts[index].right_mode == LCCF_MODEL_MODE_COUNT) {
            rc = EINVAL;
            break;
        }
        rc = create_contrast(options, &contrasts[index]);
        if (rc == 0) {
            rc = warmup_contrast(options, &contrasts[index]);
        }
        if (rc == 0) {
            rc = calibrate_contrast(options, &contrasts[index]);
        }
        if (rc != 0) {
            break;
        }
    }
    if (rc == 0) {
        for (index = 0U; index < LCCF_REPR_CONTRAST_COUNT; ++index) {
            rc = run_contrast(options, &contrasts[index], index);
            if (rc != 0) {
                break;
            }
        }
    }
    for (index = 0U; index < LCCF_REPR_CONTRAST_COUNT; ++index) {
        destroy_contrast(&contrasts[index]);
    }
    return rc;
}

int main(int argc, char **argv) {
    bench_options_t options;
    int rc;

    rc = parse_options(argc, argv, &options);
    if (rc != 0) {
        return fail_message("invalid command line");
    }
    rc = run_benchmark(&options);
    if (rc != 0) {
        return fail_message("calibration or paired execution failed");
    }
    return 0;
}
